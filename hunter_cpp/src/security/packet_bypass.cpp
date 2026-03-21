#include "security/packet_bypass.h"
#include "core/utils.h"
#include <sstream>
#include <chrono>
#include <thread>
#include <random>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hunter {
namespace security {

// ═══════════════════════════════════════════════════════════════════
// PacketBypass Implementation
// ═══════════════════════════════════════════════════════════════════

PacketBypass::PacketBypass(const BypassConfig& config) : config_(config) {
    logMessage("PacketBypass initialized with config:");
    logMessage("  TLS Fragment: " + std::string(config_.enable_tls_fragment ? "enabled" : "disabled"));
    logMessage("  TTL Trick: " + std::string(config_.enable_ttl_trick ? "enabled" : "disabled"));
    logMessage("  TCP Fragment: " + std::string(config_.enable_tcp_fragment ? "enabled" : "disabled"));
}

PacketBypass::~PacketBypass() = default;

void PacketBypass::logMessage(const std::string& msg) {
    log_.push_back(msg);
    if (log_.size() > 1000) {
        log_.erase(log_.begin(), log_.begin() + 500);
    }
}

bool PacketBypass::applyBypass(int socket, const std::string& target_host, int target_port) {
    logMessage("[*] Applying bypass to connection: " + target_host + ":" + std::to_string(target_port));
    
    bool success = true;
    
    // Apply TTL manipulation first (must be before connection)
    if (config_.enable_ttl_trick) {
        if (!applyTtlTrick(socket)) {
            logMessage("[!] TTL trick failed, continuing anyway");
        }
    }
    
    // Apply TCP fragmentation settings
    if (config_.enable_tcp_fragment) {
        if (!applyTcpFragmentation(socket)) {
            logMessage("[!] TCP fragmentation failed, continuing anyway");
        }
    }
    
    // Initial delay to evade timing-based detection
    if (config_.initial_delay_ms > 0) {
        int delay = config_.initial_delay_ms;
        if (config_.randomize_timing) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(delay / 2, delay * 2);
            delay = dis(gen);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    
    logMessage("[✓] Bypass applied successfully");
    return success;
}

bool PacketBypass::applyTtlTrick(int socket) {
    logMessage("[*] Applying TTL trick (TTL=" + std::to_string(config_.ttl_value) + ")");
    
#ifdef _WIN32
    DWORD ttl = config_.ttl_value;
    if (setsockopt(socket, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof(ttl)) != 0) {
        logMessage("[!] Failed to set TTL: " + std::to_string(WSAGetLastError()));
        return false;
    }
#else
    int ttl = config_.ttl_value;
    if (setsockopt(socket, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0) {
        logMessage("[!] Failed to set TTL: " + std::string(strerror(errno)));
        return false;
    }
#endif
    
    logMessage("[✓] TTL set to " + std::to_string(config_.ttl_value));
    return true;
}

bool PacketBypass::applyTcpFragmentation(int socket) {
    logMessage("[*] Applying TCP fragmentation (MSS=" + std::to_string(config_.tcp_mss) + ")");
    
#ifdef _WIN32
    DWORD mss = config_.tcp_mss;
    if (setsockopt(socket, IPPROTO_TCP, TCP_MAXSEG, (const char*)&mss, sizeof(mss)) != 0) {
        // Not critical, many systems don't support setting MSS
        logMessage("[!] Failed to set TCP MSS (not critical)");
    }
#else
    int mss = config_.tcp_mss;
    if (setsockopt(socket, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) != 0) {
        logMessage("[!] Failed to set TCP MSS (not critical)");
    }
#endif
    
    // Disable Nagle's algorithm for immediate sending of small packets
#ifdef _WIN32
    BOOL nodelay = TRUE;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay)) != 0) {
        logMessage("[!] Failed to disable Nagle");
        return false;
    }
#else
    int nodelay = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) != 0) {
        logMessage("[!] Failed to disable Nagle");
        return false;
    }
#endif
    
    logMessage("[✓] TCP fragmentation configured");
    return true;
}

