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

    // First: never tested
    for (auto& [hash, rec] : db_) {
        if (rec.total_tests == 0) {
            batch.push_back(rec);
            if ((int)batch.size() >= batch_size) return batch;
        }
    }

    // Second: needs_retest flag
    for (auto& [hash, rec] : db_) {
        if (rec.needs_retest && rec.total_tests > 0) {
            batch.push_back(rec);
            if ((int)batch.size() >= batch_size) return batch;
        }
    }

    // Third: stale healthy configs (re-validate after 60s)
    for (auto& [hash, rec] : db_) {
        if (rec.alive && rec.total_tests > 0 && (now - rec.last_tested) > 60.0) {
            batch.push_back(rec);
            if ((int)batch.size() >= batch_size) return batch;
        }
    }

    // Fourth: stale failed configs (retry after 120s)
    for (auto& [hash, rec] : db_) {
        if (!rec.alive && rec.total_tests > 0 && rec.consecutive_fails < 5 && 
            (now - rec.last_tested) > 120.0) {
            batch.push_back(rec);
            if ((int)batch.size() >= batch_size) return batch;
        }
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

std::set<std::string> ConfigDatabase::getAllUris() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> uris;
    for (auto& [hash, rec] : db_) uris.insert(rec.uri);
    return uris;
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
    // Remove oldest entries with most consecutive failures
    if (db_.empty()) return;
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

// ═══════════════════════════════════════════════════════════════════
// ContinuousValidator
// ═══════════════════════════════════════════════════════════════════

ContinuousValidator::ContinuousValidator(ConfigDatabase& db, int batch_size)
    : db_(db), batch_size_(batch_size) {}

bool ContinuousValidator::quickCheck(const std::string& uri) {
    // Use real proxy testing with cachefly download
    ProxyTester tester;
    tester.setXrayPath("bin/xray.exe");
    tester.setSingBoxPath("bin/sing-box.exe");
    tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
    
    ProxyTestResult result = tester.testConfig(uri, "http://cp.cloudflare.com/generate_204", 15);
    return result.success;
}

std::pair<int, int> ContinuousValidator::validateBatch() {
    // Use larger batches - TCP pre-screening skips dead servers fast
    int effective_batch = std::min(batch_size_, 50);
    auto batch = db_.getUntestedBatch(effective_batch);
    if (batch.empty()) return {0, 0};

    std::cout << "[Validator] Testing batch of " << batch.size() << " configs..." << std::endl;

    int tested = 0, passed = 0;
    auto& mgr = HunterTaskManager::instance();

    // Process in small chunks to avoid saturating the shared thread pool
    constexpr size_t VCHUNK = 10;
    for (size_t off = 0; off < batch.size(); off += VCHUNK) {
        size_t chunk_end = std::min(off + VCHUNK, batch.size());
        std::vector<std::future<std::tuple<std::string, bool, float>>> futures;
        for (size_t i = off; i < chunk_end; i++) {
            std::string uri = batch[i].uri;
            futures.push_back(mgr.submitIO([uri]() -> std::tuple<std::string, bool, float> {
                ProxyTester local_tester;
                local_tester.setXrayPath("bin/xray.exe");
                local_tester.setSingBoxPath("bin/sing-box.exe");
                local_tester.setMihomoPath("bin/mihomo-windows-amd64-compatible.exe");
                
                ProxyTestResult result = local_tester.testConfig(uri, "http://cp.cloudflare.com/generate_204", 15);
                return {uri, result.success, result.download_speed_kbps};
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
