#include "orchestrator/orchestrator.h"
#include "orchestrator/thread_manager.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"
#include "network/proxy_tester.h"

#include <iostream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>

namespace hunter {

HunterOrchestrator::HunterOrchestrator(HunterConfig& config)
    : config_(config),
      config_fetcher_(http_client_),
      xray_manager_(),
      benchmarker_(xray_manager_) {
    initComponents();
    std::cout << "Hunter Orchestrator initialized successfully" << std::endl;
}

HunterOrchestrator::~HunterOrchestrator() {
    stop();
}

void HunterOrchestrator::initComponents() {
    // XRay path
    xray_manager_.setXRayPath(config_.xrayPath());

    // Config database
    config_db_ = std::make_unique<network::ConfigDatabase>(constants::CONFIG_DB_MAX_SIZE);

    // Continuous validator
    continuous_validator_ = std::make_unique<network::ContinuousValidator>(*config_db_);

    // Flexible fetcher
    flexible_fetcher_ = std::make_unique<network::FlexibleFetcher>(config_fetcher_);

    // Load balancer
    balancer_ = std::make_unique<proxy::MultiProxyServer>(config_.multiproxyPort());
    gemini_balancer_ = std::make_unique<proxy::MultiProxyServer>(config_.geminiPort());

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
    start_time_ = utils::nowTimestamp();

    std::string runtime_dir = utils::dirName(config_.stateFile());
    if (runtime_dir.empty()) runtime_dir = "runtime";
    const std::string stop_flag = runtime_dir + "/stop.flag";

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
    auto main_cached = cached_configs_.find("HUNTER_balancer_cache.json");
    auto gemini_cached = cached_configs_.find("HUNTER_gemini_balancer_cache.json");

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
        if (main_cached != cached_configs_.end() && !main_cached->second.empty()) {
            balancer_->start(main_cached->second);
            std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                      << " <- " << main_cached->second.size() << " cached configs" << std::endl;
        } else if (!fallback_configs.empty()) {
            balancer_->start(fallback_configs);
            std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                      << " <- " << fallback_configs.size() << " fallback configs" << std::endl;
        } else {
            balancer_->start();
            std::cout << "[Startup] Main balancer :" << config_.multiproxyPort()
                      << " (empty, waiting for first scan)" << std::endl;
        }
    }

