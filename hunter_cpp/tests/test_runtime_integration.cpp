// Opt-in (cmake -DHUNTER_INTEGRATION_TESTS=ON, ctest -L integration): REAL xray end to end, loopback only.
//   local HTTP server  <-  upstream xray (vless server, freedom)  <-  user-port xray (ProxyServerManager)
// The upstream is killed mid-session while the user-port engine process stays alive: the watchdog must
// detect the drop through real traffic, verify the standby on a lease port and restart the engine on
// the user port. Exit 77 (skipped) when HUNTER_XRAY_PATH is not set.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/utils.h"
#include "network/port_lease.h"
#include "network/traffic_probe.h"
#include "proxy/process_runner.h"
#include "network/uri_parser.h"
#include "proxy/proxy_server_manager.h"
#include "proxy/xray_manager.h"
#include "test_support.h"

using namespace hunter;
using namespace hunter::proxy;

static bool waitFor(const std::function<bool()>& pred, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    return pred();
}

int main() {
    const char* xp = std::getenv("HUNTER_XRAY_PATH");
    if (!xp || access(xp, X_OK) != 0) { std::cout << "SKIP: set HUNTER_XRAY_PATH" << std::endl; return 77; }
    signal(SIGPIPE, SIG_IGN);

    // ── local target: 204 / cloudflare-style trace / 64 KiB bulk ──
    int hs = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(hs, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); bind(hs, (sockaddr*)&a, sizeof a); listen(hs, 32);
    socklen_t l = sizeof a; getsockname(hs, (sockaddr*)&a, &l); const int hp = ntohs(a.sin_port);
    std::atomic<bool> stop{false};
    std::thread srv([&] {
        while (!stop) {
            pollfd pf{hs, POLLIN, 0}; if (poll(&pf, 1, 100) <= 0) continue;
            int c = accept(hs, nullptr, nullptr); if (c < 0) continue;
            std::thread([c] {
                char b[2048]; ssize_t n = recv(c, b, sizeof b - 1, 0); if (n <= 0) { close(c); return; } b[n] = 0;
                std::string req(b), resp;
                if (req.find("GET /generate_204") == 0) resp = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
                else if (req.find("GET /cdn-cgi/trace") == 0) {
                    std::string body = "fl=1f1\nh=probe.test\nip=8.8.4.4\nts=1700000000.123\nvisit_scheme=http\nuag=test\ncolo=XXX\nsliver=none\nhttp=http/1.1\nloc=DE\ntls=off\nsni=off\nwarp=off\ngateway=off\nrbi=off\nkex=none\n";
                    resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                } else if (req.find("GET /bulk") == 0) {
                    std::string body(65536, 'x');
                    resp = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 65536\r\nConnection: close\r\n\r\n" + body;
                } else resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(c, resp.data(), resp.size(), MSG_NOSIGNAL); shutdown(c, SHUT_WR);
                char sink[256]; while (recv(c, sink, sizeof sink, 0) > 0) {}
                close(c);
            }).detach();
        }
    });

    // ── two upstream xray servers (vless, no TLS) ──
    auto reg = network::PortLeaseRegistry::create();
    ManagedEngineLauncher up_launcher(xp, "", "");
    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    auto start_upstream = [&](int* port_out) {
        auto lease = reg->acquire(); int p = lease.port(); lease.releaseSocket();
        network::LaunchRequest rq; rq.engine = "xray"; rq.ports = {p}; rq.startup_timeout_ms = 15000;
        rq.config_text = "{\"log\":{\"loglevel\":\"warning\"},\"inbounds\":[{\"listen\":\"127.0.0.1\",\"port\":" + std::to_string(p) +
            ",\"protocol\":\"vless\",\"settings\":{\"clients\":[{\"id\":\"" + uuid + "\"}],\"decryption\":\"none\"}}],\"outbounds\":[{\"protocol\":\"freedom\"}]}";
        *port_out = p;
        return up_launcher.launch(rq);
    };
    int p1 = 0, p2 = 0;
    auto up1 = start_upstream(&p1); auto up2 = start_upstream(&p2);
    T_CASE("setup: two real upstream xray servers");
    CHECK(up1.status == network::LaunchStatus::Ok && up2.status == network::LaunchStatus::Ok, "upstreams up: " + up1.detail + up2.detail);
    T_END();
    if (up1.status != network::LaunchStatus::Ok || up2.status != network::LaunchStatus::Ok) { stop = true; srv.join(); return T_SUMMARY(); }
    auto uriFor = [&](int p, const char* tag) { return "vless://" + uuid + "@127.0.0.1:" + std::to_string(p) + "?encryption=none&security=none&type=tcp#" + tag; };
    const std::string u1 = uriFor(p1, "up1"), u2 = uriFor(p2, "up2");

    // ── probe: production TrafficProbe against the local target (real SOCKS5 through the engine) ──
    network::ProbeConfig pc;
    pc.check_a_url = "http://127.0.0.1:" + std::to_string(hp) + "/generate_204";
    pc.check_b_url = "http://127.0.0.1:" + std::to_string(hp) + "/cdn-cgi/trace";
    pc.check_b_host = "probe.test";
    pc.bulk_url = "http://127.0.0.1:" + std::to_string(hp) + "/bulk";
    pc.connect_timeout_ms = 1500; pc.overall_timeout_ms = 3000; pc.bulk_timeout_ms = 4000;
    auto tp = std::make_shared<network::TrafficProbe>(network::makeCurlTransport(), pc);

    ManagerDeps d;
    d.start_watchdog = true;
    d.port_first = 39300; d.port_last = 39309;
    d.monitor.probe = [tp](int port, bool bulk, const std::string& key, const std::string& engine) {
        network::RawProbe raw = tp->run(port, bulk);
        const bool full = raw.bothPass() && !(raw.bulk_run && raw.bulk.status != network::CheckStatus::Pass);
        ProbeResult r;
        r.endpoint_key = key; r.engine = engine; r.port = port;
        r.outcome = full ? ProbeOutcome::Pass : (raw.engine_unreachable ? ProbeOutcome::EngineError : ProbeOutcome::RemoteFailure);
        r.attributable = !full && !raw.engine_unreachable;    // loopback test: no local outage possible
        r.checks = (raw.a.status == network::CheckStatus::Pass ? kCheckA : 0) | (raw.b.status == network::CheckStatus::Pass ? kCheckB : 0);
        r.latency_ms = raw.latency_ms; r.bulk_passed = raw.bulk_run && raw.bulk.status == network::CheckStatus::Pass;
        return r;
    };
    // Production config generator, minus the (correct) "never reach loopback/private ranges" rule,
    // because the local test target lives on loopback.
    d.monitor.build_config = [](const std::string& uri, int port) {
        BuiltConfig b;
        auto parsed = network::UriParser::parse(uri);
        if (!parsed || !parsed->isValid()) { b.error = "bad uri"; return b; }
        std::string cfg = XRayManager::generateTestConfig(*parsed, port);
        size_t from = cfg.find("{\"type\":\"field\",\"inboundTag\":[\"test-in\"],\"ip\":");
        const std::string tail = "\"outboundTag\":\"blackhole\"},";
        size_t to = from == std::string::npos ? from : cfg.find(tail, from);
        if (to != std::string::npos) cfg.erase(from, to + tail.size() - from);
        b.ok = !cfg.empty(); b.engine = "xray"; b.config_text = cfg;
        return b;
    };
    d.monitor.baseline = []() { return network::BaselineState::Online; };
    d.monitor.key_for = [](const std::string& u) { return u; };
    MonitorConfig cfg; cfg.probe_interval_s = 1.0; cfg.confirm_interval_s = 0.5; cfg.restart_backoff_s = {0.2, 0.4, 0.8};
    cfg.unavailable_retry_s = 2.0; cfg.process_check_s = 0.2; cfg.launch_timeout_ms = 15000;

    setenv("HUNTER_XRAY_PATH", xp, 1);
    {
        ProxyServerManager mgr(d, cfg);
        mgr.setCandidateProvider([&] { std::vector<Candidate> c; c.push_back({u2, u2, true, 90}); return c; });
        std::vector<std::string> states; std::mutex em;
        mgr.setEventCallback([&](const std::string&, const SessionEvent& e) {
            if (e.kind == EventKind::StateChanged) { std::lock_guard<std::mutex> lk(em); states.push_back(sessionStateName(e.snapshot.state)); } });

        T_CASE("real xray: Connected only after real traffic passes through the user port");
        int port = mgr.startProxy(u1);
        CHECK(port == 39300, "user port reserved");
        CHECK(waitFor([&] { return mgr.isRunning(u1); }, 30000), "Connected through xray -> upstream1 -> local server");
        SessionSnapshot s; mgr.snapshot(u1, &s);
        if (s.state != SessionState::Connected) std::cout << "\n    state=" << sessionStateName(s.state) << " reason=" << s.reason << " last=" << outcomeName(s.last_probe.outcome) << std::endl;
        CHECK(s.engine == "xray" && s.engine_pid > 0 && s.last_probe.outcome == ProbeOutcome::Pass, "snapshot: xray pid + Pass");
        { auto tr = network::makeCurlTransport(); network::TransportRequest q; q.url = pc.check_a_url; q.proxy_port = port; q.total_timeout_ms = 4000;
          CHECK(tr->fetch(q).status == 204, "an app using the SOCKS port gets 204"); }
        T_END();

        T_CASE("upstream killed mid-session (engine PID + listener survive) -> failover to standby, same user port");
        int engine_pid = s.engine_pid;
        up1.guard.reset();   // kill upstream 1
        CHECK(kill(engine_pid, 0) == 0, "user-port engine process is still alive after the upstream died");
        CHECK(waitFor([&] { SessionSnapshot q; return mgr.snapshot(u1, &q) && q.state == SessionState::Connected && q.endpoint_key == u2; }, 40000), "failed over to upstream 2 and Connected again");
        mgr.snapshot(u1, &s);
        CHECK(s.failovers == 1 && s.previous_key == u1 && s.user_port == port, "failover bookkeeping; user port unchanged");
        CHECK(s.engine_pid != engine_pid, "engine restarted on the user port");
        { auto tr = network::makeCurlTransport(); network::TransportRequest q; q.url = pc.check_a_url; q.proxy_port = port; q.total_timeout_ms = 4000;
          CHECK(tr->fetch(q).status == 204, "traffic flows through the user port again (via upstream 2)"); }
        { std::lock_guard<std::mutex> lk(em); bool sw = false; for (auto& x : states) sw |= x == "Switching"; CHECK(sw, "UI saw the Switching state"); }
        T_END();

        T_CASE("engine SIGKILLed -> restart with backoff -> Connected again (child reaped, no zombie)");
        int pid2 = s.engine_pid;
        kill(pid2, SIGKILL);
        CHECK(waitFor([&] { SessionSnapshot q; return mgr.snapshot(u1, &q) && q.state == SessionState::Connected && q.engine_pid != pid2 && q.engine_pid > 0; }, 30000), "restarted and Connected");
        CHECK(kill(pid2, 0) == -1 && errno == ESRCH, "old pid fully reaped");
        T_END();

        T_CASE("upstream 2 killed too and no candidate left: Unavailable, never Connected, SOCKS port never goes direct");
        mgr.setCandidateProvider([] { return std::vector<Candidate>(); });
        up2.guard.reset();
        CHECK(waitFor([&] { SessionSnapshot q; return mgr.snapshot(u1, &q) && q.state == SessionState::Unavailable; }, 40000), "Unavailable");
        { network::TransportRequest q; q.url = pc.check_a_url; q.proxy_port = port; q.total_timeout_ms = 3000; auto r = network::makeCurlTransport()->fetch(q);
          CHECK(r.status != 204, "no traffic gets through (no direct fallback)"); }
        T_END();

        T_CASE("Stop: engines reaped, user port closed");
        mgr.snapshot(u1, &s); int pid3 = s.engine_pid;
        mgr.stopProxy(u1);
        CHECK(mgr.waitAllStopped(10), "stopped");
        CHECK(pid3 <= 0 || (kill(pid3, 0) == -1 && errno == ESRCH), "engine gone");
        CHECK(!utils::isPortAlive(port, 300), "user port closed");
        T_END();
    }
    stop = true; srv.join();
    return T_SUMMARY();
}
