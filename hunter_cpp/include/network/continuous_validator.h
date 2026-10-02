#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <atomic>

#include "core/models.h"

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
     * @brief Update health status after a test
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
     * @brief Save entire database to disk (JSON lines format)
     * @param filepath Path to save file
     * @return Number of records saved
     */
    int saveToDisk(const std::string& filepath) const;

    /**
     * @brief Load database from disk
     * @param filepath Path to saved file
     * @return Number of records loaded
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
    std::map<std::string, ConfigHealthRecord> db_;  // keyed by URI hash
    mutable std::mutex mutex_;

    std::string hashUri(const std::string& uri) const;
    void evictStale();
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
     * @brief Run one validation batch using XRay ping test (blocking)
     * @return pair of (tested, passed)
     */
    std::pair<int, int> validateBatchWithXray();

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
    std::atomic<int> batch_port_offset_{0};  // rotates base port to avoid conflicts

    bool quickCheck(const std::string& uri);
    bool pingTestWithXray(const std::string& uri);
};

} // namespace network
} // namespace hunter
