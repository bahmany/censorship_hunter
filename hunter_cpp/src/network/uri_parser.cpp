#include "network/uri_parser.h"
#include "core/utils.h"
#include "core/constants.h"

#include <algorithm>
#include <sstream>
#include <regex>

namespace hunter {
namespace network {

bool UriParser::isValidScheme(const std::string& uri) {
    for (const auto& scheme : constants::supportedSchemes()) {
        if (utils::startsWith(uri, scheme + "://")) return true;
    }
    return false;
}

std::map<std::string, std::string> UriParser::parseQueryParams(const std::string& query) {
    std::map<std::string, std::string> params;
    for (const auto& pair : utils::split(query, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            params[pair.substr(0, eq)] = utils::urlDecode(pair.substr(eq + 1));
        } else {
            params[pair] = "";
        }
    }
    return params;
}

std::optional<ParsedConfig> UriParser::parse(const std::string& uri) {
    std::string trimmed = utils::trim(uri);
    if (trimmed.empty()) return std::nullopt;

    if (utils::startsWith(trimmed, "vmess://")) return parseVmess(trimmed);
    if (utils::startsWith(trimmed, "vless://")) return parseVless(trimmed);
    if (utils::startsWith(trimmed, "trojan://")) return parseTrojan(trimmed);
    if (utils::startsWith(trimmed, "ss://")) return parseShadowsocks(trimmed);
    if (utils::startsWith(trimmed, "hysteria2://") || utils::startsWith(trimmed, "hy2://"))
        return parseHysteria2(trimmed);
    if (utils::startsWith(trimmed, "tuic://")) return parseTuic(trimmed);

    return std::nullopt;
}

std::vector<ParsedConfig> UriParser::parseMany(const std::vector<std::string>& uris) {
    std::vector<ParsedConfig> results;
    results.reserve(uris.size());
    for (const auto& uri : uris) {
        auto parsed = parse(uri);
        if (parsed.has_value() && parsed->isValid()) {
            results.push_back(std::move(*parsed));
        }
    }
    return results;
}

// ─── VMess ───
std::optional<ParsedConfig> UriParser::parseVmess(const std::string& uri) {
    // vmess://base64json
    std::string payload = uri.substr(8); // skip "vmess://"
    // Remove fragment
    auto hash_pos = payload.find('#');
    std::string remark;
    if (hash_pos != std::string::npos) {
        remark = utils::urlDecode(payload.substr(hash_pos + 1));
        payload = payload.substr(0, hash_pos);
    }

    std::string json = utils::base64Decode(payload);
    if (json.empty() || json.find('{') == std::string::npos) return std::nullopt;

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "vmess";
    cfg.ps = remark;

    // Minimal JSON parsing for vmess fields
    auto extractField = [&json](const std::string& key) -> std::string {
        std::string search = "\"" + key + "\"";
        auto pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos = json.find(':', pos + search.size());
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < json.size() && json[pos] == ' ') pos++;
        if (pos >= json.size()) return "";
        if (json[pos] == '"') {
            auto end = json.find('"', pos + 1);
            if (end == std::string::npos) return "";
            return json.substr(pos + 1, end - pos - 1);
        }
        // Number
        auto end = json.find_first_of(",}", pos);
        if (end == std::string::npos) end = json.size();
        return utils::trim(json.substr(pos, end - pos));
    };

    cfg.address = extractField("add");
    std::string port_str = extractField("port");
    try { cfg.port = std::stoi(port_str); } catch (...) { return std::nullopt; }
    cfg.uuid = extractField("id");
    cfg.encryption = extractField("scy");
    if (cfg.encryption.empty()) cfg.encryption = "auto";
    cfg.network = extractField("net");
    if (cfg.network.empty()) cfg.network = "tcp";
    cfg.security = extractField("tls");
    cfg.sni = extractField("sni");
    cfg.host = extractField("host");
    cfg.path = extractField("path");
    cfg.type = extractField("type");
    cfg.fingerprint = extractField("fp");
    if (cfg.ps.empty()) cfg.ps = extractField("ps");

    if (cfg.address.empty() || cfg.port <= 0 || cfg.uuid.empty()) return std::nullopt;
    return cfg;
}

// ─── VLESS ───
std::optional<ParsedConfig> UriParser::parseVless(const std::string& uri) {
    // vless://uuid@host:port?params#remark
    std::string rest = uri.substr(8); // skip "vless://"

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "vless";

    // Fragment (remark)
    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }

    // Query params
    auto q_pos = rest.find('?');
    std::map<std::string, std::string> params;
    if (q_pos != std::string::npos) {
        params = parseQueryParams(rest.substr(q_pos + 1));
        rest = rest.substr(0, q_pos);
    }

