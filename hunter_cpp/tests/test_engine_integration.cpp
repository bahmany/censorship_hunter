// Opt-in integration test (cmake -DHUNTER_INTEGRATION_TESTS=ON, ctest -L integration).
// Runs a REAL xray (HUNTER_XRAY_PATH) with a mixed inbound -> freedom outbound on a leased port,
// and fetches a loopback HTTP server through it using the production curl transport. No public network.
// Exit 77 (skipped) when no xray binary is available.
#include "test_support.h"
#include "network/engine_launcher.h"
#include "network/port_lease.h"
#include "network/traffic_probe.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <thread>
using namespace hunter::network;

int main() {
    const char* xp = std::getenv("HUNTER_XRAY_PATH");
    if (!xp || access(xp, X_OK) != 0) { std::cout << "SKIP: set HUNTER_XRAY_PATH to an xray binary" << std::endl; return 77; }
    int hs = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(hs, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); bind(hs, (sockaddr*)&a, sizeof a); listen(hs, 8);
    socklen_t l = sizeof a; getsockname(hs, (sockaddr*)&a, &l); int hp = ntohs(a.sin_port);
    std::thread srv([hs] { for (int i = 0; i < 2; i++) { int c = accept(hs, nullptr, nullptr); if (c < 0) return; char b[2048]; recv(c, b, sizeof b, 0);
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
    { int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (sockaddr*)&a, sizeof a); close(c); }
    srv.join(); close(hs);
    return T_SUMMARY();
}
