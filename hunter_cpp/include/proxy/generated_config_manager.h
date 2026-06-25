#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <functional>

namespace hunter {
namespace proxy {

struct GeneratedConfig {
    std::string name;           // Abhar-VMess-1, Abhar-Trojan-1, etc.
    std::string protocol;       // vmess, vless, trojan, ss, hysteria2
    std::string host;           // api.abharcable.com
    int port = 443;             // Always 443
    std::string uuid;           // UUID for vless/vmess
    std::string password;       // Password for trojan/ss/hysteria2
    std::string ws_path;        // WebSocket path (e.g., /xray/vmess/ or /ws/proxy/tunnel_v2_2/)
    std::string sni;            // SNI value
    std::string alpn;           // ALPN (e.g., http/1.1)
    std::string fingerprint;    // TLS fingerprint (e.g., chrome)
    std::string upstream_name;  // Which upstream tunnel this routes through
    int upstream_port = 0;      // Local SOCKS port of upstream
    bool active = false;
    double created_ts = 0.0;
    double last_health_check = 0.0;
    int consecutive_failures = 0;
    int total_health_checks = 0;
    int total_passes = 0;
    float latency_ms = 0.0f;
    uint64_t total_bytes = 0;
    int active_sessions = 0;
    std::string uri;            // Full config URI (vmess://, vless://, trojan://, ss://, hysteria2://)
};

class GeneratedConfigManager {
public:
    using StateChangeCallback = std::function<void(const std::vector<GeneratedConfig>&)>;
    using UpstreamProvider = std::function<std::vector<std::pair<std::string,int>>()>;
    using HostnameProvider = std::function<std::string()>;

    GeneratedConfigManager();
    ~GeneratedConfigManager();

    GeneratedConfigManager(const GeneratedConfigManager&) = delete;
    GeneratedConfigManager& operator=(const GeneratedConfigManager&) = delete;

    void setPublicHost(const std::string& host) { public_host_ = host; }
    void setPortBase(int base) { port_base_ = base; }
    void setConfigCount(int count) { config_count_ = count; }
    void setUpstreamProvider(UpstreamProvider cb) { upstream_provider_ = std::move(cb); }
    void setHostnameProvider(HostnameProvider cb) { hostname_provider_ = std::move(cb); }
    void setStateChangeCallback(StateChangeCallback cb) { state_cb_ = std::move(cb); }

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    std::vector<GeneratedConfig> getConfigs() const;
    std::string getConfigsJson() const;
    std::string getConfigsText() const;
    std::string getConfigByName(const std::string& name) const;

    int getActiveCount() const;
    int getTotalCount() const;
    double getLastGenerationTs() const { return last_generation_ts_.load(); }

private:
    std::atomic<bool> running_{false};
    std::thread manager_thread_;

    mutable std::mutex configs_mutex_;
    std::vector<GeneratedConfig> configs_;
    std::atomic<double> last_generation_ts_{0.0};

    std::string public_host_ = "api.abharcable.com";
    int port_base_ = 443;
    int config_count_ = 20;

    UpstreamProvider upstream_provider_;
    HostnameProvider hostname_provider_;
    StateChangeCallback state_cb_;

    void managerLoop();
    void generateConfigs();
    void checkHealth();
    void notifyStateChange();
    std::string formatConfigUri(const GeneratedConfig& cfg) const;
};

} // namespace proxy
} // namespace hunter
