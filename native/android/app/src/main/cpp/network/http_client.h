#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>
#include <mutex>

namespace hunter {

// HTTP response callback from JNI (Java OkHttp)
using HttpCallback = std::function<std::string(const std::string& url,
                                                const std::string& user_agent,
                                                int timeout_seconds,
                                                const std::string& proxy)>;

// GitHub repositories for configuration sources
extern const std::vector<std::string> GITHUB_REPOS;

// Anti-censorship sources (Reality-focused, CDN-hosted)
extern const std::vector<std::string> ANTI_CENSORSHIP_SOURCES;

// Iran priority sources (Reality-focused)
extern const std::vector<std::string> IRAN_PRIORITY_SOURCES;

// NapsternetV subscription URLs
extern const std::vector<std::string> NAPSTERV_SUBSCRIPTION_URLS;

class HTTPClientManager {
public:
    HTTPClientManager();

    void set_http_callback(HttpCallback callback);
    std::string random_user_agent() const;
    std::string fetch_url(const std::string& url, int timeout = 8,
                          const std::string& proxy = "") const;

private:
    HttpCallback http_callback_;
    mutable std::mutex mutex_;
};

class ConfigFetcher {
public:
    explicit ConfigFetcher(HTTPClientManager& http_manager);

    std::set<std::string> fetch_github_configs(const std::vector<int>& proxy_ports,
                                                int max_workers = 25);
    std::set<std::string> fetch_anti_censorship_configs(const std::vector<int>& proxy_ports,
                                                         int max_workers = 20);
    std::set<std::string> fetch_iran_priority_configs(const std::vector<int>& proxy_ports,
                                                       int max_workers = 15);
    std::set<std::string> fetch_napsterv_configs(const std::vector<int>& proxy_ports);

private:
    std::set<std::string> fetch_single_url(const std::string& url,
                                            const std::vector<int>& proxy_ports,
                                            int timeout = 12);
    std::set<std::string> fetch_urls_parallel(const std::vector<std::string>& urls,
                                               const std::vector<int>& proxy_ports,
                                               int max_workers, int timeout,
                                               int global_timeout_sec);

    HTTPClientManager& http_manager_;
};

} // namespace hunter
