#pragma once

#include <string>
#include <map>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>

namespace hunter {
namespace web {

/**
 * @brief Minimal embedded HTTP server for dashboard API
 * 
 * Single-threaded HTTP/1.1 server using raw sockets.
 * Supports GET/POST routes, JSON responses, and SSE log streaming.
 */
class HttpServer {
public:
    struct Request {
        std::string method;
        std::string path;
        std::string body;
        std::map<std::string, std::string> headers;
        std::map<std::string, std::string> query_params;
    };

    struct Response {
        int status_code = 200;
        std::string content_type = "application/json";
        std::string body;
        std::map<std::string, std::string> headers;

        static Response json(const std::string& json_body, int code = 200);
        static Response html(const std::string& html_body, int code = 200);
        static Response notFound();
        static Response error(const std::string& msg, int code = 500);
    };

    using RouteHandler = std::function<Response(const Request&)>;

    explicit HttpServer(int port = 8585);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /**
     * @brief Register a route handler
     */
    void route(const std::string& method, const std::string& path, RouteHandler handler);

    /**
     * @brief Convenience: register GET route
     */
    void get(const std::string& path, RouteHandler handler);

    /**
     * @brief Convenience: register POST route
     */
    void post(const std::string& path, RouteHandler handler);

    /**
     * @brief Start the server (non-blocking)
     */
    bool start();

    /**
     * @brief Stop the server
     */
    void stop();

    /**
     * @brief Check if running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Get port
     */
    int port() const { return port_; }

    /**
     * @brief Push a log entry for SSE streaming
     */
    void pushLog(const std::string& level, const std::string& message);

    /**
     * @brief Get log entries as JSON array (for SSE/polling)
     */
    std::string getLogStreamJson(int since_index);

private:
    int port_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    struct Route {
        std::string method;
        std::string path;
        RouteHandler handler;
    };
    std::vector<Route> routes_;
    std::mutex routes_mutex_;

    // Log buffer for SSE
    struct LogEntry {
        double timestamp;
        std::string level;
        std::string message;
    };
    std::vector<LogEntry> log_buffer_;
    std::mutex log_mutex_;
    static constexpr int MAX_LOG_BUFFER = 500;

    void serverLoop();
    void handleClient(int client_fd);
    Request parseRequest(const std::string& raw);
    std::string buildResponse(const Response& resp);
    Response routeRequest(const Request& req);
};

} // namespace web
} // namespace hunter
