#include "network/continuous_validator.h"
#include "network/uri_parser.h"
#include "network/proxy_tester.h"
#include "core/utils.h"
#include "core/task_manager.h"

#include <algorithm>
#include <numeric>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hunter {
namespace network {

// ═══════════════════════════════════════════════════════════════════
// ConfigDatabase
// ═══════════════════════════════════════════════════════════════════

ConfigDatabase::ConfigDatabase(int max_size) : max_size_(max_size) {}

std::string ConfigDatabase::hashUri(const std::string& uri) const {
    return utils::sha1Hex(uri).substr(0, 16);
}

int ConfigDatabase::addConfigs(const std::set<std::string>& uris, const std::string& tag) {
    return addConfigsWithPriority(uris, tag, nullptr);
}

int ConfigDatabase::addConfigsWithPriority(const std::set<std::string>& uris, const std::string& tag,
                                          int* promoted_existing) {
    std::lock_guard<std::mutex> lock(mutex_);
    int added = 0;
    int promoted = 0;
    double now = utils::nowTimestamp();
    const bool high_priority = (tag == "manual" || tag == "import" || tag == "user_import");
    const double boost_until = high_priority ? (now + 1800.0) : 0.0;
    for (const auto& uri : uris) {
        if (uri.empty()) continue;
        std::string hash = hashUri(uri);
        auto existing = db_.find(hash);
        if (existing != db_.end()) {
            if (high_priority) {
                auto& rec = existing->second;
                rec.needs_retest = true;
                rec.priority_boost_until = std::max(rec.priority_boost_until, boost_until);
                rec.tag = tag;
                promoted++;
            }
            continue;
        }
        if ((int)db_.size() >= max_size_) evictStale();
        if ((int)db_.size() >= max_size_) break;

        ConfigHealthRecord rec;
        rec.uri = uri;
        rec.uri_hash = hash;
        rec.tag = tag;
        rec.first_seen = now;
        rec.priority_boost_until = boost_until;
        rec.needs_retest = true;
        db_[hash] = rec;
        added++;
    }
    // Enforce newest-first ordering: move newest to end of map iteration
    if (added > 0) {
        std::vector<std::pair<std::string, ConfigHealthRecord>> newest;
        for (auto& [hash, rec] : db_) {
            if (rec.first_seen == now) {
                newest.emplace_back(hash, rec);
            }
        }
        for (auto& [hash, rec] : newest) {
            db_.erase(hash);
            db_.emplace(hash, rec);
        }
    }
    if (promoted_existing) {
        *promoted_existing = promoted;
    }
    return added;
}

void ConfigDatabase::updateHealth(const std::string& uri, bool alive, float latency_ms,
                                  const std::string& engine_used, bool force_dead) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = hashUri(uri);
    auto it = db_.find(hash);
    if (it == db_.end()) return;

    auto& rec = it->second;
    rec.last_tested = utils::nowTimestamp();
    rec.total_tests++;
    rec.needs_retest = false;
    rec.priority_boost_until = 0.0;
    if (!engine_used.empty()) {
        rec.engine_used = engine_used;
    }

    if (alive) {
        rec.alive = true;
        rec.latency_ms = latency_ms;
        rec.consecutive_fails = 0;
        rec.total_passes++;
        rec.last_alive_time = rec.last_tested;
    } else {
        if (force_dead) rec.consecutive_fails = 3;
        else rec.consecutive_fails++;
        if (rec.consecutive_fails >= 3) {
            rec.alive = false;
            rec.latency_ms = 0.0f;
        }
    }
}

