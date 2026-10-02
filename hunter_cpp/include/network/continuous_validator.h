#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <atomic>

#include "core/models.h"
#include "core/health_score.h"
#include "core/db_format.h"

namespace hunter {
namespace network {

/**
 * @brief Persistent config health database
 * 
 * Stores ConfigHealthRecord for each discovered config URI,
 * tracks alive/dead status, latency, and provides batches
 * for continuous background validation.
 */
class ConfigDatabase {
public:
    explicit ConfigDatabase(int max_size = 200000);
    ~ConfigDatabase() = default;

    /**
     * @brief Add configs to the database
     * @param uris Config URIs to add
     * @param tag Source tag (e.g. "scrape", "github_bg", "harvest")
     * @return Number of newly added configs
     */
    int addConfigs(const std::set<std::string>& uris, const std::string& tag = "");
    int addConfigsWithPriority(const std::set<std::string>& uris, const std::string& tag = "",
                               int* promoted_existing = nullptr);

    /**
     * @brief Apply one typed probe round (D1/D2). Thread-safe. Duplicate run IDs, stale
     *        generations and out-of-order results are discarded; excluded outcomes
     *        (local outage, engine errors, ...) never change health or failure counters.
     */
    ApplyEffect applyProbeResult(const ProbeResult& result);

    /** @brief Unknown -> Testing when a scheduled round starts. */
    void markTestingRound(const std::string& endpoint_key);

    /** @brief Country result with timestamp / network-generation compare-and-set. */
    struct CountryUpdate {
        bool is_exit = false;          // false => server-IP country
        std::string country;           // ISO-3166 alpha-2 or "" (unknown)
        std::string ip;                // exit ip (exit only)
        std::string source;
        double at = 0.0;
        uint64_t network_generation = 0;   // exit only
        std::string geo_db_version;        // server only
        std::vector<std::string> server_ips;  // server only
    };
    bool applyCountryResult(const std::string& endpoint_key, const CountryUpdate& update);

    /**
     * @brief (C2) Visit every record under the DB lock (read-only). Keep the callback cheap and
     *        never call back into this database from it.
     */
    void forEachRecord(const std::function<void(const ConfigHealthRecord&)>& fn) const;

    /**
     * @brief (C2) Optional hook that reorders/trims the candidate pool of getUntestedBatch()
     *        (country-targeted discovery). Runs under the DB lock; must be pure and fast.
     *        pool arrives in base-priority order; the hook leaves at most batch_size records.
     */
    using BatchPrioritizer = std::function<void(std::vector<ConfigHealthRecord>& pool, int batch_size)>;
    void setBatchPrioritizer(BatchPrioritizer fn);

    /** @brief Records whose stability is Stable, ranked by the shared evaluator. */
    std::vector<ConfigHealthRecord> getRecommendedRecords(int max_count = 50);
    HealthEvaluation evaluate(const ConfigHealthRecord& rec) const;
    bool getRecord(const std::string& uri_or_key, ConfigHealthRecord* out) const;

    /// Injectable clock (UTC seconds) and thresholds, for deterministic tests / tuning.
    void setClock(ClockFn clock);
    void setThresholds(const HealthThresholds& th);
    HealthThresholds thresholds() const;
    static std::string keyFor(const std::string& uri);  // EndpointKeyV1

    struct LoadReport {
        bool ok = false;
        int loaded = 0;
        int rejected = 0;
        int source_version = 0;     // 1..4 (DB) or live-cache version
        bool migrated = false;      // legacy file rewritten as V4
        std::string backup_path;
        std::string error;          // visible failure reason (unknown version, I/O, ...)
        std::string warning;        // e.g. another instance owns the data directory (read-only)
    };
    LoadReport lastLoadReport() const;
    std::string lastSaveError() const;

    /// Read-only mode: set (sticky) when another process owns the data directory, the on-disk
    /// layout is unsupported, or a protective backup failed. All saves are refused while set.
    bool readOnly() const;
    std::string readOnlyReason() const;
    void clearReadOnly();  // explicit operator resolution

