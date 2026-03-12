#pragma once

#include <atomic>
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

private:
    struct ClientConn {
        intptr_t fd = -1;
        std::atomic<bool> alive{true};
        std::mutex write_mutex;
    };

    int control_port_ = 0;
    int monitor_port_ = 0;
    std::atomic<bool> running_{false};

    intptr_t control_listener_ = -1;
    intptr_t monitor_listener_ = -1;

    std::thread control_thread_;
    std::thread monitor_accept_thread_;
    std::thread monitor_publish_thread_;

    mutable std::mutex monitor_clients_mutex_;
    std::vector<std::shared_ptr<ClientConn>> monitor_clients_;

    CommandHandler command_handler_;
    StatusProvider status_provider_;
    LogsProvider logs_provider_;

    std::mutex recent_logs_mutex_;
    std::vector<std::string> recent_logs_cache_;

    bool startListener(int port, intptr_t& listener_fd);
    void controlLoop();
    void monitorAcceptLoop();
    void monitorPublishLoop();
    void handleControlClient(intptr_t client_fd);
    void removeDeadMonitorClients();

    bool performServerHandshake(intptr_t fd) const;
    bool readTextFrame(intptr_t fd, std::string& out) const;
    bool sendTextFrame(intptr_t fd, const std::string& payload) const;
    bool sendTextFrame(const std::shared_ptr<ClientConn>& client, const std::string& payload) const;
    void closeSocketFd(intptr_t fd) const;

    std::string makeEvent(const std::string& type, const std::string& raw_json) const;
    std::string makeLogEvent(const std::vector<std::string>& lines) const;
    static std::string httpWebSocketAcceptValue(const std::string& key);
};

} // namespace realtime
} // namespace hunter
