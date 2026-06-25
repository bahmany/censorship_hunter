#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hunter {
namespace proxy {

struct GatewaySession {
    std::string session_id;
    std::string user_token;
    std::string assigned_uri;
    std::string assigned_engine;
    int assigned_port = 0;
    double created_ts = 0.0;
    double last_activity_ts = 0.0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    bool active = true;
};

struct GatewayMetrics {
    int active_sessions = 0;
    int total_sessions = 0;
    int live_proxy_instances = 0;
    int routing_distribution_count = 0;
    double avg_latency_ms = 0.0;
    uint64_t total_bytes_proxied = 0;
    int fallback_events = 0;
};

struct ProxyPoolEntry {
    std::string uri;
    std::string engine;
    int local_port = 0;
    float latency_ms = 0.0f;
    bool healthy = false;
    int active_sessions = 0;
    double last_health_check = 0.0;
};

class ProxyGateway {
public:
    using PoolUpdateCallback = std::function<std::vector<ProxyPoolEntry>()>;
    using MetricsCallback = std::function<void(const GatewayMetrics&)>;

    explicit ProxyGateway(int http_port = 7805);
    ~ProxyGateway();

    ProxyGateway(const ProxyGateway&) = delete;
    ProxyGateway& operator=(const ProxyGateway&) = delete;

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void setPoolUpdateCallback(PoolUpdateCallback cb) { pool_callback_ = std::move(cb); }
    void setMetricsCallback(MetricsCallback cb) { metrics_callback_ = std::move(cb); }

    void updateProxyPool(const std::vector<ProxyPoolEntry>& entries);

    GatewayMetrics getMetrics() const;
    std::vector<GatewaySession> getActiveSessions() const;
    std::vector<ProxyPoolEntry> getProxyPool() const;

    int port() const { return http_port_; }

private:
    int http_port_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::thread health_thread_;

    mutable std::mutex sessions_mutex_;
    std::map<std::string, GatewaySession> sessions_;

    mutable std::mutex pool_mutex_;
    std::vector<ProxyPoolEntry> proxy_pool_;

    PoolUpdateCallback pool_callback_;
    MetricsCallback metrics_callback_;

    std::atomic<int> total_sessions_{0};
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<int> fallback_events_{0};

    void serverLoop();
    void healthMonitorLoop();
    void handleClient(int client_fd);
    void handleApiRequest(int client_fd, const std::string& method,
                          const std::string& path, const std::string& body,
                          const std::string& auth_header);
    void handleProxyConnect(int client_fd, const std::string& target_host, int target_port,
                            const std::string& session_id);

    std::string createSession(const std::string& user_token);
    bool destroySession(const std::string& session_id);
    GatewaySession* findSession(const std::string& session_id);
    void touchSession(const std::string& session_id);

    ProxyPoolEntry selectBestProxy();
    void refreshProxyPool();

    std::string buildStatusJson() const;
    std::string buildSessionsJson() const;
    std::string buildPoolJson() const;

    void sendHttpResponse(int fd, int status_code, const std::string& content_type,
                          const std::string& body);
    void sendJsonResponse(int fd, int status_code, const std::string& json);
    void sendErrorResponse(int fd, int status_code, const std::string& message);

    static std::string generateSessionId();
    static std::string urlDecode(const std::string& str);
    static std::string parsePath(const std::string& request_line);
    static std::string parseMethod(const std::string& request_line);
};

} // namespace proxy
} // namespace hunter
