#include "uri_parser.h"
#include "core/utils.h"

#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <regex>

namespace hunter {

// ---------- URL parsing helpers ----------

ParsedURL parse_url(const std::string& url) {
    ParsedURL result;

    std::string remaining = url;

    // Extract scheme
    auto scheme_end = remaining.find("://");
    if (scheme_end != std::string::npos) {
        result.scheme = remaining.substr(0, scheme_end);
        remaining = remaining.substr(scheme_end + 3);
    }

    // Extract fragment
    auto frag_pos = remaining.find('#');
    if (frag_pos != std::string::npos) {
        result.fragment = url_decode(remaining.substr(frag_pos + 1));
        remaining = remaining.substr(0, frag_pos);
    }

    // Extract query
    auto query_pos = remaining.find('?');
    if (query_pos != std::string::npos) {
        result.query = remaining.substr(query_pos + 1);
        remaining = remaining.substr(0, query_pos);
    }

    // Extract path
    auto path_pos = remaining.find('/');
    if (path_pos != std::string::npos) {
        result.path = remaining.substr(path_pos);
        remaining = remaining.substr(0, path_pos);
    }

    // Extract userinfo
    auto at_pos = remaining.find('@');
    if (at_pos != std::string::npos) {
        result.username = url_decode(remaining.substr(0, at_pos));
        remaining = remaining.substr(at_pos + 1);
    }

    // Handle IPv6 bracket notation
    if (!remaining.empty() && remaining[0] == '[') {
        auto bracket_end = remaining.find(']');
        if (bracket_end != std::string::npos) {
            result.hostname = remaining.substr(1, bracket_end - 1);
            remaining = remaining.substr(bracket_end + 1);
            if (!remaining.empty() && remaining[0] == ':') {
                try {
                    result.port = std::stoi(remaining.substr(1));
                } catch (...) {}
            }
        }
    } else {
        // Regular host:port
        auto colon_pos = remaining.rfind(':');
        if (colon_pos != std::string::npos) {
            result.hostname = remaining.substr(0, colon_pos);
            try {
                result.port = std::stoi(remaining.substr(colon_pos + 1));
            } catch (...) {
                result.hostname = remaining;
            }
        } else {
            result.hostname = remaining;
        }
    }

    return result;
}

std::unordered_map<std::string, std::string> parse_query_string(const std::string& query) {
    std::unordered_map<std::string, std::string> params;
    std::istringstream ss(query);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = url_decode(pair.substr(0, eq));
            std::string value = url_decode(pair.substr(eq + 1));
            params[key] = value;
        }
    }
    return params;
}

static std::string get_param(const std::unordered_map<std::string, std::string>& params,
                             const std::string& key, const std::string& default_val = "") {
    auto it = params.find(key);
    return (it != params.end()) ? it->second : default_val;
}

// ---------- VMess Parser ----------

