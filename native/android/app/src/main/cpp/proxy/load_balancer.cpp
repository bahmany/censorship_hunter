#include "load_balancer.h"
#include "core/utils.h"

#include <chrono>
#include <android/log.h>

#define LOG_TAG "HunterBalancer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace hunter {

MultiProxyServer::MultiProxyServer(int port, int num_backends,
                                     int health_check_interval,
                                     StealthObfuscationEngine* obfuscation_engine,
                                     bool iran_fragment_enabled)
    : port_(port)
    , num_backends_(num_backends)
    , health_check_interval_(health_check_interval)
    , iran_fragment_enabled_(iran_fragment_enabled)
    , obfuscation_engine_(obfuscation_engine) {}

MultiProxyServer::~MultiProxyServer() {
    stop();
}

void MultiProxyServer::set_start_proxy_callback(StartProxyCallback cb) {
    start_proxy_cb_ = std::move(cb);
}

void MultiProxyServer::set_stop_proxy_callback(StopProxyCallback cb) {
    stop_proxy_cb_ = std::move(cb);
}

void MultiProxyServer::set_test_url_callback(TestUrlCallback cb) {
    test_url_cb_ = std::move(cb);
}

nlohmann::json MultiProxyServer::create_balanced_config(
    const std::vector<BackendInfo>& backends) const {

    nlohmann::json outbounds = nlohmann::json::array();
    nlohmann::json selectors = nlohmann::json::array();

    if (iran_fragment_enabled_) {
        nlohmann::json fragment_outbound = {
            {"tag", "fragment"},
            {"protocol", "freedom"},
            {"settings", {
                {"domainStrategy", "AsIs"},
                {"fragment", {
                    {"packets", "tlshello"},
                    {"length", "10-20"},
                    {"interval", "10-20"}
                }}
            }}
        };
        outbounds.push_back(fragment_outbound);
    }

    int idx = 0;
    for (const auto& backend : backends) {
        if (!backend.healthy) continue;

        auto parsed = parser_.parse(backend.uri);
        if (!parsed.has_value()) continue;

        nlohmann::json outbound = parsed->outbound;
        std::string tag = "proxy-" + std::to_string(idx);
        outbound["tag"] = tag;

        if (obfuscation_engine_ && obfuscation_engine_->enabled) {
            outbound = obfuscation_engine_->apply_obfuscation_to_config(outbound);
        }

        if (iran_fragment_enabled_) {
            if (!outbound.contains("streamSettings")) {
                outbound["streamSettings"] = nlohmann::json::object();
            }
            if (!outbound["streamSettings"].contains("sockopt")) {
                outbound["streamSettings"]["sockopt"] = nlohmann::json::object();
            }
            outbound["streamSettings"]["sockopt"]["dialerProxy"] = "fragment";
        }

        outbounds.push_back(outbound);
        selectors.push_back(tag);
        ++idx;
    }

    if (selectors.empty()) {
        outbounds.push_back({
            {"tag", "direct"},
            {"protocol", "freedom"},
            {"settings", {{"domainStrategy", "AsIs"}}}
        });
        selectors.push_back("direct");
    }

    outbounds.push_back({
        {"protocol", "blackhole"},
        {"tag", "block"},
        {"settings", nlohmann::json::object()}
    });

    nlohmann::json config = {
        {"log", {{"loglevel", "warning"}}},
        {"inbounds", {{
            {"port", port_},
            {"listen", "0.0.0.0"},
            {"protocol", "socks"},
            {"settings", {{"auth", "noauth"}, {"udp", true}}},
            {"sniffing", {
                {"enabled", true},
                {"destOverride", {"http", "tls", "quic"}},
                {"routeOnly", false}
            }}
        }}},
        {"outbounds", outbounds},
        {"routing", {
            {"domainStrategy", "AsIs"},
            {"balancers", {{
                {"tag", "balancer"},
                {"selector", selectors},
                {"strategy", {{"type", "random"}}}
            }}},
            {"rules", {{
                {"type", "field"},
                {"inboundTag", {"socks"}},
                {"balancerTag", "balancer"}
            }}}
        }},
        {"dns", {
            {"servers", {
                "https://cloudflare-dns.com/dns-query",
                "https://dns.google/dns-query",
                "1.1.1.1",
                "8.8.8.8"
            }}
        }}
    };

    return config;
}