    // uuid@host:port
    auto at_pos = rest.find('@');
    if (at_pos == std::string::npos) return std::nullopt;
    cfg.uuid = rest.substr(0, at_pos);
    std::string hostport = rest.substr(at_pos + 1);

    // Handle IPv6 [host]:port
    if (hostport.front() == '[') {
        auto bracket = hostport.find(']');
        if (bracket == std::string::npos) return std::nullopt;
        cfg.address = hostport.substr(1, bracket - 1);
        if (bracket + 1 < hostport.size() && hostport[bracket + 1] == ':') {
            std::string p = hostport.substr(bracket + 2);
            // Remove trailing non-digits
            p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
            try { cfg.port = std::stoi(p); } catch (...) { return std::nullopt; }
        }
    } else {
        auto colon = hostport.rfind(':');
        if (colon == std::string::npos) return std::nullopt;
        cfg.address = hostport.substr(0, colon);
        std::string p = hostport.substr(colon + 1);
        p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
        try { cfg.port = std::stoi(p); } catch (...) { return std::nullopt; }
    }

    // Apply params
    cfg.encryption = params.count("encryption") ? params["encryption"] : "none";
    cfg.security = params.count("security") ? params["security"] : "";
    cfg.network = params.count("type") ? params["type"] : "tcp";
    cfg.sni = params.count("sni") ? params["sni"] : "";
    cfg.host = params.count("host") ? params["host"] : "";
    cfg.path = params.count("path") ? params["path"] : "";
    cfg.fingerprint = params.count("fp") ? params["fp"] : "";
    cfg.public_key = params.count("pbk") ? params["pbk"] : "";
    cfg.short_id = params.count("sid") ? params["sid"] : "";
    cfg.flow = params.count("flow") ? params["flow"] : "";

    if (cfg.address.empty() || cfg.port <= 0) return std::nullopt;
    return cfg;
}

// ─── Trojan ───
std::optional<ParsedConfig> UriParser::parseTrojan(const std::string& uri) {
    // trojan://password@host:port?params#remark
    std::string rest = uri.substr(9); // skip "trojan://"

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "trojan";

    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }

    auto q_pos = rest.find('?');
    std::map<std::string, std::string> params;
    if (q_pos != std::string::npos) {
        params = parseQueryParams(rest.substr(q_pos + 1));
        rest = rest.substr(0, q_pos);
    }

    auto at_pos = rest.find('@');
    if (at_pos == std::string::npos) return std::nullopt;
    cfg.uuid = rest.substr(0, at_pos);
    std::string hostport = rest.substr(at_pos + 1);

    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    cfg.address = hostport.substr(0, colon);
    std::string p = hostport.substr(colon + 1);
    p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
    try { cfg.port = std::stoi(p); } catch (...) { return std::nullopt; }

    cfg.security = params.count("security") ? params["security"] : "tls";
    cfg.network = params.count("type") ? params["type"] : "tcp";
    cfg.sni = params.count("sni") ? params["sni"] : "";
    cfg.host = params.count("host") ? params["host"] : "";
    cfg.path = params.count("path") ? params["path"] : "";
    cfg.fingerprint = params.count("fp") ? params["fp"] : "";

    if (cfg.address.empty() || cfg.port <= 0) return std::nullopt;
    return cfg;
}

// ─── Shadowsocks ───
std::optional<ParsedConfig> UriParser::parseShadowsocks(const std::string& uri) {
    // ss://base64(method:password)@host:port#remark
    // or ss://base64(method:password@host:port)#remark
    std::string rest = uri.substr(5); // skip "ss://"

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "shadowsocks";

    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }

    auto at_pos = rest.find('@');
    if (at_pos != std::string::npos) {
        // Format: base64(method:password)@host:port
        std::string userinfo = utils::base64Decode(rest.substr(0, at_pos));
        std::string hostport = rest.substr(at_pos + 1);

        auto colon = userinfo.find(':');
        if (colon != std::string::npos) {
            cfg.encryption = userinfo.substr(0, colon);
            cfg.uuid = userinfo.substr(colon + 1);
        }

        colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            cfg.address = hostport.substr(0, colon);
            std::string p = hostport.substr(colon + 1);
            p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
            try { cfg.port = std::stoi(p); } catch (...) {}
        }
    } else {
        // Format: base64(method:password@host:port)
        std::string decoded = utils::base64Decode(rest);
        auto colon1 = decoded.find(':');
        auto at = decoded.find('@');
        if (colon1 != std::string::npos && at != std::string::npos && colon1 < at) {
            cfg.encryption = decoded.substr(0, colon1);
            cfg.uuid = decoded.substr(colon1 + 1, at - colon1 - 1);
            std::string hostport = decoded.substr(at + 1);
            auto colon2 = hostport.rfind(':');
            if (colon2 != std::string::npos) {
                cfg.address = hostport.substr(0, colon2);
                std::string p = hostport.substr(colon2 + 1);
                p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
                try { cfg.port = std::stoi(p); } catch (...) {}
            }
        }
    }

    if (cfg.address.empty() || cfg.port <= 0) return std::nullopt;
    return cfg;
}

