#include "network/continuous_validator.h"
#include "network/uri_parser.h"
#include "network/proxy_tester.h"
#include "core/utils.h"
#include "core/task_manager.h"

#include <algorithm>
#include <numeric>
#include <iostream>

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
    std::lock_guard<std::mutex> lock(mutex_);
    int added = 0;
    double now = utils::nowTimestamp();
    for (const auto& uri : uris) {
        if (uri.empty()) continue;
        std::string hash = hashUri(uri);
        if (db_.find(hash) != db_.end()) continue;
        if ((int)db_.size() >= max_size_) evictStale();
        if ((int)db_.size() >= max_size_) break;

        ConfigHealthRecord rec;
        rec.uri = uri;
        rec.uri_hash = hash;
        rec.tag = tag;
        rec.first_seen = now;
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
    return added;
}

void ConfigDatabase::updateHealth(const std::string& uri, bool alive, float latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = hashUri(uri);
    auto it = db_.find(hash);
    if (it == db_.end()) return;

    auto& rec = it->second;
    rec.last_tested = utils::nowTimestamp();
    rec.total_tests++;
    rec.needs_retest = false;

    if (alive) {
        rec.alive = true;
        rec.latency_ms = latency_ms;
        rec.consecutive_fails = 0;
        rec.total_passes++;
        rec.last_alive_time = rec.last_tested;
    } else {
        rec.consecutive_fails++;
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
        int priority;   // 0=never tested, 1=needs_retest, 2=alive stale 30s, 3=failed stale 60s
        int total_tests; // for secondary sort: fewer tests = higher priority
    };
    std::vector<Candidate> candidates;

    for (auto& [hash, rec] : db_) {
        // Priority 0: never tested (brand new configs)
        if (rec.total_tests == 0) {
            candidates.push_back({&rec, 0, 0});
            continue;
        }
        // Priority 1: flagged for retest
        if (rec.needs_retest) {
            candidates.push_back({&rec, 1, rec.total_tests});
            continue;
        }
        // Priority 2: alive configs stale > 30 seconds — continuous health recheck
        if (rec.alive && (now - rec.last_tested) > 30.0) {
            candidates.push_back({&rec, 2, rec.total_tests});
            continue;
        }
        // Priority 3: failed configs with < 5 consecutive fails, stale > 60s
        if (!rec.alive && rec.consecutive_fails < 5 && (now - rec.last_tested) > 60.0) {
            candidates.push_back({&rec, 3, rec.total_tests});
            continue;
        }
    }

    // Sort: primary by priority (ascending), secondary by total_tests (ascending = less tested first)
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
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

// ═══════════════════════════════════════════════════════════════════
// ContinuousValidator
// ═══════════════════════════════════════════════════════════════════

ContinuousValidator::ContinuousValidator(ConfigDatabase& db, int batch_size,
                                         int timeout_s, int max_concurrent)
    : db_(db), batch_size_(batch_size), timeout_s_(timeout_s), max_concurrent_(max_concurrent) {}

bool ContinuousValidator::quickCheck(const std::string& uri) {
    // Use real proxy testing with cachefly download
    ProxyTester tester;
    tester.setXrayPath("bin/xray.exe");
    tester.setSingBoxPath("bin/sing-box.exe");
    tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
    
    ProxyTestResult result = tester.testConfig(uri, "http://cp.cloudflare.com/generate_204", timeout_s_);
    return result.success;
}

std::pair<int, int> ContinuousValidator::validateBatch() {
    // Use larger batches - TCP pre-screening skips dead servers fast
    int effective_batch = std::min(batch_size_, 50);
    auto batch = db_.getUntestedBatch(effective_batch);
    if (batch.empty()) return {0, 0};

    { std::ostringstream _ls; _ls << "[Validator] Testing batch of " << batch.size() << " configs...";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    int tested = 0, passed = 0;
    auto& mgr = HunterTaskManager::instance();

    // Process in chunks sized by user speed settings (max_concurrent_)
    size_t vchunk = (size_t)std::max(1, std::min(50, max_concurrent_));
    int test_timeout = std::max(1, std::min(30, timeout_s_));

    { std::ostringstream _ls; _ls << "[Validator] Speed: concurrency=" << vchunk << " timeout=" << test_timeout << "s";
      utils::LogRingBuffer::instance().push(_ls.str()); }

    for (size_t off = 0; off < batch.size(); off += vchunk) {
        size_t chunk_end = std::min(off + vchunk, batch.size());
        std::vector<std::future<std::tuple<std::string, bool, float>>> futures;
        for (size_t i = off; i < chunk_end; i++) {
            std::string uri = batch[i].uri;
            int timeout_cap = test_timeout;
            futures.push_back(mgr.submitIO([uri, timeout_cap]() -> std::tuple<std::string, bool, float> {
                ProxyTester local_tester;
                local_tester.setXrayPath("bin/xray.exe");
                local_tester.setSingBoxPath("bin/sing-box.exe");
                local_tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
                
                ProxyTestResult result = local_tester.testConfig(uri, "http://cp.cloudflare.com/generate_204", timeout_cap);
                // Only count as alive if actual HTTP download succeeded (not telegram-only)
                bool real_success = result.success && !result.telegram_only;
                return {uri, real_success, result.download_speed_kbps};
            }));
        }

        for (auto& fut : futures) {
            try {
                auto [uri, ok, speed] = fut.get();
                tested++;
                if (ok) passed++;
                db_.updateHealth(uri, ok, ok ? speed : 0.0f);
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
            db_.updateHealth(uri, ok, ok ? 1000.0f : 0.0f);  // Approximate latency
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
