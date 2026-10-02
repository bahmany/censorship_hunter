#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <chrono>
#include <mutex>
#include <atomic>

#include "core/health_score.h"

namespace hunter {

/**
 * @brief Resource usage mode based on system memory pressure
 */
enum class ResourceMode {
    NORMAL,
    MODERATE,
    SCALED,
    CONSERVATIVE,
    REDUCED,
    MINIMAL,
    ULTRA_MINIMAL
};

/**
 * @brief Point-in-time snapshot of system hardware resources
 */
struct HardwareSnapshot {
    int cpu_count = 4;
    float cpu_percent = 0.0f;
    float ram_total_gb = 8.0f;
    float ram_used_gb = 4.0f;
    float ram_free_gb = 4.0f;
    float ram_percent = 50.0f;
    // Budget: 20% of free RAM/CPU — keeps the app from starving the system
    float ram_budget_gb = 0.8f;   // 20% of free RAM
    float cpu_budget_cores = 1.0f; // 20% of free CPU cores
    ResourceMode mode = ResourceMode::NORMAL;
    int io_pool_size = 12;
    int cpu_pool_size = 4;
    int max_configs = 1000;
    int scan_chunk = 50;

    static HardwareSnapshot detect();
};

/**
 * @brief Parsed proxy configuration from a URI
 */
struct ParsedConfig {
    std::string uri;              // Original URI
    std::string protocol;         // vmess, vless, trojan, ss, etc.
    std::string address;          // Server address
    int port = 0;                 // Server port
    std::string uuid;             // User ID / password
    std::string encryption;       // Encryption method
    std::string network;          // tcp, ws, grpc, h2, splithttp
    std::string security;         // tls, reality, none
    std::string sni;              // Server Name Indication
    std::string path;             // WebSocket/gRPC path
    std::string host;             // Host header
    std::string fingerprint;      // uTLS fingerprint
    std::string public_key;       // Reality public key
    std::string short_id;         // Reality short ID
    std::string flow;             // XTLS flow (xtls-rprx-vision)
    std::string ps;               // Remark/name
    std::string type;             // Header type (http, none)
    std::map<std::string, std::string> extra;  // Extra params (serviceName, plugin, obfs, ...)
    std::map<std::string, std::string> options;  // FULL option map as parsed (query / vmess JSON); nothing dropped

    static bool hasBadChars_(const std::string& s) {
        for (unsigned char c : s) {
            if (c < 0x20 || c == '"' || c == '\\' || c == 0x7f) return true;
        }
        return false;
    }
    bool isValid() const {
        if (protocol.empty() || address.empty() || port < 1 || port > 65535) return false;
        // Every field spliced into generated JSON must be free of quotes/backslashes/control chars.
        if (hasBadChars_(address) || hasBadChars_(uuid) || hasBadChars_(sni) ||
            hasBadChars_(host) || hasBadChars_(encryption) || hasBadChars_(path) ||
            hasBadChars_(fingerprint) || hasBadChars_(public_key) || hasBadChars_(short_id) ||
            hasBadChars_(flow) || hasBadChars_(type) || hasBadChars_(network) || hasBadChars_(security))
            return false;
        for (const auto& kv : extra) if (hasBadChars_(kv.second)) return false;
        for (const char* k : {"alpn", "obfs", "obfs-password", "pinSHA256", "congestion_control", "udp_relay_mode"}) {
            auto it = options.find(k);
            if (it != options.end() && hasBadChars_(it->second)) return false;
        }
        if (address.size() > 253 || uuid.size() > 512) return false;
        if ((protocol == "vmess" || protocol == "vless") && uuid.empty()) return false;
        return true;
    }
    /// True when the link explicitly asks to skip upstream certificate verification
    /// (allowInsecure / insecure / skip-cert-verify aliases). Default is verified TLS.
    bool insecureTls() const {
        for (const char* k : {"allowInsecure", "insecure", "allow_insecure", "skip-cert-verify", "skip_cert_verify"}) {
            auto it = options.find(k);
            if (it != options.end() && (it->second == "1" || it->second == "true" || it->second == "True")) return true;
        }
        return false;
    }
    std::string option(const char* key, const std::string& def = "") const {
        auto it = options.find(key);
        if (it != options.end()) return it->second;
        it = extra.find(key);
        return it == extra.end() ? def : it->second;
    }
    std::string grpcServiceName() const {
        auto it = extra.find("serviceName");
        if (it != extra.end() && !it->second.empty()) return it->second;
        return path;
    }
    bool isReality() const { return security == "reality"; }
    bool isTLS() const { return security == "tls"; }
    bool isCDN() const {
        return network == "ws" || network == "grpc" || network == "splithttp" || network == "httpupgrade";
    }

    // Generate XRay-compatible JSON outbound
    /// Engine capability gate ("xray" | "sing-box" | "mihomo"). Returns "" when this engine can
    /// represent EVERY requested policy of the link (transport, plugin, pin, obfs, ...), otherwise a
    /// human reason. Generators return "" for unrepresentable links: no silent substitution.
    std::string unsupportedReason(const std::string& engine) const;
    std::string toXrayOutboundJson(int socks_port) const;
    
