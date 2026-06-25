#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hunter {
namespace proxy {

struct UpstreamTunnelConfig {
    std::string name;
    std::string host;
    int ssh_port = 22;
    std::string user;
    std::string identity_file;
    int local_socks_port = 0;
    bool preferred = true;
    int reconnect_interval_s = 5;
    int keepalive_interval_s = 30;
    int keepalive_count_max = 3;
    std::string compression = "zlib";
};

struct UpstreamTunnelState {
    std::string name;
    std::string host;
    int local_socks_port = 0;
    bool connected = false;
    bool connecting = false;
    int pid = -1;
    double connected_since = 0.0;
    double last_disconnect_ts = 0.0;
    int reconnect_count = 0;
    int total_bytes_proxied = 0;
    float latency_ms = 0.0f;
    int consecutive_failures = 0;
    std::string last_error;
    bool preferred = true;
};

class UpstreamTunnelManager {
public:
    using StateChangeCallback = std::function<void(const std::vector<UpstreamTunnelState>&)>;
    using MetricsCallback = std::function<void(const std::string& json)>;

    UpstreamTunnelManager();
    ~UpstreamTunnelManager();

    UpstreamTunnelManager(const UpstreamTunnelManager&) = delete;
    UpstreamTunnelManager& operator=(const UpstreamTunnelManager&) = delete;

    void addUpstream(const UpstreamTunnelConfig& config);
    void removeUpstream(const std::string& name);

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    std::vector<UpstreamTunnelState> getStates() const;
    UpstreamTunnelState getState(const std::string& name) const;
    int getConnectedCount() const;
    int getTotalCount() const;

    void setStateChangeCallback(StateChangeCallback cb) { state_cb_ = std::move(cb); }
    void setMetricsCallback(MetricsCallback cb) { metrics_cb_ = std::move(cb); }

    std::vector<std::pair<std::string, int>> getConnectedSocksEndpoints() const;
    std::string getBestSocksEndpoint() const;

private:
    std::atomic<bool> running_{false};
    std::thread manager_thread_;

    mutable std::mutex tunnels_mutex_;
    std::map<std::string, UpstreamTunnelConfig> configs_;
    std::map<std::string, UpstreamTunnelState> states_;
    std::map<std::string, std::thread> tunnel_threads_;

    StateChangeCallback state_cb_;
    MetricsCallback metrics_cb_;

    std::atomic<int> total_reconnects_{0};

    void managerLoop();
    void startTunnel(const std::string& name);
    void stopTunnel(const std::string& name);
    void tunnelLoop(const std::string& name);
    bool checkSocksPort(int port);
    float measureLatency(int port);
    void notifyStateChange();
    std::string buildMetricsJson() const;
};

} // namespace proxy
} // namespace hunter
