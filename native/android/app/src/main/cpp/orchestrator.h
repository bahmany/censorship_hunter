#pragma once

#include <string>
#include <vector>
#include <tuple>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/models.h"
#include "network/http_client.h"
#include "parsers/uri_parser.h"
#include "testing/benchmark.h"
#include "proxy/load_balancer.h"
#include "telegram/scraper.h"
#include "cache/cache.h"
#include "security/obfuscation.h"

namespace hunter {

// Callback to notify Java layer of progress/status
using ProgressCallback = std::function<void(const std::string& phase, int current, int total)>;
using StatusCallback = std::function<void(const std::string& status_json)>;

class HunterOrchestrator {
public:
    explicit HunterOrchestrator(HunterConfig& config);
    ~HunterOrchestrator();

    // Inject JNI callbacks
    void set_http_callback(HttpCallback cb);
    void set_start_proxy_callback(StartProxyCallback cb);
    void set_stop_proxy_callback(StopProxyCallback cb);
    void set_test_url_callback(TestUrlCallback cb);
    void set_telegram_fetch_callback(TelegramFetchCallback cb);
    void set_telegram_send_callback(TelegramSendCallback cb);
    void set_telegram_send_file_callback(TelegramSendFileCallback cb);
    void set_progress_callback(ProgressCallback cb);
    void set_status_callback(StatusCallback cb);

    // Lifecycle
    void start();
    void stop();
    bool is_running() const;

    // Manual trigger
    void run_cycle();

    // Get cached configs
    std::string get_cached_configs();

    // Status
    nlohmann::json get_status() const;

private:
    std::vector<std::string> scrape_configs();
    std::vector<HunterBenchResult> validate_configs(const std::vector<std::string>& configs,
                                                      int max_workers = 50);
    std::unordered_map<std::string, std::vector<HunterBenchResult>>
        tier_configs(const std::vector<HunterBenchResult>& results);
    void save_to_files(const std::vector<HunterBenchResult>& gold,
                       const std::vector<HunterBenchResult>& silver);
    void autonomous_loop();

    std::string balancer_cache_path(const std::string& name = "HUNTER_balancer_cache.json") const;
    std::vector<std::pair<std::string, double>> load_balancer_cache(
        const std::string& name = "HUNTER_balancer_cache.json") const;
    void save_balancer_cache(const std::vector<std::pair<std::string, double>>& configs,
                              const std::string& name = "HUNTER_balancer_cache.json");

    HunterConfig& config_;
    HTTPClientManager http_manager_;
    ConfigFetcher config_fetcher_;
    UniversalParser parser_;
    ProxyBenchmark benchmarker_;
    SmartCache cache_;
    TelegramScraper telegram_scraper_;
    TelegramReporter telegram_reporter_;
    StealthObfuscationEngine obfuscation_;
    MultiProxyServer balancer_;
    std::unique_ptr<MultiProxyServer> gemini_balancer_;

    // State
    std::vector<std::pair<std::string, double>> validated_configs_;
    int64_t last_cycle_ = 0;
    int cycle_count_ = 0;

    // Threading
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread loop_thread_;

    // Callbacks
    ProgressCallback progress_cb_;
    StatusCallback status_cb_;
};

} // namespace hunter
