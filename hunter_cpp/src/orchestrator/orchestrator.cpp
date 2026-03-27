#include "orchestrator/orchestrator.h"
#include "orchestrator/thread_manager.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "network/proxy_tester.h"
#include "network/sys_proxy.h"
#include "realtime/websocket_bridge.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <chrono>
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

std::string jsonStringArray(const std::vector<std::string>& values) {
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

int reserveTemporaryListenPort() {
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

float testProvisionedPortDownload(int port, int timeout_seconds) {
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

bool loadImportCandidates(const std::string& file_path, std::set<std::string>& valid_configs, int& invalid_count) {
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

} 

HunterOrchestrator::HunterOrchestrator(HunterConfig& config)
    : config_(config),
      config_fetcher_(http_client_),
      xray_manager_(),
      benchmarker_(xray_manager_) {
    // HTTP client auto-initializes CURL in its constructor
    initComponents();
    std::cout << "Hunter Orchestrator initialized successfully" << std::endl;
    std::cout << "[Init] HTTP client ready, config database ready" << std::endl;
}

HunterOrchestrator::~HunterOrchestrator() {
    stop();
}

void HunterOrchestrator::initComponents() {
    // XRay path
    xray_manager_.setXRayPath(config_.xrayPath());
    runtime_engine_manager_.setXRayPath(config_.xrayPath());
    runtime_engine_manager_.setSingBoxPath(config_.singBoxPath());
    runtime_engine_manager_.setMihomoPath(config_.mihomoPath());

    // Config database
    config_db_ = std::make_unique<network::ConfigDatabase>(constants::CONFIG_DB_MAX_SIZE);
    
    // Load persisted config database from disk (survives restarts)
    {
        std::string db_path = "runtime/HUNTER_config_db.tsv";
        int db_loaded = config_db_->loadFromDisk(db_path);
        if (db_loaded > 0) {
            std::cout << "[Startup] Restored " << db_loaded << " configs from disk database" << std::endl;
        }
    }

    // Continuous validator
    continuous_validator_ = std::make_unique<network::ContinuousValidator>(*config_db_);

    // Flexible fetcher
    flexible_fetcher_ = std::make_unique<network::FlexibleFetcher>(config_fetcher_);

    // Load balancer
    balancer_ = std::make_unique<proxy::MultiProxyServer>(config_.multiproxyPort());
    gemini_balancer_ = std::make_unique<proxy::MultiProxyServer>(config_.geminiPort());
    balancer_->setXRayPath(config_.xrayPath());
    balancer_->setSingBoxPath(config_.singBoxPath());
    balancer_->setMihomoPath(config_.mihomoPath());
    gemini_balancer_->setXRayPath(config_.xrayPath());
    gemini_balancer_->setSingBoxPath(config_.singBoxPath());
    gemini_balancer_->setMihomoPath(config_.mihomoPath());
    auto engine_hint_cb = [this](const std::string& uri) -> std::string {
        if (config_db_) {
            const std::string db_hint = config_db_->getPreferredEngine(uri);
            if (!db_hint.empty()) return db_hint;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = cached_engine_hints_.find(uri);
        return it != cached_engine_hints_.end() ? it->second : "";
    };
    balancer_->setEngineHintCallback(engine_hint_cb);
    gemini_balancer_->setEngineHintCallback(engine_hint_cb);

    // Load balancer caches
    loadBalancerCache("HUNTER_balancer_cache.json", balancer_.get());
    loadBalancerCache("HUNTER_gemini_balancer_cache.json", gemini_balancer_.get());

    // DPI evasion
    try {
        dpi_evasion_ = std::make_unique<security::DpiEvasionOrchestrator>();
    } catch (...) {}

    // Obfuscation engine
    obfuscation_ = std::make_unique<security::ObfuscationEngine>();

    // Telegram bot reporter
    std::string bot_token = HunterConfig::getEnv("TELEGRAM_BOT_TOKEN");
    std::string chat_id = HunterConfig::getEnv("CHAT_ID",
                          HunterConfig::getEnv("TELEGRAM_GROUP_ID"));
    if (!bot_token.empty() && !chat_id.empty()) {
        bot_reporter_ = std::make_unique<telegram::BotReporter>();
        bot_reporter_->configure(bot_token, chat_id);
    }

    // Smart cache
    std::string cache_dir = utils::dirName(config_.stateFile());
    if (cache_dir.empty()) cache_dir = "runtime";
    cache_ = std::make_unique<cache::SmartCache>(cache_dir);
}

void HunterOrchestrator::start() {
    stop_requested_ = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time_ = utils::nowTimestamp();
    }

    std::string runtime_dir = utils::dirName(config_.stateFile());
    if (runtime_dir.empty()) runtime_dir = "runtime";
    const std::string stop_flag = runtime_dir + "/stop.flag";
    const std::string cmd_file = runtime_dir + "/hunter_command.json";

    // ═══ PHASE -1: Kill processes occupying required ports ═══
    killPortOccupants();

    // ═══ PHASE 0: Load initial configs into database ═══
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
    
    // Write initial status file so UI sees real data immediately
    writeStatusFile("startup");

    // ═══ PHASE 1: Detect censorship ═══
    bool is_censored = detectCensorship();
    writeStatusFile(is_censored ? "censored" : "open");
    
    // NOTE: Do NOT block startup with long emergency bootstrap.
    // Start workers first (Phase 4), then test cached configs in background.
    // This ensures the UI gets status updates and validation starts quickly.

    // ═══ PHASE 1: Immediate connectivity from cached configs ═══
    std::vector<std::pair<std::string, float>> main_cached_configs;
    std::vector<std::pair<std::string, float>> gemini_cached_configs;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto main_cached = cached_configs_.find("HUNTER_balancer_cache.json");
        if (main_cached != cached_configs_.end()) main_cached_configs = main_cached->second;
        auto gemini_cached = cached_configs_.find("HUNTER_gemini_balancer_cache.json");
        if (gemini_cached != cached_configs_.end()) gemini_cached_configs = gemini_cached->second;
    }

    // Also load working configs from SmartCache as fallback
    std::vector<std::pair<std::string, float>> fallback_configs;
    if (cache_) {
        auto working = cache_->loadCachedConfigs(200, true);
        for (auto& uri : working) {
            if (!uri.empty() && uri.find("://") != std::string::npos) {
                fallback_configs.emplace_back(uri, 500.0f);
            }
        }
    }

    // Start main balancer with cached configs
    if (balancer_) {
        if (!main_cached_configs.empty()) {
            if (balancer_->start(main_cached_configs)) {
                std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                          << " <- " << main_cached_configs.size() << " cached configs" << std::endl;
            } else {
                utils::LogRingBuffer::instance().push(
                    "[Startup] Main balancer failed on port " + std::to_string(config_.multiproxyPort()));
            }
        } else if (!fallback_configs.empty()) {
            if (balancer_->start(fallback_configs)) {
                std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                          << " <- " << fallback_configs.size() << " fallback configs" << std::endl;
            } else {
                utils::LogRingBuffer::instance().push(
                    "[Startup] Main balancer failed on port " + std::to_string(config_.multiproxyPort()));
            }
        } else {
            balancer_->start();
            std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                      << " (empty, waiting for first scan)" << std::endl;
        }
    }

    // Start gemini balancer with cached configs
    if (gemini_balancer_) {
        if (!gemini_cached_configs.empty()) {
            if (gemini_balancer_->start(gemini_cached_configs)) {
                std::cout << "[Startup] Gemini balancer :" << config_.geminiPort()
                          << " <- " << gemini_cached_configs.size() << " cached configs" << std::endl;
            } else {
                utils::LogRingBuffer::instance().push(
                    "[Startup] Gemini balancer failed on port " + std::to_string(config_.geminiPort()));
            }
        } else if (!fallback_configs.empty()) {
            int half = std::max(1, (int)fallback_configs.size() / 2);
            std::vector<std::pair<std::string, float>> gemini_fb(
                fallback_configs.begin() + half, fallback_configs.end());
            if (!gemini_fb.empty()) {
                if (!gemini_balancer_->start(gemini_fb)) {
                    utils::LogRingBuffer::instance().push(
                        "[Startup] Gemini balancer failed on port " + std::to_string(config_.geminiPort()));
                }
            }
            else gemini_balancer_->start();
        } else {
            gemini_balancer_->start();
        }
    }

    // ═══ PHASE 1b: Provision individual ports 2901-2999 ═══
    provisionPorts();

    // ═══ PHASE 2: Start DPI evasion engines ═══
    if (dpi_evasion_) {
        try { dpi_evasion_->start(); } catch (...) {}
    }

    // ═══ PHASE 3: Print startup banner ═══
    printStartupBanner();

    // ═══ PHASE 4: Start all worker threads (background fetch + validation) ═══
    if (thread_manager_ && !thread_manager_->isRunning()) {
        thread_manager_.reset();
    }
    thread_manager_ = std::make_unique<orchestrator::ThreadManager>(this);
    thread_manager_->startAll();
    std::cout << "[Startup] All worker threads started" << std::endl;
    writeStatusFile("running");

    // ═══ PHASE 4b: Emergency bootstrap (non-blocking, runs alongside workers) ═══
    if (is_censored) {
        std::cout << "[Censorship] Starting emergency bootstrap alongside workers..." << std::endl;
        // Quick test: try 3 small parallel batches (total ~60 configs)
        // Workers are already running so validator will also be testing
        for (int attempt = 0; attempt < 3 && !stop_requested_.load(); attempt++) {
            if (utils::fileExists(stop_flag)) {
                try { std::filesystem::remove(stop_flag); } catch (...) {}
                std::cout << "\n[Hunter] stop.flag detected, shutting down..." << std::endl;
                stop();
                break;
            }
            if (utils::fileExists(cmd_file)) {
                try {
                    std::string cmd_json = utils::loadJsonFile(cmd_file);
                    std::filesystem::remove(cmd_file);
                    processStdinCommand(cmd_json);
                } catch (...) {}
                if (stop_requested_.load()) break;
            }
            auto healthy = config_db_ ? config_db_->getHealthyConfigs(5) : std::vector<std::pair<std::string,float>>{};
            if (!healthy.empty()) {
                std::cout << "[Emergency] Found " << healthy.size() << " working configs from validator" << std::endl;
                if (balancer_) {
                    balancer_->updateAvailableConfigs(healthy, true);
                    saveBalancerCache("HUNTER_balancer_cache.json", healthy);
                }
                break;
            }
            for (int wait_step = 0; wait_step < 50 && !stop_requested_.load(); ++wait_step) {
                if (utils::fileExists(stop_flag)) {
                    try { std::filesystem::remove(stop_flag); } catch (...) {}
                    std::cout << "\n[Hunter] stop.flag detected, shutting down..." << std::endl;
                    stop();
                    break;
                }
                if (utils::fileExists(cmd_file)) {
                    try {
                        std::string cmd_json = utils::loadJsonFile(cmd_file);
                        std::filesystem::remove(cmd_file);
                        processStdinCommand(cmd_json);
                    } catch (...) {}
                    if (stop_requested_.load()) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            writeStatusFile("bootstrap");
        }
    }

    // Apply initial speed profile from hardware
    {
        auto hw = HunterTaskManager::instance().getHardware();
        if (hw.cpu_count <= 2) applyAutoProfile("low");
        else if (hw.cpu_count <= 4) applyAutoProfile("medium");
        else applyAutoProfile("high");
    }

    // ═══ PHASE 5: Main loop with real-time status emission ═══
    int provision_tick = 0;
    int dashboard_tick = 0;
    int db_save_tick = 0;
    while (!stop_requested_.load() && thread_manager_ && thread_manager_->isRunning()) {
        if (utils::fileExists(stop_flag)) {
            try { std::filesystem::remove(stop_flag); } catch (...) {}
            std::cout << "\n[Hunter] stop.flag detected, shutting down..." << std::endl;
            stop();
            break;
        }

        if (utils::fileExists(cmd_file)) {
            try {
                std::string cmd_json = utils::loadJsonFile(cmd_file);
                std::filesystem::remove(cmd_file);
                processStdinCommand(cmd_json);
            } catch (...) {}
            if (stop_requested_.load()) break;
        }

        // Emit real-time status to stdout (##STATUS## line every 2s)
        std::string current_phase = paused_.load() ? "paused" : "running";
        emitStatusJson(current_phase);

        // Pause loop: still emit status but skip dashboard/provision
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Print console dashboard every ~8s
        if (++dashboard_tick >= 4) {
            dashboard_tick = 0;
            printDashboard();
        }

        // Dead proxy detection + replacement every ~30s
        if (++provision_tick >= 15) {
            provision_tick = 0;
            try { refreshProvisionedPorts(); } catch (...) {}
        }

        // Persist ConfigDB to disk every ~60s
        if (++db_save_tick >= 30) {
            db_save_tick = 0;
            if (config_db_) {
                try {
                    int saved = config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
                    if (saved > 0) {
                        utils::LogRingBuffer::instance().push(
                            "[DB] Persisted " + std::to_string(saved) + " configs to disk");
                    }
                } catch (...) {}
            }
        }

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
    
    // Persist ConfigDB before shutdown
    if (config_db_) {
        try {
            int saved = config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
            std::cout << "[Shutdown] Saved " << saved << " configs to disk" << std::endl;
        } catch (...) {}
    }
    
    if (thread_manager_) {
        thread_manager_->stopAll();
        thread_manager_.reset();
    }
    stopProvisionedPorts();
    if (dpi_evasion_) dpi_evasion_->stop();
    if (balancer_) balancer_->stop();
    if (gemini_balancer_) gemini_balancer_->stop();
    runtime_engine_manager_.stopAll();
    xray_manager_.stopAll();
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
        cached_configs_["HUNTER_balancer_cache.json"].clear();
        cached_configs_["HUNTER_gemini_balancer_cache.json"].clear();
    }

    if (balancer_) {
        balancer_->clearForcedBackend();
        balancer_->updateAvailableConfigs({}, true);
    }
    if (gemini_balancer_) {
        gemini_balancer_->clearForcedBackend();
        gemini_balancer_->updateAvailableConfigs({}, true);
    }

    stopProvisionedPorts();
    saveBalancerCache("HUNTER_balancer_cache.json", {});
    saveBalancerCache("HUNTER_gemini_balancer_cache.json", {});

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
        "[Cmd] Cleared alive configs: db=" + std::to_string(removed) +
        " and reset balancer/runtime live caches");
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

    auto filterBalancerPool = [&](proxy::MultiProxyServer* bal, const std::string& cache_name) {
        if (!bal) return;
        const auto pool = bal->getAvailableConfigsList();
        std::vector<std::pair<std::string, float>> filtered;
        filtered.reserve(pool.size());
        for (const auto& entry : pool) {
            if (uris.find(entry.uri) == uris.end()) {
                filtered.emplace_back(entry.uri, entry.latency);
            }
        }
        bal->updateAvailableConfigs(filtered, true);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cached_configs_[cache_name] = filtered;
        }
        saveBalancerCache(cache_name, filtered);
    };
    filterBalancerPool(balancer_.get(), "HUNTER_balancer_cache.json");
    filterBalancerPool(gemini_balancer_.get(), "HUNTER_gemini_balancer_cache.json");

    bool touched_ports = false;
    {
        std::lock_guard<std::mutex> lock(provision_mutex_);
        for (auto& slot : provisioned_ports_) {
            if (slot.uri.empty() || uris.find(slot.uri) == uris.end()) continue;
            if (slot.pid > 0) runtime_engine_manager_.stopProcess(slot.pid);
            slot.uri.clear();
            slot.engine_used.clear();
            slot.pid = -1;
            slot.alive = false;
            slot.tcp_alive = false;
            slot.socks_ready = false;
            slot.http_ready = false;
            slot.latency_ms = 0.0f;
            slot.consecutive_failures = 0;
            touched_ports = true;
        }
    }
    if (touched_ports) {
        provisionPorts();
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

// ─── Real-time UI Communication ───

std::string HunterOrchestrator::processRealtimeCommand(const std::string& json_line) {
    const double received_ts = utils::nowTimestamp();
    const double received_ts_ms = received_ts * 1000.0;
    auto decodeJsonString = [](const std::string& encoded) -> std::string {
        std::string out;
        out.reserve(encoded.size());
        bool escape = false;
        for (size_t i = 0; i < encoded.size(); ++i) {
            const char ch = encoded[i];
            if (!escape) {
                if (ch == '\\') {
                    escape = true;
                } else {
                    out.push_back(ch);
                }
                continue;
            }

            switch (ch) {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(ch); break;
            }
            escape = false;
        }
        if (escape) out.push_back('\\');
        return out;
    };
    auto extractString = [&](const std::string& key) -> std::string {
        const std::string needle = "\"" + key + "\"";
        auto key_pos = json_line.find(needle);
        if (key_pos == std::string::npos) return "";
        auto colon = json_line.find(':', key_pos + needle.size());
        if (colon == std::string::npos) return "";
        size_t pos = colon + 1;
        while (pos < json_line.size() && std::isspace(static_cast<unsigned char>(json_line[pos]))) pos++;
        auto first_quote = json_line.find('"', pos);
        if (first_quote == std::string::npos) return "";
        std::string encoded;
        bool escape = false;
        for (size_t i = first_quote + 1; i < json_line.size(); ++i) {
            const char ch = json_line[i];
            if (!escape && ch == '"') {
                return decodeJsonString(encoded);
            }
            encoded.push_back(ch);
            if (!escape && ch == '\\') escape = true;
            else escape = false;
        }
        return "";
    };
    auto extractInt = [&](const std::string& key, int def) -> int {
        const std::string needle = "\"" + key + "\"";
        auto key_pos = json_line.find(needle);
        if (key_pos == std::string::npos) return def;
        auto colon = json_line.find(':', key_pos + needle.size());
        if (colon == std::string::npos) return def;
        size_t pos = colon + 1;
        while (pos < json_line.size() && std::isspace(static_cast<unsigned char>(json_line[pos]))) pos++;
        return std::atoi(json_line.c_str() + pos);
    };
    auto extractBool = [&](const std::string& key, bool def) -> bool {
        const std::string needle = "\"" + key + "\"";
        auto key_pos = json_line.find(needle);
        if (key_pos == std::string::npos) return def;
        auto colon = json_line.find(':', key_pos + needle.size());
        if (colon == std::string::npos) return def;
        size_t pos = colon + 1;
        while (pos < json_line.size() && std::isspace(static_cast<unsigned char>(json_line[pos]))) pos++;
        if (json_line.compare(pos, 4, "true") == 0) return true;
        if (json_line.compare(pos, 5, "false") == 0) return false;
        return def;
    };

    auto hasKey = [&](const std::string& key) -> bool {
        return json_line.find("\"" + key + "\"") != std::string::npos;
    };
    auto splitTextLines = [](const std::string& raw) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::istringstream ss(raw);
        std::string line;
        while (std::getline(ss, line)) {
            std::string trimmed = utils::trim(line);
            if (!trimmed.empty()) out.push_back(trimmed);
        }
        return out;
    };
    auto applyRuntimeEnginePaths = [this]() {
        xray_manager_.setXRayPath(config_.xrayPath());
        runtime_engine_manager_.setXRayPath(config_.xrayPath());
        runtime_engine_manager_.setSingBoxPath(config_.singBoxPath());
        runtime_engine_manager_.setMihomoPath(config_.mihomoPath());
        if (balancer_) {
            balancer_->setXRayPath(config_.xrayPath());
            balancer_->setSingBoxPath(config_.singBoxPath());
            balancer_->setMihomoPath(config_.mihomoPath());
        }
        if (gemini_balancer_) {
            gemini_balancer_->setXRayPath(config_.xrayPath());
            gemini_balancer_->setSingBoxPath(config_.singBoxPath());
            gemini_balancer_->setMihomoPath(config_.mihomoPath());
        }
    };

    const std::string request_id = extractString("request_id");
    const bool quiet = extractBool("quiet", false);
    std::string command = extractString("command");
    if (command.empty()) {
        if (json_line.find("\"pause\"") != std::string::npos) command = "pause";
        else if (json_line.find("\"resume\"") != std::string::npos) command = "resume";
        else if (json_line.find("\"speed_profile\"") != std::string::npos) command = "speed_profile";
        else if (json_line.find("\"set_speed\"") != std::string::npos) command = "set_speed";
        else if (json_line.find("\"set_threads\"") != std::string::npos) command = "set_threads";
        else if (json_line.find("\"set_timeout\"") != std::string::npos) command = "set_timeout";
        else if (json_line.find("\"clear_old\"") != std::string::npos) command = "clear_old";
        else if (json_line.find("\"clear_alive\"") != std::string::npos) command = "clear_alive";
        else if (json_line.find("\"remove_configs\"") != std::string::npos) command = "remove_configs";
        else if (json_line.find("\"add_configs\"") != std::string::npos) command = "add_configs";
        else if (json_line.find("\"import_config_file\"") != std::string::npos) command = "import_config_file";
        else if (json_line.find("\"export_config_db\"") != std::string::npos) command = "export_config_db";
        else if (json_line.find("\"run_cycle\"") != std::string::npos) command = "run_cycle";
        else if (json_line.find("\"refresh_ports\"") != std::string::npos) command = "refresh_ports";
        else if (json_line.find("\"recheck_live_ports\"") != std::string::npos) command = "recheck_live_ports";
        else if (json_line.find("\"reprovision_ports\"") != std::string::npos) command = "reprovision_ports";
        else if (json_line.find("\"load_raw_files\"") != std::string::npos) command = "load_raw_files";
        else if (json_line.find("\"load_bundle_files\"") != std::string::npos) command = "load_bundle_files";
        else if (json_line.find("\"detect_censorship\"") != std::string::npos) command = "detect_censorship";
        else if (json_line.find("\"update_runtime_settings\"") != std::string::npos) command = "update_runtime_settings";
        else if (json_line.find("\"edge_router_bypass\"") != std::string::npos) command = "edge_router_bypass";
        else if (json_line.find("\"download_configs\"") != std::string::npos) command = "download_configs";
        else if (json_line.find("\"stop\"") != std::string::npos) command = "stop";
        else if (json_line.find("\"get_status\"") != std::string::npos) command = "get_status";
        else if (json_line.find("\"ping\"") != std::string::npos) command = "ping";
    }

    bool ok = true;
    std::string message = "ok";
    std::string data_json;

    try {
        if (command == "pause") {
            pause();
            message = "paused";
        } else if (command == "resume") {
            resume();
            message = "resumed";
        } else if (command == "speed_profile") {
            std::string value = extractString("value");
            if (value.empty()) value = "medium";
            applyAutoProfile(value);
            message = "speed_profile_applied";
        } else if (command == "set_speed") {
            int threads = extractInt("threads", speed_max_threads_.load());
            int timeout = extractInt("timeout", speed_test_timeout_.load());
            int chunk = extractInt("chunk_size", threads);
            if (threads < 1 || threads > 50) {
                ok = false;
                message = "invalid_threads";
            } else if (timeout < 1 || timeout > 10) {
                ok = false;
                message = "invalid_timeout";
            } else {
                if (chunk < 1) chunk = threads;
                chunk = std::max(1, std::min(50, chunk));
                speed_max_threads_ = threads;
                speed_test_timeout_ = timeout;
                speed_chunk_size_ = chunk;
                std::lock_guard<std::mutex> lock(speed_mutex_);
                speed_profile_name_ = "custom";
                message = "speed_updated";
            }
        } else if (command == "set_threads") {
            int val = extractInt("value", speed_max_threads_.load());
            if (val >= 1 && val <= 50) {
                speed_max_threads_ = val;
                speed_chunk_size_ = std::max(1, std::min(50, val));
                std::lock_guard<std::mutex> lock(speed_mutex_);
                speed_profile_name_ = "custom";
                message = "threads_updated";
            } else {
                ok = false;
                message = "invalid_threads";
            }
        } else if (command == "set_timeout") {
            int val = extractInt("value", speed_test_timeout_.load());
            if (val >= 1 && val <= 10) {
                speed_test_timeout_ = val;
                std::lock_guard<std::mutex> lock(speed_mutex_);
                speed_profile_name_ = "custom";
                message = "timeout_updated";
            } else {
                ok = false;
                message = "invalid_timeout";
            }
        } else if (command == "clear_old") {
            int hours = extractInt("hours", 168);
            if (hours < 1) hours = 1;
            int removed = clearOldConfigs(hours);
            message = "cleared_" + std::to_string(removed);
        } else if (command == "clear_alive") {
            int removed = clearAliveConfigs();
            message = "alive_cleared_" + std::to_string(removed);
        } else if (command == "remove_configs") {
            const std::vector<std::string> uris_list = splitTextLines(extractString("uris_text"));
            const std::set<std::string> uris(uris_list.begin(), uris_list.end());
            int removed = removeConfigs(uris);
            message = "removed_" + std::to_string(removed);
        } else if (command == "add_configs") {
            const std::string configs = extractString("configs");
            if (!configs.empty()) {
                addManualConfigs(utils::split(configs, '\n'));
                message = "configs_added";
            } else {
                ok = false;
                message = "invalid_configs_payload";
            }
        } else if (command == "import_config_file") {
            const std::string import_path = utils::trim(extractString("path"));
            if (import_path.empty()) {
                ok = false;
                message = "invalid_import_path";
            } else if (!config_db_) {
                ok = false;
                message = "config_db_unavailable";
            } else {
                std::set<std::string> valid_configs;
                int invalid_count = 0;
                if (!loadImportCandidates(import_path, valid_configs, invalid_count)) {
                    ok = false;
                    message = "import_file_not_found";
                } else if (valid_configs.empty()) {
                    ok = false;
                    message = invalid_count > 0 ? "no_valid_configs_in_file" : "empty_import_file";
                } else {
                    // Process in batches of 5000 to prevent timeout on large imports
                    constexpr int IMPORT_BATCH = 5000;
                    int total_added = 0;
                    int total_promoted = 0;
                    
                    if ((int)valid_configs.size() <= IMPORT_BATCH) {
                        int promoted = 0;
                        total_added = config_db_->addConfigsWithPriority(valid_configs, "user_import", &promoted);
                        total_promoted = promoted;
                    } else {
                        // Split into batches
                        std::set<std::string> batch;
                        int batch_num = 0;
                        for (auto it = valid_configs.begin(); it != valid_configs.end(); ++it) {
                            batch.insert(*it);
                            if ((int)batch.size() >= IMPORT_BATCH || std::next(it) == valid_configs.end()) {
                                int promoted = 0;
                                int added = config_db_->addConfigsWithPriority(batch, "user_import", &promoted);
                                total_added += added;
                                total_promoted += promoted;
                                batch_num++;
                                utils::LogRingBuffer::instance().push(
                                    "[Import] Batch " + std::to_string(batch_num) + ": +" +
                                    std::to_string(added) + " new, " + std::to_string(promoted) + " promoted");
                                batch.clear();
                            }
                        }
                    }
                    
                    utils::LogRingBuffer::instance().push(
                        "[Import] File " + import_path + ": added " + std::to_string(total_added) +
                        ", promoted " + std::to_string(total_promoted) +
                        ", invalid " + std::to_string(invalid_count) +
                        " (total parsed: " + std::to_string(valid_configs.size()) + ")");
                    message = "imported " + std::to_string(total_added) + " new, reprioritized " +
                              std::to_string(total_promoted) + " existing, invalid " + std::to_string(invalid_count);
                }
            }
        } else if (command == "export_config_db") {
            if (!config_db_) {
                ok = false;
                message = "config_db_unavailable";
            } else {
                std::string export_path = utils::trim(extractString("path"));
                std::string base = utils::dirName(config_.stateFile());
                if (base.empty()) base = "runtime";
                if (export_path.empty()) export_path = base + "/HUNTER_config_db_export.txt";
                const std::set<std::string> all_uris = config_db_->getAllUris();
                const std::vector<std::string> lines(all_uris.begin(), all_uris.end());
                if (utils::writeLines(export_path, lines)) {
                    utils::LogRingBuffer::instance().push(
                        "[Export] Wrote " + std::to_string(lines.size()) + " configs to " + export_path);
                    message = "exported " + std::to_string(lines.size()) + " configs to " + export_path;
                } else {
                    ok = false;
                    message = "export_failed";
                }
            }
        } else if (command == "run_cycle") {
            message = runCycle() ? "cycle_completed" : "cycle_skipped";
        } else if (command == "refresh_ports") {
            refreshProvisionedPorts();
            message = "ports_refreshed";
        } else if (command == "recheck_live_ports") {
            data_json = recheckLiveProvisionedPorts();
            message = "live_recheck_completed";
        } else if (command == "reprovision_ports") {
            stopProvisionedPorts();
            provisionPorts();
            message = "ports_reprovisioned";
        } else if (command == "load_raw_files") {
            message = "raw_loaded_" + std::to_string(loadRawConfigFiles());
        } else if (command == "load_bundle_files") {
            message = "bundle_loaded_" + std::to_string(loadBundleConfigs());
        } else if (command == "detect_censorship" || command == "probe_censorship") {
            if (dpi_evasion_) {
                auto pr = dpi_evasion_->probeWithDetails();
                auto& m = pr.metrics;
                std::ostringstream dd;
                dd << "{\"censored\":" << (m.network_type == "censored" ? "true" : "false")
                   << ",\"cdn_reachable\":" << (m.cdn_reachable ? "true" : "false")
                   << ",\"google_reachable\":" << (m.google_reachable ? "true" : "false")
                   << ",\"telegram_reachable\":" << (m.telegram_reachable ? "true" : "false")
                   << ",\"pressure\":\"" << jsonEscape(m.pressure_level) << "\""
                   << ",\"strategy\":\"" << jsonEscape(m.strategy) << "\""
                   << ",\"network_type\":\"" << jsonEscape(m.network_type) << "\""
                   << ",\"recommended_strategy\":\"" << jsonEscape(pr.recommended_strategy) << "\""
                   << ",\"total_duration_ms\":" << pr.total_duration_ms
                   << ",\"probe_steps\":[";
                for (size_t i = 0; i < pr.steps.size(); i++) {
                    auto& s = pr.steps[i];
                    if (i > 0) dd << ",";
                    dd << "{\"target\":\"" << jsonEscape(s.target) << "\""
                       << ",\"category\":\"" << jsonEscape(s.category) << "\""
                       << ",\"method\":\"" << jsonEscape(s.method) << "\""
                       << ",\"success\":" << (s.success ? "true" : "false")
                       << ",\"latency_ms\":" << s.latency_ms
                       << ",\"detail\":\"" << jsonEscape(s.detail) << "\"}";
                }
                dd << "]}";
                data_json = dd.str();
                message = m.network_type == "censored" ? "censored" : "open";
            } else {
                bool c = detectCensorship();
                message = c ? "censored" : "open";
                data_json = "{\"censored\":" + std::string(c ? "true" : "false") + ",\"probe_steps\":[]}";
            }
        } else if (command == "discover_exit_ip") {
            if (dpi_evasion_) {
                auto emit_discovery_event = [&](const std::string& line) {
                    utils::JsonBuilder progress_json;
                    progress_json.add("command", "discover_exit_ip")
                                 .add("line", line)
                                 .add("ts", utils::nowTimestamp());
                    realtime::broadcastGlobalMonitorEvent("discovery_log", progress_json.build());
                };
                emit_discovery_event("[DISCOVERY] discover_exit_ip command accepted");
                auto dr = dpi_evasion_->discoverExitIp(emit_discovery_event);
                std::ostringstream dd;
                dd << "{\"default_gateway\":\"" << jsonEscape(dr.default_gateway) << "\""
                   << ",\"gateway_mac\":\"" << jsonEscape(dr.gateway_mac) << "\""
                   << ",\"local_ip\":\"" << jsonEscape(dr.local_ip) << "\""
                   << ",\"public_ip\":\"" << jsonEscape(dr.public_ip) << "\""
                   << ",\"isp_info\":\"" << jsonEscape(dr.isp_info) << "\""
                   << ",\"suggested_exit_ip\":\"" << jsonEscape(dr.suggested_exit_ip) << "\""
                   << ",\"suggested_iface\":\"" << jsonEscape(dr.suggested_iface) << "\""
                   << ",\"total_duration_ms\":" << dr.total_duration_ms
                   << ",\"trace_hops\":[";
                for (size_t i = 0; i < dr.trace_hops.size(); i++) {
                    auto& h = dr.trace_hops[i];
                    if (i > 0) dd << ",";
                    dd << "{\"ttl\":" << h.ttl
                       << ",\"ip\":\"" << jsonEscape(h.ip) << "\""
                       << ",\"latency_ms\":" << h.latency_ms
                       << ",\"is_domestic\":" << (h.is_domestic ? "true" : "false")
                       << "}";
                }
                dd << "]}";
                data_json = dd.str();
                message = dr.suggested_exit_ip;
                emit_discovery_event("[DISCOVERY] discover_exit_ip command finished");
            } else {
                ok = false;
                message = "dpi_evasion_not_initialized";
            }
        } else if (command == "update_runtime_settings") {
            bool any_change = false;
            bool engine_paths_changed = false;
            bool restart_required = false;

            if (hasKey("xray_path")) {
                const std::string value = utils::trim(extractString("xray_path"));
                if (value.empty()) {
                    ok = false;
                    message = "invalid_xray_path";
                } else {
                    engine_paths_changed = engine_paths_changed || value != config_.xrayPath();
                    config_.set("xray_path", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("singbox_path")) {
                const std::string value = utils::trim(extractString("singbox_path"));
                if (value.empty()) {
                    ok = false;
                    message = "invalid_singbox_path";
                } else {
                    engine_paths_changed = engine_paths_changed || value != config_.singBoxPath();
                    config_.set("singbox_path", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("mihomo_path")) {
                const std::string value = utils::trim(extractString("mihomo_path"));
                if (value.empty()) {
                    ok = false;
                    message = "invalid_mihomo_path";
                } else {
                    engine_paths_changed = engine_paths_changed || value != config_.mihomoPath();
                    config_.set("mihomo_path", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("tor_path")) {
                const std::string value = utils::trim(extractString("tor_path"));
                if (value.empty()) {
                    ok = false;
                    message = "invalid_tor_path";
                } else {
                    config_.set("tor_path", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("max_total")) {
                const int value = extractInt("max_total", config_.maxTotal());
                if (value < 1 || value > 100000) {
                    ok = false;
                    message = "invalid_max_total";
                } else {
                    config_.set("max_total", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("max_workers")) {
                const int value = extractInt("max_workers", config_.maxWorkers());
                if (value < 1 || value > 256) {
                    ok = false;
                    message = "invalid_max_workers";
                } else {
                    config_.set("max_workers", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("scan_limit")) {
                const int value = extractInt("scan_limit", config_.scanLimit());
                if (value < 1 || value > 10000) {
                    ok = false;
                    message = "invalid_scan_limit";
                } else {
                    config_.set("scan_limit", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("sleep_seconds")) {
                const int value = extractInt("sleep_seconds", config_.sleepSeconds());
                if (value < 1 || value > 86400) {
                    ok = false;
                    message = "invalid_sleep_seconds";
                } else {
                    config_.set("sleep_seconds", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("multiproxy_port")) {
                const int value = extractInt("multiproxy_port", config_.multiproxyPort());
                if (value < 1 || value > 65535) {
                    ok = false;
                    message = "invalid_multiproxy_port";
                } else {
                    restart_required = restart_required || value != config_.multiproxyPort();
                    config_.set("multiproxy_port", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("gemini_port")) {
                const int value = extractInt("gemini_port", config_.geminiPort());
                if (value < 1 || value > 65535) {
                    ok = false;
                    message = "invalid_gemini_port";
                } else {
                    restart_required = restart_required || value != config_.geminiPort();
                    config_.set("gemini_port", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("telegram_enabled")) {
                config_.set("telegram_enabled", extractBool("telegram_enabled", config_.getBool("telegram_enabled", false)));
                any_change = true;
            }
            if (ok && hasKey("telegram_api_id")) {
                config_.set("telegram_api_id", extractString("telegram_api_id"));
                any_change = true;
            }
            if (ok && hasKey("telegram_api_hash")) {
                config_.set("telegram_api_hash", extractString("telegram_api_hash"));
                any_change = true;
            }
            if (ok && hasKey("telegram_phone")) {
                config_.set("telegram_phone", extractString("telegram_phone"));
                any_change = true;
            }
            if (ok && hasKey("telegram_limit")) {
                const int value = extractInt("telegram_limit", config_.telegramLimit());
                if (value < 0 || value > 10000) {
                    ok = false;
                    message = "invalid_telegram_limit";
                } else {
                    config_.set("telegram_limit", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("telegram_timeout_ms")) {
                const int value = extractInt("telegram_timeout_ms", config_.getInt("telegram_timeout_ms", 12000));
                if (value < 1000 || value > 120000) {
                    ok = false;
                    message = "invalid_telegram_timeout_ms";
                } else {
                    config_.set("telegram_timeout_ms", value);
                    any_change = true;
                }
            }
            if (ok && hasKey("targets_text")) {
                const std::vector<std::string> targets = splitTextLines(extractString("targets_text"));
                std::ostringstream raw;
                raw << "[";
                bool first = true;
                for (const auto& item : targets) {
                    if (!first) raw << ",";
                    first = false;
                    raw << "\"" << item << "\"";
                }
                raw << "]";
                config_.set("targets", raw.str());
                any_change = true;
            }
            if (ok && hasKey("github_urls_text")) {
                config_.setGithubUrls(splitTextLines(extractString("github_urls_text")));
                any_change = true;
            }
            if (ok && engine_paths_changed) {
                applyRuntimeEnginePaths();
                if (balancer_) {
                    const auto pool = balancer_->getAvailableConfigsList();
                    std::vector<std::pair<std::string, float>> configs;
                    configs.reserve(pool.size());
                    for (const auto& entry : pool) configs.emplace_back(entry.uri, entry.latency);
                    balancer_->updateAvailableConfigs(configs, true);
                }
                if (gemini_balancer_) {
                    const auto pool = gemini_balancer_->getAvailableConfigsList();
                    std::vector<std::pair<std::string, float>> configs;
                    configs.reserve(pool.size());
                    for (const auto& entry : pool) configs.emplace_back(entry.uri, entry.latency);
                    gemini_balancer_->updateAvailableConfigs(configs, true);
                }
                stopProvisionedPorts();
                provisionPorts();
            }
            if (ok) {
                message = restart_required
                    ? (any_change ? "runtime_settings_applied_restart_required_for_ports" : "restart_required_for_ports")
                    : (any_change ? "runtime_settings_applied" : "no_runtime_changes");
            }
        } else if (command == "edge_router_bypass") {
            std::string target_mac = extractString("target_mac");
            std::string exit_ip = extractString("exit_ip");
            std::string iface = extractString("interface");
            if (iface.empty()) iface = "eth0";

            if (target_mac.empty() || exit_ip.empty()) {
                ok = false;
                message = "target_mac and exit_ip required";
            } else {
                if (dpi_evasion_) {
                    bool success = dpi_evasion_->attemptEdgeRouterBypass(target_mac, exit_ip, iface);
                    message = success ? "edge_router_bypass_active" : "edge_router_bypass_failed";
                    utils::JsonBuilder dj;
                    dj.add("active", dpi_evasion_->isEdgeRouterBypassActive())
                      .add("status", dpi_evasion_->getEdgeRouterBypassStatus());
                    data_json = dj.build();
                } else {
                    ok = false;
                    message = "dpi_evasion_not_initialized";
                }
            }
        } else if (command == "stop") {
            stop_requested_ = true;
            message = "stop_requested";
        } else if (command == "get_status") {
            message = "status";
        } else if (command == "ping") {
            message = "pong";
        } else if (command == "download_configs") {
            std::cerr << "[DL_DEBUG] ENTER download_configs handler" << std::endl;
            
            // Extract sources array and proxy setting
            std::vector<std::string> sources;
            std::string proxy = extractString("proxy");
            
            std::cerr << "[DL_DEBUG] Proxy extracted: " << (proxy.empty() ? "EMPTY" : proxy) << std::endl;
            
            // Parse sources array from JSON
            size_t sources_start = json_line.find("\"sources\":[");
            std::cerr << "[DL_DEBUG] Looking for sources in JSON" << std::endl;
            
            if (sources_start != std::string::npos) {
                sources_start = json_line.find('[', sources_start) + 1;
                size_t sources_end = json_line.find(']', sources_start);
                if (sources_end != std::string::npos) {
                    std::string sources_str = json_line.substr(sources_start, sources_end - sources_start);
                    std::istringstream ss(sources_str);
                    std::string source;
                    while (std::getline(ss, source, ',')) {
                        source = utils::trim(source);
                        if (source.size() >= 2 && source.front() == '"' && source.back() == '"') {
                            source = source.substr(1, source.size() - 2);
                        }
                        if (!source.empty()) {
                            sources.push_back(source);
                        }
                    }
                }
            }
            
            std::cerr << "[DL_DEBUG] Parsed " << sources.size() << " sources" << std::endl;
            
            if (sources.empty()) {
                std::cerr << "[DL_DEBUG] ERROR: No sources!" << std::endl;
                ok = false;
                message = "no_sources_provided";
            } else if (isDownloadInProgress()) {
                ok = false;
                message = "download_already_running";
                std::cerr << "[DL_DEBUG] Download already running, rejecting new request" << std::endl;
            } else {
                std::cerr << "[DL_DEBUG] Processing " << sources.size() << " sources" << std::endl;
                
                // Start async download process
                message = "download_started";
                
                // Build response
                utils::JsonBuilder dj;
                dj.add("sources_count", (int)sources.size())
                  .add("proxy", proxy)
                  .add("status", "started")
                  .add("current_source", "")
                  .add("progress", 0.0)
                  .add("downloaded_count", 0)
                  .add("total_count", (int)sources.size());
                data_json = dj.build();
                
                std::cerr << "[DL_DEBUG] About to create thread..." << std::endl;
            
            try {
                // Create thread with explicit launch policy
                std::thread dl_thread([this, sources, proxy]() {
                    std::cerr << "[DL_THREAD] Thread started! sources=" << sources.size() << std::endl;
                    std::cout << "[DL_THREAD] Thread started! sources=" << sources.size() << std::endl;
                    
                    try {
                        bool finished = downloadConfigsAsync(sources, proxy);
                        (void)finished;
                        std::cerr << "[DL_THREAD] downloadConfigsAsync finished" << std::endl;
                        std::cout << "[DL_THREAD] downloadConfigsAsync finished" << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "[DL_THREAD] EXCEPTION: " << e.what() << std::endl;
                        std::cout << "[DL_THREAD] EXCEPTION: " << e.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[DL_THREAD] UNKNOWN EXCEPTION" << std::endl;
                        std::cout << "[DL_THREAD] UNKNOWN EXCEPTION" << std::endl;
                    }
                    
                    std::cerr << "[DL_THREAD] Thread ending" << std::endl;
                    std::cout << "[DL_THREAD] Thread ending" << std::endl;
                });
                
                std::cerr << "[DL_DEBUG] Thread created successfully" << std::endl;
                
                // Check if thread is joinable before detaching
                if (dl_thread.joinable()) {
                    std::cerr << "[DL_DEBUG] Thread is joinable, detaching..." << std::endl;
                    dl_thread.detach();
                    std::cerr << "[DL_DEBUG] Thread detached SUCCESS" << std::endl;
                    std::cout << "[DL_DEBUG] Download thread started successfully" << std::endl;
                } else {
                    std::cerr << "[DL_DEBUG] ERROR: Thread not joinable!" << std::endl;
                    ok = false;
                    message = "thread_not_joinable";
                }
                    
                } catch (const std::exception& e) {
                    std::cerr << "[DL_DEBUG] Thread creation FAILED: " << e.what() << std::endl;
                    ok = false;
                    message = "thread_creation_failed";
                }
            }
            
            std::cerr << "[DL_DEBUG] EXIT download_configs handler" << std::endl;
        } else {
            ok = false;
            message = "unknown_command";
        }
    } catch (const std::exception& e) {
        ok = false;
        message = e.what();
    } catch (...) {
        ok = false;
        message = "unknown_exception";
    }

    const double response_ts = utils::nowTimestamp();
    const double response_ts_ms = response_ts * 1000.0;
    utils::JsonBuilder jb;
    jb.add("type", "command_result")
      .add("ok", ok)
      .add("request_id", request_id)
      .add("command", command)
      .add("quiet", quiet)
      .add("message", message)
      .add("received_ts", received_ts)
      .add("received_ts_ms", received_ts_ms)
      .add("response_ts", response_ts)
      .add("response_ts_ms", response_ts_ms)
      .addRaw("status", buildStatusJson(paused_.load() ? "paused" : "running"));
    if (!data_json.empty()) {
        jb.addRaw("data", data_json);
    }
    return jb.build();
}

void HunterOrchestrator::processStdinCommand(const std::string& json_line) {
    try {
        const std::string response = processRealtimeCommand(json_line);
        if (!response.empty()) {
            std::cout << "##CMD##" << response << std::endl;
        }
    } catch (...) {}
}

void HunterOrchestrator::emitStatusJson(const std::string& phase) {
    // Reuse writeStatusFile logic but output to stdout with ##STATUS## prefix
    writeStatusFile(phase);
    // Also build a compact JSON and emit to stdout
    int db_total=0, db_alive=0, db_tested=0;
    if (config_db_) {
        auto s = config_db_->getStats();
        db_total = s.total; db_alive = s.alive; db_tested = s.tested_unique;
    }
    auto sp = getSpeedProfile();
    double start_time = 0.0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time = start_time_;
    }
    double uptime = utils::nowTimestamp() - start_time;
    const double ts = utils::nowTimestamp();
    const double ts_ms = ts * 1000.0;
    // Emit compact status line
    std::ostringstream ss;
    ss << "##STATUS##{\"ts\":" << ts
       << ",\"ts_ms\":" << ts_ms
       << ",\"phase\":\"" << phase
       << "\",\"paused\":" << (paused_.load() ? "true" : "false")
       << ",\"uptime_s\":" << (int)uptime
       << ",\"db_total\":" << db_total
       << ",\"db_alive\":" << db_alive
       << ",\"db_tested\":" << db_tested
       << ",\"speed_profile\":\"" << sp.profile_name << "\""
       << ",\"speed_threads\":" << sp.max_threads
       << ",\"speed_timeout\":" << sp.test_timeout_s
       << "}";
    std::cout << ss.str() << std::endl;
}

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

    // Memory status
    auto hw = HunterTaskManager::instance().getHardware();
    { std::ostringstream _ls; _ls << "Memory status: " << hw.ram_percent << "% used ("
              << hw.ram_used_gb << "GB / " << hw.ram_total_gb << "GB)";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    const bool degraded_mode = hw.ram_percent >= 90.0f;
    if (degraded_mode) {
        utils::LogRingBuffer::instance().push("[RunCycle] High RAM pressure, entering degraded cycle mode");
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
            for (const auto& slot : getProvisionedPorts()) {
                if (slot.alive && slot.socks_ready) proxy_ports.push_back(slot.port);
            }
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

    if (balancer_ && !all_configs.empty()) {
        balancer_->updateAvailableConfigs(all_configs, true);
        saveBalancerCache("HUNTER_balancer_cache.json", all_configs);
    }

    // Gemini balancer
    if (gemini_balancer_ && !all_configs.empty()) {
        int half = std::max(1, (int)all_configs.size() / 2);
        std::vector<std::pair<std::string, float>> gemini_configs(
            all_configs.begin() + half, all_configs.end());
        if (!gemini_configs.empty()) {
            gemini_balancer_->updateAvailableConfigs(gemini_configs, true);
            saveBalancerCache("HUNTER_gemini_balancer_cache.json", gemini_configs);
        }
    }

    // Refresh individual proxy ports 2901-2999
    provisionPorts();

    // Save to files
    saveToFiles(tiered.gold, tiered.silver);

    // Publish to Telegram
    if (bot_reporter_ && bot_reporter_->isConfigured() && !all_configs.empty()) {
        std::vector<std::string> uris;
        for (auto& r : tiered.gold) uris.push_back(r.uri);
        for (auto& r : tiered.silver) uris.push_back(r.uri);
        bot_reporter_->reportConfigFiles(uris);
    }

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
    if (gemini_balancer_) proxy_ports.push_back(config_.geminiPort());

    int max_total = config_.maxTotal();
    auto hw = HunterTaskManager::instance().getHardware();
    if (hw.ram_percent >= 95.0f) {
        max_total = std::min(max_total, 40);
    } else if (hw.ram_percent >= 90.0f) {
        max_total = std::min(max_total, 80);
    } else if (hw.ram_percent >= 80.0f) {
        max_total = std::min(max_total, 160);
    }
    int per_source_cap = std::max(100, max_total / 3);
    if (hw.ram_percent >= 90.0f) {
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

    // DPI-aware prioritization
    if (dpi_evasion_) {
        deduped = dpi_evasion_->prioritizeConfigsForStrategy(deduped);
    } else {
        deduped = network::prioritizeConfigs(deduped);
    }

    auto hw = HunterTaskManager::instance().getHardware();
    if (hw.ram_percent >= 90.0f && deduped.size() > 40) {
        deduped.resize(40);
        { std::ostringstream _ls; _ls << "[Bench-" << label << "] High RAM pressure: capped to " << deduped.size() << " configs";
          utils::LogRingBuffer::instance().push(_ls.str()); }
    }

    { std::ostringstream _ls; _ls << "[Bench-" << label << "] Testing " << deduped.size() << " configs";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    // Process in small chunks to avoid saturating the thread pool.
    // Use dynamic chunk size from speed controls (clamped by RAM pressure).
    int user_chunk = speed_chunk_size_.load();
    int max_threads = std::max(1, std::min(50, speed_max_threads_.load()));
    size_t chunk_limit = (size_t)std::max(1, std::min(user_chunk, max_threads));
    const size_t CHUNK_SIZE = (hw.ram_percent >= 95.0f) ? std::min<size_t>(chunk_limit, 2)
                            : (hw.ram_percent >= 90.0f) ? std::min<size_t>(chunk_limit, 4)
                            : (hw.ram_percent >= 80.0f) ? std::min<size_t>(chunk_limit, 8)
                            : std::max<size_t>(1, chunk_limit);
    const int timeout_s = speed_test_timeout_.load();
    auto& mgr = HunterTaskManager::instance();
    std::vector<BenchResult> results;

    for (size_t offset = 0; offset < deduped.size(); offset += CHUNK_SIZE) {
        // Check pause/stop between chunks
        if (stop_requested_.load() || paused_.load()) break;
        auto chunk_hw = HunterTaskManager::instance().getHardware();
        if (chunk_hw.ram_percent >= 96.0f) {
            utils::LogRingBuffer::instance().push("[Bench-" + label + "] Aborting remaining chunks due to critical RAM pressure");
            break;
        }

        size_t end = std::min(offset + CHUNK_SIZE, deduped.size());
        std::vector<std::future<BenchResult>> futures;

        for (size_t i = offset; i < end; i++) {
            std::string uri = deduped[i]; // copy for lambda capture
            futures.push_back(mgr.submitIO([uri, timeout_s]() {
                BenchResult br;
                br.uri = uri;
                try {
                    network::ProxyTester tester;
                    tester.setSingBoxPath("bin/sing-box.exe");
                    auto result = tester.testConfig(uri, "https://cachefly.cachefly.net/1mb.test", timeout_s);

                    br.success = result.success && !result.telegram_only && result.download_speed_kbps > 0.0f;
                    br.telegram_only = result.success && result.telegram_only;
                    if (br.success) {
                        br.latency_ms = result.download_speed_kbps > 0 ? 1000.0f / result.download_speed_kbps : 5000.0f;
                        br.tier = br.latency_ms <= 3000 ? "gold" : "silver";
                    } else if (br.telegram_only) {
                        br.latency_ms = 0;
                        br.tier = "telegram";
                    } else {
                        br.latency_ms = 0;
                        br.tier = "dead";
                    }
                    br.engine_used = result.engine_used;
                    br.error = result.error_message;
                } catch (const std::exception& ex) {
                    br.error = std::string("EXCEPTION: ") + ex.what();
                    br.tier = "dead";
                } catch (...) {
                    br.error = "UNKNOWN EXCEPTION in test task";
                    br.tier = "dead";
                }
                return br;
            }));
        }

        for (auto& fut : futures) {
            try {
                results.push_back(fut.get());
            } catch (const std::exception& e) {
                BenchResult r;
                r.error = e.what();
                r.tier = "dead";
                results.push_back(r);
            } catch (...) {
                BenchResult r;
                r.error = "future::get unknown exception";
                r.tier = "dead";
                results.push_back(r);
            }
        }

        if (offset + CHUNK_SIZE < deduped.size()) {
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

std::string HunterOrchestrator::balancerCachePath(const std::string& name) const {
    std::string base = utils::dirName(config_.stateFile());
    if (base.empty()) base = "runtime";
    return base + "/" + name;
}

void HunterOrchestrator::loadBalancerCache(const std::string& name, proxy::MultiProxyServer* bal) {
    if (!bal) return;
    std::string path = balancerCachePath(name);
    std::string json = utils::loadJsonFile(path);
    if (json.size() < 10 || json == "{}") return;

    // Parse configs array: [{"uri":"...","latency_ms":...}, ...]
    std::vector<std::pair<std::string, float>> configs;
    size_t pos = 0;
    while ((pos = json.find("\"uri\":\"", pos)) != std::string::npos) {
        pos += 7; // skip '"uri":"'
        size_t end = json.find('"', pos);
        if (end == std::string::npos) break;
        std::string uri = json.substr(pos, end - pos);
        pos = end + 1;

        float latency = 999.0f;
        size_t lat_pos = json.find("\"latency_ms\":", pos);
        if (lat_pos != std::string::npos && lat_pos < pos + 100) {
            lat_pos += 14; // skip '"latency_ms":'
            try { latency = std::stof(json.substr(lat_pos, 20)); } catch (...) {}
        }

        std::string engine_used;
        size_t eng_pos = json.find("\"engine_used\":\"", pos);
        if (eng_pos != std::string::npos && eng_pos < pos + 160) {
            eng_pos += 15;
            size_t eng_end = json.find('"', eng_pos);
            if (eng_end != std::string::npos) {
                engine_used = json.substr(eng_pos, eng_end - eng_pos);
            }
        }

        if (!uri.empty() && uri.find("://") != std::string::npos) {
            configs.emplace_back(uri, latency);
            if (!engine_used.empty()) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_engine_hints_[uri] = engine_used;
            }
        }
    }

    if (!configs.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cached_configs_[name] = configs;
        std::cout << "[Cache] Loaded " << configs.size() << " cached configs from " << name << std::endl;
    }
}

void HunterOrchestrator::saveBalancerCache(const std::string& name,
    const std::vector<std::pair<std::string, float>>& configs) {
    std::string path = balancerCachePath(name);
    utils::mkdirRecursive(utils::dirName(path));

    std::ostringstream ss;
    ss << "{\"saved_at\":" << utils::nowTimestamp() << ",\"configs\":[";
    bool first = true;
    int count = 0;
    for (auto& [uri, lat] : configs) {
        if (count >= 1000) break;
        if (!first) ss << ",";
        first = false;
        std::string engine_used;
        if (config_db_) engine_used = config_db_->getPreferredEngine(uri);
        if (engine_used.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto hint_it = cached_engine_hints_.find(uri);
            if (hint_it != cached_engine_hints_.end()) engine_used = hint_it->second;
        }
        ss << "{\"uri\":\"" << uri << "\",\"latency_ms\":" << lat;
        if (!engine_used.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cached_engine_hints_[uri] = engine_used;
            ss << ",\"engine_used\":\"" << engine_used << "\"";
        }
        ss << "}";
        count++;
    }
    ss << "]}";
    utils::saveJsonFile(path, ss.str());
}

int HunterOrchestrator::computeAdaptiveSleep() {
    int base = config_.sleepSeconds();
    if (consecutive_scrape_failures_.load() >= 3) return std::min(base * 2, 600);
    if (last_validated_count_.load() == 0) return std::max(120, base / 3);
    if (last_validated_count_.load() < 5) return std::max(150, base / 2);
    return base;
}

void HunterOrchestrator::provisionPorts() {
    // Get best configs from balancer or cached configs
    std::vector<std::pair<std::string, float>> best;
    auto resolve_engine_hint = [this](const std::string& uri) -> std::string {
        if (config_db_) {
            std::string engine = config_db_->getPreferredEngine(uri);
            if (!engine.empty()) return engine;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = cached_engine_hints_.find(uri);
        return it != cached_engine_hints_.end() ? it->second : "";
    };
    if (balancer_) {
        auto pool = balancer_->getAvailableConfigsList();
        for (auto& e : pool) {
            best.emplace_back(e.uri, e.latency);
            if (!e.engine_used.empty()) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_engine_hints_[e.uri] = e.engine_used;
            }
        }
    }
    if (best.empty() && config_db_) {
        best = config_db_->getHealthyConfigs(PROVISION_PORT_COUNT);
    }
    if (best.empty()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = cached_configs_.find("HUNTER_balancer_cache.json");
        if (it != cached_configs_.end()) best = it->second;
    }

    // Sort by latency (best first) — quality-based priority
    std::sort(best.begin(), best.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    if (config_db_) {
        const int telegram_slot_target = std::min(10, std::max(2, PROVISION_PORT_COUNT / 8));
        std::set<std::string> existing_uris;
        for (const auto& item : best) existing_uris.insert(item.first);
        const auto telegram_only_records = config_db_->getTelegramOnlyRecords(std::max(telegram_slot_target * 4, 20));
        int appended = 0;
        for (const auto& rec : telegram_only_records) {
            if (existing_uris.insert(rec.uri).second) {
                best.emplace_back(rec.uri, 9000.0f + static_cast<float>(appended));
                appended++;
                if (appended >= telegram_slot_target) break;
            }
        }
    }

    // Limit to available port range
    int max_slots = PROVISION_PORT_MAX - PROVISION_PORT_BASE + 1;
    int count = std::min((int)best.size(), std::min(PROVISION_PORT_COUNT, max_slots));

    std::lock_guard<std::mutex> lock(provision_mutex_);

    // Stop existing processes
    for (auto& slot : provisioned_ports_) {
        if (slot.pid > 0) {
            runtime_engine_manager_.stopProcess(slot.pid);
        }
    }
    provisioned_ports_.clear();

    // Start individual engine-specific processes on ports 2901+
    double now = utils::nowTimestamp();
    for (int i = 0; i < count; i++) {
        int socks_port = PROVISION_PORT_BASE + i;
        int http_port = socks_port;
        auto& [uri, latency] = best[i];

        auto parsed = network::UriParser::parse(uri);
        if (!parsed.has_value() || !parsed->isValid()) continue;

        const std::string preferred_engine = resolve_engine_hint(uri);
        const std::string engine_used = runtime_engine_manager_.resolveEngine(*parsed, preferred_engine);
        if (engine_used.empty()) continue;
        std::string config_text = runtime_engine_manager_.generateConfig(*parsed, socks_port, engine_used);
        std::string config_path = runtime_engine_manager_.writeConfigFile(engine_used, config_text);
        int pid = runtime_engine_manager_.startProcess(engine_used, config_path);
        utils::LocalProxyProbeResult probe;
        bool ready = false;
        if (pid > 0 && utils::waitForPortAlive(socks_port, 5000, 100)) {
            probe = utils::probeLocalMixedPort(socks_port, 2000);
            ready = probe.mixed_ready();
        }
        if (pid > 0 && !ready) {
            runtime_engine_manager_.stopProcess(pid);
            pid = -1;
        }

        PortSlot slot;
        slot.port = socks_port;
        slot.http_port = http_port;
        slot.uri = uri;
        slot.engine_used = engine_used;
        slot.pid = pid;
        slot.alive = ready;
        slot.tcp_alive = probe.tcp_alive;
        slot.socks_ready = probe.socks_ready;
        slot.http_ready = probe.http_ready;
        slot.latency_ms = latency;
        slot.last_health_check = now;
        slot.last_probe_ts = utils::nowTimestamp();
        slot.consecutive_failures = 0;
        provisioned_ports_.push_back(slot);
        if (!engine_used.empty()) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cached_engine_hints_[uri] = engine_used;
        }

        if (ready) {
            std::cout << "[Provision] MIXED:" << socks_port << " <- "
                      << uri.substr(0, std::min((int)uri.size(), 50)) << "..."
                      << " (engine=" << engine_used << ", latency=" << latency << "ms)" << std::endl;
        } else {
            utils::LogRingBuffer::instance().push(
                "[Provision] Failed to start MIXED:" + std::to_string(socks_port) +
                " for " + uri.substr(0, std::min((int)uri.size(), 60)) +
                " engine=" + engine_used +
                " tcp=" + std::string(slot.tcp_alive ? "1" : "0") +
                " socks=" + std::string(slot.socks_ready ? "1" : "0") +
                " http=" + std::string(slot.http_ready ? "1" : "0"));
        }
    }

    if (count > 0) {
        std::cout << "[Provision] " << count << " mixed ports provisioned (HTTP+SOCKS on "
                  << PROVISION_PORT_BASE << "-" << (PROVISION_PORT_BASE + count - 1)
                  << ")" << std::endl;
    }
}

void HunterOrchestrator::refreshProvisionedPorts() {
    // Health-check each provisioned port; replace dead ones with next-best configs
    std::lock_guard<std::mutex> lock(provision_mutex_);
    const double now = utils::nowTimestamp();

    std::vector<int> dead_indices;
    for (int i = 0; i < (int)provisioned_ports_.size(); i++) {
        auto& slot = provisioned_ports_[i];
        if (!slot.alive && slot.pid <= 0) {
            dead_indices.push_back(i);
            continue;
        }
        // Check health every 30 seconds
        if (now - slot.last_health_check < 30.0) continue;
        slot.last_health_check = now;

        const auto probe = utils::probeLocalMixedPort(slot.port, 1500);
        slot.tcp_alive = probe.tcp_alive;
        slot.socks_ready = probe.socks_ready;
        slot.http_ready = probe.http_ready;
        slot.last_probe_ts = utils::nowTimestamp();
        if (probe.mixed_ready()) {
            slot.alive = true;
            slot.consecutive_failures = 0;
        } else {
            slot.alive = false;
            slot.consecutive_failures++;
            if (slot.consecutive_failures >= 3) {
                // Port is dead — kill and mark for replacement
                if (slot.pid > 0) {
                    runtime_engine_manager_.stopProcess(slot.pid);
                }
                if (config_db_ && !slot.uri.empty()) {
                    config_db_->updateHealth(slot.uri, false, 0.0f, slot.engine_used, true);
                }
                slot.alive = false;
                slot.pid = -1;
                dead_indices.push_back(i);
                utils::LogRingBuffer::instance().push(
                    "[Provision] Dead proxy on port " + std::to_string(slot.port) +
                    " (failures=" + std::to_string(slot.consecutive_failures) +
                    ", tcp=" + std::string(slot.tcp_alive ? "1" : "0") +
                    ", socks=" + std::string(slot.socks_ready ? "1" : "0") +
                    ", http=" + std::string(slot.http_ready ? "1" : "0") + ")");
            }
        }
    }

    if (dead_indices.empty()) return;
    replaceProvisionedPortsLocked(dead_indices, now);
}

std::string HunterOrchestrator::recheckLiveProvisionedPorts() {
    const bool was_paused = paused_.exchange(true);
    const int timeout_s = speed_test_timeout_.load();
    std::vector<ConfigHealthRecord> live_records;
    if (config_db_) {
        live_records = config_db_->getHealthyRecords(std::max(500, config_db_->size()));
    }

    std::vector<LiveRecheckItem> results;
    results.reserve(live_records.size());
    std::vector<std::string> passed_uris;
    std::vector<std::string> failed_uris;

    auto testWithEngine = [&](const std::string& uri, const std::string& preferred_engine) -> LiveRecheckItem {
        LiveRecheckItem item;
        item.uri = uri;

        auto parsed = network::UriParser::parse(uri);
        if (!parsed.has_value() || !parsed->isValid()) {
            item.error = "invalid_uri";
            return item;
        }

        std::vector<std::string> engines;
        auto add_engine = [&](const std::string& engine) {
            if (engine.empty()) return;
            if (!runtime_engine_manager_.isEngineAvailable(engine)) return;
            if (std::find(engines.begin(), engines.end(), engine) != engines.end()) return;
            if (runtime_engine_manager_.generateConfig(*parsed, 10808, engine).empty()) return;
            engines.push_back(engine);
        };

        add_engine(preferred_engine);
        add_engine("xray");
        add_engine("sing-box");
        add_engine("mihomo");

        if (engines.empty()) {
            item.error = "no_compatible_engine";
            return item;
        }

        for (const auto& engine : engines) {
            const int port = reserveTemporaryListenPort();
            if (port <= 0) {
                item.error = "no_free_port";
                break;
            }

            const std::string config_text = runtime_engine_manager_.generateConfig(*parsed, port, engine);
            if (config_text.empty()) {
                item.error = "config_generation_failed";
                continue;
            }
            const std::string config_path = runtime_engine_manager_.writeConfigFile(engine, config_text);
            if (config_path.empty()) {
                item.error = "config_write_failed";
                continue;
            }

            int pid = runtime_engine_manager_.startProcess(engine, config_path);
            if (pid <= 0) {
                item.error = "process_start_failed";
                continue;
            }

            utils::LocalProxyProbeResult probe;
            bool ready = false;
            if (utils::waitForPortAlive(port, 5000, 100)) {
                probe = utils::probeLocalMixedPort(port, 2000);
                ready = probe.mixed_ready();
            }

            float score = -1.0f;
            if (ready) {
                score = testProvisionedPortDownload(port, timeout_s);
            }
            runtime_engine_manager_.stopProcess(pid);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));

            if (ready && score > 0.0f) {
                item.alive = true;
                item.engine_used = engine;
                item.score = score;
                item.error.clear();
                return item;
            }

            std::ostringstream err;
            err << "engine=" << engine
                << " tcp=" << (probe.tcp_alive ? "1" : "0")
                << " socks=" << (probe.socks_ready ? "1" : "0")
                << " http=" << (probe.http_ready ? "1" : "0");
            item.error = err.str();
        }

        return item;
    };

    utils::LogRingBuffer::instance().push(
        "[RecheckLive] Started deep live validation for " + std::to_string(live_records.size()) +
        " configs using temporary engine ports");

    for (const auto& rec : live_records) {
        LiveRecheckItem item = testWithEngine(rec.uri, rec.engine_used);
        if (item.alive) {
            const float latency_ms = std::max(1.0f, 1000.0f / std::max(item.score, 0.001f));
            if (config_db_) {
                config_db_->updateHealth(rec.uri, true, latency_ms, item.engine_used);
            }
            passed_uris.push_back(rec.uri);
        } else {
            if (config_db_) {
                config_db_->updateHealth(rec.uri, false, 0.0f, rec.engine_used, true);
            }
            failed_uris.push_back(rec.uri);
        }
        results.push_back(std::move(item));
    }

    if (config_db_) {
        try {
            config_db_->saveToDisk("runtime/HUNTER_config_db.tsv");
        } catch (...) {}
    }

    utils::LogRingBuffer::instance().push(
        "[RecheckLive] Completed: tested=" + std::to_string((int)results.size()) +
        " passed=" + std::to_string((int)passed_uris.size()) +
        " failed=" + std::to_string((int)failed_uris.size()) +
        " runtime_kept_paused=1");

    // Auto-download configs when trusted live proxies are found
    if (!passed_uris.empty() && !was_paused) {
        double now_ts = utils::nowTimestamp();
        // Use a simple timestamp check for now (can be enhanced later)
        static double last_download_ts = 0.0;
        
        // Check if more than 1 hour since last download
        if (last_download_ts <= 0.0 || (now_ts - last_download_ts) >= 3600.0) {
            last_download_ts = now_ts;  // Update timestamp
            
            utils::LogRingBuffer::instance().push(
                "[RecheckLive] Found " + std::to_string((int)passed_uris.size()) + 
                " trusted live proxies, triggering automatic config download");
            
            // Trigger automatic download with live proxies
            std::vector<std::string> github_urls = config_.githubUrls();
            if (!github_urls.empty()) {
                // Create download task with live proxy priority
                auto download_task = [this, github_urls]() {
                    try {
                        downloadConfigsAsync(github_urls, "");
                    } catch (const std::exception& e) {
                        utils::LogRingBuffer::instance().push(
                            "[RecheckLive] Auto-download failed: " + std::string(e.what()));
                    }
                };
                
                // Run download in background
                std::thread(download_task).detach();
            }
        } else {
            utils::LogRingBuffer::instance().push(
                "[RecheckLive] Found trusted live proxies but recent download exists (" +
                std::to_string((int)(now_ts - last_download_ts)) + " seconds ago)");
        }
    }

    std::ostringstream data;
    data << "{"
         << "\"tested\":" << (int)results.size()
         << ",\"passed\":" << (int)passed_uris.size()
         << ",\"failed\":" << (int)failed_uris.size()
         << ",\"was_paused_before\":" << (was_paused ? "true" : "false")
         << ",\"kept_paused\":true"
         << ",\"passed_uris\":" << jsonStringArray(passed_uris)
         << ",\"failed_uris\":" << jsonStringArray(failed_uris)
         << ",\"tested_uris\":" << jsonStringArray([&]() {
                std::vector<std::string> combined = passed_uris;
                combined.insert(combined.end(), failed_uris.begin(), failed_uris.end());
                return combined;
            }())
         << "}";
    return data.str();
}

void HunterOrchestrator::replaceProvisionedPortsLocked(const std::vector<int>& dead_indices, double now,
                                                       const std::set<std::string>& excluded_uris) {
    if (dead_indices.empty()) return;

    // Get replacement configs
    std::set<std::string> existing_uris;
    auto resolve_engine_hint = [this](const std::string& uri) -> std::string {
        if (config_db_) {
            std::string engine = config_db_->getPreferredEngine(uri);
            if (!engine.empty()) return engine;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = cached_engine_hints_.find(uri);
        return it != cached_engine_hints_.end() ? it->second : "";
    };
    existing_uris.insert(excluded_uris.begin(), excluded_uris.end());
    for (auto& slot : provisioned_ports_) {
        if (slot.alive && slot.pid > 0) existing_uris.insert(slot.uri);
    }

    std::vector<std::pair<std::string, float>> replacements;
    if (config_db_) {
        auto healthy = config_db_->getHealthyConfigs(PROVISION_PORT_COUNT);
        for (auto& [uri, lat] : healthy) {
            if (existing_uris.find(uri) == existing_uris.end()) {
                replacements.emplace_back(uri, lat);
            }
        }
        const auto telegram_only_records = config_db_->getTelegramOnlyRecords(40);
        int telegram_added = 0;
        for (const auto& rec : telegram_only_records) {
            if (existing_uris.find(rec.uri) != existing_uris.end()) continue;
            replacements.emplace_back(rec.uri, 9000.0f + static_cast<float>(telegram_added));
            existing_uris.insert(rec.uri);
            telegram_added++;
            if (telegram_added >= 10) break;
        }
    }
    if (replacements.empty() && balancer_) {
        auto pool = balancer_->getAvailableConfigsList();
        for (auto& e : pool) {
            if (!e.engine_used.empty()) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_engine_hints_[e.uri] = e.engine_used;
            }
            if (existing_uris.find(e.uri) == existing_uris.end()) {
                replacements.emplace_back(e.uri, e.latency);
            }
        }
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    for (int idx : dead_indices) {
        auto& slot = provisioned_ports_[idx];
        const int socks_port = slot.port;
        const int h_port = slot.http_port > 0 ? slot.http_port : socks_port;
        bool assigned = false;

        while (!replacements.empty()) {
            auto [uri, latency] = replacements.front();
            replacements.erase(replacements.begin());

            auto parsed = network::UriParser::parse(uri);
            if (!parsed.has_value() || !parsed->isValid()) continue;

            const std::string preferred_engine = resolve_engine_hint(uri);
            const std::string engine_used = runtime_engine_manager_.resolveEngine(*parsed, preferred_engine);
            if (engine_used.empty()) continue;

            std::string config_text = runtime_engine_manager_.generateConfig(*parsed, socks_port, engine_used);
            std::string config_path = runtime_engine_manager_.writeConfigFile(engine_used, config_text);
            int pid = runtime_engine_manager_.startProcess(engine_used, config_path);
            utils::LocalProxyProbeResult probe;
            bool ready = false;
            if (pid > 0 && utils::waitForPortAlive(socks_port, 5000, 100)) {
                probe = utils::probeLocalMixedPort(socks_port, 2000);
                ready = probe.mixed_ready();
            }
            if (pid > 0 && !ready) {
                runtime_engine_manager_.stopProcess(pid);
                pid = -1;
            }

            if (!ready) {
                if (config_db_) config_db_->updateHealth(uri, false, 0.0f, engine_used, true);
                utils::LogRingBuffer::instance().push(
                    "[Provision] Failed to replace MIXED:" + std::to_string(socks_port) +
                    " engine=" + engine_used +
                    " using new config tcp=" + std::string(probe.tcp_alive ? "1" : "0") +
                    " socks=" + std::string(probe.socks_ready ? "1" : "0") +
                    " http=" + std::string(probe.http_ready ? "1" : "0"));
                continue;
            }

            slot.uri = uri;
            slot.engine_used = engine_used;
            slot.http_port = h_port;
            slot.pid = pid;
            slot.alive = true;
            slot.tcp_alive = probe.tcp_alive;
            slot.socks_ready = probe.socks_ready;
            slot.http_ready = probe.http_ready;
            slot.latency_ms = latency;
            slot.last_health_check = now;
            slot.last_probe_ts = utils::nowTimestamp();
            slot.consecutive_failures = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cached_engine_hints_[uri] = engine_used;
            }

            utils::LogRingBuffer::instance().push(
                "[Provision] Replaced dead SOCKS:" + std::to_string(socks_port) +
                " HTTP:" + std::to_string(h_port) +
                " engine=" + engine_used +
                " with new config (latency=" + std::to_string((int)latency) + "ms)");
            assigned = true;
            break;
        }

        if (!assigned) {
            slot.uri.clear();
            slot.engine_used.clear();
            slot.http_port = h_port;
            slot.pid = -1;
            slot.alive = false;
            slot.tcp_alive = false;
            slot.socks_ready = false;
            slot.http_ready = false;
            slot.latency_ms = 0.0f;
            slot.last_health_check = now;
            slot.last_probe_ts = utils::nowTimestamp();
            slot.consecutive_failures = 3;
        }
    }
}

void HunterOrchestrator::stopProvisionedPorts() {
    std::lock_guard<std::mutex> lock(provision_mutex_);
    for (auto& slot : provisioned_ports_) {
        if (slot.pid > 0) {
            runtime_engine_manager_.stopProcess(slot.pid);
            slot.pid = -1;
            slot.alive = false;
            slot.tcp_alive = false;
            slot.socks_ready = false;
            slot.http_ready = false;
            slot.last_probe_ts = utils::nowTimestamp();
        }
    }
    provisioned_ports_.clear();
}

std::vector<HunterOrchestrator::PortSlot> HunterOrchestrator::getProvisionedPorts() const {
    std::lock_guard<std::mutex> lock(provision_mutex_);
    return provisioned_ports_;
}

int HunterOrchestrator::cachedConfigCount() const {
    int total = 0;
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& [_, v] : cached_configs_) total += (int)v.size();
    return total;
}

void HunterOrchestrator::printStartupBanner() {
    auto hw = HunterTaskManager::instance().getHardware();

    int cached_main = 0, cached_gemini = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = cached_configs_.find("HUNTER_balancer_cache.json");
        if (it != cached_configs_.end()) cached_main = (int)it->second.size();
        it = cached_configs_.find("HUNTER_gemini_balancer_cache.json");
        if (it != cached_configs_.end()) cached_gemini = (int)it->second.size();
    }

    cache::SmartCache::CacheStats cs;
    if (cache_) cs = cache_->getStats();

    int db_size = config_db_ ? config_db_->size() : 0;

    std::string dpi_status = "off";
    std::string dpi_strategy = "-";
    std::string net_type = "unknown";
    bool cdn_ok = false, google_ok = false, tg_ok = false;
    if (dpi_evasion_) {
        auto m = dpi_evasion_->getMetrics();
        dpi_status = "active";
        dpi_strategy = m.strategy;
        net_type = m.network_type;
        cdn_ok = m.cdn_reachable;
        google_ok = m.google_reachable;
        tg_ok = m.telegram_reachable;
    }

    bool obf_active = (obfuscation_ != nullptr);

    std::cout << "\n"
    "╔══════════════════════════════════════════════════════════════╗\n"
    "║               HUNTER Anti-Censorship Engine                 ║\n"
    "╠══════════════════════════════════════════════════════════════╣\n"
    "║  System    : " << hw.cpu_count << " CPUs, "
        << std::fixed << std::setprecision(1) << hw.ram_total_gb << " GB RAM ("
        << (int)hw.ram_percent << "% used)\n"
    "║  Balancers : Main=" << config_.multiproxyPort()
        << "  Gemini=" << config_.geminiPort() << "\n"
    "║  Proxies   : " << PROVISION_PORT_BASE << "-"
        << PROVISION_PORT_MAX
        << " (" << (int)provisioned_ports_.size() << " active)\n"
    "║  Cached    : main=" << cached_main << "  gemini=" << cached_gemini
        << "  working=" << cs.working_count << "  all=" << cs.all_count << "\n"
    "║  ConfigDB  : " << db_size << " entries\n"
    "╠══════════════════════════════════════════════════════════════╣\n"
    "║  DPI Evasion      : " << dpi_status << "  strategy=" << dpi_strategy << "\n"
    "║  Network Type     : " << net_type << "\n"
    "║  Reachability     : CDN=" << (cdn_ok?"OK":"NO")
        << "  Google=" << (google_ok?"OK":"NO")
        << "  Telegram=" << (tg_ok?"OK":"NO") << "\n"
    "║  Obfuscation      : " << (obf_active?"active":"off") << "\n"
    "║  TLS Fingerprint  : randomized\n"
    "║  Fragmentation    : auto\n"
    "╠══════════════════════════════════════════════════════════════╣\n"
    "║  Phase 1: Cached configs loaded -> immediate connectivity   ║\n"
    "║  Phase 2: DPI evasion engines started                       ║\n"
    "║  Phase 3: Background fetch (GitHub/Telegram) starting...    ║\n"
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

    // Balancer status
    BalancerStatus bal_status;
    int bal_backends = 0, bal_healthy = 0;
    bool bal_running = false;
    if (balancer_) {
        bal_status = balancer_->getStatus();
        bal_backends = bal_status.backend_count;
        bal_healthy = bal_status.healthy_count;
        bal_running = bal_status.running;
    }
    BalancerStatus gem_status;
    int gem_backends = 0, gem_healthy = 0;
    bool gem_running = false;
    if (gemini_balancer_) {
        gem_status = gemini_balancer_->getStatus();
        gem_backends = gem_status.backend_count;
        gem_healthy = gem_status.healthy_count;
        gem_running = gem_status.running;
    }

    // DPI status
    std::string dpi_str = "OFF";
    std::string dpi_color = ansi::DIM;
    if (dpi_evasion_) {
        auto m = dpi_evasion_->getMetrics();
        dpi_str = m.strategy;
        if (!m.cdn_reachable && !m.google_reachable) {
            dpi_str += " BLOCKED";
            dpi_color = ansi::RED;
        } else {
            dpi_color = ansi::GREEN;
        }
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

    // Provisioned ports
    int prov_alive = 0, prov_total = 0;
    {
        std::lock_guard<std::mutex> plock(provision_mutex_);
        prov_total = (int)provisioned_ports_.size();
        for (auto& s : provisioned_ports_) if (s.alive) prov_alive++;
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

    // ── Balancers ──
    out << "  " << ansi::BOLD << ansi::YELLOW << "\xe2\x96\xb6" << ansi::WHITE << " LOAD BALANCERS" << ansi::RESET << "\n";

    // Main balancer
    const bool bal_mixed_ready = bal_status.tcp_alive && bal_status.socks_ready && bal_status.http_ready;
    out << "    " << (bal_mixed_ready ? ansi::GREEN : ansi::RED) << (bal_mixed_ready ? "\xe2\x97\x89" : "\xe2\x97\x8b") << ansi::RESET
        << " Main  :" << config_.multiproxyPort() << "  "
        << ansi::bar(bal_healthy, bal_backends > 0 ? bal_backends : 1, 15)
        << " " << ansi::BOLD << bal_healthy << "/" << bal_backends << ansi::RESET
        << (bal_mixed_ready ? "" : (std::string(ansi::RED) + " T" + (bal_status.tcp_alive ? std::string("1") : std::string("0")) +
            " S" + (bal_status.socks_ready ? std::string("1") : std::string("0")) +
            " H" + (bal_status.http_ready ? std::string("1") : std::string("0")) + ansi::RESET)) << "\n";

    // Gemini balancer
    const bool gem_mixed_ready = gem_status.tcp_alive && gem_status.socks_ready && gem_status.http_ready;
    out << "    " << (gem_mixed_ready ? ansi::GREEN : ansi::RED) << (gem_mixed_ready ? "\xe2\x97\x89" : "\xe2\x97\x8b") << ansi::RESET
        << " Gemini:" << config_.geminiPort() << "  "
        << ansi::bar(gem_healthy, gem_backends > 0 ? gem_backends : 1, 15)
        << " " << ansi::BOLD << gem_healthy << "/" << gem_backends << ansi::RESET
        << (gem_mixed_ready ? "" : (std::string(ansi::RED) + " T" + (gem_status.tcp_alive ? std::string("1") : std::string("0")) +
            " S" + (gem_status.socks_ready ? std::string("1") : std::string("0")) +
            " H" + (gem_status.http_ready ? std::string("1") : std::string("0")) + ansi::RESET)) << "\n";

    // Provisioned proxies
    out << "    " << (prov_alive > 0 ? ansi::GREEN : ansi::DIM) << "\xe2\x97\x89" << ansi::RESET
        << " Proxies:" << PROVISION_PORT_BASE << "-" << (PROVISION_PORT_BASE + PROVISION_PORT_COUNT - 1)
        << "  " << ansi::bar(prov_alive, prov_total > 0 ? prov_total : 1, 15)
        << " " << ansi::BOLD << prov_alive << "/" << prov_total << ansi::RESET << "\n\n";

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
    out << "    DPI: " << dpi_color << dpi_str << ansi::RESET
        << "    Cycles: " << ansi::BOLD << cycles << ansi::RESET
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

void HunterOrchestrator::killPortOccupants() {
    std::cout << "[PortKill] Checking for processes occupying required ports..." << std::endl;
    
    // Only check balancer and provisioned ports (NOT ephemeral test ports)
    std::vector<int> required_ports = {
        config_.multiproxyPort(),      // Main balancer port
        config_.geminiPort(),          // Gemini balancer port
    };
    for (int i = 0; i < PROVISION_PORT_COUNT; i++) {
        required_ports.push_back(PROVISION_PORT_BASE + i);
    }
    
    bool killed_any = false;
    
#ifdef _WIN32
    // Collect PIDs from netstat for all required ports
    std::set<int> pids_to_kill;
    for (int port : required_ports) {
        if (!utils::isPortAlive(port, 200)) continue;  // Skip ports that are free
        
        std::string netstat_cmd = "netstat -ano -p TCP";
        std::string result = utils::execCommand(netstat_cmd);
        
        std::string port_str = ":" + std::to_string(port);
        std::istringstream ss(result);
        std::string line;
        while (std::getline(ss, line)) {
            // Only match LISTENING state on our exact port
            if (line.find("LISTENING") == std::string::npos) continue;
            if (line.find(port_str) == std::string::npos) continue;
            
            // Extract PID (last token)
            std::istringstream line_ss(line);
            std::string token;
            std::vector<std::string> tokens;
            while (line_ss >> token) tokens.push_back(token);
            
            if (tokens.size() >= 5) {
                try {
                    int pid = std::stoi(tokens.back());
                    if (pid > 4) pids_to_kill.insert(pid);  // Skip System (PID 0,4)
                } catch (...) {}
            }
        }
    }
    
    // Kill collected PIDs using Windows API (no shell needed)
    for (int pid : pids_to_kill) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProc) {
            DWORD err = GetLastError();
            std::cerr << "[PortKill] OpenProcess failed for PID " << pid 
                      << " (error=" << err << ")" << std::endl;
            continue;
        }
        char exe_name[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameA(hProc, 0, exe_name, &size);
        std::string name(exe_name);
        
        // Only kill xray processes, not system services
        if (name.find("xray") != std::string::npos || 
            name.find("sing-box") != std::string::npos ||
            name.find("mihomo") != std::string::npos) {
            std::cout << "[PortKill] Killing " << name << " (PID " << pid << ")" << std::endl;
            TerminateProcess(hProc, 0);
            killed_any = true;
        }
        CloseHandle(hProc);
    }
    
    if (killed_any) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#else
    // Linux/macOS: use lsof to find and kill processes
    for (int port : required_ports) {
        std::string lsof_cmd = "lsof -ti :" + std::to_string(port);
        std::string pids = utils::execCommand(lsof_cmd);
        
        if (!pids.empty()) {
            std::istringstream ss(pids);
            std::string pid_str;
            
            while (std::getline(ss, pid_str)) {
                try {
                    int pid = std::stoi(pid_str);
                    
                    // Get process name
                    std::string ps_cmd = "ps -p " + std::to_string(pid) + " -o comm=";
                    std::string process_name = utils::trim(utils::execCommand(ps_cmd));
                    
                    // Skip our own process and system processes
                    if (process_name != "hunter" && 
                        process_name != "systemd" &&
                        process_name != "kernel" &&
                        !process_name.empty()) {
                        
                        std::cout << "[PortKill] Port " << port << " occupied by " 
                                  << process_name << " (PID " << pid << "), terminating..." << std::endl;
                        
                        // Kill the process
                        std::string kill_cmd = "kill -9 " + std::to_string(pid);
                        utils::execCommand(kill_cmd);
                        killed_any = true;
                        
                        // Give it a moment to terminate
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                } catch (...) {
                    // Skip if PID parsing fails
                }
            }
        }
    }
#endif
    
    if (killed_any) {
        std::cout << "[PortKill] Some processes were terminated. Waiting for ports to be released..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // Verify ports are now free
        bool all_free = true;
        for (int port : required_ports) {
            if (utils::isPortAlive(port, 100)) {
                std::cout << "[PortKill] WARNING: Port " << port << " is still in use!" << std::endl;
                all_free = false;
            }
        }
        
        if (all_free) {
            std::cout << "[PortKill] All required ports are now free" << std::endl;
        }
    } else {
        std::cout << "[PortKill] No processes occupying required ports found" << std::endl;
    }
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
    
    bool censored = (failed >= test_hosts.size() - 1); // All or all but one failed
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

std::string HunterOrchestrator::buildStatusJson(const std::string& phase, int last_tested, int last_passed) {
    int db_total = 0, db_alive = 0, db_tested = 0, db_untested = 0;
    float db_avg_lat = 0.0f;
    int db_total_tests = 0, db_total_passes = 0;
    int db_stale = 0;
    auto workerStateName = [](WorkerState state) -> const char* {
        switch (state) {
            case WorkerState::IDLE: return "idle";
            case WorkerState::RUNNING: return "running";
            case WorkerState::SLEEPING: return "sleeping";
            case WorkerState::WORKER_ERROR: return "error";
            case WorkerState::STOPPED: return "stopped";
        }
        return "unknown";
    };
    auto resourceModeName = [](ResourceMode mode) -> const char* {
        switch (mode) {
            case ResourceMode::NORMAL: return "normal";
            case ResourceMode::MODERATE: return "moderate";
            case ResourceMode::SCALED: return "scaled";
            case ResourceMode::CONSERVATIVE: return "conservative";
            case ResourceMode::REDUCED: return "reduced";
            case ResourceMode::MINIMAL: return "minimal";
            case ResourceMode::ULTRA_MINIMAL: return "ultra_minimal";
        }
        return "unknown";
    };
    std::vector<ConfigHealthRecord> healthy_records;
    std::vector<ConfigHealthRecord> telegram_only_records;
    if (config_db_) {
        auto s = config_db_->getStats();
        db_total = s.total;
        db_alive = s.alive;
        db_tested = s.tested_unique;
        db_untested = s.untested_unique;
        db_stale = s.stale_unique;
        db_avg_lat = s.avg_latency_ms;
        db_total_tests = s.total_tested;
        db_total_passes = s.total_passed;
        healthy_records = config_db_->getHealthyRecords(200);
        telegram_only_records = config_db_->getTelegramOnlyRecords(200);
    }

    BalancerStatus bal_status;
    int bal_backends = 0, bal_healthy = 0;
    bool bal_running = false;
    if (balancer_) {
        bal_status = balancer_->getStatus();
        bal_backends = bal_status.backend_count;
        bal_healthy = bal_status.healthy_count;
        bal_running = bal_status.running;
    }
    BalancerStatus gem_status;
    int gem_backends = 0, gem_healthy = 0;
    bool gem_running = false;
    if (gemini_balancer_) {
        gem_status = gemini_balancer_->getStatus();
        gem_backends = gem_status.backend_count;
        gem_healthy = gem_status.healthy_count;
        gem_running = gem_status.running;
    }

    double start_time = 0.0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        start_time = start_time_;
    }
    double uptime = utils::nowTimestamp() - start_time;
    int pending_unique = db_untested + db_stale;
    const HardwareSnapshot hw = HunterTaskManager::instance().getHardware();
    const HunterTaskManager::Metrics task_metrics = HunterTaskManager::instance().getMetrics();
    const orchestrator::ThreadManager::Status tm_status = thread_manager_
        ? thread_manager_->getStatus()
        : orchestrator::ThreadManager::Status{};
    auto parseExtraInt = [](const std::map<std::string, std::string>& extra, const std::string& key, int fallback) -> int {
        auto it = extra.find(key);
        if (it == extra.end()) return fallback;
        try {
            return std::stoi(it->second);
        } catch (...) {
            return fallback;
        }
    };
    auto parseExtraDouble = [](const std::map<std::string, std::string>& extra, const std::string& key, double fallback) -> double {
        auto it = extra.find(key);
        if (it == extra.end()) return fallback;
        try {
            return std::stod(it->second);
        } catch (...) {
            return fallback;
        }
    };
    std::string history_json = "[]";
    int validator_last_tested = last_tested;
    int validator_last_passed = last_passed;
    int validator_interval_s = 0;
    int validator_active_test_processes = 0;
    int validator_max_test_processes = 0;
    double validator_rate_per_s = 0.0;
    double eta_seconds = 0.0;
    int effective_timeout_s = speed_test_timeout_.load();
    int effective_max_concurrent = speed_max_threads_.load();
    int effective_batch_size = std::max(1, speed_chunk_size_.load());
    int effective_chunk_size = speed_chunk_size_.load();
    auto validator_it = tm_status.workers.find("validator");
    if (validator_it != tm_status.workers.end()) {
        const auto& extra = validator_it->second.extra;
        validator_last_tested = parseExtraInt(extra, "last_tested", validator_last_tested);
        validator_last_passed = parseExtraInt(extra, "last_passed", validator_last_passed);
        validator_interval_s = parseExtraInt(extra, "interval_s", validator_interval_s);
        validator_active_test_processes = parseExtraInt(extra, "active_test_processes", validator_active_test_processes);
        validator_max_test_processes = parseExtraInt(extra, "max_test_processes", validator_max_test_processes);
        validator_rate_per_s = parseExtraDouble(extra, "rate_per_s", validator_rate_per_s);
        pending_unique = parseExtraInt(extra, "pending_unique", pending_unique);
        eta_seconds = parseExtraDouble(extra, "eta_s", eta_seconds);
        effective_timeout_s = parseExtraInt(extra, "effective_timeout_s", effective_timeout_s);
        effective_max_concurrent = parseExtraInt(extra, "effective_max_concurrent", effective_max_concurrent);
        effective_batch_size = parseExtraInt(extra, "effective_batch_size", effective_batch_size);
        effective_chunk_size = parseExtraInt(extra, "effective_chunk_size", effective_chunk_size);
        auto hist_it = extra.find("history_json");
        if (hist_it != extra.end() && !hist_it->second.empty()) {
            history_json = hist_it->second;
        }
    }

    std::ostringstream alive_json;
    alive_json << "[";
    bool alive_first = true;
    double latest_found_ts = 0.0;
    double latest_alive_ts = 0.0;
    double latest_alive_test_ts = 0.0;
    for (auto& rec : healthy_records) {
        if (!alive_first) alive_json << ",";
        alive_first = false;
        latest_found_ts = std::max(latest_found_ts, rec.first_seen);
        latest_alive_ts = std::max(latest_alive_ts, rec.last_alive_time);
        latest_alive_test_ts = std::max(latest_alive_test_ts, rec.last_tested);
        utils::JsonBuilder item;
        item.add("uri", rec.uri)
            .add("latency_ms", rec.latency_ms)
            .add("engine_used", rec.engine_used)
            .add("first_seen", rec.first_seen)
            .add("last_alive", rec.last_alive_time)
            .add("last_tested", rec.last_tested)
            .add("total_tests", rec.total_tests)
            .add("total_passes", rec.total_passes)
            .add("consecutive_fails", rec.consecutive_fails)
            .add("alive", rec.alive)
            .add("tag", rec.tag);
        alive_json << item.build();
    }
    alive_json << "]";

    std::ostringstream telegram_json;
    telegram_json << "[";
    bool telegram_first = true;
    for (auto& rec : telegram_only_records) {
        if (!telegram_first) telegram_json << ",";
        telegram_first = false;
        latest_found_ts = std::max(latest_found_ts, rec.first_seen);
        latest_alive_ts = std::max(latest_alive_ts, rec.last_alive_time);
        latest_alive_test_ts = std::max(latest_alive_test_ts, rec.last_tested);
        utils::JsonBuilder item;
        item.add("uri", rec.uri)
            .add("latency_ms", rec.latency_ms)
            .add("engine_used", rec.engine_used)
            .add("first_seen", rec.first_seen)
            .add("last_alive", rec.last_alive_time)
            .add("last_tested", rec.last_tested)
            .add("total_tests", rec.total_tests)
            .add("total_passes", rec.total_passes)
            .add("consecutive_fails", rec.consecutive_fails)
            .add("alive", rec.alive)
            .add("telegram_only", rec.telegram_only)
            .add("tag", rec.tag);
        telegram_json << item.build();
    }
    telegram_json << "]";

    utils::JsonBuilder dbj;
    dbj.add("total", db_total)
       .add("alive", db_alive)
       .add("tested_unique", db_tested)
       .add("untested_unique", db_untested)
       .add("stale_unique", db_stale)
       .add("avg_latency_ms", db_avg_lat)
       .add("total_tests", db_total_tests)
       .add("total_passes", db_total_passes);

    utils::JsonBuilder valj;
    valj.add("last_tested", validator_last_tested)
        .add("last_passed", validator_last_passed)
        .add("interval_s", validator_interval_s)
        .add("active_test_processes", validator_active_test_processes)
        .add("max_test_processes", validator_max_test_processes)
        .add("rate_per_s", validator_rate_per_s);

    // Provisioned ports array
    std::ostringstream ports_json;
    ports_json << "[";
    {
        std::lock_guard<std::mutex> plock(provision_mutex_);
        bool ports_first = true;
        for (auto& slot : provisioned_ports_) {
            if (!ports_first) ports_json << ",";
            ports_first = false;
            const int public_mixed_port = slot.port > 0 ? slot.port : slot.http_port;
            utils::JsonBuilder pj;
            pj.add("port", public_mixed_port)
              .add("http_port", public_mixed_port)
              .add("mixed", true)
              .add("uri", slot.uri)
              .add("engine_used", slot.engine_used)
              .add("alive", slot.alive)
              .add("tcp_alive", slot.tcp_alive)
              .add("socks_ready", slot.socks_ready)
              .add("http_ready", slot.http_ready)
              .add("latency_ms", slot.latency_ms)
              .add("last_probe_ts", slot.last_probe_ts)
              .add("consecutive_failures", slot.consecutive_failures);
            ports_json << pj.build();
        }
    }
    ports_json << "]";

    // Balancers array
    std::ostringstream bal_json;
    {
        utils::JsonBuilder bm;
        bm.add("port", config_.multiproxyPort())
          .add("type", "main")
          .add("running", bal_running)
          .add("backends", bal_backends)
          .add("healthy", bal_healthy)
          .add("tcp_alive", bal_status.tcp_alive)
          .add("socks_ready", bal_status.socks_ready)
          .add("http_ready", bal_status.http_ready)
          .add("last_probe_ts", bal_status.last_probe_ts);
        utils::JsonBuilder bg;
        bg.add("port", config_.geminiPort())
          .add("type", "gemini")
          .add("running", gem_running)
          .add("backends", gem_backends)
          .add("healthy", gem_healthy)
          .add("tcp_alive", gem_status.tcp_alive)
          .add("socks_ready", gem_status.socks_ready)
          .add("http_ready", gem_status.http_ready)
          .add("last_probe_ts", gem_status.last_probe_ts);
        bal_json << "[" << bm.build() << "," << bg.build() << "]";
    }

    // Speed controls JSON
    utils::JsonBuilder speedj;
    {
        std::lock_guard<std::mutex> slock(speed_mutex_);
        speedj.add("profile", speed_profile_name_);
    }
    speedj.add("max_threads", speed_max_threads_.load())
          .add("test_timeout_s", speed_test_timeout_.load())
          .add("chunk_size", speed_chunk_size_.load())
          .add("effective_max_concurrent", effective_max_concurrent)
          .add("effective_timeout_s", effective_timeout_s)
          .add("effective_batch_size", effective_batch_size)
          .add("effective_chunk_size", effective_chunk_size);

    std::ostringstream workers_json;
    workers_json << "[";
    bool workers_first = true;
    for (const auto& pair : tm_status.workers) {
        if (!workers_first) workers_json << ",";
        workers_first = false;
        const WorkerStatus& worker = pair.second;
        utils::JsonBuilder wj;
        utils::JsonBuilder extraj;
        for (const auto& extra_pair : worker.extra) {
            extraj.add(extra_pair.first, extra_pair.second);
        }
        wj.add("name", pair.first)
          .add("state", workerStateName(worker.state))
          .add("last_run", worker.last_run)
          .add("last_error", worker.last_error)
          .add("runs", worker.runs)
          .add("errors", worker.errors)
          .add("next_run_in", worker.next_run_in)
          .addRaw("extra", extraj.build());
        workers_json << wj.build();
    }
    workers_json << "]";

    utils::JsonBuilder hardwarej;
    hardwarej.add("cpu_count", hw.cpu_count)
             .add("cpu_percent", hw.cpu_percent)
             .add("ram_total_gb", hw.ram_total_gb)
             .add("ram_used_gb", hw.ram_used_gb)
             .add("ram_percent", hw.ram_percent)
             .add("mode", resourceModeName(hw.mode))
             .add("io_pool_size", hw.io_pool_size)
             .add("cpu_pool_size", hw.cpu_pool_size)
             .add("io_pending", task_metrics.io_pending)
             .add("cpu_pending", task_metrics.cpu_pending)
             .add("io_active", task_metrics.io_active)
             .add("cpu_active", task_metrics.cpu_active)
             .add("max_configs", hw.max_configs)
             .add("scan_chunk", hw.scan_chunk)
             .add("thread_count", tm_status.hardware.thread_count);

    double last_download_success_ts = 0.0;
    double last_scan_cycle_ts = 0.0;
    std::string censorship_strategy = "none";
    std::string censorship_network = "unknown";
    std::string censorship_pressure = "normal";
    bool censorship_cdn_ok = false;
    bool censorship_google_ok = false;
    bool censorship_telegram_ok = false;
    auto github_worker_it = tm_status.workers.find("github_bg");
    if (github_worker_it != tm_status.workers.end()) {
        last_download_success_ts = parseExtraDouble(github_worker_it->second.extra, "last_success_ts", 0.0);
    }
    auto scanner_worker_it = tm_status.workers.find("config_scanner");
    if (scanner_worker_it != tm_status.workers.end()) {
        last_scan_cycle_ts = parseExtraDouble(scanner_worker_it->second.extra, "last_cycle_success_ts", 0.0);
    }
    auto validator_worker_it = tm_status.workers.find("validator");
    if (validator_worker_it != tm_status.workers.end()) {
        latest_found_ts = std::max(latest_found_ts, parseExtraDouble(validator_worker_it->second.extra, "latest_first_seen_ts", 0.0));
        latest_alive_test_ts = std::max(latest_alive_test_ts, parseExtraDouble(validator_worker_it->second.extra, "latest_alive_test_ts", 0.0));
    }
    if (dpi_evasion_) {
        auto dpi_metrics = dpi_evasion_->getMetrics();
        censorship_strategy = dpi_metrics.strategy;
        censorship_network = dpi_metrics.network_type;
        censorship_pressure = dpi_metrics.pressure_level;
        censorship_cdn_ok = dpi_metrics.cdn_reachable;
        censorship_google_ok = dpi_metrics.google_reachable;
        censorship_telegram_ok = dpi_metrics.telegram_reachable;
    }

    utils::JsonBuilder activityj;
    activityj.add("last_discovery_ts", latest_found_ts)
             .add("last_alive_confirmation_ts", latest_alive_ts)
             .add("last_healthy_test_ts", latest_alive_test_ts)
             .add("last_successful_download_ts", last_download_success_ts)
             .add("last_scan_cycle_ts", last_scan_cycle_ts);

    bool edge_bypass_active = false;
    std::string edge_bypass_status = "inactive";
    if (dpi_evasion_) {
        edge_bypass_active = dpi_evasion_->isEdgeRouterBypassActive();
        edge_bypass_status = dpi_evasion_->getEdgeRouterBypassStatus();
    }

    utils::JsonBuilder censorshipj;
    censorshipj.add("strategy", censorship_strategy)
               .add("network_type", censorship_network)
               .add("pressure_level", censorship_pressure)
               .add("cdn_reachable", censorship_cdn_ok)
               .add("google_reachable", censorship_google_ok)
               .add("telegram_reachable", censorship_telegram_ok)
               .add("edge_router_bypass_active", edge_bypass_active)
               .add("edge_router_bypass_status", edge_bypass_status);

    utils::JsonBuilder runtimej;
    runtimej.add("multiproxy_port", config_.multiproxyPort())
            .add("gemini_port", config_.geminiPort())
            .add("max_total", config_.maxTotal())
            .add("max_workers", config_.maxWorkers())
            .add("scan_limit", config_.scanLimit())
            .add("sleep_seconds", config_.sleepSeconds())
            .add("xray_path", config_.xrayPath())
            .add("singbox_path", config_.singBoxPath())
            .add("mihomo_path", config_.mihomoPath())
            .add("tor_path", config_.torPath())
            .add("telegram_enabled", config_.getBool("telegram_enabled", false))
            .add("telegram_limit", config_.telegramLimit())
            .add("telegram_timeout_ms", config_.getInt("telegram_timeout_ms", 12000))
            .add("targets_count", (int)config_.telegramTargets().size())
            .add("github_urls_count", (int)config_.githubUrls().size());

    const double ts = utils::nowTimestamp();
    const double ts_ms = ts * 1000.0;
    utils::JsonBuilder root;
    root.add("ts", ts)
        .add("ts_ms", ts_ms)
        .add("phase", phase)
        .add("paused", paused_.load())
        .add("uptime_s", uptime)
        .add("balancer_backends", bal_backends)
        .add("pending_unique", pending_unique)
        .add("eta_seconds", eta_seconds)
        .addRaw("db", dbj.build())
        .addRaw("validator", valj.build())
        .addRaw("speed", speedj.build())
        .addRaw("hardware", hardwarej.build())
        .addRaw("activity", activityj.build())
        .addRaw("censorship", censorshipj.build())
        .addRaw("runtime_config", runtimej.build())
        .addRaw("workers", workers_json.str())
        .addRaw("alive_configs", alive_json.str())
        .addRaw("telegram_only_configs", telegram_json.str())
        .addRaw("history", history_json)
        .addRaw("provisioned_ports", ports_json.str())
        .addRaw("balancers", bal_json.str());

    return root.build();
}

void HunterOrchestrator::writeStatusFile(const std::string& phase, int last_tested, int last_passed) {
    std::string base = utils::dirName(config_.stateFile());
    if (base.empty()) base = "runtime";
    utils::mkdirRecursive(base);
    std::string status_path = base + "/HUNTER_status.json";
    utils::saveJsonFile(status_path, buildStatusJson(phase, last_tested, last_passed));
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
    
    // 4. Trusted live proxies from provisioned ports
    std::vector<int> live_proxy_ports;
    for (const auto& slot : getProvisionedPorts()) {
        if (slot.alive && slot.socks_ready && slot.latency_ms > 0.0f) {  // Only use working live proxies
            live_proxy_ports.push_back(slot.port);
        }
    }
    
    // Sort live proxies by latency (fastest first)
    std::sort(live_proxy_ports.begin(), live_proxy_ports.end(), [this](int a, int b) {
        const auto& ports = getProvisionedPorts();
        const PortSlot* slot_a = nullptr;
        const PortSlot* slot_b = nullptr;
        
        for (const auto& slot : ports) {
            if (slot.port == a) slot_a = &slot;
            if (slot.port == b) slot_b = &slot;
        }
        
        if (slot_a && slot_b) {
            return slot_a->latency_ms < slot_b->latency_ms;  // Lower latency = faster
        }
        return false;
    });
    
    for (int port : live_proxy_ports) {
        std::string live_proxy = "127.0.0.1:" + std::to_string(port);
        // Avoid duplicates
        if (std::find(proxy_chain.begin(), proxy_chain.end(), live_proxy) == proxy_chain.end()) {
            proxy_chain.push_back(live_proxy);
            
            // Find the slot to get latency info
            float latency = 0.0f;
            for (const auto& slot : getProvisionedPorts()) {
                if (slot.port == port) {
                    latency = slot.latency_ms;
                    break;
                }
            }
            
            std::cout << "[Download] Added trusted live proxy to fallback chain: " << live_proxy 
                      << " (latency: " << latency << " ms)" << std::endl;
        }
    }
    
    // 5. Discovered healthy local proxies from balancers
    std::vector<int> healthy_ports = orchestrator::githubProxyPorts(this);
    for (int port : healthy_ports) {
        std::string local_proxy = "127.0.0.1:" + std::to_string(port);
        // Avoid duplicates
        if (std::find(proxy_chain.begin(), proxy_chain.end(), local_proxy) == proxy_chain.end()) {
            proxy_chain.push_back(local_proxy);
            std::cout << "[Download] Added healthy local proxy to fallback chain: " << local_proxy << std::endl;
        }
    }
    
    std::cout << "[Download] Proxy fallback chain has " << proxy_chain.size() << " options" << std::endl;
    
    // Test proxy connectivity and build working proxy list
    std::vector<std::string> working_proxies;
    std::string test_url = "https://httpbin.org/ip";
    
    std::cout << "[Download] Testing proxy connectivity with test URL: " << test_url << std::endl;
    
    for (const auto& test_proxy : proxy_chain) {
        std::string proxy_url;
        if (!test_proxy.empty()) {
            proxy_url = "socks5h://" + test_proxy;
        }
        
        std::cout << "[Download] Testing connectivity: " << (test_proxy.empty() ? "direct" : test_proxy) << std::endl;
        
        try {
            std::string test_content = http_client_.get(test_url, 5000, proxy_url);
            
            if (!test_content.empty()) {
                working_proxies.push_back(test_proxy);
                std::cout << "[Download] Connectivity test PASSED for: " << (test_proxy.empty() ? "direct" : test_proxy) << std::endl;
                std::cout << "[Download] Test response size: " << test_content.length() << " bytes" << std::endl;
            } else {
                std::cout << "[Download] Connectivity test FAILED for: " << (test_proxy.empty() ? "direct" : test_proxy) << " (empty response)" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "[Download] Connectivity test EXCEPTION for: " << (test_proxy.empty() ? "direct" : test_proxy) << " - " << e.what() << std::endl;
        }
    }
    
    if (working_proxies.empty()) {
        std::cout << "[Download] ERROR: No working connectivity options available!" << std::endl;
        std::cout << "[Download] This might be due to network restrictions or proxy issues" << std::endl;
        return false;
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
            return;
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
                if (promoted_existing > 0) {
                    emitLog("Refreshed " + std::to_string(promoted_existing) + " existing configs from " + source);
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
    return successful_sources > 0;
}

} // namespace hunter
