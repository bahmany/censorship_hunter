#include "network/continuous_validator.h"
#include "network/uri_parser.h"
#include "network/proxy_tester.h"
#include "core/utils.h"
#include "core/task_manager.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <numeric>
#include <optional>
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

 namespace {

// Wait for a pool task with a hard deadline.
//
// These tasks shell out to xray/sing-box; a wedged subprocess makes the task
// never complete, and a bare future::get() then parks this validator thread
// forever. ThreadManager::stopAll() can never join it, so closing the app
// hangs instead of exiting. Skipping a straggler is safe: the task captures
// its inputs by value and the result is simply dropped.
template <typename T>
std::optional<T> getBefore(std::future<T>& fut,
                           std::chrono::steady_clock::time_point deadline) {
    auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining.count() <= 0) return std::nullopt;
    if (fut.wait_for(remaining) != std::future_status::ready) return std::nullopt;
    return fut.get();
}

// Slack added on top of the nominal per-test timeout before we give up on a
// chunk: process spawn, queueing behind other pool work, and teardown.
constexpr int kChunkSlackSeconds = 60;

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
     auto parsed = UriParser::parse(uri);
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

 bool isUsableResult(const ProxyTestResult& result) {
     return result.success && !result.telegram_only && result.download_speed_kbps > 0.0f;
 }

 bool isTelegramOnlyResult(const ProxyTestResult& result) {
     return result.success && result.telegram_only;
 }

 float healthMetricFromResult(const ProxyTestResult& result) {
     if (!isUsableResult(result)) return 0.0f;
     const float speed = std::max(result.download_speed_kbps, 0.1f);
     return std::max(1.0f, 1000.0f / speed);
 }

 } // namespace

// ═══════════════════════════════════════════════════════════════════
// ConfigDatabase
// ═══════════════════════════════════════════════════════════════════

ConfigDatabase::ConfigDatabase(int max_size) : max_size_(max_size) {}

std::string ConfigDatabase::hashUri(const std::string& uri) const {
    return utils::sha1Hex(endpointKeyForUri(uri)).substr(0, 16);
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
                                  const std::string& engine_used, bool force_dead,
                                  bool telegram_only) {
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
        rec.telegram_only = telegram_only;
        rec.latency_ms = latency_ms;
        rec.consecutive_fails = 0;
        rec.total_passes++;
        rec.last_alive_time = rec.last_tested;
    } else {
        if (force_dead) rec.consecutive_fails = 3;
        else rec.consecutive_fails++;
        // Mark dead after 2 consecutive fails (was 3) — gets dead configs
        // into the eviction pipeline faster, keeping the 1M-entry DB lean.
        if (rec.consecutive_fails >= 2) {
            rec.alive = false;
            rec.telegram_only = false;
            rec.latency_ms = 0.0f;
        }
    }
}

