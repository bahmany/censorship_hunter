#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>

namespace hunter {
namespace orchestrator {

struct CleanupFolderRule {
    std::string path;
    int max_files = 100;
    int max_age_hours = 24;
    std::string pattern;
    bool recursive = false;
};

struct CleanupStats {
    int files_scanned = 0;
    int files_deleted = 0;
    int64_t bytes_freed = 0;
    double last_run_ts = 0.0;
    double duration_ms = 0.0;
    std::map<std::string, int> per_folder_deleted;
    std::map<std::string, int> per_folder_remaining;
    std::map<std::string, int64_t> per_folder_bytes;
    std::vector<std::string> recent_deletions;
};

class RuntimeCleanupManager {
public:
    explicit RuntimeCleanupManager(const std::string& runtime_dir = "runtime");
    ~RuntimeCleanupManager();

    RuntimeCleanupManager(const RuntimeCleanupManager&) = delete;
    RuntimeCleanupManager& operator=(const RuntimeCleanupManager&) = delete;

    void setRuntimeDir(const std::string& dir);
    std::string runtimeDir() const;

    void addFolderRule(const CleanupFolderRule& rule);
    void clearFolderRules();

    CleanupStats runCleanup();
    CleanupStats getStats() const;

    void startPeriodicCleanup(int interval_seconds = 3600);
    void stopPeriodicCleanup();
    bool isPeriodicRunning() const { return periodic_running_.load(); }

    std::string buildStatsJson() const;

    struct FolderInfo {
        std::string path;
        int file_count = 0;
        int64_t total_bytes = 0;
        int oldest_file_age_hours = 0;
    };
    std::vector<FolderInfo> getFolderInfo() const;

private:
    std::string runtime_dir_;
    std::vector<CleanupFolderRule> rules_;
    mutable std::mutex rules_mutex_;
    mutable std::mutex stats_mutex_;
    CleanupStats last_stats_;

    std::atomic<bool> periodic_running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread periodic_thread_;
    int interval_seconds_ = 3600;

    void periodicLoop();
    CleanupStats cleanupFolder(const CleanupFolderRule& rule);
    int64_t getFileSize(const std::string& path) const;
    double getFileModTime(const std::string& path) const;
    bool deleteFile(const std::string& path);
    std::vector<std::string> listFiles(const std::string& dir, const std::string& pattern, bool recursive) const;
};

} // namespace orchestrator
} // namespace hunter
