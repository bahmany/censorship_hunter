// Stage 4 B: ProxyServerManager (async lifecycle, port accounting, snapshot/event API, own watchdog
// thread) with fake launcher/probe. No network, no real engines.
#include <chrono>
#include <thread>

#include "network/continuous_validator.h"
#include "proxy/proxy_server_manager.h"
#include "runtime_fakes.h"
#include "test_support.h"

using namespace fakes;

namespace {
bool waitFor(const std::function<bool()>& pred, int ms = 5000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return pred();
}
ManagerDeps mdeps(Env& env, bool watchdog = false, bool real_clock = false) {
    ManagerDeps d;
    d.monitor = env.deps();
    d.monitor.executor = nullptr;   // manager creates a thread pool per session
    if (real_clock) { d.monitor.steady = nullptr; d.monitor.utc = nullptr; }
    d.port_free = [](int) { return true; };
    d.port_first = 39100; d.port_last = 39101;
    d.start_watchdog = watchdog;
    return d;
}
}  // namespace

int main() {
    std::cout << "test_proxy_manager" << std::endl;

    T_CASE("startProxy is asynchronous: returns at once while the engine launch blocks; not Running until Pass");
    {
        Env env;
        std::mutex gm; std::condition_variable gcv; bool gate = false;
        env.launcher->before_launch = [&] { std::unique_lock<std::mutex> lk(gm); gcv.wait(lk, [&] { return gate; }); };
        ProxyServerManager mgr(mdeps(env));
        auto t0 = std::chrono::steady_clock::now();
        int port = mgr.startProxy("A");
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(port == 39100 && ms < 200, "returns immediately with the reserved port (" + std::to_string((int)ms) + " ms)");
        CHECK(mgr.getStatus("A") == ProxyStatus::Starting && !mgr.isRunning("A"), "Starting, not Running");
        CHECK(mgr.getPort("A") == 39100, "port visible while starting");
        // GUI-thread style calls stay fast even though the worker is blocked in launch().
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; i++) { mgr.getInstances(); mgr.poll(); mgr.snapshots(); }
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 200, "queries do not wait on the launch (" + std::to_string((int)ms) + " ms)");
        { std::lock_guard<std::mutex> lk(gm); gate = true; } gcv.notify_all();
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "Running after launch + Pass");
        auto inst = mgr.getInstances();
        CHECK(inst.size() == 1 && inst[0].status == ProxyStatus::Running && inst[0].port == 39100, "legacy instance view");
        mgr.stopAll(); CHECK(mgr.waitAllStopped(3), "stopped");
    }
    T_END();

    T_CASE("traffic that fails keeps the legacy status off Running (listener + alive process is not enough)");
    {
        Env env; env.probe.set("ek:A", mk(ProbeOutcome::RemoteFailure, true));
        ProxyServerManager mgr(mdeps(env));
        mgr.startProxy("A");
        bool ever_running = false;
        for (int i = 0; i < 40; i++) { env.clock.advance(1); mgr.tickAll(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); ever_running |= mgr.getStatus("A") == ProxyStatus::Running; }
        CHECK(!ever_running, "never Running");
        CHECK(waitFor([&] { return mgr.getStatus("A") == ProxyStatus::Error; }), "Error (Unavailable) with reason");
        CHECK(!mgr.getError("A").empty(), "reason exposed");
        mgr.stopAll(); mgr.waitAllStopped(3);
    }
    T_END();

    T_CASE("failed sessions release their port; range exhaustion reports an error; stop frees ports");
    {
        Env env; env.build_fails = true;
        ProxyServerManager mgr(mdeps(env));
        for (int i = 0; i < 5; i++) {
            std::string u = "bad" + std::to_string(i);
            int p = mgr.startProxy(u);
            CHECK(p == 39100, "port 39100 reused after Error #" + std::to_string(i));
            CHECK(waitFor([&] { return mgr.getStatus(u) == ProxyStatus::Error; }), "Error");
            CHECK(mgr.getPort(u) == 0, "errored session shows no port");
        }
        env.build_fails = false;
        int a = mgr.startProxy("A"), b = mgr.startProxy("B"), c = mgr.startProxy("C");
        CHECK(a != 0 && b != 0 && a != b && c == 0, "third session finds no port");
        CHECK(mgr.getError("C").find("No free ports") != std::string::npos, "range exhaustion message");
        CHECK(mgr.stopProxy("A"), "stop A");
        CHECK(waitFor([&] { SessionSnapshot q; return mgr.snapshot("A", &q) && q.state == SessionState::Stopped; }), "A fully stopped (children reaped)");
        int c2 = mgr.startProxy("C");
        CHECK(c2 == a, "A port reusable once its teardown finished: a=" + std::to_string(a) + " b=" + std::to_string(b) + " c2=" + std::to_string(c2));
        CHECK(mgr.stopProxyByPort(b) && waitFor([&] { SessionSnapshot q; return mgr.snapshot("B", &q) && q.state == SessionState::Stopped; }), "stop by port");
        mgr.stopAll(); CHECK(mgr.waitAllStopped(3) && env.launcher->live == 0, "all engines reaped");
    }
    T_END();

    T_CASE("stop is asynchronous and cancels everything; restart of the same URI uses a fresh session");
    {
        Env env;
        ProxyServerManager mgr(mdeps(env));
        int p1 = mgr.startProxy("A");
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "running");
        SessionSnapshot s1; mgr.snapshot("A", &s1);
        auto t0 = std::chrono::steady_clock::now();
        CHECK(mgr.stopProxy("A"), "stop accepted");
        { double sms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); CHECK(sms < 200, "stop returns at once"); }
        CHECK(mgr.getStatus("A") != ProxyStatus::Running && !mgr.isRunning("A"), "no longer Running immediately");
        int p2 = mgr.startProxy("A");     // while the old session may still be tearing down
        CHECK(p2 != 0, "can start again");
        SessionSnapshot s2; mgr.snapshot("A", &s2);
        CHECK(s2.generation != 0, "new session");
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "new session Running");
        (void)p1; (void)s1;
        mgr.stopAll(); CHECK(mgr.waitAllStopped(3) && env.launcher->live == 0, "everything reaped");
    }
    T_END();

    T_CASE("snapshot + event API for the GUI; manual pin");
    {
        Env env;
        ProxyServerManager mgr(mdeps(env));
        std::mutex em; std::vector<std::pair<std::string, SessionEvent>> evs;
        mgr.setEventCallback([&](const std::string& u, const SessionEvent& e) { std::lock_guard<std::mutex> lk(em); evs.emplace_back(u, e); });
        mgr.startProxy("A");
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "running");
        SessionSnapshot s; CHECK(mgr.snapshot("A", &s), "snapshot");
        CHECK(s.state == SessionState::Connected && s.endpoint_key == "ek:A" && s.last_probe.valid && s.mode == SwitchMode::Auto, "snapshot content");
        CHECK(mgr.setPinned("A", true), "pin"); mgr.snapshot("A", &s); CHECK(s.mode == SwitchMode::Pinned, "pinned visible");
        CHECK(!mgr.setPinned("nope", true) && !mgr.snapshot("nope", &s), "unknown uri");
        CHECK(mgr.snapshots().size() == 1, "snapshots()");
        { std::lock_guard<std::mutex> lk(em);
          bool saw_connected = false, all_A = true;
          for (auto& [u, e] : evs) { all_A &= u == "A"; saw_connected |= e.kind == EventKind::StateChanged && e.snapshot.state == SessionState::Connected; }
          CHECK(saw_connected && all_A, "events carry the session URI and state"); }
        mgr.stopAll(); mgr.waitAllStopped(3);
        { std::lock_guard<std::mutex> lk(em); bool stopped = false; for (auto& [u, e] : evs) stopped |= e.kind == EventKind::Stopped; CHECK(stopped, "Stopped event delivered"); }
    }
    T_END();

    T_CASE("manager-owned watchdog thread: engine death is handled without any GUI/poll() call");
    {
        Env env; MonitorConfig cfg; cfg.process_check_s = 0.05; cfg.restart_backoff_s = {0.05}; cfg.restart_jitter = 0;
        ManagerDeps d = mdeps(env, /*watchdog=*/true, /*real_clock=*/true);
        ProxyServerManager mgr(d, cfg);
        mgr.startProxy("A");
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "running");
        env.launcher->last()->alive = false;     // the engine dies; nobody calls poll()/tickAll()
        CHECK(waitFor([&] { return env.launcher->launches() >= 2; }, 5000), "watchdog restarted the engine");
        CHECK(waitFor([&] { return mgr.isRunning("A"); }), "Connected again after fresh Pass");
        mgr.stopAll(); CHECK(mgr.waitAllStopped(3), "stopped");
    }
    T_END();

    T_CASE("destructor stops and reaps every session");
    {
        Env env;
        {
            ProxyServerManager mgr(mdeps(env));
            mgr.startProxy("A"); mgr.startProxy("B");
            CHECK(waitFor([&] { return mgr.isRunning("A") && mgr.isRunning("B"); }), "both running");
            CHECK(env.launcher->live == 2, "two engines");
        }
        CHECK(env.launcher->live == 0, "destructor reaped all engines");
    }
    T_END();

    T_CASE("probe results of the live session are recorded in the attached database (applyProbeResult)");
    {
        const std::string uri = "vless://11111111-2222-3333-4444-555555555555@203.0.113.9:443?security=tls&sni=a.example.com&type=tcp#a";
        network::ConfigDatabase db(100);
        db.addConfigs({uri}, "test");
        Env env;
        const std::string key = network::ConfigDatabase::keyFor(uri);
        ManagerDeps d = mdeps(env);
        d.monitor.key_for = nullptr; d.monitor.sink = nullptr;
        std::atomic<int> n{0};
        d.monitor.probe = [&](int, bool, const std::string& k, const std::string& e) {
            ProbeResult r = mk(ProbeOutcome::Pass); r.endpoint_key = k; r.engine = e;
            r.run_id = "t-" + std::to_string(n++); r.started_at = r.finished_at = hunter::systemClock()(); return r;
        };
        ProxyServerManager mgr(d);
        mgr.attachDatabase(&db);
        mgr.startProxy(uri);
        CHECK(waitFor([&] { return mgr.isRunning(uri); }), "running");
        CHECK(waitFor([&] { ConfigHealthRecord rec; return db.getRecord(uri, &rec) && rec.ev.state == HealthState::Healthy; }), "database sees the Pass for the live session");
        (void)key;
        mgr.stopAll(); mgr.waitAllStopped(3);
    }
    T_END();

    return T_SUMMARY();
}
