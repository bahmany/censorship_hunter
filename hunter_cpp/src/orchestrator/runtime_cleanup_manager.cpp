#include "orchestrator/runtime_cleanup_manager.h"
#include "core/utils.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <iostream>

namespace hunter {
namespace orchestrator {

RuntimeCleanupManager::RuntimeCleanupManager(const std::string& runtime_dir)
    : runtime_dir_(runtime_dir) {
    if (runtime_dir_.empty()) runtime_dir_ = "runtime";
    utils::mkdirRecursive(runtime_dir_);

    addFolderRule({runtime_dir_ + "/engine_tmp", 100, 6, "", false});
    addFolderRule({runtime_dir_ + "/tmp", 100, 12, "", false});
    addFolderRule({runtime_dir_ + "/cache", 100, 48, "", false});
    addFolderRule({runtime_dir_ + "/tests", 100, 2, "", true});
    addFolderRule({runtime_dir_, 0, 0, "test_config_*.json", false});
    addFolderRule({runtime_dir_, 0, 0, "test_config_*.yaml", false});
    addFolderRule({runtime_dir_, 0, 0, "test_config_*.txt", false});
    addFolderRule({runtime_dir_, 0, 0, "*.tmp", false});
    addFolderRule({runtime_dir_, 0, 0, "*.temp", false});
    addFolderRule({runtime_dir_, 0, 0, "*_export_*.json", false});
    addFolderRule({runtime_dir_, 0, 0, "*_export_*.txt", false});
}

RuntimeCleanupManager::~RuntimeCleanupManager() {
    stopPeriodicCleanup();
}

void RuntimeCleanupManager::setRuntimeDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    runtime_dir_ = dir;
    if (runtime_dir_.empty()) runtime_dir_ = "runtime";
    utils::mkdirRecursive(runtime_dir_);
}

std::string RuntimeCleanupManager::runtimeDir() const {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    return runtime_dir_;
}

void RuntimeCleanupManager::addFolderRule(const CleanupFolderRule& rule) {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    rules_.push_back(rule);
}

void RuntimeCleanupManager::clearFolderRules() {
    std::lock_guard<std::mutex> lock(rules_mutex_);
    rules_.clear();
}

std::vector<std::string> RuntimeCleanupManager::listFiles(const std::string& dir,
                                                           const std::string& pattern,
                                                           bool recursive) const {
    std::vector<std::string> result;
    DIR* d = opendir(dir.c_str());
    if (!d) return result;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string fullpath = dir + "/" + entry->d_name;

        struct stat st;
        if (stat(fullpath.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode) && recursive) {
            auto sub = listFiles(fullpath, pattern, true);
            result.insert(result.end(), sub.begin(), sub.end());
        } else if (S_ISREG(st.st_mode)) {
            if (pattern.empty()) {
                result.push_back(fullpath);
            } else {
                std::string fname = entry->d_name;
                if (pattern[0] == '*' && pattern.size() > 1) {
                    std::string suffix = pattern.substr(1);
                    if (fname.size() >= suffix.size() &&
                        fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        result.push_back(fullpath);
                    }
                } else if (pattern.back() == '*' && pattern.size() > 1) {
                    std::string prefix = pattern.substr(0, pattern.size() - 1);
                    if (fname.size() >= prefix.size() &&
                        fname.compare(0, prefix.size(), prefix) == 0) {
                        result.push_back(fullpath);
                    }
                } else if (pattern.find('*') != std::string::npos &&
                           pattern.find('*') != 0 &&
                           pattern.find('*') != pattern.size() - 1) {
                    size_t star_pos = pattern.find('*');
                    std::string prefix = pattern.substr(0, star_pos);
                    std::string suffix = pattern.substr(star_pos + 1);
                    if (fname.size() >= prefix.size() + suffix.size() &&
                        fname.compare(0, prefix.size(), prefix) == 0 &&
                        fname.compare(fname.size() - suffix.size(), suffix.size(), suffix) == 0) {
                        result.push_back(fullpath);
                    }
                } else if (fname == pattern) {
                    result.push_back(fullpath);
                }
            }
        }
    }
    closedir(d);
    return result;
}

