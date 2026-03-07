#include "network/aggressive_harvester.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/task_manager.h"

#include <algorithm>
#include <random>
#include <future>
#include <chrono>

namespace hunter {
namespace network {

AggressiveHarvester::AggressiveHarvester(const std::vector<int>& extra_proxy_ports) {
    proxy_ports_ = {10808, 10809, 11808, 11809, 9250, 1080, 2080, 7890};
    for (int p : extra_proxy_ports) {
        if (std::find(proxy_ports_.begin(), proxy_ports_.end(), p) == proxy_ports_.end())
            proxy_ports_.push_back(p);
    }
}

std::vector<AggressiveHarvester::Source> AggressiveHarvester::allSources() {
    std::vector<Source> sources;
    for (const auto& url : constants::githubRepos())
        sources.push_back({url, "github"});
    for (const auto& url : constants::antiCensorshipSources())
        sources.push_back({url, "anti_censor"});
    for (const auto& url : constants::iranPrioritySources())
        sources.push_back({url, "iran_priority"});
    return sources;
}

void AggressiveHarvester::probeAlivePorts() {
    alive_ports_.clear();
    for (int p : proxy_ports_) {
        if (utils::isPortAlive(p, 1000)) alive_ports_.push_back(p);
    }
}

bool AggressiveHarvester::checkDirectAccess() {
    if (direct_checked_) return direct_works_;
    direct_checked_ = true;
    int code = http_.head("https://raw.githubusercontent.com", 4000);
    direct_works_ = (code > 0 && code < 500);
    return direct_works_;
}

std::optional<int> AggressiveHarvester::nextProxyPort() {
    if (alive_ports_.empty()) return std::nullopt;
    int port = alive_ports_[port_idx_ % alive_ports_.size()];
    port_idx_++;
    return port;
}

std::set<std::string> AggressiveHarvester::fetchOneSource(const Source& src) {
    std::string ua = HttpClient::randomUserAgent();

    // Strategy A: Proxy-first when direct is blocked
    if (!direct_works_) {
        auto port = nextProxyPort();
        if (port.has_value()) {
            std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*port);
            std::string body = http_.get(src.url, 12000, proxy);
            if (!body.empty()) {
                auto found = utils::tryDecodeAndExtract(body);
                if (!found.empty()) return found;
            }
        }
        return {};
    }

    // Strategy B: Direct-first
    std::string body = http_.get(src.url, 8000);
    if (!body.empty()) {
        auto found = utils::tryDecodeAndExtract(body);
        if (!found.empty()) return found;
    }

    // Fallback to proxy
    auto port = nextProxyPort();
    if (port.has_value()) {
        std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*port);
        body = http_.get(src.url, 12000, proxy);
        if (!body.empty()) {
            auto found = utils::tryDecodeAndExtract(body);
            if (!found.empty()) return found;
        }
    }

    return {};
}

std::set<std::string> AggressiveHarvester::harvest(float timeout_seconds) {
    probeAlivePorts();
    checkDirectAccess();

    auto sources = allSources();
    // Shuffle for load distribution
    static thread_local std::mt19937 rng(std::random_device{}());
    std::shuffle(sources.begin(), sources.end(), rng);

    std::set<std::string> all_configs;
    auto& mgr = HunterTaskManager::instance();
    int ok_count = 0, fail_count = 0;

    std::vector<std::future<std::set<std::string>>> futures;
    for (const auto& src : sources) {
        futures.push_back(mgr.submitIO([this, src]() { return fetchOneSource(src); }));
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((int)(timeout_seconds * 1000));

    for (auto& fut : futures) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining.count() <= 0) break;
        try {
            auto status = fut.wait_for(remaining);
            if (status == std::future_status::ready) {
                auto found = fut.get();
                if (!found.empty()) {
                    all_configs.insert(found.begin(), found.end());
                    ok_count++;
                } else {
                    fail_count++;
                }
            }
        } catch (...) { fail_count++; }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_fetched += (int)all_configs.size();
    stats_.sources_ok = ok_count;
    stats_.sources_failed = fail_count;
    stats_.last_harvest_ts = utils::nowTimestamp();
    stats_.last_harvest_count = (int)all_configs.size();

    return all_configs;
}

AggressiveHarvester::HarvestStats AggressiveHarvester::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

} // namespace network
} // namespace hunter