    /**
     * @brief Legacy adapter for pre-V4 callers (removed once A2 rewires them):
     *        alive -> full Pass, !alive -> attributable RemoteFailure.
     */
    void updateHealth(const std::string& uri, bool alive, float latency_ms = 0.0f,
                      const std::string& engine_used = "", bool force_dead = false,
                      bool telegram_only = false);

    /**
     * @brief Update Gemini accessibility status for a config.
     * @param uri Config URI
     * @param gemini_status -1=unknown, 0=blocked, 1=accessible
     */
    void updateGeminiStatus(const std::string& uri, int gemini_status);

    /**
     * @brief Get alive configs that need a Gemini check (never checked
     *        or checked more than gemini_interval_s ago).
     * @param gemini_interval_s Re-check interval (default 3600 = 1 hour)
     * @param max_count Max configs to return
     */
    std::vector<ConfigHealthRecord> getAliveForGeminiCheck(int gemini_interval_s = 3600,
                                                             int max_count = 20);

    /**
     * @brief Get a batch of untested or stale configs
     */
    std::vector<ConfigHealthRecord> getUntestedBatch(int batch_size = 80);

    /**
     * @brief Get healthy configs sorted by latency
     */
    std::vector<std::pair<std::string, float>> getHealthyConfigs(int max_count = 200);

    /**
     * @brief Get healthy config records with full details (timestamps, etc.)
     */
    std::vector<ConfigHealthRecord> getHealthyRecords(int max_count = 200);

    std::vector<ConfigHealthRecord> getTelegramOnlyRecords(int max_count = 200);

    /**
     * @brief Get all stored URIs
     */
    std::set<std::string> getAllUris();

    /**
     * @brief Get all config records with full health details (for UI display)
     * Sorted: alive first (by latency), then dead (by last_tested desc)
     */
    std::vector<ConfigHealthRecord> getAllRecords(int max_count = 500);

    /**
     * @brief Get alive config records only (cheap — O(alive log alive)).
     *
     * Unlike getAllRecords, this does NOT copy/sort the entire database;
     * it only touches alive records, which are typically a tiny fraction
     * of the total. Sorted by latency (telegram-only entries last), then
     * capped to max_count. Use this for live UI display instead of
     * getAllRecords when only working configs are of interest.
     */
    std::vector<ConfigHealthRecord> getAliveRecords(int max_count = 500);

    /**
     * @brief Get the last known preferred runtime engine for a specific URI
     */
    std::string getPreferredEngine(const std::string& uri);

    /**
     * @brief Get stats for a specific tag
     */
    struct TagStats {
        std::string tag;
        int total = 0;
        int alive = 0;
        float avg_latency_ms = 0.0f;
        int untested = 0;
        int needs_retest = 0;
    };
    TagStats getTagStats(const std::string& tag);

    /**
     * @brief Get overall database stats
     */
    struct Stats {
        int total = 0;
        int alive = 0;
        float avg_latency_ms = 0.0f;
        int tested_unique = 0;
        int untested_unique = 0;
        int stale_unique = 0;
        int total_tested = 0;
        int total_passed = 0;
    };
    Stats getStats();

    /**
     * @brief Remove configs that have not been alive for more than max_age_hours
     * @return Number of configs removed
     */
    int clearOlderThan(int max_age_hours);

    /**
     * @brief Remove currently alive configs from the database
     * @return Number of configs removed
     */
    int clearAlive();

    /**
     * @brief Remove a specific set of configs from the database
     * @return Number of configs removed
     */
    int removeUris(const std::set<std::string>& uris);

    /**
     * @brief Current database size
     */
    int size() const;

    /**
     * @brief Proactively evict dead/stale configs (call periodically,
     *        not just when the DB is full). Keeps memory usage lean
     *        when the database holds up to 1M entries.
     * @return Number of records evicted
     */
    int evictDead();

    /**
     * @brief Save entire database as TSV V4 (atomic temp+fsync+rename). A pre-V4 file at
     *        the path is first copied to <path>.v<N>.bak. A file with an unknown/newer
     *        version header is never overwritten.
     * @return Number of records saved, or -1 if refused/failed (see lastSaveError())
     */
    int saveToDisk(const std::string& filepath) const;

