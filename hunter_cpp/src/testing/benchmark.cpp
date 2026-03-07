#include "testing/benchmark.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "network/uri_parser.h"
#include "network/http_client.h"

#include <chrono>
#include <future>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hunter {
namespace testing {

ProxyBenchmark::ProxyBenchmark(proxy::XRayManager& xray) : xray_(xray) {
    start_time_ = utils::nowTimestamp();
}

std::string ProxyBenchmark::classifyTier(float latency_ms) {
    if (latency_ms <= 0) return "dead";
    if (latency_ms <= constants::GOLD_LATENCY_MS) return "gold";
    if (latency_ms <= constants::SILVER_LATENCY_MS) return "silver";
    return "dead";
}

bool ProxyBenchmark::waitForPort(int port, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (utils::isPortAlive(port, 500)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool ProxyBenchmark::testSocksConnectivity(int socks_port, const std::string& test_url,
                                            int timeout_ms, float& out_latency) {
    // Use libcurl through SOCKS5 proxy to test connectivity
    network::HttpClient http;
    std::string proxy = "socks5h://127.0.0.1:" + std::to_string(socks_port);

    auto t0 = std::chrono::steady_clock::now();
    std::string body = http.get(test_url, timeout_ms, proxy);
    auto t1 = std::chrono::steady_clock::now();

    if (body.empty()) {
        out_latency = 0;
        return false;
    }

    out_latency = std::chrono::duration<float, std::milli>(t1 - t0).count();
    return true;
}

BenchResult ProxyBenchmark::benchmarkSingle(const ParsedConfig& config, int socks_port,
                                             const std::string& test_url, int timeout_ms) {
    BenchResult result;
    result.uri = config.uri;
    result.ps = config.ps;
    result.protocol = config.protocol;

    // Generate XRay config
    std::string xray_config = proxy::XRayManager::generateConfig(config, socks_port);
    std::string config_path = xray_.writeConfigFile(xray_config);

    // Start XRay process
    int pid = xray_.startProcess(config_path);
    if (pid <= 0) {
        result.error = "Failed to start XRay";
        result.tier = "dead";
        return result;
    }

    // Wait for SOCKS port to become available
    if (!waitForPort(socks_port, constants::XRAY_START_TIMEOUT_MS)) {
        xray_.stopProcess(pid);
        result.error = "XRay port timeout";
        result.tier = "dead";
        return result;
    }

    // Test connectivity
    float latency = 0;
    bool ok = testSocksConnectivity(socks_port, test_url, timeout_ms, latency);

    // Stop XRay
    xray_.stopProcess(pid);

    if (ok && latency > 0) {
        result.success = true;
        result.latency_ms = latency;
        result.tier = classifyTier(latency);
    } else {
        result.error = "Connectivity test failed";
        result.tier = "dead";
    }

    total_tested_++;
    if (result.success) {
        total_passed_++;
        std::lock_guard<std::mutex> lock(metrics_mutex_);
        total_latency_ += latency;
    }

    if (result_callback_) {
        result_callback_(config, result.latency_ms, result.tier);
    }

    return result;
}

std::vector<BenchResult> ProxyBenchmark::benchmarkBatch(
    const std::vector<std::pair<ParsedConfig, int>>& configs,
    const std::string& test_url, int timeout_ms) {

    std::vector<BenchResult> results;
    if (configs.empty()) return results;

    auto& mgr = HunterTaskManager::instance();

    // Process in chunks to manage memory
    constexpr int CHUNK_SIZE = 50;
    for (size_t offset = 0; offset < configs.size(); offset += CHUNK_SIZE) {
        size_t end = std::min(offset + CHUNK_SIZE, configs.size());

        std::vector<std::future<BenchResult>> futures;
        for (size_t i = offset; i < end; i++) {
            auto& [parsed, port] = configs[i];
            futures.push_back(mgr.submitIO(
                [this, &parsed, port, &test_url, timeout_ms]() {
                    return benchmarkSingle(parsed, port, test_url, timeout_ms);
                }));
        }

        for (auto& fut : futures) {
            try {
                results.push_back(fut.get());
            } catch (const std::exception& e) {
                BenchResult r;
                r.error = e.what();
                r.tier = "dead";
                results.push_back(r);
            }
        }
    }

    // Sort by latency (successful first)
    std::sort(results.begin(), results.end(), [](const BenchResult& a, const BenchResult& b) {
        if (a.success != b.success) return a.success > b.success;
        return a.latency_ms < b.latency_ms;
    });

    return results;
}

ProxyBenchmark::PerfMetrics ProxyBenchmark::getMetrics() const {
    PerfMetrics m;
    m.total_tested = total_tested_.load();
    m.total_passed = total_passed_.load();
    {
        // Approximate avg latency
        if (m.total_passed > 0) {
            m.avg_latency_ms = total_latency_ / (float)m.total_passed;
        }
    }
    double elapsed = utils::nowTimestamp() - start_time_;
    if (elapsed > 0) {
        m.tasks_per_second = (float)m.total_tested / (float)elapsed;
    }
    return m;
}

} // namespace testing
} // namespace hunter