bool MultiProxyServer::write_and_start(const std::vector<BackendInfo>& backends) {
    auto config = create_balanced_config(backends);
    std::string config_str = config.dump(2);

    // Stop existing proxy
    if (current_proxy_handle_ >= 0 && stop_proxy_cb_) {
        stop_proxy_cb_(current_proxy_handle_);
        current_proxy_handle_ = -1;
    }

    // Start new proxy via JNI
    if (start_proxy_cb_) {
        current_proxy_handle_ = start_proxy_cb_(config_str, port_);
        if (current_proxy_handle_ < 0) {
            LOGW("Failed to start balancer proxy on port %d", port_);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        stats_.restarts++;
        stats_.last_restart = now_ts();
        LOGI("Balancer started on port %d with %zu backends", port_, backends.size());
        return true;
    }

    return false;
}

std::optional<double> MultiProxyServer::test_backend(const std::string& uri, int timeout) {
    auto parsed = parser_.parse(uri);
    if (!parsed.has_value()) return std::nullopt;

    int test_port = port_ + 100 + (static_cast<int>(std::hash<std::string>{}(uri)) % 50);
    if (test_port < 0) test_port = port_ + 100;

    nlohmann::json config = {
        {"log", {{"loglevel", "warning"}}},
        {"inbounds", {{
            {"port", test_port},
            {"listen", "127.0.0.1"},
            {"protocol", "socks"},
            {"settings", {{"auth", "noauth"}, {"udp", false}}}
        }}},
        {"outbounds", {parsed->outbound}}
    };

    int handle = -1;
    if (start_proxy_cb_) {
        handle = start_proxy_cb_(config.dump(), test_port);
    }
    if (handle < 0) return std::nullopt;

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::optional<double> result;
    if (test_url_cb_) {
        auto [status_code, latency] = test_url_cb_(
            "https://cp.cloudflare.com/", test_port, timeout);
        if (status_code > 0 && (status_code < 400 || status_code == 204)) {
            result = latency;
        }
    }

    if (stop_proxy_cb_ && handle >= 0) {
        stop_proxy_cb_(handle);
    }

    return result;
}

std::vector<BackendInfo> MultiProxyServer::find_working_backends(int count) {
    std::vector<std::pair<std::string, double>> configs;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        configs = available_configs_;
    }

    // Sort by latency
    std::sort(configs.begin(), configs.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    std::vector<BackendInfo> working;
    for (const auto& [uri, latency] : configs) {
        if (static_cast<int>(working.size()) >= count) break;
        if (failed_uris_.count(uri)) continue;

        auto test_result = test_backend(uri);
        if (test_result.has_value()) {
            BackendInfo info;
            info.uri = uri;
            info.latency = test_result.value();
            info.healthy = true;
            info.added_at = now_ts();
            working.push_back(info);
        } else {
            failed_uris_.insert(uri);
        }
    }

    return working;
}

void MultiProxyServer::start(
    const std::vector<std::pair<std::string, double>>* initial_configs) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (running_.load()) return;
        running_.store(true);
    }

    if (initial_configs) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        available_configs_ = *initial_configs;
    }

    auto backends = find_working_backends(num_backends_);
    if (!backends.empty()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        backends_ = backends;
        write_and_start(backends);
    }

    health_thread_ = std::thread(&MultiProxyServer::health_check_loop, this);
}

void MultiProxyServer::health_check_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(health_check_interval_));
        if (!running_.load()) break;

        std::vector<BackendInfo> current_backends;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            current_backends = backends_;
        }

        int healthy_count = 0;
        for (const auto& b : current_backends) {
            if (b.healthy) ++healthy_count;
        }

        stats_.health_checks++;

        if (healthy_count == 0 && !available_configs_.empty()) {
            auto new_backends = find_working_backends(num_backends_);
            if (!new_backends.empty()) {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                backends_ = new_backends;
                write_and_start(new_backends);
                stats_.backend_swaps++;
            }
        }
    }
}

void MultiProxyServer::update_available_configs(
    const std::vector<std::pair<std::string, double>>& configs) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        available_configs_ = configs;
        if (!running_.load()) return;
    }

    if (backends_.empty()) {
        auto new_backends = find_working_backends(num_backends_);
        if (!new_backends.empty()) {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            backends_ = new_backends;
            write_and_start(new_backends);
        }
    }
}

void MultiProxyServer::stop() {
    running_.store(false);

    if (health_thread_.joinable()) {
        health_thread_.join();
    }

    if (current_proxy_handle_ >= 0 && stop_proxy_cb_) {
        stop_proxy_cb_(current_proxy_handle_);
        current_proxy_handle_ = -1;
    }
}

nlohmann::json MultiProxyServer::get_status() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    int healthy = 0;
    for (const auto& b : backends_) {
        if (b.healthy) ++healthy;
    }

    nlohmann::json status = {
        {"running", running_.load()},
        {"port", port_},
        {"backends", healthy},
        {"total_backends", static_cast<int>(backends_.size())},
        {"stats", {
            {"restarts", stats_.restarts},
            {"health_checks", stats_.health_checks},
            {"backend_swaps", stats_.backend_swaps},
            {"last_restart", stats_.last_restart.has_value()
                             ? nlohmann::json(stats_.last_restart.value())
                             : nlohmann::json(nullptr)}
        }}
    };
    return status;
}

} // namespace hunter
