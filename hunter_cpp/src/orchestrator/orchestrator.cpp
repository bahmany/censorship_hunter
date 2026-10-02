#include "orchestrator/orchestrator.h"
#include "orchestrator/thread_manager.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "core/win_compat.h"
#include "core/engine_embed.h"
#include "core/config_embed.h"
#include "network/proxy_tester.h"
#include "network/sys_proxy.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <sstream>
#include <chrono>
#include <future>
#include <thread>
#include <filesystem>
#include <fstream>

namespace hunter {

namespace {

static const char* LIVE_RECHECK_URLS[] = {
    "https://www.gstatic.com/generate_204",
    "https://speed.cloudflare.com/__down?bytes=5120",
    "https://cachefly.cachefly.net/1mb.test",
};
static constexpr int LIVE_RECHECK_URL_COUNT = 3;

struct LiveRecheckItem {
    std::string uri;
    bool alive = false;
    std::string engine_used;
    float score = 0.0f;
    std::string error;
};

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char c : value) {
        if (c == '"') escaped += "\\\"";
        else if (c == '\\') escaped += "\\\\";
        else if (c == '\n') escaped += "\\n";
        else if (c == '\r') escaped += "\\r";
        else if (c == '\t') escaped += "\\t";
        else escaped.push_back(c);
    }
    return escaped;
}

[[maybe_unused]] std::string jsonStringArray(const std::vector<std::string>& values) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& value : values) {
        if (!first) out << ",";
        first = false;
        out << "\"" << jsonEscape(value) << "\"";
    }
    out << "]";
    return out.str();
}

bool looksLikeLiteralIp(const std::string& address) {
    if (address.empty()) return false;
    if (address.find(':') != std::string::npos) {
        for (unsigned char c : address) {
            if (!(std::isxdigit(c) || c == ':' || c == '.' || c == '[' || c == ']')) return false;
        }
        return true;
    }
    bool has_dot = false;
    for (unsigned char c : address) {
        if (c == '.') {
            has_dot = true;
            continue;
        }
        if (!std::isdigit(c)) return false;
    }
    return has_dot;
}

std::string endpointKeyForUri(const std::string& uri) {
    auto parsed = network::UriParser::parse(uri);
    if (parsed.has_value() && parsed->isValid()) {
        std::string address = utils::trim(parsed->address);
        std::transform(address.begin(), address.end(), address.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (looksLikeLiteralIp(address)) return address;
    }
    std::string fallback = utils::trim(uri);
    std::transform(fallback.begin(), fallback.end(), fallback.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return fallback;
}

std::vector<std::string> dedupeUrisByEndpoint(const std::vector<std::string>& uris) {
    std::set<std::string> seen_endpoints;
    std::vector<std::string> deduped;
    deduped.reserve(uris.size());
    for (const auto& uri : uris) {
        const std::string key = endpointKeyForUri(uri);
        if (key.empty()) continue;
        if (seen_endpoints.insert(key).second) deduped.push_back(uri);
    }
    return deduped;
}

void dedupeBenchResultsByEndpoint(std::vector<BenchResult>& results) {
    std::set<std::string> seen_endpoints;
    std::vector<BenchResult> deduped;
    deduped.reserve(results.size());
    for (const auto& result : results) {
        const std::string key = endpointKeyForUri(result.uri);
        if (key.empty()) continue;
        if (seen_endpoints.insert(key).second) deduped.push_back(result);
    }
    results.swap(deduped);
}

[[maybe_unused]] int reserveTemporaryListenPort() {
    static std::mutex port_mutex;
    static int next_port = 21000;
    std::lock_guard<std::mutex> lock(port_mutex);
    for (int attempts = 0; attempts < 12000; ++attempts) {
        const int candidate = next_port++;
        if (next_port > 45000) next_port = 21000;
        if (!utils::isPortAlive(candidate, 100)) {
            return candidate;
        }
    }
    return 0;
}

[[maybe_unused]] float testProvisionedPortDownload(int port, int timeout_seconds) {
    const int quick_timeout = std::max(3, std::min(timeout_seconds, 8));
    const int download_timeout = std::max(8, timeout_seconds);
    float speed = -1.0f;
    for (int i = 0; i < LIVE_RECHECK_URL_COUNT && speed <= 0.0f; ++i) {
        const int url_timeout = i == 0 ? quick_timeout : download_timeout;
        speed = utils::downloadSpeedViaSocks5(LIVE_RECHECK_URLS[i], "127.0.0.1", port, url_timeout);
    }
    return speed;
}

bool isImportableProxyUri(const std::string& uri) {
    if (uri.size() < 10) return false;
    static const std::vector<std::string> schemes = {
        "vmess://", "vless://", "trojan://", "ss://", "ssr://",
        "hysteria2://", "hy2://", "tuic://"
    };
    bool has_scheme = false;
    for (const auto& s : schemes) {
        if (uri.compare(0, s.size(), s) == 0) {
            has_scheme = true;
            break;
        }
    }
    if (!has_scheme) return false;
    if (uri.compare(0, 8, "vmess://") == 0) {
        std::string payload = uri.substr(8);
        auto hash = payload.find('#');
        if (hash != std::string::npos) payload = payload.substr(0, hash);
        if (payload.size() < 10) return false;
        std::string decoded = utils::base64Decode(payload);
        if (decoded.empty() || decoded.find('{') == std::string::npos) return false;
        return decoded.find("\"add\"") != std::string::npos;
    }
    auto sep = uri.find("://");
    if (sep == std::string::npos) return false;
    std::string payload = uri.substr(sep + 3);
    auto hash = payload.find('#');
    if (hash != std::string::npos) payload = payload.substr(0, hash);
    if (payload.size() < 3) return false;
    if (uri.compare(0, 8, "vless://") == 0 || uri.compare(0, 9, "trojan://") == 0) {
        if (payload.find('@') == std::string::npos) return false;
    }
    if (uri.compare(0, 5, "ss://") == 0 && payload.find('@') == std::string::npos) {
        std::string decoded = utils::base64Decode(payload);
        if (decoded.empty() || decoded.find(':') == std::string::npos) return false;
    }
    return true;
}

[[maybe_unused]] bool loadImportCandidates(const std::string& file_path, std::set<std::string>& valid_configs, int& invalid_count) {
    std::ifstream input(file_path, std::ios::binary);
    if (!input.is_open()) return false;

    std::ostringstream raw_stream;
    raw_stream << input.rdbuf();
    const std::string raw = raw_stream.str();

    std::set<std::string> extracted = utils::extractRawUrisFromText(raw);
    const auto decoded_whole = utils::tryDecodeAndExtract(raw);
    extracted.insert(decoded_whole.begin(), decoded_whole.end());

    const auto lines = utils::readLines(file_path);
    for (const auto& line : lines) {
        const std::string trimmed = utils::trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.find("://") != std::string::npos) {
            extracted.insert(trimmed);
        } else {
            const auto decoded = utils::tryDecodeAndExtract(trimmed);
            extracted.insert(decoded.begin(), decoded.end());
        }
    }

    for (const auto& uri : extracted) {
        if (isImportableProxyUri(uri)) {
            valid_configs.insert(uri);
        } else {
            invalid_count++;
        }
    }
    return true;
}

std::set<std::string> extractDownloadConfigs(const std::string& content, int* invalid_count = nullptr) {
    std::set<std::string> extracted = utils::extractRawUrisFromText(content);

    const auto decoded_whole = utils::tryDecodeAndExtract(content);
    extracted.insert(decoded_whole.begin(), decoded_whole.end());

    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = utils::trim(line);
        if (trimmed.empty()) continue;
        if (trimmed.find("://") != std::string::npos) {
            extracted.insert(trimmed);
            continue;
        }
        const auto decoded_line = utils::tryDecodeAndExtract(trimmed);
        extracted.insert(decoded_line.begin(), decoded_line.end());
    }

    std::set<std::string> valid;
    int local_invalid = 0;
    for (const auto& uri : extracted) {
        if (isImportableProxyUri(uri)) {
            valid.insert(uri);
        } else {
            local_invalid++;
        }
    }
    if (invalid_count) *invalid_count = local_invalid;
    return valid;
}

} // namespace

HunterOrchestrator::HunterOrchestrator(HunterConfig& config)
    : config_(config),
      config_fetcher_(http_client_) {
    // HTTP client auto-initializes CURL in its constructor
    initComponents();
    std::cout << "Hunter Orchestrator initialized successfully" << std::endl;
    std::cout << "[Init] HTTP client ready, config database ready" << std::endl;
}

HunterOrchestrator::~HunterOrchestrator() {
    stop();
}

