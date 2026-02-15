#include "orchestrator.h"
#include "core/utils.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <queue>
#include <android/log.h>

#define LOG_TAG "HunterOrchestrator"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace hunter {

HunterOrchestrator::HunterOrchestrator(HunterConfig& config)
    : config_(config)
    , config_fetcher_(http_manager_)
    , benchmarker_(config.get_bool("iran_fragment_enabled", false))
    , telegram_scraper_()
    , telegram_reporter_(telegram_scraper_)
    , obfuscation_(true)
    , balancer_(
          static_cast<int>(config.get_int("multiproxy_port", 10808)),
          static_cast<int>(config.get_int("multiproxy_backends", 5)),
          static_cast<int>(config.get_int("multiproxy_health_interval", 60)),
          &obfuscation_,
          config.get_bool("iran_fragment_enabled", false))
{
        gemini_balancer_ = std::make_unique<MultiProxyServer>(
            static_cast<int>(config.get_int("gemini_port", 10809)),
            static_cast<int>(config.get_int("multiproxy_backends", 5)),
            static_cast<int>(config.get_int("multiproxy_health_interval", 60)),
            &obfuscation_,
            config.get_bool("iran_fragment_enabled", false));

    // Set cache base directory
    std::string files_dir = config.get_files_dir();
    if (!files_dir.empty()) {
        cache_.set_base_dir(files_dir);
    }
}

HunterOrchestrator::~HunterOrchestrator() {
    stop();
}

// ---------- Callback injection ----------

void HunterOrchestrator::set_http_callback(HttpCallback cb) {
    http_manager_.set_http_callback(std::move(cb));
}

void HunterOrchestrator::set_start_proxy_callback(StartProxyCallback cb) {
    auto cb_copy = cb;
    benchmarker_.set_start_proxy_callback(cb);
    balancer_.set_start_proxy_callback(cb);
    if (gemini_balancer_) {
        gemini_balancer_->set_start_proxy_callback(std::move(cb_copy));
    }
}

void HunterOrchestrator::set_stop_proxy_callback(StopProxyCallback cb) {
    auto cb_copy = cb;
    benchmarker_.set_stop_proxy_callback(cb);
    balancer_.set_stop_proxy_callback(cb);
    if (gemini_balancer_) {
        gemini_balancer_->set_stop_proxy_callback(std::move(cb_copy));
    }
}

void HunterOrchestrator::set_test_url_callback(TestUrlCallback cb) {
    auto cb_copy = cb;
    benchmarker_.set_test_url_callback(cb);
    balancer_.set_test_url_callback(cb);
    if (gemini_balancer_) {
        gemini_balancer_->set_test_url_callback(std::move(cb_copy));
    }
}

void HunterOrchestrator::set_telegram_fetch_callback(TelegramFetchCallback cb) {
    telegram_scraper_.set_fetch_callback(std::move(cb));
}

void HunterOrchestrator::set_telegram_send_callback(TelegramSendCallback cb) {
    telegram_scraper_.set_send_callback(std::move(cb));
}

void HunterOrchestrator::set_telegram_send_file_callback(TelegramSendFileCallback cb) {
    telegram_scraper_.set_send_file_callback(std::move(cb));
}

void HunterOrchestrator::set_progress_callback(ProgressCallback cb) {
    progress_cb_ = std::move(cb);
}

void HunterOrchestrator::set_status_callback(StatusCallback cb) {
    status_cb_ = std::move(cb);
}

// ---------- Scraping ----------

