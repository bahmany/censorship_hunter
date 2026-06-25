#include "proxy/traffic_observer.h"
#include "core/utils.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>

namespace hunter {
namespace proxy {

namespace {

std::string jsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

} // namespace

TrafficObserver::TrafficObserver() {}

TrafficObserver::~TrafficObserver() {
    stop();
}

void TrafficObserver::start() {
    if (running_.load()) return;
    running_ = true;
    observer_thread_ = std::thread(&TrafficObserver::observerLoop, this);
    std::cout << "[TrafficObserver] Started" << std::endl;
}

void TrafficObserver::stop() {
    if (!running_.exchange(false)) return;
    if (observer_thread_.joinable()) observer_thread_.join();
}

void TrafficObserver::observerLoop() {
    while (running_.load()) {
        if (broadcast_cb_) {
            std::string json = getMetricsJson();
            broadcast_cb_(json);
        }

        for (int i = 0; i < 5 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void TrafficObserver::recordLatency(float latency_ms) {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    latency_samples_.push_back(latency_ms);
    if (latency_samples_.size() > 1000) {
        latency_samples_.erase(latency_samples_.begin(),
                               latency_samples_.begin() + 500);
    }
}

void TrafficObserver::recordBytes(uint64_t bytes_sent, uint64_t bytes_received) {
    total_bytes_sent_.fetch_add(bytes_sent);
    total_bytes_received_.fetch_add(bytes_received);
}

void TrafficObserver::recordFailure() {
    total_failures_.fetch_add(1);
}

void TrafficObserver::recordSuccess() {
    total_successes_.fetch_add(1);
}

void TrafficObserver::recordFallback() {
    fallback_events_.fetch_add(1);
}

void TrafficObserver::recordSessionEvent(const std::string& event_type) {
    total_session_events_.fetch_add(1);
    (void)event_type;
}

void TrafficObserver::updateEngineHealth(const std::string& engine, int healthy_count) {
    std::lock_guard<std::mutex> lock(engine_health_mutex_);
    engine_health_map_[engine] = healthy_count;
}

void TrafficObserver::updateRoutingDistribution(const std::string& instance_uri, int session_count) {
    std::lock_guard<std::mutex> lock(routing_mutex_);
    routing_distribution_[instance_uri] = session_count;
}

TrafficMetrics TrafficObserver::getMetrics() const {
    TrafficMetrics m;

    if (metrics_provider_) {
        m = metrics_provider_();
    }

    m.bytes_sent = total_bytes_sent_.load();
    m.bytes_received = total_bytes_received_.load();
    m.total_bytes_proxied = m.bytes_sent + m.bytes_received;
    m.total_failures = total_failures_.load();
    m.total_successes = total_successes_.load();
    int total = m.total_failures + m.total_successes;
    m.failure_rate = total > 0 ? static_cast<float>(m.total_failures) / total : 0.0f;
    m.fallback_events = fallback_events_.load();

    computeLatencyPercentiles(m);

    {
        std::lock_guard<std::mutex> lock(engine_health_mutex_);
        m.engine_health_map = engine_health_map_;
    }
    {
        std::lock_guard<std::mutex> lock(routing_mutex_);
        m.routing_distribution = routing_distribution_;
    }

    return m;
}

void TrafficObserver::computeLatencyPercentiles(TrafficMetrics& m) const {
    std::lock_guard<std::mutex> lock(latency_mutex_);
    if (latency_samples_.empty()) return;

    std::vector<float> sorted = latency_samples_;
    std::sort(sorted.begin(), sorted.end());

    size_t n = sorted.size();
    m.avg_latency_ms = 0;
    for (float v : sorted) m.avg_latency_ms += v;
    m.avg_latency_ms /= n;

    m.p50_latency_ms = sorted[n * 50 / 100];
    m.p95_latency_ms = sorted[n * 95 / 100];
    m.p99_latency_ms = sorted[n * 99 / 100];
}

std::string TrafficObserver::getMetricsJson() const {
    auto m = getMetrics();
    return buildMetricsJson(m);
}

std::string TrafficObserver::buildMetricsJson(const TrafficMetrics& m) const {
    std::ostringstream json;
    json << "{"
         << "\"active_sessions\":" << m.active_sessions << ","
         << "\"total_sessions\":" << m.total_sessions << ","
         << "\"live_proxy_instances\":" << m.live_proxy_instances << ","
         << "\"total_bytes_proxied\":" << m.total_bytes_proxied << ","
         << "\"bytes_sent\":" << m.bytes_sent << ","
         << "\"bytes_received\":" << m.bytes_received << ","
         << "\"total_failures\":" << m.total_failures << ","
         << "\"total_successes\":" << m.total_successes << ","
         << "\"failure_rate\":" << m.failure_rate << ","
         << "\"avg_latency_ms\":" << static_cast<int>(m.avg_latency_ms) << ","
         << "\"p50_latency_ms\":" << static_cast<int>(m.p50_latency_ms) << ","
         << "\"p95_latency_ms\":" << static_cast<int>(m.p95_latency_ms) << ","
         << "\"p99_latency_ms\":" << static_cast<int>(m.p99_latency_ms) << ","
         << "\"fallback_events\":" << m.fallback_events << ",";

    json << "\"engine_health_map\":{";
    bool first = true;
    for (const auto& [engine, count] : m.engine_health_map) {
        if (!first) json << ",";
        first = false;
        json << "\"" << jsonEscape(engine) << "\":" << count;
    }
    json << "},";

    json << "\"routing_distribution\":{";
    first = true;
    for (const auto& [uri, count] : m.routing_distribution) {
        if (!first) json << ",";
        first = false;
        json << "\"" << jsonEscape(uri) << "\":" << count;
    }
    json << "}";

    json << "}";
    return json.str();
}

} // namespace proxy
} // namespace hunter
