#pragma once

#include <functional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>

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
        std::string pressure_level;
        bool cdn_reachable = false;
        bool google_reachable = false;
        bool telegram_reachable = false;
    };
    DpiMetrics getMetrics() const;

    /**
     * @brief Force re-detect network conditions (on-demand probe)
     */
    void detectNetworkConditions();

    /**
     * @brief Detailed probe result for UI display
     */
    struct ProbeStep {
        std::string target;       // e.g. "1.1.1.1:443"
        std::string category;     // "cdn", "google", "telegram"
        std::string method;       // "tcp_connect", "http_head"
        bool success = false;
        int latency_ms = -1;
        std::string detail;       // e.g. "HTTP 204", "connection refused", "timeout"
    };
    struct DetailedProbeResult {
        std::vector<ProbeStep> steps;
        DpiMetrics metrics;
        std::string recommended_strategy;
        int total_duration_ms = 0;
    };

    /**
     * @brief Run detailed probe with per-step logging
     */
    DetailedProbeResult probeWithDetails();

    /**
     * @brief Network discovery result for exit IP auto-detection
     */
    struct TraceHop {
        int ttl = 0;
        std::string ip;
        int latency_ms = -1;
        std::string hostname;
        std::string asn_info;     // e.g. "AS58224 (TCI)" or "AS13335 (Cloudflare)"
        bool is_domestic = false;
    };
    struct NetworkDiscoveryResult {
        std::string default_gateway;
        std::string gateway_mac;
        std::string local_ip;
        std::string public_ip;
        std::string isp_info;
        std::string suggested_exit_ip;
        std::string suggested_iface;
        std::vector<TraceHop> trace_hops;
        int total_duration_ms = 0;
    };

    /**
     * @brief Auto-discover exit IP using gateway detection + traceroute + public IP check
     */
    NetworkDiscoveryResult discoverExitIp(const std::function<void(const std::string&)>& progress = {});

    /**
     * @brief Edge Router DPI Bypass via Heap Corruption
     * 
     * Advanced technique for extreme isolation scenarios (AS58224 BGP null-routing).
     * Exploits memory management flaw in DPI packet parsing engine to inject
     * kernel-level routes and bypass BGP blackholes.
     * 
     * @param target_mac MAC address of ISP gateway
     * @param exit_ip Foreign IP to route through
     * @param interface Network interface (default: eth0)
     * @return true if bypass successfully established
     */
    bool attemptEdgeRouterBypass(const std::string& target_mac, 
                                 const std::string& exit_ip,
                                 const std::string& iface = "eth0");

    /**
     * @brief Edge Router DPI Bypass with progress callback
     * 
     * Same as attemptEdgeRouterBypass but provides real-time progress updates
     * via callback to prevent UI from appearing frozen.
     * 
     * @param target_mac MAC address of ISP gateway
     * @param exit_ip Foreign IP to route through
     * @param interface Network interface (default: eth0)
     * @param progress_cb Function called with progress updates, returns false to abort
     * @return true if bypass successfully established
     */
    bool attemptEdgeRouterBypassWithProgress(const std::string& target_mac, 
                                           const std::string& exit_ip,
                                           const std::string& iface,
                                           std::function<bool(const std::string&)> progress_cb);

    /**
     * @brief Check if edge router bypass is active
     */
    bool isEdgeRouterBypassActive() const { return edge_bypass_active_.load(); }

    /**
     * @brief Get edge router bypass status
     */
    std::string getEdgeRouterBypassStatus() const;

    /**
     * @brief Get edge router bypass execution log
     */
    std::vector<std::string> getEdgeRouterBypassLog() const;

    /**
     * @brief Manual test: Run bypass on specific IP with MAC display
     * @param target_ip IP address to test (e.g., "192.168.1.1")
     * @return true if any bypass method succeeded
     */
    bool testBypassOnSpecificIp(const std::string& target_ip);

    /**
     * @brief Get list of domestic hops with MAC addresses
     * @return Vector of (IP, MAC, TTL) tuples
     */
    std::vector<std::tuple<std::string, std::string, int>> getDomesticHopsWithMac();

    /**
     * @brief Enable/disable packet-level bypass techniques
     */
    void setPacketBypassEnabled(bool enabled) { packet_bypass_enabled_.store(enabled); }
    bool isPacketBypassEnabled() const { return packet_bypass_enabled_.load(); }

    /**
     * @brief Configure packet bypass settings
     */
    struct PacketBypassSettings {
        bool tls_fragment = true;
        int fragment_size = 100;
        int fragment_delay_ms = 10;
        bool ttl_trick = false;
        int ttl_value = 8;
    };
    void setPacketBypassSettings(const PacketBypassSettings& settings);
    PacketBypassSettings getPacketBypassSettings() const;