int64_t RuntimeCleanupManager::getFileSize(const std::string& path) const {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return (int64_t)st.st_size;
    }
    return 0;
}

double RuntimeCleanupManager::getFileModTime(const std::string& path) const {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return (double)st.st_mtime;
    }
    return 0.0;
}

bool RuntimeCleanupManager::deleteFile(const std::string& path) {
    return ::remove(path.c_str()) == 0;
}

CleanupStats RuntimeCleanupManager::cleanupFolder(const CleanupFolderRule& rule) {
    CleanupStats stats;
    stats.per_folder_deleted[rule.path] = 0;
    stats.per_folder_remaining[rule.path] = 0;
    stats.per_folder_bytes[rule.path] = 0;

    if (!utils::fileExists(rule.path) && rule.pattern.empty()) {
        return stats;
    }

    std::string search_dir = rule.path;
    if (!rule.pattern.empty() && rule.path == runtime_dir_) {
        search_dir = runtime_dir_;
    }

    auto files = listFiles(search_dir, rule.pattern, rule.recursive);
    stats.files_scanned += (int)files.size();

    double now = utils::nowTimestamp();
    struct FileEntry {
        std::string path;
        double mod_time;
        int64_t size;
    };

    std::vector<FileEntry> entries;
    for (const auto& f : files) {
        FileEntry fe;
        fe.path = f;
        fe.mod_time = getFileModTime(f);
        fe.size = getFileSize(f);
        entries.push_back(fe);
    }

    std::sort(entries.begin(), entries.end(),
              [](const FileEntry& a, const FileEntry& b) {
                  return a.mod_time < b.mod_time;
              });

    int remaining = (int)entries.size();
    for (const auto& fe : entries) {
        bool should_delete = false;
        std::string reason;

        double age_hours = (now - fe.mod_time) / 3600.0;

        if (rule.max_age_hours > 0 && age_hours > rule.max_age_hours) {
            should_delete = true;
            reason = "expired";
        }

        if (!should_delete && rule.max_files > 0 && remaining > rule.max_files) {
            should_delete = true;
            reason = "over_limit";
        }

        if (should_delete) {
            if (deleteFile(fe.path)) {
                stats.files_deleted++;
                stats.bytes_freed += fe.size;
                stats.per_folder_deleted[rule.path]++;
                remaining--;

                if (stats.recent_deletions.size() < 50) {
                    std::string fname = fe.path;
                    size_t pos = fname.find_last_of('/');
                    if (pos != std::string::npos) fname = fname.substr(pos + 1);
                    stats.recent_deletions.push_back(fname + " (" + reason + ")");
                }
            }
        } else {
            stats.per_folder_remaining[rule.path]++;
            stats.per_folder_bytes[rule.path] += fe.size;
        }
    }

    return stats;
}

CleanupStats RuntimeCleanupManager::runCleanup() {
    CleanupStats combined;
    double start = utils::nowTimestamp();

    std::vector<CleanupFolderRule> rules_copy;
    {
        std::lock_guard<std::mutex> lock(rules_mutex_);
        rules_copy = rules_;
    }

    for (const auto& rule : rules_copy) {
        auto s = cleanupFolder(rule);
        combined.files_scanned += s.files_scanned;
        combined.files_deleted += s.files_deleted;
        combined.bytes_freed += s.bytes_freed;
        for (const auto& kv : s.per_folder_deleted)
            combined.per_folder_deleted[kv.first] += kv.second;
        for (const auto& kv : s.per_folder_remaining)
            combined.per_folder_remaining[kv.first] += kv.second;
        for (const auto& kv : s.per_folder_bytes)
            combined.per_folder_bytes[kv.first] += kv.second;
        for (const auto& d : s.recent_deletions)
            combined.recent_deletions.push_back(d);
    }

    combined.last_run_ts = start;
    combined.duration_ms = (utils::nowTimestamp() - start) * 1000.0;

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        last_stats_ = combined;
    }

    if (combined.files_deleted > 0) {
        std::cout << "[Cleanup] Deleted " << combined.files_deleted
                  << " files, freed " << (combined.bytes_freed / 1024) << " KB"
                  << " in " << combined.duration_ms << " ms" << std::endl;
    }

    return combined;
}

