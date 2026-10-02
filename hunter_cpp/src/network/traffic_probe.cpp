#include "network/traffic_probe.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <set>

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace hunter {
namespace network {

const char* transportErrorName(TransportError e) {
    switch (e) {
        case TransportError::None: return "none";
        case TransportError::Timeout: return "timeout";
        case TransportError::ProxyConnect: return "proxy_connect";
        case TransportError::ProxyFailure: return "proxy_failure";
        case TransportError::Dns: return "dns";
        case TransportError::Connect: return "connect";
        case TransportError::Tls: return "tls";
        case TransportError::TooLarge: return "too_large";
        case TransportError::Other: return "other";
    }
    return "other";
}

// ── libcurl transport ───────────────────────────────────────────────────
namespace {

struct CurlCtx {
    std::string body;
    uint64_t total = 0;
    uint64_t cap = 0;
    bool overflow = false;
    std::string content_type, content_encoding, location;
};

size_t writeCb(char* ptr, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<CurlCtx*>(ud);
    size_t len = sz * n;
    c->total += len;
    if (c->total > c->cap) { c->overflow = true; return 0; }  // abort transfer
    c->body.append(ptr, len);
    return len;
}

std::string lowerTrim(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    s = s.substr(b, e - b);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

size_t headerCb(char* ptr, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<CurlCtx*>(ud);
    size_t len = sz * n;
    std::string line(ptr, len);
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        // New status line (e.g. after a proxy CONNECT response): reset captured headers.
        if (line.compare(0, 5, "HTTP/") == 0) { c->content_type.clear(); c->content_encoding.clear(); c->location.clear(); }
        return len;
    }
    std::string name = lowerTrim(line.substr(0, colon));
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
    size_t b = 0; while (b < val.size() && std::isspace((unsigned char)val[b])) b++;
    val = val.substr(b);
    if (name == "content-type") c->content_type = val;
    else if (name == "content-encoding") c->content_encoding = val;
    else if (name == "location") c->location = val;
    return len;
}

void curlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

class CurlTransport : public ProbeTransport {
public:
    TransportResponse fetch(const TransportRequest& req) override {
        curlGlobalInit();
        TransportResponse out;
        CURL* h = curl_easy_init();
        if (!h) { out.error = TransportError::Other; out.error_detail = "curl_easy_init failed"; return out; }
        CurlCtx ctx;
        ctx.cap = req.max_body_bytes;
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(h, CURLOPT_URL, req.url.c_str());
        curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);          // no redirects
        curl_easy_setopt(h, CURLOPT_USERAGENT, "hunter-probe/1");
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)req.connect_timeout_ms);
        curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, (long)req.total_timeout_ms);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, req.tls_verify ? 1L : 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, req.tls_verify ? 2L : 0L);
        const char* ca = std::getenv("HUNTER_CA_BUNDLE");
        if (ca && *ca) curl_easy_setopt(h, CURLOPT_CAINFO, ca);
        if (req.proxy_port > 0) {
            curl_easy_setopt(h, CURLOPT_PROXY, "127.0.0.1");
            curl_easy_setopt(h, CURLOPT_PROXYPORT, (long)req.proxy_port);
            curl_easy_setopt(h, CURLOPT_PROXYTYPE, (long)CURLPROXY_SOCKS5_HOSTNAME);  // remote DNS
            curl_easy_setopt(h, CURLOPT_NOPROXY, "");  // never bypass the tunnel
        } else {
            curl_easy_setopt(h, CURLOPT_PROXY, "");    // direct: ignore system/env proxies
            curl_easy_setopt(h, CURLOPT_NOPROXY, "*");
        }
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, headerCb);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);

        CURLcode rc = curl_easy_perform(h);
        double total = 0.0;
        curl_easy_getinfo(h, CURLINFO_TOTAL_TIME, &total);
        out.elapsed_ms = total * 1000.0;
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        out.status = status;
        out.content_type = ctx.content_type;
        out.content_encoding = ctx.content_encoding;
        out.location = ctx.location;
        out.body = std::move(ctx.body);
        out.body_bytes = ctx.total;
        if (rc != CURLE_OK) {
            out.error_detail = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            const bool proxied = req.proxy_port > 0;
            if (ctx.overflow) out.error = TransportError::TooLarge;
            else switch (rc) {
                case CURLE_OPERATION_TIMEDOUT: out.error = TransportError::Timeout; break;
                case CURLE_COULDNT_CONNECT: out.error = proxied ? TransportError::ProxyConnect : TransportError::Connect; break;
                case CURLE_COULDNT_RESOLVE_HOST: out.error = proxied ? TransportError::ProxyFailure : TransportError::Dns; break;
                case CURLE_PROXY: out.error = TransportError::ProxyFailure; break;
                case CURLE_PEER_FAILED_VERIFICATION:
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_SSL_CERTPROBLEM:
                case CURLE_SSL_CIPHER:
                case CURLE_SSL_CACERT_BADFILE:
                case CURLE_SSL_ISSUER_ERROR:
                    out.error = TransportError::Tls; break;
                default: out.error = TransportError::Other; break;
            }
        }
        curl_easy_cleanup(h);
        return out;
    }
};

}  // namespace

