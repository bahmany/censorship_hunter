#include "network/import_filter.h"
#include "core/utils.h"

namespace hunter {
namespace network {

bool isImportableProxyUri(const std::string& uri) {
    if (uri.size() < 10) return false;
    // ssr:// deliberately excluded — no embedded engine can run it.
    static const std::vector<std::string> schemes = {
        "vmess://", "vless://", "trojan://", "ss://",
        "hysteria2://", "hy2://", "tuic://", "wireguard://"
    };
    bool has_scheme = false;
    for (const auto& s : schemes) {
        if (uri.compare(0, s.size(), s) == 0) {
            has_scheme = true;
            break;
        }
    }
    if (!has_scheme) return false;
    if (uri.compare(0, 8, "vmess://") == 0) {
        std::string payload = uri.substr(8);
        auto hash = payload.find('#');
        if (hash != std::string::npos) payload = payload.substr(0, hash);
        if (payload.size() < 10) return false;
        std::string decoded = utils::base64Decode(payload);
        if (decoded.empty() || decoded.find('{') == std::string::npos) return false;
        return decoded.find("\"add\"") != std::string::npos;
    }
    auto sep = uri.find("://");
    if (sep == std::string::npos) return false;
    std::string payload = uri.substr(sep + 3);
    auto hash = payload.find('#');
    if (hash != std::string::npos) payload = payload.substr(0, hash);
    if (payload.size() < 3) return false;
    if (uri.compare(0, 8, "vless://") == 0 || uri.compare(0, 9, "trojan://") == 0 ||
        uri.compare(0, 12, "wireguard://") == 0) {
        if (payload.find('@') == std::string::npos) return false;
    }
    if (uri.compare(0, 5, "ss://") == 0 && payload.find('@') == std::string::npos) {
        std::string decoded = utils::base64Decode(payload);
        if (decoded.empty() || decoded.find(':') == std::string::npos) return false;
    }
    return true;
}

} // namespace network
} // namespace hunter
