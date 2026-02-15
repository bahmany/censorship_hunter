#include "config.h"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "HunterConfig"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace hunter {

HunterConfig::HunterConfig(const std::string& secrets_file)
    : secrets_file_(secrets_file) {
    load_default_config();
    if (!secrets_file_.empty()) {
        load_env_file(secrets_file_);
    }
    load_from_environment();
}

void HunterConfig::load_default_config() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string runtime_dir = files_dir_.empty() ? "runtime" : files_dir_ + "/runtime";

    // Telegram Configuration (user must provide via env file)
    config_["api_id"] = 0;
    config_["api_hash"] = std::string("");
    config_["phone"] = std::string("");
    config_["bot_token"] = std::string("");
    config_["report_channel"] = 0LL;
    config_["chat_id"] = std::string("");
    config_["session_name"] = std::string("session");
    config_["telegram_limit"] = 50;
    config_["bot_username"] = std::string("");

    // Target Channels
    config_["targets"] = std::vector<std::string>{
        "v2rayngvpn", "mitivpn", "proxymtprotoir", "Porteqal3",
        "v2ray_configs_pool", "vmessorg", "V2rayNGn", "v2ray_swhil",
        "VmessProtocol", "PrivateVPNs", "DirectVPN", "v2rayNG_Matsuri",
        "FalconPolV2rayNG", "ShadowSocks_s", "napsternetv_config",
        "VlessConfig", "iP_CF", "ConfigsHUB"
    };

    // Paths
    config_["xray_path"] = std::string("");
    config_["state_file"] = runtime_dir + "/HUNTER_state.json";
    config_["raw_file"] = runtime_dir + "/HUNTER_raw.txt";
    config_["gold_file"] = runtime_dir + "/HUNTER_gold.txt";
    config_["silver_file"] = runtime_dir + "/HUNTER_silver.txt";
    config_["bridge_pool_file"] = runtime_dir + "/HUNTER_bridge_pool.txt";
    config_["validated_jsonl"] = runtime_dir + "/HUNTER_validated.jsonl";

    // Testing Configuration
    config_["test_url"] = std::string("https://www.cloudflare.com/cdn-cgi/trace");
    config_["google_test_url"] = std::string("https://www.google.com/generate_204");
    config_["scan_limit"] = 50;
    config_["latest_total"] = 500;
    config_["max_total"] = 3000;
    config_["npvt_scan_limit"] = 50;
    config_["max_workers"] = 50;
    config_["timeout_seconds"] = 10;

    // Timing Configuration
    config_["sleep_seconds"] = 300;
    config_["cleanup_interval"] = 24 * 3600;
    config_["recursive_ratio"] = 0.15;

    // Bridge Configuration
    config_["max_bridges"] = 8;
    config_["bridge_base"] = 11808;
    config_["bench_base"] = 12808;

    // MultiProxy Configuration
    config_["multiproxy_port"] = 10808;
    config_["multiproxy_backends"] = 5;
    config_["multiproxy_health_interval"] = 60;
    config_["gemini_balancer_enabled"] = false;
    config_["gemini_port"] = 10809;

    // Connection Configuration
    config_["connect_tries"] = 4;

    // Feature Flags
    config_["adee_enabled"] = true;
    config_["iran_fragment_enabled"] = false;
    config_["gateway_enabled"] = false;
    config_["web_server_enabled"] = true;
    config_["web_server_port"] = 8080;

    // Gateway Configuration
    config_["gateway_socks_port"] = 10808;
    config_["gateway_http_port"] = 10809;
    config_["gateway_dns_port"] = 53;
}

void HunterConfig::load_env_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Trim
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        if (line.empty() || line[0] == '#') continue;

        std::string key, value;

        // Handle PowerShell env files: $env:KEY = VALUE
        static const std::regex ps_regex(R"(^\$env:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$)");
        std::smatch match;
        if (std::regex_match(line, match, ps_regex)) {
            key = match[1].str();
            value = match[2].str();
        } else {
            // Handle standard KEY=VALUE
            auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;
            key = line.substr(0, eq_pos);
            value = line.substr(eq_pos + 1);
            // Trim key and value
            auto trim = [](std::string& s) {
                auto a = s.find_first_not_of(" \t");
                auto b = s.find_last_not_of(" \t");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trim(key);
            trim(value);
        }

        if (key.empty()) continue;

        // Remove surrounding quotes
        if (value.size() >= 2) {
            if ((value.front() == '"' && value.back() == '"') ||
                (value.front() == '\'' && value.back() == '\'')) {
                value = value.substr(1, value.size() - 2);
            }
        }

        // Only set if not already in env
        if (env_.find(key) == env_.end()) {
            env_[key] = value;
        }
    }
}