std::optional<HunterParsedConfig> VMessParser::parse(const std::string& uri) {
    try {
        if (uri.size() < 9) return std::nullopt;
        std::string payload = uri.substr(8); // skip "vmess://"
        std::string decoded = safe_b64decode(payload);
        auto j = nlohmann::json::parse(decoded);

        std::string host = j.value("add", "");
        int port = 0;
        if (j["port"].is_number()) {
            port = j["port"].get<int>();
        } else if (j["port"].is_string()) {
            try { port = std::stoi(j["port"].get<std::string>()); } catch (...) {}
        }
        std::string uuid = j.value("id", "");
        std::string ps = clean_ps_string(j.value("ps", "Unknown"));

        if (host.empty() || port == 0 || uuid.empty() || host == "0.0.0.0") {
            return std::nullopt;
        }

        int alter_id = 0;
        if (j.contains("aid")) {
            if (j["aid"].is_number()) alter_id = j["aid"].get<int>();
            else if (j["aid"].is_string()) {
                try { alter_id = std::stoi(j["aid"].get<std::string>()); } catch (...) {}
            }
        }

        nlohmann::json outbound = {
            {"protocol", "vmess"},
            {"settings", {
                {"vnext", {{
                    {"address", host},
                    {"port", port},
                    {"users", {{
                        {"id", uuid},
                        {"alterId", alter_id},
                        {"security", j.value("scy", "auto")}
                    }}}
                }}}
            }},
            {"streamSettings", {
                {"network", j.value("net", "tcp")},
                {"security", j.value("tls", "none")}
            }}
        };

        if (j.value("net", "") == "ws") {
            outbound["streamSettings"]["wsSettings"] = {
                {"path", j.value("path", "/")},
                {"headers", {{"Host", j.value("host", "")}}}
            };
        }

        if (j.value("tls", "") == "tls") {
            outbound["streamSettings"]["tlsSettings"] = {
                {"serverName", j.value("sni", host)},
                {"allowInsecure", false}
            };
        }

        HunterParsedConfig config;
        config.uri = uri;
        config.outbound = outbound;
        config.host = host;
        config.port = port;
        config.identity = uuid;
        config.ps = ps;
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------- VLESS Parser ----------

std::optional<HunterParsedConfig> VLESSParser::parse(const std::string& uri) {
    try {
        ParsedURL parsed = parse_url(uri);
        auto params = parse_query_string(parsed.query);

        std::string uuid = parsed.username;
        std::string host = parsed.hostname;
        int port = parsed.port > 0 ? parsed.port : 443;
        std::string ps = clean_ps_string(parsed.fragment.empty() ? "Unknown" : parsed.fragment);

        if (host.empty() || uuid.empty() || host == "0.0.0.0") {
            return std::nullopt;
        }

        std::string security = get_param(params, "security", "none");
        std::string transport = get_param(params, "type", "tcp");
        std::string encryption = get_param(params, "encryption", "none");

        nlohmann::json outbound = {
            {"protocol", "vless"},
            {"settings", {
                {"vnext", {{
                    {"address", host},
                    {"port", port},
                    {"users", {{
                        {"id", uuid},
                        {"encryption", encryption}
                    }}}
                }}}
            }},
            {"streamSettings", {
                {"network", transport},
                {"security", security}
            }}
        };

        if (security == "tls" || security == "reality") {
            nlohmann::json base_settings = {
                {"serverName", get_param(params, "sni", host)},
                {"allowInsecure", false}
            };

            if (security == "reality") {
                base_settings["fingerprint"] = get_param(params, "fp", "chrome");
                base_settings["publicKey"] = get_param(params, "pbk", "");
                base_settings["shortId"] = get_param(params, "sid", "");
                outbound["streamSettings"]["realitySettings"] = base_settings;
            } else {
                outbound["streamSettings"]["tlsSettings"] = base_settings;
            }
        }

        if (transport == "ws") {
            outbound["streamSettings"]["wsSettings"] = {
                {"path", get_param(params, "path", "/")},
                {"headers", {{"Host", get_param(params, "host", "")}}}
            };
        } else if (transport == "grpc") {
            outbound["streamSettings"]["grpcSettings"] = {
                {"serviceName", get_param(params, "serviceName", "")}
            };
        }

        HunterParsedConfig config;
        config.uri = uri;
        config.outbound = outbound;
        config.host = host;
        config.port = port;
        config.identity = uuid;
        config.ps = ps;
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------- Trojan Parser ----------

std::optional<HunterParsedConfig> TrojanParser::parse(const std::string& uri) {
    try {
        ParsedURL parsed = parse_url(uri);
        auto params = parse_query_string(parsed.query);

        std::string password = parsed.username;
        std::string host = parsed.hostname;
        int port = parsed.port > 0 ? parsed.port : 443;
        std::string ps = clean_ps_string(parsed.fragment.empty() ? "Unknown" : parsed.fragment);

        if (host.empty() || password.empty() || host == "0.0.0.0") {
            return std::nullopt;
        }

        std::string transport = get_param(params, "type", "tcp");
        bool allow_insecure = (get_param(params, "allowInsecure", "0") == "1");

        nlohmann::json outbound = {
            {"protocol", "trojan"},
            {"settings", {
                {"servers", {{
                    {"address", host},
                    {"port", port},
                    {"password", password}
                }}}
            }},
            {"streamSettings", {
                {"network", transport},
                {"security", "tls"},
                {"tlsSettings", {
                    {"serverName", get_param(params, "sni", host)},
                    {"allowInsecure", allow_insecure}
                }}
            }}
        };

        HunterParsedConfig config;
        config.uri = uri;
        config.outbound = outbound;
        config.host = host;
        config.port = port;
        config.identity = password;
        config.ps = ps;
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------- Shadowsocks Parser ----------

std::optional<HunterParsedConfig> ShadowsocksParser::parse(const std::string& uri) {
    try {
        std::string parsed_uri = uri;
        std::string ps = "Unknown";

        auto hash_pos = parsed_uri.find('#');
        if (hash_pos != std::string::npos) {
            ps = clean_ps_string(url_decode(parsed_uri.substr(hash_pos + 1)));
            parsed_uri = parsed_uri.substr(0, hash_pos);
        }

        std::string core = parsed_uri.substr(5); // skip "ss://"

        // Remove query string for base64 decoding
        std::string query_part;
        auto q_pos = core.find('?');
        if (q_pos != std::string::npos) {
            query_part = core.substr(q_pos);
            core = core.substr(0, q_pos);
        }

        if (core.find('@') == std::string::npos) {
            core = safe_b64decode(core);
        }

        if (core.find('@') == std::string::npos) {
            return std::nullopt;
        }

        auto at_pos = core.find('@');
        std::string userinfo = core.substr(0, at_pos);
        std::string hostport = core.substr(at_pos + 1);

        if (hostport.find(':') == std::string::npos) {
            return std::nullopt;
        }

        std::string method, password;

        // Try base64-decode userinfo first
        try {
            std::string decoded = safe_b64decode(userinfo);
            auto colon = decoded.find(':');
            if (colon != std::string::npos) {
                method = decoded.substr(0, colon);
                password = decoded.substr(colon + 1);
            }
        } catch (...) {}

        if (method.empty() || password.empty()) {
            auto colon = userinfo.find(':');
            if (colon != std::string::npos) {
                method = userinfo.substr(0, colon);
                password = userinfo.substr(colon + 1);
            } else {
                return std::nullopt;
            }
        }

        auto last_colon = hostport.rfind(':');
        std::string host = hostport.substr(0, last_colon);
        std::string port_str = hostport.substr(last_colon + 1);

        // Extract just digits from port
        std::string port_digits;
        for (char c : port_str) {
            if (std::isdigit(c)) port_digits += c;
            else break;
        }
        int port = 0;
        try { port = std::stoi(port_digits); } catch (...) { return std::nullopt; }

        if (host.empty() || port == 0 || host == "0.0.0.0") {
            return std::nullopt;
        }

        nlohmann::json outbound = {
            {"protocol", "shadowsocks"},
            {"settings", {
                {"servers", {{
                    {"address", host},
                    {"port", port},
                    {"method", method},
                    {"password", password}
                }}}
            }}
        };

        HunterParsedConfig config;
        config.uri = uri;
        config.outbound = outbound;
        config.host = host;
        config.port = port;
        config.identity = method + ":" + password;
        config.ps = ps;
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

// ---------- Universal Parser ----------

std::optional<HunterParsedConfig> UniversalParser::parse(const std::string& uri) const {
    if (uri.empty() || uri.find("://") == std::string::npos) {
        return std::nullopt;
    }

    auto scheme_end = uri.find("://");
    std::string proto = to_lower(uri.substr(0, scheme_end));

    if (proto == "vmess") return VMessParser::parse(uri);
    if (proto == "vless") return VLESSParser::parse(uri);
    if (proto == "trojan") return TrojanParser::parse(uri);
    if (proto == "ss" || proto == "shadowsocks") return ShadowsocksParser::parse(uri);

    return std::nullopt;
}

} // namespace hunter