std::vector<std::string> HunterOrchestrator::scrape_configs() {
    std::vector<std::string> configs;
    std::vector<int> proxy_ports = {static_cast<int>(config_.get_int("multiproxy_port", 10808))};

    // 1. Telegram configs - highest priority
    auto telegram_channels = config_.get_string_list("targets");
    if (!telegram_channels.empty()) {
        int telegram_limit = static_cast<int>(config_.get_int("telegram_limit", 50));
        try {
            auto telegram_configs = telegram_scraper_.scrape_configs(telegram_channels, telegram_limit);
            configs.insert(configs.end(), telegram_configs.begin(), telegram_configs.end());
            LOGI("Telegram sources: %zu configs", telegram_configs.size());
        } catch (...) {
            LOGW("Telegram scrape failed");
        }
    }

    if (progress_cb_) progress_cb_("scraping_github", 0, 0);

    // 2. GitHub configs
    try {
        auto github_configs = config_fetcher_.fetch_github_configs(proxy_ports);
        configs.insert(configs.end(), github_configs.begin(), github_configs.end());
        LOGI("GitHub sources: %zu configs", github_configs.size());
    } catch (...) {
        LOGW("GitHub fetch failed");
    }

    if (progress_cb_) progress_cb_("scraping_anticensorship", 0, 0);

    // 3. Anti-censorship configs
    try {
        auto anti_censorship = config_fetcher_.fetch_anti_censorship_configs(proxy_ports);
        configs.insert(configs.end(), anti_censorship.begin(), anti_censorship.end());
        LOGI("Anti-censorship sources: %zu configs", anti_censorship.size());
    } catch (...) {
        LOGW("Anti-censorship fetch failed");
    }

    // 4. Iran priority configs
    try {
        auto iran_priority = config_fetcher_.fetch_iran_priority_configs(proxy_ports);
        configs.insert(configs.end(), iran_priority.begin(), iran_priority.end());
        LOGI("Iran priority sources: %zu configs", iran_priority.size());
    } catch (...) {
        LOGW("Iran priority fetch failed");
    }

    // 5. Load cached working configs if total is still low
    if (configs.size() < 500) {
        auto cached = cache_.load_cached_configs(500, true);
        if (!cached.empty()) {
            configs.insert(configs.end(), cached.begin(), cached.end());
            LOGI("Cached working configs: %zu added", cached.size());
        }
    }

    return configs;
}

// ---------- Validation ----------

