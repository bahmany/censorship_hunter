#pragma once
// Fake ProbeTransport + canned responses for A2 tests (no network).
#include <functional>
#include <mutex>
#include <vector>
#include "network/traffic_probe.h"

namespace fake {
using namespace hunter::network;

inline std::string trace(const std::string& ip = "93.184.216.34", const std::string& loc = "NL",
                         const std::string& h = "www.cloudflare.com") {
    return "fl=123f45\nh=" + h + "\nip=" + ip + "\nts=1760000000.123\nvisit_scheme=https\nuag=curl/8\ncolo=AMS\nsliver=none\nhttp=http/2\nloc=" +
           loc + "\ntls=TLSv1.3\nsni=plaintext\nwarp=off\ngateway=off\nrbi=off\nkex=X25519\n";
}
inline TransportResponse okA() { TransportResponse r; r.status = 204; r.elapsed_ms = 40; return r; }
inline TransportResponse okB(const std::string& body = trace()) {
    TransportResponse r; r.status = 200; r.body = body; r.body_bytes = body.size(); r.elapsed_ms = 60; r.content_type = "text/plain"; return r;
}
inline TransportResponse okBulk(uint64_t n = 65536, double ms = 500) {
    TransportResponse r; r.status = 200; r.body.assign(n, '\x5a'); r.body_bytes = n; r.elapsed_ms = ms;
    r.content_type = "application/octet-stream"; return r;
}
inline TransportResponse err(TransportError e, const std::string& d = "x") {
    TransportResponse r; r.error = e; r.error_detail = d; return r;
}
inline bool isA(const TransportRequest& q) { return q.url.find("generate_204") != std::string::npos; }
inline bool isB(const TransportRequest& q) { return q.url.find("cdn-cgi/trace") != std::string::npos; }
inline bool isBulk(const TransportRequest& q) { return q.url.find("__down") != std::string::npos; }

class FakeTransport : public ProbeTransport {
public:
    using Handler = std::function<TransportResponse(const TransportRequest&)>;
    Handler handler;
    mutable std::mutex mu;
    std::vector<TransportRequest> calls;
    TransportResponse fetch(const TransportRequest& q) override {
        { std::lock_guard<std::mutex> l(mu); calls.push_back(q); }
        return handler ? handler(q) : err(TransportError::Other);
    }
    int count(int port) const {
        std::lock_guard<std::mutex> l(mu);
        int n = 0; for (auto& c : calls) if (c.proxy_port == port) n++; return n;
    }
};
}  // namespace fake