std::shared_ptr<ProbeTransport> makeCurlTransport() { return std::make_shared<CurlTransport>(); }

// ── Strict parsers ──────────────────────────────────────────────────────
bool isPublicIpLiteral(const std::string& ip) {
    if (ip.empty() || ip.size() > 45) return false;
    unsigned char b[16];
    if (inet_pton(AF_INET, ip.c_str(), b) == 1) {
        unsigned a = b[0], c = b[1];
        if (a == 0 || a == 10 || a == 127) return false;
        if (a == 100 && c >= 64 && c <= 127) return false;     // CGNAT
        if (a == 169 && c == 254) return false;
        if (a == 172 && c >= 16 && c <= 31) return false;
        if (a == 192 && c == 168) return false;
        if (a == 192 && c == 0 && b[2] == 0) return false;      // 192.0.0.0/24
        if (a == 192 && c == 0 && b[2] == 2) return false;      // TEST-NET-1
        if (a == 198 && (c == 18 || c == 19)) return false;
        if (a == 198 && c == 51 && b[2] == 100) return false;
        if (a == 203 && c == 0 && b[2] == 113) return false;
        if (a >= 224) return false;                              // multicast / reserved
        return true;
    }
    if (inet_pton(AF_INET6, ip.c_str(), b) == 1) {
        bool all_zero = true;
        for (int i = 0; i < 16; i++) if (b[i]) { all_zero = false; break; }
        if (all_zero) return false;
        bool loop = true;
        for (int i = 0; i < 15; i++) if (b[i]) { loop = false; break; }
        if (loop && b[15] == 1) return false;
        if ((b[0] & 0xfe) == 0xfc) return false;                 // fc00::/7
        if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return false; // fe80::/10
        if (b[0] == 0xff) return false;                          // multicast
        if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return false;  // doc
        // IPv4-mapped ::ffff:a.b.c.d -> judge the embedded IPv4
        bool mapped = true;
        for (int i = 0; i < 10; i++) if (b[i]) { mapped = false; break; }
        if (mapped && b[10] == 0xff && b[11] == 0xff) {
            char buf[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, b + 12, buf, sizeof buf)) return false;
            return isPublicIpLiteral(buf);
        }
        return true;
    }
    return false;
}

TraceInfo parseCloudflareTrace(const std::string& body, const std::string& expected_host) {
    TraceInfo t;
    auto fail = [&](const char* why) { t.valid = false; t.reason = why; return t; };
    if (body.empty() || body.size() > 4096) return fail("size");
    if (body.find('\0') != std::string::npos) return fail("nul");
    {
        std::string low = body.substr(0, 512);
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (low.find("<html") != std::string::npos || low.find("<!doctype") != std::string::npos ||
            low.find("<head") != std::string::npos || low.find("<body") != std::string::npos)
            return fail("html");
    }
    std::map<std::string, std::string> kv;
    std::set<std::string> dup;
    size_t pos = 0, lines = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? body.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (++lines > 64 || line.size() > 256) return fail("too_many_lines");
        for (unsigned char c : line) if (c < 0x20 || c == 0x7f) return fail("control_char");
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) return fail("malformed_line");
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (!kv.emplace(k, v).second) dup.insert(k);
    }
    for (const char* req : {"h", "ip", "ts", "loc"}) {
        if (dup.count(req)) return fail("duplicate_key");
        if (!kv.count(req)) return fail("missing_key");
    }
    if (kv["h"] != expected_host) return fail("wrong_host");
    if (!isPublicIpLiteral(kv["ip"])) return fail("bad_ip");
    {   // ts: digits with at most one '.'
        const std::string& ts = kv["ts"];
        if (ts.empty() || ts.size() > 24) return fail("bad_ts");
        int dots = 0, digits = 0;
        for (char c : ts) { if (c == '.') dots++; else if (std::isdigit((unsigned char)c)) digits++; else return fail("bad_ts"); }
        if (dots > 1 || digits == 0 || ts.front() == '.') return fail("bad_ts");
    }
    const std::string& loc = kv["loc"];
    if (loc.size() != 2 || loc[0] < 'A' || loc[0] > 'Z' || loc[1] < 'A' || loc[1] > 'Z') return fail("bad_loc");
    t.valid = true;
    t.ip = kv["ip"];
    t.loc = loc;
    t.host = kv["h"];
    return t;
}

