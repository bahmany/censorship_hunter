#include "orchestrator/thread_manager.h"
#include "orchestrator/orchestrator.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "network/proxy_tester.h"
#include "network/uri_parser.h"

#include <chrono>
#include <deque>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <algorithm>

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
    // Aliases for compatibility
    int total_fetched = 0;
    int valid_configs = 0;
    int sources_found = 0;
    double timestamp = 0.0;
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

    // Bound the in-memory seen set to prevent unbounded growth. The cache
    // file on disk is the persistent record; the in-memory set is only for
    // dedup within a session. When it exceeds the cap, trim oldest entries
    // (std::set is ordered, so begin() = oldest). This keeps memory bounded
    // while still deduplicating recent configs. The ConfigDB has its own
    // independent dedup, so trimming the seen set only risks re-appending
    // a few duplicates to the cache file — harmless.
    static constexpr size_t SEEN_MAX = 200000;
    if (seen.size() > SEEN_MAX) {
        size_t to_remove = seen.size() - (SEEN_MAX * 3 / 4);  // trim to 75%
        auto it = seen.begin();
        for (size_t i = 0; i < to_remove && it != seen.end(); ++i) {
            it = seen.erase(it);
        }
    }

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

    std::vector<int> proxy_ports; // no local proxy balancer anymore; direct fetch only
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