void ConfigDatabase::updateGeminiStatus(const std::string& uri, int gemini_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = hashUri(uri);
    auto it = db_.find(hash);
    if (it == db_.end()) return;
    it->second.gemini_status = gemini_status;
    it->second.gemini_checked_at = utils::nowTimestamp();
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveForGeminiCheck(
        int gemini_interval_s, int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> result;
    double now = utils::nowTimestamp();
    for (auto& [hash, rec] : db_) {
        if (!rec.alive) continue;
        // Need check if: never checked (gemini_checked_at == 0) OR
        // last check is older than gemini_interval_s
        if (rec.gemini_checked_at == 0.0 || (now - rec.gemini_checked_at) > gemini_interval_s) {
            result.push_back(rec);
            if ((int)result.size() >= max_count) break;
        }
    }
    return result;
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
        if (rec.alive && !rec.telegram_only && rec.latency_ms > 0) {
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
        if (rec.alive && !rec.telegram_only && rec.latency_ms > 0) {
            healthy.push_back(rec);
        }
    }
    std::sort(healthy.begin(), healthy.end(),
              [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) { return a.latency_ms < b.latency_ms; });
    if ((int)healthy.size() > max_count) healthy.resize(max_count);
    return healthy;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getTelegramOnlyRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> healthy;
    for (auto& [hash, rec] : db_) {
        if (rec.alive && rec.telegram_only) {
            healthy.push_back(rec);
        }
    }
    std::sort(healthy.begin(), healthy.end(),
              [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
                  return a.last_alive_time > b.last_alive_time;
              });
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
    std::sort(all.begin(), all.end(), [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
        if (a.alive != b.alive) return a.alive > b.alive;
        if (a.alive && b.alive) {
            if (a.telegram_only != b.telegram_only) return a.telegram_only < b.telegram_only;
            if (!a.telegram_only && !b.telegram_only) return a.latency_ms < b.latency_ms;
            return a.last_alive_time > b.last_alive_time;
        }
        return a.last_tested > b.last_tested;
    });
    if ((int)all.size() > max_count) all.resize(max_count);
    return all;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> alive;
    for (auto& [hash, rec] : db_) {
        if (rec.alive) alive.push_back(rec);
    }
    std::sort(alive.begin(), alive.end(), [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
        // Non-telegram first, then telegram-only; within each group, by latency.
        if (a.telegram_only != b.telegram_only) return a.telegram_only < b.telegram_only;
        if (!a.telegram_only && !b.telegram_only) {
            if (a.latency_ms != b.latency_ms) return a.latency_ms < b.latency_ms;
        }
        return a.last_alive_time > b.last_alive_time;
    });
    if ((int)alive.size() > max_count) alive.resize(max_count);
    return alive;
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

int ConfigDatabase::evictDead() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_.empty()) return 0;
    double now = utils::nowTimestamp();
    int before = (int)db_.size();
    evictStale();
    // Also drop configs that were never alive and are older than 10 min
    constexpr double NEVER_ALIVE_TTL = 0.167 * 3600.0;  // 10 minutes
    std::vector<std::string> stale;
    for (auto& [hash, rec] : db_) {
        if (!rec.alive && rec.last_alive_time == 0.0 &&
            rec.total_tests >= 2 && rec.consecutive_fails >= 2 &&
            (now - rec.first_seen) > NEVER_ALIVE_TTL) {
            stale.push_back(hash);
        }
    }
    for (auto& h : stale) db_.erase(h);
    return before - (int)db_.size();
}

void ConfigDatabase::evictStale() {
    if (db_.empty()) return;
    double now = utils::nowTimestamp();
    // Aggressive eviction thresholds — tuned for low-RAM systems.
    // Dead configs are evicted after 15 min (was 1 hour) to keep RAM lean.
    constexpr double DEAD_TTL = 0.25 * 3600.0;  // 15 minutes
    constexpr int FAIL_THRESHOLD = 3;

    // Phase 1: Remove configs that have been continuously offline for 15+ min
    std::vector<std::string> dead_hashes;
    for (auto& [hash, rec] : db_) {
        if (!rec.alive && rec.total_tests > 0 && rec.last_alive_time > 0.0 &&
            (now - rec.last_alive_time) > DEAD_TTL) {
            dead_hashes.push_back(hash);
        }
        // Also remove configs that were NEVER alive and tested 3+ times
        if (!rec.alive && rec.total_tests >= FAIL_THRESHOLD && rec.last_alive_time == 0.0 &&
            rec.consecutive_fails >= FAIL_THRESHOLD) {
            dead_hashes.push_back(hash);
        }
    }
    for (auto& h : dead_hashes) db_.erase(h);

    // Phase 2: If still over capacity, remove oldest high-failure entries
    if ((int)db_.size() < max_size_) return;
    std::vector<std::pair<std::string, double>> candidates;
    for (auto& [hash, rec] : db_) {
        if (rec.consecutive_fails >= FAIL_THRESHOLD || (!rec.alive && rec.total_tests > 2)) {
            candidates.emplace_back(hash, rec.first_seen);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    // Evict up to 25% of dead candidates to free memory quickly
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
    ofs << "#HUNTER_CONFIG_DB_V3\n";
    int saved = 0;
    for (const auto& [hash, rec] : db_) {
        if (rec.uri.empty()) continue;
        ofs << rec.uri << '\t'
            << rec.tag << '\t'
            << rec.engine_used << '\t'
            << std::fixed << rec.first_seen << '\t'
            << rec.last_tested << '\t'
            << rec.last_alive_time << '\t'
            << (rec.alive ? 1 : 0) << '\t'
            << (rec.telegram_only ? 1 : 0) << '\t'
            << rec.latency_ms << '\t'
            << rec.consecutive_fails << '\t'
            << rec.total_tests << '\t'
            << rec.total_passes << '\t'
            << rec.gemini_status << '\t'
            << std::fixed << rec.gemini_checked_at << '\n';
        saved++;
    }
    return saved;
}

int ConfigDatabase::loadFromDisk(const std::string& filepath) {
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs) return 0;

    std::string line;
    if (!std::getline(ifs, line)) return 0;
    const bool is_v3 = line.find("#HUNTER_CONFIG_DB_V3") != std::string::npos;
    const bool is_v2 = line.find("#HUNTER_CONFIG_DB_V2") != std::string::npos;
    const bool is_v1 = line.find("#HUNTER_CONFIG_DB_V1") != std::string::npos;
    if (!is_v3 && !is_v2 && !is_v1) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    int loaded = 0;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::istringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) {
            fields.push_back(field);
        }
        if ((is_v3 && fields.size() < 14) || (is_v2 && fields.size() < 12) || (is_v1 && fields.size() < 11)) continue;

        std::string uri = fields[0];
        if (uri.empty() || uri.find("://") == std::string::npos) continue;

        std::string hash = hashUri(uri);
        if (db_.find(hash) != db_.end()) continue;
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
            int shift = 0;
            if (is_v3 || is_v2) {
                rec.telegram_only = (std::stoi(fields[7]) != 0);
                shift = 1;
            }
            rec.latency_ms = std::stof(fields[7 + shift]);
            rec.consecutive_fails = std::stoi(fields[8 + shift]);
            rec.total_tests = std::stoi(fields[9 + shift]);
            rec.total_passes = std::stoi(fields[10 + shift]);
            if (is_v3 && fields.size() >= 14) {
                rec.gemini_status = std::stoi(fields[11 + shift]);
                rec.gemini_checked_at = std::stod(fields[12 + shift]);
            }
        } catch (...) {
            continue;
        }
        rec.needs_retest = true;
        db_[hash] = rec;
        loaded++;
    }
    return loaded;
}

