#include "orchestrator/thread_manager.h"
#include "orchestrator/orchestrator.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "network/uri_parser.h"

#include <chrono>
#include <deque>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hunter {
namespace orchestrator {

namespace {

struct GitHubRefreshResult {
    int fetched_total = 0;
    int valid_total = 0;
    int invalid_total = 0;
    int appended = 0;
    int added = 0;
    std::string reason = "ok";
};

std::set<std::string>& githubSeenStore() {
    static std::set<std::string> seen;
    return seen;
}

std::mutex& githubSeenMutex() {
    static std::mutex mutex;
    return mutex;
}

bool& githubSeenLoaded() {
    static bool loaded = false;
    return loaded;
}

std::string githubCachePathFor(HunterOrchestrator* orch) {
    std::string base = utils::dirName(orch->config().stateFile());
    if (base.empty()) base = "runtime";
    utils::mkdirRecursive(base);
    return base + "/HUNTER_github_configs_cache.txt";
}

void ensureGitHubSeenLoaded(const std::string& cache_path) {
    std::lock_guard<std::mutex> lock(githubSeenMutex());
    if (githubSeenLoaded()) return;
    auto lines = utils::readLines(cache_path);
    auto& seen = githubSeenStore();
    seen.insert(lines.begin(), lines.end());
    githubSeenLoaded() = true;
}

int appendGitHubCacheLines(const std::string& cache_path, const std::vector<std::string>& configs) {
    std::lock_guard<std::mutex> lock(githubSeenMutex());
    auto& loaded = githubSeenLoaded();
    auto& seen = githubSeenStore();
    if (!loaded) {
        auto lines = utils::readLines(cache_path);
        seen.insert(lines.begin(), lines.end());
        loaded = true;
    }
    std::vector<std::string> new_lines;
    for (const auto& raw : configs) {
        std::string config = utils::trim(raw);
        if (!config.empty() && seen.insert(config).second) {
            new_lines.push_back(config);
        }
    }
    if (new_lines.empty()) return 0;
    std::ofstream f(cache_path, std::ios::app);
    if (!f) return 0;
    for (const auto& c : new_lines) f << c << "\n";
    return (int)new_lines.size();
}

std::set<std::string> filterValidGithubConfigs(const std::set<std::string>& configs, int* invalid_count) {
    std::set<std::string> valid;
    int invalid = 0;
    for (const auto& raw : configs) {
        std::string uri = utils::trim(raw);
        if (uri.empty()) continue;
        auto parsed = network::UriParser::parse(uri);
        if (!parsed.has_value() || !parsed->isValid()) {
            invalid++;
            continue;
        }
        valid.insert(uri);
    }
    if (invalid_count) *invalid_count = invalid;
    return valid;
}

std::vector<int> githubProxyPorts(HunterOrchestrator* orch) {
    std::set<int> seen_ports;
    std::vector<int> ports;
    auto add_port = [&](int port) {
        if (port > 0 && seen_ports.insert(port).second) {
            ports.push_back(port);
        }
    };

    add_port(orch->config().multiproxyPort());
    add_port(orch->config().geminiPort());

    for (const auto& slot : orch->getProvisionedPorts()) {
        if ((slot.alive || slot.pid > 0) && slot.port > 0) {
            add_port(slot.port);
        }
    }

    return ports;
}

GitHubRefreshResult refreshGithubConfigs(HunterOrchestrator* orch, int cap,
                                         int timeout_per, float overall_timeout,
                                         const std::string& tag,
                                         const std::string& log_prefix) {
    GitHubRefreshResult result;
    auto log_result = [&]() {
        std::ostringstream _ls;
        _ls << log_prefix << " reason=" << result.reason
            << " fetched=" << result.fetched_total
            << " valid=" << result.valid_total
            << " invalid=" << result.invalid_total
            << " appended=" << result.appended
            << " db+" << result.added;
        utils::LogRingBuffer::instance().push(_ls.str());
    };

    auto& mgr = HunterTaskManager::instance();
    std::unique_lock<std::timed_mutex> lock(mgr.fetchLock(), std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(10))) {
        result.reason = "scrape_lock_busy";
        log_result();
        return result;
    }

    auto proxy_ports = githubProxyPorts(orch);
    auto github_urls = orch->config().githubUrls();
    auto fetched = orch->configFetcher().fetchGithubConfigs(github_urls, proxy_ports, cap, timeout_per, overall_timeout);
    lock.unlock();

    result.fetched_total = (int)fetched.size();
    if (fetched.empty()) {
        result.reason = "no_configs";
        log_result();
        return result;
    }

    auto valid = filterValidGithubConfigs(fetched, &result.invalid_total);
    result.valid_total = (int)valid.size();
    if (valid.empty()) {
        result.reason = "no_valid_configs";
        log_result();
        return result;
    }

    std::vector<std::string> valid_list(valid.begin(), valid.end());
    result.appended = appendGitHubCacheLines(githubCachePathFor(orch), valid_list);

    auto* cache = orch->cache();
    if (cache) cache->saveConfigs(valid_list, false);

    auto* db = orch->configDb();
    if (db) result.added = db->addConfigs(valid, tag);

    log_result();
    return result;
}

}

