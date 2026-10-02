#pragma once
// Real-traffic probe (design D1): HTTPS checks THROUGH a local SOCKS5 port,
// strict validation, injectable transport. No process management here.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "core/health_score.h"

namespace hunter {
namespace network {

// ── Transport (injectable) ──────────────────────────────────────────────
enum class TransportError {
    None,
    Timeout,
    ProxyConnect,   // could not connect to the LOCAL proxy port (engine problem)
    ProxyFailure,   // proxy answered but could not reach the destination (remote problem)
    Dns,
    Connect,
    Tls,            // certificate / handshake failure
    TooLarge,       // body exceeded max_body_bytes
    Other
};
const char* transportErrorName(TransportError e);

struct TransportRequest {
    std::string url;
    int proxy_port = 0;          // 0 = DIRECT (proxies bypassed); else 127.0.0.1:<port> SOCKS5h
    int connect_timeout_ms = 3000;
    int total_timeout_ms = 8000;
    uint64_t max_body_bytes = 8192;
    bool tls_verify = true;      // always true for health probes
};

struct TransportResponse {
    TransportError error = TransportError::None;
    std::string error_detail;
    long status = 0;
    std::string content_type;
    std::string content_encoding;
    std::string location;        // Location header if any
    std::string body;            // decoded body (bounded by max_body_bytes)
    uint64_t body_bytes = 0;     // decoded body size
    double elapsed_ms = 0.0;     // wall time until the response completed
};

class ProbeTransport {
public:
    virtual ~ProbeTransport() = default;
    virtual TransportResponse fetch(const TransportRequest& req) = 0;
};

// libcurl implementation: CURLPROXY_SOCKS5_HOSTNAME, TLS verify ON (peer=1, host=2),
// FOLLOWLOCATION=0, NOPROXY set explicitly so environment proxies never apply.
std::shared_ptr<ProbeTransport> makeCurlTransport();

// ── Probe profile / thresholds (all tunables in one struct, amendment M4) ─
struct ProbeConfig {
    int probe_profile = 1;
    std::string check_a_url = "https://www.gstatic.com/generate_204";
    std::string check_b_url = "https://www.cloudflare.com/cdn-cgi/trace";
    std::string check_b_host = "www.cloudflare.com";
    std::string bulk_url = "https://speed.cloudflare.com/__down?bytes=65536";
    int connect_timeout_ms = 3000;
    int overall_timeout_ms = 8000;
    int bulk_timeout_ms = 12000;
    uint64_t bulk_bytes = 65536;
    size_t trace_min_bytes = 64;
    size_t trace_max_bytes = 4096;
};

// ── Strict parsers (exposed for tests) ──────────────────────────────────
struct TraceInfo {
    bool valid = false;
    std::string reason;
    std::string ip;
    std::string loc;      // "[A-Z]{2}" syntactically valid
    std::string host;
};
// Bounded key=value parser. Requires unique h=<expected_host>, public ip=, numeric ts=, loc=[A-Z]{2}.
TraceInfo parseCloudflareTrace(const std::string& body, const std::string& expected_host);
bool isPublicIpLiteral(const std::string& ip);

// ── Raw (unclassified) round outcome ────────────────────────────────────
enum class CheckStatus { NotRun, Pass, Fail };
enum class CheckFailure { None, Timeout, ProxyConnect, Remote, Tls, BadStatus, Redirect, BadBody };

struct CheckOutcome {
    CheckStatus status = CheckStatus::NotRun;
    CheckFailure failure = CheckFailure::None;
    std::string detail;
    double elapsed_ms = 0.0;
};

struct RawProbe {
    CheckOutcome a;
    CheckOutcome b;
    CheckOutcome bulk;
    bool bulk_run = false;
    double latency_ms = -1.0;   // max(A,B) wall time when both pass
    double bulk_kibps = 0.0;    // real KiB/s of the bulk transfer, 0 if not run / failed
    uint64_t bulk_bytes = 0;
    std::string exit_ip;
    std::string exit_country;   // stored only, no UI (D4)
    int port = 0;
    double started_at = 0.0;    // UTC seconds
    double finished_at = 0.0;
    bool engine_died = false;          // local engine process exited during/after the round (set by the tester)
    bool engine_unreachable = false;   // local proxy port refused on every check
    bool bothPass() const { return a.status == CheckStatus::Pass && b.status == CheckStatus::Pass; }
};

class TrafficProbe {
public:
    explicit TrafficProbe(std::shared_ptr<ProbeTransport> transport = nullptr,
                          ProbeConfig cfg = ProbeConfig(), ClockFn clock = nullptr);

    // Runs A and B concurrently through 127.0.0.1:<port>; optional bulk afterwards when
    // both pass. port == 0 runs the same checks DIRECT (used by ConnectivityBaseline).
    RawProbe run(int port, bool with_bulk) const;

    // Individual strict checks (usable by tests / baseline).
    CheckOutcome checkA(int port) const;
    CheckOutcome checkB(int port, TraceInfo* info) const;
    CheckOutcome checkBulk(int port, double* kibps, uint64_t* bytes) const;

    const ProbeConfig& config() const { return cfg_; }
    ProbeTransport& transport() const { return *transport_; }

private:
    std::shared_ptr<ProbeTransport> transport_;
    ProbeConfig cfg_;
    ClockFn clock_;
};

}  // namespace network
}  // namespace hunter
