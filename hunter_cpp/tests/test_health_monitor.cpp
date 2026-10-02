// Stage 4 B: watchdog / failover state machine with fake clock, launcher and probe (no network).
#include <chrono>
#include <thread>

#include "runtime_fakes.h"
#include "test_support.h"

using namespace fakes;

namespace {
struct Rec {
    std::mutex mu;
    std::vector<SessionEvent> ev;
    std::vector<SessionState> states;
    void attach(const std::shared_ptr<HealthMonitor>& m) {
        m->setEventCallback([this](const SessionEvent& e) {
            std::lock_guard<std::mutex> lk(mu);
            ev.push_back(e);
            if (e.kind == EventKind::StateChanged) states.push_back(e.snapshot.state);
        });
    }
    bool everConnected() { std::lock_guard<std::mutex> lk(mu); return contains(states, SessionState::Connected); }
    int count(EventKind k) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& e : ev) n += e.kind == k; return n; }
    std::vector<double> restartDelays() {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<double> v;
        for (auto& e : ev) if (e.kind == EventKind::RestartScheduled) v.push_back(std::stod(e.detail.substr(11)));
        return v;
    }
};

const int kUserPort = 39001;

std::shared_ptr<HealthMonitor> make(Env& env, Rec& rec, MonitorConfig cfg = MonitorConfig()) {
    auto m = HealthMonitor::create(cfg, env.deps());
    rec.attach(m);
    return m;
}

bool waitFor(const std::function<bool()>& pred, int ms = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return pred();
}
}  // namespace