// ═══════════════════════════════════════════════════════════════════
// BaseWorker
// ═══════════════════════════════════════════════════════════════════

BaseWorker::BaseWorker(const std::string& worker_name, int interval,
                       std::atomic<bool>& stop_event)
    : name(worker_name), interval_seconds(interval), stop_(stop_event) {
    status_.name = worker_name;
    status_.state = WorkerState::IDLE;
}

BaseWorker::~BaseWorker() {
    if (thread_.joinable()) thread_.join();
}

void BaseWorker::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread(&BaseWorker::runLoop, this);
}

void BaseWorker::join(float timeout) {
    if (thread_.joinable()) {
        thread_.join();
    }
}

WorkerStatus BaseWorker::getStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void BaseWorker::updateExtra(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.extra[key] = value;
}

HardwareSnapshot BaseWorker::getHardware() {
    return HunterTaskManager::instance().getHardware();
}

void BaseWorker::runLoop() {
    utils::LogRingBuffer::instance().push("[" + name + "] Thread started");
    while (!stop_.load()) {
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.state = WorkerState::RUNNING;
        }
        try {
            execute();
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.runs++;
            status_.last_run = utils::nowTimestamp();
            status_.last_error.clear();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.errors++;
            status_.last_error = e.what();
            status_.state = WorkerState::WORKER_ERROR;
            std::cerr << "[" << name << "] Error: " << e.what() << std::endl;
        } catch (...) {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.errors++;
            status_.last_error = "unknown non-std exception";
            status_.state = WorkerState::WORKER_ERROR;
            std::cerr << "[" << name << "] Error: unknown non-std exception" << std::endl;
        }

        if (stop_.load()) break;

        // Sleep with countdown
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.state = WorkerState::SLEEPING;
        }
        int remaining = interval_seconds;
        while (remaining > 0 && !stop_.load()) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.next_run_in = (double)remaining;
            }
            int sleep_ms = std::min(remaining, 5) * 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            remaining -= 5;
        }
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = WorkerState::STOPPED;
    }
}

// ═══════════════════════════════════════════════════════════════════
// ConfigScannerWorker
// ═══════════════════════════════════════════════════════════════════

