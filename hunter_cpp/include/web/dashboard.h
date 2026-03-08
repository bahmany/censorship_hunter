#pragma once

#include <string>
#include <functional>
#include <memory>

#include "web/http_server.h"
#include "core/models.h"

namespace hunter {

class HunterOrchestrator;

namespace web {

/**
 * @brief Web dashboard — registers all API routes and serves the HTML UI
 * 
 * Provides REST API endpoints for the real-time dashboard:
 *   /              → HTML dashboard
 *   /api/status    → Overall status
 *   /api/threads/status → Worker thread status
 *   /api/balancer/details → Balancer backend info
 *   /api/logs/stream → SSE log streaming
 *   /api/command   → CLI commands (force_cycle, clear_pid, etc.)
 */
class Dashboard {
public:
    Dashboard(HunterOrchestrator* orch, int port = 8585);
    ~Dashboard();

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    /**
     * @brief Start the dashboard server
     */
    bool start();

    /**
     * @brief Stop the dashboard server
     */
    void stop();

    /**
     * @brief Push a log entry
     */
    void pushLog(const std::string& level, const std::string& message);

    /**
     * @brief Check if running
     */
    bool isRunning() const;

private:
    HunterOrchestrator* orch_;
    std::unique_ptr<HttpServer> server_;

    void registerRoutes();

    // Route handlers
    HttpServer::Response handleIndex(const HttpServer::Request& req);
    HttpServer::Response handleApiStatus(const HttpServer::Request& req);
    HttpServer::Response handleApiThreadsStatus(const HttpServer::Request& req);
    HttpServer::Response handleApiBalancerDetails(const HttpServer::Request& req);
    HttpServer::Response handleApiBalancerForce(const HttpServer::Request& req);
    HttpServer::Response handleApiBalancerUnforce(const HttpServer::Request& req);
    HttpServer::Response handleApiBalancerSetBackends(const HttpServer::Request& req);
    HttpServer::Response handleApiCommand(const HttpServer::Request& req);
    HttpServer::Response handleApiLogStream(const HttpServer::Request& req);
    HttpServer::Response handleApiTelegramStatus(const HttpServer::Request& req);
    HttpServer::Response handleApiAliveConfigs(const HttpServer::Request& req);

    // Generate the full HTML dashboard
    static std::string generateDashboardHtml();
};

} // namespace web
} // namespace hunter
