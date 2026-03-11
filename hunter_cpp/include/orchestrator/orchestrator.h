#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <memory>
#include <set>

#include "core/config.h"
#include "core/models.h"
#include "network/http_client.h"
#include "network/uri_parser.h"
#include "network/continuous_validator.h"
#include "network/aggressive_harvester.h"
#include "network/flexible_fetcher.h"
#include "proxy/load_balancer.h"
#include "proxy/xray_manager.h"
#include "testing/benchmark.h"
#include "security/dpi_evasion.h"
#include "security/obfuscation.h"
#include "telegram/bot_reporter.h"
#include "cache/smart_cache.h"

namespace hunter {

// Forward declaration
namespace orchestrator { class ThreadManager; }

/**
 * @brief Main Hunter orchestrator — coordinates the full autonomous workflow
 * 
 * Manages the complete lifecycle: scraping configs from multiple sources,
 * benchmarking them, feeding working configs to the load balancer,
 * publishing results to Telegram, and maintaining persistent caches.
 */
class HunterOrchestrator {
public:
    explicit HunterOrchestrator(HunterConfig& config);
    ~HunterOrchestrator();

    HunterOrchestrator(const HunterOrchestrator&) = delete;
    HunterOrchestrator& operator=(const HunterOrchestrator&) = delete;

    // ─── Lifecycle ───

    /**
     * @brief Start the orchestrator (blocking — runs ThreadManager + dashboard)
     */
    void start();

    /**
     * @brief Stop the orchestrator and all managed threads
     */
    void stop();

    // ─── Core Cycle ───

    /**
     * @brief Run one complete hunter cycle (scrape → benchmark → balance → publish)
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
     * @brief Kill processes occupying required ports
     */
    void killPortOccupants();

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
    proxy::MultiProxyServer* balancer() { return balancer_.get(); }
    proxy::MultiProxyServer* geminiBalancer() { return gemini_balancer_.get(); }
    proxy::XRayManager& xrayManager() { return xray_manager_; }
    testing::ProxyBenchmark& benchmarker() { return benchmarker_; }
    security::DpiEvasionOrchestrator* dpiEvasion() { return dpi_evasion_.get(); }
    telegram::BotReporter* botReporter() { return bot_reporter_.get(); }
    cache::SmartCache* cache() { return cache_.get(); }

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
    void addManualConfigs(const std::vector<std::string>& uris);

    // ─── Dashboard ───
    void printStartupBanner();
    void printDashboard();
    void writeStatusFile(const std::string& phase = "", int last_tested = 0, int last_passed = 0);

    // ─── Real-time UI Communication (stdin/stdout JSON lines) ───
    /** Process a single JSON command line received from stdin (Flutter UI) */
    void processStdinCommand(const std::string& json_line);
    /** Emit ##STATUS## JSON line to stdout for Flutter UI to parse in real-time */
    void emitStatusJson(const std::string& phase = "running");

    // ─── Port Provisioning (2901-2999) ───
    void provisionPorts();
    void stopProvisionedPorts();
    void refreshProvisionedPorts();
    struct PortSlot {
        int port = 0;          // SOCKS port
        int http_port = 0;     // HTTP port (port + 100)
        std::string uri;
        int pid = -1;
        bool alive = false;
        float latency_ms = 0.0f;
        double last_health_check = 0.0;
        int consecutive_failures = 0;
    };
    std::vector<PortSlot> getProvisionedPorts() const;

private:
    HunterConfig& config_;

    // Components
    network::HttpClient http_client_;
    network::ConfigFetcher config_fetcher_;
    std::unique_ptr<network::FlexibleFetcher> flexible_fetcher_;
    std::unique_ptr<network::ConfigDatabase> config_db_;
    std::unique_ptr<network::ContinuousValidator> continuous_validator_;
    proxy::XRayManager xray_manager_;
    testing::ProxyBenchmark benchmarker_;
    std::unique_ptr<proxy::MultiProxyServer> balancer_;
    std::unique_ptr<proxy::MultiProxyServer> gemini_balancer_;
    std::unique_ptr<security::DpiEvasionOrchestrator> dpi_evasion_;
    std::unique_ptr<security::ObfuscationEngine> obfuscation_;
    std::unique_ptr<telegram::BotReporter> bot_reporter_;
    std::unique_ptr<cache::SmartCache> cache_;
    std::unique_ptr<orchestrator::ThreadManager> thread_manager_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};

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
    std::mutex cycle_lock_;
    std::mutex state_mutex_;
    double start_time_ = 0.0;

    // Port provisioning (2901-2999)
    static constexpr int PROVISION_PORT_BASE = 2901;
    static constexpr int PROVISION_PORT_MAX = 2999;
    static constexpr int PROVISION_PORT_COUNT = 20;
    std::vector<PortSlot> provisioned_ports_;
    mutable std::mutex provision_mutex_;

    // Private methods
    void initComponents();
    void loadBalancerCache(const std::string& name, proxy::MultiProxyServer* bal);
    void saveBalancerCache(const std::string& name,
                           const std::vector<std::pair<std::string, float>>& configs);
    std::string balancerCachePath(const std::string& name) const;
    void saveToFiles(const std::vector<BenchResult>& gold,
                     const std::vector<BenchResult>& silver);
    int appendUniqueLines(const std::string& filepath,
                          const std::vector<std::string>& lines);
    int computeAdaptiveSleep();
};

} // namespace hunter
