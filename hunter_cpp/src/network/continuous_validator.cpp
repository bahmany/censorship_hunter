#include "network/continuous_validator.h"
#include "network/uri_parser.h"
#include "network/proxy_tester.h"
#include "core/utils.h"
#include "core/endpoint_key.h"
#include "core/db_format.h"
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

ConfigDatabase::ConfigDatabase(int max_size) : max_size_(max_size), clock_(systemClock()) {}

std::string ConfigDatabase::keyFor(const std::string& uri) {
    return endpointKeyForUri(uri);
}

void ConfigDatabase::setClock(ClockFn clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = clock ? std::move(clock) : systemClock();
}

void ConfigDatabase::setThresholds(const HealthThresholds& th) {
    std::lock_guard<std::mutex> lock(mutex_);
    th_ = th;
}

HealthThresholds ConfigDatabase::thresholds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return th_;
}


int ConfigDatabase::addConfigs(const std::set<std::string>& uris, const std::string& tag) {
    return addConfigsWithPriority(uris, tag, nullptr);
}

int ConfigDatabase::addConfigsWithPriority(const std::set<std::string>& uris, const std::string& tag,
                                          int* promoted_existing) {
    std::lock_guard<std::mutex> lock(mutex_);
    int added = 0;
    int promoted = 0;
    double now = clock_();
    const bool high_priority = (tag == "manual" || tag == "import" || tag == "user_import");
    const double boost_until = high_priority ? (now + 1800.0) : 0.0;
    for (const auto& uri : uris) {
        if (uri.empty()) continue;
        std::string hash = keyFor(uri);
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
        initRecordIdentity(&rec);
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

namespace {
void touchLegacyCounters(ConfigHealthRecord& rec, double t, const std::string& engine) {
    rec.last_tested = t;
    rec.needs_retest = false;
    rec.priority_boost_until = 0.0;
    if (!engine.empty()) rec.engine_used = engine;
}
}  // namespace

ApplyEffect ConfigDatabase::applyProbeResult(const ProbeResult& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = db_.find(r.endpoint_key);
    if (it == db_.end()) return ApplyEffect::UnknownEndpoint;
    return applyLocked(it->second, r);
}

ApplyEffect ConfigDatabase::applyLocked(ConfigHealthRecord& rec, const ProbeResult& r) {
    const ApplyEffect eff = applyProbe(rec.ev, r, th_);
    if (eff != ApplyEffect::Applied && eff != ApplyEffect::Excluded) return eff;
    touchLegacyCounters(rec, r.finished_at, r.engine);
    if (eff == ApplyEffect::Applied) {
        rec.total_tests++;
        if (r.outcome == ProbeOutcome::Pass) {
            rec.total_passes++;
            rec.telegram_only = false;
            if (!r.exit_country.empty() && r.finished_at >= rec.exit_country_at) {
                rec.exit_country = r.exit_country;
                rec.exit_ip = r.exit_ip;
                rec.exit_country_source = "cloudflare_trace";
                rec.exit_country_at = r.finished_at;
            }
        }
    }
    syncLegacyFromEvidence(&rec);
    return eff;
}

bool ConfigDatabase::applyCountryResult(const std::string& endpoint_key, const CountryUpdate& u) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = db_.find(endpoint_key);
    if (it == db_.end()) return false;
    auto& rec = it->second;
    if (u.is_exit) {
        if (u.at < rec.exit_country_at) return false;
        if (u.network_generation < rec.network_generation) return false;
        rec.exit_country = u.country;
        rec.exit_ip = u.ip;
        rec.exit_country_source = u.source;
        rec.exit_country_at = u.at;
        rec.network_generation = u.network_generation;
    } else {
        if (u.at < rec.server_country_at) return false;
        rec.server_country = u.country;
        rec.server_country_source = u.source;
        rec.server_country_at = u.at;
        rec.geo_db_version = u.geo_db_version;
        rec.server_ips = u.server_ips;
    }
    return true;
}

void ConfigDatabase::markTestingRound(const std::string& endpoint_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = db_.find(endpoint_key);
    if (it != db_.end()) markTesting(it->second.ev);
}

// Legacy adapter (until A2 rewires callers): alive -> full Pass, !alive -> attributable
// RemoteFailure, telegram-only -> capability hint without health evidence.
void ConfigDatabase::updateHealth(const std::string& uri, bool alive, float latency_ms,
                                  const std::string& engine_used, bool force_dead,
                                  bool telegram_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = db_.find(keyFor(uri));
    if (it == db_.end()) return;
    auto& rec = it->second;
    const double t = std::max(clock_(), rec.ev.last_attempt_at);
    if (alive && telegram_only) {
        touchLegacyCounters(rec, t, engine_used);
        rec.total_tests++;
        rec.total_passes++;
        rec.alive = true;
        rec.telegram_only = true;
        rec.ev.telegram_only = true;
        rec.latency_ms = latency_ms;
        rec.last_alive_time = t;
        return;
    }
    rec.ev.telegram_only = false;
    ProbeResult r;
    r.endpoint_key = it->first;
    r.engine = engine_used;
    r.started_at = r.finished_at = t;
    r.attributable = true;
    if (alive) {
        r.outcome = ProbeOutcome::Pass;
        r.latency_ms = std::isfinite(latency_ms) && latency_ms >= 0.0f ? latency_ms : 0.0;
    } else {
        r.outcome = ProbeOutcome::RemoteFailure;
    }
    applyLocked(rec, r);
    if (!alive && force_dead) {
        forceDead(rec.ev, t, th_);
        syncLegacyFromEvidence(&rec);
    }
    if (alive) rec.latency_ms = latency_ms;
}

void ConfigDatabase::updateGeminiStatus(const std::string& uri, int gemini_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = keyFor(uri);
    auto it = db_.find(hash);
    if (it == db_.end()) return;
    it->second.gemini_status = gemini_status;
    it->second.gemini_checked_at = clock_();
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveForGeminiCheck(
        int gemini_interval_s, int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> result;
    double now = clock_();
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
    double now = clock_();

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

RankKey ConfigDatabase::rankKeyLocked(const ConfigHealthRecord& rec, double now) const {
    HealthEvaluation e = evaluateHealth(rec.ev, now, th_);
    RankKey k;
    k.tier = e.tier;
    k.score = e.score;
    k.last_full_success = rec.ev.last_full_success;
    k.key = rec.endpoint_key;
    return k;
}

// Shared ranking: health tier first (Stable, Healthy, Degraded, hint-only ...), then score,
// latest full success, canonical key. Records without evidence fall back to the legacy latency
// hint so pre-V4 data still sorts sanely.
void ConfigDatabase::sortRankedLocked(std::vector<ConfigHealthRecord>& v, double now) const {
    struct Item { RankKey k; double hint; size_t idx; };
    std::vector<Item> items;
    items.reserve(v.size());
    for (size_t i = 0; i < v.size(); i++)
        items.push_back({rankKeyLocked(v[i], now), v[i].telegram_only ? 1e18 : (double)v[i].latency_ms, i});
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.k.tier != b.k.tier) return a.k.tier < b.k.tier;
        if (a.k.score != b.k.score) return a.k.score > b.k.score;
        if (a.k.tier == 3 && a.hint != b.hint) return a.hint < b.hint;
        return rankedBefore(a.k, b.k);
    });
    std::vector<ConfigHealthRecord> out;
    out.reserve(v.size());
    for (auto& it : items) out.push_back(std::move(v[it.idx]));
    v.swap(out);
}