// ─── Live connections cache ───
// A separate file (HUNTER_live_cache.tsv) stores only alive configs.
// This survives restarts — on startup, alive configs are re-merged
// into the DB and given priority for revalidation.

int ConfigDatabase::saveLiveToDisk(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try { utils::mkdirRecursive(utils::dirName(filepath)); } catch (...) {}
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) return 0;
    ofs << "#HUNTER_LIVE_CACHE_V2\n";
    int saved = 0;
    for (const auto& [hash, rec] : db_) {
        // Save configs that are currently alive OR were alive recently
        // (within 3 days). This preserves live connections across restarts
        // even if they're temporarily down.
        if (rec.uri.empty()) continue;
        if (!rec.alive && rec.last_alive_time > 0.0) {
            // Was alive but currently dead — keep only if within 3-day TTL
            double age = utils::nowTimestamp() - rec.last_alive_time;
            if (age > 259200.0) continue;  // 3 days
        } else if (!rec.alive) {
            continue;  // Never alive — don't cache
        }
        ofs << rec.uri << '\t'
            << rec.tag << '\t'
            << rec.engine_used << '\t'
            << std::fixed << rec.first_seen << '\t'
            << rec.last_tested << '\t'
            << rec.last_alive_time << '\t'
            << (rec.alive ? 1 : 0) << '\t'
            << (rec.telegram_only ? 1 : 0) << '\t'
            << rec.latency_ms << '\t'
            << rec.consecutive_fails << '\t'
            << rec.total_tests << '\t'
            << rec.total_passes << '\t'
            << rec.gemini_status << '\t'
            << std::fixed << rec.gemini_checked_at << '\n';
        saved++;
    }
    return saved;
}

