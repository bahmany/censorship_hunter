#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include "core/models.h"
#include "proxy/xray_manager.h"

namespace hunter {
namespace testing {

/**
 * @brief Proxy config benchmarker using XRay subprocesses
 * 
 * Spawns temporary XRay processes for each config, tests HTTP
 * connectivity through the SOCKS port, and measures latency.
 * Supports batch benchmarking with memory-safe chunking.
 */
class ProxyBenchmark {
public:
    using ResultCallback = std::function<void(const ParsedConfig&, float latency, const std::string& tier)>;

    explicit ProxyBenchmark(proxy::XRayManager& xray);
    ~ProxyBenchmark() = default;

    /**
     * @brief Set callback for individual results
     */
    void setResultCallback(ResultCallback cb) { result_callback_ = cb; }

    /**
     * @brief Benchmark a batch of configs
     * @param configs Parsed configs with assigned SOCKS ports
     * @param test_url URL to test connectivity
     * @param timeout_ms Per-config test timeout
     * @return List of benchmark results
     */
    std::vector<BenchResult> benchmarkBatch(
        const std::vector<std::pair<ParsedConfig, int>>& configs,
        const std::string& test_url = "http://cp.cloudflare.com/generate_204",
        int timeout_ms = 15000);

    /**
     * @brief Benchmark a single config
     */
    BenchResult benchmarkSingle(const ParsedConfig& config, int socks_port,
                                 const std::string& test_url = "http://cp.cloudflare.com/generate_204",
                                 int timeout_ms = 15000);

    /**
     * @brief Classify latency into tier
     */
    static std::string classifyTier(float latency_ms);

    /**
     * @brief Get performance metrics
     */
    struct PerfMetrics {
        int total_tested = 0;
        int total_passed = 0;
        float avg_latency_ms = 0.0f;
        float tasks_per_second = 0.0f;
    };
    PerfMetrics getMetrics() const;

private:
    proxy::XRayManager& xray_;
    ResultCallback result_callback_;
    std::atomic<int> total_tested_{0};
    std::atomic<int> total_passed_{0};
    float total_latency_ = 0.0f;
    std::mutex metrics_mutex_;
    double start_time_ = 0.0;

    bool testSocksConnectivity(int socks_port, const std::string& test_url,
                                int timeout_ms, float& out_latency);
    bool waitForPort(int port, int timeout_ms = 5000);
};

} // namespace testing
} // namespace hunter
