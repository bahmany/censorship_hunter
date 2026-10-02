#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <optional>

#include "network/http_client.h"

namespace hunter {
namespace network {

/**
 * @brief Mass config harvester using all available proxy ports
 * 
 * Fetches from ALL known sources in parallel, using round-robin
 * proxy ports and direct/proxy fallback strategies.
 */
class AggressiveHarvester {
public:
    explicit AggressiveHarvester(const std::vector<int>& extra_proxy_ports = {});
    ~AggressiveHarvester() = default;

    /**
     * @brief Run a full harvest cycle
     * @param timeout_seconds Overall timeout
     * @return Set of all discovered config URIs
     */
    std::set<std::string> harvest(float timeout_seconds = 300.0f);

    /**
     * @brief Get harvest statistics
     */
    struct HarvestStats {
        int total_fetched = 0;
        int sources_ok = 0;
        int sources_failed = 0;
        double last_harvest_ts = 0.0;
        int last_harvest_count = 0;
    };
    HarvestStats getStats() const;

private:
    struct Source {
        std::string url;
        std::string tag;
    };

    /**
     * @brief Per-harvest state shared with the in-flight fetch tasks.
     *
     * Held by shared_ptr and captured *by value* into every task submitted to
     * the IO pool. harvest() gives up on stragglers once the deadline passes,
     * so those tasks can still be running after harvest() returns and after
     * the AggressiveHarvester itself is destroyed (HarvesterWorker::execute()
     * builds one on the stack). Keeping their state here — instead of in
     * AggressiveHarvester members — is what stops that from being a
     * use-after-free.
     */
    struct Context {
        HttpClient http;
        std::vector<int> alive_ports;
        std::atomic<int> port_idx{0};
        std::atomic<bool> direct_works{false};
        std::atomic<bool> cancelled{false};
    };

    std::vector<int> proxy_ports_;
    HarvestStats stats_;
    mutable std::mutex mutex_;

    static std::vector<Source> allSources();
    static std::vector<int> probeAlivePorts(const std::vector<int>& ports);
    static bool checkDirectAccess(HttpClient& http);
    static std::optional<int> nextProxyPort(Context& ctx);
    static std::set<std::string> fetchOneSource(const std::shared_ptr<Context>& ctx,
                                                const Source& src);
};

} // namespace network
} // namespace hunter
