#pragma once

#include <string>
#include <map>
#include <mutex>
#include <vector>
#include <cstdlib>

namespace hunter {

/**
 * @brief Hunter configuration manager
 * 
 * Thread-safe configuration store with environment variable overrides.
 * Follows the same pattern as Python HunterConfig.
 */
class HunterConfig {
public:
    HunterConfig();
    ~HunterConfig() = default;

    /**
     * @brief Load configuration from JSON file
     * @param path Path to config file
     * @return true on success
     */
    bool loadFromFile(const std::string& path);

    /**
     * @brief Save configuration to JSON file
     * @param path Path to config file
     * @return true on success
     */
    bool saveToFile(const std::string& path) const;

    /**
     * @brief Get a string config value
     * @param key Config key
     * @param default_val Default if not found
     */
    std::string getString(const std::string& key, const std::string& default_val = "") const;

    /**
     * @brief Get an integer config value
     */
    int getInt(const std::string& key, int default_val = 0) const;

    /**
     * @brief Get a float config value
     */
    float getFloat(const std::string& key, float default_val = 0.0f) const;

    /**
     * @brief Get a boolean config value
     */
    bool getBool(const std::string& key, bool default_val = false) const;

    /**
     * @brief Get a string list config value
     */
    std::vector<std::string> getStringList(const std::string& key) const;

    /**
     * @brief Set a config value
     */
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, int value);
    void set(const std::string& key, float value);
    void set(const std::string& key, bool value);

    /**
     * @brief Get environment variable with prefix HUNTER_
     */
    static std::string getEnv(const std::string& name, const std::string& default_val = "");
    static int getEnvInt(const std::string& name, int default_val = 0);
    static bool getEnvBool(const std::string& name, bool default_val = false);

    // ─── Convenience accessors for common settings ───

    int multiproxyPort() const { return getInt("multiproxy_port", 10808); }
    int geminiPort() const { return getInt("gemini_port", 10809); }
    int maxTotal() const { return getInt("max_total", 1000); }
    int maxWorkers() const { return getInt("max_workers", 12); }
    int scanLimit() const { return getInt("scan_limit", 50); }
    int sleepSeconds() const { return getInt("sleep_seconds", 300); }
    int telegramLimit() const { return getInt("telegram_limit", 50); }
    std::string stateFile() const { return getString("state_file", "runtime/hunter_state.json"); }
    std::string goldFile() const { return getString("gold_file", "runtime/gold.txt"); }
    std::string silverFile() const { return getString("silver_file", "runtime/silver.txt"); }
    std::string xrayPath() const { return getString("xray_path", "xray.exe"); }
    std::vector<std::string> telegramTargets() const { return getStringList("targets"); }

    /**
     * @brief Get GitHub config source URLs (user-configurable, defaults to constants::githubRepos())
     */
    std::vector<std::string> githubUrls() const;

    /**
     * @brief Set GitHub config source URLs
     */
    void setGithubUrls(const std::vector<std::string>& urls);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> data_;

    void setDefaults();
};

} // namespace hunter
