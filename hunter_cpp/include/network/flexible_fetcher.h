#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <functional>

#include "core/models.h"
#include "network/http_client.h"

namespace hunter {
namespace network {

/**
 * @brief Flexible config fetcher with multiple fallback strategies
 * 
 * Coordinates Telegram scraping and HTTP source fetching with
 * circuit breakers, adaptive strategy selection, and metrics tracking.
 */
class FlexibleFetcher {
public:
    FlexibleFetcher(ConfigFetcher& http_fetcher);
    ~FlexibleFetcher() = default;

    /**
     * @brief Fetch from HTTP sources in parallel
     */
    std::vector<FetchResult> fetchHttpSourcesParallel(
        const std::vector<int>& proxy_ports,
        int max_configs = 200,
        float timeout_per_source = 25.0f,
        int max_workers = 6,
        const std::vector<std::string>& github_urls = {});

    /**
     * @brief Get source rankings by success rate
     */
    struct SourceRanking {
        std::string name;
        float score;
        int successes;
        int failures;
    };
    std::vector<SourceRanking> getSourceRankings() const;

private:
    ConfigFetcher& http_fetcher_;

    struct SourceMetrics {
        std::string name;
        int success_count = 0;
        int failure_count = 0;
        double last_success = 0.0;
        double last_failure = 0.0;
        float avg_configs = 0.0f;
        int consecutive_failures = 0;
    };

    std::map<std::string, SourceMetrics> metrics_;
    mutable std::mutex metrics_mutex_;

    // Circuit breaker
    struct CircuitState {
        int failures = 0;
        double last_failure = 0.0;
        bool open = false;
    };
    std::map<std::string, CircuitState> circuits_;
    std::mutex circuit_mutex_;

    bool isCircuitOpen(const std::string& source);
    void recordSuccess(const std::string& source, int count, double duration_ms);
    void recordFailure(const std::string& source, const std::string& error);
};

} // namespace network
} // namespace hunter
