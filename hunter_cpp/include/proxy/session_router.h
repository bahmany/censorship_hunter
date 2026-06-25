#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "proxy/proxy_hosting_manager.h"

namespace hunter {
namespace proxy {

struct RoutingDecision {
    std::string instance_uri;
    std::string engine;
    int local_port = 0;
    float score = 0.0f;
    std::string reason;
};

struct SessionRoute {
    std::string session_id;
    std::string instance_uri;
    std::string engine;
    int local_port = 0;
    double assigned_ts = 0.0;
    double last_rebalance_ts = 0.0;
    int rebalance_count = 0;
};

class SessionRouterEngine {
public:
    SessionRouterEngine();
    ~SessionRouterEngine();

    SessionRouterEngine(const SessionRouterEngine&) = delete;
    SessionRouterEngine& operator=(const SessionRouterEngine&) = delete;

    RoutingDecision route(const std::string& session_id);
    RoutingDecision rebalance(const std::string& session_id);
    bool releaseSession(const std::string& session_id);

    void updateInstances(const std::vector<ProxyInstance>& instances);
    void recordSessionBytes(const std::string& session_id, uint64_t bytes);
    void recordSessionFailure(const std::string& session_id);
    void recordSessionSuccess(const std::string& session_id);

    std::vector<SessionRoute> getActiveRoutes() const;
    int getActiveRouteCount() const;

    struct RoutingMetrics {
        int total_routes = 0;
        int total_rebalances = 0;
        int active_routes = 0;
        float avg_score = 0.0f;
        int total_failures = 0;
        int total_successes = 0;
    };
    RoutingMetrics getMetrics() const;

private:
    mutable std::mutex routes_mutex_;
    std::map<std::string, SessionRoute> routes_;
    std::map<std::string, int> instance_load_;

    mutable std::mutex instances_mutex_;
    std::vector<ProxyInstance> instances_;

    std::atomic<int> total_routes_{0};
    std::atomic<int> total_rebalances_{0};
    std::atomic<int> total_failures_{0};
    std::atomic<int> total_successes_{0};

    float computeScore(const ProxyInstance& instance, int current_load) const;
    ProxyInstance findBestInstance() const;
};

} // namespace proxy
} // namespace hunter