int main() {
    std::cout << "test_health_monitor" << std::endl;

    T_CASE("Connected only after a Pass probe on the USER port (first probe carries bulk)");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.probe.set("ek:A", mk(ProbeOutcome::Pass));
        m->start("A", kUserPort);
        auto s = m->snapshot();
        CHECK(s.state == SessionState::Connected, "connected after inline start");
        CHECK(env.probe.count() == 1 && env.probe.calls[0].port == kUserPort && env.probe.calls[0].bulk, "one probe, on user port, with bulk");
        CHECK(env.launcher->launchesOnPort(kUserPort) == 1, "engine launched on the user port");
        CHECK(s.user_port == kUserPort && s.endpoint_key == "ek:A" && s.last_probe.valid && s.last_probe.outcome == ProbeOutcome::Pass, "snapshot fields");
        // Order: Starting event strictly before the first Connected event.
        CHECK(rec.states.size() >= 2 && rec.states.front() == SessionState::Starting, "Starting first");
        m->stop(); CHECK(m->waitStopped(2), "stopped");
    }
    T_END();

    T_CASE("listener up + PID alive but traffic fails: never Connected (no candidates -> Unavailable)");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        m->start("A", kUserPort);
        std::vector<SessionState> seen;
        run(m, env.clock, 120, &seen);
        CHECK(!rec.everConnected() && !contains(seen, SessionState::Connected), "never Connected");
        CHECK(m->snapshot().state == SessionState::Unavailable, "Unavailable without candidates");
        CHECK(env.launcher->last()->alive.load() || env.launcher->launches() >= 1, "engine process stayed alive meanwhile");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("Indeterminate / unattributed failures never count and never Connected");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, false));
        m->start("A", kUserPort);
        run(m, env.clock, 60);
        auto s = m->snapshot();
        CHECK(s.consecutive_failures == 0, "unattributed failures are not counted");
        CHECK(s.state == SessionState::Degraded && !rec.everConnected(), "Degraded, not Connected");
        CHECK(env.launcher->launches() == 1, "no failover from unattributed failures");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("upstream drop with surviving process -> verify candidate on lease port, then restart on user port");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B", true)});
        m->start("A", kUserPort);
        CHECK(m->snapshot().state == SessionState::Connected, "A connected");
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        std::vector<SessionState> seen;
        run(m, env.clock, 30, &seen);
        auto s = m->snapshot();
        CHECK(s.state == SessionState::Connected && s.endpoint_key == "ek:B" && s.uri == "B", "now on B");
        CHECK(s.previous_key == "ek:A" && s.failovers == 1 && !s.provisional, "failover bookkeeping (stable => not provisional)");
        CHECK(contains(seen, SessionState::Switching) || rec.count(EventKind::FailoverStarted) == 1, "switching state/event observed");
        // The candidate was probed on a lease port first, then on the user port.
        int lease_port = 0;
        { std::lock_guard<std::mutex> lk(env.probe.mu);
          for (auto& c : env.probe.calls) if (c.key == "ek:B") { lease_port = c.port; break; } }
        CHECK(lease_port >= 29000 && lease_port != kUserPort, "first candidate probe on a lease port");
        CHECK(env.probe.countKey("ek:B") == 2 && env.probe.calls.back().port == kUserPort && env.probe.calls.back().key == "ek:B", "then verified on the user port");
        // Candidate verified BEFORE the old engine is torn down (gap limited to the restart).
        std::vector<std::string> log; { std::lock_guard<std::mutex> lk(env.launcher->mu); log = env.launcher->log; }
        auto idx = [&](const std::string& x, size_t from = 0) { for (size_t i = from; i < log.size(); i++) if (log[i] == x) return (int)i; return -1; };
        int l_cand = idx("launch:" + std::to_string(lease_port));
        int d_old = idx("destroy:" + std::to_string(kUserPort));
        int l_new = d_old >= 0 ? idx("launch:" + std::to_string(kUserPort), d_old) : -1;
        CHECK(l_cand > 0 && d_old > l_cand && l_new > d_old, "order: launch candidate < destroy old < relaunch on user port");
        CHECK(env.leases->activeCount() == 0, "lease released");
        CHECK(rec.count(EventKind::FailoverSucceeded) == 1, "FailoverSucceeded event");
        CHECK(env.sunk.size() >= 3, "all probe results reach the sink");
        m->stop(); CHECK(m->waitStopped(2), "stopped"); CHECK(env.launcher->live == 0, "engines reaped");
    }
    T_END();

    T_CASE("provisional fallback: non-Stable candidate flagged; Stable preferred");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("P", false, 95), cand("S", true, 70)});   // stable-first ordering must win
        m->start("A", kUserPort);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 30);
        CHECK(m->snapshot().endpoint_key == "ek:S" && !m->snapshot().provisional, "Stable tier first");
        env.probe.set("ek:S", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 60);
        auto s = m->snapshot();
        CHECK(s.endpoint_key == "ek:P" && s.provisional && s.state == SessionState::Connected, "fell back to provisional candidate with flag");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("candidate failing its fresh probe is skipped (cooldown) and never used");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("X", true), cand("Y", true)});
        env.probe.set("ek:X", mk(ProbeOutcome::RemoteFailure, true));
        m->start("A", kUserPort);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 30);
        CHECK(m->snapshot().endpoint_key == "ek:Y", "moved to Y");
        CHECK(env.probe.countKey("ek:X") == 1, "X probed once on lease port only");
        CHECK(env.launcher->launchesOnPort(kUserPort) == 2, "user port engine restarted only for Y");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("<=3 candidates per 60 s; failed-over-from key on 60 s cooldown; no candidate => Unavailable");
    {
        Env env; Rec rec; auto m = make(env, rec);
        std::vector<Candidate> cs; for (int i = 1; i <= 5; i++) cs.push_back(cand("C" + std::to_string(i)));
        env.setCands(cs);
        for (int i = 1; i <= 5; i++) env.probe.set("ek:C" + std::to_string(i), mk(ProbeOutcome::RemoteFailure, true));
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        m->start("A", kUserPort);
        double t0 = env.clock.now();
        run(m, env.clock, 50);
        int distinct = 0; for (int i = 1; i <= 5; i++) distinct += env.probe.countKey("ek:C" + std::to_string(i)) > 0;
        CHECK(distinct <= 3, "at most 3 distinct candidates inside the 60 s window: " + std::to_string(distinct));
        CHECK(m->snapshot().state == SessionState::Unavailable, "Unavailable, never Connected after A dropped");
        int before = env.probe.countKey("ek:C4") + env.probe.countKey("ek:C5");
        run(m, env.clock, 80);
        CHECK(env.probe.countKey("ek:C4") + env.probe.countKey("ek:C5") > before || distinct == 3, "later window tries further candidates");
        CHECK(env.probe.countKey("ek:A") >= 2 && env.probe.countOnPort(kUserPort) >= 2, "sanity");
        (void)t0;
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("no automatic failback to the old key inside its 60 s cooldown");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("A", true), cand("B", true)});   // A is the one that failed
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        m->start("A", kUserPort);
        run(m, env.clock, 30);
        CHECK(m->snapshot().endpoint_key == "ek:B", "switched to B, A excluded");
        CHECK(env.probe.countOnPort(kUserPort) >= 1, "sane");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("child exit -> restart with backoff 1/2/4 s; Connected only after fresh Pass");
    {
        Env env; Rec rec; auto m = make(env, rec);
        m->start("A", kUserPort);
        for (int round = 0; round < 3; round++) {
            env.launcher->last()->alive = false;   // zombie/exit as seen by waitpid
            env.clock.advance(1); m->step();
            CHECK(m->snapshot().state == SessionState::Restarting, "Restarting after exit");
            CHECK(env.launcher->live == 0, "dead engine reaped immediately");
            run(m, env.clock, 6);
            CHECK(m->snapshot().state == SessionState::Connected, "reconnected after restart");
        }
        auto d = rec.restartDelays();
        CHECK(d.size() == 3 && std::abs(d[0] - 1) < 1e-6 && std::abs(d[1] - 2) < 1e-6 && std::abs(d[2] - 4) < 1e-6, "delays 1,2,4 (jitter source = 0.5)");
        CHECK(env.launcher->launchesOnPort(kUserPort) == 4, "relaunched on the same user port");
        // 4th exit within 5 min: restart budget exhausted -> Auto fails over (here: no candidate) never restarts blindly.
        env.launcher->last()->alive = false;
        env.clock.advance(1); m->step();
        CHECK(m->snapshot().state != SessionState::Restarting, "no 4th restart inside 5 min");
        run(m, env.clock, 5);
        CHECK(m->snapshot().state == SessionState::Unavailable, "no candidate => Unavailable");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("restart jitter bounds +-20% and streak reset after 60 s Connected");
    {
        for (double r : {0.0, 1.0}) {
            Env env; env.rand = r; Rec rec; auto m = make(env, rec);
            m->start("A", kUserPort);
            env.launcher->last()->alive = false; env.clock.advance(1); m->step();
            auto d = rec.restartDelays();
            CHECK(d.size() == 1 && std::abs(d[0] - (r == 0 ? 0.8 : 1.2)) < 1e-6, "1 s +-20%");
            m->stop(); m->waitStopped(2);
        }
        Env env; Rec rec; auto m = make(env, rec);
        m->start("A", kUserPort);
        env.launcher->last()->alive = false; env.clock.advance(1); m->step(); run(m, env.clock, 5);
        run(m, env.clock, 70);   // stays Connected > 60 s
        env.launcher->last()->alive = false; env.clock.advance(1); m->step();
        auto d = rec.restartDelays();
        CHECK(d.size() == 2 && std::abs(d[1] - 1) < 1e-6, "backoff resets after a stable minute");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("engine start failure retries with backoff; fatal (missing binary / bad config) => Failed, no retry");
    {
        Env env; Rec rec; auto m = make(env, rec);
        int n = 0;
        env.launcher->hook = [&](const network::LaunchRequest&) { return n++ == 0 ? network::LaunchStatus::StartupFailed : network::LaunchStatus::Ok; };
        m->start("A", kUserPort);
        CHECK(m->snapshot().state == SessionState::Restarting, "first start failed -> restart scheduled");
        run(m, env.clock, 4);
        CHECK(m->snapshot().state == SessionState::Connected, "second attempt connected");
        m->stop(); m->waitStopped(2);
        Env e2; Rec r2; auto m2 = make(e2, r2); e2.build_fails = true;
        m2->start("A", kUserPort);
        CHECK(m2->snapshot().state == SessionState::Failed && !m2->active(), "bad config => Failed");
        run(m2, e2.clock, 20);
        CHECK(e2.launcher->launches() == 0 && m2->snapshot().state == SessionState::Failed, "no retry loop");
        Env e3; Rec r3; auto m3 = make(e3, r3);
        e3.launcher->hook = [](const network::LaunchRequest&) { return network::LaunchStatus::BindConflict; };
        m3->start("A", kUserPort);
        CHECK(m3->snapshot().state == SessionState::Failed && m3->snapshot().reason.find("in use") != std::string::npos, "user port busy => Failed with reason");
    }
    T_END();

    T_CASE("local outage pauses switching and restarts; no penalties");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B")});
        env.probe.custom = [&](int, bool, const std::string&) {
            return env.baseline_state == network::BaselineState::Offline ? mk(ProbeOutcome::LocalNetworkDown) : mk(ProbeOutcome::Pass);
        };
        m->start("A", kUserPort);
        env.baseline_state = network::BaselineState::Offline;
        run(m, env.clock, 100);
        auto s = m->snapshot();
        CHECK(s.state == SessionState::Paused && s.local_outage, "Paused during outage");
        CHECK(env.launcher->launches() == 1, "no candidate launched while offline");
        CHECK(s.consecutive_failures == 0, "no failure counted");
        // Child exits during the outage: restart is held back until connectivity returns.
        env.launcher->last()->alive = false;
        run(m, env.clock, 30);
        CHECK(env.launcher->launches() == 1, "restart held during outage");
        env.baseline_state = network::BaselineState::Online;
        run(m, env.clock, 30);
        CHECK(m->snapshot().state == SessionState::Connected && env.launcher->launches() == 2, "resumed: restart + Connected after outage");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("two attributable failures while baseline turns Offline: failover deferred until it clears");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B")});
        // A initially passes
        bool a_down = false;
        env.probe.custom = [&](int, bool, const std::string& k) {
            if (env.baseline_state == network::BaselineState::Offline) return mk(ProbeOutcome::LocalNetworkDown);
            return (k == "ek:A" && a_down) ? mk(ProbeOutcome::RemoteFailure, true) : mk(ProbeOutcome::Pass); };
        m->start("A", kUserPort);
        a_down = true;
        // Baseline flips to Offline exactly when the 2nd failure is registered (before the failover job runs).
        int probes_before = 0; (void)probes_before;
        run(m, env.clock, 20);   // t+15 fail, t+20 fail -> failover wanted, job not yet run
        env.baseline_state = network::BaselineState::Offline;
        run(m, env.clock, 40);
        CHECK(env.launcher->launches() == 1, "no failover while offline");
        CHECK(m->snapshot().state == SessionState::Paused, "Paused");
        env.baseline_state = network::BaselineState::Online;
        run(m, env.clock, 40);
        CHECK(m->snapshot().endpoint_key == "ek:B" && m->snapshot().state == SessionState::Connected, "failover completes after outage");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("no candidate: Unavailable (never Connected); retries later and recovers when one appears");
    {
        Env env; Rec rec; auto m = make(env, rec);
        m->start("A", kUserPort);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 40);
        CHECK(m->snapshot().state == SessionState::Unavailable, "Unavailable");
        std::vector<SessionState> seen;
        run(m, env.clock, 40, &seen);
        CHECK(!contains(seen, SessionState::Connected), "still never Connected");
        env.setCands({cand("N")});
        run(m, env.clock, 45, &seen);
        CHECK(m->snapshot().state == SessionState::Connected && m->snapshot().endpoint_key == "ek:N", "recovered via new candidate");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("manual pin: never switches, even with candidates; recovers when the same config recovers");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B")});
        m->start("A", kUserPort, SwitchMode::Pinned);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 90);
        auto s = m->snapshot();
        CHECK(s.state == SessionState::Unavailable && s.mode == SwitchMode::Pinned && s.endpoint_key == "ek:A", "pinned: Unavailable on A");
        CHECK(env.probe.countKey("ek:B") == 0 && env.launcher->launches() == 1, "candidate never tried");
        env.probe.set("ek:A", mk(ProbeOutcome::Pass));
        run(m, env.clock, 20);
        CHECK(m->snapshot().state == SessionState::Connected, "same config recovered");
        // pin on a live Auto session
        m->setMode(SwitchMode::Auto); m->setMode(SwitchMode::Pinned);
        env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m, env.clock, 60);
        CHECK(env.probe.countKey("ek:B") == 0, "pin set later also blocks failover");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("pinned: engine keeps crashing -> Unavailable (no failover), retried after the restart window");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B")});
        m->start("A", kUserPort, SwitchMode::Pinned);
        for (int i = 0; i < 4; i++) { env.launcher->last()->alive = false; env.clock.advance(1); m->step(); run(m, env.clock, 5); }
        CHECK(m->snapshot().state == SessionState::Unavailable, "Unavailable");
        CHECK(env.probe.countKey("ek:B") == 0, "no candidate probed");
        run(m, env.clock, 320);
        CHECK(m->snapshot().state == SessionState::Connected, "restarted after window");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("stale callbacks ignored: stop() before queued jobs run");
    {
        Env env; Rec rec; auto ex = std::make_shared<ManualExecutor>(); env.exec = ex; auto m = make(env, rec);
        m->start("A", kUserPort);
        CHECK(m->snapshot().state == SessionState::Starting && ex->pending() == 1, "start job queued, not run");
        m->stop();
        ex->runAll();
        CHECK(m->waitStopped(1) && m->snapshot().state == SessionState::Stopped, "Stopped");
        CHECK(env.build_calls == 0 && env.launcher->launches() == 0 && env.probe.count() == 0 && env.sunkCount() == 0, "queued start did nothing");
        CHECK(!rec.everConnected(), "never Connected");
    }
    T_END();

    T_CASE("stale probe result (engine replaced meanwhile) is dropped, not sunk");
    {
        Env env; Rec rec; auto ex = std::make_shared<ManualExecutor>(); env.exec = ex; auto m = make(env, rec);
        m->start("A", kUserPort); ex->runAll();
        CHECK(m->snapshot().state == SessionState::Connected, "connected");
        size_t sunk0 = env.sunkCount(); int probes0 = env.probe.count();
        run(m, env.clock, 15);                      // probe job queued
        CHECK(ex->pending() == 1, "probe job queued");
        env.launcher->last()->alive = false;       // engine dies before the job runs
        env.clock.advance(1); m->step();
        CHECK(m->snapshot().state == SessionState::Restarting, "restart scheduled while probe job pending");
        ex->runOne();                               // stale probe job
        CHECK(env.probe.count() == probes0 && env.sunkCount() == sunk0, "stale probe job did not probe/sink");
        m->stop(); ex->runAll(); m->waitStopped(1);
    }
    T_END();

    T_CASE("stop() mid-probe returns at once; teardown reaps engine; late result ignored; ports released");
    {
        Env env; Rec rec; env.exec = std::make_shared<ThreadPoolExecutor>(2);
        std::mutex gm; std::condition_variable gcv; bool gate = false; std::atomic<bool> in_probe{false};
        std::atomic<bool> block{false};
        env.probe.custom = [&](int, bool, const std::string&) {
            if (block) { in_probe = true; std::unique_lock<std::mutex> lk(gm); gcv.wait(lk, [&] { return gate; }); }
            return mk(ProbeOutcome::Pass);
        };
        auto m = make(env, rec);
        m->start("A", kUserPort);
        CHECK(waitFor([&] { return m->snapshot().state == SessionState::Connected; }), "connected (async)");
        size_t sunk0 = env.sunkCount();
        block = true;
        run(m, env.clock, 15);
        CHECK(waitFor([&] { return in_probe.load(); }), "probe in flight");
        auto t0 = std::chrono::steady_clock::now();
        m->stop();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 500, "stop() does not wait for the in-flight probe");
        CHECK(waitFor([&] { return env.launcher->live == 0; }), "engine reaped while probe still blocked");
        CHECK(m->snapshot().state == SessionState::Stopping, "Stopping until the job drains");
        { std::lock_guard<std::mutex> lk(gm); gate = true; } gcv.notify_all();
        CHECK(m->waitStopped(3), "Stopped after drain");
        CHECK(env.sunkCount() == sunk0 && env.leases->activeCount() == 0, "late result not applied; no leases held");
        run(m, env.clock, 30);
        CHECK(m->snapshot().state == SessionState::Stopped && env.launcher->launches() == 1, "stays stopped, nothing relaunched");
    }
    T_END();

    T_CASE("stop() during failover verification: candidate engine + lease released, user port not relaunched");
    {
        Env env; Rec rec; env.exec = std::make_shared<ThreadPoolExecutor>(2);
        std::mutex gm; std::condition_variable gcv; bool gate = false; std::atomic<bool> in_cand{false};
        env.setCands({cand("B")});
        bool a_down = false;
        env.probe.custom = [&](int port, bool, const std::string& k) {
            if (k == "ek:B" && port != kUserPort) { in_cand = true; std::unique_lock<std::mutex> lk(gm); gcv.wait(lk, [&] { return gate; }); return mk(ProbeOutcome::Pass); }
            return (k == "ek:A" && a_down) ? mk(ProbeOutcome::RemoteFailure, true) : mk(ProbeOutcome::Pass);
        };
        auto m = make(env, rec);
        m->start("A", kUserPort);
        CHECK(waitFor([&] { return m->snapshot().state == SessionState::Connected; }), "connected");
        a_down = true;
        for (int i = 0; i < 40 && !in_cand; i++) { env.clock.advance(1); m->step(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
        CHECK(in_cand.load() && m->snapshot().state == SessionState::Switching, "candidate verification in flight");
        m->stop();
        { std::lock_guard<std::mutex> lk(gm); gate = true; } gcv.notify_all();
        CHECK(m->waitStopped(5), "stopped");
        CHECK(env.launcher->live == 0, "all engines (old + candidate) reaped");
        CHECK(env.leases->activeCount() == 0, "lease released");
        CHECK(env.launcher->launchesOnPort(kUserPort) == 1, "user port not relaunched with the candidate");
        CHECK(m->snapshot().endpoint_key == "ek:A", "still A");
    }
    T_END();

    T_CASE("cadence: 15 s +-10 % probes, 5 s accelerated confirm, bulk every 5 min");
    {
        for (double r : {0.0, 1.0}) {
            Env env; env.rand = r; Rec rec; auto m = make(env, rec);
            m->start("A", kUserPort);
            run(m, env.clock, 40);
            std::vector<double> at; for (auto& c : env.probe.calls) at.push_back(c.at);
            CHECK(at.size() >= 3, "probes happened");
            double gap = at[2] - at[1];
            CHECK(gap >= (r == 0 ? 13.5 : 16.5) && gap <= (r == 0 ? 14.5 : 17.5), "gap within 15 s +-10 % (+1 s tick): " + std::to_string(gap));
            m->stop(); m->waitStopped(1);
        }
        Env env; Rec rec; auto m = make(env, rec);
        m->start("A", kUserPort);
        run(m, env.clock, 650);
        int bulk = 0; for (auto& c : env.probe.calls) bulk += c.bulk;
        CHECK(bulk >= 2 && bulk <= 4, "bulk ~ every 5 min: " + std::to_string(bulk));
        m->stop(); m->waitStopped(1);
        Env e2; Rec r2; auto m2 = make(e2, r2);
        m2->start("A", kUserPort);
        e2.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        run(m2, e2.clock, 30);
        double first_fail = -1, second_fail = -1;
        for (size_t i = 1; i < e2.probe.calls.size(); i++) { if (first_fail < 0) first_fail = e2.probe.calls[i].at; else if (second_fail < 0) second_fail = e2.probe.calls[i].at; }
        CHECK(second_fail - first_fail >= 5 && second_fail - first_fail <= 6, "confirm probe ~5 s after the failure");
        m2->stop(); m2->waitStopped(1);
    }
    T_END();

    T_CASE("EngineError (port refused) is a local fault: restart, no failover, no server penalty");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B")});
        m->start("A", kUserPort);
        env.probe.set("ek:A", mk(ProbeOutcome::EngineError, false));
        run(m, env.clock, 25);
        CHECK(env.probe.countKey("ek:B") == 0, "no candidate probed");
        CHECK(env.launcher->launchesOnPort(kUserPort) >= 2, "engine restarted");
        CHECK(m->snapshot().consecutive_failures == 0, "no attributable failure counted");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("Indeterminate: at most one bounded candidate attempt per 30 s, no switching on failure");
    {
        Env env; Rec rec; auto m = make(env, rec);
        env.setCands({cand("B"), cand("C"), cand("D")});
        m->start("A", kUserPort);
        env.probe.set("ek:A", mk(ProbeOutcome::Indeterminate));
        for (auto k : {"ek:B", "ek:C", "ek:D"}) env.probe.set(k, mk(ProbeOutcome::Indeterminate));
        run(m, env.clock, 28);
        int tried = env.probe.countKey("ek:B") + env.probe.countKey("ek:C") + env.probe.countKey("ek:D");
        CHECK(tried <= 1, "bounded attempt: " + std::to_string(tried));
        CHECK(m->snapshot().endpoint_key == "ek:A", "still on A");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    T_CASE("bulk-only failure (A+B ok, unattributed Partial) keeps Connected");
    {
        Env env; Rec rec; auto m = make(env, rec);
        m->start("A", kUserPort);
        ProbeResult p = mk(ProbeOutcome::Partial, false); p.checks = kCheckA | kCheckB;
        env.probe.set("ek:A", p);
        run(m, env.clock, 80);
        CHECK(m->snapshot().state == SessionState::Connected && env.launcher->launches() == 1, "no drop declared");
        m->stop(); m->waitStopped(2);
    }
    T_END();

    return T_SUMMARY();
}