std::vector<ConfigHealthRecord> ConfigDatabase::getUntestedBatch(int batch_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> batch;
    double now = utils::nowTimestamp();

    // Collect candidates with priority score (lower = higher priority)
    struct Candidate {
        const ConfigHealthRecord* rec;
        bool boosted;
        int priority;   // 0=never tested, 1=needs_retest, 2=alive stale 30s, 3=failed stale 60s
        int total_tests; // for secondary sort: fewer tests = higher priority
    };
    std::vector<Candidate> candidates;

    for (auto& [hash, rec] : db_) {
        const bool boosted = rec.priority_boost_until > now;
        // Priority 0: never tested (brand new configs)
        if (rec.total_tests == 0) {
            candidates.push_back({&rec, boosted, 0, 0});
            continue;
        }
        // Priority 1: flagged for retest
        if (rec.needs_retest) {
            candidates.push_back({&rec, boosted, 1, rec.total_tests});
            continue;
        }
        // Priority 2: alive configs stale > 30 seconds — continuous health recheck
        if (rec.alive && (now - rec.last_tested) > 30.0) {
            candidates.push_back({&rec, boosted, 2, rec.total_tests});
            continue;
        }
        // Priority 3: failed configs — backoff based on consecutive failures
        if (!rec.alive && rec.consecutive_fails > 0) {
            double backoff_s;
            if (rec.consecutive_fails >= 10) {
                backoff_s = 1800.0; // 30 min for heavily-failing configs
            } else if (rec.consecutive_fails >= 5) {
                backoff_s = 300.0;  // 5 min for moderately-failing configs
            } else {
                backoff_s = 60.0;   // 1 min for recently-failing configs
            }
            if ((now - rec.last_tested) > backoff_s) {
                candidates.push_back({&rec, boosted, 3, rec.total_tests});
            }
            continue;
        }
    }

    // Sort: primary by priority (ascending), secondary by total_tests (ascending = less tested first)
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.boosted != b.boosted) return a.boosted > b.boosted;
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.total_tests < b.total_tests;
    });

    for (auto& c : candidates) {
        batch.push_back(*c.rec);
        if ((int)batch.size() >= batch_size) break;
    }

    return batch;
}

std::vector<std::pair<std::string, float>> ConfigDatabase::getHealthyConfigs(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<std::string, float>> healthy;
    for (auto& [hash, rec] : db_) {
        if (rec.alive && rec.latency_ms > 0) {
            healthy.emplace_back(rec.uri, rec.latency_ms);
        }
    }
    std::sort(healthy.begin(), healthy.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    if ((int)healthy.size() > max_count) healthy.resize(max_count);
    return healthy;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getHealthyRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> healthy;
    for (auto& [hash, rec] : db_) {
        if (rec.alive && rec.latency_ms > 0) {
            healthy.push_back(rec);
        }
    }
    std::sort(healthy.begin(), healthy.end(),
              [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) { return a.latency_ms < b.latency_ms; });
    if ((int)healthy.size() > max_count) healthy.resize(max_count);
    return healthy;
}

std::set<std::string> ConfigDatabase::getAllUris() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> uris;
    for (auto& [hash, rec] : db_) uris.insert(rec.uri);
    return uris;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAllRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> all;
    all.reserve(db_.size());
    for (auto& [hash, rec] : db_) {
        all.push_back(rec);
    }
    // Sort: alive first (by latency asc), then dead (by last_tested desc)
    std::sort(all.begin(), all.end(), [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
        if (a.alive != b.alive) return a.alive > b.alive; // alive first
        if (a.alive) return a.latency_ms < b.latency_ms;  // alive: lower latency first
        return a.last_tested > b.last_tested;              // dead: most recently tested first
    });
    if ((int)all.size() > max_count) all.resize(max_count);
    return all;
}

std::string ConfigDatabase::getPreferredEngine(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = hashUri(uri);
    auto it = db_.find(hash);
    if (it == db_.end()) return "";
    return it->second.engine_used;
}

ConfigDatabase::TagStats ConfigDatabase::getTagStats(const std::string& tag) {
    std::lock_guard<std::mutex> lock(mutex_);
    TagStats stats;
    stats.tag = tag;
    if (tag.empty()) return stats;

    std::vector<float> latencies;
    for (auto& [hash, rec] : db_) {
        if (rec.tag != tag) continue;
        stats.total++;
        if (rec.alive && rec.latency_ms > 0) {
            stats.alive++;
            latencies.push_back(rec.latency_ms);
        }
        if (rec.total_tests == 0) stats.untested++;
        if (rec.needs_retest) stats.needs_retest++;
    }

    if (!latencies.empty()) {
        float sum = std::accumulate(latencies.begin(), latencies.end(), 0.0f);
        stats.avg_latency_ms = sum / (float)latencies.size();
    }
    return stats;
}

