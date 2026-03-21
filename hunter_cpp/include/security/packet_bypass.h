#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <thread>
#include <atomic>

namespace hunter {
namespace security {

/**
 * @brief Packet-level DPI bypass techniques for Iranian censorship
 * 
 * Implements proven methods that work against Iranian DPI infrastructure:
 * - TLS ClientHello fragmentation (bypass SNI inspection)
 * - TTL manipulation (expire packets before DPI boxes)
 * - TCP SYN fragmentation (evade connection tracking)
 * - Domain fronting (hide real destination)
 * - Packet timing manipulation (evade pattern detection)
 */
class PacketBypass {
public:
    struct BypassConfig {
        // TLS Fragmentation
        bool enable_tls_fragment;
        int fragment_size;
        int fragment_delay_ms;
        
        // TTL Manipulation
        bool enable_ttl_trick;
        int ttl_value;
        
        // TCP Fragmentation
        bool enable_tcp_fragment;
        int tcp_mss;
        
        // Domain Fronting
        bool enable_domain_fronting;
        std::string front_domain;
        
        // Timing
        int initial_delay_ms;
        bool randomize_timing;
        
        // Advanced
        bool enable_fake_sni;
        std::string fake_sni_domain;
        
        BypassConfig()
            : enable_tls_fragment(true)
            , fragment_size(120)
            , fragment_delay_ms(15)
            , enable_ttl_trick(true)
            , ttl_value(8)
            , enable_tcp_fragment(true)
            , tcp_mss(536)
            , enable_domain_fronting(false)
            , front_domain("")
            , initial_delay_ms(50)
            , randomize_timing(true)
            , enable_fake_sni(false)
            , fake_sni_domain("www.google.com") {}
    };

    explicit PacketBypass(const BypassConfig& config = BypassConfig());
    ~PacketBypass();

    /**
     * @brief Apply bypass to outgoing connection
     * @param socket Socket file descriptor
     * @param target_host Real destination hostname
     * @param target_port Real destination port
     * @return true if bypass applied successfully
     */
    bool applyBypass(int socket, const std::string& target_host, int target_port);

    /**
     * @brief Configure XRay with bypass settings
     * @return JSON fragment for XRay streamSettings
     */
    std::string generateXRayBypassConfig() const;

    /**
     * @brief Test if bypass is working
     * @param test_url URL to test (e.g., "https://1.1.1.1")
     * @return true if connection succeeds
     */
    bool testBypass(const std::string& test_url);

    /**
     * @brief Get current configuration
     */
    BypassConfig getConfig() const { return config_; }

    /**
     * @brief Update configuration
     */
    void setConfig(const BypassConfig& config) { config_ = config; }

    /**
     * @brief Get execution log
     */
    std::vector<std::string> getLog() const { return log_; }

private:
    BypassConfig config_;
    std::vector<std::string> log_;

    void logMessage(const std::string& msg);
    
    // TLS Fragmentation
    bool applyTlsFragmentation(int socket);
    bool fragmentClientHello(const uint8_t* data, size_t len, 
                            std::vector<std::vector<uint8_t>>& fragments);
    
    // TTL Manipulation
    bool applyTtlTrick(int socket);
    
    // TCP Fragmentation
    bool applyTcpFragmentation(int socket);
    
    // Domain Fronting
    bool applyDomainFronting(int socket, const std::string& real_host);
    
    // Fake SNI
    bool sendFakeSni(int socket);
};

/**
 * @brief GoodbyeDPI-style bypass implementation
 * 
 * Implements techniques from GoodbyeDPI/zapret/PowerTunnel:
 * - HTTP Host header fragmentation
 * - TCP window size manipulation
 * - Fake packets injection
 * - HTTPS fragmentation
 */
class GoodbyeDpiBypass {
public:
    struct DpiBypassMode {
        bool fragment_http_host;
        bool fragment_https_sni;
        bool fake_packet_injection;
        bool tcp_window_trick;
        bool wrong_checksum;
        bool wrong_seq;
        int fragment_position;
        
        DpiBypassMode() 
            : fragment_http_host(true)
            , fragment_https_sni(true)
            , fake_packet_injection(true)
            , tcp_window_trick(true)
            , wrong_checksum(false)
            , wrong_seq(true)
            , fragment_position(2) {}
    };

    explicit GoodbyeDpiBypass(const DpiBypassMode& mode = DpiBypassMode());

    /**
     * @brief Start bypass service (WinDivert on Windows)
     * @return true if started successfully
     */
    bool start();

    /**
     * @brief Stop bypass service
     */
    void stop();

    /**
     * @brief Check if bypass is active
     */
    bool isActive() const { return active_; }

    /**
     * @brief Get statistics
     */
    struct Stats {
        uint64_t packets_processed = 0;
        uint64_t packets_fragmented = 0;
        uint64_t fake_packets_sent = 0;
        uint64_t connections_bypassed = 0;
    };
    Stats getStats() const { return stats_; }

private:
    DpiBypassMode mode_;
    bool active_ = false;
    Stats stats_;
    void* windivert_handle_ = nullptr;
    std::thread worker_thread_;

    void processingLoop();
    bool processPacket(uint8_t* packet, size_t len);
    bool fragmentHttpPacket(uint8_t* packet, size_t len);
    bool fragmentHttpsPacket(uint8_t* packet, size_t len);
    bool injectFakePacket(const uint8_t* packet, size_t len);
};

/**
 * @brief Iranian-specific DPI bypass strategies
 * 
 * Tailored for Iranian DPI infrastructure (TCI, Irancell, etc.)
 */
class IranianDpiBypass {
public:
    enum class Strategy {
        TLS_FRAGMENT,           // TLS fragmentation (works on most ISPs)
        TTL_TRICK,              // TTL manipulation (works on TCI)
        DOMAIN_FRONTING,        // CDN fronting (works with Cloudflare)
        FAKE_SNI,               // Fake SNI injection (works on Irancell)
        COMBINED,               // Combine multiple techniques
        AUTO                    // Auto-detect best strategy
    };

    explicit IranianDpiBypass(Strategy strategy = Strategy::AUTO);

    /**
     * @brief Auto-detect best bypass strategy for current ISP
     */
    Strategy detectBestStrategy();

    /**
     * @brief Apply bypass to XRay configuration
     * @param xray_config Existing XRay config JSON
     * @return Modified config with bypass settings
     */
    std::string applyToXRayConfig(const std::string& xray_config);

    /**
     * @brief Test multiple strategies and return best one
     * @param test_endpoints Endpoints to test (e.g., ["1.1.1.1:443", "8.8.8.8:443"])
     * @return Best working strategy
     */
    Strategy findWorkingStrategy(const std::vector<std::string>& test_endpoints);

    /**
     * @brief Get current strategy
     */
    Strategy getStrategy() const { return strategy_; }

    /**
     * @brief Get strategy name
     */
    static std::string strategyName(Strategy s);

private:
    Strategy strategy_;
    
    bool testStrategy(Strategy s, const std::string& endpoint);
    std::string generateXRayConfigForStrategy(Strategy s);
};

} // namespace security
} // namespace hunter
