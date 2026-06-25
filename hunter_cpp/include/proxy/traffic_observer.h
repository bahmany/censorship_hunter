#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hunter {
namespace proxy {

struct TrafficMetrics {
    int active_sessions = 0;
    int total_sessions = 0;
    int live_proxy_instances = 0;

    uint64_t total_bytes_proxied = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;

    int total_failures = 0;
    int total_successes = 0;
    float failure_rate = 0.0f;

    float avg_latency_ms = 0.0f;
    float p50_latency_ms = 0.0f;
    float p95_latency_ms = 0.0f;
    float p99_latency_ms = 0.0f;

    int fallback_events = 0;

    std::map<std::string, int> engine_health_map;
    std::map<std::string, int> routing_distribution;

    double observation_window_s = 0.0;
};

class TrafficObserver {
public:
    using MetricsBroadcastCallback = std::function<void(const std::string&)>;
    using MetricsProvider = std::function<TrafficMetrics()>;

    TrafficObserver();
    ~TrafficObserver();

    TrafficObserver(const TrafficObserver&) = delete;
    TrafficObserver& operator=(const TrafficObserver&) = delete;

    void setBroadcastCallback(MetricsBroadcastCallback cb) { broadcast_cb_ = std::move(cb); }
    void setMetricsProvider(MetricsProvider cb) { metrics_provider_ = std::move(cb); }

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void recordLatency(float latency_ms);
    void recordBytes(uint64_t bytes_sent, uint64_t bytes_received);
    void recordFailure();
    void recordSuccess();
    void recordFallback();
    void recordSessionEvent(const std::string& event_type);

    TrafficMetrics getMetrics() const;
    std::string getMetricsJson() const;

    void updateEngineHealth(const std::string& engine, int healthy_count);
    void updateRoutingDistribution(const std::string& instance_uri, int session_count);

private:
    std::atomic<bool> running_{false};
    std::thread observer_thread_;

    MetricsBroadcastCallback broadcast_cb_;
    MetricsProvider metrics_provider_;

    mutable std::mutex latency_mutex_;
    std::vector<float> latency_samples_;

    std::atomic<uint64_t> total_bytes_sent_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
    std::atomic<int> total_failures_{0};
    std::atomic<int> total_successes_{0};
    std::atomic<int> fallback_events_{0};
    std::atomic<int> total_session_events_{0};

    mutable std::mutex engine_health_mutex_;
    std::map<std::string, int> engine_health_map_;

    mutable std::mutex routing_mutex_;
    std::map<std::string, int> routing_distribution_;

    void observerLoop();
    std::string buildMetricsJson(const TrafficMetrics& m) const;
    void computeLatencyPercentiles(TrafficMetrics& m) const;
};

} // namespace proxy
} // namespace hunter