void HunterOrchestrator::initComponents() {
    // Config database
    config_db_ = std::make_unique<network::ConfigDatabase>(constants::CONFIG_DB_MAX_SIZE);
    
    // Load persisted config database from disk (survives restarts)
    {
        std::string db_path = "runtime/HUNTER_config_db.tsv";
        int db_loaded = config_db_->loadFromDisk(db_path);
        if (db_loaded > 0) {
            std::cout << "[Startup] Restored " << db_loaded << " configs from disk database" << std::endl;
            // If DB exceeds the max size limit (e.g. limit was lowered),
            // evict dead/stale configs immediately to free RAM.
            int evicted = config_db_->evictDead();
            if (evicted > 0) {
                std::cout << "[Startup] Evicted " << evicted << " stale configs (DB trimmed to limit)" << std::endl;
            }
        }
    }

    // Load live connections cache — alive configs from previous runs.
    // These are merged into the DB and given priority for revalidation.
    {
        std::string live_path = "runtime/HUNTER_live_cache.tsv";
        int live_loaded = config_db_->loadLiveFromDisk(live_path);
        if (live_loaded > 0) {
            std::cout << "[Startup] Restored " << live_loaded << " live connections from cache" << std::endl;
            utils::LogRingBuffer::instance().push(
                "[Startup] Restored " + std::to_string(live_loaded) + " live connections from cache");
        }
    }

    // Continuous validator
    continuous_validator_ = std::make_unique<network::ContinuousValidator>(*config_db_);

    // Flexible fetcher
    flexible_fetcher_ = std::make_unique<network::FlexibleFetcher>(config_fetcher_);

    // Smart cache
    std::string cache_dir = utils::dirName(config_.stateFile());
    if (cache_dir.empty()) cache_dir = "runtime";
    cache_ = std::make_unique<cache::SmartCache>(cache_dir);

    // Runtime cleanup manager
    cleanup_manager_ = std::make_unique<orchestrator::RuntimeCleanupManager>(cache_dir);
}

void HunterOrchestrator::start() {
    std::cout << "[Orchestrator] start() called" << std::endl;
    stop_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time_ = utils::nowTimestamp();
    }

    std::string runtime_dir = utils::dirName(config_.stateFile());
    if (runtime_dir.empty()) runtime_dir = "runtime";
    const std::string stop_flag = runtime_dir + "/stop.flag";

    // ═══ PHASE 0a: Kill orphaned test processes from previous runs ═══
    // When the app is killed (e.g. pkill -9), child xray/sing-box processes
    // become orphans and keep holding ports + RAM. Kill them before starting.
    {
        int killed = 0;
        // pkill -f "temp_xray_test|temp_singbox_test|temp_mihomo_test|proxy_server"
        // We use the system call since we need pattern matching on the cmdline.
        int rc = std::system("pkill -9 -f 'temp_xray_test' 2>/dev/null; "
                             "pkill -9 -f 'temp_singbox_test' 2>/dev/null; "
                             "pkill -9 -f 'temp_mihomo_test' 2>/dev/null; "
                             "pkill -9 -f 'proxy_server_' 2>/dev/null");
        (void)rc;  // pkill returns non-zero if no processes matched
        std::cout << "[Startup] Cleaned up orphaned test processes" << std::endl;
        utils::LogRingBuffer::instance().push(
            "[Startup] Cleaned up orphaned test processes (" + std::to_string(killed) + ")");
    }

    // ═══ PHASE 0: Runtime cleanup — remove stale temp files ═══
    if (cleanup_manager_) {
        std::cout << "[Startup] Running runtime cleanup..." << std::endl;
        auto cleanup_stats = cleanup_manager_->runCleanup();
        std::cout << "[Startup] Cleanup: deleted " << cleanup_stats.files_deleted
                  << " files, freed " << (cleanup_stats.bytes_freed / 1024) << " KB" << std::endl;
        cleanup_manager_->startPeriodicCleanup(300); // every 5 min — temp files expire after 1h
    }

    // ═══ PHASE 1: Load initial configs into database ═══
    std::cout << "[Startup] Loading initial configurations..." << std::endl;

    // Always load raw config files as seed data
    int initial_loaded = loadRawConfigFiles();
    std::cout << "[Startup] Loaded " << initial_loaded << " initial configs from raw files" << std::endl;

    // Also load from all_cache.txt if exists
    std::string all_cache_file = "runtime/HUNTER_all_cache.txt";
    if (utils::fileExists(all_cache_file)) {
        auto cache_lines = utils::readLines(all_cache_file);
        std::set<std::string> cache_configs;
        for (const auto& line : cache_lines) {
            std::string trimmed = utils::trim(line);
            if (!trimmed.empty() && trimmed.find("://") != std::string::npos) {
                cache_configs.insert(trimmed);
            }
        }
        if (!cache_configs.empty()) {
            int cache_loaded = config_db_->addConfigs(cache_configs, "cache");
            std::cout << "[Startup] Loaded " << cache_loaded << " configs from cache file" << std::endl;
        }
    }

    // If database is still empty, try bundle configs
    if (config_db_ && config_db_->size() == 0) {
        std::cout << "[Startup] Database still empty, loading bundle configs..." << std::endl;
        int bundle_loaded = loadBundleConfigs();
        std::cout << "[Startup] Loaded " << bundle_loaded << " configs from bundle files" << std::endl;
    }

    // ═══ PHASE 1b: Load embedded configs (single-file build) ═══
    // The single-file build bakes a freshly-downloaded config bundle into
    // the executable at build time. Load it into the database so the user
    // has immediate proxy candidates on first launch — before any network
    // scraping begins. This is additive: it merges with anything already
    // loaded from disk cache above.
    if (hunter::embed::hasEmbeddedConfigs()) {
        std::cout << "[Startup] Loading embedded config bundle..." << std::endl;
        const auto& embedded = hunter::embed::configs();
        if (!embedded.empty()) {
            std::set<std::string> embedded_set(embedded.begin(), embedded.end());
            int embedded_loaded = config_db_->addConfigs(embedded_set, "embedded");
            std::cout << "[Startup] Loaded " << embedded_loaded << " configs from embedded bundle ("
                      << embedded.size() << " total)" << std::endl;
            utils::LogRingBuffer::instance().push(
                "[Startup] Loaded " + std::to_string(embedded_loaded) +
                " configs from embedded bundle");
        } else {
            std::cout << "[Startup] Embedded config bundle is empty" << std::endl;
        }
    } else {
        std::cout << "[Startup] No embedded config bundle (live scraping will populate DB)" << std::endl;
    }

    // ═══ PHASE 2: Detect censorship (informational) ═══
    bool is_censored = detectCensorship();
    std::cout << "[Startup] Direct connectivity: " << (is_censored ? "CENSORED" : "OPEN") << std::endl;

    // ═══ PHASE 3: Print startup banner ═══
    printStartupBanner();

    // ═══ PHASE 4: Start all worker threads (background fetch + validation) ═══
    if (thread_manager_ && !thread_manager_->isRunning()) {
        thread_manager_.reset();
    }
    thread_manager_ = std::make_unique<orchestrator::ThreadManager>(this);
    thread_manager_->startAll();
    std::cout << "[Startup] All worker threads started" << std::endl;

    // Apply initial speed profile from hardware (overridable via HUNTER_SPEED_PROFILE env var)
    {
        auto hw = HunterTaskManager::instance().getHardware();
        const char* env_profile = std::getenv("HUNTER_SPEED_PROFILE");
        std::string profile;
        if (env_profile && *env_profile) {
            profile = std::string(env_profile);
        } else if (hw.ram_free_gb < 2.0f) {
            profile = "low";
        } else if (hw.cpu_count <= 2) {
            profile = "low";
        } else if (hw.cpu_count <= 4) {
            profile = "medium";
        } else {
            profile = "high";
        }
        applyAutoProfile(profile);
    }

    // ═══ PHASE 5: Main loop ═══
    int dashboard_tick = 0;
    int db_save_tick = 0;
    while (!stop_requested_.load() && thread_manager_ && thread_manager_->isRunning()) {
        if (utils::fileExists(stop_flag)) {
            try { std::filesystem::remove(stop_flag); } catch (...) {}
            std::cout << "\n[Hunter] stop.flag detected, shutting down..." << std::endl;
            stop();
            break;
        }

        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Print console dashboard every ~8s
        if (++dashboard_tick >= 4) {
            dashboard_tick = 0;
            printDashboard();
        }

        // Persist ConfigDB to disk every ~60s
        if (++db_save_tick >= 30) {
            db_save_tick = 0;
            if (config_db_) {
                try {
                    int saved = config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
                    // Also save live connections cache (alive configs only).
                    // This is a small file that survives restarts.
                    int live_saved = config_db_->saveLiveToDisk("runtime/HUNTER_live_cache.tsv");
                    if (saved > 0) {
                        utils::LogRingBuffer::instance().push(
                            "[DB] Persisted " + std::to_string(saved) + " configs to disk" +
                            (live_saved > 0 ? " (" + std::to_string(live_saved) + " live)" : ""));
                    }
                } catch (...) {}
            }
        }

        // Reap zombie child processes non-blockingly. Proxy engine subprocesses
        // (xray/sing-box/mihomo) are killed with killAndWait, but if a process
        // is stuck in uninterruptible sleep (D state), it can't be reaped
        // immediately and becomes a zombie until reaped. Over hours, zombies
        // accumulate and consume PID table entries. This periodic reap cleans
        // them up without blocking the main loop.
        utils::reapZombies();

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (thread_manager_) {
        thread_manager_->stopAll();
        thread_manager_.reset();
    }
}

void HunterOrchestrator::stop() {
    stop_requested_ = true;
    paused_ = false;
    {
        std::lock_guard<std::mutex> lk(download_thread_mutex_);
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
    }

    // Persist ConfigDB and live cache before shutdown
    if (config_db_) {
        try {
            int saved = config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
            int live_saved = config_db_->saveLiveToDisk("runtime/HUNTER_live_cache.tsv");
            std::cout << "[Shutdown] Saved " << saved << " configs to disk ("
                      << live_saved << " live)" << std::endl;
        } catch (...) {}
    }

    // Stop cleanup manager periodic thread
    if (cleanup_manager_) {
        cleanup_manager_->stopPeriodicCleanup();
    }

    // Stop all running proxy servers (release ports 3110-3120)
    proxy_server_manager_.stopAll();

    if (thread_manager_) {
        thread_manager_->stopAll();
        thread_manager_.reset();
    }
}

