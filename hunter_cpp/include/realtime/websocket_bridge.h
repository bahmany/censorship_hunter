#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hunter {
namespace realtime {

class WebSocketBridge {
public:
    using CommandHandler = std::function<std::string(const std::string&)>;
    using StatusProvider = std::function<std::string()>;
    using LogsProvider = std::function<std::vector<std::string>()>;

    WebSocketBridge(int control_port, int monitor_port);
    ~WebSocketBridge();

    WebSocketBridge(const WebSocketBridge&) = delete;
    WebSocketBridge& operator=(const WebSocketBridge&) = delete;

    void setCommandHandler(CommandHandler handler);
    void setStatusProvider(StatusProvider provider);
    void setLogsProvider(LogsProvider provider);

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void broadcastMonitorJson(const std::string& json_payload);
    void broadcastMonitorEvent(const std::string& type, const std::string& raw_json);

private:
    struct ClientConn {
        intptr_t fd = -1;
        std::atomic<bool> alive{true};
        std::mutex write_mutex;
        std::chrono::steady_clock::time_point last_activity;
        std::chrono::steady_clock::time_point connected_at;
    };

    struct ConnectionStats {
        std::atomic<int> active_monitor_clients{0};
        std::atomic<int> active_control_clients{0};
        std::atomic<int> total_connections{0};
        std::atomic<int> total_disconnections{0};
        std::atomic<int> total_reconnects{0};
        std::atomic<int> messages_sent{0};
        std::atomic<int> messages_received{0};
        std::atomic<int> pings_sent{0};
        std::atomic<int> pongs_received{0};
    };

    int control_port_ = 0;
    int monitor_port_ = 0;
    std::atomic<bool> running_{false};

    intptr_t control_listener_ = -1;
    intptr_t monitor_listener_ = -1;

    std::thread control_thread_;
    std::thread monitor_accept_thread_;
    std::thread monitor_publish_thread_;
    std::thread heartbeat_thread_;

    mutable ConnectionStats stats_;

    mutable std::mutex monitor_clients_mutex_;
    std::vector<std::shared_ptr<ClientConn>> monitor_clients_;

    CommandHandler command_handler_;
    StatusProvider status_provider_;
    LogsProvider logs_provider_;

    size_t log_since_ = 0; // tracks last-sent ring-buffer generation for incremental log streaming

    bool startListener(int port, intptr_t& listener_fd);
    void controlLoop();
    void monitorAcceptLoop();
    void monitorPublishLoop();
    void heartbeatLoop();
    void handleControlClient(intptr_t client_fd);
    void removeDeadMonitorClients();

    bool performServerHandshake(intptr_t fd) const;
    bool readFrame(intptr_t fd, std::string& out, uint8_t& opcode) const;
    bool readTextFrame(intptr_t fd, std::string& out) const;
    bool sendTextFrame(intptr_t fd, const std::string& payload) const;
    bool sendPingFrame(intptr_t fd) const;
    bool sendPongFrame(intptr_t fd, const std::string& payload) const;
    bool sendTextFrame(const std::shared_ptr<ClientConn>& client, const std::string& payload) const;
    bool sendPingFrame(const std::shared_ptr<ClientConn>& client) const;
    void closeSocketFd(intptr_t fd) const;

    std::string makeEvent(const std::string& type, const std::string& raw_json) const;
    std::string makeLogEvent(const std::vector<std::string>& lines) const;
    std::string makeStatsJson() const;
    static std::string httpWebSocketAcceptValue(const std::string& key);

public:
    std::string getStatsJson() const;
    int getActiveMonitorClientCount() const;
};

bool broadcastGlobalMonitorEvent(const std::string& type, const std::string& raw_json);

} // namespace realtime
} // namespace hunter
