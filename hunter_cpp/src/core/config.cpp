#include "core/config.h"
#include "core/utils.h"
#include "core/constants.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace hunter {

HunterConfig::HunterConfig() {
    setDefaults();
}

void HunterConfig::setDefaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    data_["multiproxy_port"] = std::to_string(constants::DEFAULT_SOCKS_PORT);
    data_["gemini_port"] = std::to_string(constants::DEFAULT_GEMINI_PORT);
    data_["max_total"] = std::to_string(constants::DEFAULT_MAX_CONFIGS);
    data_["max_workers"] = std::to_string(constants::DEFAULT_MAX_WORKERS);
    data_["scan_limit"] = std::to_string(constants::DEFAULT_SCAN_LIMIT);
    data_["sleep_seconds"] = "300";
    data_["telegram_limit"] = "50";
    data_["state_file"] = "runtime/hunter_state.json";
    data_["gold_file"] = "runtime/HUNTER_gold.txt";
    data_["silver_file"] = "runtime/HUNTER_silver.txt";
    data_["xray_path"] = "bin/xray.exe";
    data_["singbox_path"] = "bin/sing-box.exe";
    data_["mihomo_path"] = "bin/mihomo-windows-amd64-compatible.exe";
    data_["tor_path"] = "bin/tor.exe";
}

bool HunterConfig::loadFromFile(const std::string& path) {
    std::string json = utils::loadJsonFile(path);
    if (json == "{}") return false;

    // Minimal JSON parser for flat key-value pairs
    std::lock_guard<std::mutex> lock(mutex_);
    // Simple state machine: find "key":"value" or "key":number pairs
    size_t pos = 0;
    while (pos < json.size()) {
        // Find key
        auto kstart = json.find('"', pos);
        if (kstart == std::string::npos) break;
        auto kend = json.find('"', kstart + 1);
        if (kend == std::string::npos) break;
        std::string key = json.substr(kstart + 1, kend - kstart - 1);

        // Find colon
        auto colon = json.find(':', kend + 1);
        if (colon == std::string::npos) break;

        // Find value
        pos = colon + 1;
        while (pos < json.size() && json[pos] == ' ') pos++;
        if (pos >= json.size()) break;

        if (json[pos] == '"') {
            // String value
            auto vend = json.find('"', pos + 1);
            if (vend == std::string::npos) break;
            data_[key] = json.substr(pos + 1, vend - pos - 1);
            pos = vend + 1;
        } else if (json[pos] == '[') {
            // Array value - store raw
            int depth = 1;
            size_t astart = pos;
            pos++;
            while (pos < json.size() && depth > 0) {
                if (json[pos] == '[') depth++;
                else if (json[pos] == ']') depth--;
                pos++;
            }
            data_[key] = json.substr(astart, pos - astart);
        } else if (json[pos] == 't' || json[pos] == 'f') {
            // Boolean
            if (json.substr(pos, 4) == "true") { data_[key] = "true"; pos += 4; }
            else { data_[key] = "false"; pos += 5; }
        } else {
            // Number
            size_t nstart = pos;
            while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '.' || json[pos] == '-'))
                pos++;
            data_[key] = json.substr(nstart, pos - nstart);
        }
    }
    return true;
}

bool HunterConfig::saveToFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    utils::JsonBuilder jb;
    for (auto& [k, v] : data_) {
        jb.add(k, v);
    }
    return utils::saveJsonFile(path, jb.build());
}

std::string HunterConfig::getString(const std::string& key, const std::string& def) const {
    // Check env override first: HUNTER_<UPPER_KEY>
    std::string env_key = "HUNTER_" + key;
    std::transform(env_key.begin(), env_key.end(), env_key.begin(), ::toupper);
    const char* env = std::getenv(env_key.c_str());
    if (env) return std::string(env);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    return it != data_.end() ? it->second : def;
}

int HunterConfig::getInt(const std::string& key, int def) const {
    std::string s = getString(key, "");
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

float HunterConfig::getFloat(const std::string& key, float def) const {
    std::string s = getString(key, "");
    if (s.empty()) return def;
    try { return std::stof(s); } catch (...) { return def; }
}

bool HunterConfig::getBool(const std::string& key, bool def) const {
    std::string s = getString(key, "");
    if (s.empty()) return def;
    return s == "true" || s == "1" || s == "yes";
}

std::vector<std::string> HunterConfig::getStringList(const std::string& key) const {
    std::string raw = getString(key, "");
    if (raw.empty()) return {};
    // Parse JSON array: ["item1","item2"]
    std::vector<std::string> result;
    if (raw.front() == '[' && raw.back() == ']') {
        raw = raw.substr(1, raw.size() - 2);
    }
    std::istringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = utils::trim(token);
        if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
            token = token.substr(1, token.size() - 2);
        }
        if (!token.empty()) result.push_back(token);
    }
    return result;
}

void HunterConfig::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = value;
}

void HunterConfig::set(const std::string& key, int value) {
    set(key, std::to_string(value));
}

void HunterConfig::set(const std::string& key, float value) {
    set(key, std::to_string(value));
}

void HunterConfig::set(const std::string& key, bool value) {
    set(key, value ? std::string("true") : std::string("false"));
}

std::vector<std::string> HunterConfig::githubUrls() const {
    auto urls = getStringList("github_urls");
    if (urls.empty()) {
        return constants::githubRepos();
    }
    return urls;
}

void HunterConfig::setGithubUrls(const std::vector<std::string>& urls) {
    std::string arr = "[";
    for (size_t i = 0; i < urls.size(); i++) {
        if (i > 0) arr += ",";
        arr += "\"" + urls[i] + "\"";
    }
    arr += "]";
    set("github_urls", arr);
}

std::string HunterConfig::getEnv(const std::string& name, const std::string& def) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : def;
}

int HunterConfig::getEnvInt(const std::string& name, int def) {
    std::string s = getEnv(name, "");
    if (s.empty()) return def;
    try { return std::stoi(s); } catch (...) { return def; }
}

bool HunterConfig::getEnvBool(const std::string& name, bool def) {
    std::string s = getEnv(name, "");
    if (s.empty()) return def;
    return s == "true" || s == "1" || s == "yes";
}

} // namespace hunter
