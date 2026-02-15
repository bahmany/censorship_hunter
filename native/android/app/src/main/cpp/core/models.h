#pragma once

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace hunter {

struct HunterParsedConfig {
    std::string uri;
    nlohmann::json outbound;
    std::string host;
    int port = 0;
    std::string identity;
    std::string ps;
};

struct HunterBenchResult {
    std::string uri;
    nlohmann::json outbound;
    std::string host;
    int port = 0;
    std::string identity;
    std::string ps;
    double latency_ms = 0.0;
    std::optional<std::string> ip;
    std::optional<std::string> country_code;
    std::string region;
    std::string tier;
};

struct ProxyStats {
    int total_configs = 0;
    int working_configs = 0;
    int gold_configs = 0;
    int silver_configs = 0;
    double average_latency = 0.0;
    std::optional<double> last_update;
};

struct GatewayStats {
    bool running = false;
    std::optional<std::string> current_config;
    int uptime = 0;
    int socks_port = 0;
    int http_port = 0;
    int dns_port = 0;
    int64_t requests = 0;
    int64_t bytes_sent = 0;
    int64_t bytes_received = 0;
};

struct BalancerStats {
    bool running = false;
    int port = 0;
    int backends = 0;
    int total_backends = 0;
    int restarts = 0;
    int health_checks = 0;
    int backend_swaps = 0;
    std::optional<double> last_restart;
};

} // namespace hunter