void HunterConfig::load_from_environment() {
    std::lock_guard<std::mutex> lock(mutex_);

    struct EnvMapping {
        std::string env_key;
        std::string config_key;
        enum Type { INT, DOUBLE, STRING, BOOL } type;
    };

    static const std::vector<EnvMapping> mappings = {
        {"api_id", "api_id", EnvMapping::INT},
        {"HUNTER_API_ID", "api_id", EnvMapping::INT},
        {"TELEGRAM_API_ID", "api_id", EnvMapping::INT},
        {"api_hash", "api_hash", EnvMapping::STRING},
        {"HUNTER_API_HASH", "api_hash", EnvMapping::STRING},
        {"TELEGRAM_API_HASH", "api_hash", EnvMapping::STRING},
        {"phone", "phone", EnvMapping::STRING},
        {"HUNTER_PHONE", "phone", EnvMapping::STRING},
        {"TELEGRAM_PHONE", "phone", EnvMapping::STRING},
        {"bot_token", "bot_token", EnvMapping::STRING},
        {"TOKEN", "bot_token", EnvMapping::STRING},
        {"TELEGRAM_BOT_TOKEN", "bot_token", EnvMapping::STRING},
        {"chat_id", "chat_id", EnvMapping::STRING},
        {"CHAT_ID", "chat_id", EnvMapping::STRING},
        {"TELEGRAM_GROUP_ID", "chat_id", EnvMapping::STRING},
        {"report_channel", "report_channel", EnvMapping::INT},
        {"session_name", "session_name", EnvMapping::STRING},
        {"HUNTER_SESSION", "session_name", EnvMapping::STRING},
        {"TELEGRAM_SESSION", "session_name", EnvMapping::STRING},
        {"telegram_limit", "telegram_limit", EnvMapping::INT},
        {"HUNTER_TELEGRAM_LIMIT", "telegram_limit", EnvMapping::INT},
        {"HUNTER_BOT_USERNAME", "bot_username", EnvMapping::STRING},
        {"xray_path", "xray_path", EnvMapping::STRING},
        {"HUNTER_XRAY_PATH", "xray_path", EnvMapping::STRING},
        {"HUNTER_TEST_URL", "test_url", EnvMapping::STRING},
        {"HUNTER_GOOGLE_TEST_URL", "google_test_url", EnvMapping::STRING},
        {"HUNTER_SCAN_LIMIT", "scan_limit", EnvMapping::INT},
        {"HUNTER_LATEST_URIS", "latest_total", EnvMapping::INT},
        {"HUNTER_MAX_CONFIGS", "max_total", EnvMapping::INT},
        {"HUNTER_NPVT_SCAN", "npvt_scan_limit", EnvMapping::INT},
        {"HUNTER_WORKERS", "max_workers", EnvMapping::INT},
        {"HUNTER_TEST_TIMEOUT", "timeout_seconds", EnvMapping::INT},
        {"HUNTER_SLEEP", "sleep_seconds", EnvMapping::INT},
        {"HUNTER_CLEANUP", "cleanup_interval", EnvMapping::INT},
        {"HUNTER_RECURSIVE_RATIO", "recursive_ratio", EnvMapping::DOUBLE},
        {"HUNTER_MAX_BRIDGES", "max_bridges", EnvMapping::INT},
        {"HUNTER_BRIDGE_BASE", "bridge_base", EnvMapping::INT},
        {"HUNTER_BENCH_BASE", "bench_base", EnvMapping::INT},
        {"HUNTER_MULTIPROXY_PORT", "multiproxy_port", EnvMapping::INT},
        {"HUNTER_MULTIPROXY_BACKENDS", "multiproxy_backends", EnvMapping::INT},
        {"HUNTER_MULTIPROXY_HEALTH_INTERVAL", "multiproxy_health_interval", EnvMapping::INT},
        {"HUNTER_GEMINI_BALANCER", "gemini_balancer_enabled", EnvMapping::BOOL},
        {"HUNTER_GEMINI_PORT", "gemini_port", EnvMapping::INT},
        {"HUNTER_CONNECT_TRIES", "connect_tries", EnvMapping::INT},
        {"ADEE_ENABLED", "adee_enabled", EnvMapping::BOOL},
        {"IRAN_FRAGMENT_ENABLED", "iran_fragment_enabled", EnvMapping::BOOL},
        {"GATEWAY_ENABLED", "gateway_enabled", EnvMapping::BOOL},
        {"HUNTER_WEB_SERVER", "web_server_enabled", EnvMapping::BOOL},
        {"HUNTER_WEB_PORT", "web_server_port", EnvMapping::INT},
        {"GATEWAY_SOCKS_PORT", "gateway_socks_port", EnvMapping::INT},
        {"GATEWAY_HTTP_PORT", "gateway_http_port", EnvMapping::INT},
        {"GATEWAY_DNS_PORT", "gateway_dns_port", EnvMapping::INT},
    };

    for (const auto& m : mappings) {
        auto it = env_.find(m.env_key);
        if (it == env_.end()) continue;
        const std::string& val = it->second;
        try {
            switch (m.type) {
                case EnvMapping::INT:
                    config_[m.config_key] = std::stoll(val);
                    break;
                case EnvMapping::DOUBLE:
                    config_[m.config_key] = std::stod(val);
                    break;
                case EnvMapping::STRING:
                    config_[m.config_key] = val;
                    break;
                case EnvMapping::BOOL: {
                    std::string lower = val;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    config_[m.config_key] = (lower == "true");
                    break;
                }
            }
        } catch (const std::exception& e) {
            LOGW("Invalid value for %s: %s", m.env_key.c_str(), val.c_str());
        }
    }

    // Handle HUNTER_TARGETS
    auto targets_it = env_.find("HUNTER_TARGETS");
    if (targets_it != env_.end() && !targets_it->second.empty()) {
        std::vector<std::string> targets;
        std::istringstream ss(targets_it->second);
        std::string token;
        while (std::getline(ss, token, ',')) {
            auto a = token.find_first_not_of(" \t");
            auto b = token.find_last_not_of(" \t");
            if (a != std::string::npos) {
                targets.push_back(token.substr(a, b - a + 1));
            }
        }
        if (!targets.empty()) {
            config_["targets"] = targets;
        }
    }
}