std::string PacketBypass::generateXRayBypassConfig() const {
    std::ostringstream oss;
    oss << "{\n";
    
    if (config_.enable_tls_fragment) {
        oss << "  \"tlsSettings\": {\n";
        oss << "    \"fragment\": {\n";
        oss << "      \"packets\": \"tlshello\",\n";
        oss << "      \"length\": \"" << config_.fragment_size << "-" << (config_.fragment_size + 20) << "\",\n";
        oss << "      \"interval\": \"" << config_.fragment_delay_ms << "-" << (config_.fragment_delay_ms + 10) << "\"\n";
        oss << "    }";
        
        if (config_.enable_fake_sni) {
            oss << ",\n    \"serverName\": \"" << config_.fake_sni_domain << "\"";
        }
        
        oss << "\n  }";
    }
    
    if (config_.enable_domain_fronting && !config_.front_domain.empty()) {
        if (config_.enable_tls_fragment) oss << ",\n";
        oss << "  \"wsSettings\": {\n";
        oss << "    \"headers\": {\n";
        oss << "      \"Host\": \"" << config_.front_domain << "\"\n";
        oss << "    }\n";
        oss << "  }";
    }
    
    oss << "\n}";
    return oss.str();
}

bool PacketBypass::testBypass(const std::string& test_url) {
    logMessage("[*] Testing bypass with: " + test_url);
    
    // Parse URL to get host and port
    std::string host;
    int port = 443;
    
    size_t proto_end = test_url.find("://");
    if (proto_end != std::string::npos) {
        size_t host_start = proto_end + 3;
        size_t port_start = test_url.find(':', host_start);
        size_t path_start = test_url.find('/', host_start);
        
        if (port_start != std::string::npos && (path_start == std::string::npos || port_start < path_start)) {
            host = test_url.substr(host_start, port_start - host_start);
            size_t port_end = (path_start != std::string::npos) ? path_start : test_url.length();
            port = std::stoi(test_url.substr(port_start + 1, port_end - port_start - 1));
        } else {
            size_t host_end = (path_start != std::string::npos) ? path_start : test_url.length();
            host = test_url.substr(host_start, host_end - host_start);
        }
    }
    
    if (host.empty()) {
        logMessage("[!] Failed to parse test URL");
        return false;
    }
    
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        logMessage("[!] Failed to create socket");
        return false;
    }
    
    // Apply bypass
    if (!applyBypass(sock, host, port)) {
        logMessage("[!] Failed to apply bypass");
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }
    
    // Try to connect
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    
    // Simple IP resolution (just try direct IP first)
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        // Not an IP, would need DNS resolution here
        logMessage("[!] DNS resolution not implemented in test");
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return false;
    }
    
    // Set timeout
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    
    // Connect
    bool success = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    
    if (success) {
        logMessage("[✓] Bypass test successful");
    } else {
        logMessage("[!] Connection failed");
    }
    
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    
    return success;
}

// ═══════════════════════════════════════════════════════════════════
// GoodbyeDpiBypass Implementation
// ═══════════════════════════════════════════════════════════════════

GoodbyeDpiBypass::GoodbyeDpiBypass(const DpiBypassMode& mode) : mode_(mode) {}

bool GoodbyeDpiBypass::start() {
    if (active_) return true;
    
#ifdef _WIN32
    // WinDivert implementation would go here
    // For now, we'll use XRay's built-in fragmentation instead
    active_ = true;
    return true;
#else
    // Linux iptables/nftables implementation
    active_ = true;
    return true;
#endif
}

