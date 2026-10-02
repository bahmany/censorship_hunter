#include "core/health_score.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace hunter {
namespace {
const char* kOutcomes[] = {"Pass", "RemoteFailure", "Partial", "LocalNetworkDown", "Indeterminate",
                           "EngineError", "BindConflict", "Unsupported", "InvalidConfig", "Cancelled"};
const char* kStates[] = {"Unknown", "Testing", "Healthy", "Degraded", "Unstable", "Dead"};
const char* kStab[] = {"Unrated", "Stable", "Unstable", "Dead"};
bool finiteNonNeg(double v) { return std::isfinite(v) && v >= 0.0; }
}  // namespace

const char* outcomeName(ProbeOutcome o) { return kOutcomes[static_cast<int>(o)]; }
bool parseOutcome(const std::string& s, ProbeOutcome* o) {
    for (int i = 0; i < 10; i++) if (s == kOutcomes[i]) { *o = static_cast<ProbeOutcome>(i); return true; }
    return false;
}
const char* healthStateName(HealthState s) { return kStates[static_cast<int>(s)]; }
bool parseHealthState(const std::string& s, HealthState* o) {
    for (int i = 0; i < 6; i++) if (s == kStates[i]) { *o = static_cast<HealthState>(i); return true; }
    return false;
}
const char* stabilityName(Stability s) { return kStab[static_cast<int>(s)]; }
bool parseStability(const std::string& s, Stability* o) {
    for (int i = 0; i < 4; i++) if (s == kStab[i]) { *o = static_cast<Stability>(i); return true; }
    return false;
}

ClockFn systemClock() {
    return []() {
        using namespace std::chrono;
        return duration<double>(system_clock::now().time_since_epoch()).count();
    };
}

bool isEligibleResult(const ProbeResult& r) {
    switch (r.outcome) {
        case ProbeOutcome::Pass: return true;
        case ProbeOutcome::RemoteFailure:
        case ProbeOutcome::Partial: return r.attributable;
        default: return false;
    }
}

void markTesting(HealthEvidence& ev) {
    if (ev.state == HealthState::Unknown) ev.state = HealthState::Testing;
}

static void pushSample(HealthEvidence& ev, const ProbeSample& s, const HealthThresholds& th) {
    ev.ring.push_back(s);
    const size_t cap = static_cast<size_t>(std::max(1, th.ring_size));
    while (ev.ring.size() > cap) ev.ring.pop_front();
}

