// Opt-in integration test (cmake -DHUNTER_INTEGRATION_TESTS=ON, ctest -L integration).
// Runs a REAL xray (HUNTER_XRAY_PATH) with a mixed inbound -> freedom outbound on a leased port,
// and fetches a loopback HTTP server through it using the production curl transport. No public network.
// Exit 77 (skipped) when no xray binary is available.
#include "test_support.h"
#include "network/engine_launcher.h"
#include "network/port_lease.h"
#include "network/traffic_probe.h"
#include "network/uri_parser.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <poll.h>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace hunter::network;

int main() {
    const char* xp = std::getenv("HUNTER_XRAY_PATH");
    const char* sbp = std::getenv("HUNTER_SINGBOX_PATH");
    const bool have_sb = sbp && access(sbp, X_OK) == 0;
    if ((!xp || access(xp, X_OK) != 0) && !have_sb) { std::cout << "SKIP: set HUNTER_XRAY_PATH and/or HUNTER_SINGBOX_PATH" << std::endl; return 77; }
    if (!xp || access(xp, X_OK) != 0) {   // sing-box only
        T_CASE("real sing-box: generated hy2/tuic/insecure configs start and listen");
        auto reg = PortLeaseRegistry::create();
        for (const char* uri : {"hysteria2://pw@203.0.113.9:443?sni=a.example.com&insecure=1", "tuic://11111111-2222-3333-4444-555555555555:pw@203.0.113.9:443?sni=a.example.com&alpn=h3",
                                "trojan://pw@203.0.113.9:443?security=tls&sni=a.example.com&allowInsecure=1"}) {
            auto c = hunter::network::UriParser::parse(uri); CHECK(c && c->isValid(), "parsed");
            auto lease = reg->acquire(); LaunchRequest rq; rq.engine = "sing-box"; rq.ports = {lease.port()}; rq.startup_timeout_ms = 15000;
            rq.config_text = c->toSingBoxConfigJson(lease.port()); lease.releaseSocket();
            ProcessEngineLauncher pl("", sbp, ""); auto lr = pl.launch(rq);
            CHECK(lr.status == LaunchStatus::Ok, std::string(uri) + ": " + lr.detail);
        }
        T_END();
        return T_SUMMARY();
    }
    int hs = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(hs, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); bind(hs, (sockaddr*)&a, sizeof a); listen(hs, 8);
    socklen_t l = sizeof a; getsockname(hs, (sockaddr*)&a, &l); int hp = ntohs(a.sin_port);
    std::atomic<bool> stop{false};
    std::thread srv([hs, &stop] { while (!stop) { pollfd pf{hs, POLLIN, 0}; if (poll(&pf, 1, 100) <= 0) continue;   // stop-aware: never blocks forever
        int c = accept(hs, nullptr, nullptr); if (c < 0) continue; char b[2048]; recv(c, b, sizeof b, 0);
        const char* r = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"; send(c, r, strlen(r), MSG_NOSIGNAL); close(c); } });

    T_CASE("real xray: launch on leased port, fetch loopback server through it");
    auto reg = PortLeaseRegistry::create();
    auto lease = reg->acquire(); int port = lease.port();
    LaunchRequest rq; rq.engine = "xray"; rq.ports = {port}; rq.startup_timeout_ms = 15000;
    rq.config_text = "{\"log\":{\"loglevel\":\"warning\"},\"inbounds\":[{\"tag\":\"in\",\"port\":" + std::to_string(port) +
        ",\"listen\":\"127.0.0.1\",\"protocol\":\"mixed\",\"settings\":{\"udp\":false}}],\"outbounds\":[{\"protocol\":\"freedom\",\"tag\":\"out\"}]}";
    lease.releaseSocket();
    ProcessEngineLauncher pl(xp, "", "");
    auto lr = pl.launch(rq);
    CHECK(lr.status == LaunchStatus::Ok, "xray started: " + lr.detail);
    if (lr.status == LaunchStatus::Ok) {
        auto tr = makeCurlTransport(); TransportRequest q; q.url = "http://127.0.0.1:" + std::to_string(hp) + "/generate_204"; q.proxy_port = port; q.total_timeout_ms = 8000;
        auto r = tr->fetch(q); CHECK(r.status == 204, "204 through real xray");
    }
    T_END();
    stop = true;
    srv.join(); close(hs);
    return T_SUMMARY();
}
