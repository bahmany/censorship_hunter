#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/models.h"

namespace hunter {
namespace proxy {

struct ProxyInstance {
    std::string uri;
    std::string engine;
    int local_port = 0;
    float latency_ms = 0.0f;
    bool healthy = false;
    double promoted_ts = 0.0;
    double last_health_check = 0.0;
    int consecutive_failures = 0;
    int total_sessions_routed = 0;
    uint64_t total_bytes = 0;
    float success_rate = 1.0f;
    int engine_stability_score = 100;
};

class ProxyHostingManager {
public:
    using HealthyConfigsProvider = std::function<std::vector<ConfigHealthRecord>(int)>;
    using EngineHintProvider = std::function<std::string(const std::string&)>;
    using BalancerUpdateCallback = std::function<void(const std::vector<std::pair<std::string, float>>&)>;
    using InstanceStateCallback = std::function<void(const std::vector<ProxyInstance>&)>;

    ProxyHostingManager();
    ~ProxyHostingManager();

    ProxyHostingManager(const ProxyHostingManager&) = delete;
    ProxyHostingManager& operator=(const ProxyHostingManager&) = delete;

    void setHealthyConfigsProvider(HealthyConfigsProvider cb) { healthy_provider_ = std::move(cb); }
    void setEngineHintProvider(EngineHintProvider cb) { engine_hint_ = std::move(cb); }
    void setBalancerUpdateCallback(BalancerUpdateCallback cb) { balancer_cb_ = std::move(cb); }
    void setInstanceStateCallback(InstanceStateCallback cb) { state_cb_ = std::move(cb); }

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    std::vector<ProxyInstance> getInstances() const;
    ProxyInstance getBestInstance() const;
    int getLiveCount() const;
    int getTotalCount() const;

    void promoteConfigs(const std::vector<ConfigHealthRecord>& records);
    void recycleInstance(const std::string& uri);
    void markInstanceHealth(const std::string& uri, bool healthy, float latency_ms);

private:
    std::atomic<bool> running_{false};
    std::thread manager_thread_;

    mutable std::mutex instances_mutex_;
    std::map<std::string, ProxyInstance> instances_;

    HealthyConfigsProvider healthy_provider_;
    EngineHintProvider engine_hint_;
    BalancerUpdateCallback balancer_cb_;
    InstanceStateCallback state_cb_;

    std::atomic<int> total_promoted_{0};
    std::atomic<int> total_recycled_{0};

    void managerLoop();
    void refreshFromConfigDb();
    void checkInstanceHealth();
    void updateBalancer();
    void notifyStateChange();
};

} // namespace proxy
} // namespace hunter
