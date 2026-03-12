#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <set>
#include <utility>

#include "core/models.h"
#include "proxy/runtime_engine_manager.h"

namespace hunter {
namespace proxy {

/**
 * @brief Multi-backend SOCKS5 proxy load balancer
 * 
 * Manages multiple XRay backend processes and distributes incoming
 * SOCKS5 connections using least-ping strategy. Supports health
 * monitoring, forced backend override, and hot config updates.
 */
class MultiProxyServer {
public:
    using StatusCallback = std::function<void(const BalancerStatus&)>;
    using EngineHintCallback = std::function<std::string(const std::string&)>;

    explicit MultiProxyServer(int port = 10808, int max_backends = 20);
    ~MultiProxyServer();

    MultiProxyServer(const MultiProxyServer&) = delete;
    MultiProxyServer& operator=(const MultiProxyServer&) = delete;

    /**
     * @brief Start the balancer server
     * @param initial_configs Initial backend configs [(uri, latency_ms)]
     * @return true on success
     */
    bool start(const std::vector<std::pair<std::string, float>>& initial_configs = {});

    /**
     * @brief Stop the balancer server
     */
    void stop();

    /**
     * @brief Check if running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Update available configs (hot reload)
     * @param configs New backend configs [(uri, latency_ms)]
     * @param trusted Whether these configs are benchmark-verified
     */
    void updateAvailableConfigs(const std::vector<std::pair<std::string, float>>& configs,
                                 bool trusted = false);

    /**
     * @brief Force a specific backend
     */
    bool setForcedBackend(const std::string& uri, bool permanent = false);

    /**
     * @brief Clear forced backend
     */
    void clearForcedBackend();

    /**
     * @brief Get current status
     */
    BalancerStatus getStatus() const;

    /**
     * @brief Get all backend details
     */
    std::vector<Backend> getAllBackends() const;

    /**
     * @brief Get available configs list for pool display
     */
    struct PoolEntry {
        std::string uri;
        std::string uri_short;
        float latency;
        std::string engine_used;
    };
    std::vector<PoolEntry> getAvailableConfigsList() const;

    /**
     * @brief Get working backends
     */
    std::vector<Backend> getWorkingBackends() const;

    /**
     * @brief Get port
     */
    int port() const { return port_; }

    /**
     * @brief Set status callback
     */
    void setStatusCallback(StatusCallback cb) { status_callback_ = cb; }

    void setXRayPath(const std::string& path) { xray_path_ = path; runtime_engine_manager_.setXRayPath(path); }
    void setSingBoxPath(const std::string& path) { runtime_engine_manager_.setSingBoxPath(path); }
    void setMihomoPath(const std::string& path) { runtime_engine_manager_.setMihomoPath(path); }
    void setEngineHintCallback(EngineHintCallback cb) { engine_hint_callback_ = std::move(cb); }

private:
    int port_;
    int max_backends_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    std::thread health_thread_;

    // Backends
    std::vector<Backend> backends_;
    std::vector<std::pair<std::string, float>> available_configs_;
    mutable std::mutex backends_mutex_;

    // Forced backend
    std::string forced_uri_;
    bool forced_permanent_ = false;

    // XRay process management
    struct XRayProcess {
        std::string uri;
        std::string engine_used;
        int socks_port = 0;
        int pid = 0;
        bool alive = false;
    };
    std::vector<XRayProcess> xray_processes_;
    std::string xray_path_ = "bin/xray.exe";
    RuntimeEngineManager runtime_engine_manager_;
    bool tcp_alive_ = false;
    bool socks_ready_ = false;
    bool http_ready_ = false;
    double last_probe_ts_ = 0.0;

    StatusCallback status_callback_;
    EngineHintCallback engine_hint_callback_;

    void serverLoop();
    void healthMonitorLoop();
    void refreshBackends();
    void refreshBackends_unlocked();
    bool startXRayProcess(const std::string& uri, int socks_port);
    void stopXRayProcess(int pid);
    std::string generateXRayConfig(const std::string& uri, int socks_port) const;
    std::string shortenUri(const std::string& uri) const;
};

} // namespace proxy
} // namespace hunter
