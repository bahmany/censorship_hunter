#include "cache.h"
#include "core/utils.h"

namespace hunter {

// ---------- SmartCache ----------

SmartCache::SmartCache(const std::string& cache_file, const std::string& working_cache_file)
    : cache_file_(cache_file), working_cache_file_(working_cache_file) {}

void SmartCache::set_base_dir(const std::string& dir) {
    base_dir_ = dir;
}

std::string SmartCache::resolve_path(const std::string& filename) const {
    if (base_dir_.empty()) return filename;
    return base_dir_ + "/" + filename;
}

int SmartCache::save_configs(const std::vector<std::string>& configs, bool working) {
    std::string target = working ? resolve_path(working_cache_file_) : resolve_path(cache_file_);
    int appended = append_unique_lines(target, configs);
    if (appended > 0) {
        last_successful_fetch_ = now_ts();
        consecutive_failures_ = 0;
    }
    return appended;
}

std::set<std::string> SmartCache::load_cached_configs(int max_count, bool working_only) {
    std::set<std::string> configs;
    std::vector<std::string> sources;

    if (working_only) {
        sources.push_back(resolve_path(working_cache_file_));
    } else {
        sources.push_back(resolve_path(cache_file_));
        sources.push_back(resolve_path(working_cache_file_));
    }

    for (const auto& path : sources) {
        auto lines = read_lines(path);
        // Take last max_count lines
        size_t start = (lines.size() > static_cast<size_t>(max_count))
                       ? lines.size() - max_count : 0;
        for (size_t i = start; i < lines.size(); ++i) {
            if (!lines[i].empty() && lines[i].find("://") != std::string::npos) {
                configs.insert(lines[i]);
            }
        }
    }

    return configs;
}

void SmartCache::record_failure() {
    consecutive_failures_++;
}

bool SmartCache::should_use_cache() const {
    return consecutive_failures_ >= 2;
}

int SmartCache::get_failure_count() const {
    return consecutive_failures_;
}

// ---------- ResilientHeartbeat ----------

ResilientHeartbeat::ResilientHeartbeat()
    : last_heartbeat_(now_ts()) {}

bool ResilientHeartbeat::is_connected() const {
    return is_connected_;
}

int64_t ResilientHeartbeat::time_since_heartbeat() const {
    return now_ts() - last_heartbeat_;
}

void ResilientHeartbeat::mark_connected() {
    is_connected_ = true;
    last_heartbeat_ = now_ts();
    reconnect_attempts_ = 0;
}

void ResilientHeartbeat::mark_disconnected() {
    is_connected_ = false;
}

bool ResilientHeartbeat::should_attempt_reconnect() {
    if (reconnect_attempts_ >= max_reconnect_attempts_) return false;
    reconnect_attempts_++;
    return true;
}

void ResilientHeartbeat::reset() {
    is_connected_ = false;
    reconnect_attempts_ = 0;
    last_heartbeat_ = now_ts();
}

} // namespace hunter
