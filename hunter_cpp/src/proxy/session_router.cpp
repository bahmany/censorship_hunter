#include "proxy/session_router.h"
#include "core/utils.h"

#include <algorithm>
#include <cmath>

namespace hunter {
namespace proxy {

SessionRouterEngine::SessionRouterEngine() {}

SessionRouterEngine::~SessionRouterEngine() = default;

float SessionRouterEngine::computeScore(const ProxyInstance& instance, int current_load) const {
    if (!instance.healthy) return -1.0f;

    float latency_score = 1000.0f / (instance.latency_ms + 10.0f);
    float load_score = 100.0f / (static_cast<float>(current_load) + 1.0f);
    float health_score = instance.success_rate * 100.0f;
    float stability_score = static_cast<float>(instance.engine_stability_score);

    return latency_score * 0.35f + load_score * 0.25f + health_score * 0.25f + stability_score * 0.15f;
}

ProxyInstance SessionRouterEngine::findBestInstance() const {
    ProxyInstance best;
    float best_score = -1.0f;

    for (const auto& inst : instances_) {
        if (!inst.healthy) continue;
        int load = 0;
        {
            auto it = instance_load_.find(inst.uri);
            if (it != instance_load_.end()) load = it->second;
        }
        float score = computeScore(inst, load);
        if (score > best_score) {
            best_score = score;
            best = inst;
        }
    }

    return best;
}

RoutingDecision SessionRouterEngine::route(const std::string& session_id) {
    RoutingDecision decision;

    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        auto it = routes_.find(session_id);
        if (it != routes_.end()) {
            std::lock_guard<std::mutex> ilock(instances_mutex_);
            for (const auto& inst : instances_) {
                if (inst.uri == it->second.instance_uri && inst.healthy) {
                    decision.instance_uri = inst.uri;
                    decision.engine = inst.engine;
                    decision.local_port = inst.local_port;
                    decision.score = computeScore(inst, instance_load_[inst.uri]);
                    decision.reason = "existing_route";
                    return decision;
                }
            }
        }
    }

    ProxyInstance best;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        best = findBestInstance();
    }

    if (best.uri.empty()) {
        decision.reason = "no_healthy_instances";
        return decision;
    }

    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        SessionRoute route;
        route.session_id = session_id;
        route.instance_uri = best.uri;
        route.engine = best.engine;
        route.local_port = best.local_port;
        route.assigned_ts = utils::nowTimestamp();
        route.last_rebalance_ts = route.assigned_ts;
        routes_[session_id] = route;
        instance_load_[best.uri]++;
    }

    total_routes_.fetch_add(1);
    decision.instance_uri = best.uri;
    decision.engine = best.engine;
    decision.local_port = best.local_port;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        decision.score = computeScore(best, instance_load_[best.uri]);
    }
    decision.reason = "new_route";
    return decision;
}

RoutingDecision SessionRouterEngine::rebalance(const std::string& session_id) {
    RoutingDecision decision;

    std::string old_uri;
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        auto it = routes_.find(session_id);
        if (it == routes_.end()) {
            return route(session_id);
        }
        old_uri = it->second.instance_uri;
    }

    ProxyInstance best;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        best = findBestInstance();
    }

    if (best.uri.empty() || best.uri == old_uri) {
        {
            std::lock_guard<std::mutex> lock(instances_mutex_);
            for (const auto& inst : instances_) {
                if (inst.uri == old_uri) {
                    decision.instance_uri = inst.uri;
                    decision.engine = inst.engine;
                    decision.local_port = inst.local_port;
                    decision.score = computeScore(inst, 0);
                    break;
                }
            }
        }
        decision.reason = "no_better_instance";
        return decision;
    }

    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        auto it = routes_.find(session_id);
        if (it != routes_.end()) {
            if (instance_load_[old_uri] > 0) instance_load_[old_uri]--;
            it->second.instance_uri = best.uri;
            it->second.engine = best.engine;
            it->second.local_port = best.local_port;
            it->second.last_rebalance_ts = utils::nowTimestamp();
            it->second.rebalance_count++;
            instance_load_[best.uri]++;
        }
    }

    total_rebalances_.fetch_add(1);
    decision.instance_uri = best.uri;
    decision.engine = best.engine;
    decision.local_port = best.local_port;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        decision.score = computeScore(best, instance_load_[best.uri]);
    }
    decision.reason = "rebalanced";
    return decision;
}

bool SessionRouterEngine::releaseSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auto it = routes_.find(session_id);
    if (it == routes_.end()) return false;
    if (instance_load_[it->second.instance_uri] > 0) {
        instance_load_[it->second.instance_uri]--;
    }
    routes_.erase(it);
    return true;
}

void SessionRouterEngine::updateInstances(const std::vector<ProxyInstance>& instances) {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    instances_ = instances;
}

void SessionRouterEngine::recordSessionBytes(const std::string& session_id, uint64_t bytes) {
    (void)session_id;
    (void)bytes;
}

void SessionRouterEngine::recordSessionFailure(const std::string& session_id) {
    total_failures_.fetch_add(1);
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auto it = routes_.find(session_id);
    if (it != routes_.end()) {
        instance_load_[it->second.instance_uri] =
            std::max(0, instance_load_[it->second.instance_uri] - 1);
    }
}

void SessionRouterEngine::recordSessionSuccess(const std::string& session_id) {
    total_successes_.fetch_add(1);
    (void)session_id;
}

std::vector<SessionRoute> SessionRouterEngine::getActiveRoutes() const {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    std::vector<SessionRoute> result;
    result.reserve(routes_.size());
    for (const auto& [id, route] : routes_) {
        result.push_back(route);
    }
    return result;
}

int SessionRouterEngine::getActiveRouteCount() const {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    return static_cast<int>(routes_.size());
}

SessionRouterEngine::RoutingMetrics SessionRouterEngine::getMetrics() const {
    RoutingMetrics m;
    m.total_routes = total_routes_.load();
    m.total_rebalances = total_rebalances_.load();
    m.active_routes = getActiveRouteCount();
    m.total_failures = total_failures_.load();
    m.total_successes = total_successes_.load();

    float total_score = 0;
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        for (const auto& inst : instances_) {
            if (inst.healthy) {
                int load = 0;
                std::lock_guard<std::mutex> rlock(routes_mutex_);
                auto it = instance_load_.find(inst.uri);
                if (it != instance_load_.end()) load = it->second;
                total_score += computeScore(inst, load);
                count++;
            }
        }
    }
    m.avg_score = count > 0 ? total_score / count : 0.0f;
    return m;
}

} // namespace proxy
} // namespace hunter