    // Start gemini balancer with cached configs
    if (gemini_balancer_) {
        if (gemini_cached != cached_configs_.end() && !gemini_cached->second.empty()) {
            gemini_balancer_->start(gemini_cached->second);
            std::cout << "[Startup] Gemini balancer :" << config_.geminiPort()
                      << " <- " << gemini_cached->second.size() << " cached configs" << std::endl;
        } else if (!fallback_configs.empty()) {
            int half = std::max(1, (int)fallback_configs.size() / 2);
            std::vector<std::pair<std::string, float>> gemini_fb(
                fallback_configs.begin() + half, fallback_configs.end());
            if (!gemini_fb.empty()) gemini_balancer_->start(gemini_fb);
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
            auto healthy = config_db_ ? config_db_->getHealthyConfigs(5) : std::vector<std::pair<std::string,float>>{};
            if (!healthy.empty()) {
                std::cout << "[Emergency] Found " << healthy.size() << " working configs from validator" << std::endl;
                if (balancer_) {
                    balancer_->updateAvailableConfigs(healthy, true);
                    saveBalancerCache("HUNTER_balancer_cache.json", healthy);
                }
                break;
            }
            // Give validator time to test some configs
            std::this_thread::sleep_for(std::chrono::seconds(10));
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
    const std::string cmd_file = runtime_dir + "/hunter_command.json";
    int provision_tick = 0;
    int dashboard_tick = 0;
    while (!stop_requested_.load() && thread_manager_ && thread_manager_->isRunning()) {
        if (utils::fileExists(stop_flag)) {
            try { std::filesystem::remove(stop_flag); } catch (...) {}
            std::cout << "\n[Hunter] stop.flag detected, shutting down..." << std::endl;
            stop();
            break;
        }

        // Fallback: read command file if stdin isn't used (standalone CLI)
        if (utils::fileExists(cmd_file)) {
            try {
                std::string cmd_json = utils::loadJsonFile(cmd_file);
                std::filesystem::remove(cmd_file);
                processStdinCommand(cmd_json);
            } catch (...) {}
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
    if (thread_manager_) {
        thread_manager_->stopAll();
        thread_manager_.reset();
    }
    stopProvisionedPorts();
    if (dpi_evasion_) dpi_evasion_->stop();
    if (balancer_) balancer_->stop();
    if (gemini_balancer_) gemini_balancer_->stop();
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
    int added = config_db_->addConfigs(valid_set, "manual");
    utils::LogRingBuffer::instance().push(
        "[Cmd] Added " + std::to_string(added) + " manual configs (" +
        std::to_string(valid_set.size()) + " submitted)");
}

// ─── Real-time UI Communication ───

void HunterOrchestrator::processStdinCommand(const std::string& json_line) {
    try {
        std::string cmd = utils::trim(json_line);
        if (cmd.empty()) return;
        if (cmd.find("\"pause\"") != std::string::npos) {
            pause();
        } else if (cmd.find("\"resume\"") != std::string::npos) {
            resume();
        } else if (cmd.find("\"speed_profile\"") != std::string::npos) {
            auto pos = cmd.find("\"value\":");
            if (pos != std::string::npos) {
                auto vs = cmd.find('"', pos + 8);
                auto ve = cmd.find('"', vs + 1);
                if (vs != std::string::npos && ve != std::string::npos)
                    applyAutoProfile(cmd.substr(vs + 1, ve - vs - 1));
            }
        } else if (cmd.find("\"set_threads\"") != std::string::npos) {
            auto pos = cmd.find("\"value\":");
            if (pos != std::string::npos) {
                int val = std::atoi(cmd.c_str() + pos + 8);
                if (val >= 1 && val <= 50) {
                    speed_max_threads_ = val;
                    speed_chunk_size_ = std::max(1, std::min(50, val));
                    std::lock_guard<std::mutex> lock(speed_mutex_);
                    speed_profile_name_ = "custom";
                }
            }
        } else if (cmd.find("\"set_timeout\"") != std::string::npos) {
            auto pos = cmd.find("\"value\":");
            if (pos != std::string::npos) {
                int val = std::atoi(cmd.c_str() + pos + 8);
                if (val >= 1 && val <= 10) {
                    speed_test_timeout_ = val;
                    std::lock_guard<std::mutex> lock(speed_mutex_);
                    speed_profile_name_ = "custom";
                }
            }
        } else if (cmd.find("\"clear_old\"") != std::string::npos) {
            auto pos = cmd.find("\"hours\":");
            int hours = 168;
            if (pos != std::string::npos) {
                hours = std::atoi(cmd.c_str() + pos + 8);
                if (hours < 1) hours = 1;
            }
            clearOldConfigs(hours);
        } else if (cmd.find("\"add_configs\"") != std::string::npos) {
            auto pos = cmd.find("\"configs\":");
            if (pos != std::string::npos) {
                auto vs = cmd.find('"', pos + 10);
                auto ve = cmd.rfind('"');
                if (vs != std::string::npos && ve > vs) {
                    auto lines = utils::split(cmd.substr(vs+1, ve-vs-1), '\n');
                    addManualConfigs(lines);
                }
            }
        } else if (cmd.find("\"stop\"") != std::string::npos) {
            stop_requested_ = true;
        } else if (cmd.find("\"get_status\"") != std::string::npos) {
            emitStatusJson(paused_.load() ? "paused" : "running");
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
    double uptime = utils::nowTimestamp() - start_time_;
    // Emit compact status line
    std::ostringstream ss;
    ss << "##STATUS##{\"phase\":\"" << phase
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
        last_good_configs_ = std::vector<std::pair<std::string, float>>(
            all_configs.begin(), all_configs.begin() + std::min((int)all_configs.size(), 200));
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
    std::set<std::string> seen;
    std::vector<std::string> deduped;
    for (auto& uri : configs) {
        if (seen.insert(uri).second) deduped.push_back(uri);
    }

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
                    tester.setXrayPath("bin/xray.exe");
                    auto result = tester.testConfig(uri, "http://cp.cloudflare.com/generate_204", timeout_s);

                    br.success = result.success && !result.telegram_only;
                    br.latency_ms = br.success ? (result.download_speed_kbps > 0 ? 1000.0f / result.download_speed_kbps : 5000.0f) : 0;
                    br.tier = br.success ? (br.latency_ms <= 3000 ? "gold" : "silver") : "dead";
                    br.error = result.error_message;
                    if (result.telegram_only) br.error = "Telegram-only (no HTTP download)";
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

    // Sort by latency (successful first)
    std::sort(results.begin(), results.end(), [](const BenchResult& a, const BenchResult& b) {
        if (a.success != b.success) return a.success > b.success;
        return a.latency_ms < b.latency_ms;
    });

    // Update ConfigDB with results
    if (config_db_) {
        for (auto& r : results) {
            config_db_->updateHealth(r.uri, r.success, r.latency_ms);
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

        if (!uri.empty() && uri.find("://") != std::string::npos) {
            configs.emplace_back(uri, latency);
        }
    }

    if (!configs.empty()) {
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
        ss << "{\"uri\":\"" << uri << "\",\"latency_ms\":" << lat << "}";
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
    if (balancer_) {
        auto pool = balancer_->getAvailableConfigsList();
        for (auto& e : pool) best.emplace_back(e.uri, e.latency);
    }
    if (best.empty() && config_db_) {
        best = config_db_->getHealthyConfigs(PROVISION_PORT_COUNT);
    }
    if (best.empty()) {
        auto it = cached_configs_.find("HUNTER_balancer_cache.json");
        if (it != cached_configs_.end()) best = it->second;
    }

    // Sort by latency (best first) — quality-based priority
    std::sort(best.begin(), best.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    // Limit to available port range
    int max_slots = PROVISION_PORT_MAX - PROVISION_PORT_BASE + 1;
    int count = std::min((int)best.size(), std::min(PROVISION_PORT_COUNT, max_slots));

    std::lock_guard<std::mutex> lock(provision_mutex_);

    // Stop existing processes
    for (auto& slot : provisioned_ports_) {
        if (slot.pid > 0) {
            xray_manager_.stopProcess(slot.pid);
        }
    }
    provisioned_ports_.clear();

    // Start individual XRay processes on ports 2901+
    double now = utils::nowTimestamp();
    for (int i = 0; i < count; i++) {
        int port = PROVISION_PORT_BASE + i;
        auto& [uri, latency] = best[i];

        auto parsed = network::UriParser::parse(uri);
        if (!parsed.has_value() || !parsed->isValid()) continue;

        std::string config_json = proxy::XRayManager::generateConfig(*parsed, port);
        std::string config_path = xray_manager_.writeConfigFile(config_json);
        int pid = xray_manager_.startProcess(config_path);

        PortSlot slot;
        slot.port = port;
        slot.uri = uri;
        slot.pid = pid;
        slot.alive = (pid > 0);
        slot.latency_ms = latency;
        slot.last_health_check = now;
        slot.consecutive_failures = 0;
        provisioned_ports_.push_back(slot);

        if (pid > 0) {
            std::cout << "[Provision] Port " << port << " <- "
                      << uri.substr(0, std::min((int)uri.size(), 50)) << "..."
                      << " (latency=" << latency << "ms)" << std::endl;
        }
    }

    if (count > 0) {
        std::cout << "[Provision] " << count << " ports provisioned ("
                  << PROVISION_PORT_BASE << "-" << (PROVISION_PORT_BASE + count - 1)
                  << ")" << std::endl;
    }
}

void HunterOrchestrator::refreshProvisionedPorts() {
    // Health-check each provisioned port; replace dead ones with next-best configs
    std::lock_guard<std::mutex> lock(provision_mutex_);
    double now = utils::nowTimestamp();

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

        bool port_ok = utils::isPortAlive(slot.port, 2000);
        if (port_ok) {
            slot.alive = true;
            slot.consecutive_failures = 0;
        } else {
            slot.consecutive_failures++;
            if (slot.consecutive_failures >= 3) {
                // Port is dead — kill and mark for replacement
                if (slot.pid > 0) {
                    xray_manager_.stopProcess(slot.pid);
                }
                slot.alive = false;
                slot.pid = -1;
                dead_indices.push_back(i);
                utils::LogRingBuffer::instance().push(
                    "[Provision] Dead proxy on port " + std::to_string(slot.port) +
                    " (failures=" + std::to_string(slot.consecutive_failures) + ")");
            }
        }
    }

    if (dead_indices.empty()) return;

    // Get replacement configs
    std::set<std::string> existing_uris;
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
    }
    if (replacements.empty() && balancer_) {
        auto pool = balancer_->getAvailableConfigsList();
        for (auto& e : pool) {
            if (existing_uris.find(e.uri) == existing_uris.end()) {
                replacements.emplace_back(e.uri, e.latency);
            }
        }
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    int replaced = 0;
    for (int idx : dead_indices) {
        if (replaced >= (int)replacements.size()) break;
        auto& [uri, latency] = replacements[replaced];

        auto parsed = network::UriParser::parse(uri);
        if (!parsed.has_value() || !parsed->isValid()) {
            replaced++;
            continue;
        }

        int port = provisioned_ports_[idx].port;
        std::string config_json = proxy::XRayManager::generateConfig(*parsed, port);
        std::string config_path = xray_manager_.writeConfigFile(config_json);
        int pid = xray_manager_.startProcess(config_path);

        provisioned_ports_[idx].uri = uri;
        provisioned_ports_[idx].pid = pid;
        provisioned_ports_[idx].alive = (pid > 0);
        provisioned_ports_[idx].latency_ms = latency;
        provisioned_ports_[idx].last_health_check = now;
        provisioned_ports_[idx].consecutive_failures = 0;

        if (pid > 0) {
            utils::LogRingBuffer::instance().push(
                "[Provision] Replaced dead port " + std::to_string(port) +
                " with new config (latency=" + std::to_string((int)latency) + "ms)");
        }
        replaced++;
    }
}

void HunterOrchestrator::stopProvisionedPorts() {
    std::lock_guard<std::mutex> lock(provision_mutex_);
    for (auto& slot : provisioned_ports_) {
        if (slot.pid > 0) {
            xray_manager_.stopProcess(slot.pid);
            slot.pid = -1;
            slot.alive = false;
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
    for (auto& [_, v] : cached_configs_) total += (int)v.size();
    return total;
}

void HunterOrchestrator::printStartupBanner() {
    auto hw = HunterTaskManager::instance().getHardware();

    int cached_main = 0, cached_gemini = 0;
    {
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

    double uptime = utils::nowTimestamp() - start_time_;
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
    int bal_backends = 0, bal_healthy = 0;
    bool bal_running = false;
    if (balancer_) {
        auto bs = balancer_->getStatus();
        bal_backends = bs.backend_count;
        bal_healthy = bs.healthy_count;
        bal_running = bs.running;
    }
    int gem_backends = 0, gem_healthy = 0;
    bool gem_running = false;
    if (gemini_balancer_) {
        auto gs = gemini_balancer_->getStatus();
        gem_backends = gs.backend_count;
        gem_healthy = gs.healthy_count;
        gem_running = gs.running;
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
    out << "    " << (bal_running ? ansi::GREEN : ansi::RED) << (bal_running ? "\xe2\x97\x89" : "\xe2\x97\x8b") << ansi::RESET
        << " Main  :" << config_.multiproxyPort() << "  "
        << ansi::bar(bal_healthy, bal_backends > 0 ? bal_backends : 1, 15)
        << " " << ansi::BOLD << bal_healthy << "/" << bal_backends << ansi::RESET
        << (bal_running ? "" : (std::string(ansi::RED) + " DOWN" + ansi::RESET)) << "\n";

    // Gemini balancer
    out << "    " << (gem_running ? ansi::GREEN : ansi::RED) << (gem_running ? "\xe2\x97\x89" : "\xe2\x97\x8b") << ansi::RESET
        << " Gemini:" << config_.geminiPort() << "  "
        << ansi::bar(gem_healthy, gem_backends > 0 ? gem_backends : 1, 15)
        << " " << ansi::BOLD << gem_healthy << "/" << gem_backends << ansi::RESET
        << (gem_running ? "" : (std::string(ansi::RED) + " DOWN" + ansi::RESET)) << "\n";

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
        if (hProc) {
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

void HunterOrchestrator::writeStatusFile(const std::string& phase, int last_tested, int last_passed) {
    std::string base = utils::dirName(config_.stateFile());
    if (base.empty()) base = "runtime";
    utils::mkdirRecursive(base);
    std::string status_path = base + "/HUNTER_status.json";

    int db_total = 0, db_alive = 0, db_tested = 0, db_untested = 0;
    float db_avg_lat = 0.0f;
    int db_total_tests = 0, db_total_passes = 0;
    int db_stale = 0;
    std::vector<ConfigHealthRecord> healthy_records;
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
    }

    int bal_backends = 0, bal_healthy = 0;
    bool bal_running = false;
    if (balancer_) {
        auto bs = balancer_->getStatus();
        bal_backends = bs.backend_count;
        bal_healthy = bs.healthy_count;
        bal_running = bs.running;
    }
    int gem_backends = 0, gem_healthy = 0;
    bool gem_running = false;
    if (gemini_balancer_) {
        auto gs = gemini_balancer_->getStatus();
        gem_backends = gs.backend_count;
        gem_healthy = gs.healthy_count;
        gem_running = gs.running;
    }

    double uptime = utils::nowTimestamp() - start_time_;
    int pending_unique = db_untested + db_stale;

    std::ostringstream alive_json;
    alive_json << "[";
    bool alive_first = true;
    for (auto& rec : healthy_records) {
        if (!alive_first) alive_json << ",";
        alive_first = false;
        utils::JsonBuilder item;
        item.add("uri", rec.uri)
            .add("latency_ms", rec.latency_ms)
            .add("first_seen", rec.first_seen)
            .add("last_alive", rec.last_alive_time)
            .add("total_tests", rec.total_tests);
        alive_json << item.build();
    }
    alive_json << "]";

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
    valj.add("last_tested", last_tested)
        .add("last_passed", last_passed)
        .add("rate_per_s", 0.0);

    // Provisioned ports array
    std::ostringstream ports_json;
    ports_json << "[";
    {
        std::lock_guard<std::mutex> plock(provision_mutex_);
        bool ports_first = true;
        for (auto& slot : provisioned_ports_) {
            if (!ports_first) ports_json << ",";
            ports_first = false;
            utils::JsonBuilder pj;
            pj.add("port", slot.port)
              .add("uri", slot.uri)
              .add("alive", slot.alive)
              .add("latency_ms", slot.latency_ms)
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
          .add("healthy", bal_healthy);
        utils::JsonBuilder bg;
        bg.add("port", config_.geminiPort())
          .add("type", "gemini")
          .add("running", gem_running)
          .add("backends", gem_backends)
          .add("healthy", gem_healthy);
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
          .add("chunk_size", speed_chunk_size_.load());

    utils::JsonBuilder root;
    root.add("ts", utils::nowTimestamp())
        .add("phase", phase)
        .add("paused", paused_.load())
        .add("uptime_s", uptime)
        .add("balancer_backends", bal_backends)
        .add("pending_unique", pending_unique)
        .add("eta_seconds", 0.0)
        .addRaw("db", dbj.build())
        .addRaw("validator", valj.build())
        .addRaw("speed", speedj.build())
        .addRaw("alive_configs", alive_json.str())
        .addRaw("provisioned_ports", ports_json.str())
        .addRaw("balancers", bal_json.str());

    utils::saveJsonFile(status_path, root.build());
}

} // namespace hunter
