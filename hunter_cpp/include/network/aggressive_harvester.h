#pragma once

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
    HttpClient http_;
    std::vector<int> proxy_ports_;
    std::vector<int> alive_ports_;
    int port_idx_ = 0;
    bool direct_works_ = false;
    bool direct_checked_ = false;
    HarvestStats stats_;
    mutable std::mutex mutex_;

    struct Source {
        std::string url;
        std::string tag;
    };

    static std::vector<Source> allSources();
    std::optional<int> nextProxyPort();
    void probeAlivePorts();
    bool checkDirectAccess();
    std::set<std::string> fetchOneSource(const Source& src);
};

} // namespace network
} // namespace hunter