CleanupStats RuntimeCleanupManager::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return last_stats_;
}

void RuntimeCleanupManager::startPeriodicCleanup(int interval_seconds) {
    if (periodic_running_.load()) return;
    interval_seconds_ = interval_seconds;
    stop_flag_ = false;
    periodic_running_ = true;
    periodic_thread_ = std::thread(&RuntimeCleanupManager::periodicLoop, this);
}

void RuntimeCleanupManager::stopPeriodicCleanup() {
    stop_flag_ = true;
    if (periodic_thread_.joinable()) {
        periodic_thread_.join();
    }
    periodic_running_ = false;
}

void RuntimeCleanupManager::periodicLoop() {
    while (!stop_flag_.load()) {
        for (int i = 0; i < interval_seconds_ && !stop_flag_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (stop_flag_.load()) break;
        try {
            runCleanup();
        } catch (...) {}
    }
}

std::vector<RuntimeCleanupManager::FolderInfo> RuntimeCleanupManager::getFolderInfo() const {
    std::vector<FolderInfo> result;
    std::vector<CleanupFolderRule> rules_copy;
    {
        std::lock_guard<std::mutex> lock(rules_mutex_);
        rules_copy = rules_;
    }

    double now = utils::nowTimestamp();
    for (const auto& rule : rules_copy) {
        FolderInfo info;
        info.path = rule.path;

        auto files = listFiles(rule.path, rule.pattern, rule.recursive);
        info.file_count = (int)files.size();

        double oldest = now;
        for (const auto& f : files) {
            int64_t sz = getFileSize(f);
            info.total_bytes += sz;
            double mt = getFileModTime(f);
            if (mt > 0 && mt < oldest) oldest = mt;
        }

        if (info.file_count > 0) {
            info.oldest_file_age_hours = (int)((now - oldest) / 3600.0);
        }

        result.push_back(info);
    }
    return result;
}

std::string RuntimeCleanupManager::buildStatsJson() const {
    CleanupStats stats;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats = last_stats_;
    }

    auto folder_infos = getFolderInfo();

    std::ostringstream folders_json;
    folders_json << "[";
    bool first = true;
    for (const auto& fi : folder_infos) {
        if (!first) folders_json << ",";
        first = false;
        utils::JsonBuilder fj;
        fj.add("path", fi.path)
          .add("file_count", fi.file_count)
          .add("total_bytes", (double)fi.total_bytes)
          .add("oldest_file_age_hours", fi.oldest_file_age_hours);
        folders_json << fj.build();
    }
    folders_json << "]";

    std::ostringstream deletions_json;
    deletions_json << "[";
    bool dfirst = true;
    for (const auto& d : stats.recent_deletions) {
        if (!dfirst) deletions_json << ",";
        dfirst = false;
        std::string escaped;
        escaped.reserve(d.size() + 8);
        for (char c : d) {
            switch (c) {
                case '\\': escaped += "\\\\"; break;
                case '"': escaped += "\\\""; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default: escaped.push_back(c); break;
            }
        }
        deletions_json << "\"" << escaped << "\"";
    }
    deletions_json << "]";

    utils::JsonBuilder root;
    root.add("last_run_ts", stats.last_run_ts)
        .add("duration_ms", stats.duration_ms)
        .add("files_scanned", stats.files_scanned)
        .add("files_deleted", stats.files_deleted)
        .add("bytes_freed", (double)stats.bytes_freed)
        .addRaw("folders", folders_json.str())
        .addRaw("recent_deletions", deletions_json.str());

    return root.build();
}

} // namespace orchestrator
} // namespace hunter
