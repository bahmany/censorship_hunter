#include "network/connectivity_baseline.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace hunter {
namespace network {

const char* baselineStateName(BaselineState s) {
    switch (s) {
        case BaselineState::Online: return "online";
        case BaselineState::Offline: return "offline";
        case BaselineState::Indeterminate: return "indeterminate";
    }
    return "indeterminate";
}

// ── ControlTracker ──────────────────────────────────────────────────────
void ControlTracker::record(const std::string& key, uint32_t mask, double now, uint64_t generation) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& e = last_[key];
    e.mask = mask;
    e.at = now;
    e.gen = generation;
    if (last_.size() > 4096) {   // bounded
        for (auto it = last_.begin(); it != last_.end();) it = (now - it->second.at > 300.0) ? last_.erase(it) : std::next(it);
    }
}

bool ControlTracker::passed(const std::string& exclude_key, uint32_t bit, double t0, double t1, uint64_t generation) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [k, v] : last_) {
        if (k == exclude_key) continue;
        if ((v.mask & bit) == bit && v.gen == generation && v.at >= t0 && v.at <= t1) return true;
    }
    return false;
}

void ControlTracker::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    last_.clear();
}

// ── ConnectivityBaseline ────────────────────────────────────────────────
namespace {
bool defaultLinkUp() {
#ifdef __linux__
    std::ifstream f("/proc/net/route");
    if (!f) return true;   // unknown => assume up (conservative: Indeterminate, not Offline)
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string iface, dest, gw, flags;
        if (!(ss >> iface >> dest >> gw >> flags)) continue;
        if (dest == "00000000") {
            unsigned long fl = std::strtoul(flags.c_str(), nullptr, 16);
            if (fl & 0x1) return true;   // RTF_UP default route
        }
    }
    return false;   // route table readable but no default route
#else
    return true;
#endif
}
}  // namespace

ConnectivityBaseline::ConnectivityBaseline(std::shared_ptr<ProbeTransport> transport, ProbeConfig cfg,
                                           ClockFn clock, LinkFn link, SleepFn sleep)
    : transport_(transport ? std::move(transport) : makeCurlTransport()),
      cfg_(std::move(cfg)),
      clock_(clock ? std::move(clock) : systemClock()),
      link_(link ? std::move(link) : LinkFn(defaultLinkUp)),
      sleep_(sleep ? std::move(sleep)
                   : SleepFn([](double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); })) {}

std::shared_ptr<ConnectivityBaseline> ConnectivityBaseline::shared() {
    static std::shared_ptr<ConnectivityBaseline> inst = [] {
        auto b = std::make_shared<ConnectivityBaseline>();
        const char* env = std::getenv("HUNTER_DIRECT_BASELINE");
        if (env && std::string(env) == "0") b->setEnabled(false);
        return b;
    }();
    return inst;
}

void ConnectivityBaseline::setEnabled(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_ = on;
    have_ = false;
}
bool ConnectivityBaseline::enabled() const { std::lock_guard<std::mutex> lk(mu_); return enabled_; }
void ConnectivityBaseline::bumpGeneration() {
    std::lock_guard<std::mutex> lk(mu_);
    generation_++;
    have_ = false;
    controls_.clear();
}
uint64_t ConnectivityBaseline::generation() const { std::lock_guard<std::mutex> lk(mu_); return generation_; }

BaselineSnapshot ConnectivityBaseline::current(double max_age_s) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!enabled_) {
            BaselineSnapshot s;
            s.enabled = false; s.state = BaselineState::Indeterminate; s.at = clock_(); s.generation = generation_;
            return s;
        }
        if (have_ && cached_.generation == generation_) {
            double age = clock_() - cached_.at;
            if (age >= 0.0 && age <= std::min(max_age_s, kCacheSeconds)) return cached_;
        }
    }
    BaselineSnapshot m = measure();   // network I/O outside the lock
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || m.generation != generation_) {
        // The world changed while we were measuring (M5 disabled / network generation bumped):
        // the measurement is not evidence for anyone. Never cache it, never report it as ok.
        m.a_ok = m.b_ok = false;
        m.state = BaselineState::Indeterminate;
        m.enabled = enabled_;
        return m;   // keeps the stale generation => the classifier rejects it
    }
    cached_ = m; have_ = true;
    return m;
}

