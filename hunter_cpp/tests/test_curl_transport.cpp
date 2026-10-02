#include "test_support.h"
#include "network/traffic_probe.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <thread>
using namespace hunter::network;

static int listenLoop(int* port) {
    int s = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    bind(s, (sockaddr*)&a, sizeof a); listen(s, 16); socklen_t l = sizeof a; getsockname(s, (sockaddr*)&a, &l); *port = ntohs(a.sin_port); return s;
}
static bool readN(int fd, void* b, size_t n) { size_t g = 0; while (g < n) { ssize_t r = recv(fd, (char*)b + g, n - g, 0); if (r <= 0) return false; g += r; } return true; }
static void sendAll(int fd, const std::string& s) { size_t o = 0; while (o < s.size()) { ssize_t w = send(fd, s.data() + o, s.size() - o, MSG_NOSIGNAL); if (w <= 0) return; o += w; } }

static std::atomic<bool> g_stop{false};
static void httpServer(int ls) {
    while (!g_stop) {
        int c = accept(ls, nullptr, nullptr); if (c < 0) break;
        std::string req; char ch; while (req.find("\r\n\r\n") == std::string::npos && recv(c, &ch, 1, 0) > 0) req += ch;
        std::string path = req.substr(req.find(' ') + 1); path = path.substr(0, path.find(' '));
        if (path == "/generate_204") sendAll(c, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
        else if (path == "/redir") sendAll(c, "HTTP/1.1 302 Found\r\nLocation: http://captive.invalid/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        else if (path == "/big") { std::string b(100000, 'x'); sendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n" + b); }
        else sendAll(c, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        close(c);
    }
}
static std::mutex g_m; static std::string g_host; static int g_atyp = -1;
static void socksServer(int ls, int http_port) {
    while (!g_stop) {
        int c = accept(ls, nullptr, nullptr); if (c < 0) break;
        unsigned char h[2]; if (!readN(c, h, 2)) { close(c); continue; }
        std::vector<unsigned char> m(h[1]); readN(c, m.data(), m.size());
        sendAll(c, std::string("\x05\x00", 2));
        unsigned char q[4]; if (!readN(c, q, 4)) { close(c); continue; }
        std::string host; if (q[3] == 3) { unsigned char n; readN(c, &n, 1); host.resize(n); readN(c, &host[0], n); } else { unsigned char skip[4]; readN(c, skip, 4); host = "ip"; }
        unsigned char pt[2]; readN(c, pt, 2);
        { std::lock_guard<std::mutex> g(g_m); g_host = host; g_atyp = q[3]; }
        sendAll(c, std::string("\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00", 10));
        int up = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(http_port);
        if (connect(up, (sockaddr*)&a, sizeof a) != 0) { close(c); close(up); continue; }
        std::thread t([&] { char b[4096]; ssize_t n; while ((n = recv(c, b, sizeof b, 0)) > 0) send(up, b, n, MSG_NOSIGNAL); shutdown(up, SHUT_WR); });
        char b[4096]; ssize_t n; while ((n = recv(up, b, sizeof b, 0)) > 0) send(c, b, n, MSG_NOSIGNAL);
        shutdown(c, SHUT_WR); t.join(); close(c); close(up);
    }
}

int main() {
    int hp, sp; int hl = listenLoop(&hp), sl = listenLoop(&sp);
    std::thread ht(httpServer, hl), st(socksServer, sl, hp);
    auto tr = makeCurlTransport();
    auto fetch = [&](const std::string& path, int proxy, uint64_t cap = 8192) {
        TransportRequest q; q.url = "http://probe-host.invalid" + path; q.proxy_port = proxy; q.max_body_bytes = cap; q.total_timeout_ms = 5000; q.connect_timeout_ms = 2000;
        return tr->fetch(q); };

    T_CASE("SOCKS5h: hostname is sent to the proxy, 204 returned");
    { auto r = fetch("/generate_204", sp); CHECK(r.error == TransportError::None && r.status == 204, "204 via socks");
      std::lock_guard<std::mutex> g(g_m); CHECK(g_atyp == 3 && g_host == "probe-host.invalid", "domain (ATYP 3) passed, no local DNS"); } T_END();

    T_CASE("redirect is reported, never followed");
    { auto r = fetch("/redir", sp); CHECK(r.status == 302 && r.location.find("captive.invalid") != std::string::npos, "302 + Location"); } T_END();

    T_CASE("body cap aborts with TooLarge; small body returned");
    { auto r = fetch("/big", sp); CHECK(r.error == TransportError::TooLarge, "too large");
      auto s = fetch("/small", sp); CHECK(s.status == 200 && s.body == "hello" && s.body_bytes == 5, "small body"); CHECK(s.elapsed_ms > 0, "elapsed measured"); } T_END();

    T_CASE("closed local proxy port => ProxyConnect (engine problem)");
    { int dead, dl = listenLoop(&dead); close(dl); auto r = fetch("/generate_204", dead); CHECK(r.error == TransportError::ProxyConnect, std::string("got ") + transportErrorName(r.error)); } T_END();

    T_CASE("port 0 bypasses proxy (direct)");
    { TransportRequest q; q.url = "http://127.0.0.1:" + std::to_string(hp) + "/generate_204"; q.proxy_port = 0; q.total_timeout_ms = 5000;
      setenv("http_proxy", "http://127.0.0.1:1", 1); setenv("HTTP_PROXY", "http://127.0.0.1:1", 1);
      auto r = tr->fetch(q); CHECK(r.status == 204, "direct ignores env proxies"); } T_END();

    g_stop = true; shutdown(hl, SHUT_RDWR); shutdown(sl, SHUT_RDWR); close(hl); close(sl);
    { int c = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(hp); connect(c, (sockaddr*)&a, sizeof a); close(c);
      c = socket(AF_INET, SOCK_STREAM, 0); a.sin_port = htons(sp); connect(c, (sockaddr*)&a, sizeof a); close(c); }
    ht.detach(); st.detach();
    return T_SUMMARY();
}
