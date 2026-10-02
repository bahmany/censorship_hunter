#include "core/health_score.h"
#include "test_support.h"

using namespace hunter;

static HealthThresholds TH;

static ProbeResult pass(double t, double lat = 100.0, bool bulk = false, const std::string& id = "") {
    ProbeResult r;
    r.outcome = ProbeOutcome::Pass;
    r.finished_at = r.started_at = t;
    r.latency_ms = lat;
    r.bulk_passed = bulk;
    r.run_id = id;
    return r;
}
static ProbeResult fail(double t, ProbeOutcome o = ProbeOutcome::RemoteFailure) {
    ProbeResult r;
    r.outcome = o;
    r.finished_at = r.started_at = t;
    r.attributable = (o == ProbeOutcome::RemoteFailure);  // baseline attribution is explicit for Partial
    return r;
}
static HealthEvaluation ev(const HealthEvidence& e, double now) { return evaluateHealth(e, now, TH); }

int main() {
    std::cout << "=== Health scoring / state machine ===\n";

    T_CASE("score vector [newest 1, older 0] gives S = 5/9");
    {
        HealthEvidence e;
        applyProbe(e, fail(1.0), TH);
        applyProbe(e, pass(2.0), TH);
        auto x = ev(e, 2.0);
        CHECK_NEAR(x.ewma, 5.0 / 9.0, 1e-12, "S=5/9");
    }
    T_END();

    T_CASE("EWMA edge cases");
    {
        HealthEvidence e;
        CHECK(ev(e, 0).ewma == 0.0, "empty ring S=0");
        CHECK(ev(e, 0).score == 0.0, "empty score 0");
        applyProbe(e, pass(1.0), TH);
        CHECK_NEAR(ev(e, 1.0).ewma, 1.0, 1e-12, "single pass S=1");
        HealthEvidence f;
        applyProbe(f, pass(1.0), TH);
        applyProbe(f, fail(2.0), TH);
        CHECK_NEAR(ev(f, 2.0).ewma, 0.8 / 1.8 * 1.0 * 0 + 0.2 * 0.8 / (0.2 + 0.2 * 0.8), 1e-12, "[newest 0, older 1]");
        HealthEvidence g;
        for (int i = 0; i < 30; i++) applyProbe(g, pass(1.0 + i), TH);
        CHECK(g.ring.size() == 20, "ring bounded at N=20");
        CHECK(g.eligible_count == 30, "eligible_count not truncated");
        CHECK_NEAR(ev(g, 40).ewma, 1.0, 1e-12, "all pass");
    }
    T_END();

    T_CASE("p90 nearest-rank boundaries");
    {
        auto p90 = [](int m) {
            HealthEvidence e;
            for (int i = 1; i <= m; i++) applyProbe(e, pass(i, 100.0 * i), TH);
            return ev(e, m).p90_ms;
        };
        CHECK(p90(1) == 100.0, "m=1 -> 1st");
        CHECK(p90(2) == 200.0, "m=2 -> ceil(1.8)=2nd");
        CHECK(p90(9) == 900.0, "m=9 -> ceil(8.1)=9th");
        CHECK(p90(10) == 900.0, "m=10 -> ceil(9.0)=9th (exact boundary)");
        CHECK(p90(11) == 1000.0, "m=11 -> ceil(9.9)=10th");
        CHECK(p90(20) == 1800.0, "m=20 -> 18th");
        HealthEvidence none;
        applyProbe(none, fail(1.0), TH);
        CHECK(!ev(none, 1).has_p90 && ev(none, 1).latency_factor == 0.0, "no successes: L=0");
        HealthEvidence one;
        applyProbe(one, pass(1.0, 500.0), TH);
        CHECK_NEAR(ev(one, 1).latency_factor, 0.5, 1e-12, "L at p90=500 is 0.5");
        HealthEvidence z;
        applyProbe(z, pass(1.0, 0.0), TH);
        CHECK_NEAR(ev(z, 1).latency_factor, 1.0, 1e-12, "L at 0 ms is 1");
    }
    T_END();

    T_CASE("freshness boundaries");
    {
        HealthEvidence e;
        CHECK(ev(e, 100).freshness == 0.0, "no success -> F=0");
        applyProbe(e, pass(1000.0), TH);
        CHECK_NEAR(ev(e, 1000.0).freshness, 1.0, 1e-12, "age 0");
        CHECK_NEAR(ev(e, 1300.0).freshness, 0.5, 1e-12, "age 300 -> 0.5");
        CHECK_NEAR(ev(e, 1600.0).freshness, 0.25, 1e-12, "age 600 -> 0.25");
        CHECK_NEAR(ev(e, 500.0).freshness, 1.0, 1e-12, "negative age clamps to 1");
    }
    T_END();

    T_CASE("protocol factor and score composition");
    {
        HealthEvidence e;
        applyProbe(e, pass(10.0, 500.0), TH);
        CHECK_NEAR(ev(e, 10.0).score, 50.0, 1e-9, "100*1*0.5*1*1");
        e.insecure_tls = true;
        CHECK_NEAR(ev(e, 10.0).score, 40.0, 1e-9, "insecure P=0.8");
        e.insecure_tls = false;
        e.unsupported = true;
        CHECK(ev(e, 10.0).score == 0.0, "unsupported P=0");
        e.unsupported = false;
        e.telegram_only = true;
        CHECK(ev(e, 10.0).score == 0.0, "telegram-only score 0");
        e.telegram_only = false;
        HealthEvidence fast;
        applyProbe(fast, pass(10.0, 0.0), TH);
        CHECK_NEAR(ev(fast, 10.0).score, 100.0, 1e-9, "max score 100");
    }
    T_END();

    T_CASE("Unknown -> Testing -> Healthy");
    {
        HealthEvidence e;
        CHECK(e.state == HealthState::Unknown, "initial");
        markTesting(e);
        CHECK(e.state == HealthState::Testing, "testing");
        applyProbe(e, pass(5.0), TH);
        CHECK(e.state == HealthState::Healthy, "first full pass -> Healthy");
        markTesting(e);
        CHECK(e.state == HealthState::Healthy, "markTesting only affects Unknown");
        CHECK(ev(e, 5.0).stability == Stability::Unstable, "one pass is not Stable");
        HealthEvidence u;
        CHECK(ev(u, 0).stability == Stability::Unrated, "no evidence: Unrated");
    }
    T_END();

    T_CASE("failure transitions and Dead needs 3 failures spanning >= 30 s");
    {
        HealthEvidence e;
        applyProbe(e, pass(0.0), TH);
        applyProbe(e, fail(100.0), TH);
        CHECK(e.state == HealthState::Degraded, "1 failure -> Degraded");
        applyProbe(e, fail(110.0), TH);
        CHECK(e.state == HealthState::Unstable, "2 failures -> Unstable");
        applyProbe(e, fail(120.0), TH);
        CHECK(e.state == HealthState::Unstable, "3 failures spanning 20 s -> still Unstable");
        applyProbe(e, fail(130.0), TH);
        CHECK(e.state == HealthState::Dead, "span 30 s -> Dead");
        CHECK(ev(e, 130.0).stability == Stability::Dead, "stability Dead");
        CHECK(e.failure_streak == 4, "failure streak counted");
        applyProbe(e, fail(131.0), TH);
        CHECK(e.state == HealthState::Dead, "stays Dead");
        HealthEvidence f;
        applyProbe(f, fail(0.0), TH);
        applyProbe(f, fail(15.0), TH);
        applyProbe(f, fail(30.0), TH);
        CHECK(f.state == HealthState::Dead, "exactly 30 s span with 3 failures -> Dead");
        CHECK(f.eligible_count == 3, "eligible");
    }
    T_END();

    T_CASE("recovery needs two full passes >= 15 s apart");
    {
        HealthEvidence e;
        for (double t : {0.0, 15.0, 30.0}) applyProbe(e, fail(t), TH);
        CHECK(e.state == HealthState::Dead, "dead");
        applyProbe(e, pass(100.0), TH);
        CHECK(e.state == HealthState::Degraded, "first recovery pass remains Degraded");
        CHECK(e.failure_streak == 0, "failure streak reset on full pass");
        applyProbe(e, pass(110.0), TH);
        CHECK(e.state == HealthState::Degraded, "second pass only 10 s later: still Degraded");
        applyProbe(e, pass(114.9), TH);
        CHECK(e.state == HealthState::Degraded, "14.9 s after first: still Degraded");
        applyProbe(e, pass(115.0), TH);
        CHECK(e.state == HealthState::Healthy, "15 s after first pass -> Healthy");
        // failure during recovery restarts it
        HealthEvidence g;
        applyProbe(g, pass(0.0), TH);
        applyProbe(g, fail(10.0), TH);
        applyProbe(g, pass(20.0), TH);
        applyProbe(g, fail(25.0), TH);
        applyProbe(g, pass(30.0), TH);
        applyProbe(g, pass(40.0), TH);
        CHECK(g.state == HealthState::Degraded, "recovery restarted after failure");
        applyProbe(g, pass(45.0), TH);
        CHECK(g.state == HealthState::Healthy, "recovered");
    }
    T_END();

    T_CASE("excluded outcomes never count");
    {
        const ProbeOutcome excluded[] = {ProbeOutcome::LocalNetworkDown, ProbeOutcome::Indeterminate,
                                         ProbeOutcome::EngineError, ProbeOutcome::BindConflict,
                                         ProbeOutcome::Unsupported, ProbeOutcome::InvalidConfig,
                                         ProbeOutcome::Cancelled};
        for (auto o : excluded) {
            HealthEvidence e;
            applyProbe(e, pass(0.0), TH);
            applyProbe(e, fail(10.0), TH);  // Degraded, failure streak 1
            auto before = e;
            auto eff = applyProbe(e, fail(20.0, o), TH);
            CHECK(eff == ApplyEffect::Excluded, std::string("excluded: ") + outcomeName(o));
            CHECK(e.eligible_count == before.eligible_count && e.ring.size() == before.ring.size(), "no sample");
            CHECK(e.failure_streak == before.failure_streak && e.state == before.state, "no state change");
            CHECK(e.failure_streak_started_at == before.failure_streak_started_at, "streak start intact");
            CHECK(e.last_attempt_at == 20.0 && e.last_outcome == outcomeName(o), "attempt recorded");
            CHECK(!isEligibleResult(fail(0, o)), "not eligible");
        }
        // excluded rounds neither reset nor increment a failure streak, and can't bridge a gap
        HealthEvidence g;
        applyProbe(g, fail(0.0), TH);
        applyProbe(g, fail(10.0, ProbeOutcome::LocalNetworkDown), TH);
        applyProbe(g, fail(20.0, ProbeOutcome::Indeterminate), TH);
        CHECK(g.failure_streak == 1, "streak unchanged by excluded");
        HealthEvidence h;
        applyProbe(h, pass(1.0), TH);
        applyProbe(h, fail(50.0, ProbeOutcome::EngineError), TH);
        applyProbe(h, pass(100.0), TH);
        CHECK(h.success_streak == 1 && h.streak_started_at == 100.0, "excluded round cannot bridge a >90 s gap");
        // non-attributable remote failure is excluded; attributable Partial counts as failure
        HealthEvidence p;
        applyProbe(p, pass(0.0), TH);
        ProbeResult na = fail(10.0);
        na.attributable = false;
        CHECK(applyProbe(p, na, TH) == ApplyEffect::Excluded && p.state == HealthState::Healthy, "unattributed failure excluded");
        ProbeResult part = fail(20.0, ProbeOutcome::Partial);
        part.attributable = true;
        CHECK(applyProbe(p, part, TH) == ApplyEffect::Applied && p.failure_streak == 1, "attributable partial = failure");
        HealthEvidence q;
        applyProbe(q, pass(0.0), TH);
        CHECK(applyProbe(q, fail(10.0, ProbeOutcome::Partial), TH) == ApplyEffect::Excluded, "default partial excluded from ring");
        CHECK(q.state == HealthState::Degraded && q.failure_streak == 0 && q.success_streak == 0, "partial -> Degraded, no death increment");
    }
    T_END();

    T_CASE("duplicate run ids, stale generations, bad input");
    {
        HealthEvidence e;
        CHECK(applyProbe(e, pass(1.0, 10, false, "r1"), TH) == ApplyEffect::Applied, "first");
        CHECK(applyProbe(e, pass(2.0, 10, false, "r1"), TH) == ApplyEffect::Duplicate, "duplicate id discarded");
        CHECK(e.eligible_count == 1, "duplicate not counted");
        ProbeResult g2 = pass(3.0);
        g2.generation = 5;
        CHECK(applyProbe(e, g2, TH) == ApplyEffect::Applied, "gen 5");
        ProbeResult g1 = pass(4.0);
        g1.generation = 4;
        CHECK(applyProbe(e, g1, TH) == ApplyEffect::Stale, "older generation discarded");
        CHECK(applyProbe(e, pass(2.5), TH) == ApplyEffect::Stale, "out-of-order result discarded");
        CHECK(applyProbe(e, pass(5.0, std::nan("")), TH) == ApplyEffect::Rejected, "NaN latency rejected");
        CHECK(applyProbe(e, pass(5.0, -1.0), TH) == ApplyEffect::Rejected, "negative latency rejected");
        ProbeResult bad = pass(std::nan(""));
        CHECK(applyProbe(e, bad, TH) == ApplyEffect::Rejected, "NaN time rejected");
        CHECK(e.eligible_count == 2, "rejected not counted");
    }
    T_END();

    T_CASE("Stable criteria with fake clock");
    {
        auto run = [](double step, int n, double lat, bool bulk) {
            HealthEvidence e;
            for (int i = 0; i < n; i++) applyProbe(e, pass(1000.0 + step * i, lat, bulk), TH);
            return e;
        };
        HealthEvidence e = run(60.0, 6, 200.0, true);  // 6 passes, span 300 s
        double last = 1000.0 + 300.0;
        CHECK(ev(e, last).stable && ev(e, last).stability == Stability::Stable, "6 passes / 300 s -> Stable");
        CHECK(ev(e, last).tier == 0, "tier 0");
        CHECK(!ev(run(60.0, 5, 200.0, true), 1240.0).stable, "5 passes not enough");
        CHECK(!ev(run(59.0, 6, 200.0, true), 1000.0 + 295.0).stable, "span 295 s < 300");
        CHECK(!ev(run(60.0, 6, 3001.0, true), last).stable, "p90 3001 ms > 3000");
        CHECK(ev(run(60.0, 6, 3000.0, true), last).stable, "p90 exactly 3000 ms ok");
        CHECK(!ev(run(60.0, 6, 200.0, false), last).stable, "no bulk success");
        CHECK(ev(e, last + 180.0).stable, "success age exactly 180 s ok");
        CHECK(!ev(e, last + 180.5).stable && ev(e, last + 181.0).stability == Stability::Unstable, "stale certification");
        CHECK(!ev(e, last + 181.0).switch_eligible, "stale not switch eligible");
        CHECK(!ev(e, last + 301.0).stable, "bulk older than 300 s");
        // gap > 90 s restarts the run
        HealthEvidence g;
        for (int i = 0; i < 5; i++) applyProbe(g, pass(1000.0 + 60.0 * i, 200, true), TH);
        applyProbe(g, pass(1240.0 + 91.0, 200, true), TH);
        CHECK(g.success_streak == 1 && g.streak_started_at == 1331.0, "gap 91 s restarts streak");
        HealthEvidence g2;
        for (int i = 0; i < 5; i++) applyProbe(g2, pass(1000.0 + 60.0 * i, 200, true), TH);
        applyProbe(g2, pass(1240.0 + 90.0, 200, true), TH);
        CHECK(g2.success_streak == 6, "gap exactly 90 s keeps streak");
        // S >= 0.90 threshold, with hand-built evidence: [F,F,F,P x6] vs [F,P x6]
        auto crafted = [](int fails) {
            HealthEvidence x;
            x.state = HealthState::Healthy;
            x.eligible_count = fails + 6;
            x.success_streak = 6;
            x.streak_started_at = 1000.0;
            x.last_full_success = 1300.0;
            x.last_bulk_success = 1300.0;
            double t = 1000.0 - 10.0 * fails;
            for (int i = 0; i < fails; i++, t += 10.0) x.ring.push_back({"", t, false, -1.0});
            for (int i = 0; i < 6; i++) x.ring.push_back({"", 1000.0 + 60.0 * i, true, 200.0});
            return x;
        };
        std::string why;
        CHECK(evidenceConsistent(crafted(3), TH, &why), why);
        CHECK(ev(crafted(3), 1300.0).ewma < 0.90 && !ev(crafted(3), 1300.0).stable, "S < 0.90 blocks Stable");
        CHECK(ev(crafted(1), 1300.0).ewma >= 0.90 && ev(crafted(1), 1300.0).stable, "S >= 0.90 allows Stable");
        // failure removes eligibility immediately
        applyProbe(e, fail(last + 10.0), TH);
        CHECK(!ev(e, last + 10.0).stable && e.success_streak == 0, "first failure removes Stable");
        // Healthy-only
        HealthEvidence d = run(60.0, 6, 200.0, true);
        applyProbe(d, fail(1301.0, ProbeOutcome::Partial), TH);
        CHECK(!ev(d, 1301.0).stable && d.state == HealthState::Degraded, "Degraded is never Stable");
        // local outage preserves certification (excluded)
        HealthEvidence k = run(60.0, 6, 200.0, true);
        applyProbe(k, fail(1310.0, ProbeOutcome::LocalNetworkDown), TH);
        CHECK(ev(k, 1310.0).stable, "local outage preserves certification");
    }
    T_END();

    T_CASE("ranking: tier, score, latest success, key");
    {
        RankKey a{0, 10, 5, "b"}, b{1, 99, 9, "a"};
        CHECK(rankedBefore(a, b) && !rankedBefore(b, a), "tier first");
        RankKey c{1, 50, 1, "z"}, d{1, 60, 0, "a"};
        CHECK(rankedBefore(d, c), "score desc");
        RankKey e1{1, 50, 9, "z"}, e2{1, 50, 5, "a"};
        CHECK(rankedBefore(e1, e2), "latest success");
        RankKey f1{1, 50, 5, "a"}, f2{1, 50, 5, "b"};
        CHECK(rankedBefore(f1, f2) && !rankedBefore(f2, f1) && !rankedBefore(f1, f1), "key lexicographic");
    }
    T_END();

    T_CASE("evidence consistency validator");
    {
        HealthEvidence e;
        for (int i = 0; i < 3; i++) applyProbe(e, pass(100.0 + i * 10.0, 100, true), TH);
        std::string why;
        CHECK(evidenceConsistent(e, TH, &why), why);
        HealthEvidence b = e; b.state = HealthState::Dead;
        CHECK(!evidenceConsistent(b, TH, &why), "dead without failures");
        b = e; b.success_streak = 99;
        CHECK(!evidenceConsistent(b, TH, &why), "streak > eligible");
        b = e; b.ring.back().success = false; b.ring.back().latency_ms = -1;
        CHECK(!evidenceConsistent(b, TH, &why), "ring contradicts streak");
        b = e; b.eligible_count = 1;
        CHECK(!evidenceConsistent(b, TH, &why), "eligible < ring");
        b = e; b.failure_streak = 1; b.failure_streak_started_at = 5;
        CHECK(!evidenceConsistent(b, TH, &why), "both streaks");
    }
    T_END();

    return T_SUMMARY();
}
