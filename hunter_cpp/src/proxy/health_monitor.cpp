#include "proxy/health_monitor.h"

#include <algorithm>
#include <chrono>

namespace hunter {
namespace proxy {

using network::LaunchRequest;
using network::LaunchResult;
using network::LaunchStatus;

const char* sessionStateName(SessionState s) {
    switch (s) {
        case SessionState::Stopped: return "Stopped";
        case SessionState::Starting: return "Starting";
        case SessionState::Connected: return "Connected";
        case SessionState::Degraded: return "Degraded";
        case SessionState::Switching: return "Switching";
        case SessionState::Restarting: return "Restarting";
        case SessionState::Paused: return "Paused";
        case SessionState::Unavailable: return "Unavailable";
        case SessionState::Failed: return "Failed";
        case SessionState::Stopping: return "Stopping";
    }
    return "?";
}

const char* eventKindName(EventKind k) {
    switch (k) {
        case EventKind::StateChanged: return "StateChanged";
        case EventKind::ProbeCompleted: return "ProbeCompleted";
        case EventKind::EngineExited: return "EngineExited";
        case EventKind::RestartScheduled: return "RestartScheduled";
        case EventKind::FailoverStarted: return "FailoverStarted";
        case EventKind::FailoverSucceeded: return "FailoverSucceeded";
        case EventKind::FailoverFailed: return "FailoverFailed";
        case EventKind::Stopped: return "Stopped";
    }
    return "?";
}

// ── ThreadPoolExecutor ──────────────────────────────────────────────────
ThreadPoolExecutor::ThreadPoolExecutor(size_t threads) : st_(std::make_shared<State>()) {
    for (size_t i = 0; i < std::max<size_t>(1, threads); i++) {
        th_.emplace_back([st = st_] {
            for (;;) {
                std::function<void()> fn;
                {
                    std::unique_lock<std::mutex> lk(st->mu);
                    st->cv.wait(lk, [&] { return st->stop || !st->q.empty(); });
                    if (st->q.empty()) return;   // stop && drained
                    fn = std::move(st->q.front());
                    st->q.pop_front();
                }
                try { fn(); } catch (...) {}
            }
        });
    }
}
ThreadPoolExecutor::~ThreadPoolExecutor() {
    { std::lock_guard<std::mutex> lk(st_->mu); st_->stop = true; }
    st_->cv.notify_all();
    for (auto& t : th_) {
        if (!t.joinable()) continue;
        if (t.get_id() == std::this_thread::get_id()) t.detach();   // last reference dropped by a worker
        else t.join();
    }
}
void ThreadPoolExecutor::post(std::function<void()> fn) {
    { std::lock_guard<std::mutex> lk(st_->mu); st_->q.push_back(std::move(fn)); }
    st_->cv.notify_one();
}

// ── Impl ────────────────────────────────────────────────────────────────
namespace {
enum class Job { None, Start, Probe, Restart, Failover };
}

struct HealthMonitor::Impl : std::enable_shared_from_this<HealthMonitor::Impl> {
    MonitorConfig cfg;
    MonitorDeps deps;

    mutable std::mutex mu;
    mutable std::condition_variable cv;
    std::recursive_mutex cb_mu;     // serialises delivery; callbacks may call back into the monitor
    EventCallback cb;
    std::vector<SessionEvent> pending;

    // session
    SessionState state = SessionState::Stopped;
    SwitchMode mode = SwitchMode::Auto;
    std::string uri, key, engine, reason, previous_key;
    int user_port = 0;
    uint64_t gen = 0;          // session generation
    uint64_t epoch = 0;        // user-port engine instance counter
    LaunchResult cur;          // engine on the user port (empty guard = none)
    bool provisional = false;
    bool outage = false;
    int fails = 0;             // consecutive attributable failures
    int engine_faults = 0;     // consecutive probe rounds with a dead/unreachable local port
    int indeterminate = 0;
    int failovers = 0;
    LastProbe last;
    double state_since_utc = 0, connected_since_utc = 0;
    double connected_since_steady = -1;

