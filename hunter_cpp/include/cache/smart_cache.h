#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>

namespace hunter {
namespace cache {

/**
 * @brief Smart config cache with file persistence
 * 
 * Manages cached proxy configs with working/all separation,
 * deduplication, and age-based staleness detection.
 */
class SmartCache {
public:
    explicit SmartCache(const std::string& cache_dir = "runtime");
    ~SmartCache() = default;

    /**
     * @brief Save configs to cache
     * @param configs Config URIs
     * @param working If true, save to working cache; else to all cache
     */
    void saveConfigs(const std::vector<std::string>& configs, bool working = false);

    /**
     * @brief Load cached configs
     * @param max_count Max configs to return
     * @param working_only Only load from working cache
     */
    std::vector<std::string> loadCachedConfigs(int max_count = 500, bool working_only = true);

    /**
     * @brief Get cache statistics
     */
    struct CacheStats {
        int all_count = 0;
        int working_count = 0;
        double all_age_hours = 0.0;
        double working_age_hours = 0.0;
    };
    CacheStats getStats() const;

    /**
     * @brief Clear all caches
     */
    void clear();

private:
    std::string cache_dir_;
    std::string allCachePath() const;
    std::string workingCachePath() const;
    mutable std::mutex mutex_;
};

} // namespace cache
} // namespace hunter
