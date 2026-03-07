#include "network/http_client.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"

#include <curl/curl.h>
#include <algorithm>
#include <random>
#include <future>
#include <chrono>

namespace hunter {
namespace network {

bool HttpClient::curl_initialized_ = false;
std::mutex HttpClient::curl_init_mutex_;

void HttpClient::ensureCurlInit() {
    std::lock_guard<std::mutex> lock(curl_init_mutex_);
    if (!curl_initialized_) {
        curl_global_init(CURL_GLOBAL_ALL);
        curl_initialized_ = true;
    }
}

HttpClient::HttpClient() { ensureCurlInit(); }
HttpClient::~HttpClient() {}

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

std::string HttpClient::get(const std::string& url, int timeout_ms, const std::string& proxy_url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)std::min(timeout_ms, 4000));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, randomUserAgent().c_str());

    if (!proxy_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return "";
    return response;
}

int HttpClient::head(const std::string& url, int timeout_ms, const std::string& proxy_url) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)std::min(timeout_ms, 4000));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, randomUserAgent().c_str());

    if (!proxy_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? (int)http_code : -1;
}

std::string HttpClient::post(const std::string& url, const std::string& body,
                              const std::string& content_type, int timeout_ms,
                              const std::string& proxy_url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Content-Type: " + content_type).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, randomUserAgent().c_str());

    if (!proxy_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return res == CURLE_OK ? response : "";
}

void HttpClient::resetSessions() {
    // libcurl handles are per-call, no persistent sessions to reset
}

std::string HttpClient::randomUserAgent() {
    return utils::randomChoice(constants::userAgents());
}

// ═══════════════════════════════════════════════════════════════════
// ConfigFetcher
// ═══════════════════════════════════════════════════════════════════

const std::vector<int> ConfigFetcher::EXTRA_PROXY_PORTS = {
    11808, 11809, 9250, 1080, 1081, 2080, 7890, 10808, 10809
};

ConfigFetcher::ConfigFetcher(HttpClient& http) : http_(http) {}

bool ConfigFetcher::checkDirectAccess() {
    if (direct_works_.load() != -1) return direct_works_.load() == 1;
    int code = http_.head("https://raw.githubusercontent.com", 4000);
    bool ok = code > 0 && code < 500;
    direct_works_ = ok ? 1 : 0;
    return ok;
}

std::optional<int> ConfigFetcher::findBestProxyPort(const std::vector<int>& proxy_ports) {
    std::set<int> seen;
    std::vector<int> candidates;
    for (int p : proxy_ports) {
        if (seen.insert(p).second) candidates.push_back(p);
    }
    for (int p : EXTRA_PROXY_PORTS) {
        if (seen.insert(p).second) candidates.push_back(p);
    }
    std::optional<int> fallback;
    for (int p : candidates) {
        if (!utils::isPortAlive(p, 1000)) continue;
        float lat = utils::portLatency(p, 2500);
        if (lat >= 0 && lat < 2500) return p;
        if (!fallback.has_value()) fallback = p;
    }
    return fallback;
}

bool ConfigFetcher::isCircuitOpen(const std::string& url) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    auto it = failed_urls_.find(url);
    if (it == failed_urls_.end()) return false;
    if (it->second.fail_count >= 3) {
        if (utils::nowTimestamp() - it->second.last_fail < 600.0) return true;
        failed_urls_.erase(it);
    }
    return false;
}

void ConfigFetcher::recordFailure(const std::string& url) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    auto& s = failed_urls_[url];
    s.fail_count++;
    s.last_fail = utils::nowTimestamp();
}

void ConfigFetcher::recordSuccess(const std::string& url) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    failed_urls_.erase(url);
}