    // scheduling (steady seconds; <0 = none)
    double next_probe = -1, last_bulk_attempt = -1000000, restart_due = -1, unavailable_retry = -1;
    double last_indeterminate_attempt = -1000000;
    bool failover_wanted = false, failover_indeterminate = false;
    int restart_streak = 0;
    std::deque<double> restart_times, cand_times;
    std::map<std::string, double> cooldown;   // endpoint_key -> until (steady)

    // jobs
    Job busy = Job::None;
    int inflight = 0;
    bool teardown_done = true;

    Impl(MonitorConfig c, MonitorDeps d) : cfg(std::move(c)), deps(std::move(d)) {
        if (!deps.steady) deps.steady = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
        if (!deps.utc) deps.utc = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
        if (!deps.rand01) deps.rand01 = [] { return 0.5; };
        if (!deps.executor) deps.executor = std::make_shared<InlineExecutor>();
        if (!deps.key_for) deps.key_for = [](const std::string& u) { return u; };
    }

    double jitter(double base, double frac) { return base * (1.0 + frac * (2.0 * deps.rand01() - 1.0)); }

    // ── snapshots / events (callers hold mu) ──
    SessionSnapshot snapLocked() const {
        SessionSnapshot s;
        s.state = state; s.mode = mode; s.uri = uri; s.endpoint_key = key; s.engine = engine;
        s.user_port = user_port; s.generation = gen; s.provisional = provisional;
        s.local_outage = outage; s.consecutive_failures = fails; s.failovers = failovers;
        s.previous_key = previous_key; s.last_probe = last; s.reason = reason;
        s.state_since_utc = state_since_utc;
        s.connected_since_utc = state == SessionState::Connected ? connected_since_utc : 0.0;
        const double now = deps.steady();
        int rs = 0; for (double t : restart_times) if (now - t < cfg.restart_window_s) rs++;
        s.restarts_in_window = rs;
        int cs = 0; for (double t : cand_times) if (now - t < cfg.candidate_window_s) cs++;
        s.candidates_tried_in_window = cs;
        if (cur.guard && deps.pid_of) s.engine_pid = deps.pid_of(cur.guard);
        return s;
    }
    void emitLocked(EventKind k, std::string detail = "") {
        SessionEvent e; e.kind = k; e.detail = std::move(detail); e.snapshot = snapLocked();
        pending.push_back(std::move(e));
    }
    void setStateLocked(SessionState s, const std::string& why) {
        if (state == s && reason == why) return;
        const bool changed = state != s;
        state = s; reason = why;
        if (changed) {
            state_since_utc = deps.utc();
            if (s == SessionState::Connected) { connected_since_utc = state_since_utc; connected_since_steady = deps.steady(); }
            else connected_since_steady = -1;
        }
        emitLocked(EventKind::StateChanged, why);
    }
    void deliver() {
        std::lock_guard<std::recursive_mutex> cl(cb_mu);
        for (;;) {
            std::vector<SessionEvent> evs;
            EventCallback c;
            { std::lock_guard<std::mutex> lk(mu); evs.swap(pending); c = cb; }
            if (evs.empty()) return;
            if (!c) continue;
            for (auto& e : evs) { try { c(e); } catch (...) {} }
        }
    }

    // ── helpers ──
    bool aliveLocked() { return cur.guard && (!cur.alive || cur.alive()); }
    bool stale(uint64_t g) const { return g != gen || state == SessionState::Stopping || state == SessionState::Stopped; }

    void maybeFinishStopLocked() {
        if (state == SessionState::Stopping && teardown_done && inflight == 0) {
            state = SessionState::Stopped; reason = "stopped"; state_since_utc = deps.utc();
            connected_since_steady = -1;
            emitLocked(EventKind::Stopped, "stopped");
            cv.notify_all();
        }
    }