int ConfigDatabase::loadLiveFromDisk(const std::string& filepath) {
    std::ifstream ifs(filepath, std::ios::binary);
    if (!ifs) return 0;
    std::string line;
    if (!std::getline(ifs, line)) return 0;
    const bool is_v2 = line.find("#HUNTER_LIVE_CACHE_V2") != std::string::npos;
    const bool is_v1 = line.find("#HUNTER_LIVE_CACHE_V1") != std::string::npos;
    if (!is_v2 && !is_v1) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    int loaded = 0;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::istringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) fields.push_back(field);
        if ((is_v2 && fields.size() < 14) || (is_v1 && fields.size() < 12)) continue;

        std::string uri = fields[0];
        if (uri.empty() || uri.find("://") == std::string::npos) continue;
        std::string hash = hashUri(uri);

        auto existing = db_.find(hash);
        if (existing != db_.end()) {
            // Merge: if the cached record says alive, mark it alive in DB
            // and set needs_retest so it gets revalidated soon.
            try {
                bool was_alive = (std::stoi(fields[6]) != 0);
                double last_alive = std::stod(fields[5]);
                if (was_alive) {
                    existing->second.alive = true;
                    existing->second.last_alive_time = last_alive;
                    existing->second.consecutive_fails = 0;
                    existing->second.needs_retest = true;
                    // Restore gemini status from cache
                    if (is_v2 && fields.size() >= 14) {
                        existing->second.gemini_status = std::stoi(fields[12]);
                        existing->second.gemini_checked_at = std::stod(fields[13]);
                    }
                    loaded++;
                }
            } catch (...) {}
            continue;
        }

        // New record — add to DB
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
            rec.telegram_only = (std::stoi(fields[7]) != 0);
            rec.latency_ms = std::stof(fields[8]);
            rec.consecutive_fails = std::stoi(fields[9]);
            rec.total_tests = std::stoi(fields[10]);
            rec.total_passes = std::stoi(fields[11]);
            if (is_v2 && fields.size() >= 14) {
                rec.gemini_status = std::stoi(fields[12]);
                rec.gemini_checked_at = std::stod(fields[13]);
            }
        } catch (...) { continue; }
        rec.needs_retest = true;
        db_[hash] = rec;
        loaded++;
    }
    return loaded;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveForRevalidation(
        int revalidate_interval_s, int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> result;
    double now = utils::nowTimestamp();
    for (auto& [hash, rec] : db_) {
        if (!rec.alive) continue;
        double age = now - rec.last_tested;
        if (age >= revalidate_interval_s) {
            result.push_back(rec);
            if ((int)result.size() >= max_count) break;
        }
    }
    return result;
}