ConfigDatabase::Stats ConfigDatabase::getStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s;
    s.total = (int)db_.size();
    std::vector<float> latencies;
    double now = utils::nowTimestamp();
    double stale_threshold = 300.0; // 5 min
    for (auto& [hash, rec] : db_) {
        if (rec.alive) {
            s.alive++;
            if (rec.latency_ms > 0) latencies.push_back(rec.latency_ms);
        }
        if (rec.total_tests > 0) {
            s.tested_unique++;
            if ((now - rec.last_tested) > stale_threshold) {
                s.stale_unique++;
            }
        } else {
            s.untested_unique++;
        }
        s.total_tested += rec.total_tests;
        s.total_passed += rec.total_passes;
    }
    if (!latencies.empty()) {
        s.avg_latency_ms = std::accumulate(latencies.begin(), latencies.end(), 0.0f) /
                           (float)latencies.size();
    }
    return s;
}

int ConfigDatabase::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int)db_.size();
}

void ConfigDatabase::evictStale() {
    if (db_.empty()) return;
    double now = utils::nowTimestamp();
    constexpr double THREE_HOURS = 3.0 * 3600.0;

    // Phase 1: Remove configs that have been continuously offline for 3+ hours
    std::vector<std::string> dead_hashes;
    for (auto& [hash, rec] : db_) {
        if (!rec.alive && rec.total_tests > 0 && rec.last_alive_time > 0.0 &&
            (now - rec.last_alive_time) > THREE_HOURS) {
            dead_hashes.push_back(hash);
        }
        // Also remove configs that were NEVER alive and tested 5+ times
        if (!rec.alive && rec.total_tests >= 5 && rec.last_alive_time == 0.0 &&
            rec.consecutive_fails >= 5) {
            dead_hashes.push_back(hash);
        }
    }
    for (auto& h : dead_hashes) db_.erase(h);

    // Phase 2: If still over capacity, remove oldest high-failure entries
    if ((int)db_.size() < max_size_) return;
    std::vector<std::pair<std::string, double>> candidates;
    for (auto& [hash, rec] : db_) {
        if (rec.consecutive_fails >= 5 || (!rec.alive && rec.total_tests > 3)) {
            candidates.emplace_back(hash, rec.first_seen);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    int to_remove = std::max(1, (int)candidates.size() / 4);
    for (int i = 0; i < to_remove && i < (int)candidates.size(); i++) {
        db_.erase(candidates[i].first);
    }
}

int ConfigDatabase::clearOlderThan(int max_age_hours) {
    std::lock_guard<std::mutex> lock(mutex_);
    double now = utils::nowTimestamp();
    double cutoff = (double)max_age_hours * 3600.0;
    int removed = 0;
    for (auto it = db_.begin(); it != db_.end(); ) {
        auto& rec = it->second;
        // Remove if: never alive and old enough, or last alive too long ago
        bool should_remove = false;
        if (rec.last_alive_time > 0.0 && (now - rec.last_alive_time) > cutoff) {
            should_remove = true;
        } else if (rec.last_alive_time == 0.0 && rec.first_seen > 0.0 && (now - rec.first_seen) > cutoff) {
            should_remove = true;
        }
        if (should_remove) {
            it = db_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

int ConfigDatabase::clearAlive() {
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;
    for (auto it = db_.begin(); it != db_.end(); ) {
        if (it->second.alive) {
            it = db_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

int ConfigDatabase::removeUris(const std::set<std::string>& uris) {
    if (uris.empty()) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;
    for (const auto& uri : uris) {
        const std::string hash = hashUri(uri);
        auto it = db_.find(hash);
        if (it != db_.end()) {
            db_.erase(it);
            removed++;
        }
    }
    return removed;
}

int ConfigDatabase::saveToDisk(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try { utils::mkdirRecursive(utils::dirName(filepath)); } catch (...) {}
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) return 0;
    // Header line
    ofs << "#HUNTER_CONFIG_DB_V1\n";
    int saved = 0;
    for (const auto& [hash, rec] : db_) {
        if (rec.uri.empty()) continue;
        // Tab-separated: uri \t tag \t engine_used \t first_seen \t last_tested \t last_alive_time
        //                \t alive \t latency_ms \t consecutive_fails \t total_tests \t total_passes
        ofs << rec.uri << '\t'
            << rec.tag << '\t'
            << rec.engine_used << '\t'
            << std::fixed << rec.first_seen << '\t'
            << rec.last_tested << '\t'
            << rec.last_alive_time << '\t'
            << (rec.alive ? 1 : 0) << '\t'
            << rec.latency_ms << '\t'
            << rec.consecutive_fails << '\t'
            << rec.total_tests << '\t'
            << rec.total_passes << '\n';
        saved++;
    }
    return saved;
}

int ConfigDatabase::loadFromDisk(const std::string& filepath) {
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs) return 0;

    std::string line;
    // Check header
    if (!std::getline(ifs, line)) return 0;
    if (line.find("#HUNTER_CONFIG_DB_V1") == std::string::npos) {
        // Legacy format or corrupted — skip
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int loaded = 0;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Parse tab-separated fields
        std::vector<std::string> fields;
        std::istringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() < 11) continue;

        std::string uri = fields[0];
        if (uri.empty() || uri.find("://") == std::string::npos) continue;

        std::string hash = hashUri(uri);
        if (db_.find(hash) != db_.end()) continue; // already exists
        if ((int)db_.size() >= max_size_) break;

        ConfigHealthRecord rec;
        rec.uri = uri;
        rec.uri_hash = hash;
        rec.tag = fields[1];
        rec.engine_used = fields[2];
        try {
            rec.first_seen = std::stod(fields[3]);
            rec.last_tested = std::stod(fields[4]);
            rec.last_alive_time = std::stod(fields[5]);
            rec.alive = (std::stoi(fields[6]) != 0);
            rec.latency_ms = std::stof(fields[7]);
            rec.consecutive_fails = std::stoi(fields[8]);
            rec.total_tests = std::stoi(fields[9]);
            rec.total_passes = std::stoi(fields[10]);
        } catch (...) {
            continue; // malformed line, skip
        }
        rec.needs_retest = true; // always retest after loading from disk
        db_[hash] = rec;
        loaded++;
    }
    return loaded;
}

// ═══════════════════════════════════════════════════════════════════
// ContinuousValidator
// ═══════════════════════════════════════════════════════════════════

ContinuousValidator::ContinuousValidator(ConfigDatabase& db, int batch_size,
                                         int timeout_s, int max_concurrent)
    : db_(db), batch_size_(batch_size), timeout_s_(timeout_s), max_concurrent_(max_concurrent) {}

bool ContinuousValidator::quickCheck(const std::string& uri) {
    // Require real download-capable success only
    ProxyTester tester;
    tester.setXrayPath("bin/xray.exe");
    tester.setSingBoxPath("bin/sing-box.exe");
    tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
    
    ProxyTestResult result = tester.testConfig(uri, "https://cachefly.cachefly.net/1mb.test", timeout_s_);
    return result.success && !result.telegram_only && result.download_speed_kbps > 0.0f;
}

std::pair<int, int> ContinuousValidator::validateBatch() {
    int effective_batch = std::min(batch_size_, 50);
    auto batch = db_.getUntestedBatch(effective_batch);
    if (batch.empty()) return {0, 0};

    { std::ostringstream _ls; _ls << "[Validator] Testing batch of " << batch.size() << " configs...";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    int tested = 0, passed = 0;
    auto& mgr = HunterTaskManager::instance();
    int test_timeout = std::max(1, std::min(30, timeout_s_));

    // ═══ Split: xray-compatible → batch test, others → individual ═══
    std::vector<std::string> xray_uris;
    std::vector<std::string> other_uris;
    for (auto& rec : batch) {
        bool needs_individual = (rec.uri.find("hysteria2://") == 0 || rec.uri.find("tuic://") == 0);
        if (needs_individual) other_uris.push_back(rec.uri);
        else xray_uris.push_back(rec.uri);
    }

    // ═══ Primary: batch test with single xray process (v2rayN pattern) ═══
    if (!xray_uris.empty()) {
        static std::atomic<int> s_batch_offset{0};
        int base = 29100 + (s_batch_offset.fetch_add((int)xray_uris.size() + 10) % 400);

        ProxyTester batch_tester;
        batch_tester.setXrayPath("bin/xray.exe");
        auto results = batch_tester.batchTestWithXray(xray_uris, base, test_timeout);
        for (auto& r : results) {
            bool full_success = r.success && !r.telegram_only && r.download_speed_kbps > 0.0f;
            tested++;
            if (full_success) passed++;
            db_.updateHealth(r.uri, full_success, full_success ? r.download_speed_kbps : 0.0f, r.engine_used);
        }
    }

    // ═══ Fallback: individual test for non-xray protocols ═══
    size_t vchunk = (size_t)std::max(1, std::min(50, max_concurrent_));

    { std::ostringstream _ls; _ls << "[Validator] Batch: " << xray_uris.size() << " xray, " << other_uris.size() << " individual";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    for (size_t off = 0; off < other_uris.size(); off += vchunk) {
        size_t chunk_end = std::min(off + vchunk, other_uris.size());
        std::vector<std::future<std::tuple<std::string, bool, float, std::string>>> futures;
        for (size_t i = off; i < chunk_end; i++) {
            std::string uri = other_uris[i];
            int timeout_cap = test_timeout;
            futures.push_back(mgr.submitIO([uri, timeout_cap]() -> std::tuple<std::string, bool, float, std::string> {
                ProxyTester local_tester;
                local_tester.setXrayPath("bin/xray.exe");
                local_tester.setSingBoxPath("bin/sing-box.exe");
                local_tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
                
                ProxyTestResult result = local_tester.testConfig(uri, "https://cachefly.cachefly.net/1mb.test", timeout_cap);
                bool is_alive = result.success && !result.telegram_only && result.download_speed_kbps > 0.0f;
                return {uri, is_alive, is_alive ? result.download_speed_kbps : 0.0f, result.engine_used};
            }));
        }

        for (auto& fut : futures) {
            try {
                auto [uri, ok, speed, engine_used] = fut.get();
                tested++;
                if (ok) passed++;
                db_.updateHealth(uri, ok, ok ? speed : 0.0f, engine_used);
            } catch (const std::exception& e) {
                std::cout << "  [Validator] Future exception: " << e.what() << std::endl;
            } catch (...) {
                std::cout << "  [Validator] Unknown future exception" << std::endl;
            }
        }
    }

    total_tested_ += tested;
    total_passed_ += passed;
    
    auto stats = db_.getStats();
    std::cout << "[Validator] Round done: tested=" << tested << " passed=" << passed 
              << " (DB: total=" << stats.total << " alive=" << stats.alive 
              << " untested=" << stats.untested_unique << ")" << std::endl;
    
    return {tested, passed};
}

bool ContinuousValidator::pingTestWithXray(const std::string& uri) {
    // Use XRay to test if config can connect/download anything
    // For now, fallback to quickCheck; in future, spawn xray with config and test connectivity
    return quickCheck(uri);
}

std::pair<int, int> ContinuousValidator::validateBatchWithXray() {
    auto batch = db_.getUntestedBatch(batch_size_);
    if (batch.empty()) return {0, 0};

    int tested = 0, passed = 0;
    auto& mgr = HunterTaskManager::instance();

    std::vector<std::future<std::pair<std::string, bool>>> futures;
    for (const auto& rec : batch) {
        futures.push_back(mgr.submitIO([this, uri = rec.uri]() -> std::pair<std::string, bool> {
            bool ok = pingTestWithXray(uri);
            return {uri, ok};
        }));
    }

    for (auto& fut : futures) {
        try {
            auto [uri, ok] = fut.get();
            tested++;
            if (ok) passed++;
            db_.updateHealth(uri, ok, ok ? 1000.0f : 0.0f, "xray");
        } catch (...) {}
    }

    total_tested_ += tested;
    total_passed_ += passed;
    return {tested, passed};
}

ContinuousValidator::ValidatorStats ContinuousValidator::getStats() const {
    ValidatorStats s;
    s.total_tested = total_tested_.load();
    s.total_passed = total_passed_.load();
    s.db = db_.getStats();
    return s;
}

} // namespace network
} // namespace hunter