ApplyEffect applyProbe(HealthEvidence& ev, const ProbeResult& r, const HealthThresholds& th) {
    if (!finiteNonNeg(r.finished_at)) return ApplyEffect::Rejected;
    if (r.outcome == ProbeOutcome::Pass && !finiteNonNeg(r.latency_ms)) return ApplyEffect::Rejected;
    if (r.generation != 0 && r.generation < ev.generation) return ApplyEffect::Stale;
    if (!r.run_id.empty()) {
        for (const auto& s : ev.ring) if (s.id == r.run_id) return ApplyEffect::Duplicate;
    }
    if (ev.last_attempt_at > 0.0 && r.finished_at < ev.last_attempt_at) return ApplyEffect::Stale;

    if (r.generation > ev.generation) ev.generation = r.generation;
    ev.last_attempt_at = r.finished_at;
    ev.last_outcome = outcomeName(r.outcome);
    if (r.bulk_passed && r.outcome == ProbeOutcome::Pass) ev.last_bulk_success = r.finished_at;

    if (!isEligibleResult(r)) {
        // Partial that is not attributable still means "not a full pass": Degraded, and the
        // success run is broken. It never touches counters, ring or the failure streak.
        if (r.outcome == ProbeOutcome::Partial) {
            if (ev.state == HealthState::Healthy || ev.state == HealthState::Unknown ||
                ev.state == HealthState::Testing)
                ev.state = HealthState::Degraded;
            ev.success_streak = 0;
            ev.streak_started_at = 0.0;
        }
        return ApplyEffect::Excluded;
    }

    ev.eligible_count = ev.eligible_count == UINT32_MAX ? UINT32_MAX : ev.eligible_count + 1;
    ProbeSample s;
    s.id = r.run_id;
    s.t = r.finished_at;
    if (r.outcome == ProbeOutcome::Pass) {
        s.success = true;
        s.latency_ms = r.latency_ms;
        pushSample(ev, s, th);
        // Gap rule: an over-long gap between full successes restarts the run.
        // (Recovery from a non-Healthy state ignores the gap rule; see below.)
        if (ev.state == HealthState::Healthy && ev.success_streak > 0 && ev.last_full_success > 0.0 &&
            r.finished_at - ev.last_full_success > th.stable_max_gap_s) {
            ev.success_streak = 0;
        }
        if (ev.success_streak == 0) ev.streak_started_at = r.finished_at;
        ev.success_streak++;
        ev.last_full_success = r.finished_at;
        ev.failure_streak = 0;
        ev.failure_streak_started_at = 0.0;
        switch (ev.state) {
            case HealthState::Unknown:
            case HealthState::Testing:
            case HealthState::Healthy:
                ev.state = HealthState::Healthy;
                break;
            default:  // Degraded / Unstable / Dead: two full passes >= recovery gap apart
                if (ev.success_streak >= 2 &&
                    r.finished_at - ev.streak_started_at >= th.recovery_min_gap_s) {
                    ev.state = HealthState::Healthy;
                    // Stability evidence restarts from the recovery point.
                    ev.success_streak = 1;
                    ev.streak_started_at = r.finished_at;
                } else
                    ev.state = HealthState::Degraded;
                break;
        }
    } else {
        s.success = false;
        s.latency_ms = -1.0;
        pushSample(ev, s, th);
        ev.success_streak = 0;
        ev.streak_started_at = 0.0;
        if (ev.failure_streak == 0) ev.failure_streak_started_at = r.finished_at;
        ev.failure_streak++;
        const double span = r.finished_at - ev.failure_streak_started_at;
        if (ev.failure_streak >= static_cast<uint32_t>(th.dead_failures) && span >= th.dead_min_span_s)
            ev.state = HealthState::Dead;
        else if (ev.state == HealthState::Dead)
            ev.state = HealthState::Dead;  // only a recovery leaves Dead
        else if (ev.failure_streak >= static_cast<uint32_t>(th.unstable_failures))
            ev.state = HealthState::Unstable;
        else
            ev.state = HealthState::Degraded;
    }
    return ApplyEffect::Applied;
}

void forceDead(HealthEvidence& ev, double now, const HealthThresholds& th) {
    ev.state = HealthState::Dead;
    ev.success_streak = 0;
    ev.streak_started_at = 0.0;
    if (ev.failure_streak < static_cast<uint32_t>(th.dead_failures))
        ev.failure_streak = static_cast<uint32_t>(th.dead_failures);
    if (ev.failure_streak_started_at <= 0.0 || now - ev.failure_streak_started_at < th.dead_min_span_s)
        ev.failure_streak_started_at = now - th.dead_min_span_s;
    if (ev.failure_streak_started_at < 0.0) ev.failure_streak_started_at = 0.0;
    if (ev.eligible_count < ev.failure_streak) ev.eligible_count = ev.failure_streak;
}

bool evidenceConsistent(const HealthEvidence& ev, const HealthThresholds& th, std::string* why) {
    auto bad = [&](const char* m) { if (why) *why = m; return false; };
    if (ev.ring.size() > static_cast<size_t>(std::max(1, th.ring_size))) return bad("ring too large");
    if (ev.eligible_count < ev.ring.size()) return bad("eligible_count < ring size");
    if (ev.success_streak > ev.eligible_count || ev.failure_streak > ev.eligible_count)
        return bad("streak exceeds eligible_count");
    if (ev.success_streak > 0 && ev.failure_streak > 0) return bad("both streaks active");
    if ((ev.success_streak > 0) != (ev.streak_started_at > 0.0)) return bad("success streak start mismatch");
    if ((ev.failure_streak > 0) != (ev.failure_streak_started_at > 0.0)) return bad("failure streak start mismatch");
    if (ev.success_streak > 0 && ev.last_full_success < ev.streak_started_at) return bad("last success before streak start");
    // Tail of ring must agree with streaks.
    size_t n = ev.ring.size();
    for (size_t i = 0; i < std::min<size_t>(n, ev.success_streak); i++)
        if (!ev.ring[n - 1 - i].success) return bad("ring contradicts success streak");
    for (size_t i = 0; i < std::min<size_t>(n, ev.failure_streak); i++)
        if (ev.ring[n - 1 - i].success) return bad("ring contradicts failure streak");
    double prev = -1.0;
    for (const auto& s : ev.ring) {
        if (!finiteNonNeg(s.t) || s.t < prev) return bad("ring times invalid");
        if (s.success && !finiteNonNeg(s.latency_ms)) return bad("ring latency invalid");
        prev = s.t;
    }
    bool any_success = false;
    for (const auto& s : ev.ring) any_success |= s.success;
    if (any_success && ev.last_full_success <= 0.0) return bad("ring success without last_full_success");
    switch (ev.state) {
        case HealthState::Unknown:
        case HealthState::Testing:
            if (ev.eligible_count != 0) return bad("unknown state with evidence");
            break;
        case HealthState::Healthy:
            if (ev.failure_streak != 0 || ev.last_full_success <= 0.0) return bad("healthy inconsistent");
            break;
        case HealthState::Dead:
            if (ev.failure_streak < static_cast<uint32_t>(th.dead_failures)) return bad("dead without failures");
            break;
        case HealthState::Unstable:
            if (ev.failure_streak < static_cast<uint32_t>(th.unstable_failures)) return bad("unstable without failures");
            break;
        case HealthState::Degraded: break;
    }
    return true;
}

