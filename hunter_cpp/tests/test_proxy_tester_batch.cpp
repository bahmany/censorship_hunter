#include "test_support.h"
#include "fake_transport.h"
#include "network/proxy_tester.h"
#include "network/uri_parser.h"
#include "proxy/xray_manager.h"
#include <atomic>
#include <cstdlib>
#include <map>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using namespace hunter;
using namespace hunter::network;
using namespace fake;

static std::string hostAfter(const std::string& text, const std::string& anchor, const std::string& key) {
    auto p = text.find(anchor); if (p == std::string::npos) return "";
    p = text.find(key, p); if (p == std::string::npos) return "";
    p += key.size(); return text.substr(p, text.find('"', p) - p);
}

struct MockLauncher : EngineLauncher {
    std::mutex mu;
    struct Call { std::string engine; std::string text; std::vector<int> ports; };
    std::vector<Call> calls;
    std::map<int, std::pair<std::string, std::string>> live;   // port -> {engine, host}
    std::set<std::string> missing;
    int conflicts = 0;           // number of BindConflict answers to give first
    bool break_all = false;
    bool crash = false;          // launched engine reports exited during the round      // every launch is rejected
    bool available(const std::string& e) const override { return !missing.count(e); }
    LaunchResult launch(const LaunchRequest& rq) override {
        std::lock_guard<std::mutex> l(mu);
        calls.push_back({rq.engine, rq.config_text, rq.ports});
        LaunchResult r;
        if (conflicts > 0) { conflicts--; r.status = LaunchStatus::BindConflict; return r; }
        if (rq.config_text.find("crash") != std::string::npos) crash = true;
        if (break_all || rq.config_text.find("bad-") != std::string::npos) { r.status = LaunchStatus::StartupFailed; r.detail = "bad outbound"; return r; }
        for (int p : rq.ports) {
            std::string host = rq.engine == "xray" ? hostAfter(rq.config_text, "\"tag\":\"proxy-" + std::to_string(p) + "\"", "\"address\":\"")
                                                   : hostAfter(rq.config_text, "\"type\":", "\"server\":\"");
            live[p] = {rq.engine, host};
        }
        auto ports = rq.ports; auto* self = this;
        r.status = LaunchStatus::Ok;
        if (crash) r.alive = [] { return false; };
        r.guard = std::shared_ptr<void>(nullptr, [self, ports](void*) { std::lock_guard<std::mutex> g(self->mu); for (int p : ports) self->live.erase(p); });
        return r;
    }
    int count(const std::string& engine) { int n = 0; for (auto& c : calls) if (c.engine == engine) n++; return n; }
};

struct Rig {
    std::shared_ptr<MockLauncher> ml = std::make_shared<MockLauncher>();
    std::shared_ptr<FakeTransport> tr = std::make_shared<FakeTransport>();
    std::shared_ptr<FakeTransport> direct = std::make_shared<FakeTransport>();
    std::shared_ptr<ConnectivityBaseline> bl;
    std::shared_ptr<PortLeaseRegistry> reg = PortLeaseRegistry::create();
    ProxyTester t;
    Rig() {
        tr->handler = [this](const TransportRequest& q) -> TransportResponse {
            std::string host, engine;
            { std::lock_guard<std::mutex> l(ml->mu); auto it = ml->live.find(q.proxy_port); if (it == ml->live.end()) return err(TransportError::ProxyConnect); host = it->second.second; engine = it->second.first; }
            if (host.find("dead") != std::string::npos) return err(TransportError::Timeout);
            if (host.find("crash") != std::string::npos) return err(TransportError::Tls, "Broken pipe");
            if (host.find("slowbulk") != std::string::npos && isBulk(q)) return err(TransportError::Timeout);
            if (isA(q)) return okA(); if (isB(q)) return okB(); return okBulk();
        };
        direct->handler = [](const TransportRequest& q) { return isA(q) ? okA() : okB(); };
        bl = std::make_shared<ConnectivityBaseline>(direct);
        t.setLauncher(ml); t.setTrafficProbe(std::make_shared<TrafficProbe>(tr)); t.setBaseline(bl); t.setLeaseRegistry(reg);
    }
};
static std::string trojan(const std::string& host) { return "trojan://pw@" + host + ":443?sni=" + host; }
static std::string hy2(const std::string& host) { return "hysteria2://pw@" + host + ":443?sni=" + host; }
static std::string tuic(const std::string& host) { return "tuic://11111111-2222-3333-4444-555555555555:pw@" + host + ":443?sni=" + host; }