    // RAII: marks a job finished, releases the busy slot and (maybe) completes a pending Stop.
    struct JobScope {
        Impl& d; Job kind;
        JobScope(Impl& i, Job k) : d(i), kind(k) {}
        ~JobScope() {
            { std::lock_guard<std::mutex> lk(d.mu); d.busy = Job::None; d.inflight--; d.maybeFinishStopLocked(); d.cv.notify_all(); }
            d.deliver();
        }
    };
    void postJob(Job kind, std::function<void()> fn) {
        // caller already incremented inflight and set busy; the job keeps Impl alive until it ends
        auto self = shared_from_this();
        deps.executor->post([self, kind, fn = std::move(fn)]() {
            JobScope scope(*self, kind);
            try { fn(); } catch (...) {}
        });
    }

    void applySink(const ProbeResult& r) { if (deps.sink) { try { deps.sink(r); } catch (...) {} } }

    ProbeResult runProbe(int port, bool bulk, const std::string& k, const std::string& eng) {
        ProbeResult r;
        if (deps.probe) r = deps.probe(port, bulk, k, eng);
        else { r.outcome = ProbeOutcome::EngineError; }
        r.port = port;
        return r;
    }

    bool offline() {
        if (!deps.baseline) return false;
        try { return deps.baseline() == network::BaselineState::Offline; } catch (...) { return false; }
    }

    void pruneLocked(double now) {
        while (!restart_times.empty() && now - restart_times.front() >= cfg.restart_window_s) restart_times.pop_front();
        while (!cand_times.empty() && now - cand_times.front() >= cfg.candidate_window_s) cand_times.pop_front();
        for (auto it = cooldown.begin(); it != cooldown.end();) it = it->second <= now ? cooldown.erase(it) : std::next(it);
    }

    double nextProbeDelayLocked() { return jitter(cfg.probe_interval_s, cfg.probe_jitter); }

    // Launch an engine for `u` on `port`; no locks held by the caller.
    LaunchResult launchEngine(const std::string& u, int port, std::string* engine_out, std::string* err, bool* fatal) {
        LaunchResult none;
        BuiltConfig bc;
        try { bc = deps.build_config ? deps.build_config(u, port) : BuiltConfig(); } catch (...) {}
        if (!bc.ok) { *err = bc.error.empty() ? "cannot build engine config" : bc.error; *fatal = true; return none; }
        if (deps.launcher && !deps.launcher->available(bc.engine)) {
            *err = bc.engine + " binary not found"; *fatal = true; return none;
        }
        LaunchRequest rq; rq.engine = bc.engine; rq.config_text = bc.config_text; rq.ports = {port};
        rq.startup_timeout_ms = cfg.launch_timeout_ms;
        LaunchResult lr = deps.launcher ? deps.launcher->launch(rq) : none;
        *engine_out = bc.engine;
        if (lr.status == LaunchStatus::Ok) return lr;
        *err = lr.detail.empty() ? "engine launch failed" : lr.detail;
        *fatal = lr.status == LaunchStatus::BinaryMissing || lr.status == LaunchStatus::Error ||
                 lr.status == LaunchStatus::BindConflict;
        if (lr.status == LaunchStatus::BindConflict) *err = "port " + std::to_string(port) + " is in use";
        return none;
    }

    // ── restart scheduling (caller holds mu) ──
    void scheduleRestartLocked(const std::string& why) {
        const double now = deps.steady();
        pruneLocked(now);
        if ((int)restart_times.size() >= cfg.max_restarts) {
            // Budget exhausted: Auto fails over to another candidate, Pinned waits for the window.
            if (mode == SwitchMode::Auto) {
                failover_wanted = true; failover_indeterminate = false;
                setStateLocked(SessionState::Degraded, "engine keeps failing (" + why + "); failing over");
            } else {
                unavailable_retry = restart_times.front() + cfg.restart_window_s;
                restart_due = -1;
                setStateLocked(SessionState::Unavailable, "engine keeps failing (" + why + "); pinned, restart budget exhausted");
            }
            return;
        }
        const size_t idx = std::min<size_t>((size_t)restart_streak, cfg.restart_backoff_s.size() - 1);
        const double delay = jitter(cfg.restart_backoff_s[idx], cfg.restart_jitter);
        restart_streak++;
        restart_times.push_back(now);
        restart_due = now + delay;
        next_probe = -1;
        setStateLocked(SessionState::Restarting, why);
        emitLocked(EventKind::RestartScheduled, "restart in " + std::to_string(delay) + "s");
    }