HealthEvaluation evaluateHealth(const HealthEvidence& ev, double now, const HealthThresholds& th) {
    HealthEvaluation e;
    // S: normalized finite-window EWMA, newest sample has index 0.
    double num = 0.0, den = 0.0, w = th.ewma_alpha;
    std::vector<double> lat;
    for (auto it = ev.ring.rbegin(); it != ev.ring.rend(); ++it) {
        num += w * (it->success ? 1.0 : 0.0);
        den += w;
        w *= (1.0 - th.ewma_alpha);
        if (it->success && finiteNonNeg(it->latency_ms)) lat.push_back(it->latency_ms);
    }
    e.ewma = den > 0.0 ? num / den : 0.0;
    if (!lat.empty()) {
        std::sort(lat.begin(), lat.end());
        size_t m = lat.size();
        size_t rank = (9 * m + 9) / 10;  // ceil(0.9 m) in integer arithmetic
        e.p90_ms = lat[rank - 1];
        e.has_p90 = true;
        e.latency_factor = 1.0 / (1.0 + e.p90_ms / th.latency_scale_ms);
    }
    if (ev.last_full_success > 0.0)
        e.freshness = std::exp2(-std::max(0.0, now - ev.last_full_success) / th.freshness_half_life_s);
    e.protocol_factor = (ev.unsupported || ev.telegram_only) ? 0.0
                        : (ev.insecure_tls ? th.protocol_factor_insecure : th.protocol_factor_secure);
    double sc = 100.0 * e.ewma * e.latency_factor * e.freshness * e.protocol_factor;
    if (!std::isfinite(sc)) sc = 0.0;
    e.score = std::min(100.0, std::max(0.0, sc));

    const bool fresh = ev.last_full_success > 0.0 &&
                       (now - ev.last_full_success) <= th.stable_max_success_age_s;
    e.switch_eligible = ev.state == HealthState::Healthy && fresh;
    e.stable = ev.state == HealthState::Healthy &&
               ev.success_streak >= static_cast<uint32_t>(th.stable_min_successes) &&
               ev.streak_started_at > 0.0 &&
               (ev.last_full_success - ev.streak_started_at) >= th.stable_min_span_s &&
               e.ewma >= th.stable_min_ewma && e.has_p90 && e.p90_ms <= th.stable_max_p90_ms &&
               fresh && ev.last_bulk_success > 0.0 &&
               (now - ev.last_bulk_success) <= th.stable_max_bulk_age_s &&
               !ev.telegram_only && !ev.unsupported;
    if (ev.eligible_count == 0) e.stability = Stability::Unrated;
    else if (ev.state == HealthState::Dead) e.stability = Stability::Dead;
    else if (e.stable) e.stability = Stability::Stable;
    else e.stability = Stability::Unstable;

    switch (ev.state) {
        case HealthState::Healthy: e.tier = e.stable ? 0 : 1; break;
        case HealthState::Degraded: e.tier = 2; break;
        case HealthState::Unknown:
        case HealthState::Testing: e.tier = 3; break;
        case HealthState::Unstable: e.tier = 4; break;
        case HealthState::Dead: e.tier = 5; break;
    }
    return e;
}

bool rankedBefore(const RankKey& a, const RankKey& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.score != b.score) return a.score > b.score;
    if (a.last_full_success != b.last_full_success) return a.last_full_success > b.last_full_success;
    return a.key < b.key;
}

}  // namespace hunter
