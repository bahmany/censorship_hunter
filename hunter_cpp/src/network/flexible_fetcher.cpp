#include "network/flexible_fetcher.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"

#include <future>
#include <chrono>

namespace hunter {
namespace network {

FlexibleFetcher::FlexibleFetcher(ConfigFetcher& http_fetcher)
    : http_fetcher_(http_fetcher) {}

bool FlexibleFetcher::isCircuitOpen(const std::string& source) {
    std::lock_guard<std::mutex> lock(circuit_mutex_);
    auto it = circuits_.find(source);
    if (it == circuits_.end()) return false;
    auto& cb = it->second;
    if (!cb.open) return false;
    if (utils::nowTimestamp() - cb.last_failure > 120.0) {
        cb.open = false;
        cb.failures = 0;
        return false;
    }
    return true;
}

void FlexibleFetcher::recordSuccess(const std::string& source, int count, double duration_ms) {
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        auto& m = metrics_[source];
        m.name = source;
        m.success_count++;
        m.consecutive_failures = 0;
        m.last_success = utils::nowTimestamp();
        int n = m.success_count;
        m.avg_configs = (m.avg_configs * (n - 1) + count) / (float)n;
    }
    {
        std::lock_guard<std::mutex> lock(circuit_mutex_);
        circuits_.erase(source);
    }
}

void FlexibleFetcher::recordFailure(const std::string& source, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        auto& m = metrics_[source];
        m.name = source;
        m.failure_count++;
        m.consecutive_failures++;
        m.last_failure = utils::nowTimestamp();
    }
    {
        std::lock_guard<std::mutex> lock(circuit_mutex_);
        auto& cb = circuits_[source];
        cb.failures++;
        cb.last_failure = utils::nowTimestamp();
        if (cb.failures >= 3) cb.open = true;
    }
}

std::vector<FetchResult> FlexibleFetcher::fetchHttpSourcesParallel(
    const std::vector<int>& proxy_ports,
    int max_configs, float timeout_per_source, int max_workers,
    const std::vector<std::string>& github_urls) {

    struct SourceMethod {
        std::string name;
        std::function<std::set<std::string>()> fn;
    };

    int tp = std::max(6, std::min(15, (int)(timeout_per_source / 3.0f)));
    float ot = std::max(10.0f, timeout_per_source);

    std::vector<SourceMethod> methods;
    if (!github_urls.empty()) {
        methods.push_back({"github", [&]() { return http_fetcher_.fetchGithubConfigs(github_urls, proxy_ports, max_configs, tp, ot); }});
    } else {
        methods.push_back({"github", [&]() { return http_fetcher_.fetchGithubConfigs(proxy_ports, max_configs, tp, ot); }});
    }
    methods.push_back({"anti_censorship", [&]() { return http_fetcher_.fetchAntiCensorshipConfigs(proxy_ports, max_configs, tp, ot); }});
    methods.push_back({"iran_priority", [&]() { return http_fetcher_.fetchIranPriorityConfigs(proxy_ports, max_configs, tp, ot); }});

    auto& mgr = HunterTaskManager::instance();
    std::vector<std::future<FetchResult>> futures;

    for (auto& m : methods) {
        if (isCircuitOpen(m.name)) {
            FetchResult r;
            r.source = m.name;
            r.error = "Circuit breaker open";
            futures.push_back(std::async(std::launch::deferred, [r]() { return r; }));
            continue;
        }

        futures.push_back(mgr.submitCPU([this, name = m.name, fn = m.fn]() -> FetchResult {
            FetchResult result;
            result.source = name;
            double t0 = utils::nowTimestamp();
            try {
                auto configs = fn();
                result.duration_ms = (utils::nowTimestamp() - t0) * 1000.0;
                if (!configs.empty()) {
                    result.configs = std::move(configs);
                    result.success = true;
                    recordSuccess(name, (int)result.configs.size(), result.duration_ms);
                } else {
                    result.error = "No configs returned";
                    recordFailure(name, result.error);
                }
            } catch (const std::exception& e) {
                result.duration_ms = (utils::nowTimestamp() - t0) * 1000.0;
                result.error = e.what();
                recordFailure(name, result.error);
            }
            return result;
        }));
    }

    std::vector<FetchResult> results;
    for (auto& fut : futures) {
        try {
            results.push_back(fut.get());
        } catch (const std::exception& e) {
            FetchResult r;
            r.error = e.what();
            results.push_back(r);
        }
    }
    return results;
}

std::vector<FlexibleFetcher::SourceRanking> FlexibleFetcher::getSourceRankings() const {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    std::vector<SourceRanking> rankings;
    for (auto& [name, m] : metrics_) {
        float total = (float)(m.success_count + m.failure_count);
        float rate = total > 0 ? m.success_count / total : 0.5f;
        float score = rate * 100.0f + std::min(m.avg_configs / 10.0f, 20.0f);
        rankings.push_back({name, score, m.success_count, m.failure_count});
    }
    std::sort(rankings.begin(), rankings.end(),
              [](const auto& a, const auto& b) { return a.score > b.score; });
    return rankings;
}

} // namespace network
} // namespace hunter