// ── TrafficProbe ────────────────────────────────────────────────────────
TrafficProbe::TrafficProbe(std::shared_ptr<ProbeTransport> transport, ProbeConfig cfg, ClockFn clock)
    : transport_(transport ? std::move(transport) : makeCurlTransport()),
      cfg_(std::move(cfg)), clock_(clock ? std::move(clock) : systemClock()) {}

namespace {
CheckOutcome failFromTransport(const TransportResponse& r) {
    CheckOutcome o;
    o.status = CheckStatus::Fail;
    o.elapsed_ms = r.elapsed_ms;
    o.detail = std::string(transportErrorName(r.error)) + ": " + r.error_detail;
    switch (r.error) {
        case TransportError::Timeout: o.failure = CheckFailure::Timeout; break;
        case TransportError::ProxyConnect: o.failure = CheckFailure::ProxyConnect; break;
        case TransportError::Tls: o.failure = CheckFailure::Tls; break;
        default: o.failure = CheckFailure::Remote; break;
    }
    return o;
}
CheckOutcome failWith(CheckFailure f, const std::string& d, double ms) {
    CheckOutcome o; o.status = CheckStatus::Fail; o.failure = f; o.detail = d; o.elapsed_ms = ms; return o;
}
bool isRedirect(long s) { return s >= 300 && s < 400; }
bool looksHtml(const std::string& body) {
    std::string low = body.substr(0, 128);
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return low.find("<html") != std::string::npos || low.find("<!doctype") != std::string::npos;
}
}  // namespace

CheckOutcome TrafficProbe::checkA(int port) const {
    TransportRequest rq;
    rq.url = cfg_.check_a_url; rq.proxy_port = port;
    rq.connect_timeout_ms = cfg_.connect_timeout_ms; rq.total_timeout_ms = cfg_.overall_timeout_ms;
    rq.max_body_bytes = 1024;
    TransportResponse r = transport_->fetch(rq);
    if (r.error != TransportError::None) return failFromTransport(r);
    if (isRedirect(r.status)) return failWith(CheckFailure::Redirect, "redirect " + std::to_string(r.status), r.elapsed_ms);
    if (r.status != 204) return failWith(CheckFailure::BadStatus, "status " + std::to_string(r.status), r.elapsed_ms);
    if (r.body_bytes != 0 || !r.body.empty()) return failWith(CheckFailure::BadBody, "non-empty 204 body", r.elapsed_ms);
    CheckOutcome o; o.status = CheckStatus::Pass; o.elapsed_ms = r.elapsed_ms; return o;
}

