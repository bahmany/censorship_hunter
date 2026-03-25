#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <chrono>
#include <mutex>
#include <atomic>

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
    float ram_percent = 50.0f;
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
    std::map<std::string, std::string> extra;  // Extra params

    static bool hasBadChars_(const std::string& s) {
        for (unsigned char c : s) {
            if (c < 0x20 || c == '"' || c == '\\' || c == 0x7f) return true;
        }
        return false;
    }
    bool isValid() const {
        if (protocol.empty() || address.empty() || port < 1 || port > 65535) return false;
        if (hasBadChars_(address) || hasBadChars_(uuid) || hasBadChars_(sni) ||
            hasBadChars_(host) || hasBadChars_(encryption)) return false;
        if (address.size() > 253 || uuid.size() > 512) return false;
        return true;
    }
    bool isReality() const { return security == "reality"; }
    bool isTLS() const { return security == "tls"; }
    bool isCDN() const {
        return network == "ws" || network == "grpc" || network == "splithttp" || network == "httpupgrade";
    }

    // Generate XRay-compatible JSON outbound
    std::string toXrayOutboundJson(int socks_port) const;
    
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
    std::string tier;             // "gold", "silver", "dead"
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
    std::string uri_hash;         // SHA1 of URI
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
