#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/health_score.h"
#include "network/connectivity_baseline.h"
#include "network/engine_launcher.h"
#include "network/port_lease.h"
#include "network/traffic_probe.h"

namespace hunter {
namespace network {

/**
 * @brief Result of a proxy test (one per input URI, same order).
 *
 * `probe` is the typed round result to feed ConfigDatabase::applyProbeResult.
 * Engine/config/local problems carry excluded outcomes (EngineError, InvalidConfig,
 * Unsupported, BindConflict, LocalNetworkDown, Indeterminate): they never count against
 * the server.
 */
struct ProxyTestResult {
    bool success = false;              // outcome == Pass (both strict checks)
    bool telegram_only = false;        // legacy field, always false (Telegram is optional capability metadata)
    float download_speed_kbps = 0.0f;  // REAL bulk throughput in KiB/s; 0 = not measured (never an inverse latency)
    std::string error_message;
    std::string engine_used;           // "xray", "sing-box", "mihomo"
    std::string uri;
    // Typed result
    ProbeResult probe;
    bool has_probe = false;
    double latency_ms = -1.0;          // max(CheckA, CheckB) wall time on success, else -1
    std::string endpoint_key;
};

struct BatchTestOptions {
    int timeout_seconds = 10;     // overall per-check deadline ceiling (capped to the probe profile)
    bool bulk = false;            // run the 64 KiB transfer for configs that pass both checks
    uint64_t generation = 0;      // network generation stamped on every ProbeResult
    std::string run_prefix;       // unique round identity prefix; auto-generated when empty
    int bind_retries = 3;         // lease collision retries (zero penalty)
    int max_launches = 64;        // bisect budget per call
};

/**
 * @brief Proxy engine tester (real-traffic probes, attribution, port leases, engine dispatch).
 */
class ProxyTester {
public:
    ProxyTester();
    ~ProxyTester();

    /**
     * @brief Test many configs. xray-compatible configs share one xray process per (sub)group;
     *        hysteria2/tuic run in isolated sing-box workers. Startup failures of the shared
     *        xray process are bisected so one bad outbound never fails its neighbours.
     * @return one result per input URI, in order.
     */
    std::vector<ProxyTestResult> testBatch(const std::vector<std::string>& config_uris,
                                           const BatchTestOptions& opts = BatchTestOptions());

    /// Single config convenience wrapper over testBatch. `test_url` is ignored (fixed probe profile).
    ProxyTestResult testConfig(const std::string& config_uri,
                               const std::string& test_url = "",
                               int timeout_seconds = 30);

    static int activeTestCount();
    static int peakTestCount();
    static int maxConcurrentTestCount();

    void setXrayPath(const std::string& path);
    void setSingBoxPath(const std::string& path);
    void setMihomoPath(const std::string& path);

    // Dependency injection (tests). All default to process-wide real implementations.
    void setLauncher(std::shared_ptr<EngineLauncher> l);
    void setTrafficProbe(std::shared_ptr<TrafficProbe> p) { probe_ = std::move(p); }
    void setBaseline(std::shared_ptr<ConnectivityBaseline> b) { baseline_ = std::move(b); }
    void setLeaseRegistry(std::shared_ptr<PortLeaseRegistry> r) { leases_ = std::move(r); }
    void setClock(ClockFn c) { clock_ = std::move(c); }

    /// QUIC upstreams (hysteria2/tuic) cannot be judged by a raw TCP connect.
    static bool needsTcpPrescreen(const std::string& protocol);

    /**
     * @brief Optional capability: is Gemini (Google AI API) reachable through this config?
     *        -1 unknown/error, 0 blocked, 1 accessible. Metadata only, never health.
     */
    int checkGeminiAccess(const std::string& config_uri, int timeout_seconds = 15);

private:
    std::string xray_path_;
    std::string singbox_path_;
    std::string mihomo_path_;
    std::shared_ptr<EngineLauncher> launcher_;
    bool custom_launcher_ = false;
    std::shared_ptr<TrafficProbe> probe_;
    std::shared_ptr<ConnectivityBaseline> baseline_;
    std::shared_ptr<PortLeaseRegistry> leases_;
    ClockFn clock_;

    void ensureDeps();
    struct Impl;
    friend struct Impl;
};

} // namespace network
} // namespace hunter
