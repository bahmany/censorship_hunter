#include "proxy/proxy_hosting_manager.h"
#include "core/utils.h"

#include <algorithm>
#include <iostream>

namespace hunter {
namespace proxy {

ProxyHostingManager::ProxyHostingManager() {}

ProxyHostingManager::~ProxyHostingManager() {
    stop();
}

void ProxyHostingManager::start() {
    if (running_.load()) return;
    running_ = true;
    manager_thread_ = std::thread(&ProxyHostingManager::managerLoop, this);
    std::cout << "[HostingManager] Started" << std::endl;
}

void ProxyHostingManager::stop() {
    if (!running_.exchange(false)) return;
    if (manager_thread_.joinable()) manager_thread_.join();
}

void ProxyHostingManager::managerLoop() {
    while (running_.load()) {
        refreshFromConfigDb();
        checkInstanceHealth();
        updateBalancer();
        notifyStateChange();

        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }
}

void ProxyHostingManager::refreshFromConfigDb() {
    if (!healthy_provider_) return;

    auto records = healthy_provider_(50);
    if (records.empty()) return;

    promoteConfigs(records);
}

void ProxyHostingManager::promoteConfigs(const std::vector<ConfigHealthRecord>& records) {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    double now = utils::nowTimestamp();

    for (const auto& rec : records) {
        auto it = instances_.find(rec.uri);
        if (it == instances_.end()) {
            ProxyInstance inst;
            inst.uri = rec.uri;
            inst.engine = rec.engine_used;
            if (inst.engine.empty() && engine_hint_) {
                inst.engine = engine_hint_(rec.uri);
            }
            inst.latency_ms = rec.latency_ms;
            inst.healthy = rec.alive;
            inst.promoted_ts = now;
            inst.last_health_check = now;
            inst.success_rate = rec.total_tests > 0
                ? static_cast<float>(rec.total_passes) / rec.total_tests
                : 1.0f;
            instances_[rec.uri] = inst;
            total_promoted_.fetch_add(1);
        } else {
            it->second.latency_ms = rec.latency_ms;
            it->second.healthy = rec.alive;
            it->second.last_health_check = now;
            if (!rec.engine_used.empty()) {
                it->second.engine = rec.engine_used;
            }
            it->second.success_rate = rec.total_tests > 0
                ? static_cast<float>(rec.total_passes) / rec.total_tests
                : it->second.success_rate;
            if (rec.alive) {
                it->second.consecutive_failures = 0;
            }
        }
    }
}

void ProxyHostingManager::checkInstanceHealth() {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    double now = utils::nowTimestamp();

    std::vector<std::string> to_recycle;
    for (auto& [uri, inst] : instances_) {
        double age = now - inst.last_health_check;
        if (!inst.healthy) {
            inst.consecutive_failures++;
            if (inst.consecutive_failures > 5) {
                to_recycle.push_back(uri);
            }
        }
        if (age > 300.0 && inst.consecutive_failures > 3) {
            to_recycle.push_back(uri);
        }
    }

    for (const auto& uri : to_recycle) {
        instances_.erase(uri);
        total_recycled_.fetch_add(1);
    }
}

void ProxyHostingManager::updateBalancer() {
    if (!balancer_cb_) return;

    std::vector<std::pair<std::string, float>> configs;
    {
        std::lock_guard<std::mutex> lock(instances_mutex_);
        for (const auto& [uri, inst] : instances_) {
            if (inst.healthy) {
                configs.emplace_back(uri, inst.latency_ms);
            }
        }
    }

    if (!configs.empty()) {
        balancer_cb_(configs);
    }
}

void ProxyHostingManager::notifyStateChange() {
    if (!state_cb_) return;
    state_cb_(getInstances());
}

std::vector<ProxyInstance> ProxyHostingManager::getInstances() const {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    std::vector<ProxyInstance> result;
    result.reserve(instances_.size());
    for (const auto& [uri, inst] : instances_) {
        result.push_back(inst);
    }
    return result;
}

ProxyInstance ProxyHostingManager::getBestInstance() const {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    ProxyInstance best;
    best.latency_ms = 1e9f;
    for (const auto& [uri, inst] : instances_) {
        if (!inst.healthy) continue;
        if (inst.latency_ms < best.latency_ms) {
            best = inst;
        }
    }
    return best;
}

int ProxyHostingManager::getLiveCount() const {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    return static_cast<int>(
        std::count_if(instances_.begin(), instances_.end(),
            [](const auto& p) { return p.second.healthy; }));
}

int ProxyHostingManager::getTotalCount() const {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    return static_cast<int>(instances_.size());
}

void ProxyHostingManager::recycleInstance(const std::string& uri) {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    if (instances_.erase(uri) > 0) {
        total_recycled_.fetch_add(1);
    }
}

void ProxyHostingManager::markInstanceHealth(const std::string& uri, bool healthy, float latency_ms) {
    std::lock_guard<std::mutex> lock(instances_mutex_);
    auto it = instances_.find(uri);
    if (it != instances_.end()) {
        it->second.healthy = healthy;
        it->second.latency_ms = latency_ms;
        it->second.last_health_check = utils::nowTimestamp();
        if (healthy) {
            it->second.consecutive_failures = 0;
        } else {
            it->second.consecutive_failures++;
        }
    }
}

} // namespace proxy
} // namespace hunter