    /**
     * @brief Load database from disk (V4 strict; V3/V2/V1 are migrated: URIs re-keyed,
     *        health reset to Unknown, file atomically rewritten as V4 with a .v<N>.bak).
     * @return Number of records loaded (details in lastLoadReport())
     */
    int loadFromDisk(const std::string& filepath);

    /**
     * @brief Save only alive/live configs to a separate cache file.
     *        This file survives restarts and is re-merged into the DB
     *        on startup so live connections are never lost.
     * @param filepath Path to live cache file
     * @return Number of live records saved
     */
    int saveLiveToDisk(const std::string& filepath) const;

    /**
     * @brief Load live configs from cache and merge into DB.
     *        Alive configs from the cache are marked alive and given
     *        priority for revalidation. Does NOT overwrite existing records.
     * @param filepath Path to live cache file
     * @return Number of live records loaded
     */
    int loadLiveFromDisk(const std::string& filepath);

    /**
     * @brief Get alive configs that need revalidation (last tested
     *        more than revalidate_interval_s ago). Called by the
     *        validator to re-check live connections every 30 min.
     * @param revalidate_interval_s Max age in seconds (default 1800 = 30 min)
     * @param max_count Max configs to return
     * @return Vector of alive records needing revalidation
     */
    std::vector<ConfigHealthRecord> getAliveForRevalidation(int revalidate_interval_s = 1800,
                                                             int max_count = 100);

    /**
     * @brief Remove configs from the live list that have been dead for
     *        more than dead_ttl_s seconds. These are configs that were
     *        once alive but have been continuously failing for 3 days.
     *        They are evicted from the DB entirely.
     * @param dead_ttl_s TTL in seconds (default 259200 = 3 days)
     * @return Number of records removed
     */
    int removeDeadLive(int dead_ttl_s = 259200);

private:
    int max_size_;
    std::map<std::string, ConfigHealthRecord> db_;  // keyed by EndpointKeyV1
    mutable std::mutex mutex_;
    ClockFn clock_;
    BatchPrioritizer batch_prioritizer_;   // guarded by mutex_ (C2)
    HealthThresholds th_;
    LoadReport load_report_;
    std::string save_error_;
    mutable bool read_only_ = false;
    mutable std::string read_only_reason_;
    mutable size_t last_written_hash_ = 0;  // hash of the bytes this instance last published (DB file)

    bool acquireWriteAccessLocked(const std::string& filepath, std::string* err) const;
    bool checkDestinationLocked(const std::string& filepath, bool is_live, std::string* err) const;
    void setReadOnlyLocked(const std::string& why) const;

    void evictStale();
    ApplyEffect applyLocked(ConfigHealthRecord& rec, const ProbeResult& r);
    RankKey rankKeyLocked(const ConfigHealthRecord& rec, double now) const;
    void sortRankedLocked(std::vector<ConfigHealthRecord>& v, double now) const;
    // Merge a parsed record (load paths); `overwrite_newer` governs live-cache merging.
    bool mergeLoadedLocked(ConfigHealthRecord&& rec, bool legacy_source);
};

/**
 * @brief Continuous background config validator
 * 
 * Takes batches from ConfigDatabase and performs quick TCP
 * connectivity checks to maintain up-to-date health info.
 */
class ContinuousValidator {
public:
    explicit ContinuousValidator(ConfigDatabase& db, int batch_size = 80,
                                 int timeout_s = 15, int max_concurrent = 10);
    ~ContinuousValidator() = default;

    /**
     * @brief Run one validation batch (blocking)
     * @return pair of (tested, passed)
     */
    std::pair<int, int> validateBatch();

    /**
     * @brief Get cumulative stats
     */
    struct ValidatorStats {
        int total_tested = 0;
        int total_passed = 0;
        ConfigDatabase::Stats db;
    };
    ValidatorStats getStats() const;

private:
    ConfigDatabase& db_;
    int batch_size_;
    int timeout_s_;
    int max_concurrent_;
    std::atomic<int> total_tested_{0};
    std::atomic<int> total_passed_{0};
};

} // namespace network
} // namespace hunter
