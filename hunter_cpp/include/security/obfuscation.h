#pragma once

#include <string>
#include <vector>
#include <map>

#include "core/models.h"

namespace hunter {
namespace security {

/**
 * @brief Stealth obfuscation engine for proxy configs
 * 
 * Applies anti-DPI obfuscation techniques to proxy URIs:
 * - SNI randomization
 * - TLS fingerprint rotation
 * - Path obfuscation for WebSocket/gRPC
 * - Header injection
 */
class ObfuscationEngine {
public:
    ObfuscationEngine();
    ~ObfuscationEngine() = default;

    /**
     * @brief Apply obfuscation to a parsed config
     * @param config Config to obfuscate (modified in place)
     * @return true if any obfuscation was applied
     */
    bool obfuscate(ParsedConfig& config);

    /**
     * @brief Apply obfuscation to a URI string
     * @param uri Original URI
     * @return Obfuscated URI, or original if not applicable
     */
    std::string obfuscateUri(const std::string& uri);

    /**
     * @brief Set obfuscation intensity (0.0 = none, 1.0 = maximum)
     */
    void setIntensity(float intensity) { intensity_ = intensity; }

    /**
     * @brief Get obfuscation statistics
     */
    struct Stats {
        int total_processed = 0;
        int total_obfuscated = 0;
        int sni_randomized = 0;
        int path_obfuscated = 0;
        int fingerprint_rotated = 0;
    };
    Stats getStats() const { return stats_; }

private:
    float intensity_ = 0.7f;
    Stats stats_;

    // SNI randomization pool
    static const std::vector<std::string>& sniPool();

    // TLS fingerprint pool
    static const std::vector<std::string>& fingerprintPool();

    // WebSocket path randomization
    static std::string randomPath();

    void randomizeSni(ParsedConfig& config);
    void rotateFingerprint(ParsedConfig& config);
    void obfuscatePath(ParsedConfig& config);
};

} // namespace security
} // namespace hunter