CheckOutcome TrafficProbe::checkB(int port, TraceInfo* info) const {
    TransportRequest rq;
    rq.url = cfg_.check_b_url; rq.proxy_port = port;
    rq.connect_timeout_ms = cfg_.connect_timeout_ms; rq.total_timeout_ms = cfg_.overall_timeout_ms;
    rq.max_body_bytes = cfg_.trace_max_bytes;
    TransportResponse r = transport_->fetch(rq);
    if (r.error == TransportError::TooLarge) return failWith(CheckFailure::BadBody, "trace body too large", r.elapsed_ms);
    if (r.error != TransportError::None) return failFromTransport(r);
    if (isRedirect(r.status)) return failWith(CheckFailure::Redirect, "redirect " + std::to_string(r.status), r.elapsed_ms);
    if (r.status != 200) return failWith(CheckFailure::BadStatus, "status " + std::to_string(r.status), r.elapsed_ms);
    std::string ct = lowerTrim(r.content_type);
    if (ct.compare(0, 10, "text/plain") != 0) return failWith(CheckFailure::BadBody, "content-type " + ct, r.elapsed_ms);
    if (r.body.size() < cfg_.trace_min_bytes || r.body.size() > cfg_.trace_max_bytes)
        return failWith(CheckFailure::BadBody, "trace size " + std::to_string(r.body.size()), r.elapsed_ms);
    TraceInfo t = parseCloudflareTrace(r.body, cfg_.check_b_host);
    if (!t.valid) return failWith(CheckFailure::BadBody, "trace " + t.reason, r.elapsed_ms);
    if (info) *info = t;
    CheckOutcome o; o.status = CheckStatus::Pass; o.elapsed_ms = r.elapsed_ms; return o;
}

CheckOutcome TrafficProbe::checkBulk(int port, double* kibps, uint64_t* bytes) const {
    TransportRequest rq;
    rq.url = cfg_.bulk_url; rq.proxy_port = port;
    rq.connect_timeout_ms = cfg_.connect_timeout_ms; rq.total_timeout_ms = cfg_.bulk_timeout_ms;
    rq.max_body_bytes = cfg_.bulk_bytes + 1;
    TransportResponse r = transport_->fetch(rq);
    if (r.error == TransportError::TooLarge) return failWith(CheckFailure::BadBody, "bulk oversize", r.elapsed_ms);
    if (r.error != TransportError::None) return failFromTransport(r);
    if (isRedirect(r.status)) return failWith(CheckFailure::Redirect, "redirect " + std::to_string(r.status), r.elapsed_ms);
    if (r.status != 200) return failWith(CheckFailure::BadStatus, "status " + std::to_string(r.status), r.elapsed_ms);
    if (!r.content_encoding.empty()) return failWith(CheckFailure::BadBody, "content-encoding", r.elapsed_ms);
    std::string ct = lowerTrim(r.content_type);
    if (ct.compare(0, 5, "text/") == 0) return failWith(CheckFailure::BadBody, "textual content-type " + ct, r.elapsed_ms);
    if (r.body_bytes != cfg_.bulk_bytes || r.body.size() != cfg_.bulk_bytes)
        return failWith(CheckFailure::BadBody, "bulk size " + std::to_string(r.body_bytes), r.elapsed_ms);
    if (looksHtml(r.body)) return failWith(CheckFailure::BadBody, "html bulk body", r.elapsed_ms);
    if (!(r.elapsed_ms > 0.0) || !std::isfinite(r.elapsed_ms)) return failWith(CheckFailure::BadBody, "bad timing", r.elapsed_ms);
    if (kibps) *kibps = (double(r.body_bytes) / 1024.0) / (r.elapsed_ms / 1000.0);
    if (bytes) *bytes = r.body_bytes;
    CheckOutcome o; o.status = CheckStatus::Pass; o.elapsed_ms = r.elapsed_ms; return o;
}

RawProbe TrafficProbe::run(int port, bool with_bulk) const {
    RawProbe p;
    p.port = port;
    p.started_at = clock_();
    TraceInfo info;
    auto fb = std::async(std::launch::async, [this, port, &info]() { return checkB(port, &info); });
    p.a = checkA(port);
    p.b = fb.get();
    if (p.b.status == CheckStatus::Pass) { p.exit_ip = info.ip; p.exit_country = info.loc; }
    if (p.bothPass()) {
        p.latency_ms = std::max(p.a.elapsed_ms, p.b.elapsed_ms);
        if (with_bulk) {
            p.bulk_run = true;
            p.bulk = checkBulk(port, &p.bulk_kibps, &p.bulk_bytes);
            if (p.bulk.status != CheckStatus::Pass) { p.bulk_kibps = 0.0; p.bulk_bytes = 0; }
        }
    }
    p.engine_unreachable = port > 0 && p.a.failure == CheckFailure::ProxyConnect && p.b.failure == CheckFailure::ProxyConnect;
    p.finished_at = clock_();
    return p;
}

}  // namespace network
}  // namespace hunter