BaselineSnapshot ConnectivityBaseline::measure() {
    uint64_t gen = generation();
    TrafficProbe probe(transport_, cfg_, clock_);
    auto once = [&](bool* a, bool* b) {
        auto fb = std::async(std::launch::async, [&] { TraceInfo ti; return probe.checkB(0, &ti); });
        CheckOutcome ca = probe.checkA(0);
        CheckOutcome cb = fb.get();
        *a = ca.status == CheckStatus::Pass;
        *b = cb.status == CheckStatus::Pass;
    };
    BaselineSnapshot s;
    s.enabled = true;
    s.generation = gen;
    s.link_up = link_();
    once(&s.a_ok, &s.b_ok);
    if (!s.a_ok && !s.b_ok && !s.link_up) {
        // OS says no usable link: confirm once more 5 s later before declaring Offline.
        sleep_(5.0);
        s.link_up = link_();
        once(&s.a_ok, &s.b_ok);
    }
    s.at = clock_();
    if (s.a_ok || s.b_ok) s.state = BaselineState::Online;
    else if (!s.link_up) s.state = BaselineState::Offline;
    else s.state = BaselineState::Indeterminate;   // link exists but direct checks fail: censorship/DNS/captive portal possible
    return s;
}

// ── attribution ─────────────────────────────────────────────────────────
Attribution classifyRound(const RawProbe& raw, const BaselineSnapshot& base, const ControlTracker* controls,
                          const std::string& key, uint64_t generation) {
    Attribution out;
    const double t0 = raw.started_at - kWindowSlackSeconds;
    const double t1 = raw.finished_at + kWindowSlackSeconds;
    const bool bulk_failed = raw.bulk_run && raw.bulk.status != CheckStatus::Pass;

    if (raw.engine_died && !(raw.bothPass() && !bulk_failed)) {   // local crash: never the server's fault
        out.outcome = ProbeOutcome::EngineError;
        return out;
    }
    if (raw.bothPass()) {
        if (bulk_failed) { out.outcome = ProbeOutcome::Partial; out.attributable = false; return out; }   // A+B ok, bulk not
        out.outcome = ProbeOutcome::Pass; out.attributable = true; return out;
    }
    if (raw.engine_unreachable) { out.outcome = ProbeOutcome::EngineError; return out; }

    // Direct evidence only counts when it belongs to this round: same generation, enabled,
    // and captured inside the round window.
    const bool base_valid = base.enabled && base.generation == generation && base.at >= t0 && base.at <= t1;

    auto failedAttributable = [&](const CheckOutcome& c, bool direct_ok, uint32_t bit) {
        if (c.status != CheckStatus::Fail) return false;
        if (c.failure == CheckFailure::ProxyConnect) return false;   // local engine, never the server
        if (base_valid && direct_ok) return true;
        return controls && controls->passed(key, bit, t0, t1, generation);
    };
    bool attributable = failedAttributable(raw.a, base.a_ok, kCheckA) || failedAttributable(raw.b, base.b_ok, kCheckB);
    const bool one_passed = raw.a.status == CheckStatus::Pass || raw.b.status == CheckStatus::Pass;

    if (one_passed) {
        out.outcome = ProbeOutcome::Partial;
        out.attributable = attributable;
        return out;
    }
    if (attributable) { out.outcome = ProbeOutcome::RemoteFailure; out.attributable = true; return out; }
    out.attributable = false;
    out.outcome = (base_valid && base.state == BaselineState::Offline) ? ProbeOutcome::LocalNetworkDown
                                                                      : ProbeOutcome::Indeterminate;
    return out;
}

ProbeResult buildProbeResult(const RawProbe& raw, const Attribution& attr, const std::string& key,
                             const std::string& engine, const std::string& run_id, uint64_t generation) {
    ProbeResult r;
    r.endpoint_key = key;
    r.run_id = run_id;
    r.generation = generation;
    r.port = raw.port;
    r.engine = engine;
    r.outcome = attr.outcome;
    r.attributable = attr.attributable;
    r.started_at = raw.started_at;
    r.finished_at = raw.finished_at;
    if (raw.a.status == CheckStatus::Pass) r.checks |= kCheckA;
    if (raw.b.status == CheckStatus::Pass) r.checks |= kCheckB;
    if (attr.outcome == ProbeOutcome::Pass) {
        r.latency_ms = raw.latency_ms;
        r.exit_ip = raw.exit_ip;
        r.exit_country = raw.exit_country;
        if (raw.bulk_run && raw.bulk.status == CheckStatus::Pass) {
            r.bulk_passed = true;
            r.checks |= kCheckBulk;
            r.bytes = raw.bulk_bytes;
        }
    } else if (raw.b.status == CheckStatus::Pass) {
        r.exit_ip = raw.exit_ip;
        r.exit_country = raw.exit_country;
    }
    return r;
}

}  // namespace network
}  // namespace hunter
