#include "cache/smart_cache.h"
#include "core/utils.h"

#include <fstream>
#include <algorithm>
#include <set>
#include <filesystem>

namespace fs = std::filesystem;

namespace hunter {
namespace cache {

SmartCache::SmartCache(const std::string& cache_dir) : cache_dir_(cache_dir) {
    utils::mkdirRecursive(cache_dir_);
}

std::string SmartCache::allCachePath() const { return cache_dir_ + "/HUNTER_all_cache.txt"; }
std::string SmartCache::workingCachePath() const { return cache_dir_ + "/HUNTER_working_cache.txt"; }

void SmartCache::saveConfigs(const std::vector<std::string>& configs, bool working) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = working ? workingCachePath() : allCachePath();
    std::set<std::string> existing;
    {
        std::ifstream f(path);
        std::string line;
        while (std::getline(f, line)) {
            line = utils::trim(line);
            if (!line.empty()) existing.insert(line);
        }
    }
    std::ofstream f(path, std::ios::app);
    for (const auto& c : configs) {
        if (!c.empty() && existing.find(c) == existing.end()) {
            f << c << "\n";
            existing.insert(c);
        }
    }
}

std::vector<std::string> SmartCache::loadCachedConfigs(int max_count, bool working_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string path = working_only ? workingCachePath() : allCachePath();
    auto lines = utils::readLines(path);
    if ((int)lines.size() > max_count) {
        lines.erase(lines.begin(), lines.begin() + ((int)lines.size() - max_count));
    }
    return lines;
}

SmartCache::CacheStats SmartCache::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    CacheStats s;
    auto fileAgeHours = [](const std::string& path) -> double {
        try {
            auto mod = fs::last_write_time(path);
            auto now = fs::file_time_type::clock::now();
            auto age = std::chrono::duration<double>(now - mod).count();
            return age / 3600.0;
        } catch (...) { return 0.0; }
    };
    try {
        if (fs::exists(allCachePath())) {
            s.all_count = (int)utils::readLines(allCachePath()).size();
            s.all_age_hours = fileAgeHours(allCachePath());
        }
    } catch (...) {}
    try {
        if (fs::exists(workingCachePath())) {
            s.working_count = (int)utils::readLines(workingCachePath()).size();
            s.working_age_hours = fileAgeHours(workingCachePath());
        }
    } catch (...) {}
    return s;
}

void SmartCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    try { fs::remove(allCachePath()); } catch (...) {}
    try { fs::remove(workingCachePath()); } catch (...) {}
}

} // namespace cache
} // namespace hunter
