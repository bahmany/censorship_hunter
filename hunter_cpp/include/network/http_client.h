#pragma once

#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include <optional>

namespace hunter {
namespace network {

/**
 * @brief HTTP client manager with proxy support and circuit breaker
 * 
 * Wraps libcurl for HTTP/HTTPS requests with SOCKS5 proxy fallback,
 * circuit breaker pattern for failing URLs, and auto-detection of
 * direct GitHub access.
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /**
     * @brief Perform HTTP GET request
     * @param url Target URL
     * @param timeout_ms Request timeout
     * @param proxy_url Optional SOCKS5 proxy (e.g. "socks5h://127.0.0.1:10808")
     * @return Response body, or empty on failure
     */
    std::string get(const std::string& url, int timeout_ms = 8000,
                    const std::string& proxy_url = "");

    /**
     * @brief Perform HTTP HEAD request
     * @return HTTP status code, or -1 on failure
     */
    int head(const std::string& url, int timeout_ms = 4000,
             const std::string& proxy_url = "");

    /**
     * @brief Perform HTTP POST request
     */
    std::string post(const std::string& url, const std::string& body,
                     const std::string& content_type = "application/json",
                     int timeout_ms = 8000, const std::string& proxy_url = "");

    /**
     * @brief Reset sessions (free stale connections)
     */
    void resetSessions();

    /**
     * @brief Get a random user agent string
     */
    static std::string randomUserAgent();

private:
    std::mutex mutex_;
    static bool curl_initialized_;
    static std::mutex curl_init_mutex_;

    static void ensureCurlInit();
};

/**
 * @brief Config fetcher with proxy fallback and circuit breaker
 * 
 * Auto-detects whether direct GitHub access works. If not, switches
 * to proxy-first mode. Supports circuit breaker for failing URLs.
 */
class ConfigFetcher {
public:
    explicit ConfigFetcher(HttpClient& http);
    ~ConfigFetcher() = default;

    /**
     * @brief Fetch configs from GitHub repos
     * @param proxy_ports Local SOCKS proxy ports to try
     * @param max_configs Maximum configs to collect (0 = unlimited)
     * @param timeout_per Per-URL timeout in seconds
     * @param overall_timeout Total timeout in seconds
     * @return Set of discovered config URIs
     */
    std::set<std::string> fetchGithubConfigs(
        const std::vector<int>& proxy_ports,
        int max_configs = 0,
        int timeout_per = 9,
        float overall_timeout = 40.0f);

    /**
     * @brief Fetch configs from a custom list of GitHub/subscription URLs
     */
    std::set<std::string> fetchGithubConfigs(
        const std::vector<std::string>& urls,
        const std::vector<int>& proxy_ports,
        int max_configs = 0,
        int timeout_per = 9,
        float overall_timeout = 40.0f);

    /**
     * @brief Fetch configs from anti-censorship sources
     */
    std::set<std::string> fetchAntiCensorshipConfigs(
        const std::vector<int>& proxy_ports,
        int max_configs = 0,
        int timeout_per = 9,
        float overall_timeout = 25.0f);

    /**
     * @brief Fetch configs from Iran priority sources
     */
    std::set<std::string> fetchIranPriorityConfigs(
        const std::vector<int>& proxy_ports,
        int max_configs = 0,
        int timeout_per = 10,
        float overall_timeout = 25.0f);

    /**
     * @brief Check if direct GitHub access works
     */
    bool checkDirectAccess();

    /**
     * @brief Find the best alive proxy port
     */
    std::optional<int> findBestProxyPort(const std::vector<int>& proxy_ports);

private:
    HttpClient& http_;
    std::atomic<int> direct_works_{-1};  // -1=unknown, 0=no, 1=yes
    double last_direct_check_ts_ = 0.0;
    int direct_fail_streak_ = 0;

    // Circuit breaker
    struct UrlState {
        int fail_count = 0;
        double last_fail = 0.0;
    };
    std::map<std::string, UrlState> failed_urls_;
    std::mutex cb_mutex_;

    bool isCircuitOpen(const std::string& url);
    void recordFailure(const std::string& url);
    void recordSuccess(const std::string& url);

    // Core fetch
    std::set<std::string> fetchSingleUrl(const std::string& url,
                                          const std::vector<int>& proxy_ports,
                                          int timeout);
    std::set<std::string> fetchUrlsParallel(
        const std::vector<std::string>& urls,
        const std::vector<int>& proxy_ports,
        int timeout_per, float overall_timeout,
        int cap, const std::string& label);

    // Extra proxy ports to scan
    static const std::vector<int> EXTRA_PROXY_PORTS;
};

} // namespace network
} // namespace hunter
