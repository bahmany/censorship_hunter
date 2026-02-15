#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace hunter {

extern const std::vector<std::string> OBF_CDN_WHITELIST_DOMAINS;

class AdversarialDPIExhaustionEngine {
public:
    explicit AdversarialDPIExhaustionEngine(bool enabled = true);

    void start();
    void stop();
    std::string get_current_sni() const;
    std::string rotate_sni();
    std::unordered_map<std::string, int64_t> get_stats() const;
    nlohmann::json apply_obfuscation_to_config(const nlohmann::json& outbound,
                                                const std::string& current_sni) const;

    bool enabled;

private:
    bool running_ = false;
    size_t current_sni_index_ = 0;
    std::unordered_map<std::string, int64_t> stats_;
    std::vector<std::string> cdn_whitelist_;
    int64_t last_sni_rotation_ = 0;
    std::vector<std::vector<uint8_t>> ac_patterns_;

    void generate_ac_patterns();
};

class StealthObfuscationEngine {
public:
    explicit StealthObfuscationEngine(bool enabled = true);

    std::string get_current_sni() const;
    std::string rotate_sni();
    nlohmann::json apply_obfuscation_to_config(const nlohmann::json& outbound) const;
    std::unordered_map<std::string, int64_t> get_stats() const;

    bool enabled;

private:
    mutable size_t current_sni_index_ = 0;
    std::vector<std::string> cdn_whitelist_;
    mutable std::unordered_map<std::string, int64_t> stats_;
};

} // namespace hunter