std::vector<HunterBenchResult> HunterOrchestrator::validate_configs(
    const std::vector<std::string>& configs, int max_workers) {

    std::vector<HunterBenchResult> results;
    std::string test_url = config_.get_string("test_url",
                                               "https://www.cloudflare.com/cdn-cgi/trace");
    int timeout = static_cast<int>(config_.get_int("timeout_seconds", 10));

    int configured_workers = static_cast<int>(config_.get_int("max_workers", 0));
    if (configured_workers > 0) max_workers = configured_workers;

    // Deduplicate
    int max_total = static_cast<int>(config_.get_int("max_total", 3000));
    std::set<std::string> seen;
    std::vector<std::string> deduped;
    for (const auto& c : configs) {
        if (seen.insert(c).second) {
            deduped.push_back(c);
            if (static_cast<int>(deduped.size()) >= max_total) break;
        }
    }

    // Prioritize
    auto limited = prioritize_configs(deduped);
    LOGI("Prioritized %zu configs by anti-DPI features", limited.size());

    if (limited.empty()) return results;

    int base_port = static_cast<int>(config_.get_int("multiproxy_port", 10808)) + 1000;
    int workers = std::max(1, std::min(max_workers, std::min(200, static_cast<int>(limited.size()))));

    // Port pool
    std::queue<int> port_pool;
    std::mutex pool_mutex;
    for (int i = 0; i < workers; ++i) {
        port_pool.push(base_port + i);
    }

    std::mutex results_mutex;
    std::atomic<int> completed{0};
    int total = static_cast<int>(limited.size());

    auto bench_one = [&](const std::string& uri) {
        auto parsed = parser_.parse(uri);
        if (!parsed.has_value()) return;

        int port = 0;
        {
            std::lock_guard<std::mutex> lock(pool_mutex);
            if (port_pool.empty()) return;
            port = port_pool.front();
            port_pool.pop();
        }

        try {
            auto latency = benchmarker_.benchmark_config(parsed.value(), port, test_url, timeout);
            if (latency.has_value()) {
                auto result = benchmarker_.create_bench_result(parsed.value(), latency.value());
                std::lock_guard<std::mutex> lock(results_mutex);
                results.push_back(result);
            }
        } catch (...) {}

        {
            std::lock_guard<std::mutex> lock(pool_mutex);
            port_pool.push(port);
        }

        int done = ++completed;
        if (progress_cb_ && done % 10 == 0) {
            progress_cb_("validating", done, total);
        }
    };

    // Execute with thread pool
    size_t index = 0;
    std::mutex index_mutex;
    std::vector<std::thread> threads;

    for (int i = 0; i < workers; ++i) {
        threads.emplace_back([&]() {
            while (true) {
                size_t my_index;
                {
                    std::lock_guard<std::mutex> lock(index_mutex);
                    if (index >= limited.size()) return;
                    my_index = index++;
                }
                bench_one(limited[my_index]);
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    // Sort by latency
    std::sort(results.begin(), results.end(),
              [](const HunterBenchResult& a, const HunterBenchResult& b) {
                  return a.latency_ms < b.latency_ms;
              });

    return results;
}

// ---------- Tiering ----------

std::unordered_map<std::string, std::vector<HunterBenchResult>>
HunterOrchestrator::tier_configs(const std::vector<HunterBenchResult>& results) {
    std::vector<HunterBenchResult> gold, silver;

    for (const auto& r : results) {
        if (r.tier == "gold") gold.push_back(r);
        else if (r.tier == "silver") silver.push_back(r);
    }

    if (gold.size() > 100) gold.resize(100);
    if (silver.size() > 200) silver.resize(200);

    return {{"gold", gold}, {"silver", silver}};
}

// ---------- File saving ----------

void HunterOrchestrator::save_to_files(const std::vector<HunterBenchResult>& gold,
                                         const std::vector<HunterBenchResult>& silver) {
    std::string gold_file = config_.get_string("gold_file");
    std::string silver_file = config_.get_string("silver_file");

    try {
        if (!gold_file.empty()) {
            std::vector<std::string> uris;
            for (const auto& r : gold) uris.push_back(r.uri);
            write_lines(gold_file, uris);
        }
    } catch (...) {}

    try {
        if (!silver_file.empty()) {
            std::vector<std::string> uris;
            for (const auto& r : silver) uris.push_back(r.uri);
            write_lines(silver_file, uris);
        }
    } catch (...) {}
}

// ---------- Balancer cache ----------

std::string HunterOrchestrator::balancer_cache_path(const std::string& name) const {
    std::string state_file = config_.get_string("state_file");
    if (!state_file.empty()) {
        auto pos = state_file.rfind('/');
        if (pos != std::string::npos) {
            return state_file.substr(0, pos) + "/" + name;
        }
    }
    std::string files_dir = config_.get_files_dir();
    if (!files_dir.empty()) {
        return files_dir + "/runtime/" + name;
    }
    return "runtime/" + name;
}

std::vector<std::pair<std::string, double>> HunterOrchestrator::load_balancer_cache(
    const std::string& name) const {
    std::string path = balancer_cache_path(name);
    auto payload = load_json(path, nlohmann::json{});
    std::vector<std::pair<std::string, double>> seed;

    if (!payload.contains("configs") || !payload["configs"].is_array()) {
        return seed;
    }

    for (const auto& item : payload["configs"]) {
        if (!item.is_object() || !item.contains("uri") || !item["uri"].is_string()) continue;
        double latency = item.value("latency_ms", 0.0);
        seed.emplace_back(item["uri"].get<std::string>(), latency);
    }

    return seed;
}

void HunterOrchestrator::save_balancer_cache(
    const std::vector<std::pair<std::string, double>>& configs,
    const std::string& name) {
    std::string path = balancer_cache_path(name);

    // Ensure parent directory
    auto pos = path.rfind('/');
    if (pos != std::string::npos) {
        ensure_directory(path.substr(0, pos));
    }

    nlohmann::json items = nlohmann::json::array();
    int count = 0;
    for (const auto& [uri, lat] : configs) {
        if (count >= 1000) break;
        items.push_back({{"uri", uri}, {"latency_ms", lat}});
        ++count;
    }

    nlohmann::json payload = {
        {"saved_at", now_ts()},
        {"configs", items}
    };

    save_json(path, payload);
}

// ---------- Cycle ----------

void HunterOrchestrator::run_cycle() {
    std::lock_guard<std::mutex> lock(mutex_);
    cycle_count_++;
    auto cycle_start = std::chrono::steady_clock::now();

    LOGI("Starting hunter cycle #%d", cycle_count_);
    if (progress_cb_) progress_cb_("cycle_start", cycle_count_, 0);

    // Scrape
    auto raw_configs = scrape_configs();
    LOGI("Total raw configs: %zu", raw_configs.size());

    // Cache new configs
    cache_.save_configs(raw_configs, false);

    // Validate
    if (progress_cb_) progress_cb_("validating", 0, static_cast<int>(raw_configs.size()));
    auto validated = validate_configs(raw_configs);
    LOGI("Validated configs: %zu", validated.size());

    // Save working configs to cache
    if (!validated.empty()) {
        std::vector<std::string> working_uris;
        for (const auto& r : validated) working_uris.push_back(r.uri);
        cache_.save_configs(working_uris, true);
    }

    // Tier
    auto tiered = tier_configs(validated);
    auto& gold_configs = tiered["gold"];
    auto& silver_configs = tiered["silver"];
    LOGI("Gold tier: %zu, Silver tier: %zu", gold_configs.size(), silver_configs.size());

    // Update balancer
    std::vector<std::pair<std::string, double>> all_configs;
    for (const auto& r : gold_configs) all_configs.emplace_back(r.uri, r.latency_ms);
    for (const auto& r : silver_configs) all_configs.emplace_back(r.uri, r.latency_ms);

    balancer_.update_available_configs(all_configs);
    save_balancer_cache(all_configs, "HUNTER_balancer_cache.json");

    // Gemini balancer
    std::vector<std::pair<std::string, double>> gemini_configs;
    if (gemini_balancer_) {
        for (const auto& r : gold_configs) {
            std::string ps_lower = to_lower(r.ps);
            if (ps_lower.find("gemini") != std::string::npos ||
                ps_lower.find("gmn") != std::string::npos) {
                gemini_configs.emplace_back(r.uri, r.latency_ms);
            }
        }
        for (const auto& r : silver_configs) {
            std::string ps_lower = to_lower(r.ps);
            if (ps_lower.find("gemini") != std::string::npos ||
                ps_lower.find("gmn") != std::string::npos) {
                gemini_configs.emplace_back(r.uri, r.latency_ms);
            }
        }
        gemini_balancer_->update_available_configs(gemini_configs);
        save_balancer_cache(gemini_configs, "HUNTER_gemini_balancer_cache.json");
    }

    if (gold_configs.empty() && silver_configs.empty()) {
        try {
            std::string xray_path = config_.get_string("xray_path");
            std::string msg = "Hunter cycle #" + std::to_string(cycle_count_) + " finished\n";
            msg += "raw=" + std::to_string(raw_configs.size()) + ", validated=0\n";
            msg += "Tip: set a runnable XRay binary path in config (xray_path).\n";
            msg += "xray_path=" + (xray_path.empty() ? std::string("<empty>") : xray_path);
            telegram_scraper_.send_report(msg);
        } catch (...) {}
    }

    // Report to Telegram
    std::vector<nlohmann::json> gold_report;
    for (const auto& r : gold_configs) {
        gold_report.push_back({
            {"ps", r.ps},
            {"latency_ms", r.latency_ms},
            {"region", r.region},
            {"tier", r.tier}
        });
    }
    telegram_reporter_.report_gold_configs(gold_report);

    try {
        std::vector<std::string> gold_uris;
        for (const auto& r : gold_configs) gold_uris.push_back(r.uri);
        std::vector<std::string> gemini_uris;
        for (const auto& [uri, _] : gemini_configs) gemini_uris.push_back(uri);
        telegram_reporter_.report_config_files(
            gold_uris,
            gemini_configs.empty() ? nullptr : &gemini_uris);
    } catch (...) {}

    // Save to files
    save_to_files(gold_configs, silver_configs);

    auto cycle_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(cycle_end - cycle_start).count();
    LOGI("Cycle completed in %.1f seconds", elapsed);

    last_cycle_ = now_ts();

    if (progress_cb_) progress_cb_("cycle_done", cycle_count_, 0);
    if (status_cb_) status_cb_(get_status().dump());
}

// ---------- Autonomous loop ----------

void HunterOrchestrator::autonomous_loop() {
    int sleep_seconds = static_cast<int>(config_.get_int("sleep_seconds", 300));

    // Initial cycle
    run_cycle();

    while (running_.load()) {
        // Sleep in small intervals for responsiveness to stop()
        for (int i = 0; i < sleep_seconds && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (!running_.load()) break;

        if (now_ts() - last_cycle_ >= sleep_seconds) {
            try {
                run_cycle();
            } catch (...) {
                LOGE("Error in autonomous loop");
                // Back off on error
                for (int i = 0; i < 60 && running_.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }

        // Periodic status report
        if (cycle_count_ > 0 && cycle_count_ % 10 == 0) {
            telegram_reporter_.report_status(balancer_.get_status());
        }
    }
}

// ---------- Lifecycle ----------

void HunterOrchestrator::start() {
    if (running_.load()) return;

    // Ensure runtime directory
    std::string files_dir = config_.get_files_dir();
    if (!files_dir.empty()) {
        ensure_directory(files_dir + "/runtime");
    }

    running_.store(true);

    // Start balancer with cached configs
    auto seed = load_balancer_cache("HUNTER_balancer_cache.json");
    if (!seed.empty()) {
        balancer_.start(&seed);
    } else {
        balancer_.start();
    }

    if (gemini_balancer_) {
        auto gemini_seed = load_balancer_cache("HUNTER_gemini_balancer_cache.json");
        if (!gemini_seed.empty()) {
            gemini_balancer_->start(&gemini_seed);
        } else {
            gemini_balancer_->start();
        }
    }

    // Start autonomous loop in background
    loop_thread_ = std::thread(&HunterOrchestrator::autonomous_loop, this);

    LOGI("Hunter orchestrator started");
}

void HunterOrchestrator::stop() {
    running_.store(false);

    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }

    balancer_.stop();
    if (gemini_balancer_) {
        gemini_balancer_->stop();
    }
    telegram_scraper_.disconnect();

    LOGI("Hunter orchestrator stopped");
}

bool HunterOrchestrator::is_running() const {
    return running_.load();
}

nlohmann::json HunterOrchestrator::get_status() const {
    return {
        {"running", running_.load()},
        {"cycle_count", cycle_count_},
        {"last_cycle", last_cycle_},
        {"balancer", balancer_.get_status()},
        {"validated_configs", validated_configs_.size()}
    };
}

std::string HunterOrchestrator::get_cached_configs() {
    nlohmann::json out = nlohmann::json::array();

    try {
        auto cached = load_balancer_cache();
        if (!cached.empty()) {
            for (const auto& [uri, latency] : cached) {
                std::string ps = uri;
                auto parsed = parser_.parse(uri);
                if (parsed.has_value() && !parsed->ps.empty()) {
                    ps = parsed->ps;
                }
                out.push_back({
                    {"ps", ps},
                    {"latency_ms", static_cast<int>(latency)},
                    {"uri", uri}
                });
            }
            return out.dump();
        }
    } catch (...) {}

    try {
        std::string gold_file = config_.get_string("gold_file");
        if (!gold_file.empty()) {
            auto lines = read_lines(gold_file);
            for (const auto& uri : lines) {
                if (uri.empty()) continue;
                std::string ps = uri;
                auto parsed = parser_.parse(uri);
                if (parsed.has_value() && !parsed->ps.empty()) {
                    ps = parsed->ps;
                }
                out.push_back({
                    {"ps", ps},
                    {"latency_ms", 0},
                    {"uri", uri}
                });
            }
        }
    } catch (...) {}

    return out.dump();
}

} // namespace hunter