    // ── jobs ──
    void onPassLocked(const ProbeResult& r, bool bulk_asked) {
        const double now = deps.steady();
        fails = 0; engine_faults = 0; indeterminate = 0; outage = false;
        failover_wanted = false; unavailable_retry = -1; restart_due = -1;
        next_probe = now + nextProbeDelayLocked();
        (void)bulk_asked; (void)r;
        setStateLocked(SessionState::Connected, "");
    }

    void noteProbeLocked(const ProbeResult& r) {
        last.valid = true; last.outcome = r.outcome; last.attributable = r.attributable;
        last.bulk_passed = r.bulk_passed; last.latency_ms = r.latency_ms;
        last.at_utc = deps.utc(); last.exit_country = r.exit_country;
        emitLocked(EventKind::ProbeCompleted, outcomeName(r.outcome));
    }

    // Apply a classified probe of the USER-port engine. Returns false if discarded as stale.
    bool handleProbe(uint64_t g, uint64_t ep, const ProbeResult& r, bool bulk_asked) {
        std::lock_guard<std::mutex> lk(mu);
        if (stale(g) || ep != epoch) return false;
        const double now = deps.steady();
        noteProbeLocked(r);
        if (bulk_asked) last_bulk_attempt = now;
        switch (r.outcome) {
            case ProbeOutcome::Pass:
                onPassLocked(r, bulk_asked);
                break;
            case ProbeOutcome::LocalNetworkDown:
                outage = true;
                next_probe = now + cfg.outage_poll_s;
                setStateLocked(SessionState::Paused, "local network down: switching and restarts paused");
                break;
            case ProbeOutcome::EngineError:
            case ProbeOutcome::BindConflict:
            case ProbeOutcome::Cancelled: {
                // Local engine problem (listener refused / died): never an upstream failure.
                engine_faults++;
                next_probe = now + cfg.confirm_interval_s;
                if (engine_faults >= 2 || !aliveLocked()) {
                    killCurrentLocked();
                    scheduleRestartLocked("engine unreachable");
                } else {
                    setStateLocked(SessionState::Degraded, "engine port not answering");
                }
                break;
            }
            case ProbeOutcome::Indeterminate:
            case ProbeOutcome::Unsupported:
            case ProbeOutcome::InvalidConfig: {
                outage = false;
                indeterminate++;
                next_probe = now + cfg.confirm_interval_s;
                setStateLocked(SessionState::Degraded, "connectivity unconfirmed (no penalty)");
                if (mode == SwitchMode::Auto && indeterminate >= 2 &&
                    now - last_indeterminate_attempt >= cfg.indeterminate_wait_s) {
                    // One bounded fresh-candidate attempt, then wait indeterminate_wait_s.
                    last_indeterminate_attempt = now;
                    failover_wanted = true; failover_indeterminate = true;
                }
                break;
            }
            case ProbeOutcome::RemoteFailure:
            case ProbeOutcome::Partial: {
                const bool ab_ok = (r.checks & kCheckA) && (r.checks & kCheckB);
                if (r.outcome == ProbeOutcome::Partial && !r.attributable && ab_ok) {
                    // Small checks fine, only the 64 KiB transfer failed: traffic works, not a drop.
                    next_probe = now + nextProbeDelayLocked();
                    if (state != SessionState::Connected && state != SessionState::Unavailable)
                        setStateLocked(SessionState::Connected, "");
                    break;
                }
                outage = false;
                next_probe = now + cfg.confirm_interval_s;
                if (!r.attributable) {
                    setStateLocked(SessionState::Degraded, "probe failed, not attributable to the server");
                    break;
                }
                fails++;
                if (fails >= cfg.fail_threshold) {
                    if (mode == SwitchMode::Auto && state == SessionState::Unavailable && unavailable_retry >= 0 && now < unavailable_retry) {
                        // A failover round just found nothing: keep probing, retry switching on schedule.
                        next_probe = now + nextProbeDelayLocked();
                    } else if (mode == SwitchMode::Auto) {
                        failover_wanted = true; failover_indeterminate = false;
                        setStateLocked(SessionState::Degraded, "upstream drop confirmed; failing over");
                    } else {
                        next_probe = now + nextProbeDelayLocked();
                        setStateLocked(SessionState::Unavailable, "upstream not reachable (pinned: not switching)");
                    }
                } else {
                    setStateLocked(SessionState::Degraded, "tunnel check failed; confirming");
                }
                break;
            }
        }
        return true;
    }

