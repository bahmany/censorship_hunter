#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include <functional>
#include <mutex>

namespace hunter {

using ConfigValue = std::variant<long long, double, bool, std::string, std::vector<std::string>>;

class HunterConfig {
public:
    explicit HunterConfig(const std::string& secrets_file = "hunter_secrets.env");

    long long get_int(const std::string& key, long long default_val = 0) const;
    double get_double(const std::string& key, double default_val = 0.0) const;
    bool get_bool(const std::string& key, bool default_val = false) const;
    std::string get_string(const std::string& key, const std::string& default_val = "") const;
    std::vector<std::string> get_string_list(const std::string& key) const;

    void set_int(const std::string& key, long long value);
    void set_double(const std::string& key, double value);
    void set_bool(const std::string& key, bool value);
    void set_string(const std::string& key, const std::string& value);
    void set_string_list(const std::string& key, const std::vector<std::string>& value);

    std::vector<std::string> validate() const;

    void set_files_dir(const std::string& dir);
    std::string get_files_dir() const;

    void set_env(const std::string& key, const std::string& value);
    std::string get_env(const std::string& key, const std::string& default_val = "") const;

private:
    void load_default_config();
    void load_env_file(const std::string& path);
    void load_from_environment();

    mutable std::mutex mutex_;
    std::string secrets_file_;
    std::string files_dir_;
    std::unordered_map<std::string, ConfigValue> config_;
    std::unordered_map<std::string, std::string> env_;
};

} // namespace hunter