std::set<std::string> ConfigFetcher::fetchSingleUrl(
    const std::string& url, const std::vector<int>& proxy_ports, int timeout) {
    if (isCircuitOpen(url)) return {};

    if (direct_works_.load() == -1) checkDirectAccess();
    bool direct_blocked = (direct_works_.load() == 0);

    // Strategy A: Proxy-first when direct is blocked
    if (direct_blocked) {
        auto best = findBestProxyPort(proxy_ports);
        if (best.has_value()) {
            std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*best);
            std::string body = http_.get(url, timeout * 1000, proxy);
            if (!body.empty()) {
                auto found = utils::tryDecodeAndExtract(body);
                if (!found.empty()) { recordSuccess(url); return found; }
            }
        }
        // Fallback: try direct anyway
        std::string body = http_.get(url, std::min(timeout, 6) * 1000);
        if (!body.empty()) {
            auto found = utils::tryDecodeAndExtract(body);
            if (!found.empty()) { recordSuccess(url); return found; }
        }
        recordFailure(url);
        return {};
    }

    // Strategy B: Direct-first
    std::string body = http_.get(url, std::min(timeout, 5) * 1000);
    if (!body.empty()) {
        auto found = utils::tryDecodeAndExtract(body);
        if (!found.empty()) {
            direct_fail_streak_ = 0;
            recordSuccess(url);
            return found;
        }
    }
    direct_fail_streak_++;
    if (direct_fail_streak_ >= 2) direct_works_ = 0;

    // Fallback to proxy
    auto best = findBestProxyPort(proxy_ports);
    if (best.has_value()) {
        std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*best);
        body = http_.get(url, timeout * 1000, proxy);
        if (!body.empty()) {
            auto found = utils::tryDecodeAndExtract(body);
            if (!found.empty()) { recordSuccess(url); return found; }
        }
    }

    recordFailure(url);
    return {};
}

std::set<std::string> ConfigFetcher::fetchUrlsParallel(
    const std::vector<std::string>& urls, const std::vector<int>& proxy_ports,
    int timeout_per, float overall_timeout, int cap, const std::string& label) {

    std::set<std::string> configs;
    std::mutex configs_mutex;
    auto& mgr = HunterTaskManager::instance();

    std::vector<std::future<std::set<std::string>>> futures;
    for (const auto& url : urls) {
        futures.push_back(mgr.submitIO([this, url, &proxy_ports, timeout_per]() {
            return fetchSingleUrl(url, proxy_ports, timeout_per);
        }));
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((int)(overall_timeout * 1000));

    for (auto& fut : futures) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining.count() <= 0) break;
        try {
            auto status = fut.wait_for(remaining);
            if (status == std::future_status::ready) {
                auto found = fut.get();
                if (!found.empty()) {
                    std::lock_guard<std::mutex> lock(configs_mutex);
                    for (const auto& u : found) {
                        configs.insert(u);
                        if (cap > 0 && (int)configs.size() >= cap) break;
                    }
                }
            }
        } catch (...) {}
        if (cap > 0 && (int)configs.size() >= cap) break;
    }

    return configs;
}

std::set<std::string> ConfigFetcher::fetchGithubConfigs(
    const std::vector<int>& proxy_ports, int max_configs,
    int timeout_per, float overall_timeout) {
    if (direct_works_.load() == -1) checkDirectAccess();
    return fetchUrlsParallel(constants::githubRepos(), proxy_ports,
                              timeout_per, overall_timeout, max_configs, "GitHub");
}

std::set<std::string> ConfigFetcher::fetchAntiCensorshipConfigs(
    const std::vector<int>& proxy_ports, int max_configs,
    int timeout_per, float overall_timeout) {
    return fetchUrlsParallel(constants::antiCensorshipSources(), proxy_ports,
                              timeout_per, overall_timeout, max_configs, "Anti-censorship");
}

std::set<std::string> ConfigFetcher::fetchIranPriorityConfigs(
    const std::vector<int>& proxy_ports, int max_configs,
    int timeout_per, float overall_timeout) {
    return fetchUrlsParallel(constants::iranPrioritySources(), proxy_ports,
                              timeout_per, overall_timeout, max_configs, "Iran-priority");
}

} // namespace network
} // namespace hunter
