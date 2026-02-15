#include "obfuscation.h"
#include "core/utils.h"

#include <random>
#include <algorithm>

namespace hunter {

const std::vector<std::string> OBF_CDN_WHITELIST_DOMAINS = {
    "cloudflare.com", "cdn.cloudflare.com", "cloudflare-dns.com",
    "fastly.net", "fastly.com", "global.fastly.net",
    "akamai.net", "akamaiedge.net", "akamaihd.net",
    "azureedge.net", "azure.com", "microsoft.com",
    "amazonaws.com", "cloudfront.net", "awsglobalaccelerator.com",
    "googleusercontent.com", "googleapis.com", "gstatic.com",
    "workers.dev", "pages.dev", "vercel.app", "r2.dev", "arvan.run",
    "arvancdn.com"
};

// ---------- AdversarialDPIExhaustionEngine ----------

AdversarialDPIExhaustionEngine::AdversarialDPIExhaustionEngine(bool enabled)
    : enabled(enabled) {
    cdn_whitelist_ = OBF_CDN_WHITELIST_DOMAINS;
    last_sni_rotation_ = now_ts();
    stats_ = {
        {"stress_packets_sent", 0},
        {"fragmented_packets", 0},
        {"sni_rotations", 0},
        {"cache_miss_induced", 0},
        {"start_time", 0},
    };
    generate_ac_patterns();
}

void AdversarialDPIExhaustionEngine::generate_ac_patterns() {
    static std::mt19937 rng(std::random_device{}());
    ac_patterns_.clear();
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> pattern(128);
        for (auto& b : pattern) {
            b = static_cast<uint8_t>(rng() % 256);
        }
        ac_patterns_.push_back(std::move(pattern));
    }
}

void AdversarialDPIExhaustionEngine::start() {
    if (!enabled || running_) return;
    running_ = true;
    stats_["start_time"] = now_ts();
}

void AdversarialDPIExhaustionEngine::stop() {
    running_ = false;
}

std::string AdversarialDPIExhaustionEngine::get_current_sni() const {
    if (cdn_whitelist_.empty()) return "cloudflare.com";
    return cdn_whitelist_[current_sni_index_ % cdn_whitelist_.size()];
}

std::string AdversarialDPIExhaustionEngine::rotate_sni() {
    current_sni_index_ = (current_sni_index_ + 1) % cdn_whitelist_.size();
    stats_["sni_rotations"]++;
    last_sni_rotation_ = now_ts();
    return get_current_sni();
}

std::unordered_map<std::string, int64_t> AdversarialDPIExhaustionEngine::get_stats() const {
    auto stats = stats_;
    int64_t start = stats.count("start_time") ? stats["start_time"] : now_ts();
    stats["uptime"] = (start > 0) ? (now_ts() - start) : 0;
    return stats;
}

nlohmann::json AdversarialDPIExhaustionEngine::apply_obfuscation_to_config(
    const nlohmann::json& outbound, const std::string& current_sni) const {
    if (!enabled) return outbound;

    nlohmann::json conf = outbound;
    if (!conf.contains("streamSettings")) return conf;

    auto& settings = conf["streamSettings"];

    if (settings.contains("tlsSettings")) {
        settings["tlsSettings"]["serverName"] = current_sni;
    }

    if (settings.contains("wsSettings")) {
        auto& ws = settings["wsSettings"];
        if (!ws.contains("headers")) ws["headers"] = nlohmann::json::object();
        ws["headers"]["Host"] = current_sni;
        if (!ws.contains("grpcSettings")) ws["grpcSettings"] = nlohmann::json::object();
        ws["grpcSettings"]["authority"] = current_sni;
    }

    return conf;
}

// ---------- StealthObfuscationEngine ----------

StealthObfuscationEngine::StealthObfuscationEngine(bool enabled)
    : enabled(enabled) {
    // Use first 8 CDN domains for stealth rotation
    cdn_whitelist_.assign(
        OBF_CDN_WHITELIST_DOMAINS.begin(),
        OBF_CDN_WHITELIST_DOMAINS.begin() + std::min(static_cast<size_t>(8), OBF_CDN_WHITELIST_DOMAINS.size())
    );
    stats_ = {{"configs_obfuscated", 0}, {"sni_rotations", 0}};
}

std::string StealthObfuscationEngine::get_current_sni() const {
    if (cdn_whitelist_.empty()) return "cdn.cloudflare.com";
    return cdn_whitelist_[current_sni_index_ % cdn_whitelist_.size()];
}

std::string StealthObfuscationEngine::rotate_sni() {
    current_sni_index_ = (current_sni_index_ + 1) % cdn_whitelist_.size();
    stats_["sni_rotations"]++;
    return get_current_sni();
}

nlohmann::json StealthObfuscationEngine::apply_obfuscation_to_config(
    const nlohmann::json& outbound) const {
    if (!enabled || !outbound.contains("streamSettings")) {
        return outbound;
    }

    std::string sni = get_current_sni();
    nlohmann::json conf = outbound;
    auto& settings = conf["streamSettings"];

    if (settings.contains("tlsSettings")) {
        settings["tlsSettings"]["serverName"] = sni;
    }
    if (settings.contains("grpcSettings")) {
        settings["grpcSettings"]["authority"] = sni;
    }
    if (settings.contains("wsSettings")) {
        auto& ws = settings["wsSettings"];
        if (!ws.contains("headers")) ws["headers"] = nlohmann::json::object();
        ws["headers"]["Host"] = sni;
    }

    stats_["configs_obfuscated"]++;
    return conf;
}

std::unordered_map<std::string, int64_t> StealthObfuscationEngine::get_stats() const {
    return stats_;
}

} // namespace hunter