    // Take the user-port engine out for destruction elsewhere (caller destroys outside the lock).
    void killCurrentLocked() { graveyard.push_back(std::move(cur)); cur = LaunchResult(); epoch++; }
    std::vector<LaunchResult> graveyard;   // engines awaiting blocking destruction (outside mu)
    void reapGraveyard() {
        std::vector<LaunchResult> g;
        { std::lock_guard<std::mutex> lk(mu); g.swap(graveyard); }
        g.clear();   // guard dtors terminate + reap the child
    }

    void probeCurrent(uint64_t g, uint64_t ep, bool bulk) {
        std::string k, eng; int port;
        { std::lock_guard<std::mutex> lk(mu); if (stale(g) || ep != epoch) return; k = key; eng = engine; port = user_port; }
        ProbeResult r = runProbe(port, bulk, k, eng);
        { std::lock_guard<std::mutex> lk(mu); if (stale(g) || ep != epoch) return; }
        if (handleProbe(g, ep, r, bulk)) applySink(r);
    }

    void startJob(uint64_t g) { launchAndProbe(g, /*initial=*/true); }
    void restartJob(uint64_t g) { launchAndProbe(g, /*initial=*/false); }

    void launchAndProbe(uint64_t g, bool initial) {
        std::string u; int port;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return;
            restart_due = -1;
            u = uri; port = user_port;
        }
        if (!initial && offline()) {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return;
            outage = true; restart_due = deps.steady() + cfg.outage_poll_s;
            setStateLocked(SessionState::Paused, "local network down: restart paused");
            return;
        }
        reapGraveyard();
        std::string eng, err; bool fatal = false;
        LaunchResult lr = launchEngine(u, port, &eng, &err, &fatal);
        uint64_t ep = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) { graveyard.push_back(std::move(lr)); }   // cancelled while launching
            else if (!lr.guard) {
                if (fatal) {
                    setStateLocked(SessionState::Failed, err);
                    restart_due = -1; next_probe = -1;
                } else {
                    scheduleRestartLocked("engine failed to start: " + err);
                }
            } else {
                cur = std::move(lr); epoch++; ep = epoch; engine = eng;
                engine_faults = 0;
                setStateLocked(initial ? SessionState::Starting : SessionState::Restarting,
                               initial ? "verifying traffic through the proxy" : "engine restarted; verifying traffic");
                next_probe = -1;
            }
        }
        reapGraveyard();
        if (ep) probeCurrent(g, ep, /*bulk=*/true);
    }

    void probeJob(uint64_t g, uint64_t ep, bool bulk) { probeCurrent(g, ep, bulk); }

    // ── failover (M3): verify on a lease port, then restart the engine on the user port ──
    void failoverJob(uint64_t g, bool indeterminate_attempt) {
        std::string from_key;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return;
            failover_wanted = false;
        }
        if (offline()) {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return;
            outage = true; next_probe = deps.steady() + cfg.outage_poll_s;
            failover_wanted = true;
            setStateLocked(SessionState::Paused, "local network down: failover paused");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return;
            from_key = key;
            if (!from_key.empty() && !indeterminate_attempt) cooldown[from_key] = deps.steady() + cfg.candidate_cooldown_s;
            setStateLocked(SessionState::Switching, "looking for a verified candidate");
            emitLocked(EventKind::FailoverStarted, from_key);
        }
        deliver();

        std::vector<Candidate> cands;
        try { if (deps.candidates) cands = deps.candidates(); } catch (...) {}
        std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.stable && !b.stable; });

        int budget_cap = indeterminate_attempt ? 1 : cfg.max_candidates_per_window;
        int tried_here = 0;
        for (const Candidate& c : cands) {
            {
                std::lock_guard<std::mutex> lk(mu);
                if (stale(g)) return;
                const double now = deps.steady();
                pruneLocked(now);
                if (c.endpoint_key.empty() || c.endpoint_key == from_key || c.endpoint_key == key) continue;
                if (cooldown.count(c.endpoint_key)) continue;
                const int left = cfg.max_candidates_per_window - (int)cand_times.size();
                if (left <= 0 || tried_here >= budget_cap) break;
                cand_times.push_back(now);
                tried_here++;
                reason = "verifying candidate";
            }
            if (verifyAndSwitch(g, c)) return;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (stale(g)) return;
        const double now = deps.steady();
        unavailable_retry = now + (indeterminate_attempt ? cfg.indeterminate_wait_s : cfg.unavailable_retry_s);
        if (aliveLocked()) next_probe = now + (indeterminate_attempt ? cfg.confirm_interval_s : nextProbeDelayLocked());
        if (indeterminate_attempt && aliveLocked()) {
            setStateLocked(SessionState::Degraded, "connectivity unconfirmed; no candidate verified");
            next_probe = now + cfg.confirm_interval_s;
        } else {
            setStateLocked(SessionState::Unavailable, "no verified candidate available");
        }
        emitLocked(EventKind::FailoverFailed, cands.empty() ? "no candidates" : "no candidate passed verification");
    }

    void coolDown(const std::string& k) {
        std::lock_guard<std::mutex> lk(mu);
        cooldown[k] = deps.steady() + cfg.candidate_cooldown_s;
    }

    bool verifyAndSwitch(uint64_t g, const Candidate& c) {
        // 1) fresh FULL probe on a lease port (never on the user port).
        {
            network::PortLease lease = deps.leases ? deps.leases->acquire() : network::PortLease();
            if (!lease.valid()) return false;
            std::string eng;
            LaunchResult lr;
            for (int attempt = 0; attempt < 3; attempt++) {
                BuiltConfig bc;
                try { bc = deps.build_config(c.uri, lease.port()); } catch (...) {}
                if (!bc.ok || !deps.launcher || !deps.launcher->available(bc.engine)) break;
                lease.releaseSocket();
                LaunchRequest rq; rq.engine = bc.engine; rq.config_text = bc.config_text; rq.ports = {lease.port()};
                rq.startup_timeout_ms = cfg.launch_timeout_ms;
                lr = deps.launcher->launch(rq);
                eng = bc.engine;
                if (lr.status == LaunchStatus::BindConflict) { if (!lease.reacquire()) break; continue; }   // zero penalty
                break;
            }
            if (!lr.guard) { coolDown(c.endpoint_key); return false; }
            { std::lock_guard<std::mutex> lk(mu); if (stale(g)) return true; }
            ProbeResult r = runProbe(lease.port(), /*bulk=*/true, c.endpoint_key, eng);
            { std::lock_guard<std::mutex> lk(mu); if (stale(g)) return true; }
            applySink(r);
            if (r.outcome != ProbeOutcome::Pass) { coolDown(c.endpoint_key); return false; }
            // lr + lease released here (engine reaped, port freed) before the user-port restart
        }
        // 2) swap the engine on the user port.
        std::string u; int port;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return true;
            killCurrentLocked();
            setStateLocked(SessionState::Switching, "restarting engine on the user port");
            u = c.uri; port = user_port;
        }
        reapGraveyard();
        std::string eng, err; bool fatal = false;
        LaunchResult lr = launchEngine(u, port, &eng, &err, &fatal);
        uint64_t ep = 0;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) { graveyard.push_back(std::move(lr)); return true; }
            if (!lr.guard) { cooldown[c.endpoint_key] = deps.steady() + cfg.candidate_cooldown_s; return false; }
            cur = std::move(lr); epoch++; ep = epoch; engine = eng;
            previous_key = key; key = c.endpoint_key; uri = c.uri; provisional = !c.stable;
        }
        reapGraveyard();
        // 3) Connected only after a Pass on the USER port.
        ProbeResult r2 = runProbe(port, true, c.endpoint_key, eng);
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stale(g)) return true;
            if (ep != epoch) return true;   // engine died meanwhile; the watchdog schedules a restart
            noteProbeLocked(r2);
            last_bulk_attempt = deps.steady();
            if (r2.outcome == ProbeOutcome::Pass) {
                failovers++;
                restart_streak = 0;
                onPassLocked(r2, true);
                emitLocked(EventKind::FailoverSucceeded, c.endpoint_key);
                ok = true;
            } else {
                killCurrentLocked();
                cooldown[c.endpoint_key] = deps.steady() + cfg.candidate_cooldown_s;
            }
        }
        applySink(r2);
        return ok;
    }

    // ── watchdog ──
    void step() {
        Job kind = Job::None;
        uint64_t g = 0, ep = 0;
        bool bulk = false, ind = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (state == SessionState::Stopped || state == SessionState::Stopping || state == SessionState::Failed) return;
            const double now = deps.steady();
            pruneLocked(now);

            // 1 s process check (zombie-safe: alive() reaps through waitpid)
            if (cur.guard && !aliveLocked()) {
                emitLocked(EventKind::EngineExited, "engine process exited");
                killCurrentLocked();
                if (busy == Job::None || busy == Job::Probe) {
                    scheduleRestartLocked("engine process exited");
                }   // else: the running start/restart/failover job owns the next step
            }
            if (state == SessionState::Connected && connected_since_steady >= 0 &&
                now - connected_since_steady >= cfg.stable_connected_reset_s) restart_streak = 0;

            if (busy == Job::None) {
                const bool have_engine = cur.guard != nullptr;
                if (restart_due >= 0 && now >= restart_due) {
                    kind = Job::Restart;
                } else if (failover_wanted && mode == SwitchMode::Auto && !outage) {
                    kind = Job::Failover; ind = failover_indeterminate;
                } else if (failover_wanted && outage && now >= next_probe && have_engine) {
                    kind = Job::Probe;   // look for the outage to end first
                } else if (!have_engine && unavailable_retry >= 0 && now >= unavailable_retry &&
                           (state == SessionState::Unavailable)) {
                    if (mode == SwitchMode::Auto) { kind = Job::Failover; ind = false; failover_wanted = false; }
                    else { unavailable_retry = -1; scheduleRestartLocked("retrying pinned config"); }
                } else if (have_engine && unavailable_retry >= 0 && now >= unavailable_retry &&
                           state == SessionState::Unavailable && mode == SwitchMode::Auto) {
                    kind = Job::Failover; ind = false;
                } else if (have_engine && next_probe >= 0 && now >= next_probe &&
                           (state == SessionState::Connected || state == SessionState::Degraded ||
                            state == SessionState::Paused || state == SessionState::Unavailable ||
                            state == SessionState::Starting || state == SessionState::Restarting)) {
                    kind = Job::Probe;
                    bulk = now - last_bulk_attempt >= cfg.bulk_interval_s;
                }
                if (kind != Job::None) {
                    if (kind == Job::Failover) unavailable_retry = -1;
                    busy = kind; inflight++; g = gen; ep = epoch;
                    if (kind == Job::Probe) next_probe = -1;
                }
            }
        }
        reapGraveyard();
        deliver();
        if (kind == Job::None) return;
        switch (kind) {
            case Job::Restart: postJob(kind, [this, g] { restartJob(g); }); break;
            case Job::Failover: postJob(kind, [this, g, ind] { failoverJob(g, ind); }); break;
            case Job::Probe: postJob(kind, [this, g, ep, bulk] { probeJob(g, ep, bulk); }); break;
            default: break;
        }
    }

    void doStart(const std::string& u, int port, SwitchMode m) {
        uint64_t g;
        {
            std::lock_guard<std::mutex> lk(mu);
            gen++; g = gen;
            graveyard.push_back(std::move(cur)); cur = LaunchResult(); epoch++;
            uri = u; key = deps.key_for(u); user_port = port; mode = m; engine.clear();
            provisional = false; outage = false; fails = 0; engine_faults = 0; indeterminate = 0; failovers = 0;
            previous_key.clear(); last = LastProbe(); restart_streak = 0;
            restart_times.clear(); cand_times.clear(); cooldown.clear();
            next_probe = restart_due = unavailable_retry = -1; failover_wanted = false;
            last_bulk_attempt = -1000000; teardown_done = true;
            setStateLocked(SessionState::Starting, "launching engine");
            busy = Job::Start; inflight++;
        }
        reapGraveyard();
        deliver();
        postJob(Job::Start, [this, g] { startJob(g); });
    }

    void doStop() {
        std::shared_ptr<std::vector<LaunchResult>> vic;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (state == SessionState::Stopped || state == SessionState::Stopping) return;
            gen++;   // every in-flight callback becomes stale
            epoch++;
            vic = std::make_shared<std::vector<LaunchResult>>();
            vic->push_back(std::move(cur)); cur = LaunchResult();
            next_probe = restart_due = unavailable_retry = -1; failover_wanted = false;
            teardown_done = false;
            inflight++;   // the teardown counts as a job: Stopped only after it finished
            setStateLocked(SessionState::Stopping, "stop requested");
        }
        deliver();
        auto self = shared_from_this();
        deps.executor->post([self, vic]() {
            vic->clear();   // guard dtors: terminate + reap children, remove config files
            self->reapGraveyard();
            {
                std::lock_guard<std::mutex> lk(self->mu);
                self->teardown_done = true;
                self->inflight--;
                self->maybeFinishStopLocked();
                self->cv.notify_all();
            }
            self->deliver();
        });
    }
};