// ─── Pause / Resume ───

void HunterOrchestrator::pause() {
    paused_ = true;
    utils::LogRingBuffer::instance().push("[Cmd] PAUSED by user");
}

void HunterOrchestrator::resume() {
    paused_ = false;
    utils::LogRingBuffer::instance().push("[Cmd] RESUMED by user");
}

// ─── Speed Controls ───

void HunterOrchestrator::setSpeedProfile(const SpeedProfile& p) {
    speed_max_threads_ = std::max(1, std::min(50, p.max_threads));
    speed_test_timeout_ = std::max(1, std::min(10, p.test_timeout_s));
    speed_chunk_size_ = std::max(1, std::min(50, p.chunk_size));
    {
        std::lock_guard<std::mutex> lock(speed_mutex_);
        speed_profile_name_ = p.profile_name;
    }
    utils::LogRingBuffer::instance().push(
        "[Speed] Profile=" + p.profile_name +
        " threads=" + std::to_string(speed_max_threads_.load()) +
        " timeout=" + std::to_string(speed_test_timeout_.load()) +
        " chunk=" + std::to_string(speed_chunk_size_.load()));
}

HunterOrchestrator::SpeedProfile HunterOrchestrator::getSpeedProfile() const {
    SpeedProfile p;
    p.max_threads = speed_max_threads_.load();
    p.test_timeout_s = speed_test_timeout_.load();
    p.chunk_size = speed_chunk_size_.load();
    {
        std::lock_guard<std::mutex> lock(speed_mutex_);
        p.profile_name = speed_profile_name_;
    }
    return p;
}

void HunterOrchestrator::applyAutoProfile(const std::string& level) {
    auto hw = HunterTaskManager::instance().getHardware();
    SpeedProfile p;
    p.profile_name = level;
    if (level == "low") {
        p.max_threads = std::max(2, std::min(hw.cpu_count, 4));
        p.test_timeout_s = 8;
        p.chunk_size = std::max(2, hw.cpu_count);
    } else if (level == "high") {
        p.max_threads = std::min(50, std::max(10, hw.cpu_count * 3));
        p.test_timeout_s = 3;
        p.chunk_size = std::min(30, std::max(8, hw.cpu_count * 2));
    } else { // medium
        p.profile_name = "medium";
        p.max_threads = std::min(30, std::max(5, hw.cpu_count * 2));
        p.test_timeout_s = 5;
        p.chunk_size = std::min(15, std::max(4, hw.cpu_count));
    }
    setSpeedProfile(p);
}

// ─── Maintenance ───

int HunterOrchestrator::clearOldConfigs(int max_age_hours) {
    if (!config_db_) return 0;
    return config_db_->clearOlderThan(max_age_hours);
}

int HunterOrchestrator::clearAliveConfigs() {
    int removed = 0;
    if (config_db_) {
        removed = config_db_->clearAlive();
        try {
            config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
        } catch (...) {}
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_good_configs_.clear();
    }

    try {
        const std::string gold_path = config_.goldFile();
        if (!gold_path.empty()) {
            std::ofstream gold_out(gold_path, std::ios::trunc);
        }
        const std::string silver_path = config_.silverFile();
        if (!silver_path.empty()) {
            std::ofstream silver_out(silver_path, std::ios::trunc);
        }
    } catch (...) {}

    utils::LogRingBuffer::instance().push(
        "[Cmd] Cleared alive configs: db=" + std::to_string(removed));
    return removed;
}

int HunterOrchestrator::removeConfigs(const std::set<std::string>& uris) {
    if (uris.empty()) return 0;

    int removed = 0;
    if (config_db_) {
        removed = config_db_->removeUris(uris);
        try {
            config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
        } catch (...) {}
    }

    auto filterLines = [&](const std::string& path) {
        if (path.empty()) return;
        std::vector<std::string> filtered;
        for (const auto& line : utils::readLines(path)) {
            if (uris.find(line) == uris.end()) filtered.push_back(line);
        }
        utils::writeLines(path, filtered);
    };
    filterLines(config_.goldFile());
    filterLines(config_.silverFile());

    auto filterPairs = [&](std::vector<std::pair<std::string, float>>& items) {
        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& item) {
            return uris.find(item.first) != uris.end();
        }), items.end());
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        filterPairs(last_good_configs_);
        for (auto& [name, items] : cached_configs_) {
            filterPairs(items);
        }
        for (const auto& uri : uris) {
            cached_engine_hints_.erase(uri);
        }
    }

    utils::LogRingBuffer::instance().push(
        "[Cmd] Removed " + std::to_string(removed) + " configs from DB/runtime artifacts");
    return removed;
}

void HunterOrchestrator::addManualConfigs(const std::vector<std::string>& uris) {
    if (!config_db_ || uris.empty()) return;
    std::set<std::string> valid_set;
    for (auto& raw : uris) {
        std::string u = utils::trim(raw);
        if (!u.empty() && u.find("://") != std::string::npos) {
            valid_set.insert(u);
        }
    }
    if (valid_set.empty()) return;
    int promoted = 0;
    int added = config_db_->addConfigsWithPriority(valid_set, "manual", &promoted);
    utils::LogRingBuffer::instance().push(
        "[Cmd] Added " + std::to_string(added) + " manual configs, promoted " +
        std::to_string(promoted) + " existing (" + std::to_string(valid_set.size()) + " submitted)");
}

std::string HunterOrchestrator::triggerRuntimeCleanup() {
    if (!cleanup_manager_) return "{\"ok\":false,\"error\":\"cleanup_manager_not_initialized\"}";
    auto stats = cleanup_manager_->runCleanup();
    utils::LogRingBuffer::instance().push(
        "[Cmd] Runtime cleanup: deleted " + std::to_string(stats.files_deleted) +
        " files, freed " + std::to_string(stats.bytes_freed / 1024) + " KB");
    return cleanup_manager_->buildStatsJson();
}

// ─── Real-time UI Communication ───