std::vector<std::pair<std::string, float>> ConfigDatabase::getHealthyConfigs(int max_count) {
    auto recs = getHealthyRecords(max_count);
    std::vector<std::pair<std::string, float>> healthy;
    healthy.reserve(recs.size());
    for (auto& r : recs) healthy.emplace_back(r.uri, r.latency_ms);
    return healthy;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getHealthyRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> healthy;
    for (auto& [hash, rec] : db_) {
        if (rec.alive && !rec.telegram_only && rec.latency_ms > 0) healthy.push_back(rec);
    }
    sortRankedLocked(healthy, clock_());
    if ((int)healthy.size() > max_count) healthy.resize(max_count);
    return healthy;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getRecommendedRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double t = clock_();
    std::vector<ConfigHealthRecord> out;
    for (auto& [hash, rec] : db_)
        if (evaluateHealth(rec.ev, t, th_).stability == Stability::Stable) out.push_back(rec);
    sortRankedLocked(out, t);
    if ((int)out.size() > max_count) out.resize(max_count);
    return out;
}

HealthEvaluation ConfigDatabase::evaluate(const ConfigHealthRecord& rec) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return evaluateHealth(rec.ev, clock_(), th_);
}

bool ConfigDatabase::getRecord(const std::string& uri_or_key, ConfigHealthRecord* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = db_.find(uri_or_key.compare(0, 4, "ek1:") == 0 ? uri_or_key : keyFor(uri_or_key));
    if (it == db_.end()) return false;
    *out = it->second;
    return true;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getTelegramOnlyRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> healthy;
    for (auto& [hash, rec] : db_) {
        if (rec.alive && rec.telegram_only) healthy.push_back(rec);
    }
    std::sort(healthy.begin(), healthy.end(),
              [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
                  if (a.last_alive_time != b.last_alive_time) return a.last_alive_time > b.last_alive_time;
                  return a.endpoint_key < b.endpoint_key;
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
    std::vector<ConfigHealthRecord> alive, dead;
    for (auto& [hash, rec] : db_) (rec.alive ? alive : dead).push_back(rec);
    // Usable first through the shared evaluator, then non-alive by most recent test.
    sortRankedLocked(alive, clock_());
    std::sort(dead.begin(), dead.end(), [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
        if (a.last_tested != b.last_tested) return a.last_tested > b.last_tested;
        return a.endpoint_key < b.endpoint_key;
    });
    alive.insert(alive.end(), std::make_move_iterator(dead.begin()), std::make_move_iterator(dead.end()));
    if ((int)alive.size() > max_count) alive.resize(max_count);
    return alive;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveRecords(int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> alive, tg;
    for (auto& [hash, rec] : db_) {
        if (rec.alive) (rec.telegram_only ? tg : alive).push_back(rec);
    }
    sortRankedLocked(alive, clock_());
    std::sort(tg.begin(), tg.end(), [](const ConfigHealthRecord& a, const ConfigHealthRecord& b) {
        if (a.last_alive_time != b.last_alive_time) return a.last_alive_time > b.last_alive_time;
        return a.endpoint_key < b.endpoint_key;
    });
    alive.insert(alive.end(), tg.begin(), tg.end());
    if ((int)alive.size() > max_count) alive.resize(max_count);
    return alive;
}

std::string ConfigDatabase::getPreferredEngine(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string hash = keyFor(uri);
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
    double now = clock_();
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
    int before = (int)db_.size();
    evictStale();
    return before - (int)db_.size();
}

// Age basis for eviction: last evidence of life, else first sighting. Local outages never
// create failure evidence (excluded outcomes), so they cannot mass-delete records.
static double evidenceAnchor(const ConfigHealthRecord& rec) {
    double a = std::max(rec.ev.last_full_success, rec.last_alive_time);
    return a > 0.0 ? a : rec.first_seen;
}

void ConfigDatabase::evictStale() {
    if (db_.empty()) return;
    const double now = clock_();
    // Phase 1: Dead records past the retention window (>= 72 h).
    std::vector<std::string> dead_hashes;
    for (auto& [hash, rec] : db_) {
        if (rec.ev.state == HealthState::Dead && (now - evidenceAnchor(rec)) > th_.dead_retention_s)
            dead_hashes.push_back(hash);
    }
    for (auto& h : dead_hashes) db_.erase(h);

    // Phase 2: at capacity, evict the oldest inactive evidence (never Healthy/Degraded).
    if ((int)db_.size() < max_size_) return;
    std::vector<std::pair<std::string, double>> candidates;
    for (auto& [hash, rec] : db_) {
        const auto st = rec.ev.state;
        if (st == HealthState::Dead || st == HealthState::Unstable ||
            ((st == HealthState::Unknown || st == HealthState::Testing) && !rec.alive))
            candidates.emplace_back(hash, std::max(rec.ev.last_attempt_at, evidenceAnchor(rec)));
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second < b.second;
        return a.first < b.first;
    });
    int to_remove = std::max(1, (int)candidates.size() / 4);
    for (int i = 0; i < to_remove && i < (int)candidates.size(); i++) db_.erase(candidates[i].first);
}

int ConfigDatabase::clearOlderThan(int max_age_hours) {
    std::lock_guard<std::mutex> lock(mutex_);
    double now = clock_();
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
        const std::string hash = keyFor(uri);
        auto it = db_.find(hash);
        if (it != db_.end()) {
            db_.erase(it);
            removed++;
        }
    }
    return removed;
}

std::vector<ConfigHealthRecord> ConfigDatabase::getAliveForRevalidation(
        int revalidate_interval_s, int max_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConfigHealthRecord> result;
    double now = clock_();
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
    double now = clock_();
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