// ── facade ──────────────────────────────────────────────────────────────
std::shared_ptr<HealthMonitor> HealthMonitor::create(MonitorConfig cfg, MonitorDeps deps) {
    return std::shared_ptr<HealthMonitor>(new HealthMonitor(std::move(cfg), std::move(deps)));
}
HealthMonitor::HealthMonitor(MonitorConfig cfg, MonitorDeps deps) : d_(std::make_shared<Impl>(std::move(cfg), std::move(deps))) {}
HealthMonitor::~HealthMonitor() {
    // Owner contract: stop() + waitStopped() first; make the destructor safe regardless.
    d_->doStop();
    waitStopped(10.0);   // jobs hold their own reference to Impl, so late workers stay safe
}
void HealthMonitor::start(const std::string& uri, int user_port, SwitchMode mode) { d_->doStart(uri, user_port, mode); }
void HealthMonitor::stop() { d_->doStop(); }
void HealthMonitor::step() { d_->step(); }
SessionSnapshot HealthMonitor::snapshot() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->snapLocked(); }
void HealthMonitor::setEventCallback(EventCallback cb) { std::lock_guard<std::mutex> lk(d_->mu); d_->cb = std::move(cb); }
bool HealthMonitor::active() const {
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->state != SessionState::Stopped && d_->state != SessionState::Failed;
}
void HealthMonitor::setMode(SwitchMode m) {
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->mode = m;
    if (m == SwitchMode::Pinned) { d_->failover_wanted = false; d_->unavailable_retry = -1; }
}
bool HealthMonitor::waitStopped(double timeout_s) const {
    bool ok;
    {
        std::unique_lock<std::mutex> lk(d_->mu);
        ok = d_->cv.wait_for(lk, std::chrono::duration<double>(timeout_s),
                             [&] { return d_->state == SessionState::Stopped || (d_->state == SessionState::Failed && d_->inflight == 0); });
    }
    // The Stopped event is delivered by the worker right after it flips the state; make sure it has
    // been delivered (or deliver it here) before reporting completion.
    if (ok) d_->deliver();
    return ok;
}

}  // namespace proxy
}  // namespace hunter
