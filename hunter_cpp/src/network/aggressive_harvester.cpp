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

namespace {
// Grace period we keep waiting for stragglers after the harvest deadline, so
// their results are still collected instead of thrown away.
constexpr int kDrainGraceMs = 15000;
} // namespace

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

std::vector<int> AggressiveHarvester::probeAlivePorts(const std::vector<int>& ports) {
    std::vector<int> alive;
    for (int p : ports) {
        if (utils::isPortAlive(p, 1000)) alive.push_back(p);
    }
    return alive;
}

bool AggressiveHarvester::checkDirectAccess(HttpClient& http) {
    int code = http.head("https://raw.githubusercontent.com", 4000);
    return code > 0 && code < 500;
}

std::optional<int> AggressiveHarvester::nextProxyPort(Context& ctx) {
    if (ctx.alive_ports.empty()) return std::nullopt;
    int idx = ctx.port_idx.fetch_add(1);
    return ctx.alive_ports[(size_t)(idx % (int)ctx.alive_ports.size())];
}

std::set<std::string> AggressiveHarvester::fetchOneSource(const std::shared_ptr<Context>& ctx,
                                                          const Source& src) {
    // Tasks still queued when harvest() gives up should not spend another
    // 12s on the wire — bail out immediately instead.
    if (ctx->cancelled.load()) return {};

    // Strategy A: Proxy-first when direct is blocked
    if (!ctx->direct_works.load()) {
        auto port = nextProxyPort(*ctx);
        if (port.has_value()) {
            std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*port);
            std::string body = ctx->http.get(src.url, 12000, proxy);
            if (!body.empty()) {
                auto found = utils::tryDecodeAndExtract(body);
                if (!found.empty()) return found;
            }
        }
        return {};
    }

    // Strategy B: Direct-first
    std::string body = ctx->http.get(src.url, 8000);
    if (!body.empty()) {
        auto found = utils::tryDecodeAndExtract(body);
        if (!found.empty()) return found;
    }

    if (ctx->cancelled.load()) return {};

    // Fallback to proxy
    auto port = nextProxyPort(*ctx);
    if (port.has_value()) {
        std::string proxy = "socks5h://127.0.0.1:" + std::to_string(*port);
        body = ctx->http.get(src.url, 12000, proxy);
        if (!body.empty()) {
            auto found = utils::tryDecodeAndExtract(body);
            if (!found.empty()) return found;
        }
    }

    return {};
}

std::set<std::string> AggressiveHarvester::harvest(float timeout_seconds) {
    auto ctx = std::make_shared<Context>();
    ctx->alive_ports = probeAlivePorts(proxy_ports_);
    ctx->direct_works.store(checkDirectAccess(ctx->http));

    auto sources = allSources();
    // Shuffle for load distribution
    static thread_local std::mt19937 rng(std::random_device{}());
    std::shuffle(sources.begin(), sources.end(), rng);

    std::set<std::string> all_configs;
    auto& mgr = HunterTaskManager::instance();
    int ok_count = 0, fail_count = 0;

    std::vector<std::future<std::set<std::string>>> futures;
    futures.reserve(sources.size());
    for (const auto& src : sources) {
        // Capture the context by value: these tasks outlive harvest() whenever
        // the deadline expires, and `this` may be gone by the time they run.
        futures.push_back(mgr.submitIO([ctx, src]() { return fetchOneSource(ctx, src); }));
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds((int)(timeout_seconds * 1000));

    auto collect = [&](std::future<std::set<std::string>>& fut) {
        try {
            auto found = fut.get();
            if (!found.empty()) {
                all_configs.insert(found.begin(), found.end());
                ok_count++;
            } else {
                fail_count++;
            }
        } catch (...) { fail_count++; }
    };

    size_t next = 0;
    for (; next < futures.size(); next++) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining.count() <= 0) break;
        try {
            if (futures[next].wait_for(remaining) != std::future_status::ready) break;
        } catch (...) { fail_count++; continue; }
        collect(futures[next]);
    }

    if (next < futures.size()) {
        // Past the deadline. Tell everything still queued to give up, then
        // drain what we can within a bounded grace period so results already
        // on the wire are not wasted.
        ctx->cancelled.store(true);
        auto drain_deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kDrainGraceMs);
        for (; next < futures.size(); next++) {
            auto remaining = drain_deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) { fail_count++; continue; }
            try {
                if (futures[next].wait_for(remaining) != std::future_status::ready) {
                    fail_count++;
                    continue;
                }
            } catch (...) { fail_count++; continue; }
            collect(futures[next]);
        }
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
