#pragma once

#include <string>
#include <vector>
#include <set>
#include <cstdint>

namespace hunter {

class SmartCache {
public:
    SmartCache(const std::string& cache_file = "subscriptions_cache.txt",
               const std::string& working_cache_file = "working_configs_cache.txt");

    void set_base_dir(const std::string& dir);
    int save_configs(const std::vector<std::string>& configs, bool working = false);
    std::set<std::string> load_cached_configs(int max_count = 1000, bool working_only = false);
    void record_failure();
    bool should_use_cache() const;
    int get_failure_count() const;

private:
    std::string resolve_path(const std::string& filename) const;

    std::string cache_file_;
    std::string working_cache_file_;
    std::string base_dir_;
    int64_t last_successful_fetch_ = 0;
    int consecutive_failures_ = 0;
};

class ResilientHeartbeat {
public:
    explicit ResilientHeartbeat();

    bool is_connected() const;
    int64_t time_since_heartbeat() const;
    void mark_connected();
    void mark_disconnected();
    bool should_attempt_reconnect();
    void reset();

private:
    int64_t last_heartbeat_;
    bool is_connected_ = false;
    int reconnect_attempts_ = 0;
    int max_reconnect_attempts_ = 5;
};

} // namespace hunter