int main() {
    setenv("HUNTER_ENABLE_TCP_PRESCREEN", "1", 1);   // must precede first use (cached)

    T_CASE("xray group: one bad outbound is bisected out, neighbours pass");
    { Rig r; std::vector<std::string> u; for (int i = 0; i < 7; i++) u.push_back(trojan("ok" + std::to_string(i) + ".example.com"));
      u[3] = trojan("bad-3.example.com");
      auto res = r.t.testBatch(u);
      CHECK(res.size() == 7, "7 results");
      for (int i = 0; i < 7; i++) {
          if (i == 3) { CHECK(res[i].probe.outcome == ProbeOutcome::InvalidConfig && !res[i].probe.attributable, "bad one = InvalidConfig, no penalty"); }
          else { CHECK(res[i].success && res[i].probe.outcome == ProbeOutcome::Pass, "neighbour passes " + std::to_string(i)); }
      }
      CHECK(r.ml->count("xray") > 1 && r.ml->count("xray") <= 16, "bisect launches bounded"); CHECK(res[0].probe.exit_country == "NL", "country stored");
      CHECK(res[0].latency_ms > 0, "latency set"); CHECK(r.reg->activeCount() == 0, "leases released"); } T_END();

    T_CASE("dead server: attributable RemoteFailure when baseline online; none when offline");
    { Rig r; auto res = r.t.testBatch({trojan("ok.example.com"), trojan("dead.example.com")});
      CHECK(res[0].success, "ok passes"); CHECK(res[1].probe.outcome == ProbeOutcome::RemoteFailure && res[1].probe.attributable, "dead attributable");
      Rig o; o.direct->handler = [](const TransportRequest&) { return err(TransportError::Timeout); };
      o.bl = std::make_shared<ConnectivityBaseline>(o.direct, ProbeConfig(), nullptr, [] { return false; }, [](double) {}); o.t.setBaseline(o.bl);
      auto res2 = o.t.testBatch({trojan("dead.example.com")});
      CHECK(res2[0].probe.outcome == ProbeOutcome::LocalNetworkDown && !res2[0].probe.attributable, "offline: no penalty"); } T_END();

    T_CASE("hy2/tuic go to sing-box, one worker each; xray never sees them");
    { Rig r; auto res = r.t.testBatch({trojan("a.example.com"), hy2("h.example.com"), tuic("t.example.com")}, [] { BatchTestOptions o; o.bulk = true; return o; }());
      CHECK(res[0].engine_used == "xray" && res[1].engine_used == "sing-box" && res[2].engine_used == "sing-box", "engines");
      CHECK(r.ml->count("sing-box") == 2 && r.ml->count("xray") == 1, "launch counts");
      for (auto& c : r.ml->calls) if (c.engine == "xray") CHECK(c.text.find("hysteria2") == std::string::npos && c.text.find("tuic") == std::string::npos, "xray config has no QUIC");
      CHECK(res[1].success && res[2].success, "quic pass"); CHECK(res[1].download_speed_kbps > 100.0f, "real bulk KiB/s"); } T_END();

    T_CASE("QUIC skips TCP prescreen; TCP prescreen fail is a classified raw failure");
    { Rig r; auto res = r.t.testBatch({hy2("127.0.0.1"), trojan("127.0.0.1")});
      CHECK(res[0].success, "hy2 to closed TCP port is still probed"); CHECK(!res[1].success && res[1].has_probe, "trojan prescreen fails");
      CHECK(r.ml->count("xray") == 0, "prescreen-failed config never launched");
      CHECK(res[1].probe.outcome == ProbeOutcome::RemoteFailure, "classified by baseline"); } T_END();

    T_CASE("BindConflict: retried on fresh ports with zero penalty; exhausted = BindConflict outcome");
    { Rig r; r.ml->conflicts = 2; auto res = r.t.testBatch({trojan("a.example.com")});
      CHECK(res[0].success, "passes after 2 conflicts"); CHECK(r.ml->count("xray") == 3, "3 launches");
      std::set<int> ps; for (auto& c : r.ml->calls) ps.insert(c.ports[0]); CHECK(ps.size() >= 2, "ports re-leased");
      Rig x; x.ml->conflicts = 99; auto rx = x.t.testBatch({trojan("a.example.com")});
      CHECK(rx[0].probe.outcome == ProbeOutcome::BindConflict && !rx[0].probe.attributable, "exhausted"); CHECK(x.ml->count("xray") == 3, "bounded to 3"); } T_END();

    T_CASE("missing binary => Unsupported; broken engine => EngineError, not InvalidConfig");
    { Rig r; r.ml->missing = {"xray", "sing-box", "mihomo"}; auto res = r.t.testBatch({trojan("a.example.com"), hy2("b.example.com")});
      CHECK(res[0].probe.outcome == ProbeOutcome::Unsupported && res[1].probe.outcome == ProbeOutcome::Unsupported, "unsupported");
      Rig b; b.ml->break_all = true; auto rb = b.t.testBatch({trojan("a.example.com"), trojan("b.example.com"), trojan("c.example.com")});
      for (auto& x : rb) CHECK(x.probe.outcome == ProbeOutcome::EngineError, "engine error"); } T_END();

    T_CASE("invalid URI => InvalidConfig without launching");
    { Rig r; auto res = r.t.testBatch({"vless://notauri", "trojan://pw@h.example.com:443abc"});
      for (auto& x : res) CHECK(x.probe.outcome == ProbeOutcome::InvalidConfig, "invalid"); CHECK(r.ml->calls.empty(), "no launch"); } T_END();

    T_CASE("test/batch configs: fragment policy via sockopt.dialerProxy, no direct outbound");
    { Rig r; r.t.testBatch({trojan("a.example.com"), trojan("b.example.com")});
      auto& txt = r.ml->calls[0].text;
      CHECK(txt.find("\"dialerProxy\":\"fragment-out\"") != std::string::npos && txt.find("\"fragment\"") != std::string::npos, "batch fragment");
      CHECK(txt.find("\"tag\":\"direct\"") == std::string::npos && txt.find("\"outboundTag\":\"direct\"") == std::string::npos, "no direct");
      auto c = UriParser::parse(trojan("a.example.com"));
      auto single = proxy::XRayManager::generateTestConfig(*c, 31555);
      CHECK(single.find("\"dialerProxy\":\"fragment-out\"") != std::string::npos, "single test config shares policy");
      CHECK(single.find("\"tag\":\"direct\"") == std::string::npos, "single no direct"); } T_END();

    T_CASE("run identity and generation stamped");
    { Rig r; BatchTestOptions o; o.generation = 7; o.run_prefix = "rp";
      auto res = r.t.testBatch({trojan("a.example.com"), trojan("b.example.com")}, o);
      CHECK(res[0].probe.generation == 7 && res[0].probe.run_id == "rp-0" && res[1].probe.run_id == "rp-1", "ids");
      CHECK(!res[0].probe.endpoint_key.empty() && res[0].endpoint_key == res[0].probe.endpoint_key, "key"); } T_END();

    // ── Fix round 1 regressions ──
    const std::string kInsecure = "trojan://review-only@127.0.0.1:34990?security=tls&sni=localhost&allowInsecure=1&fp=chrome";   // reviewer fixture URI
    // The fixture URI points at 127.0.0.1:34990; keep a TCP listener there so the (enabled) prescreen passes.
    int lsock = socket(AF_INET, SOCK_STREAM, 0); { sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(34990);
      int one = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); bind(lsock, (sockaddr*)&a, sizeof a); listen(lsock, 64); }
    T_CASE("#1 insecure TLS link runs on sing-box with insecure honored, never on xray");
    { Rig r; auto res = r.t.testBatch({kInsecure});
      CHECK(res[0].engine_used == "sing-box" && r.ml->count("xray") == 0, "sing-box chosen");
      CHECK(!r.ml->calls.empty() && r.ml->calls[0].text.find("\"insecure\":true") != std::string::npos, "insecure emitted");
      CHECK(res[0].success, "passes through capable engine"); } T_END();

    T_CASE("#1 insecure link without sing-box => Unsupported, non-attributable (no false Dead)");
    { Rig r; r.ml->missing = {"sing-box"}; auto res = r.t.testBatch({kInsecure});
      CHECK(res[0].probe.outcome == ProbeOutcome::Unsupported && !res[0].probe.attributable, "unsupported");
      CHECK(r.ml->calls.empty(), "nothing launched on an engine that ignores the option"); } T_END();

    T_CASE("#4 bulk failure after A+B pass => Partial (not Pass, not attributed)");
    { Rig r; BatchTestOptions o; o.bulk = true; auto res = r.t.testBatch({trojan("slowbulk.example.com"), trojan("ok.example.com")}, o);
      CHECK(res[0].probe.outcome == ProbeOutcome::Partial && !res[0].success && !res[0].probe.attributable, "partial");
      CHECK(!res[0].probe.bulk_passed && res[0].error_message.find("bulk") != std::string::npos, "reason preserved");
      CHECK(res[1].success && res[1].probe.bulk_passed, "other passes fully"); } T_END();

    T_CASE("#5 engine exit during the round => EngineError for failing configs, even with a healthy control");
    { Rig r; r.ml->crash = true; auto res = r.t.testBatch({trojan("crash.example.com"), trojan("crash2.example.com")});
      for (auto& x : res) CHECK(x.probe.outcome == ProbeOutcome::EngineError && !x.probe.attributable, "engine error");
      Rig q; q.ml->conflicts = 0; auto ok = q.t.testBatch({trojan("dead.example.com"), trojan("ok.example.com")});
      CHECK(ok[0].probe.outcome == ProbeOutcome::RemoteFailure, "no crash => normal attribution"); } T_END();

    T_CASE("#2 results carry the live baseline generation");
    { Rig r; r.bl->bumpGeneration(); r.bl->bumpGeneration(); auto res = r.t.testBatch({trojan("dead.example.com")});
      CHECK(res[0].probe.generation == 2, "generation propagated"); CHECK(res[0].probe.attributable, "fresh evidence under same generation"); } T_END();
    T_CASE("round3: reviewer XHTTP fixture (insecure) => Unsupported non-attributable, nothing launched");
    { Rig r; auto res = r.t.testBatch({"vless://11111111-2222-3333-4444-555555555555@127.0.0.1:34990?security=tls&type=xhttp&path=%2Freview&sni=localhost&allowInsecure=1"});
      CHECK(res[0].probe.outcome == ProbeOutcome::Unsupported && !res[0].probe.attributable, "unsupported");
      CHECK(res[0].error_message.find("xhttp") != std::string::npos, "reason names the transport: " + res[0].error_message); CHECK(r.ml->calls.empty(), "no engine launched");
      Rig x; auto rx = x.t.testBatch({"vless://11111111-2222-3333-4444-555555555555@127.0.0.1:34990?security=tls&type=madeup&sni=localhost"});
      CHECK(rx[0].probe.outcome == ProbeOutcome::Unsupported && x.ml->calls.empty(), "unknown transport unsupported"); } T_END();

    T_CASE("round3: reviewer hy2 pin fixture (insecure + zero pin) is never a Pass");
    { Rig r; auto res = r.t.testBatch({"hysteria2://pw@127.0.0.1:34990?sni=localhost&insecure=1&pinSHA256=00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00"});
      CHECK(res[0].probe.outcome == ProbeOutcome::Unsupported && !res[0].probe.attributable && !res[0].success, "unsupported, not Pass");
      CHECK(r.ml->calls.empty(), "nothing launched with an ignored identity constraint"); } T_END();

    T_CASE("round3: xhttp (non-insecure) still runs on xray");
    { Rig r; auto res = r.t.testBatch({"vless://11111111-2222-3333-4444-555555555555@127.0.0.1:34990?security=tls&type=xhttp&path=%2Fx&sni=localhost"});
      CHECK(res[0].engine_used == "xray", "xray handles xhttp natively"); } T_END();
    close(lsock);
    return T_SUMMARY();
}