private:
    std::atomic<DpiStrategy> strategy_{DpiStrategy::NONE};
    std::atomic<NetworkType> network_type_{NetworkType::UNKNOWN};
    std::atomic<bool> running_{false};
    std::thread adaptation_thread_;

    bool cdn_reachable_ = false;
    bool google_reachable_ = false;
    bool telegram_reachable_ = false;
    std::atomic<int> blocked_signals_{0};
    std::atomic<bool> edge_bypass_active_{false};
    std::string edge_bypass_status_;
    std::vector<std::string> edge_bypass_log_;
    std::atomic<bool> packet_bypass_enabled_{true};
    PacketBypassSettings packet_bypass_settings_;
    mutable std::mutex state_mutex_;

    void adaptationLoop();
    DpiStrategy selectStrategy();
    int scoreConfig(const std::string& uri) const;
    
    // Edge router bypass helpers
    bool initializeRawSocket(const std::string& iface);
    bool performMemoryLeak(const std::string& target_mac);
    uint64_t calculateLibcBase(const std::string& icmp_response);
    bool injectRopChain(const std::string& target_mac, uint64_t libc_base);
    bool injectStaticRoute(const std::string& exit_ip);
    bool verifyBypass();
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

/**
 * @brief Edge Router DPI Bypass Implementation
 * 
 * Specialized class for handling heap corruption-based DPI bypass
 * in extreme network isolation scenarios.
 */
class EdgeRouterBypass {
public:
    struct BypassConfig {
        std::string target_mac;      // ISP Gateway MAC
        std::string exit_ip;         // Foreign IP for route
        std::string iface;           // Network interface
        int timeout_ms = 30000;      // Operation timeout
        bool verbose = true;         // Detailed logging
    };

    explicit EdgeRouterBypass(const BypassConfig& config);
    ~EdgeRouterBypass();

    /**
     * @brief Execute the complete bypass sequence
     */
    bool execute();

    /**
     * @brief Execute bypass with progress callback
     * 
     * Same as execute() but provides real-time progress updates via callback
     * to prevent UI from appearing frozen during long operations.
     * 
     * @param progress_cb Function called with progress updates, returns false to abort
     * @return true if bypass successfully established
     */
    bool executeWithProgress(std::function<bool(const std::string&)> progress_cb);

    /**
     * @brief Get current operation status
     */
    std::string getStatus() const { return status_; }

    /**
     * @brief Get execution log
     */
    std::vector<std::string> getExecutionLog() const { return execution_log_; }

    /**
     * @brief Traceroute hop information (public for manual testing)
     */
    struct TracerouteHop {
        int ttl;
        std::string ip;
        int latency_ms;
        bool is_huawei_switch;
        bool is_chinese_isp;
        std::string mac_hint;
    };

    /**
     * @brief Run traceroute to discover network hops (public for manual testing)
     */
    bool runTraceroute(const std::string& target, std::vector<TracerouteHop>& hops);

private:
    BypassConfig config_;
    std::string status_;
    std::vector<std::string> execution_log_;
    int raw_sock_ = -1;
    std::string resolved_iface_name_;
    std::string resolved_gateway_ip_;
    unsigned long resolved_if_index_ = 0;
    unsigned long resolved_local_addr_ = 0;

    bool initRawSocket();
    void log(const std::string& message);
    bool sendFragmentationAnomaly(const std::string& profile);
    bool analyzeIcmpResponse(uint64_t& libc_base);
    bool constructRopChain(std::vector<uint8_t>& rop_chain);
    bool injectPayload(const std::vector<uint8_t>& payload, const std::string& delivery_mode);
    bool verifyRouteInjection();
    bool executeBypassWithTraceroute();
    bool applyHuaweiBypassMethod(int method_id, const TracerouteHop& hop);
    
    // 12 Bypass methods for Huawei/Chinese switches
    bool bypassMethod1_Fragmentation(const TracerouteHop& hop);
    bool bypassMethod2_IcmpTiming(const TracerouteHop& hop);
    bool bypassMethod3_TcpMssClamping(const TracerouteHop& hop);
    bool bypassMethod4_UdpFlood(const TracerouteHop& hop);
    bool bypassMethod5_SynCookies(const TracerouteHop& hop);
    bool bypassMethod6_ArpSpoof(const TracerouteHop& hop);
    bool bypassMethod7_IpIdManipulation(const TracerouteHop& hop);
    bool bypassMethod8_TtlManipulation(const TracerouteHop& hop);
    bool bypassMethod9_PortHopping(const TracerouteHop& hop);
    bool bypassMethod10_DnsTunneling(const TracerouteHop& hop);
    bool bypassMethod11_Ipv6Transition(const TracerouteHop& hop);
    bool bypassMethod12_GreTunnel(const TracerouteHop& hop);
};

} // namespace security
} // namespace hunter
