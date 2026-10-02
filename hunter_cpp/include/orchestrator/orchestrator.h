#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <memory>
#include <set>
#include <thread>

#include "core/config.h"
#include "core/models.h"
#include "network/http_client.h"
#include "network/uri_parser.h"
#include "network/continuous_validator.h"
#include "network/aggressive_harvester.h"
#include "network/flexible_fetcher.h"
#include "cache/smart_cache.h"
#include "orchestrator/runtime_cleanup_manager.h"
#include "proxy/proxy_server_manager.h"

namespace hunter {

// Forward declaration
namespace orchestrator { class ThreadManager; }

/**
 * @brief Main Hunter orchestrator — coordinates the fetch/test workflow
 *
 * Manages the full lifecycle: scraping configs from multiple provider
 * sources, testing them for liveness across threads, and maintaining the
 * persistent health database that the GUI reads from.
 */
class HunterOrchestrator {
public:
    explicit HunterOrchestrator(HunterConfig& config);
    ~HunterOrchestrator();

    HunterOrchestrator(const HunterOrchestrator&) = delete;
    HunterOrchestrator& operator=(const HunterOrchestrator&) = delete;

    // ─── Lifecycle ───

    /**
     * @brief Start the orchestrator (blocking — runs ThreadManager)
     */
    void start();

    /**
     * @brief Stop the orchestrator and all managed threads
     */
    void stop();

    // ─── Core Cycle ───

    /**
     * @brief Run one complete hunter cycle (scrape → validate → tier)
     * @return true on success
     */
    bool runCycle();

    /**
     * @brief Detect if internet is censored by testing direct connectivity
     * @return true if censorship detected (no direct internet)
     */
    bool detectCensorship();

    /**
     * @brief Load configs from raw files into database
     * @return Number of configs loaded
     */
    int loadRawConfigFiles();

    /**
     * @brief Test cached configs sequentially until one works
     * @return true if a working config was found
     */
    bool testCachedConfigs();

    /**
     * @brief Load configs from bundle files when GitHub is inaccessible
     * @return Number of configs loaded
     */
    int loadBundleConfigs();

    /**
     * @brief Emergency bootstrap: load raw configs and test until one works
     * @return true if a working config was found
     */
    bool emergencyBootstrap();

    /**
     * @brief Scrape configs from all sources
     * @return Map with "telegram" and "http" lists
     */
    struct ScrapeResult {
        std::vector<std::string> telegram;
        std::vector<std::string> http;
    };
    ScrapeResult scrapeConfigs();

    /**
     * @brief Validate/benchmark a list of configs
     * @return Sorted benchmark results
     */
    std::vector<BenchResult> validateConfigs(
        const std::vector<std::string>& configs,
        const std::string& label = "default",
        int base_port_offset = 0);

    /**
     * @brief Tier configs into gold/silver
     */
    struct TieredConfigs {
        std::vector<BenchResult> gold;
        std::vector<BenchResult> silver;
    };
    TieredConfigs tierConfigs(const std::vector<BenchResult>& results);

    // ─── Component Access ───

    HunterConfig& config() { return config_; }
    network::HttpClient& httpClient() { return http_client_; }
    network::ConfigFetcher& configFetcher() { return config_fetcher_; }
    network::ConfigDatabase* configDb() { return config_db_.get(); }
    cache::SmartCache* cache() { return cache_.get(); }
    proxy::ProxyServerManager& proxyServerManager() { return proxy_server_manager_; }

    // ─── State ───

    int cycleCount() const { return cycle_count_.load(); }
    int lastValidatedCount() const { return last_validated_count_.load(); }
    std::vector<std::pair<std::string, float>>& lastGoodConfigs() { return last_good_configs_; }
    int cachedConfigCount() const;

    // ─── Pause / Resume ───
    void pause();
    void resume();
    bool isPaused() const { return paused_.load(); }

    // ─── Dynamic Speed Controls ───
    struct SpeedProfile {
        int max_threads = 10;       // 1-50
        int test_timeout_s = 5;     // 1-10
        int chunk_size = 15;        // concurrent tests per chunk
        std::string profile_name;   // "low", "medium", "high", "custom"
    };
    void setSpeedProfile(const SpeedProfile& p);
    SpeedProfile getSpeedProfile() const;
    void applyAutoProfile(const std::string& level); // "low", "medium", "high"
    int maxThreads() const { return speed_max_threads_.load(); }
    int testTimeout() const { return speed_test_timeout_.load(); }
    int chunkSize() const { return speed_chunk_size_.load(); }

    // ─── Maintenance ───
    int clearOldConfigs(int max_age_hours = 168); // default 7 days
    int clearAliveConfigs();
    int removeConfigs(const std::set<std::string>& uris);
    void addManualConfigs(const std::vector<std::string>& uris);
    std::string triggerRuntimeCleanup();

    // ─── Dashboard (terminal, optional — GUI reads configDb() directly) ───
    void printStartupBanner();
    void printDashboard();

    /**
     * @brief Download configs from multiple sources with proxy fallback chain
     * @param sources List of source URLs to download from
     * @param proxy App-configured proxy (empty string if none)
     */
    bool downloadConfigsAsync(const std::vector<std::string>& sources, const std::string& proxy);
    bool isDownloadInProgress() const { return download_in_progress_.load(); }

private:
    HunterConfig& config_;

    // Components
    network::HttpClient http_client_;
    network::ConfigFetcher config_fetcher_;
    std::unique_ptr<network::FlexibleFetcher> flexible_fetcher_;
    std::unique_ptr<network::ConfigDatabase> config_db_;
    std::unique_ptr<network::ContinuousValidator> continuous_validator_;
    std::unique_ptr<cache::SmartCache> cache_;
    std::unique_ptr<orchestrator::ThreadManager> thread_manager_;
    std::unique_ptr<orchestrator::RuntimeCleanupManager> cleanup_manager_;
    proxy::ProxyServerManager proxy_server_manager_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> download_in_progress_{false};
    std::mutex download_thread_mutex_;
    std::thread download_thread_;

    // Speed controls (atomic for thread-safe live updates)
    std::atomic<int> speed_max_threads_{10};
    std::atomic<int> speed_test_timeout_{5};
    std::atomic<int> speed_chunk_size_{15};
    std::string speed_profile_name_ = "medium";
    mutable std::mutex speed_mutex_;

    // State
    std::atomic<int> cycle_count_{0};
    std::atomic<int> last_validated_count_{0};
    std::atomic<int> consecutive_scrape_failures_{0};
    std::vector<std::pair<std::string, float>> last_good_configs_;
    std::map<std::string, std::vector<std::pair<std::string, float>>> cached_configs_;
    std::map<std::string, std::string> cached_engine_hints_;
    std::mutex cycle_lock_;
    mutable std::mutex state_mutex_;
    mutable std::mutex status_mutex_;
    double start_time_ = 0.0;

    // Private methods
    void initComponents();
    void saveToFiles(const std::vector<BenchResult>& gold,
                     const std::vector<BenchResult>& silver);
    int appendUniqueLines(const std::string& filepath,
                          const std::vector<std::string>& lines);
    int computeAdaptiveSleep();
};

} // namespace hunter
