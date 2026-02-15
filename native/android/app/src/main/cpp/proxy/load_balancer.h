#pragma once

#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>

#include "core/models.h"
#include "parsers/uri_parser.h"
#include "security/obfuscation.h"
#include "testing/benchmark.h"

namespace hunter {

struct BackendInfo {
    std::string uri;
    double latency = 0.0;
    bool healthy = true;
    int64_t added_at = 0;
};

class MultiProxyServer {
public:
    MultiProxyServer(int port = 10808,
                     int num_backends = 5,
                     int health_check_interval = 60,
                     StealthObfuscationEngine* obfuscation_engine = nullptr,
                     bool iran_fragment_enabled = false);
    ~MultiProxyServer();

    void set_start_proxy_callback(StartProxyCallback cb);
    void set_stop_proxy_callback(StopProxyCallback cb);
    void set_test_url_callback(TestUrlCallback cb);

    void start(const std::vector<std::pair<std::string, double>>* initial_configs = nullptr);
    void stop();
    void update_available_configs(const std::vector<std::pair<std::string, double>>& configs);
    nlohmann::json get_status() const;

private:
    nlohmann::json create_balanced_config(const std::vector<BackendInfo>& backends) const;
    bool write_and_start(const std::vector<BackendInfo>& backends);
    std::optional<double> test_backend(const std::string& uri, int timeout = 8);
    std::vector<BackendInfo> find_working_backends(int count = 5);
    void health_check_loop();

    int port_;
    int num_backends_;
    int health_check_interval_;
    bool iran_fragment_enabled_;
    UniversalParser parser_;
    StealthObfuscationEngine* obfuscation_engine_;

    mutable std::recursive_mutex mutex_;
    std::atomic<bool> running_{false};
    int current_proxy_handle_ = -1;
    std::vector<BackendInfo> backends_;
    std::vector<std::pair<std::string, double>> available_configs_;
    std::set<std::string> failed_uris_;

    struct {
        int restarts = 0;
        int health_checks = 0;
        int backend_swaps = 0;
        std::optional<int64_t> last_restart;
    } stats_;

    std::thread health_thread_;

    StartProxyCallback start_proxy_cb_;
    StopProxyCallback stop_proxy_cb_;
    TestUrlCallback test_url_cb_;
};

} // namespace hunter
