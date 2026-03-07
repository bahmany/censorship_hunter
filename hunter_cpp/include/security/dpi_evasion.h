#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

#include "core/models.h"

namespace hunter {
namespace security {

/**
 * @brief DPI evasion orchestrator
 * 
 * Detects network conditions and selects optimal anti-DPI strategy.
 * Supports TLS fingerprint evasion, fragmentation, Reality configs,
 * and SplitHTTP transport.
 */
class DpiEvasionOrchestrator {
public:
    DpiEvasionOrchestrator();
    ~DpiEvasionOrchestrator();

    /**
     * @brief Initialize and detect network conditions
     */
    bool start();

    /**
     * @brief Stop adaptation loop
     */
    void stop();

    /**
     * @brief Get optimal DPI evasion strategy
     */
    DpiStrategy getOptimalStrategy() const { return strategy_.load(); }

    /**
     * @brief Get detected network type
     */
    NetworkType getNetworkType() const { return network_type_.load(); }

    /**
     * @brief Prioritize configs based on current DPI strategy
     */
    std::vector<std::string> prioritizeConfigsForStrategy(
        const std::vector<std::string>& uris);

    /**
     * @brief Get status summary
     */
    std::string getStatusSummary() const;

    /**
     * @brief Get DPI metrics
     */
    struct DpiMetrics {
        std::string strategy;
        std::string network_type;
        bool cdn_reachable = false;
        bool google_reachable = false;
        bool telegram_reachable = false;
    };
    DpiMetrics getMetrics() const;

private:
    std::atomic<DpiStrategy> strategy_{DpiStrategy::NONE};
    std::atomic<NetworkType> network_type_{NetworkType::UNKNOWN};
    std::atomic<bool> running_{false};
    std::thread adaptation_thread_;

    bool cdn_reachable_ = false;
    bool google_reachable_ = false;
    bool telegram_reachable_ = false;
    mutable std::mutex state_mutex_;

    void detectNetworkConditions();
    void adaptationLoop();
    DpiStrategy selectStrategy();
    int scoreConfig(const std::string& uri) const;
};

/**
 * @brief DPI pressure engine for opening bypass holes
 */
class DpiPressureEngine {
public:
    explicit DpiPressureEngine(float intensity = 0.7f);
    ~DpiPressureEngine() = default;

    /**
     * @brief Run one pressure cycle
     * @return Stats map with probe results
     */
    std::map<std::string, int> runPressureCycle();

private:
    float intensity_;

    int probeTls(int count);
    int probeTcp(int count);
    int probeDecoy(int count);
    int probeTelegram(int count);
};

} // namespace security
} // namespace hunter
