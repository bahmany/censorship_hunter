#include "network/uri_parser.h"
#include "core/utils.h"
#include "core/constants.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace hunter {
namespace network {

namespace {

// Strict port: 1-5 ASCII digits only, 1..65535 ("443abc", "4-43", "", "0" are rejected).
bool parsePortStrict(const std::string& s, int* out) {
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    *out = v;
    return true;
}

bool isIpv6Literal(const std::string& h) {
    unsigned char buf[16];
    return h.find(':') != std::string::npos && inet_pton(AF_INET6, h.c_str(), buf) == 1;
}

bool hostCharsOk(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (isIpv6Literal(h)) return true;
    for (unsigned char c : h) {
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

// host:port | [v6]:port | host (when port_optional). Unbracketed IPv6 is accepted only as a bare
// literal without port. Anything with a trailing path ("/...") must be stripped by the caller.
bool splitHostPort(const std::string& hp, std::string* host, int* port, bool port_optional, int default_port) {
    if (hp.empty()) return false;
    if (hp.front() == '[') {
        auto rb = hp.find(']');
        if (rb == std::string::npos) return false;
        std::string h = hp.substr(1, rb - 1);
        if (!isIpv6Literal(h)) return false;
        *host = h;
        if (rb + 1 == hp.size()) {
            if (!port_optional) return false;
            *port = default_port;
            return true;
        }
        if (hp[rb + 1] != ':') return false;
        return parsePortStrict(hp.substr(rb + 2), port);
    }
    if (std::count(hp.begin(), hp.end(), ':') > 1) {
        if (!port_optional || !isIpv6Literal(hp)) return false;
        *host = hp;
        *port = default_port;
        return true;
    }
    auto colon = hp.rfind(':');
    if (colon == std::string::npos) {
        if (!port_optional || !hostCharsOk(hp)) return false;
        *host = hp;
        *port = default_port;
        return true;
    }
    std::string h = hp.substr(0, colon);
    if (!hostCharsOk(h)) return false;
    *host = h;
    return parsePortStrict(hp.substr(colon + 1), port);
}

// Percent-decode WITHOUT turning '+' into a space (credentials keep '+').
std::string pctDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string b64UrlDecode(std::string s) {
    for (auto& c : s) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    return utils::base64Decode(s);
}

// Pieces shared by all "scheme://userinfo@host:port/path?query#frag" URIs.
struct Split {
    std::string userinfo;
    bool has_userinfo = false;
    std::string hostport;
    std::string frag;
    std::map<std::string, std::string> params;
};

Split splitUri(std::string rest) {
    Split sp;
    auto hash = rest.find('#');
    if (hash != std::string::npos) { sp.frag = utils::urlDecode(rest.substr(hash + 1)); rest = rest.substr(0, hash); }
    auto q = rest.find('?');
    if (q != std::string::npos) {
        sp.params = UriParser::parseQueryParams(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    auto at = rest.rfind('@');
    if (at != std::string::npos) { sp.userinfo = rest.substr(0, at); sp.has_userinfo = true; rest = rest.substr(at + 1); }
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);   // "host:443/" and "host:443/path"
    sp.hostport = rest;
    return sp;
}

std::string param(const std::map<std::string, std::string>& m, const char* k, const std::string& def = "") {
    auto it = m.find(k);
    return it == m.end() ? def : it->second;
}

void retainOptions(ParsedConfig& cfg, const std::map<std::string, std::string>& params) {
    for (const auto& kv : params) cfg.options[kv.first] = kv.second;   // full option map, nothing dropped
}

// Minimal strict parser for a FLAT JSON object of scalars (vmess share links).
// Returns false on nested values / malformed text so callers can fall back.
bool parseFlatJson(const std::string& j, std::map<std::string, std::string>* out) {
    size_t i = 0, n = j.size();
    auto ws = [&] { while (i < n && std::isspace((unsigned char)j[i])) i++; };
    auto str = [&](std::string* r) -> bool {
        if (i >= n || j[i] != '"') return false;
        i++;
        r->clear();
        while (i < n && j[i] != '"') {
            if (j[i] == '\\') {
                i++;
                if (i >= n) return false;
                switch (j[i]) {
                    case 'n': *r += '\n'; break; case 't': *r += '\t'; break; case 'r': *r += '\r'; break;
                    case 'b': *r += '\b'; break; case 'f': *r += '\f'; break;
                    case 'u': {
                        if (i + 4 >= n) return false;
                        unsigned cp = 0;
                        for (int k = 1; k <= 4; k++) { if (!std::isxdigit((unsigned char)j[i + k])) return false; cp = cp * 16 + (unsigned)std::stoi(std::string(1, j[i + k]), nullptr, 16); }
                        i += 4;
                        if (cp < 0x80) *r += (char)cp;
                        else if (cp < 0x800) { *r += (char)(0xC0 | (cp >> 6)); *r += (char)(0x80 | (cp & 0x3F)); }
                        else { *r += (char)(0xE0 | (cp >> 12)); *r += (char)(0x80 | ((cp >> 6) & 0x3F)); *r += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: *r += j[i]; break;   // \" \\ \/
                }
                i++;
            } else *r += j[i++];
        }
        if (i >= n) return false;
        i++;
        return true;
    };
    ws();
    if (i >= n || j[i] != '{') return false;
    i++;
    ws();
    if (i < n && j[i] == '}') return true;
    while (i < n) {
        ws();
        std::string k, v;
        if (!str(&k)) return false;
        ws();
        if (i >= n || j[i] != ':') return false;
        i++;
        ws();
        if (i >= n) return false;
        if (j[i] == '"') { if (!str(&v)) return false; }
        else if (j[i] == '{' || j[i] == '[') return false;   // nested: not a flat vmess object
        else {
            size_t e = j.find_first_of(",}", i);
            if (e == std::string::npos) return false;
            v = utils::trim(j.substr(i, e - i));
            i = e;
        }
        (*out)[k] = v;
        ws();
        if (i < n && j[i] == ',') { i++; continue; }
        if (i < n && j[i] == '}') return true;
        return false;
    }
    return false;
}

}  // namespace

bool UriParser::isValidScheme(const std::string& uri) {
    for (const auto& scheme : constants::supportedSchemes()) {
        if (utils::startsWith(uri, scheme + "://")) return true;
    }
    return false;
}

std::map<std::string, std::string> UriParser::parseQueryParams(const std::string& query) {
    std::map<std::string, std::string> params;
    for (const auto& pair : utils::split(query, '&')) {
        if (pair.empty()) continue;
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
    std::string payload = uri.substr(8);
    auto hash_pos = payload.find('#');
    std::string remark;
    if (hash_pos != std::string::npos) {
        remark = utils::urlDecode(payload.substr(hash_pos + 1));
        payload = payload.substr(0, hash_pos);
    }
    std::string json = b64UrlDecode(payload);
    if (json.empty() || json.find('{') == std::string::npos) return std::nullopt;

    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "vmess";
    cfg.ps = remark;

    std::map<std::string, std::string> f;
    if (!parseFlatJson(json, &f)) {
        // Legacy tolerant extraction (nested/odd JSON) - keeps previously parseable links alive.
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
            auto end = json.find_first_of(",}", pos);
            if (end == std::string::npos) end = json.size();
            return utils::trim(json.substr(pos, end - pos));
        };
        for (const char* k : {"add", "port", "id", "scy", "net", "tls", "sni", "host", "path", "type", "fp", "ps", "alpn", "aid", "allowInsecure"})
            f[k] = extractField(k);
    }
    cfg.address = f["add"];
    if (!parsePortStrict(f["port"], &cfg.port)) return std::nullopt;
    cfg.uuid = f["id"];
    cfg.encryption = f["scy"].empty() ? "auto" : f["scy"];
    cfg.network = f["net"].empty() ? "tcp" : f["net"];
    cfg.security = f["tls"];
    cfg.sni = f["sni"];
    cfg.host = f["host"];
    cfg.path = f["path"];
    cfg.type = f["type"];
    cfg.fingerprint = f["fp"];
    if (cfg.ps.empty()) cfg.ps = f["ps"];
    for (const auto& kv : f) cfg.options[kv.first] = kv.second;
    if (cfg.network == "grpc" && !cfg.path.empty()) cfg.extra["serviceName"] = cfg.path;   // vmess keeps it in "path"

    if (!hostCharsOk(cfg.address) || cfg.uuid.empty()) return std::nullopt;
    return cfg;
}

// ─── VLESS ───
std::optional<ParsedConfig> UriParser::parseVless(const std::string& uri) {
    Split sp = splitUri(uri.substr(8));
    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "vless";
    cfg.ps = sp.frag;
    if (!sp.has_userinfo) return std::nullopt;
    cfg.uuid = pctDecode(sp.userinfo);
    if (!splitHostPort(sp.hostport, &cfg.address, &cfg.port, false, 0)) return std::nullopt;
    const auto& p = sp.params;
    retainOptions(cfg, p);
    cfg.encryption = param(p, "encryption", "none");
    cfg.security = param(p, "security");
    cfg.network = param(p, "type", "tcp");
    cfg.sni = param(p, "sni");
    cfg.host = param(p, "host");
    cfg.path = param(p, "path");
    cfg.fingerprint = param(p, "fp");
    cfg.public_key = param(p, "pbk");
    cfg.short_id = param(p, "sid");
    cfg.flow = param(p, "flow");
    cfg.type = param(p, "headerType");
    if (p.count("serviceName")) cfg.extra["serviceName"] = p.at("serviceName");
    if (cfg.uuid.empty()) return std::nullopt;
    return cfg;
}

// ─── Trojan ───
std::optional<ParsedConfig> UriParser::parseTrojan(const std::string& uri) {
    Split sp = splitUri(uri.substr(9));
    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "trojan";
    cfg.ps = sp.frag;
    if (!sp.has_userinfo) return std::nullopt;
    cfg.uuid = pctDecode(sp.userinfo);
    if (!splitHostPort(sp.hostport, &cfg.address, &cfg.port, false, 0)) return std::nullopt;
    const auto& p = sp.params;
    retainOptions(cfg, p);
    cfg.security = param(p, "security", "tls");
    cfg.network = param(p, "type", "tcp");
    cfg.sni = param(p, "sni");
    cfg.host = param(p, "host");
    cfg.path = param(p, "path");
    cfg.fingerprint = param(p, "fp");
    cfg.type = param(p, "headerType");
    if (p.count("serviceName")) cfg.extra["serviceName"] = p.at("serviceName");
    return cfg;
}

// ─── Shadowsocks ───
std::optional<ParsedConfig> UriParser::parseShadowsocks(const std::string& uri) {
    // SIP002:  ss://base64url(method:password)@host:port[/][?plugin=...][#tag]
    //          ss://method:password@host:port   (plain, percent-encoded userinfo)
    // Legacy:  ss://base64(method:password@host:port)[#tag]
    std::string rest = uri.substr(5);
    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "shadowsocks";

    auto hash_pos = rest.find('#');
    if (hash_pos != std::string::npos) {
        cfg.ps = utils::urlDecode(rest.substr(hash_pos + 1));
        rest = rest.substr(0, hash_pos);
    }
    std::map<std::string, std::string> params;
    auto q = rest.find('?');
    if (q != std::string::npos) {   // the query is split off BEFORE any port parsing (plugin digits never leak into the port)
        params = parseQueryParams(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    retainOptions(cfg, params);
    if (params.count("plugin")) cfg.extra["plugin"] = params.at("plugin");

    auto at_pos = rest.rfind('@');
    if (at_pos != std::string::npos) {
        std::string raw_user = rest.substr(0, at_pos);
        std::string userinfo = pctDecode(raw_user);
        if (userinfo.find(':') == std::string::npos) userinfo = b64UrlDecode(userinfo);   // base64 form
        auto colon = userinfo.find(':');
        if (colon == std::string::npos) return std::nullopt;
        cfg.encryption = userinfo.substr(0, colon);
        cfg.uuid = userinfo.substr(colon + 1);
        std::string hostport = rest.substr(at_pos + 1);
        if (!splitHostPort(hostport, &cfg.address, &cfg.port, false, 0)) return std::nullopt;
    } else {
        std::string decoded = b64UrlDecode(pctDecode(rest));
        auto at = decoded.rfind('@');
        auto colon1 = decoded.find(':');
        if (at == std::string::npos || colon1 == std::string::npos || colon1 > at) return std::nullopt;
        cfg.encryption = decoded.substr(0, colon1);
        cfg.uuid = decoded.substr(colon1 + 1, at - colon1 - 1);
        if (!splitHostPort(decoded.substr(at + 1), &cfg.address, &cfg.port, false, 0)) return std::nullopt;
    }
    if (cfg.encryption.empty()) return std::nullopt;
    return cfg;
}

// ─── Hysteria2 ───
std::optional<ParsedConfig> UriParser::parseHysteria2(const std::string& uri) {
    std::string rest = uri;
    if (utils::startsWith(rest, "hysteria2://")) rest = rest.substr(12);
    else if (utils::startsWith(rest, "hy2://")) rest = rest.substr(6);
    else return std::nullopt;

    Split sp = splitUri(rest);
    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "hysteria2";
    cfg.ps = sp.frag;
    if (sp.has_userinfo) cfg.uuid = pctDecode(sp.userinfo);
    if (!splitHostPort(sp.hostport, &cfg.address, &cfg.port, true, 443)) return std::nullopt;
    const auto& p = sp.params;
    retainOptions(cfg, p);
    cfg.sni = param(p, "sni");
    cfg.security = "tls";
    for (const char* k : {"obfs", "obfs-password", "pinSHA256", "alpn", "up", "down", "upmbps", "downmbps"})
        if (p.count(k)) cfg.extra[k] = p.at(k);
    return cfg;
}

// ─── TUIC ───
std::optional<ParsedConfig> UriParser::parseTuic(const std::string& uri) {
    Split sp = splitUri(uri.substr(7));
    ParsedConfig cfg;
    cfg.uri = uri;
    cfg.protocol = "tuic";
    cfg.ps = sp.frag;
    if (sp.has_userinfo) {
        std::string userinfo = sp.userinfo;
        auto colon = userinfo.find(':');
        if (colon != std::string::npos) {
            cfg.uuid = pctDecode(userinfo.substr(0, colon));
            cfg.extra["password"] = pctDecode(userinfo.substr(colon + 1));
        } else {
            cfg.uuid = pctDecode(userinfo);
        }
    }
    if (!splitHostPort(sp.hostport, &cfg.address, &cfg.port, false, 0)) return std::nullopt;
    const auto& p = sp.params;
    retainOptions(cfg, p);
    cfg.sni = param(p, "sni");
    cfg.security = "tls";
    for (const char* k : {"congestion_control", "udp_relay_mode", "alpn"})
        if (p.count(k)) cfg.extra[k] = p.at(k);
    if (cfg.uuid.empty()) return std::nullopt;
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