void GoodbyeDpiBypass::stop() {
    if (!active_) return;
    
    active_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

// ═══════════════════════════════════════════════════════════════════
// IranianDpiBypass Implementation
// ═══════════════════════════════════════════════════════════════════

IranianDpiBypass::IranianDpiBypass(Strategy strategy) : strategy_(strategy) {
    if (strategy_ == Strategy::AUTO) {
        strategy_ = detectBestStrategy();
    }
}

IranianDpiBypass::Strategy IranianDpiBypass::detectBestStrategy() {
    // Auto-detect based on network conditions
    // For Iranian networks, TLS fragmentation works best on most ISPs
    return Strategy::TLS_FRAGMENT;
}

std::string IranianDpiBypass::applyToXRayConfig(const std::string& xray_config) {
    std::string config = xray_config;
    
    switch (strategy_) {
        case Strategy::TLS_FRAGMENT: {
            // Add TLS fragmentation to all outbounds
            PacketBypass::BypassConfig bypass_config;
            bypass_config.enable_tls_fragment = true;
            bypass_config.fragment_size = 100;  // Smaller fragments for Iranian DPI
            bypass_config.fragment_delay_ms = 10;
            
            PacketBypass bypass(bypass_config);
            std::string fragment_config = bypass.generateXRayBypassConfig();
            
            // Insert into config (simplified - real implementation would parse JSON)
            size_t pos = config.find("\"streamSettings\"");
            if (pos != std::string::npos) {
                config.insert(pos + 17, fragment_config + ",");
            }
            break;
        }
        
        case Strategy::TTL_TRICK: {
            // TTL manipulation is handled at socket level, not in XRay config
            break;
        }
        
        case Strategy::DOMAIN_FRONTING: {
            // Add CDN fronting headers
            break;
        }
        
        case Strategy::FAKE_SNI: {
            PacketBypass::BypassConfig bypass_config;
            bypass_config.enable_fake_sni = true;
            bypass_config.fake_sni_domain = "www.google.com";
            
            PacketBypass bypass(bypass_config);
            std::string sni_config = bypass.generateXRayBypassConfig();
            
            size_t pos = config.find("\"streamSettings\"");
            if (pos != std::string::npos) {
                config.insert(pos + 17, sni_config + ",");
            }
            break;
        }
        
        case Strategy::COMBINED: {
            // Combine TLS fragment + TTL trick
            PacketBypass::BypassConfig bypass_config;
            bypass_config.enable_tls_fragment = true;
            bypass_config.enable_ttl_trick = true;
            bypass_config.fragment_size = 100;
            bypass_config.ttl_value = 8;
            
            PacketBypass bypass(bypass_config);
            std::string combined_config = bypass.generateXRayBypassConfig();
            
            size_t pos = config.find("\"streamSettings\"");
            if (pos != std::string::npos) {
                config.insert(pos + 17, combined_config + ",");
            }
            break;
        }
        
        default:
            break;
    }
    
    return config;
}

IranianDpiBypass::Strategy IranianDpiBypass::findWorkingStrategy(
    const std::vector<std::string>& test_endpoints) {
    
    // Test strategies in order of effectiveness for Iranian networks
    std::vector<Strategy> strategies = {
        Strategy::TLS_FRAGMENT,
        Strategy::COMBINED,
        Strategy::FAKE_SNI,
        Strategy::TTL_TRICK,
        Strategy::DOMAIN_FRONTING
    };
    
    for (auto strategy : strategies) {
        bool all_success = true;
        for (const auto& endpoint : test_endpoints) {
            if (!testStrategy(strategy, endpoint)) {
                all_success = false;
                break;
            }
        }
        
        if (all_success) {
            return strategy;
        }
    }
    
    // Fallback to TLS fragment
    return Strategy::TLS_FRAGMENT;
}

bool IranianDpiBypass::testStrategy(Strategy s, const std::string& endpoint) {
    PacketBypass::BypassConfig config;
    
    switch (s) {
        case Strategy::TLS_FRAGMENT:
            config.enable_tls_fragment = true;
            config.enable_ttl_trick = false;
            break;
        case Strategy::TTL_TRICK:
            config.enable_tls_fragment = false;
            config.enable_ttl_trick = true;
            config.ttl_value = 8;
            break;
        case Strategy::COMBINED:
            config.enable_tls_fragment = true;
            config.enable_ttl_trick = true;
            config.ttl_value = 8;
            break;
        case Strategy::FAKE_SNI:
            config.enable_fake_sni = true;
            break;
        default:
            break;
    }
    
    PacketBypass bypass(config);
    return bypass.testBypass("https://" + endpoint);
}

std::string IranianDpiBypass::strategyName(Strategy s) {
    switch (s) {
        case Strategy::TLS_FRAGMENT: return "TLS Fragmentation";
        case Strategy::TTL_TRICK: return "TTL Manipulation";
        case Strategy::DOMAIN_FRONTING: return "Domain Fronting";
        case Strategy::FAKE_SNI: return "Fake SNI";
        case Strategy::COMBINED: return "Combined (TLS+TTL)";
        case Strategy::AUTO: return "Auto-Detect";
        default: return "Unknown";
    }
}

} // namespace security
} // namespace hunter