std::string runtimeBasePathFor(HunterOrchestrator* orch) {
    std::string base = utils::dirName(orch->config().stateFile());
    if (base.empty()) base = "runtime";
    utils::mkdirRecursive(base);
    return base;
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

void BaseWorker::requestStop() {
    stop_wait_cv_.notify_all();
}

WorkerStatus BaseWorker::getStatus() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

void BaseWorker::setPauseCallback(std::function<bool()> callback) {
    pause_callback_ = std::move(callback);
}

void BaseWorker::updateExtra(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.extra[key] = value;
}

HardwareSnapshot BaseWorker::getHardware() {
    return HunterTaskManager::instance().getHardware();
}

bool BaseWorker::sleepInterruptible(std::chrono::milliseconds duration) {
    std::unique_lock<std::mutex> lock(stop_wait_mutex_);
    return stop_wait_cv_.wait_for(lock, duration, [this] { return stop_.load(); });
}

void BaseWorker::runLoop() {
    utils::LogRingBuffer::instance().push("[" + name + "] Thread started");
    while (!stop_.load()) {
        while (!stop_.load() && pause_callback_ && pause_callback_()) {
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.state = WorkerState::SLEEPING;
                status_.next_run_in = 0.0;
                status_.extra["paused"] = "true";
            }
            sleepInterruptible(std::chrono::milliseconds(250));
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.extra["paused"] = "false";
        }
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
            if (pause_callback_ && pause_callback_()) {
                while (!stop_.load() && pause_callback_ && pause_callback_()) {
                    {
                        std::lock_guard<std::mutex> lock(status_mutex_);
                        status_.state = WorkerState::SLEEPING;
                        status_.next_run_in = 0.0;
                        status_.extra["paused"] = "true";
                    }
                    sleepInterruptible(std::chrono::milliseconds(250));
                }
                {
                    std::lock_guard<std::mutex> lock(status_mutex_);
                    status_.extra["paused"] = "false";
                }
            }
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                status_.next_run_in = (double)remaining;
            }
            int sleep_ms = std::min(remaining, 1) * 1000;
            sleepInterruptible(std::chrono::milliseconds(sleep_ms));
            remaining -= 1;
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
    auto task_metrics = HunterTaskManager::instance().getMetrics();
    { std::ostringstream _ls; _ls << "[Scanner] Starting cycle (mode=" << (int)hw.mode
              << ", RAM=" << hw.ram_percent << "% (free=" << std::fixed << std::setprecision(1)
              << hw.ram_free_gb << "GB, budget=20%=" << hw.ram_budget_gb << "GB)"
              << ", CPU=" << std::setprecision(0) << hw.cpu_percent << "% (budget=20%="
              << std::setprecision(1) << hw.cpu_budget_cores << " cores)"
              << ", workers=" << hw.io_pool_size << ", max_configs=" << hw.max_configs << ")";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    updateExtra("mode", std::to_string((int)hw.mode));
    updateExtra("ram_percent", std::to_string(hw.ram_percent));
    updateExtra("io_pending", std::to_string(task_metrics.io_pending));
    updateExtra("io_active", std::to_string(task_metrics.io_active));
    updateExtra("cpu_pending", std::to_string(task_metrics.cpu_pending));
    updateExtra("cpu_active", std::to_string(task_metrics.cpu_active));

    orch_->config().set("max_total", hw.max_configs);
    orch_->config().set("max_workers", hw.io_pool_size);

    const double now_ts = utils::nowTimestamp();
    updateExtra("last_cycle_ts", std::to_string(now_ts));
    bool ran = orch_->runCycle();
    updateExtra("last_cycle_ok", ran ? "true" : "false");
    updateExtra("validated", std::to_string(orch_->lastValidatedCount()));
    if (ran) {
        updateExtra("last_cycle_success_ts", std::to_string(now_ts));
    }

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
    // If the DB has many untested configs, run more frequently to drain
    // the backlog faster. The validator worker handles per-batch testing,
    // but the scanner's runCycle also injects untested configs into the
    // validation pipeline and scrapes new sources.
    if (auto* db = orch_->configDb()) {
        auto stats = db->getStats();
        if (stats.untested_unique > 10000) {
            base = std::min(base, 30);  // drain large backlogs fast
        } else if (stats.untested_unique > 1000) {
            base = std::min(base, 45);
        }
    }
    interval_seconds = base;
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
        { std::ostringstream _ls; _ls << "[Harvester] Initial startup delay: " << constants::HARVESTER_INITIAL_DELAY_S << "s to avoid CPU spikes";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        int remaining = constants::HARVESTER_INITIAL_DELAY_S;
        while (remaining > 0 && !stop_.load()) {
            sleepInterruptible(std::chrono::seconds(std::min(remaining, 5)));
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
        { std::ostringstream _ls; _ls << "[GitHub BG] Initial startup delay: " << constants::GITHUB_BG_INITIAL_DELAY_S << "s to avoid network burst";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        int remaining = constants::GITHUB_BG_INITIAL_DELAY_S;
        while (remaining > 0 && !stop_.load()) {
            sleepInterruptible(std::chrono::seconds(std::min(remaining, 5)));
            remaining -= 5;
        }
        if (stop_.load()) return;
    }

    // Clear stale reason
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.extra.erase("reason");
    }

    // Load enabled sources from source manager
    std::vector<std::string> enabled_sources;
    std::string sources_path = runtimeBasePathFor(orch_) + "/sources_manager.tsv";
    std::ifstream sources_file(sources_path);
    if (sources_file.is_open()) {
        std::string line;
        while (std::getline(sources_file, line)) {
            line = utils::trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> cols;
            std::istringstream row(line);
            std::string col;
            while (std::getline(row, col, '\t')) cols.push_back(col);
            if (cols.size() >= 10) {
                // TSV format:
                // 0=enabled, 1=priority, 2=category, 3=added_ts, 4=last_success_ts,
                // 5=total_configs_found, 6=success_rate, 7=name, 8=description, 9=url
                const bool enabled = cols[0] == "1";
                const std::string url = utils::trim(cols[9]);
                if (enabled && (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)) {
                    enabled_sources.push_back(url);
                }
            }
        }
        sources_file.close();
    }
    
    // Fallback to config githubUrls if no sources file exists
    if (enabled_sources.empty()) {
        enabled_sources = orch_->config().githubUrls();
        utils::LogRingBuffer::instance().push("[GitHubBG] Using fallback githubUrls from config");
    } else {
        utils::LogRingBuffer::instance().push("[GitHubBG] Loaded " + std::to_string(enabled_sources.size()) + " enabled sources from manager");
    }
    
    if (enabled_sources.empty()) {
        updateExtra("reason", "no_enabled_sources");
        utils::LogRingBuffer::instance().push("[GitHubBG] No enabled sources configured");
        interval_seconds = 300;
        return;
    }

    int cap = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_CAP", constants::DEFAULT_GITHUB_BG_CAP);
    cap = std::max(200, std::min(1000000, cap));
    { std::ostringstream _ls; _ls << "[GitHubBG] Fetching from " << enabled_sources.size() 
        << " sources (cap=" << cap << ") with proxy fallback";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    if (orch_->isDownloadInProgress()) {
        updateExtra("reason", "download_busy");
        interval_seconds = 30;
        utils::LogRingBuffer::instance().push("[GitHubBG] Download already running, retrying in 30s");
        return;
    }

    const auto* db_before_ptr = orch_->configDb();
    const int db_size_before = db_before_ptr ? db_before_ptr->size() : 0;
    const std::string app_proxy = orch_->config().getString("config_download_proxy", "");
    const bool success = orch_->downloadConfigsAsync(enabled_sources, app_proxy);
    const double now_ts = utils::nowTimestamp();

    auto* db_after_ptr = orch_->configDb();
    const int db_size_after = db_after_ptr ? db_after_ptr->size() : db_size_before;
    const int db_added = std::max(0, db_size_after - db_size_before);

    updateExtra("sources_found", std::to_string((int)enabled_sources.size()));
    updateExtra("db_added", std::to_string(db_added));
    updateExtra("db_size", std::to_string(db_size_after));

    if (!success) {
        pending_connectivity_retry_ = true;
        updateExtra("reason", "connectivity_retry_pending");
        utils::LogRingBuffer::instance().push("[GitHubBG] No connectivity/new payload. Will retry every 30s until online.");
        interval_seconds = 30;
        return;
    }

    pending_connectivity_retry_ = false;
    last_success_ts_ = now_ts;
    download_count_++;

    updateExtra("downloads", std::to_string(download_count_));
    updateExtra("last_success_ts", std::to_string(last_success_ts_));
    updateExtra("reason", "success");

    auto* db = orch_->configDb();
    if (db) {
        network::ConfigDatabase::TagStats tag_stats = db->getTagStats("download");
        updateExtra("download_total", std::to_string(tag_stats.total));
        updateExtra("download_alive", std::to_string(tag_stats.alive));
        updateExtra("download_untested", std::to_string(tag_stats.untested));
        updateExtra("download_needs_retest", std::to_string(tag_stats.needs_retest));
    }

    // Default schedule: every 30 minutes.
    interval_seconds = constants::GITHUB_BG_INTERVAL_S;

    std::ostringstream final_msg;
    final_msg << "[GitHubBG] Cycle complete: interval=" << interval_seconds
              << "s download_count=" << download_count_
              << " db_added=" << db_added
              << " pending_retry=" << (pending_connectivity_retry_ ? "1" : "0");
    utils::LogRingBuffer::instance().push(final_msg.str());
}

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
    auto task_metrics = HunterTaskManager::instance().getMetrics();

    // ─── Batch size selection ───
    // The validator now uses a two-phase approach: a fast TCP pre-screen
    // (2-second TCP connect, no process spawn) followed by full proxy tests
    // only for configs that pass. The TCP pre-screen is very cheap — it's
    // just a socket connect — so we can safely use a larger batch size even
    // under memory pressure.
    //
    // The old throttling (RAM >= 95% → batch=8) caused a death spiral:
    // high RAM → tiny batch → slow testing → configs pile up → more RAM →
    // even smaller batch. With TCP pre-screening, each test is 15x cheaper,
    // so we raise the floor significantly.
    // Larger batch sizes now that we use batchTestWithXray (one xray process
    // for all configs). Each inbound adds ~2-5MB to the xray process, so
    // 200 inbounds ≈ 1GB. We cap based on free RAM.
    int batch_size = std::max(50, std::min(200, orch_->chunkSize() * 8));
    if (hw.ram_percent >= 95.0f) batch_size = std::min(batch_size, 50);
    else if (hw.ram_percent >= 90.0f) batch_size = std::min(batch_size, 80);
    else if (hw.ram_percent >= 80.0f) batch_size = std::min(batch_size, 120);
    const int io_pressure_limit = std::max(4, task_metrics.io_pool_size * 2);
    if (task_metrics.io_pending >= io_pressure_limit) {
        batch_size = std::max(10, std::min(batch_size, task_metrics.io_pool_size * 2));
    }

    int timeout_s = orch_->testTimeout();
    int max_concurrent = orch_->maxThreads();
    if (task_metrics.io_pending >= io_pressure_limit) {
        max_concurrent = std::max(4, std::min(max_concurrent, task_metrics.io_pool_size));
    }
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
    updateExtra("interval_s", std::to_string(interval_seconds));
    updateExtra("effective_timeout_s", std::to_string(timeout_s));
    updateExtra("effective_max_concurrent", std::to_string(max_concurrent));
    updateExtra("effective_batch_size", std::to_string(batch_size));
    updateExtra("effective_chunk_size", std::to_string(orch_->chunkSize()));
    updateExtra("active_test_processes", std::to_string(network::ProxyTester::activeTestCount()));
    updateExtra("max_test_processes", std::to_string(network::ProxyTester::maxConcurrentTestCount()));
    updateExtra("task_io_pending", std::to_string(task_metrics.io_pending));
    updateExtra("task_io_active", std::to_string(task_metrics.io_active));
    updateExtra("task_cpu_pending", std::to_string(task_metrics.cpu_pending));
    updateExtra("task_cpu_active", std::to_string(task_metrics.cpu_active));

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

    std::ostringstream hist;
    hist << "[";
    bool first = true;
    for (auto& row : history) {
        if (!first) hist << ",";
        first = false;
        hist << row;
    }
    hist << "]";
    updateExtra("history_json", hist.str());

    auto all_records = db->getAllRecords(500);
    double latest_first_seen_ts = 0.0;
    double latest_alive_test_ts = 0.0;
    static bool had_alive_configs = false;
    static uint64_t last_live_refresh_attempt_ms = 0;
    static uint64_t last_live_refresh_success_ms = 0;
    int alive_count_for_refresh = 0;
    for (auto& rec : all_records) {
        if (rec.first_seen > latest_first_seen_ts) latest_first_seen_ts = rec.first_seen;
        if (rec.alive) alive_count_for_refresh++;
        if (rec.alive && rec.last_tested > latest_alive_test_ts) latest_alive_test_ts = rec.last_tested;
    }
    updateExtra("latest_first_seen_ts", std::to_string(latest_first_seen_ts));
    updateExtra("latest_alive_test_ts", std::to_string(latest_alive_test_ts));

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

    // Write files FIRST so realtime consumers see them immediately, then update balancers
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

            if (should_refresh_after_live) {
                last_live_refresh_attempt_ms = now_ms;
                int github_cap = HunterConfig::getEnvInt("HUNTER_GITHUB_BG_CAP", constants::DEFAULT_GITHUB_BG_CAP);
                github_cap = std::max(200, std::min(1000000, github_cap));
                auto refresh = refreshGithubConfigs(orch_, github_cap, 6, 20.0f, "github_bg", "[Validator->GitHub]");
                // Copy values to compatibility fields
                refresh.total_fetched = refresh.fetched_total;
                refresh.valid_configs = refresh.valid_total;
                refresh.sources_found = 6;  // Number of sources attempted
                refresh.timestamp = utils::nowTimestamp();
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
// ThreadManager
// ═══════════════════════════════════════════════════════════════════

ThreadManager::ThreadManager(HunterOrchestrator* orch) : orch_(orch) {
    scanner_ = std::make_unique<ConfigScannerWorker>(orch, stop_event_);
    harvester_ = std::make_unique<HarvesterWorker>(orch, stop_event_);
    github_downloader_ = std::make_unique<GitHubDownloaderWorker>(orch, stop_event_);
    validator_ = std::make_unique<ValidatorWorker>(orch, stop_event_);

    all_workers_ = {
        scanner_.get(), harvester_.get(), github_downloader_.get(), validator_.get()
    };

    for (auto* w : all_workers_) {
        w->setPauseCallback([this]() {
            return orch_ && orch_->isPaused();
        });
    }
}

ThreadManager::~ThreadManager() {
    stopAll();
}

void ThreadManager::startAll() {
    stop_event_ = false;
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
        if (!w) continue;
        try { w->requestStop(); } catch (...) {}
    }
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
    s.hardware.thread_count = static_cast<int>(all_workers_.size());

    for (auto* w : all_workers_) {
        s.workers[w->name] = w->getStatus();
    }
    return s;
}

} // namespace orchestrator
} // namespace hunter