int ConfigDatabase::removeDeadLive(int dead_ttl_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    double now = utils::nowTimestamp();
    int removed = 0;
    std::vector<std::string> to_remove;
    for (auto& [hash, rec] : db_) {
        // Remove configs that were once alive but have been dead for
        // longer than dead_ttl_s (3 days by default).
        if (!rec.alive && rec.last_alive_time > 0.0) {
            double dead_age = now - rec.last_alive_time;
            if (dead_age > dead_ttl_s) {
                to_remove.push_back(hash);
            }
        }
    }
    for (auto& h : to_remove) {
        db_.erase(h);
        removed++;
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
    ProxyTester tester;

    ProxyTestResult result = tester.testConfig(uri, "https://cachefly.cachefly.net/1mb.test", timeout_s_);
    return isUsableResult(result);
}

std::pair<int, int> ContinuousValidator::validateBatch() {
    // ─── Direct full proxy testing ───
    // We test each config by spawning a proxy engine (xray/sing-box) and
    // attempting a download through it. We do NOT do a TCP pre-screen
    // first, because in censored environments (Iran, China, etc.), ISPs
    // use DPI to block raw TCP connections to proxy server ports. A
    // server that fails a raw TCP connect can still work through xray,
    // which uses TLS/obfuscation to bypass DPI. TCP pre-screening would
    // filter out ALL configs as "dead" in censored environments.
    //
    // Performance is managed through:
    // - Single engine per config (don't try xray→sing-box→mihomo for dead servers)
    // - Larger batch sizes (40-100 configs per cycle)
    // - Faster validator interval (5 seconds)
    // - Concurrency limiting via max_concurrent_

    // Proactively evict dead configs each batch to keep the 1M-entry DB lean.
    // This runs every ~5s (VALIDATOR_INTERVAL_S) and prevents dead configs
    // from accumulating and consuming RAM between full-capacity evictions.
    db_.evictDead();

    // Remove configs that were once alive but have been dead for 3+ days.
    // These are permanently dead and should not clutter the live cache.
    static double last_dead_live_check = 0.0;
    double now_ts = utils::nowTimestamp();
    if (now_ts - last_dead_live_check > 3600.0) {  // check hourly
        int removed = db_.removeDeadLive(259200);  // 3 days
        if (removed > 0) {
            utils::LogRingBuffer::instance().push(
                "[Validator] Removed " + std::to_string(removed) +
                " configs dead for 3+ days from live cache");
        }
        last_dead_live_check = now_ts;
    }

    // ─── Phase 1: Revalidate alive configs every 30 minutes ───
    // Live connections must be checked periodically to ensure they still
    // work. We retest up to 20 alive configs per batch whose last test
    // was >30 min ago. This keeps the live list fresh without overwhelming
    // the test pipeline.
    constexpr int REVALIDATE_INTERVAL_S = 1800;  // 30 minutes
    constexpr int REVALIDATE_BATCH = 20;
    auto revalidate_batch = db_.getAliveForRevalidation(REVALIDATE_INTERVAL_S, REVALIDATE_BATCH);
    int revalidated = 0, revalidated_passed = 0;

    if (!revalidate_batch.empty()) {
        int test_timeout = std::max(1, std::min(30, timeout_s_));
        size_t vchunk = (size_t)std::max(1, std::min(20, max_concurrent_));
        auto& mgr = HunterTaskManager::instance();

        for (size_t off = 0; off < revalidate_batch.size(); off += vchunk) {
            size_t chunk_end = std::min(off + vchunk, revalidate_batch.size());
            std::vector<std::future<ProxyTestResult>> futures;
            for (size_t i = off; i < chunk_end; i++) {
                std::string uri = revalidate_batch[i].uri;
                int timeout_cap = test_timeout;
                futures.push_back(mgr.submitIO([uri, timeout_cap]() -> ProxyTestResult {
                    ProxyTester local_tester;
                    return local_tester.testConfig(uri, "https://cachefly.cachefly.net/1mb.test", timeout_cap);
                }));
            }
            auto chunk_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(test_timeout + kChunkSlackSeconds);
            for (auto& fut : futures) {
                try {
                    auto maybe = getBefore(fut, chunk_deadline);
                    if (!maybe) continue;  // straggler: drop it rather than hang
                    const auto& result = *maybe;
                    bool ok = isUsableResult(result);
                    bool telegram_only = isTelegramOnlyResult(result);
                    float health_metric = healthMetricFromResult(result);
                    revalidated++;
                    if (ok || telegram_only) revalidated_passed++;
                    db_.updateHealth(result.uri, ok || telegram_only,
                                     ok ? health_metric : 0.0f,
                                     result.engine_used, false, telegram_only);
                } catch (...) {}
            }
        }
        if (revalidated > 0) {
            std::ostringstream ss;
            ss << "[Validator] Revalidated " << revalidated << " live configs ("
               << revalidated_passed << " still alive)";
            utils::LogRingBuffer::instance().push(ss.str());
        }
    }

    // ─── Phase 1.5: Gemini accessibility check for alive configs ───
    // Check if alive configs can reach Gemini (Google AI API). This runs
    // every hour per config (not every batch) to avoid overhead. We check
    // up to 5 configs per batch that haven't been checked in the last hour.
    constexpr int GEMINI_CHECK_INTERVAL_S = 3600;  // 1 hour
    constexpr int GEMINI_CHECK_BATCH = 5;
    auto gemini_batch = db_.getAliveForGeminiCheck(GEMINI_CHECK_INTERVAL_S, GEMINI_CHECK_BATCH);
    int gemini_checked = 0, gemini_accessible = 0;

    if (!gemini_batch.empty()) {
        int gemini_timeout = std::max(5, std::min(20, timeout_s_));
        size_t gchunk = (size_t)std::max(1, std::min(5, max_concurrent_));
        auto& gmgr = HunterTaskManager::instance();

        // Pair each URI with its future for proper result tracking
        struct GeminiTask {
            std::string uri;
            std::future<int> future;
        };
        std::vector<GeminiTask> gemini_tasks;

        for (size_t off = 0; off < gemini_batch.size(); off += gchunk) {
            size_t chunk_end = std::min(off + gchunk, gemini_batch.size());
            for (size_t i = off; i < chunk_end; i++) {
                std::string uri = gemini_batch[i].uri;
                int gtimeout = gemini_timeout;
                GeminiTask task;
                task.uri = uri;
                task.future = gmgr.submitIO([uri, gtimeout]() -> int {
                    ProxyTester gemini_tester;
                    return gemini_tester.checkGeminiAccess(uri, gtimeout);
                });
                gemini_tasks.push_back(std::move(task));
            }
            // Wait for this chunk to complete before starting the next
            auto gemini_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(gemini_timeout + kChunkSlackSeconds);
            for (size_t i = off; i < chunk_end && i < gemini_tasks.size(); i++) {
                try {
                    auto status = getBefore(gemini_tasks[i].future, gemini_deadline);
                    if (!status) continue;  // straggler: drop it rather than hang
                    gemini_checked++;
                    if (*status == 1) gemini_accessible++;
                    db_.updateGeminiStatus(gemini_tasks[i].uri, *status);
                } catch (...) {
                    db_.updateGeminiStatus(gemini_tasks[i].uri, -1);
                }
            }
        }
        if (gemini_checked > 0) {
            std::ostringstream ss;
            ss << "[Validator] Gemini check: " << gemini_accessible << "/" << gemini_checked
               << " configs can reach Gemini API";
            utils::LogRingBuffer::instance().push(ss.str());
        }
    }

    // ─── Phase 2: Test untested/new configs ───
    // Use batchTestWithXray (one xray process for ALL configs in the batch)
    // instead of testConfig (one process per config). This is 10-50x faster:
    // a single xray process with N inbounds tests all configs in parallel
    // through their individual SOCKS ports, vs. spawning/killing N separate
    // processes. With 100k+ untested configs, this is the difference between
    // finding working proxies in minutes vs. hours.
    int effective_batch = std::max(1, std::min(batch_size_, 200));
    auto batch = db_.getUntestedBatch(effective_batch);
    if (batch.empty() && revalidated == 0) return {0, 0};
    if (batch.empty()) return {revalidated, revalidated_passed};

    int tested = 0, passed = 0;
    int test_timeout = std::max(1, std::min(30, timeout_s_));

    // Collect URIs for batch testing
    std::vector<std::string> batch_uris;
    batch_uris.reserve(batch.size());
    for (auto& rec : batch) {
        batch_uris.push_back(rec.uri);
    }

    // Use a port range that doesn't conflict with the scanner (which uses
    // DEFAULT_BENCHMARK_BASE_PORT + offset). Rotate the offset each call
    // so consecutive batches don't reuse ports still in TIME_WAIT.
    constexpr int VALIDATOR_BASE_PORT = 22000;
    int port_offset = batch_port_offset_.fetch_add(500) % 5000;
    int batch_base_port = VALIDATOR_BASE_PORT + port_offset;

    ProxyTester tester;
    std::vector<ProxyTestResult> batch_results;
    try {
        batch_results = tester.batchTestWithXray(batch_uris, batch_base_port, test_timeout);
    } catch (const std::exception& e) {
        std::cout << "  [Validator] Batch exception: " << e.what() << std::endl;
    } catch (...) {
        std::cout << "  [Validator] Batch unknown exception" << std::endl;
    }

    // Update DB health from batch results
    for (size_t i = 0; i < batch_results.size() && i < batch_uris.size(); i++) {
        const auto& result = batch_results[i];
        bool ok = isUsableResult(result);
        bool telegram_only = isTelegramOnlyResult(result);
        float health_metric = healthMetricFromResult(result);
        tested++;
        if (ok) passed++;
        db_.updateHealth(batch_uris[i], ok || telegram_only,
                         ok ? health_metric : 0.0f,
                         result.engine_used, false, telegram_only);
    }

    total_tested_ += tested;
    total_passed_ += passed;
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

    // All tests were submitted at once, so budget the whole batch rather than
    // one chunk: concurrency is capped by the pool, not by this loop.
    const int xray_timeout = std::max(1, std::min(30, timeout_s_));
    auto batch_deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(xray_timeout * 4 + kChunkSlackSeconds);
    for (auto& fut : futures) {
        try {
            auto maybe = getBefore(fut, batch_deadline);
            if (!maybe) continue;  // straggler: drop it rather than hang
            const auto& [uri, ok] = *maybe;
            tested++;
            if (ok) passed++;
            db_.updateHealth(uri, ok, ok ? 1000.0f : 0.0f, "sing-box");
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