// ─── Hysteria2 ───
std::optional<ParsedConfig> UriParser::parseHysteria2(const std::string& uri) {
    // hysteria2://auth@host:port?params#remark (or hy2://)
    std::string rest = uri;
    if (utils::startsWith(rest, "hysteria2://")) rest = rest.substr(12);
    else if (utils::startsWith(rest, "hy2://")) rest = rest.substr(6);
    else return std::nullopt;

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "hysteria2";

    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }

    auto q_pos = rest.find('?');
    std::map<std::string, std::string> params;
    if (q_pos != std::string::npos) {
        params = parseQueryParams(rest.substr(q_pos + 1));
        rest = rest.substr(0, q_pos);
    }

    auto at_pos = rest.find('@');
    if (at_pos != std::string::npos) {
        cfg.uuid = rest.substr(0, at_pos);
        rest = rest.substr(at_pos + 1);
    }

    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        cfg.address = rest.substr(0, colon);
        std::string p = rest.substr(colon + 1);
        p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
        try { cfg.port = std::stoi(p); } catch (...) {}
    } else {
        cfg.address = rest;
        cfg.port = 443;
    }

    cfg.sni = params.count("sni") ? params["sni"] : "";
    cfg.security = "tls";

    if (cfg.address.empty() || cfg.port <= 0) return std::nullopt;
    return cfg;
}

// ─── TUIC ───
std::optional<ParsedConfig> UriParser::parseTuic(const std::string& uri) {
    // tuic://uuid:password@host:port?params#remark
    std::string rest = uri.substr(7); // skip "tuic://"

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "tuic";

    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }

    auto q_pos = rest.find('?');
    std::map<std::string, std::string> params;
    if (q_pos != std::string::npos) {
        params = parseQueryParams(rest.substr(q_pos + 1));
        rest = rest.substr(0, q_pos);
    }

    auto at_pos = rest.find('@');
    if (at_pos != std::string::npos) {
        std::string userinfo = rest.substr(0, at_pos);
        auto colon = userinfo.find(':');
        if (colon != std::string::npos) {
            cfg.uuid = userinfo.substr(0, colon);
            cfg.extra["password"] = userinfo.substr(colon + 1);
        } else {
            cfg.uuid = userinfo;
        }
        rest = rest.substr(at_pos + 1);
    }

    auto colon = rest.rfind(':');
    if (colon != std::string::npos) {
        cfg.address = rest.substr(0, colon);
        std::string p = rest.substr(colon + 1);
        p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return !std::isdigit(c); }), p.end());
        try { cfg.port = std::stoi(p); } catch (...) {}
    }

    cfg.sni = params.count("sni") ? params["sni"] : "";
    cfg.security = "tls";

    if (cfg.address.empty() || cfg.port <= 0) return std::nullopt;
    return cfg;
}

// ─── Prioritize configs ───
std::vector<std::string> prioritizeConfigs(const std::vector<std::string>& uris) {
    struct ScoredUri {
        std::string uri;
        int score;
    };

    std::vector<ScoredUri> scored;
    scored.reserve(uris.size());

    for (const auto& uri : uris) {
        int score = 0;
        std::string lower = uri;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // Reality configs get highest priority
        if (lower.find("security=reality") != std::string::npos) score += 100;
        // VLESS preferred
        if (utils::startsWith(lower, "vless://")) score += 50;
        // gRPC transport
        if (lower.find("type=grpc") != std::string::npos) score += 30;
        // WebSocket (CDN-friendly)
        if (lower.find("type=ws") != std::string::npos) score += 25;
        // SplitHTTP
        if (lower.find("type=splithttp") != std::string::npos) score += 35;
        // TLS
        if (lower.find("security=tls") != std::string::npos) score += 20;
        // Fingerprint present
        if (lower.find("fp=") != std::string::npos) score += 10;
        // Trojan
        if (utils::startsWith(lower, "trojan://")) score += 40;
        // Hysteria2
        if (utils::startsWith(lower, "hysteria2://") || utils::startsWith(lower, "hy2://"))
            score += 45;

        scored.push_back({uri, score});
    }

    std::sort(scored.begin(), scored.end(),
              [](const ScoredUri& a, const ScoredUri& b) { return a.score > b.score; });

    std::vector<std::string> result;
    result.reserve(scored.size());
    for (auto& s : scored) result.push_back(std::move(s.uri));
    return result;
}

} // namespace network
} // namespace hunter