long long HunterConfig::get_int(const std::string& key, long long default_val) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it == config_.end()) return default_val;
    if (auto* p = std::get_if<long long>(&it->second)) return *p;
    return default_val;
}

double HunterConfig::get_double(const std::string& key, double default_val) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it == config_.end()) return default_val;
    if (auto* p = std::get_if<double>(&it->second)) return *p;
    if (auto* p = std::get_if<long long>(&it->second)) return static_cast<double>(*p);
    return default_val;
}

bool HunterConfig::get_bool(const std::string& key, bool default_val) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it == config_.end()) return default_val;
    if (auto* p = std::get_if<bool>(&it->second)) return *p;
    return default_val;
}

std::string HunterConfig::get_string(const std::string& key, const std::string& default_val) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it == config_.end()) return default_val;
    if (auto* p = std::get_if<std::string>(&it->second)) return *p;
    return default_val;
}

std::vector<std::string> HunterConfig::get_string_list(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = config_.find(key);
    if (it == config_.end()) return {};
    if (auto* p = std::get_if<std::vector<std::string>>(&it->second)) return *p;
    return {};
}

void HunterConfig::set_int(const std::string& key, long long value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

void HunterConfig::set_double(const std::string& key, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

void HunterConfig::set_bool(const std::string& key, bool value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

void HunterConfig::set_string(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

void HunterConfig::set_string_list(const std::string& key, const std::vector<std::string>& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_[key] = value;
}

std::vector<std::string> HunterConfig::validate() const {
    std::vector<std::string> errors;

    long long api_id = get_int("api_id");
    if (api_id == 0) {
        errors.push_back("HUNTER_API_ID is required");
    }

    std::string api_hash = get_string("api_hash");
    if (api_hash.empty()) {
        errors.push_back("HUNTER_API_HASH is required");
    }

    std::string phone = get_string("phone");
    if (phone.empty()) {
        errors.push_back("HUNTER_PHONE is required");
    }

    struct NumericCheck {
        std::string field;
        long long min_val;
        long long max_val;
    };
    static const std::vector<NumericCheck> checks = {
        {"scan_limit",      1, 1000},
        {"max_total",       1, 10000},
        {"max_workers",     1, 200},
        {"timeout_seconds", 1, 60},
        {"telegram_limit",  1, 500},
        {"sleep_seconds",  10, 3600},
    };

    for (const auto& c : checks) {
        long long val = get_int(c.field);
        if (val < c.min_val || val > c.max_val) {
            errors.push_back(c.field + " must be between " +
                             std::to_string(c.min_val) + " and " +
                             std::to_string(c.max_val));
        }
    }

    return errors;
}

void HunterConfig::set_files_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    files_dir_ = dir;

    // Update path-based configs
    std::string runtime_dir = dir + "/runtime";
    config_["state_file"] = runtime_dir + "/HUNTER_state.json";
    config_["raw_file"] = runtime_dir + "/HUNTER_raw.txt";
    config_["gold_file"] = runtime_dir + "/HUNTER_gold.txt";
    config_["silver_file"] = runtime_dir + "/HUNTER_silver.txt";
    config_["bridge_pool_file"] = runtime_dir + "/HUNTER_bridge_pool.txt";
    config_["validated_jsonl"] = runtime_dir + "/HUNTER_validated.jsonl";
}

std::string HunterConfig::get_files_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return files_dir_;
}

void HunterConfig::set_env(const std::string& key, const std::string& value) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        env_[key] = value;
    }
    load_from_environment();
}

std::string HunterConfig::get_env(const std::string& key, const std::string& default_val) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = env_.find(key);
    return (it != env_.end()) ? it->second : default_val;
}

} // namespace hunter