    // Generate full Xray config JSON with SOCKS inbound
    std::string toXrayConfigJson(int socks_port) const;
    
    // Generate full sing-box config JSON with SOCKS inbound
    std::string toSingBoxConfigJson(int socks_port) const;
    
    // Generate full mihomo (Clash Meta) config YAML with SOCKS inbound
    std::string toMihomoConfigYaml(int socks_port) const;
};

/**
 * @brief Benchmark result for a single config test
 */
struct BenchResult {
    std::string uri;
    float latency_ms = 0.0f;
    bool success = false;
    std::string tier;             // "gold","silver" on pass; else "dead"/"unstable" (Stability verdict) or "untested" (excluded/insufficient evidence)
    std::string ps;               // Config remark
    std::string protocol;
    std::string engine_used;
    std::string error;
    bool telegram_only = false;

    bool isGold() const { return tier == "gold"; }
    bool isSilver() const { return tier == "silver"; }
};

/**
 * @brief Health record for continuous config validation
 */
struct ConfigHealthRecord {
    std::string uri;
    std::string uri_hash;         // == endpoint_key (kept for source compatibility)
    std::string endpoint_key;     // EndpointKeyV1 ("ek1:<sha256>")
    int key_version = 1;
    std::string tag;              // Source tag (scrape, github_bg, harvest)
    std::string engine_used;
    double first_seen = 0.0;
    double priority_boost_until = 0.0;
    double last_tested = 0.0;
    double last_alive_time = 0.0; // When config was last confirmed alive
    bool alive = false;
    bool telegram_only = false;
    float latency_ms = 0.0f;
    int consecutive_fails = 0;
    int total_tests = 0;
    int total_passes = 0;
    bool needs_retest = true;
    // Gemini (Google AI) accessibility through this proxy.
    // -1 = unknown/not checked, 0 = blocked, 1 = accessible
    int gemini_status = -1;
    double gemini_checked_at = 0.0;  // Timestamp of last Gemini check

    // ── Stage 4 / D2: typed health evidence (source of truth). The legacy fields above
    // (alive, latency_ms, consecutive_fails, last_alive_time, ...) are derived from it
    // by ConfigDatabase and are hints only for records without evidence.
    HealthEvidence ev;
    // Session-only scheduling state (not persisted)
    double next_retry_at = 0.0;   // backoff after excluded (infrastructure/local) rounds
    int excluded_streak = 0;
    int legacy_fails = 0;         // consecutive failures reported through the legacy adapter
    // Country (D4 columns; written by later batches via applyCountryResult)
    std::vector<std::string> server_ips;
    std::string server_country;
    std::string server_country_source;
    double server_country_at = 0.0;
    std::string geo_db_version;
    std::string exit_ip;
    std::string exit_country;
    std::string exit_country_source;
    double exit_country_at = 0.0;
    uint64_t network_generation = 0;
};

/**
 * @brief Worker thread state
 */
enum class WorkerState {
    IDLE,
    RUNNING,
    SLEEPING,
    WORKER_ERROR,
    STOPPED
};

/**
 * @brief Status of a single worker thread
 */
struct WorkerStatus {
    std::string name;
    WorkerState state = WorkerState::IDLE;
    double last_run = 0.0;
    std::string last_error;
    int runs = 0;
    int errors = 0;
    double next_run_in = 0.0;
    std::map<std::string, std::string> extra;
};

/**
 * @brief Connection state for proxy backends
 */
enum class BackendState {
    HEALTHY,
    DEGRADED,
    DEAD,
    UNKNOWN
};

/**
 * @brief A proxy backend in the load balancer
 */
struct Backend {
    std::string uri;
    float latency_ms = 0.0f;
    BackendState state = BackendState::UNKNOWN;
    int consecutive_fails = 0;
    double last_check = 0.0;
    bool trusted = false;
    std::string engine_used;
    int local_port = 0;
};

/**
 * @brief Balancer status snapshot
 */
struct BalancerStatus {
    int port = 0;
    bool running = false;
    int backend_count = 0;
    int healthy_count = 0;
    bool tcp_alive = false;
    bool socks_ready = false;
    bool http_ready = false;
    double last_probe_ts = 0.0;
    std::string forced_uri;
    std::vector<Backend> backends;
};

/**
 * @brief DPI evasion strategy
 */
enum class DpiStrategy {
    NONE,
    SPLITHTTP_CDN,
    REALITY_DIRECT,
    WEBSOCKET_CDN,
    GRPC_CDN,
    HYSTERIA2
};

/**
 * @brief Network type detection
 */
enum class NetworkType {
    UNKNOWN,
    WIFI,
    MOBILE_4G,
    MOBILE_5G,
    ETHERNET,
    CENSORED
};

/**
 * @brief Fetch result from a config source
 */
struct FetchResult {
    std::string source;
    std::set<std::string> configs;
    bool success = false;
    double duration_ms = 0.0;
    std::string error;
};

} // namespace hunter