ConfigScannerWorker::ConfigScannerWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("config_scanner", constants::SCANNER_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_SCANNER_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

void ConfigScannerWorker::execute() {
    auto hw = getHardware();
    { std::ostringstream _ls; _ls << "[Scanner] Starting cycle (mode=" << (int)hw.mode
              << ", RAM=" << hw.ram_percent << "%, workers=" << hw.io_pool_size << ")";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    updateExtra("mode", std::to_string((int)hw.mode));
    updateExtra("ram_percent", std::to_string(hw.ram_percent));

    orch_->config().set("max_total", hw.max_configs);
    orch_->config().set("max_workers", hw.io_pool_size);

    bool ran = orch_->runCycle();
    updateExtra("last_cycle_ok", ran ? "true" : "false");
    updateExtra("validated", std::to_string(orch_->lastValidatedCount()));

    // Adaptive interval
    // Default: continuous scanning (can be disabled via env HUNTER_CONTINUOUS=false)
    // If user explicitly overrides interval via env, do not auto-adjust.
    const int env_interval = HunterConfig::getEnvInt("HUNTER_SCANNER_INTERVAL_S", -1);
    if (env_interval > 0) {
        interval_seconds = env_interval;
        return;
    }
    const bool continuous = HunterConfig::getEnvBool("HUNTER_CONTINUOUS", true);
    if (!continuous) {
        if (orch_->lastValidatedCount() == 0) {
            interval_seconds = 600;
        } else if (orch_->lastValidatedCount() < 5) {
            interval_seconds = 900;
        } else {
            interval_seconds = constants::SCANNER_INTERVAL_S;
        }
        return;
    }

    // Continuous mode: scale frequency with resource mode.
    int base = 120;
    switch (hw.mode) {
        case ResourceMode::NORMAL: base = 60; break;
        case ResourceMode::MODERATE: base = 75; break;
        case ResourceMode::SCALED: base = 90; break;
        case ResourceMode::CONSERVATIVE: base = 120; break;
        case ResourceMode::REDUCED: base = 180; break;
        case ResourceMode::MINIMAL: base = 240; break;
        case ResourceMode::ULTRA_MINIMAL: base = 300; break;
    }
    // If we didn't validate anything, back off a bit.
    if (orch_->lastValidatedCount() == 0) base = std::max(base, 180);
    interval_seconds = base;
}

// ═══════════════════════════════════════════════════════════════════
// TelegramPublisherWorker
// ═══════════════════════════════════════════════════════════════════

TelegramPublisherWorker::TelegramPublisherWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("telegram_publisher", constants::PUBLISHER_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_PUBLISHER_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

void TelegramPublisherWorker::execute() {
    auto* reporter = orch_->botReporter();
    if (!reporter || !reporter->isConfigured()) return;

    // Collect URIs from gold/silver files
    auto gold_uris = utils::readLines(orch_->config().goldFile());
    if (gold_uris.empty()) return;

    int max_lines = orch_->config().getInt("telegram_publish_max_lines", 50);
    if ((int)gold_uris.size() > max_lines)
        gold_uris.resize(max_lines);

    bool ok = reporter->reportConfigFiles(gold_uris);
    updateExtra("published", ok ? "true" : "false");
    updateExtra("count", std::to_string(gold_uris.size()));
}

// ═══════════════════════════════════════════════════════════════════
// BalancerWorker
// ═══════════════════════════════════════════════════════════════════

BalancerWorker::BalancerWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("balancer", constants::BALANCER_CHECK_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_BALANCER_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

void BalancerWorker::execute() {
    auto* bal = orch_->balancer();
    if (!bal) return;

    auto status = bal->getStatus();
    updateExtra("port", std::to_string(status.port));
    updateExtra("backends", std::to_string(status.backend_count));
    updateExtra("healthy", std::to_string(status.healthy_count));
    updateExtra("running", status.running ? "true" : "false");

    // Check if port responds, restart if needed
    if (status.running && !utils::isPortAlive(status.port, 2000)) {
        utils::LogRingBuffer::instance().push("[Balancer] Port not responding, refreshing...");
        // Balancer health monitor handles this internally
    }
}

// ═══════════════════════════════════════════════════════════════════
// HealthMonitorWorker
// ═══════════════════════════════════════════════════════════════════

HealthMonitorWorker::HealthMonitorWorker(std::atomic<bool>& stop,
                                          std::vector<BaseWorker*> all_workers)
    : BaseWorker("health_monitor", constants::HEALTH_MONITOR_INTERVAL_S, stop),
      workers_(std::move(all_workers)) {}

void HealthMonitorWorker::execute() {
    auto hw = HunterTaskManager::instance().getHardware();
    updateExtra("ram_percent", std::to_string(hw.ram_percent));
    updateExtra("cpu_count", std::to_string(hw.cpu_count));
    updateExtra("mode", std::to_string((int)hw.mode));

    // Check worker health
    int running = 0, errors = 0;
    for (auto* w : workers_) {
        auto st = w->getStatus();
        if (st.state == WorkerState::RUNNING || st.state == WorkerState::SLEEPING) running++;
        if (st.state == WorkerState::WORKER_ERROR) errors++;
    }
    updateExtra("workers_running", std::to_string(running));
    updateExtra("workers_error", std::to_string(errors));

    // Memory pressure GC
    if (hw.ram_percent >= 90) {
        HunterTaskManager::instance().maybeResize();
    }
}

// ═══════════════════════════════════════════════════════════════════
// HarvesterWorker
// ═══════════════════════════════════════════════════════════════════

HarvesterWorker::HarvesterWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("harvester", constants::HARVESTER_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_HARVESTER_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

void HarvesterWorker::execute() {
    if (first_run_) {
        first_run_ = false;
        { std::ostringstream _ls; _ls << "[Harvester] Initial delay " << constants::HARVESTER_INITIAL_DELAY_S << "s";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        int remaining = constants::HARVESTER_INITIAL_DELAY_S;
        while (remaining > 0 && !stop_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(std::min(remaining, 5)));
            remaining -= 5;
        }
        if (stop_.load()) return;
    }

    auto* db = orch_->configDb();
    if (!db) return;

    auto& mgr = HunterTaskManager::instance();
    std::unique_lock<std::timed_mutex> lock(mgr.fetchLock(), std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(10))) {
        utils::LogRingBuffer::instance().push("[Harvester] Fetch lock busy, skipping");
        return;
    }

    network::AggressiveHarvester harvester({
        orch_->config().multiproxyPort(),
        orch_->config().geminiPort()
    });

    const int harvest_timeout_s = HunterConfig::getEnvInt("HUNTER_HARVEST_TIMEOUT_S", 60);
    auto configs = harvester.harvest((float)harvest_timeout_s);
    lock.unlock();

    if (!configs.empty()) {
        int added = db->addConfigs(configs, "harvest");
        harvest_count_++;
        updateExtra("last_count", std::to_string(configs.size()));
        updateExtra("new_added", std::to_string(added));
        updateExtra("db_size", std::to_string(db->size()));
        { std::ostringstream _ls; _ls << "[Harvester] Got " << configs.size() << " configs, " << added << " new";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    // In continuous mode, harvest more frequently (still respecting env override if set via constructor)
    const int env_interval = HunterConfig::getEnvInt("HUNTER_HARVESTER_INTERVAL_S", -1);
    const bool continuous = HunterConfig::getEnvBool("HUNTER_CONTINUOUS", true);
    if (continuous && env_interval <= 0) {
        interval_seconds = std::min(interval_seconds, 900);
    }
}

// ═══════════════════════════════════════════════════════════════════
// GitHubDownloaderWorker
// ═══════════════════════════════════════════════════════════════════

GitHubDownloaderWorker::GitHubDownloaderWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("github_bg", constants::GITHUB_BG_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

std::string GitHubDownloaderWorker::cacheFile() {
    if (!cache_path_.empty()) return cache_path_;
    cache_path_ = githubCachePathFor(orch_);
    return cache_path_;
}

void GitHubDownloaderWorker::loadSeen() {
    ensureGitHubSeenLoaded(cacheFile());
}

int GitHubDownloaderWorker::appendNew(const std::vector<std::string>& configs) {
    loadSeen();
    return appendGitHubCacheLines(cacheFile(), configs);
}

void GitHubDownloaderWorker::execute() {
    interval_seconds = constants::GITHUB_BG_INTERVAL_S;

    bool enabled = HunterConfig::getEnvBool("HUNTER_GITHUB_BG_ENABLED", true);
    if (!enabled) {
        updateExtra("reason", "disabled");
        utils::LogRingBuffer::instance().push("[GitHubBG] Disabled");
        return;
    }

    if (first_run_) {
        first_run_ = false;
        updateExtra("reason", "initial_delay");
        { std::ostringstream _ls; _ls << "[GitHubBG] Initial delay " << constants::GITHUB_BG_INITIAL_DELAY_S << "s";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        int remaining = constants::GITHUB_BG_INITIAL_DELAY_S;
        while (remaining > 0 && !stop_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(std::min(remaining, 5)));
            remaining -= 5;
        }
        if (stop_.load()) return;
    }

    // Clear stale reason
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.extra.erase("reason");
    }

    int cap = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_CAP", constants::DEFAULT_GITHUB_BG_CAP);
    cap = std::max(200, std::min(150000, cap));
    { std::ostringstream _ls; _ls << "[GitHubBG] Fetching (cap=" << cap << ")";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    auto refresh = refreshGithubConfigs(orch_, cap, 9, 40.0f, "github_bg", "[GitHubBG]");
    if (refresh.reason == "scrape_lock_busy") {
        updateExtra("last_count", "0");
        updateExtra("valid_count", "0");
        updateExtra("invalid_filtered", "0");
        updateExtra("reason", refresh.reason);
        interval_seconds = 60;
        return;
    }

    if (refresh.fetched_total == 0 || refresh.valid_total == 0) {
        updateExtra("last_count", std::to_string(refresh.fetched_total));
        updateExtra("valid_count", std::to_string(refresh.valid_total));
        updateExtra("invalid_filtered", std::to_string(refresh.invalid_total));
        updateExtra("new_appended", std::to_string(refresh.appended));
        updateExtra("new_added_db", std::to_string(refresh.added));
        updateExtra("reason", refresh.reason);
        interval_seconds = 300;
        return;
    }

    auto* db = orch_->configDb();

    // Get tag stats
    network::ConfigDatabase::TagStats tag_stats;
    if (db) tag_stats = db->getTagStats("github_bg");

    download_count_++;
    updateExtra("last_count", std::to_string(refresh.fetched_total));
    updateExtra("valid_count", std::to_string(refresh.valid_total));
    updateExtra("invalid_filtered", std::to_string(refresh.invalid_total));
    updateExtra("new_appended", std::to_string(refresh.appended));
    updateExtra("new_added_db", std::to_string(refresh.added));
    updateExtra("db_size", std::to_string(db ? db->size() : 0));
    updateExtra("github_total", std::to_string(tag_stats.total));
    updateExtra("github_alive", std::to_string(tag_stats.alive));
    updateExtra("github_avg_latency_ms", std::to_string(tag_stats.avg_latency_ms));
    updateExtra("github_untested", std::to_string(tag_stats.untested));
    updateExtra("github_needs_retest", std::to_string(tag_stats.needs_retest));
    updateExtra("downloads", std::to_string(download_count_));

    { std::ostringstream _ls; _ls << "[GitHubBG] Accepted " << refresh.valid_total << " valid configs from "
              << refresh.fetched_total << " fetched, appended " << refresh.appended << ", db+" << refresh.added;
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // In continuous mode, check GitHub sources more frequently (still can be overridden by env)
    const int env_interval = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_INTERVAL_S", -1);
    const bool continuous2 = HunterConfig::getEnvBool("HUNTER_CONTINUOUS", true);
    if (continuous2 && env_interval <= 0) {
        interval_seconds = std::min(interval_seconds, 300);
    }
}

// ═══════════════════════════════════════════════════════════════════
// ValidatorWorker
// ═══════════════════════════════════════════════════════════════════

ValidatorWorker::ValidatorWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("validator", constants::VALIDATOR_INTERVAL_S, stop), orch_(orch) {
    const int env_interval = HunterConfig::getEnvInt("HUNTER_VALIDATOR_INTERVAL_S", -1);
    if (env_interval > 0) interval_seconds = env_interval;
}

void ValidatorWorker::execute() {
    auto* db = orch_->configDb();
    if (!db) {
        utils::LogRingBuffer::instance().push("[Validator] WARNING: ConfigDB is null");
        return;
    }

    auto hw = HunterTaskManager::instance().getHardware();
    int batch_size = std::max(5, std::min(50, orch_->chunkSize() * 2));
    if (hw.ram_percent >= 95.0f) batch_size = std::min(batch_size, 8);
    else if (hw.ram_percent >= 90.0f) batch_size = std::min(batch_size, 12);
    else if (hw.ram_percent >= 80.0f) batch_size = std::min(batch_size, 20);

    int timeout_s = orch_->testTimeout();
    int max_concurrent = orch_->maxThreads();
    network::ContinuousValidator validator(*db, batch_size, timeout_s, max_concurrent);
    auto [tested, passed] = validator.validateBatch();

    { std::ostringstream _ls; _ls << "[Validator] Batch: tested=" << tested << " passed=" << passed;
      utils::LogRingBuffer::instance().push(_ls.str()); }

    updateExtra("last_tested", std::to_string(tested));
    updateExtra("last_passed", std::to_string(passed));

    auto stats = validator.getStats();
    updateExtra("db_total", std::to_string(stats.db.total));
    updateExtra("db_alive", std::to_string(stats.db.alive));
    updateExtra("db_tested_unique", std::to_string(stats.db.tested_unique));
    updateExtra("db_untested_unique", std::to_string(stats.db.untested_unique));
    updateExtra("db_stale_unique", std::to_string(stats.db.stale_unique));
    updateExtra("total_tested", std::to_string(stats.db.total_tested));
    updateExtra("total_passed", std::to_string(stats.db.total_passed));

    static uint64_t last_ms = 0;
    static int last_total_tests = 0;
    static std::deque<std::string> history;

    uint64_t now_ms = utils::nowMs();
    double now_ts = utils::nowTimestamp();
    double dt = (last_ms == 0) ? 0.0 : (double)(now_ms - last_ms) / 1000.0;
    double rate_tests = 0.0;
    if (dt > 0.0) {
        int d = stats.db.total_tested - last_total_tests;
        if (d < 0) d = 0;
        rate_tests = (double)d / dt;
    }
    int pending_unique = stats.db.untested_unique + stats.db.stale_unique;
    double eta_s = 0.0;
    if (rate_tests > 0.0001 && pending_unique > 0) {
        eta_s = (double)pending_unique / rate_tests;
    }

    updateExtra("rate_per_s", std::to_string(rate_tests));
    updateExtra("pending_unique", std::to_string(pending_unique));
    updateExtra("eta_s", std::to_string(eta_s));

    {
        std::ostringstream p;
        p << "{\"ts\":" << now_ts
          << ",\"tested_unique\":" << stats.db.tested_unique
          << ",\"alive\":" << stats.db.alive
          << ",\"untested_unique\":" << stats.db.untested_unique
          << ",\"stale_unique\":" << stats.db.stale_unique
          << ",\"pending_unique\":" << pending_unique
          << ",\"rate_per_s\":" << rate_tests
          << "}";
        history.push_back(p.str());
        while (history.size() > 180) history.pop_front();
    }

    last_ms = now_ms;
    last_total_tests = stats.db.total_tested;

    std::string base = utils::dirName(orch_->config().stateFile());
    if (base.empty()) base = "runtime";
    std::string status_path = base + "/HUNTER_status.json";

    // Debug logging for status file path
    static bool first_status_write = true;
    if (first_status_write) {
        utils::LogRingBuffer::instance().push("[Validator] Status path: " + status_path);
        first_status_write = false;
    }

    std::ostringstream hist;
    hist << "[";
    bool first = true;
    for (auto& row : history) {
        if (!first) hist << ",";
        first = false;
        hist << row;
    }
    hist << "]";

    auto all_records = db->getAllRecords(500);
    static bool had_alive_configs = false;
    static uint64_t last_live_refresh_attempt_ms = 0;
    static uint64_t last_live_refresh_success_ms = 0;

    // Build alive_configs JSON with full health details
    std::ostringstream alive_json;
    alive_json << "[";
    bool alive_first = true;
    int alive_count_for_refresh = 0;
    for (auto& rec : all_records) {
        if (!alive_first) alive_json << ",";
        alive_first = false;
        utils::JsonBuilder item;
        item.add("uri", rec.uri)
            .add("latency_ms", rec.latency_ms)
            .add("alive", rec.alive)
            .add("first_seen", rec.first_seen)
            .add("last_tested", rec.last_tested)
            .add("last_alive", rec.last_alive_time)
            .add("total_tests", rec.total_tests)
            .add("total_passes", rec.total_passes)
            .add("consecutive_fails", rec.consecutive_fails)
            .add("tag", rec.tag);
        alive_json << item.build();
        if (rec.alive) alive_count_for_refresh++;
    }
    alive_json << "]";

    utils::JsonBuilder dbj;
    dbj.add("total", stats.db.total)
       .add("alive", stats.db.alive)
       .add("tested_unique", stats.db.tested_unique)
       .add("untested_unique", stats.db.untested_unique)
       .add("stale_unique", stats.db.stale_unique)
       .add("avg_latency_ms", stats.db.avg_latency_ms)
       .add("total_tests", stats.db.total_tested)
       .add("total_passes", stats.db.total_passed);

    utils::JsonBuilder valj;
    valj.add("last_tested", tested)
       .add("last_passed", passed)
       .add("interval_s", interval_seconds)
       .add("rate_per_s", rate_tests);

    utils::JsonBuilder root;
    root.add("ts", now_ts)
        .addRaw("db", dbj.build())
        .addRaw("validator", valj.build())
        .add("pending_unique", pending_unique)
        .add("eta_seconds", eta_s)
        .addRaw("alive_configs", alive_json.str())
        .addRaw("history", hist.str());

    bool write_ok = utils::saveJsonFile(status_path, root.build());
    { std::ostringstream _ls; _ls << "[Validator] Status: " << (write_ok ? "OK" : "FAIL")
              << " db=" << stats.db.total << " alive=" << stats.db.alive
              << " records=" << all_records.size();
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // In continuous mode, validate more frequently (still can be overridden by env)
    const int env_interval = HunterConfig::getEnvInt("HUNTER_VALIDATOR_INTERVAL_S", -1);
    const bool continuous3 = HunterConfig::getEnvBool("HUNTER_CONTINUOUS", true);
    if (continuous3 && env_interval <= 0) {
        interval_seconds = std::min(interval_seconds, 2);
    }

    // Build healthy pair list for balancer/file operations
    std::vector<std::pair<std::string, float>> healthy_pairs;
    for (auto& rec : all_records) {
        if (rec.alive && rec.latency_ms > 0)
            healthy_pairs.emplace_back(rec.uri, rec.latency_ms);
    }

    const bool has_live_now = alive_count_for_refresh > 0;
    const bool should_refresh_after_live = has_live_now && (!had_alive_configs ||
        (last_live_refresh_success_ms == 0 && (last_live_refresh_attempt_ms == 0 || (now_ms - last_live_refresh_attempt_ms) > 120000)));

    // Write files FIRST so Flutter sees them immediately, then update balancers
    if (stats.db.alive > 0) {
        const auto& healthy = healthy_pairs;
        std::cout << "[Validator] Alive=" << stats.db.alive << " healthy_from_db=" << healthy.size() << std::endl;
        std::cout.flush();
        if (!healthy.empty()) {
            // ── 1. Write gold/silver files immediately ──
            std::string gold_file = orch_->config().goldFile();
            std::string silver_file = orch_->config().silverFile();
            std::cout << "[Validator] Writing gold=" << gold_file << " silver=" << silver_file << std::endl;
            std::cout.flush();
            std::vector<std::string> gold_uris;
            for (auto& [uri, lat] : healthy) {
                gold_uris.push_back(uri);
            }
            if (!gold_uris.empty()) {
                int added = utils::appendUniqueLines(gold_file, gold_uris);
                std::cout << "[Validator] Gold: wrote " << added << " new (total " << gold_uris.size() << ") to " << gold_file << std::endl;
                std::cout.flush();
            }

            // ── 2. Write balancer cache JSON ──
            std::ostringstream bcj;
            bcj << "{\"configs\":[";
            bool bfirst = true;
            for (auto& [uri, lat] : healthy) {
                if (!bfirst) bcj << ",";
                bfirst = false;
                bcj << "{\"uri\":\"" << uri << "\",\"latency_ms\":" << lat << "}";
            }
            bcj << "]}";
            bool bc_ok = utils::saveJsonFile(base + "/HUNTER_balancer_cache.json", bcj.str());
            std::cout << "[Validator] Balancer cache: " << (bc_ok ? "OK" : "FAIL") << " (" << healthy.size() << " configs)" << std::endl;
            std::cout.flush();

            // ── 3. Update balancers (may be slow, do AFTER file writes) ──
            if (orch_->balancer()) {
                orch_->balancer()->updateAvailableConfigs(healthy, true);
            }
            if (orch_->geminiBalancer() && healthy.size() > 1) {
                int half = std::max(1, (int)healthy.size() / 2);
                std::vector<std::pair<std::string, float>> gemini_configs(
                    healthy.begin() + half, healthy.end());
                orch_->geminiBalancer()->updateAvailableConfigs(gemini_configs, true);
            }

            if (should_refresh_after_live) {
                last_live_refresh_attempt_ms = now_ms;
                int github_cap = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_CAP", constants::DEFAULT_GITHUB_BG_CAP);
                github_cap = std::max(200, std::min(150000, github_cap));
                auto refresh = refreshGithubConfigs(orch_, github_cap, 6, 20.0f, "github_bg", "[Validator->GitHub]");
                updateExtra("live_github_reason", refresh.reason);
                updateExtra("live_github_fetched", std::to_string(refresh.fetched_total));
                updateExtra("live_github_valid", std::to_string(refresh.valid_total));
                updateExtra("live_github_invalid", std::to_string(refresh.invalid_total));
                updateExtra("live_github_appended", std::to_string(refresh.appended));
                updateExtra("live_github_added_db", std::to_string(refresh.added));
                if (refresh.valid_total > 0) {
                    last_live_refresh_success_ms = now_ms;
                }
            }
        } else {
            std::cout << "[Validator] WARNING: alive=" << stats.db.alive << " but getHealthyConfigs returned empty!" << std::endl;
            std::cout.flush();
        }
    }

    had_alive_configs = has_live_now;
}

// ═══════════════════════════════════════════════════════════════════
// DpiPressureWorker
// ═══════════════════════════════════════════════════════════════════

DpiPressureWorker::DpiPressureWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("dpi_pressure", constants::DPI_PRESSURE_INTERVAL_S, stop), orch_(orch) {}

void DpiPressureWorker::execute() {
    security::DpiPressureEngine engine(0.7f);
    auto stats = engine.runPressureCycle();

    updateExtra("tls_ok", std::to_string(stats["tls_probes_ok"]));
    updateExtra("telegram_reachable", std::to_string(stats["telegram_reachable"]));
    updateExtra("cycles", std::to_string(stats["pressure_cycles"]));
}

// ═══════════════════════════════════════════════════════════════════
// ImportWatcherWorker
// ═══════════════════════════════════════════════════════════════════

ImportWatcherWorker::ImportWatcherWorker(HunterOrchestrator* orch, std::atomic<bool>& stop)
    : BaseWorker("import_watcher", 30, stop), orch_(orch) {}

void ImportWatcherWorker::ensureImportDirs() {
    namespace fs = std::filesystem;
    try {
        fs::create_directories("config/import");
        fs::create_directories("config/import/processed");
        fs::create_directories("config/import/invalid");
    } catch (...) {}
}

bool ImportWatcherWorker::isValidProxyUri(const std::string& uri) const {
    if (uri.size() < 10) return false;

    // Must have a supported scheme
    static const std::vector<std::string> schemes = {
        "vmess://", "vless://", "trojan://", "ss://", "ssr://",
        "hysteria2://", "hy2://", "tuic://"
    };
    bool has_scheme = false;
    for (const auto& s : schemes) {
        if (uri.compare(0, s.size(), s) == 0) { has_scheme = true; break; }
    }
    if (!has_scheme) return false;

    // vmess:// must be followed by valid base64 that decodes to JSON
    if (uri.compare(0, 8, "vmess://") == 0) {
        std::string payload = uri.substr(8);
        auto hash = payload.find('#');
        if (hash != std::string::npos) payload = payload.substr(0, hash);
        if (payload.size() < 10) return false;
        std::string decoded = utils::base64Decode(payload);
        if (decoded.empty() || decoded.find('{') == std::string::npos) return false;
        // Must have "add" field (server address)
        if (decoded.find("\"add\"") == std::string::npos) return false;
        return true;
    }

    // For other schemes: scheme://payload — payload must not be empty
    auto sep = uri.find("://");
    if (sep == std::string::npos) return false;
    std::string payload = uri.substr(sep + 3);
    // Remove fragment
    auto hash = payload.find('#');
    if (hash != std::string::npos) payload = payload.substr(0, hash);
    if (payload.size() < 3) return false;

    // vless/trojan: must contain @ (uuid@host:port)
    if (uri.compare(0, 8, "vless://") == 0 || uri.compare(0, 9, "trojan://") == 0) {
        if (payload.find('@') == std::string::npos) return false;
    }

    // ss:// can be base64 encoded or method:password@host:port
    // Just check it's not obviously garbage
    if (uri.compare(0, 5, "ss://") == 0) {
        if (payload.find('@') == std::string::npos) {
            // Might be base64 encoded — try decode
            std::string decoded = utils::base64Decode(payload);
            if (decoded.empty() || decoded.find(':') == std::string::npos) return false;
        }
    }

    return true;
}

void ImportWatcherWorker::execute() {
    namespace fs = std::filesystem;
    ensureImportDirs();

    const std::string import_dir = "config/import";

    // Scan for .txt files in import directory
    std::vector<std::string> files_to_process;
    try {
        for (const auto& entry : fs::directory_iterator(import_dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            // Accept .txt, .conf, .list, .sub, or no extension
            if (ext == ".txt" || ext == ".conf" || ext == ".list" || ext == ".sub" || ext.empty()) {
                files_to_process.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& e) {
        return; // Directory doesn't exist or access error
    }

    if (files_to_process.empty()) return;

    auto* db = orch_->configDb();
    if (!db) return;

    int batch_valid = 0, batch_invalid = 0, batch_duplicate = 0;
    std::set<std::string> valid_configs;
    std::vector<std::string> invalid_lines;

    for (const auto& file_path : files_to_process) {
        std::string filename = fs::path(file_path).filename().string();
        utils::LogRingBuffer::instance().push("[Import] Processing: " + filename);

        auto lines = utils::readLines(file_path);
        if (lines.empty()) {
            utils::LogRingBuffer::instance().push("[Import] " + filename + " is empty, skipping");
            // Move empty file to processed
            try {
                fs::rename(file_path, import_dir + "/processed/" + filename);
            } catch (...) {
                try { fs::remove(file_path); } catch (...) {}
            }
            continue;
        }

        // Also try base64 decode the entire file content (some subscription URLs return base64)
        std::set<std::string> extracted;
        for (const auto& line : lines) {
            std::string trimmed = utils::trim(line);
            if (trimmed.empty()) continue;

            // If line looks like a URI, add directly
            if (trimmed.find("://") != std::string::npos) {
                extracted.insert(trimmed);
            } else {
                // Try base64 decode
                auto decoded_uris = utils::tryDecodeAndExtract(trimmed);
                extracted.insert(decoded_uris.begin(), decoded_uris.end());
            }
        }

        // Also try the whole file as base64
        if (extracted.empty() && lines.size() == 1) {
            auto decoded_uris = utils::tryDecodeAndExtract(lines[0]);
            extracted.insert(decoded_uris.begin(), decoded_uris.end());
        }

        for (const auto& uri : extracted) {
            // Dedup check
            if (seen_uris_.count(uri)) {
                batch_duplicate++;
                continue;
            }

            // Validate URI format
            if (!isValidProxyUri(uri)) {
                batch_invalid++;
                invalid_lines.push_back(uri);
                continue;
            }

            seen_uris_.insert(uri);
            valid_configs.insert(uri);
            batch_valid++;
        }

        // Move processed file
        try {
            std::string dest = import_dir + "/processed/" + filename;
            // If dest exists, add timestamp
            if (fs::exists(dest)) {
                auto now = std::chrono::system_clock::now().time_since_epoch();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
                dest = import_dir + "/processed/" + std::to_string(ms) + "_" + filename;
            }
            fs::rename(file_path, dest);
        } catch (...) {
            try { fs::remove(file_path); } catch (...) {}
        }
    }

    // Save invalid lines for user reference
    if (!invalid_lines.empty()) {
        std::string invalid_file = import_dir + "/invalid/last_invalid.txt";
        std::ofstream f(invalid_file, std::ios::app);
        if (f.is_open()) {
            auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            f << "# --- Import scan at " << std::ctime(&now);
            for (const auto& line : invalid_lines) {
                f << line << "\n";
            }
        }
    }

    // Add valid configs to database
    if (!valid_configs.empty()) {
        int added = db->addConfigs(valid_configs, "import");
        total_imported_ += batch_valid;
        total_invalid_ += batch_invalid;
        total_duplicate_ += batch_duplicate;

        { std::ostringstream _ls; _ls << "[Import] " << files_to_process.size() << " files: "
                  << batch_valid << " valid, " << batch_invalid << " invalid -> " << added << " new";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    } else if (batch_invalid > 0 || batch_duplicate > 0) {
        { std::ostringstream _ls; _ls << "[Import] " << files_to_process.size() << " files: "
                  << "0 valid, " << batch_invalid << " invalid, " << batch_duplicate << " dup";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    updateExtra("total_imported", std::to_string(total_imported_));
    updateExtra("total_invalid", std::to_string(total_invalid_));
    updateExtra("total_duplicate", std::to_string(total_duplicate_));
    updateExtra("db_size", std::to_string(db->size()));
}

// ═══════════════════════════════════════════════════════════════════
// ThreadManager
// ═══════════════════════════════════════════════════════════════════

ThreadManager::ThreadManager(HunterOrchestrator* orch) : orch_(orch) {
    scanner_ = std::make_unique<ConfigScannerWorker>(orch, stop_event_);
    telegram_ = std::make_unique<TelegramPublisherWorker>(orch, stop_event_);
    balancer_ = std::make_unique<BalancerWorker>(orch, stop_event_);
    harvester_ = std::make_unique<HarvesterWorker>(orch, stop_event_);
    github_downloader_ = std::make_unique<GitHubDownloaderWorker>(orch, stop_event_);
    validator_ = std::make_unique<ValidatorWorker>(orch, stop_event_);
    dpi_pressure_ = std::make_unique<DpiPressureWorker>(orch, stop_event_);
    import_watcher_ = std::make_unique<ImportWatcherWorker>(orch, stop_event_);

    all_workers_ = {
        scanner_.get(), telegram_.get(), balancer_.get(),
        harvester_.get(), github_downloader_.get(), validator_.get(),
        dpi_pressure_.get(), import_watcher_.get()
    };

    health_ = std::make_unique<HealthMonitorWorker>(stop_event_, all_workers_);
    all_workers_.insert(all_workers_.begin(), health_.get());
}

ThreadManager::~ThreadManager() {
    stopAll();
}

void ThreadManager::startAll() {
    auto hw = HunterTaskManager::instance().getHardware();
    std::cout << "ThreadManager starting - " << hw.cpu_count << " CPUs, "
              << hw.ram_total_gb << " GB RAM (" << hw.ram_percent << "% used)"
              << std::endl;

    for (auto* w : all_workers_) {
        try { w->start(); } catch (const std::exception& e) {
            std::cerr << "Failed to start " << w->name << ": " << e.what() << std::endl;
        }
    }
    std::cout << "All " << all_workers_.size() << " workers started" << std::endl;
}

void ThreadManager::stopAll(float timeout) {
    std::cout << "ThreadManager: stopping all workers..." << std::endl;
    stop_event_ = true;
    for (auto* w : all_workers_) {
        try { w->join(timeout / (float)all_workers_.size()); } catch (...) {}
    }
    std::cout << "ThreadManager: all workers stopped" << std::endl;
}

ThreadManager::Status ThreadManager::getStatus() const {
    Status s;
    auto hw = HunterTaskManager::instance().getHardware();
    s.hardware.cpu_count = hw.cpu_count;
    s.hardware.cpu_percent = hw.cpu_percent;
    s.hardware.ram_total_gb = hw.ram_total_gb;
    s.hardware.ram_used_gb = hw.ram_used_gb;
    s.hardware.ram_percent = hw.ram_percent;
    static const char* mode_names[] = {
        "NORMAL","MODERATE","SCALED","CONSERVATIVE","REDUCED","MINIMAL","ULTRA-MINIMAL"
    };
    s.hardware.mode = mode_names[std::min((int)hw.mode, 6)];
    s.hardware.thread_count = 0; // Would need platform-specific thread count

    for (auto* w : all_workers_) {
        s.workers[w->name] = w->getStatus();
    }
    return s;
}

} // namespace orchestrator
} // namespace hunter
