#ifndef HUNTER_SWITCH_BYPASS_H
#define HUNTER_SWITCH_BYPASS_H

#include <string>
#include <cstdint>

namespace hunter {
namespace security {

/**
 * Comprehensive switch-specific bypass methods for Iranian network infrastructure.
 * Supports: Huawei NetEngine/NE8000, ZTE ZXR10/M6000, Cisco Catalyst, Nokia 7750 SR,
 * Doran DPI, Yaftaar DPI systems.
 */
class SwitchBypassMethods {
public:
    // ============================================================================
    // Huawei NetEngine 8000 / NE8000 F-series Methods
    // ============================================================================
    
    /** 
     * Method 1: SRv6 bypass - exploits Huawei's SRv6 smart routing implementation
     * NetEngine 8000 X8/X16 uses SRv6 for AI traffic analysis
     */
    static bool bypassHuaweiSRv6(const std::string& ip, const std::string& gw, uint32_t if_index);
    
    /**
     * Method 2: Huawei AI traffic analysis bypass
     * Exploits AI-based traffic classification in NE8000 F1A/F2C
     */
    static bool bypassHuaweiAI(const std::string& ip);
    
    // ============================================================================
    // ZTE ZXR10 9900 / M6000-2S15E Methods  
    // ============================================================================
    
    /**
     * Method 3: Hyper-Converged Edge Router bypass
     * ZTE M6000-2S15E hyper-converged smart network processor
     */
    static bool bypassZTEHyperConverged(const std::string& ip);
    
    /**
     * Method 4: ZTE SRv6 smart routing bypass
     * ZXR10 9916/9908 uses SRv6 protocol
     */
    static bool bypassZTESRv6(const std::string& ip);
    
    // ============================================================================
    // Cisco Catalyst 9500/9300 Methods (LCT Tehran)
    // ============================================================================
    
    /**
     * Method 5: Flexible NetFlow bypass
     * Cisco C9500-24Y4C-A / C9300-24P-E with Flexible NetFlow
     */
    static bool bypassCiscoNetFlow(const std::string& ip);
    
    /**
     * Method 6: Cisco ACL bypass via fragmented packets
     * Exploits ACL processing at LCT Tehran
     */
    static bool bypassCiscoACL(const std::string& ip, const std::string& gw, uint32_t if_index);
    
    // ============================================================================
    // Nokia 7750 SR Series Methods (Legacy international gateways)
    // ============================================================================
    
    /**
     * Method 7: Nokia LSP (Label Switched Path) bypass
     * 7750 SR Series uses LSP for traffic engineering
     */
    static bool bypassNokiaLSP(const std::string& ip);
    
    /**
     * Method 8: Nokia SR legacy gateway bypass
     * For older Nokia international gateway routers
     */
    static bool bypassNokiaSR(const std::string& ip);
    
    // ============================================================================
    // Doran DPI System Methods (Iranian DPI)
    // ============================================================================
    
    /**
     * Method 9: Doran whitelist bypass
     * Doran uses whitelist filtering in stealth blackout mode (10.10.34.x)
     */
    static bool bypassDoranWhitelist(const std::string& ip, const std::string& gw, uint32_t if_index);
    
    // ============================================================================
    // Yaftaar DPI System Methods (Iranian DPI)
    // ============================================================================
    
    /**
     * Method 10: Yaftaar Starlink detection bypass
     * Yaftaar detects Starlink via burst traffic patterns
     */
    static bool bypassYaftaarStarlink(const std::string& ip);
    
    // ============================================================================
    // HP ProLiant Methods (Doran/Yaftaar Backend Servers)
    // ============================================================================
    
    /**
     * Method 11: MSS Clamping bypass
     * HP DL380 servers running DPI software use MSS clamping
     */
    static bool bypassHPMSSClamping(const std::string& ip, const std::string& gw, uint32_t if_index);
    
    // ============================================================================
    // General Iranian Infrastructure Methods
    // ============================================================================
    
    /**
     * Method 12: DNS poisoning bypass
     * Bypass Iranian DNS interception/poisoning
     */
    static bool bypassIranianDNSPoison(const std::string& ip);
    
    /**
     * Method 13: Tiered internet bypass
     * Evade traffic throttling on Iranian tiered internet
     */
    static bool bypassTieredInternet(const std::string& ip);
    
    /**
     * Method 14: Safe search enforcement bypass
     * Bypass safe search enforcement systems
     */
    static bool bypassSafeSearch(const std::string& ip);
    
    /**
     * Method 15: Stealth blackout mode bypass
     * Bypass VPN/proxy detection in stealth blackout mode
     */
    static bool bypassStealthBlackout(const std::string& ip);
};

} // namespace security
} // namespace hunter

#endif // HUNTER_SWITCH_BYPASS_H