bool HunterOrchestrator::runCycle() {
    std::unique_lock<std::mutex> lock(cycle_lock_, std::try_to_lock);
    if (!lock.owns_lock()) {
        utils::LogRingBuffer::instance().push("[CycleLock] Another cycle is already running, skipping");
        return false;
    }

    cycle_count_++;
    double cycle_start = utils::nowTimestamp();
    { std::ostringstream _ls; _ls << "Starting hunter cycle #" << cycle_count_.load();
      utils::LogRingBuffer::instance().push(_ls.str()); }

  try {

    // Pre-cycle cleanup: remove stale temp files before validation/export
    if (cleanup_manager_) {
        cleanup_manager_->runCleanup();
    }

    // Memory status
    auto hw = HunterTaskManager::instance().getHardware();
    { std::ostringstream _ls; _ls << "Memory status: " << hw.ram_percent << "% used ("
              << hw.ram_used_gb << "GB / " << hw.ram_total_gb << "GB, free=" << hw.ram_free_gb << "GB)";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // Degraded mode when free RAM is very low (< 1GB). Each xray/sing-box
    // test process needs ~50-100MB, so we need at least 1GB free to run
    // tests without risking OOM. This is based on FREE RAM, not total
    // used RAM, so it won't trigger just because other system processes
    // are using a lot of memory.
    const bool degraded_mode = hw.ram_free_gb < 1.0f;
    if (degraded_mode) {
        utils::LogRingBuffer::instance().push("[RunCycle] Low free RAM (<1GB), entering degraded cycle mode");
    }

    // Scrape configs
    auto scraped = scrapeConfigs();
    auto& tg_raw = scraped.telegram;
    auto& http_raw = scraped.http;
    { std::ostringstream _ls; _ls << "Total raw configs: " << tg_raw.size() << " Telegram + "
              << http_raw.size() << " HTTP/GitHub";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // Store in ConfigDB
    if (config_db_) {
        int added_tg = 0;
        int added_http = 0;
        if (!tg_raw.empty()) {
            std::set<std::string> tg_set(tg_raw.begin(), tg_raw.end());
            added_tg = config_db_->addConfigs(tg_set, "telegram");
        }
        if (!http_raw.empty()) {
            std::set<std::string> http_set(http_raw.begin(), http_raw.end());
            added_http = config_db_->addConfigs(http_set, "http");
        }
        { std::ostringstream _ls; _ls << "[ConfigDB] Stored +" << added_tg << " tg, +" << added_http
                  << " http (DB: " << config_db_->size() << ")";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    // Supplement with healthy DB configs
    if (config_db_) {
        auto healthy = config_db_->getHealthyConfigs(300);
        std::set<std::string> existing(tg_raw.begin(), tg_raw.end());
        existing.insert(http_raw.begin(), http_raw.end());
        for (auto& [uri, lat] : healthy) {
            if (existing.find(uri) == existing.end()) {
                http_raw.push_back(uri);
            }
        }
    }

    // Inject untested batch from ConfigDB
    if (config_db_) {
        int batch_size = HunterConfig::getEnvInt("HUNTER_BG_VALIDATION_BATCH",
                                                  constants::DEFAULT_BG_VALIDATION_BATCH);
        batch_size = std::max(0, std::min(400, batch_size));
        if (batch_size > 0) {
            auto batch = config_db_->getUntestedBatch(batch_size);
            std::set<std::string> existing(tg_raw.begin(), tg_raw.end());
            existing.insert(http_raw.begin(), http_raw.end());
            std::vector<std::string> batch_uris;
            for (auto& rec : batch) {
                if (existing.find(rec.uri) == existing.end()) {
                    batch_uris.push_back(rec.uri);
                }
            }
            if (!batch_uris.empty()) {
                http_raw.insert(http_raw.begin(), batch_uris.begin(), batch_uris.end());
                { std::ostringstream _ls; _ls << "[ConfigDB] Queued validation for " << batch_uris.size() << " configs";
                  utils::LogRingBuffer::instance().push(_ls.str()); }
            }
        }
    }

    // Benchmark line 1: Telegram configs
    std::vector<BenchResult> validated;
    if (!tg_raw.empty()) {
        { std::ostringstream _ls; _ls << "[Bench] Telegram: " << tg_raw.size() << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        auto tg_validated = validateConfigs(tg_raw, "Telegram", 0);
        { std::ostringstream _ls; _ls << "[Bench-Telegram] Result: " << tg_validated.size() << " working";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        validated.insert(validated.end(), tg_validated.begin(), tg_validated.end());
    }

    // Benchmark line 2: HTTP/GitHub configs
    if (!http_raw.empty()) {
        { std::ostringstream _ls; _ls << "[Bench] GitHub/HTTP: " << http_raw.size() << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        auto http_validated = validateConfigs(http_raw, "GitHub", 5000);
        { std::ostringstream _ls; _ls << "[Bench-GitHub] Result: " << http_validated.size() << " working";
          utils::LogRingBuffer::instance().push(_ls.str()); }
        validated.insert(validated.end(), http_validated.begin(), http_validated.end());
    }

    dedupeBenchResultsByEndpoint(validated);

    // Sort by latency
    std::sort(validated.begin(), validated.end(),
              [](const BenchResult& a, const BenchResult& b) { return a.latency_ms < b.latency_ms; });
    { std::ostringstream _ls; _ls << "Validated configs (combined): " << validated.size();
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // Tier configs
    auto tiered = tierConfigs(validated);
    { std::ostringstream _ls; _ls << "Gold: " << tiered.gold.size() << ", Silver: " << tiered.silver.size();
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // Update balancer
    std::vector<std::pair<std::string, float>> all_configs;
    for (auto& r : tiered.gold) all_configs.emplace_back(r.uri, r.latency_ms);
    for (auto& r : tiered.silver) all_configs.emplace_back(r.uri, r.latency_ms);
    last_validated_count_ = (int)all_configs.size();

    if (!all_configs.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_good_configs_ = std::vector<std::pair<std::string, float>>(
            all_configs.begin(), all_configs.begin() + std::min((int)all_configs.size(), 200));
    }

    if (!all_configs.empty()) {
        double last_github_success_ts = 0.0;
        if (thread_manager_) {
            auto tm_status = thread_manager_->getStatus();
            auto github_worker_it = tm_status.workers.find("github_bg");
            if (github_worker_it != tm_status.workers.end()) {
                auto extra_it = github_worker_it->second.extra.find("last_success_ts");
                if (extra_it != github_worker_it->second.extra.end()) {
                    try { last_github_success_ts = std::stod(extra_it->second); } catch (...) {}
                }
            }
        }
        const double now_ts = utils::nowTimestamp();
        if (last_github_success_ts <= 0.0 || (now_ts - last_github_success_ts) >= 3600.0) {
            std::vector<int> proxy_ports;
            const int github_cap = std::max(200, std::min(config_.maxTotal(), 5000));
            auto fetched = config_fetcher_.fetchGithubConfigs(config_.githubUrls(), proxy_ports, github_cap, 9, 40.0f);
            std::set<std::string> valid;
            for (const auto& uri : fetched) {
                if (isImportableProxyUri(uri)) valid.insert(uri);
            }
            int added = 0;
            if (config_db_ && !valid.empty()) {
                added = config_db_->addConfigs(valid, "github_refresh");
            }
            if (cache_ && !valid.empty()) {
                cache_->saveConfigs(std::vector<std::string>(valid.begin(), valid.end()), false);
            }
            { std::ostringstream _ls; _ls << "[GitHubRefresh] stale_sources=1 fetched=" << fetched.size()
                      << " valid=" << valid.size() << " db+" << added;
              utils::LogRingBuffer::instance().push(_ls.str()); }
        }
    }

    // Save to files
    saveToFiles(tiered.gold, tiered.silver);

    double cycle_time = utils::nowTimestamp() - cycle_start;
    { std::ostringstream _ls; _ls << "Cycle #" << cycle_count_.load() << " completed in " << (int)cycle_time << "s";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    return true;

  } catch (const std::exception& e) {
    { std::ostringstream _ls; _ls << "[RunCycle] EXCEPTION: " << e.what();
      utils::LogRingBuffer::instance().push(_ls.str()); }
    return false;
  } catch (...) {
    utils::LogRingBuffer::instance().push("[RunCycle] UNKNOWN EXCEPTION caught — cycle aborted");
    return false;
  }
}

HunterOrchestrator::ScrapeResult HunterOrchestrator::scrapeConfigs() {
    ScrapeResult result;
    std::vector<int> proxy_ports = {config_.multiproxyPort()};

    int max_total = config_.maxTotal();
    auto hw = HunterTaskManager::instance().getHardware();
    // Cap based on free RAM (not total used)
    if (hw.ram_free_gb < 0.5f) {
        max_total = std::min(max_total, 40);
    } else if (hw.ram_free_gb < 1.0f) {
        max_total = std::min(max_total, 80);
    } else if (hw.ram_free_gb < 2.0f) {
        max_total = std::min(max_total, 160);
    }
    int per_source_cap = std::max(100, max_total / 3);
    if (hw.ram_free_gb < 1.0f) {
        per_source_cap = std::max(20, std::min(per_source_cap, 40));
    }

    const bool tg_enabled = config_.getBool("telegram_enabled", false);
    const std::string tg_api_id = config_.getString("telegram_api_id", "");
    const std::string tg_api_hash = config_.getString("telegram_api_hash", "");
    const std::string tg_phone = config_.getString("telegram_phone", "");
    const std::vector<std::string> tg_targets_raw = config_.telegramTargets();
    const bool tg_configured = tg_enabled && !tg_api_id.empty() && !tg_api_hash.empty() && !tg_phone.empty() && !tg_targets_raw.empty();

    if (tg_configured) {
        int tg_limit = std::max(0, std::min(config_.telegramLimit(), max_total));
        if (tg_limit == 0) tg_limit = std::min(50, max_total);
        const int tg_timeout_ms = std::max(3000, std::min(25000, config_.getInt("telegram_timeout_ms", 12000)));

        std::vector<int> tg_proxy_candidates;
        {
            int env_port = HunterConfig::getEnvInt("HUNTER_TELEGRAM_PROXY_PORT", 0);
            if (env_port > 0) tg_proxy_candidates.push_back(env_port);
            for (int p : std::vector<int>{1080, 7890, 2080, 9250, 11808, 11809, 10808, 10809}) {
                tg_proxy_candidates.push_back(p);
            }
            for (int p : proxy_ports) tg_proxy_candidates.push_back(p);
        }

        auto fetch_telegram_page = [&](const std::string& url, std::string& used_proxy) -> std::string {
            used_proxy = "direct";
            std::string body = http_client_.get(url, tg_timeout_ms);
            if (!body.empty()) return body;

            std::set<int> seen_ports;
            for (int p : tg_proxy_candidates) {
                if (p <= 0) continue;
                if (!seen_ports.insert(p).second) continue;
                if (!utils::isPortAlive(p, 800)) continue;
                std::string proxy = "socks5h://127.0.0.1:" + std::to_string(p);
                body = http_client_.get(url, tg_timeout_ms, proxy);
                if (!body.empty()) {
                    used_proxy = proxy;
                    return body;
                }
            }
            return "";
        };

        std::set<std::string> tg_set;
        std::string last_used_proxy = "direct";
        for (auto t : tg_targets_raw) {
            t = utils::trim(t);
            if (t.empty()) continue;
            if (utils::startsWith(t, "https://")) {
                auto pos = t.find_last_of('/');
                if (pos != std::string::npos) t = t.substr(pos + 1);
            }
            if (utils::startsWith(t, "http://")) {
                auto pos = t.find_last_of('/');
                if (pos != std::string::npos) t = t.substr(pos + 1);
            }
            if (!t.empty() && t.front() == '@') t = t.substr(1);
            t = utils::trim(t);
            if (t.empty()) continue;

            std::string url = "https://t.me/s/" + t;

            std::string used_proxy;
            std::string body = fetch_telegram_page(url, used_proxy);
            if (!used_proxy.empty()) last_used_proxy = used_proxy;
            if (body.empty()) continue;

            auto found = utils::extractRawUrisFromText(body);
            for (auto& u : found) {
                tg_set.insert(u);
                if ((int)tg_set.size() >= tg_limit) break;
            }
            if ((int)tg_set.size() >= tg_limit) break;
        }

        for (auto& u : tg_set) result.telegram.push_back(u);

        { std::ostringstream _ls; _ls << "[TelegramScrape] targets=" << tg_targets_raw.size()
                  << ", configs=" << result.telegram.size()
                  << ", proxy=" << last_used_proxy;
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    // HTTP/GitHub fetch
    if (flexible_fetcher_) {
        auto& mgr = HunterTaskManager::instance();
        std::unique_lock<std::timed_mutex> lock(mgr.fetchLock(), std::defer_lock);
        if (!lock.try_lock_for(std::chrono::seconds(10))) {
            utils::LogRingBuffer::instance().push("[Scrape] Fetch lock busy, proceeding without lock");
        }

        int http_cap = per_source_cap;
        if (!result.telegram.empty()) {
            http_cap = std::max(50, per_source_cap / 2);
        }

        auto github_urls = config_.githubUrls();
        auto http_results = flexible_fetcher_->fetchHttpSourcesParallel(
            proxy_ports, http_cap, 25.0f, 6, github_urls);

        if (lock.owns_lock()) lock.unlock();

        for (auto& fr : http_results) {
            if (fr.success) {
                for (auto& uri : fr.configs) result.http.push_back(uri);
            }
        }
    }

    // Cache fallback
    if (cache_) {
        auto cached = cache_->loadCachedConfigs(500, true);
        std::set<std::string> existing(result.telegram.begin(), result.telegram.end());
        existing.insert(result.http.begin(), result.http.end());
        for (auto& c : cached) {
            if (existing.find(c) == existing.end()) result.http.push_back(c);
        }
    }

    { std::ostringstream _ls; _ls << "[Fetch] Total: " << result.telegram.size() << " Telegram + "
              << result.http.size() << " HTTP/GitHub";
      utils::LogRingBuffer::instance().push(_ls.str()); }
    return result;
}

std::vector<BenchResult> HunterOrchestrator::validateConfigs(
    const std::vector<std::string>& configs, const std::string& label, int base_port_offset) {

    if (configs.empty()) return {};

    // Deduplicate
    std::vector<std::string> deduped = dedupeUrisByEndpoint(configs);

    int max_total = config_.maxTotal();
    if ((int)deduped.size() > max_total) deduped.resize(max_total);

    deduped = network::prioritizeConfigs(deduped);

    auto hw = HunterTaskManager::instance().getHardware();
    // Cap configs based on free RAM (not total used). Each test process
    // needs ~50-100MB, so with <2GB free we cap to 40 configs.
    if (hw.ram_free_gb < 2.0f && deduped.size() > 40) {
        deduped.resize(40);
        { std::ostringstream _ls; _ls << "[Bench-" << label << "] Low free RAM: capped to " << deduped.size() << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    { std::ostringstream _ls; _ls << "[Bench-" << label << "] Testing " << deduped.size() << " configs (batch mode)";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // ─── Batch testing: single xray process with N inbounds ───
    // Instead of spawning one xray/sing-box process per config (slow, RAM-heavy),
    // we use batchTestWithXray which starts ONE xray process with all configs as
    // inbounds on unique SOCKS ports, then tests them all in parallel. This is
    // 10-50x faster and uses far less RAM.
    //
    // Batch size: limited by available ports (500 per batch) and free RAM.
    // Each inbound adds ~2-5MB to the xray process, so 100 inbounds ≈ 500MB.
    const int timeout_s = speed_test_timeout_.load();
    const int base_port = constants::DEFAULT_BENCHMARK_BASE_PORT + base_port_offset;

    // Dynamic batch size based on free RAM
    size_t batch_size = 100;  // default: 100 configs per batch (1 xray process)
    if (hw.ram_free_gb < 0.5f) batch_size = 10;
    else if (hw.ram_free_gb < 1.0f) batch_size = 25;
    else if (hw.ram_free_gb < 2.0f) batch_size = 50;
    // Allow user override via speed chunk size (clamped)
    int user_chunk = speed_chunk_size_.load();
    if (user_chunk > 0) batch_size = std::min(batch_size, (size_t)std::max(1, std::min(user_chunk, 200)));

    std::vector<BenchResult> results;
    network::ProxyTester tester;

    for (size_t offset = 0; offset < deduped.size(); offset += batch_size) {
        // Check pause/stop between batches
        if (stop_requested_.load() || paused_.load()) break;
        auto chunk_hw = HunterTaskManager::instance().getHardware();
        if (chunk_hw.ram_free_gb < 0.3f) {
            utils::LogRingBuffer::instance().push("[Bench-" + label + "] Aborting: free RAM < 0.3GB");
            break;
        }

        size_t end = std::min(offset + batch_size, deduped.size());
        std::vector<std::string> batch_configs(deduped.begin() + offset, deduped.begin() + end);

        // Use a unique base port per batch to avoid conflicts with previous batches
        int batch_base_port = base_port + static_cast<int>(offset);

        { std::ostringstream _ls; _ls << "[Bench-" << label << "] Batch " << (offset / batch_size + 1)
          << ": testing " << batch_configs.size() << " configs (ports " << batch_base_port << "+)";
          utils::LogRingBuffer::instance().push(_ls.str()); }

        std::vector<network::ProxyTestResult> batch_results;
        try {
            batch_results = tester.batchTestWithXray(batch_configs, batch_base_port, timeout_s);
        } catch (const std::exception& ex) {
            utils::LogRingBuffer::instance().push("[Bench-" + label + "] Batch exception: " + std::string(ex.what()));
            for (size_t i = 0; i < batch_configs.size(); i++) {
                BenchResult br;
                br.uri = batch_configs[i];
                br.error = std::string("Batch exception: ") + ex.what();
                br.tier = "dead";
                results.push_back(br);
            }
            continue;
        } catch (...) {
            utils::LogRingBuffer::instance().push("[Bench-" + label + "] Batch unknown exception");
            for (size_t i = 0; i < batch_configs.size(); i++) {
                BenchResult br;
                br.uri = batch_configs[i];
                br.error = "Batch unknown exception";
                br.tier = "dead";
                results.push_back(br);
            }
            continue;
        }

        // Convert ProxyTestResult → BenchResult
        for (size_t i = 0; i < batch_results.size() && i < batch_configs.size(); i++) {
            BenchResult br;
            br.uri = batch_configs[i];
            const auto& r = batch_results[i];

            br.success = r.success && !r.telegram_only && r.download_speed_kbps > 0.0f;
            br.telegram_only = r.success && r.telegram_only;
            if (br.success) {
                br.latency_ms = r.download_speed_kbps > 0 ? 1000.0f / r.download_speed_kbps : 5000.0f;
                br.tier = br.latency_ms <= 3000 ? "gold" : "silver";
            } else if (br.telegram_only) {
                br.latency_ms = 0;
                br.tier = "telegram";
            } else {
                br.latency_ms = 0;
                br.tier = "dead";
            }
            br.engine_used = r.engine_used;
            br.error = r.error_message;
            results.push_back(br);
        }

        if (offset + batch_size < deduped.size()) {
            { std::ostringstream _ls; _ls << "[Bench-" << label << "] Progress: " << results.size() << "/" << deduped.size();
              utils::LogRingBuffer::instance().push(_ls.str()); }
        }
    }

    dedupeBenchResultsByEndpoint(results);

    // Sort by latency (successful first)
    std::sort(results.begin(), results.end(), [](const BenchResult& a, const BenchResult& b) {
        if (a.success != b.success) return a.success > b.success;
        if (a.telegram_only != b.telegram_only) return a.telegram_only > b.telegram_only;
        return a.latency_ms < b.latency_ms;
    });

    // Update ConfigDB with results
    if (config_db_) {
        for (auto& r : results) {
            config_db_->updateHealth(r.uri, r.success || r.telegram_only,
                                     r.success ? r.latency_ms : 0.0f,
                                     r.engine_used, false, r.telegram_only);
        }
    }

    return results;
}

HunterOrchestrator::TieredConfigs HunterOrchestrator::tierConfigs(
    const std::vector<BenchResult>& results) {
    TieredConfigs tiered;
    for (auto& r : results) {
        if (r.tier == "gold") tiered.gold.push_back(r);
        else if (r.tier == "silver") tiered.silver.push_back(r);
    }
    if (tiered.gold.size() > 100) tiered.gold.resize(100);
    if (tiered.silver.size() > 200) tiered.silver.resize(200);
    return tiered;
}

void HunterOrchestrator::saveToFiles(const std::vector<BenchResult>& gold,
                                      const std::vector<BenchResult>& silver) {
    std::string gold_file = config_.goldFile();
    std::string silver_file = config_.silverFile();

    if (!gold_file.empty() && !gold.empty()) {
        std::vector<std::string> uris;
        for (auto& r : gold) uris.push_back(r.uri);
        int added = appendUniqueLines(gold_file, uris);
        if (added > 0) { std::ostringstream _ls; _ls << "Gold file: +" << added << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    if (!silver_file.empty() && !silver.empty()) {
        std::vector<std::string> uris;
        for (auto& r : silver) uris.push_back(r.uri);
        int added = appendUniqueLines(silver_file, uris);
        if (added > 0) { std::ostringstream _ls; _ls << "Silver file: +" << added << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }
}

int HunterOrchestrator::appendUniqueLines(const std::string& filepath,
                                            const std::vector<std::string>& lines) {
    return utils::appendUniqueLines(filepath, lines);
}

int HunterOrchestrator::computeAdaptiveSleep() {
    int base = config_.sleepSeconds();
    if (consecutive_scrape_failures_.load() >= 3) return std::min(base * 2, 600);
    if (last_validated_count_.load() == 0) return std::max(120, base / 3);
    if (last_validated_count_.load() < 5) return std::max(150, base / 2);
    return base;
}

int HunterOrchestrator::cachedConfigCount() const {
    int total = 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& [_, v] : cached_configs_) total += (int)v.size();
    return total;
}

void HunterOrchestrator::printStartupBanner() {
    auto hw = HunterTaskManager::instance().getHardware();

    cache::SmartCache::CacheStats cs;
    if (cache_) cs = cache_->getStats();

    int db_size = config_db_ ? config_db_->size() : 0;

    std::cout << "\n"
    "╔══════════════════════════════════════════════════════════════╗\n"
    "║                   HUNTER Config Discovery                    ║\n"
    "╠══════════════════════════════════════════════════════════════╣\n"
    "║  System    : " << hw.cpu_count << " CPUs, "
        << std::fixed << std::setprecision(1) << hw.ram_total_gb << " GB RAM ("
        << (int)hw.ram_percent << "% used)\n"
    "║  Cached    : working=" << cs.working_count << "  all=" << cs.all_count << "\n"
    "║  ConfigDB  : " << db_size << " entries\n"
    "╠══════════════════════════════════════════════════════════════╣\n"
    "║  Workers: config_scanner, github_bg, harvester, validator    ║\n"
    "╚══════════════════════════════════════════════════════════════╝\n"
    << std::endl;
}

// ANSI color/style helpers
namespace ansi {
    const char* RESET   = "\033[0m";
    const char* BOLD    = "\033[1m";
    const char* DIM     = "\033[2m";
    const char* RED     = "\033[31m";
    const char* GREEN   = "\033[32m";
    const char* YELLOW  = "\033[33m";
    const char* BLUE    = "\033[34m";
    const char* MAGENTA = "\033[35m";
    const char* CYAN    = "\033[36m";
    const char* WHITE   = "\033[37m";
    const char* BG_RED  = "\033[41m";
    const char* BG_GREEN= "\033[42m";
    const char* CLEAR_SCREEN = "\033[2J\033[H";

    std::string bar(int filled, int total, int width = 20) {
        if (total <= 0) return std::string(width, '-');
        int f = (filled * width) / total;
        if (f > width) f = width;
        std::string b;
        for (int i = 0; i < width; i++) {
            if (i < f) b += "\033[32m\xe2\x96\x88\033[0m";  // green block
            else b += "\033[90m\xe2\x96\x91\033[0m";          // dark shade
        }
        return b;
    }

    std::string ramBar(float pct, int width = 20) {
        int f = (int)(pct * width / 100.0f);
        if (f > width) f = width;
        std::string b;
        for (int i = 0; i < width; i++) {
            if (i < f) {
                if (pct > 85) b += "\033[31m\xe2\x96\x88\033[0m";       // red
                else if (pct > 65) b += "\033[33m\xe2\x96\x88\033[0m";  // yellow
                else b += "\033[36m\xe2\x96\x88\033[0m";                 // cyan
            } else {
                b += "\033[90m\xe2\x96\x91\033[0m";
            }
        }
        return b;
    }
}

static int s_dashboard_frame = 0;

void HunterOrchestrator::printDashboard() {
    s_dashboard_frame++;
    const char* spinner[] = {"\xe2\x97\x89", "\xe2\x97\x8b", "\xe2\x97\x89", "\xe2\x97\x8b"};  // ◉ ◯
    const char* spin = spinner[s_dashboard_frame % 4];

    double start_time = 0.0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time = start_time_;
    }
    double uptime = utils::nowTimestamp() - start_time;
    int up_h = (int)(uptime / 3600);
    int up_m = ((int)uptime % 3600) / 60;
    int up_s = (int)uptime % 60;

    auto hw = HunterTaskManager::instance().getHardware();
    int validated = last_validated_count_.load();
    int cycles = cycle_count_.load();

    // DB stats
    int db_total = 0, db_alive = 0, db_untested = 0;
    float db_avg_lat = 0.0f;
    if (config_db_) {
        auto st = config_db_->getStats();
        db_total = st.total;
        db_alive = st.alive;
        db_untested = st.untested_unique;
        db_avg_lat = st.avg_latency_ms;
    }

    // Workers
    int w_running = 0, w_sleeping = 0, w_err = 0, w_total = 0;
    std::vector<std::pair<std::string, WorkerStatus>> worker_list;
    if (thread_manager_) {
        auto st = thread_manager_->getStatus();
        for (auto& [name, ws] : st.workers) {
            w_total++;
            if (ws.state == WorkerState::RUNNING) w_running++;
            else if (ws.state == WorkerState::SLEEPING) w_sleeping++;
            else if (ws.state == WorkerState::WORKER_ERROR) w_err++;
            worker_list.push_back({name, ws});
        }
    }

    char time_buf[32];
    std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", up_h, up_m, up_s);

    // ═══ Clear screen and draw ═══
    std::ostringstream out;
    out << ansi::CLEAR_SCREEN;

    // Header
    out << ansi::BOLD << ansi::CYAN
        << "  \xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"  // ███████
        << "  H U N T E R  " << spin << "  "
        << ansi::WHITE << time_buf
        << ansi::DIM << "  uptime" << ansi::RESET << "\n";

    out << ansi::CYAN
        << "  \xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"  // ┅┅┅┅┅┅┅
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << "\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81\xe2\x94\x81"
        << ansi::RESET << "\n\n";

    // ── Config Database ──
    out << "  " << ansi::BOLD << ansi::YELLOW << "\xe2\x96\xb6" << ansi::WHITE << " CONFIG DATABASE" << ansi::RESET << "\n";
    out << "    Total: " << ansi::BOLD << ansi::WHITE << db_total << ansi::RESET
        << "   " << ansi::GREEN << "\xe2\x97\x8f " << db_alive << " alive" << ansi::RESET
        << "   " << ansi::DIM << db_untested << " untested" << ansi::RESET
        << "   " << ansi::DIM << "avg " << (int)db_avg_lat << "ms" << ansi::RESET << "\n";
    out << "    " << ansi::bar(db_alive, db_total > 0 ? db_total : 1, 40) 
        << " " << ansi::DIM << (db_total > 0 ? (db_alive * 100 / db_total) : 0) << "% alive" << ansi::RESET << "\n\n";

    // ── Workers ──
    out << "  " << ansi::BOLD << ansi::YELLOW << "\xe2\x96\xb6" << ansi::WHITE << " WORKERS "
        << ansi::DIM << "(" << w_running << " run " << w_sleeping << " sleep";
    if (w_err > 0) out << " " << ansi::RED << w_err << " err" << ansi::DIM;
    out << ")" << ansi::RESET << "\n";

    for (auto& [name, ws] : worker_list) {
        const char* state_icon;
        const char* state_color;
        switch (ws.state) {
            case WorkerState::RUNNING:
                state_icon = "\xe2\x9a\xa1"; // ⚡
                state_color = ansi::GREEN;
                break;
            case WorkerState::SLEEPING:
                state_icon = "\xe2\x8f\xb8";  // ⏸
                state_color = ansi::DIM;
                break;
            case WorkerState::WORKER_ERROR:
                state_icon = "\xe2\x9c\x96"; // ✖
                state_color = ansi::RED;
                break;
            default:
                state_icon = "\xe2\x97\x8b"; // ◯
                state_color = ansi::DIM;
                break;
        }
        // Truncate name
        std::string short_name = name.size() > 18 ? name.substr(0, 18) : name;
        while (short_name.size() < 18) short_name += ' ';

        out << "    " << state_color << state_icon << " " << short_name << ansi::RESET;
        out << " runs:" << ansi::BOLD << ws.runs << ansi::RESET;
        if (ws.errors > 0) out << " " << ansi::RED << "err:" << ws.errors << ansi::RESET;
        if (ws.state == WorkerState::SLEEPING && ws.next_run_in > 0) {
            out << " " << ansi::DIM << "next:" << (int)ws.next_run_in << "s" << ansi::RESET;
        }
        out << "\n";
    }
    out << "\n";

    // ── System ──
    out << "  " << ansi::BOLD << ansi::YELLOW << "\xe2\x96\xb6" << ansi::WHITE << " SYSTEM" << ansi::RESET << "\n";
    out << "    RAM: " << ansi::ramBar(hw.ram_percent, 25) 
        << " " << ansi::BOLD << (int)hw.ram_percent << "%" << ansi::RESET
        << ansi::DIM << " (" << std::fixed << std::setprecision(1) << hw.ram_used_gb << "/" << hw.ram_total_gb << " GB)" << ansi::RESET << "\n";
    out << "    Cycles: " << ansi::BOLD << cycles << ansi::RESET
        << "    Validated: " << ansi::BOLD << ansi::GREEN << validated << ansi::RESET << "\n\n";

    // ── Live Activity ──
    auto recent_logs = utils::LogRingBuffer::instance().recent(12);
    if (!recent_logs.empty()) {
        out << "  " << ansi::BOLD << ansi::YELLOW << "\xe2\x96\xb6" << ansi::WHITE << " LIVE ACTIVITY" << ansi::RESET << "\n";
        for (auto& line : recent_logs) {
            // Color code based on content
            if (line.find("] OK ") != std::string::npos || line.find("TG-OK") != std::string::npos) {
                out << ansi::GREEN;
            } else if (line.find("FAIL") != std::string::npos || line.find("DEAD") != std::string::npos) {
                out << ansi::RED;
            } else if (line.find("SKIP") != std::string::npos || line.find("error=") != std::string::npos) {
                out << ansi::DIM;
            } else {
                out << ansi::DIM;
            }
            // Truncate long lines
            std::string display = line.size() > 90 ? line.substr(0, 87) + "..." : line;
            out << "   " << display << ansi::RESET << "\n";
        }
        out << "\n";
    }

    // Footer
    out << ansi::DIM << "  [Ctrl+C to stop | runtime/stop.flag for graceful shutdown]" << ansi::RESET << "\n";

    std::cout << out.str() << std::flush;
}

int HunterOrchestrator::loadBundleConfigs() {
    std::vector<std::string> bundle_files = {
        "bundle/telegram_configs.txt",
        "bundle/http_configs.txt",
        "bundle/mixed_configs.txt",
        "bundle/iran_configs.txt",
        "bundle/europe_configs.txt",
        "bundle/usa_configs.txt"
    };
    
    int total_loaded = 0;
    std::set<std::string> all_configs;
    
    for (const auto& file : bundle_files) {
        if (!utils::fileExists(file)) {
            std::cout << "[Bundle] " << file << " not found" << std::endl;
            continue;
        }
        
        auto lines = utils::readLines(file);
        if (lines.empty()) {
            std::cout << "[Bundle] " << file << " is empty" << std::endl;
            continue;
        }
        
        // Parse URIs from file
        int file_count = 0;
        for (const auto& line : lines) {
            std::string trimmed = utils::trim(line);
            if (!trimmed.empty() && trimmed.find("://") != std::string::npos) {
                all_configs.insert(trimmed);
                file_count++;
            }
        }
        
        std::cout << "[Bundle] Loaded " << file_count << " configs from " << file << std::endl;
    }
    
    // Add to ConfigDatabase
    if (config_db_ && !all_configs.empty()) {
        total_loaded = config_db_->addConfigs(all_configs, "bundle");
        std::cout << "[Bundle] Added " << total_loaded << " new configs to database" << std::endl;
    }
    
    return total_loaded;
}

bool HunterOrchestrator::detectCensorship() {
    std::cout << "[Censorship] Testing direct internet connectivity..." << std::endl;
    
    // Test direct TCP connections to known-good IPs
    std::vector<std::pair<std::string, int>> test_hosts = {
        {"1.1.1.1", 443},      // Cloudflare DNS
        {"8.8.8.8", 53},        // Google DNS
        {"1.0.0.1", 443},      // Cloudflare DNS
        {"208.67.222.222", 443} // OpenDNS
    };
    
    int failed = 0;
    for (auto& [host, port] : test_hosts) {
        auto sock = utils::createTcpSocket(host, port, 3.0); // 3 second timeout
        if (sock == INVALID_SOCKET) {
            failed++;
            std::cout << "[Censorship] " << host << ":" << port << " - FAILED" << std::endl;
        } else {
            utils::closeSocket(sock);
            std::cout << "[Censorship] " << host << ":" << port << " - OK" << std::endl;
        }
    }
    
    bool censored = (failed >= static_cast<int>(test_hosts.size()) - 1); // All or all but one failed
    if (censored) {
        std::cout << "[Censorship] *** CENSORSHIP DETECTED *** (" << failed << "/" << test_hosts.size() << " hosts blocked)" << std::endl;
    } else {
        std::cout << "[Censorship] Internet appears accessible (" << failed << "/" << test_hosts.size() << " hosts blocked)" << std::endl;
    }
    
    return censored;
}

int HunterOrchestrator::loadRawConfigFiles() {
    std::vector<std::string> raw_files = {
        "config/All_Configs_Sub.txt",
        "config/all_extracted_configs.txt",
        "config/sub.txt"
    };
    
    int total_loaded = 0;
    std::set<std::string> all_configs;
    
    for (const auto& file : raw_files) {
        if (!utils::fileExists(file)) {
            std::cout << "[RawFiles] " << file << " not found" << std::endl;
            continue;
        }
        
        auto lines = utils::readLines(file);
        if (lines.empty()) {
            std::cout << "[RawFiles] " << file << " is empty" << std::endl;
            continue;
        }
        
        // Parse URIs from file
        int file_count = 0;
        for (const auto& line : lines) {
            std::string trimmed = utils::trim(line);
            if (!trimmed.empty() && trimmed.find("://") != std::string::npos) {
                all_configs.insert(trimmed);
                file_count++;
            }
        }
        
        std::cout << "[RawFiles] Loaded " << file_count << " configs from " << file << std::endl;
    }
    
    // Add to ConfigDatabase
    if (config_db_ && !all_configs.empty()) {
        total_loaded = config_db_->addConfigs(all_configs, "raw_file");
        std::cout << "[RawFiles] Added " << total_loaded << " new configs to database" << std::endl;
    }
    
    return total_loaded;
}

bool HunterOrchestrator::testCachedConfigs() {
    // This is now a no-op — validation is handled by ValidatorWorker
    // which runs in parallel with proper batching and status updates.
    // Keeping the method for API compatibility.
    utils::LogRingBuffer::instance().push("[CachedTest] Delegated to ValidatorWorker");
    return false;
}

bool HunterOrchestrator::emergencyBootstrap() {
    // Emergency bootstrap is now handled inline in start() after workers are running.
    // The ValidatorWorker handles parallel testing with proper status updates.
    std::cout << "[Emergency] Bootstrap delegated to ValidatorWorker" << std::endl;
    return false;
}

bool HunterOrchestrator::downloadConfigsAsync(const std::vector<std::string>& sources, const std::string& proxy) {
    bool expected = false;
    if (!download_in_progress_.compare_exchange_strong(expected, true)) {
        utils::LogRingBuffer::instance().push("[Download] Skipped: another download is already running");
        return false;
    }
    struct DownloadGuard {
        std::atomic<bool>& flag;
        ~DownloadGuard() { flag.store(false); }
    } guard{download_in_progress_};

    std::cout << "[Download] downloadConfigsAsync started with " << sources.size() << " sources" << std::endl;
    utils::LogRingBuffer::instance().push("[Download] Started with " + std::to_string(sources.size()) + " sources");
    
    int total_sources = static_cast<int>(sources.size());
    int downloaded_count = 0;
    int completed_sources = 0;
    int successful_sources = 0;
    
    if (sources.empty()) {
        std::cout << "[Download] ERROR: No sources provided!" << std::endl;
        return false;
    }
    
    std::cout << "[Download] Sources to download:" << std::endl;
    for (size_t i = 0; i < sources.size(); ++i) {
        std::cout << "[Download]   " << (i+1) << ". " << sources[i] << std::endl;
    }
    
    // Build proxy fallback chain: direct -> system proxy -> app proxy -> discovered healthy proxies
    std::vector<std::string> proxy_chain;
    
    // 1. Direct connection (empty string)
    proxy_chain.push_back("");
    
    // 2. Windows system proxy if available
    network::SysProxy sys_proxy;
    if (sys_proxy.isEnabled()) {
        int system_port = sys_proxy.getActivePort();
        if (system_port > 0) {
            std::string system_proxy_url = "127.0.0.1:" + std::to_string(system_port);
            proxy_chain.push_back(system_proxy_url);
            std::cout << "[Download] Added Windows system proxy to fallback chain: " << system_proxy_url << std::endl;
        }
    }
    
    // 3. App-configured proxy if provided and different from system proxy
    if (!proxy.empty()) {
        // Check if this proxy is already in the chain
        if (std::find(proxy_chain.begin(), proxy_chain.end(), proxy) == proxy_chain.end()) {
            proxy_chain.push_back(proxy);
            std::cout << "[Download] Added app-configured proxy to fallback chain: " << proxy << std::endl;
        }
    }
    
    
    std::cout << "[Download] Proxy fallback chain has " << proxy_chain.size() << " options" << std::endl;
    
    // Test proxy connectivity and build working proxy list
    std::vector<std::string> working_proxies;
    static const std::vector<std::string> connectivity_urls = {
        "https://www.gstatic.com/generate_204",
        "https://cp.cloudflare.com/generate_204",
        "https://httpbin.org/ip"
    };
    std::cout << "[Download] Testing proxy connectivity using "
              << connectivity_urls.size() << " probe URLs" << std::endl;
    
    for (const auto& test_proxy : proxy_chain) {
        std::string proxy_url;
        if (!test_proxy.empty()) {
            proxy_url = "socks5h://" + test_proxy;
        }
        
        std::cout << "[Download] Testing connectivity: " << (test_proxy.empty() ? "direct" : test_proxy) << std::endl;
        
        bool proxy_ok = false;
        for (const auto& test_url : connectivity_urls) {
            try {
                std::string test_content = http_client_.get(test_url, 5000, proxy_url);
                if (!test_content.empty()) {
                    proxy_ok = true;
                    break;
                }
            } catch (...) {}
        }

        if (proxy_ok) {
            working_proxies.push_back(test_proxy);
            std::cout << "[Download] Connectivity test PASSED for: "
                      << (test_proxy.empty() ? "direct" : test_proxy) << std::endl;
        } else {
            std::cout << "[Download] Connectivity test FAILED for: "
                      << (test_proxy.empty() ? "direct" : test_proxy) << std::endl;
            }
    }
    
    if (working_proxies.empty()) {
        std::cout << "[Download] Connectivity probes failed for all options; attempting fallback chain anyway" << std::endl;
        working_proxies = proxy_chain;
    }
    
    std::cout << "[Download] Found " << working_proxies.size() << " working connectivity options" << std::endl;
    
    // Emit initial progress
    auto emitProgress = [&](const std::string& current_source, float progress, const std::string& status, const std::string& used_proxy = "") {
        utils::JsonBuilder dj;
        dj.add("type", "download_progress")
          .add("current_source", current_source)
          .add("progress", progress)
          .add("downloaded_count", completed_sources)
          .add("total_count", total_sources)
          .add("status", status)
          .add("proxy", used_proxy);
        
        std::string msg = "##DOWNLOAD_PROGRESS##" + dj.build();
        std::cout << msg << std::endl;
        utils::LogRingBuffer::instance().push(msg);
    };
    
    auto emitLog = [&](const std::string& log_msg) {
        std::string msg = "##DOWNLOAD_LOG##" + log_msg;
        std::cout << msg << std::endl;
        utils::LogRingBuffer::instance().push(msg);
    };
    
    emitProgress("", 0.0f, "starting", "");
    std::cout << "[Download] Starting download of " << total_sources << " sources" << std::endl;
    emitLog("Starting download of " + std::to_string(total_sources) + " sources");
    
    // Download each source with proxy fallback
    for (size_t i = 0; i < sources.size(); ++i) {
        std::cout << "[Download] === Starting source " << (i+1) << "/" << total_sources << " ===" << std::endl;
        
        if (stop_requested_.load()) {
            std::cout << "[Download] Download stopped by user request" << std::endl;
            emitProgress("", static_cast<float>(downloaded_count) / total_sources, "stopped", "");
            return successful_sources > 0;
        }
        
        const std::string& source = sources[i];
        float progress = static_cast<float>(i) / total_sources;
        emitProgress(source, progress, "downloading", "");
        std::cout << "[Download] Processing source " << (i+1) << "/" << total_sources << ": " << source << std::endl;
        emitLog("Processing source " + std::to_string(i+1) + "/" + std::to_string(total_sources) + ": " + source);
        
        std::string content;
        bool success = false;
        int configs_found = 0;
        int unique_added = 0;
        std::string error_msg;
        std::string successful_proxy;
        
        // Try each working proxy in order until one succeeds
        for (const auto& working_proxy : working_proxies) {
            if (stop_requested_.load()) break;
            
            std::string proxy_url;
            if (!working_proxy.empty()) {
                // Support both HTTP and SOCKS proxies
                if (working_proxy.find("://") != std::string::npos) {
                    proxy_url = working_proxy;  // Already has protocol
                } else {
                    proxy_url = "socks5h://" + working_proxy;  // Default to SOCKS5
                }
            }
            
            std::cout << "[Download] Trying " << (working_proxy.empty() ? "direct connection" : "proxy " + proxy_url) 
                      << " for " << source << std::endl;
            emitLog("Trying " + std::string(working_proxy.empty() ? "direct connection" : "proxy " + proxy_url) + " for " + source);
            
            try {
                std::cout << "[Download] Making HTTP request to: " << source << std::endl;
                std::cout << "[Download] Using proxy: " << (proxy_url.empty() ? "none (direct)" : proxy_url) << std::endl;
                std::cout << "[Download] Timeout: 15 seconds" << std::endl;
                
                content = http_client_.get(source, 15000, proxy_url);  // 15 second timeout
                success = !content.empty();
                
                std::cout << "[Download] HTTP request completed. Success: " << (success ? "YES" : "NO") << std::endl;
                std::cout << "[Download] Content length: " << content.length() << " bytes" << std::endl;
                
                if (success) {
                    successful_proxy = working_proxy;
                    std::cout << "[Download] SUCCESS: Downloaded " << content.length()
                              << " bytes from " << source << " via "
                              << (working_proxy.empty() ? "direct connection" : "proxy " + working_proxy) << std::endl;
                    emitLog("SUCCESS: Downloaded " + std::to_string(content.length()) + " bytes from " + source + " via " + (working_proxy.empty() ? "direct connection" : "proxy " + working_proxy));
                    utils::LogRingBuffer::instance().push("[Download] SUCCESS: " + std::to_string(content.length()) + " bytes from " + source);
                    
                    // Show content preview
                    std::string preview = content.substr(0, 200);
                    std::cout << "[Download] Content preview: " << preview << "..." << std::endl;
                    
                    break;  // Success, no need to try more proxies
                } else {
                    std::cout << "[Download] FAILED: Empty response from " << source << std::endl;
                    std::cout << "[Download] Failed via " << (working_proxy.empty() ? "direct connection" : "proxy " + working_proxy) 
                              << ", trying next option..." << std::endl;
                    emitLog("FAILED: " + std::string(working_proxy.empty() ? "direct connection" : "proxy " + working_proxy) + " failed for " + source);
                }
            } catch (const std::exception& e) {
                std::cout << "[Download] EXCEPTION during HTTP request: " << e.what() << std::endl;
                std::cout << "[Download] Exception occurred with proxy: " << (working_proxy.empty() ? "direct connection" : working_proxy) << std::endl;
                emitLog("ERROR: Exception via " + std::string(working_proxy.empty() ? "direct connection" : "proxy " + working_proxy) + ": " + e.what() + " for " + source);
            }
        }
        
        if (success && !content.empty()) {
            std::cout << "[Download] Successfully downloaded " << content.length() 
                      << " bytes from " << source << std::endl;
            
            // Parse and add configs to main database
            int invalid_configs = 0;
            std::set<std::string> valid_configs = extractDownloadConfigs(content, &invalid_configs);
            configs_found = static_cast<int>(valid_configs.size());
            std::cout << "[Download] Parsed " << configs_found << " config lines from " << source << std::endl;
            emitLog("Parsed " + std::to_string(configs_found) + " configs from " + source);
            
            if (!valid_configs.empty() && config_db_) {
                int promoted_existing = 0;
                unique_added = config_db_->addConfigsWithPriority(valid_configs, "download", &promoted_existing);
                downloaded_count += unique_added;
                std::cout << "[Download] Added " << unique_added << " unique configs from " << source
                          << " (found " << configs_found << " total, refreshed "
                          << promoted_existing << " existing, invalid " << invalid_configs << ")" << std::endl;
                emitLog("Added " + std::to_string(unique_added) + " unique configs from " + source);
                utils::LogRingBuffer::instance().push("[Download] Added " + std::to_string(unique_added) + " configs from " + source +
                    " (total " + std::to_string(configs_found) + ", invalid " + std::to_string(invalid_configs) + ")");
                if (promoted_existing > 0) {
                    emitLog("Refreshed " + std::to_string(promoted_existing) + " existing configs from " + source);
                }
                // Persist main DB immediately so new downloads are usable even after restart/crash.
                if (unique_added > 0 || promoted_existing > 0) {
                    try {
                        const int saved_rows = config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
                        emitLog("Persisted DB snapshot (" + std::to_string(saved_rows) + " rows) after " + source);
                    } catch (const std::exception& e) {
                        emitLog("WARNING: Failed to persist DB after " + source + ": " + e.what());
                    }
                }
            } else if (valid_configs.empty()) {
                std::cout << "[Download] WARNING: No valid config formats found in " << source << std::endl;
                emitLog("WARNING: No valid configs found in " + source);
            } else if (!config_db_) {
                std::cout << "[Download] ERROR: Config database not available for " << source << std::endl;
                emitLog("ERROR: Config database not available");
            }
            
            progress = static_cast<float>(i + 1) / total_sources;
            completed_sources = static_cast<int>(i + 1);
            successful_sources++;
            emitProgress(source, progress, "completed", successful_proxy);
            
        } else {
            error_msg = "All connectivity options failed";
            std::cout << "[Download] Failed to download from " << source
                      << " after trying " << working_proxies.size() << " connectivity options" << std::endl;
            emitLog("FAILED: Could not download from " + source + " - all proxies failed");
            utils::LogRingBuffer::instance().push("[Download] FAILED: " + source);
            completed_sources = static_cast<int>(i + 1);
            emitProgress(source, progress, "failed", "");
        }
        
        // Record download history
        utils::JsonBuilder history;
        history.add("type", "download_history")
              .add("url", source)
              .add("timestamp", utils::nowTimestamp())
              .add("success", success)
              .add("configs_found", configs_found)
              .add("unique_configs", unique_added)
              .add("error", error_msg)
              .add("proxy_used", successful_proxy);
        std::string history_msg = "##DOWNLOAD_HISTORY##" + history.build();
        std::cout << history_msg << std::endl;
        utils::LogRingBuffer::instance().push(history_msg);
        
        // Small delay between downloads
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Final progress
    emitProgress("", 1.0f, "finished", "");
    std::cout << "[Download] Finished. Downloaded " << downloaded_count << " unique configs from "
              << total_sources << " sources using proxy fallback chain" << std::endl;
    std::cout << "[Download] Summary: " << downloaded_count << " configs added, "
              << total_sources << " sources processed" << std::endl;
    emitLog("COMPLETED: Downloaded " + std::to_string(downloaded_count) + " unique configs from " + std::to_string(total_sources) + " sources");
    utils::LogRingBuffer::instance().push("[Download] COMPLETED: " + std::to_string(downloaded_count) + " configs from " + std::to_string(total_sources) + " sources");
    return successful_sources > 0;
}

} // namespace hunter
