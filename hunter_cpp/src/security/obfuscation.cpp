#include "security/obfuscation.h"
#include "core/utils.h"
#include "network/uri_parser.h"

#include <random>
#include <algorithm>

namespace hunter {
namespace security {

ObfuscationEngine::ObfuscationEngine() {}

const std::vector<std::string>& ObfuscationEngine::sniPool() {
    static const std::vector<std::string> pool = {
        "www.google.com", "www.microsoft.com", "www.apple.com",
        "cdn.cloudflare.com", "ajax.googleapis.com", "fonts.gstatic.com",
        "www.amazon.com", "static.cloudflareinsights.com",
        "www.gstatic.com", "update.googleapis.com",
        "dl.google.com", "play.google.com",
    };
    return pool;
}

const std::vector<std::string>& ObfuscationEngine::fingerprintPool() {
    static const std::vector<std::string> pool = {
        "chrome", "firefox", "safari", "ios", "android",
        "edge", "randomized", "random",
    };
    return pool;
}

std::string ObfuscationEngine::randomPath() {
    static thread_local std::mt19937 rng(std::random_device{}());
    static const std::vector<std::string> segments = {
        "api", "v1", "v2", "ws", "connect", "stream", "data",
        "proxy", "cdn", "assets", "static", "media",
    };
    std::uniform_int_distribution<int> dist(2, 4);
    int depth = dist(rng);
    std::string path = "/";
    for (int i = 0; i < depth; i++) {
        if (i > 0) path += "/";
        path += utils::randomChoice(segments);
    }
    return path;
}

void ObfuscationEngine::randomizeSni(ParsedConfig& config) {
    if (config.security != "reality" && !config.sni.empty()) {
        // Don't randomize SNI for Reality configs
        if (config.security == "tls" && intensity_ > 0.5f) {
            config.sni = utils::randomChoice(sniPool());
            stats_.sni_randomized++;
        }
    }
}

void ObfuscationEngine::rotateFingerprint(ParsedConfig& config) {
    if (config.fingerprint.empty() || intensity_ > 0.3f) {
        config.fingerprint = utils::randomChoice(fingerprintPool());
        stats_.fingerprint_rotated++;
    }
}

void ObfuscationEngine::obfuscatePath(ParsedConfig& config) {
    if ((config.network == "ws" || config.network == "grpc") && intensity_ > 0.6f) {
        if (config.path.empty() || config.path == "/") {
            config.path = randomPath();
            stats_.path_obfuscated++;
        }
    }
}

bool ObfuscationEngine::obfuscate(ParsedConfig& config) {
    stats_.total_processed++;
    bool changed = false;

    if (intensity_ > 0.3f) {
        rotateFingerprint(config);
        changed = true;
    }
    if (intensity_ > 0.5f && config.security == "tls") {
        randomizeSni(config);
        changed = true;
    }
    if (intensity_ > 0.6f) {
        obfuscatePath(config);
        changed = true;
    }

    if (changed) stats_.total_obfuscated++;
    return changed;
}

std::string ObfuscationEngine::obfuscateUri(const std::string& uri) {
    auto parsed = network::UriParser::parse(uri);
    if (!parsed.has_value()) return uri;
    obfuscate(*parsed);
    // Note: reconstructing URI from parsed config is complex;
    // return original for now (obfuscation applied at outbound JSON level)
    return uri;
}

} // namespace security
} // namespace hunter
