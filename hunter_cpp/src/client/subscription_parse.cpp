#include "client/subscription_parse.h"
#include "network/uri_parser.h"
#include "core/utils.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace hunter {
namespace client {

namespace {

// Check if a string consists only of base64 characters and whitespace
bool isBase64Blob(const std::string& s) {
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) {
            return false;
        }
    }
    return true;
}

// Split string by any of \r\n|\n|\r
std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// Trim whitespace from both ends (using utils::trim)
std::string trimLine(const std::string& s) {
    return utils::trim(s);
}

// Check if a string starts with any of the supported scheme prefixes
bool hasSupportedScheme(const std::string& s) {
    // Heap-allocated and never destroyed: safe if exit() runs while workers are live.
    static const std::vector<std::string>& prefixes = *new std::vector<std::string>{
        "vmess://", "vless://", "trojan://", "ss://", "ssr://",
        "hysteria2://", "hy2://", "tuic://", "wireguard://",
        "socks://", "socks5://", "anytls://"
    };
    for (const auto& prefix : prefixes) {
        if (utils::startsWith(s, prefix)) {
            return true;
        }
    }
    return false;
}

// Parse a single Clash proxy (either block or flow style) into a URI
// Split `k: v` pairs on commas/newlines at brace depth 0, respecting quotes.
std::vector<std::string> splitClashPairs(const std::string& s) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; cur += c; continue; }
        if (c == '{' || c == '[') { ++depth; cur += c; continue; }
        if (c == '}' || c == ']') { --depth; cur += c; continue; }
        if ((c == ',' || c == '\n') && depth == 0) {
            if (!utils::trim(cur).empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!utils::trim(cur).empty()) out.push_back(cur);
    return out;
}

// First ':' at brace depth 0 outside quotes; npos if none.
size_t findPairColon(const std::string& s) {
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        else if (c == ':' && depth == 0) return i;
    }
    return std::string::npos;
}

std::string unquote(std::string v) {
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        return v.substr(1, v.size() - 2);
    return v;
}

std::string parseClashProxy(const std::string& proxyStr) {
    // Collect key-value pairs from a flow `{...}` item or a block blob of `k: v` lines.
    std::map<std::string, std::string> kv;
    std::string remaining = utils::trim(proxyStr);

    // Remove outer braces if present (flow style)
    if (!remaining.empty() && remaining[0] == '{' && remaining.back() == '}') {
        remaining = remaining.substr(1, remaining.size() - 2);
    }

    for (const std::string& pair : splitClashPairs(remaining)) {
        size_t eqPos = findPairColon(pair);
        if (eqPos == std::string::npos) continue;
        std::string key = unquote(utils::trim(pair.substr(0, eqPos)));
        std::string value = unquote(utils::trim(pair.substr(eqPos + 1)));
        if (!key.empty()) kv[key] = value;
    }

    // Now extract fields
    auto itType = kv.find("type");
    if (itType == kv.end()) return "";
    std::string type = itType->second;

    auto itServer = kv.find("server");
    if (itServer == kv.end()) return "";
    std::string server = itServer->second;

    auto itPort = kv.find("port");
    if (itPort == kv.end()) return "";
    int port;
    try {
        port = std::stoi(itPort->second);
    } catch (...) {
        return "";
    }
    if (port < 1 || port > 65535) return "";

    auto itName = kv.find("name");
    std::string name = (itName != kv.end()) ? itName->second : "";

    // Build URI based on type
    if (type == "vmess") {
        auto itUuid = kv.find("uuid");
        if (itUuid == kv.end()) return "";
        std::string uuid = itUuid->second;

        std::string alterId = kv.count("alterId") ? kv["alterId"] : "0";
        std::string cipher = kv.count("cipher") ? kv["cipher"] : "auto";
        std::string network = kv.count("network") ? kv["network"] : "tcp";
        std::string tls = kv.count("tls") && (kv["tls"] == "true" || kv["tls"] == "True") ? "tls" : "";
        std::string sni = kv.count("servername") ? kv["servername"] : "";
        bool skipCertVerify = kv.count("skip-cert-verify") && 
                              (kv["skip-cert-verify"] == "true" || kv["skip-cert-verify"] == "True");

        std::string wsPath = "";
        std::string wsHost = "";
        if (network == "ws" || network == "websocket") {
            // In our simplified parsing, we don't have nested structure
            // We'll assume the ws-opts are flattened with prefix
            // For flow style, we might have ws-opts.path etc.
            // We'll look for keys starting with "ws-opts."
            for (const auto& kvp : kv) {
                if (utils::startsWith(kvp.first, "ws-opts.")) {
                    std::string subkey = kvp.first.substr(8); // after "ws-opts."
                    if (subkey == "path") {
                        wsPath = kvp.second;
                    } else if (subkey == "headers") {
                        // Headers might be a map, we'll look for Host
                        // For simplicity, assume it's a string like "{Host: h.com}"
                        std::string headers = kvp.second;
                        if (headers.front() == '{' && headers.back() == '}') {
                            headers = headers.substr(1, headers.size() - 2);
                            size_t hostPos = headers.find("Host:");
                            if (hostPos != std::string::npos) {
                                size_t valueStart = hostPos + 5;
                                while (valueStart < headers.size() && 
                                       (headers[valueStart] == ' ' || headers[valueStart] == '\t')) {
                                    valueStart++;
                                }
                                size_t valueEnd = headers.find(',', valueStart);
                                if (valueEnd == std::string::npos) valueEnd = headers.size();
                                wsHost = headers.substr(valueStart, valueEnd - valueStart);
                                // Trim quotes
                                if (!wsHost.empty() && (wsHost.front() == '"' || wsHost.front() == '\'')) {
                                    wsHost = wsHost.substr(1, wsHost.size() - 2);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Build vmess JSON
        std::string json = "{"
            "\"v\":\"2\","
            "\"ps\":\"" + utils::urlEncode(name) + "\","
            "\"add\":\"" + server + "\","
            "\"port\":" + std::to_string(port) + ","
            "\"id\":\"" + uuid + "\","
            "\"aid\":\"" + alterId + "\","
            "\"scy\":\"" + cipher + "\","
            "\"net\":\"" + (network == "websocket" ? "ws" : network) + "\","
            "\"type\":\"none\","
            "\"host\":\"" + (wsHost.empty() ? server : wsHost) + "\","
            "\"path\":\"" + wsPath + "\","
            "\"tls\":\"" + (tls.empty() ? "" : tls) + "\","
            "\"sni\":\"" + sni + "\""
            "}";

        std::string b64 = utils::base64Encode(json);
        return "vmess://" + b64 + (name.empty() ? "" : "#" + utils::urlEncode(name));
    } else if (type == "vless") {
        auto itUuid = kv.find("uuid");
        if (itUuid == kv.end()) return "";
        std::string uuid = itUuid->second;

        std::string encryption = kv.count("encryption") ? kv["encryption"] : "none";
        std::string network = kv.count("network") ? kv["network"] : "tcp";
        std::string tls = kv.count("tls") && (kv["tls"] == "true" || kv["tls"] == "True") ? "tls" : "";
        std::string sni = kv.count("servername") ? kv["servername"] : "";
        bool skipCertVerify = kv.count("skip-cert-verify") && 
                              (kv["skip-cert-verify"] == "true" || kv["skip-cert-verify"] == "True");
        std::string flow = kv.count("flow") ? kv["flow"] : "";
        std::string fp = kv.count("fp") ? kv["fp"] : "";
        std::string typeHeader = kv.count("type") ? kv["type"] : ""; // headerType

        std::string wsPath = "";
        std::string wsHost = "";
        if (network == "ws" || network == "websocket") {
            for (const auto& kvp : kv) {
                if (utils::startsWith(kvp.first, "ws-opts.")) {
                    std::string subkey = kvp.first.substr(8);
                    if (subkey == "path") {
                        wsPath = kvp.second;
                    } else if (subkey == "headers") {
                        std::string headers = kvp.second;
                        if (headers.front() == '{' && headers.back() == '}') {
                            headers = headers.substr(1, headers.size() - 2);
                            size_t hostPos = headers.find("Host:");
                            if (hostPos != std::string::npos) {
                                size_t valueStart = hostPos + 5;
                                while (valueStart < headers.size() && 
                                       (headers[valueStart] == ' ' || headers[valueStart] == '\t')) {
                                    valueStart++;
                                }
                                size_t valueEnd = headers.find(',', valueStart);
                                if (valueEnd == std::string::npos) valueEnd = headers.size();
                                wsHost = headers.substr(valueStart, valueEnd - valueStart);
                                if (!wsHost.empty() && (wsHost.front() == '"' || wsHost.front() == '\'')) {
                                    wsHost = wsHost.substr(1, wsHost.size() - 2);
                                }
                            }
                        }
                    }
                }
            }
        }

        std::string uri = "vless://" + uuid + "@" + server + ":" + std::to_string(port);
        bool hasQuery = false;
        std::string queryStr;
        auto addQueryParam = [&](const std::string& key, const std::string& value) {
            queryStr += (hasQuery ? "&" : "?") + key + "=" + value;
            hasQuery = true;
        };
        if (!encryption.empty() && encryption != "none") {
            addQueryParam("encryption", encryption);
        }
        if (!network.empty() && network != "tcp") {
            addQueryParam("type", network);
        }
        if (!tls.empty()) {
            addQueryParam("security", tls);
        }
        if (!sni.empty()) {
            addQueryParam("sni", utils::urlEncode(sni));
        }
        if (!skipCertVerify) {
            addQueryParam("allowInsecure", "1");
        }
        if (!flow.empty()) {
            addQueryParam("flow", utils::urlEncode(flow));
        }
        if (!fp.empty()) {
            addQueryParam("fp", utils::urlEncode(fp));
        }
        if (!typeHeader.empty()) {
            addQueryParam("headerType", utils::urlEncode(typeHeader));
        }
        if (!wsHost.empty() && (network == "ws" || network == "websocket")) {
            addQueryParam("host", utils::urlEncode(wsHost));
        }
        if (!wsPath.empty() && (network == "ws" || network == "websocket")) {
            addQueryParam("path", utils::urlEncode(wsPath));
        }

        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        if (hasQuery) {
            uri += queryStr;
        }
        return uri;
    } else if (type == "ss") {
        auto itCipher = kv.find("cipher");
        if (itCipher == kv.end()) return "";
        std::string cipher = itCipher->second;
        auto itPassword = kv.find("password");
        if (itPassword == kv.end()) return "";
        std::string password = itPassword->second;

        std::string uri = "ss://" + utils::base64Encode(cipher + ":" + password) + 
                          "@" + server + ":" + std::to_string(port);
        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        return uri;
    } else if (type == "trojan") {
        auto itPassword = kv.find("password");
        if (itPassword == kv.end()) return "";
        std::string password = itPassword->second;

        std::string sni = kv.count("servername") ? kv["servername"] : "";
        bool skipCertVerify = kv.count("skip-cert-verify") && 
                              (kv["skip-cert-verify"] == "true" || kv["skip-cert-verify"] == "True");

        std::string uri = "trojan://" + password + "@" + server + ":" + 
                          std::to_string(port);
        bool hasQuery = false;
        std::string queryStr;
        auto addQueryParam = [&](const std::string& key, const std::string& value) {
            queryStr += (hasQuery ? "&" : "?") + key + "=" + value;
            hasQuery = true;
        };
        if (!sni.empty()) {
            uri += "?sni=" + utils::urlEncode(sni);
            hasQuery = true;
        }
        if (skipCertVerify) {
            addQueryParam("allowInsecure", "1");
        }
        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        return uri;
    } else if (type == "hysteria2" || type == "hy2") {
        auto itPassword = kv.find("password");
        if (itPassword == kv.end()) return "";
        std::string password = itPassword->second;

        std::string sni = kv.count("servername") ? kv["servername"] : "";
        bool insecure = kv.count("insecure") && 
                        (kv["insecure"] == "true" || kv["insecure"] == "True");

        std::string uri = "hysteria2://" + password + "@" + server + ":" + 
                          std::to_string(port);
        bool hasQuery = false;
        std::string queryStr;
        auto addQueryParam = [&](const std::string& key, const std::string& value) {
            queryStr += (hasQuery ? "&" : "?") + key + "=" + value;
            hasQuery = true;
        };
        if (!sni.empty()) {
            uri += "?sni=" + utils::urlEncode(sni);
            hasQuery = true;
        }
        if (insecure) {
            addQueryParam("insecure", "1");
        }
        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        return uri;
    } else if (type == "tuic") {
        auto itUuid = kv.find("uuid");
        if (itUuid == kv.end()) return "";
        std::string uuid = itUuid->second;
        auto itPassword = kv.find("password");
        if (itPassword == kv.end()) return "";
        std::string password = itPassword->second;

        std::string sni = kv.count("servername") ? kv["servername"] : "";
        bool insecure = kv.count("insecure") && 
                        (kv["insecure"] == "true" || kv["insecure"] == "True");
        std::string congestionControl = kv.count("congestion_control") ? 
                                        kv["congestion_control"] : "bbr";

        std::string uri = "tuic://" + uuid + ":" + password + "@" + server + ":" + 
                          std::to_string(port);
        bool hasQuery = false;
        std::string queryStr;
        auto addQueryParam = [&](const std::string& key, const std::string& value) {
            queryStr += (hasQuery ? "&" : "?") + key + "=" + value;
            hasQuery = true;
        };
        if (!sni.empty()) {
            uri += "?sni=" + utils::urlEncode(sni);
            hasQuery = true;
        }
        if (insecure) {
            addQueryParam("allow_insecure", "1");
        }
        if (!congestionControl.empty() && congestionControl != "bbr") {
            addQueryParam("congestion_control", utils::urlEncode(congestionControl));
        }
        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        return uri;
    } else if (type == "wireguard") {
        auto itPrivateKey = kv.find("private-key");
        if (itPrivateKey == kv.end()) itPrivateKey = kv.find("private_key");
        if (itPrivateKey == kv.end()) return "";
        std::string privateKey = itPrivateKey->second;
        // mihomo uses `public-key`; some exporters use `publickey`
        std::string publicKey;
        if (kv.count("publickey")) publicKey = kv["publickey"];
        else if (kv.count("public-key")) publicKey = kv["public-key"];
        if (publicKey.empty()) return "";
        std::string reserved = kv.count("reserved") ? kv["reserved"] : "0";

        std::string uri = "wireguard://" + utils::urlEncode(privateKey) + "@" +
                          server + ":" + std::to_string(port);
        bool hasQuery = false;
        std::string queryStr;
        auto addQueryParam = [&](const std::string& key, const std::string& value) {
            queryStr += (hasQuery ? "&" : "?") + key + "=" + value;
            hasQuery = true;
        };
        if (!publicKey.empty()) {
            addQueryParam("publickey", utils::urlEncode(publicKey));
        }
        if (!reserved.empty() && reserved != "0") {
            addQueryParam("reserved", utils::urlEncode(reserved));
        }
        // local address (clash `ip`/`ipv6`) -> `address=` for sing-box local_address
        if (kv.count("ip") && !kv["ip"].empty()) addQueryParam("address", utils::urlEncode(kv["ip"]));
        else if (kv.count("ipv6") && !kv["ipv6"].empty()) addQueryParam("address", utils::urlEncode(kv["ipv6"]));
        uri += queryStr;
        if (!name.empty()) {
            uri += "#" + utils::urlEncode(name);
        }
        return uri;
    }

    // Unsupported type
    return "";
}

// Indentation (leading spaces) of a raw line.
int lineIndent(const std::string& line) {
    int n = 0;
    for (char c : line) { if (c == ' ') ++n; else if (c == '\t') n += 2; else break; }
    return n;
}

// Flatten one block-style list item's lines into `k: v` / `parent.child: v` pair lines.
// `lines` carry original indentation; caller stripped the leading `- ` from line 0.
// Rules matching parseClashProxy's key expectations:
//   - first-level nesting stays dotted:  `ws-opts:` / `path: /x` -> `ws-opts.path: /x`
//   - second-level and deeper collapse to flow: `ws-opts:`/`headers:`/`Host: h` -> `ws-opts.headers: {Host: h}`
//   - nested `- item` scalars under a key become `key: [a, b]`
std::vector<std::string> flattenClashItem(const std::vector<std::string>& itemLines) {
    struct Frame { int indent; std::string key; };
    std::vector<std::pair<std::string,std::string>> pairs;   // ordered output
    std::vector<std::string> order;                          // top-level emission order
    std::vector<Frame> stack;
    // pending second-level flow maps and lists, keyed by full key
    std::map<std::string, std::vector<std::string>> flowChildren;
    std::map<std::string, std::vector<std::string>> listChildren;

    for (const std::string& raw : itemLines) {
        int ind = lineIndent(raw);
        std::string t = trimLine(raw);
        if (t.empty() || t[0] == '#') continue;
        while (!stack.empty() && ind <= stack.back().indent) stack.pop_back();

        if (t.size() >= 2 && t[0] == '-' && t[1] == ' ') {
            // list scalar under the innermost frame
            std::string v = unquote(trimLine(t.substr(2)));
            // strip trailing comment
            size_t h = v.find(" #");
            if (h != std::string::npos) v = trimLine(v.substr(0, h));
            if (!stack.empty()) listChildren[stack.back().key].push_back(v);
            continue;
        }

        size_t cp = findPairColon(t);
        if (cp == std::string::npos) continue;
        std::string key = unquote(utils::trim(t.substr(0, cp)));
        std::string val = utils::trim(t.substr(cp + 1));
        if (!val.empty() && val[0] == '#') val.clear();
        if (!val.empty() && val.front() != '"' && val.front() != '\'' &&
            val.front() != '{' && val.front() != '[') {
            size_t h = val.find(" #");
            if (h != std::string::npos) val = utils::trim(val.substr(0, h));
        }

        std::string full;
        for (const auto& f : stack) { full += f.key; full += '.'; }
        full += key;

        if (val.empty()) {
            // `key:` — children follow (map or list). Push frame.
            if (stack.empty()) order.push_back(full);  // only first-level frames emit positionally
            stack.push_back({ind, full});
        } else if (stack.size() >= 2) {
            // grandchild or deeper: fold into innermost frame's flow map
            flowChildren[stack.back().key].push_back(key + ": " + unquote(val));
        } else if (stack.size() == 1) {
            pairs.push_back({full, unquote(val)});
        } else {
            pairs.push_back({key, unquote(val)});
        }
    }

    // Emit: scalars in order, then frame keys (flow map or list) at their first-seen position.
    std::vector<std::string> out;
    std::set<std::string> emitted;
    auto emitFrame = [&](const std::string& fkey) {
        if (emitted.count(fkey)) return;
        emitted.insert(fkey);
        if (listChildren.count(fkey)) {
            std::string v = "[";
            for (size_t i = 0; i < listChildren[fkey].size(); ++i) {
                if (i) v += ", ";
                v += listChildren[fkey][i];
            }
            v += "]";
            out.push_back(fkey + ": " + v);
        } else if (flowChildren.count(fkey)) {
            std::string v = "{";
            for (size_t i = 0; i < flowChildren[fkey].size(); ++i) {
                if (i) v += ", ";
                v += flowChildren[fkey][i];
            }
            v += "}";
            out.push_back(fkey + ": " + v);
        } else {
            out.push_back(fkey + ": ");
        }
    };
    for (const auto& p : pairs) {
        // if this pair's dotted parent has a frame entry, parent was already pushed via `key:` line
        out.push_back(p.first + ": " + p.second);
    }
    for (const auto& f : order) emitFrame(f);
    // any frame not in order (deeper) — already folded into parent's flow map
    return out;
}

// Find matching '}' for a '{' at pos, tracking nesting and quotes.
size_t matchBrace(const std::string& s, size_t pos) {
    int depth = 0; char quote = 0;
    for (size_t i = pos; i < s.size(); ++i) {
        char c = s[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) return i; }
    }
    return std::string::npos;
}

// Simple Clash YAML parser for proxies list
std::vector<std::string> parseClashYaml(const std::string& yaml, int& skipped) {
    std::vector<std::string> uris;
    skipped = 0;

    size_t proxiesPos = yaml.find("proxies:");
    if (proxiesPos == std::string::npos) return uris;

    std::vector<std::string> lines = splitLines(yaml.substr(proxiesPos + 8));

    // Collect list items: `- ` lines plus their deeper-indented continuation lines.
    std::vector<std::vector<std::string>> items;
    int itemIndent = -1;
    for (const std::string& raw : lines) {
        std::string t = trimLine(raw);
        if (t.empty() || t[0] == '#') continue;
        int ind = lineIndent(raw);
        if (ind == 0) break;  // next top-level key ends the proxies section
        if (t.size() > 1 && t[0] == '-' && t[1] == ' ' && (itemIndent < 0 || ind <= itemIndent)) {
            if (itemIndent < 0) itemIndent = ind;
            items.push_back({t.substr(2)});
        } else if (!items.empty() && ind > itemIndent) {
            items.back().push_back(raw);
        }
    }

    for (const auto& itemLines : items) {
        std::string first = trimLine(itemLines[0]);
        std::string blob;
        if (!first.empty() && first[0] == '{') {
            size_t close = matchBrace(first, 0);
            if (close == std::string::npos) { ++skipped; continue; }
            blob = first.substr(0, close + 1);
        } else {
            // join block lines; flattenClashItem needs original indentation for
            // continuation lines (lines[1..]) but the first line came stripped.
            std::vector<std::string> withIndents;
            withIndents.push_back("  " + first);  // artificial child indent
            for (size_t i = 1; i < itemLines.size(); ++i) withIndents.push_back(itemLines[i]);
            auto pairs = flattenClashItem(withIndents);
            blob.clear();
            for (auto& p : pairs) { blob += p; blob += '\n'; }
        }
        std::string uri = parseClashProxy(blob);
        if (!uri.empty()) uris.push_back(uri); else ++skipped;
    }
    return uris;
}

} // namespace

SubscriptionParseResult parseSubscriptionText(std::string body) {
    SubscriptionParseResult result;

    // Cap input at ~8 MiB
    if (body.size() > 8 * 1024 * 1024) {
        result.skipped = 1; // Indicate that we skipped due to size
        return result;
    }

    // Check if body looks like a single base64 blob
    if (isBase64Blob(body)) {
        std::string decoded = utils::base64Decode(body);
        if (!decoded.empty() && 
            (decoded.find("://") != std::string::npos || 
             decoded.find('\n') != std::string::npos ||
             decoded.find('\r') != std::string::npos)) {
            // Treat as base64-encoded list
            result.used_base64 = true;
            body = std::move(decoded); // Use the decoded content for further processing
        }
        // If decode fails or doesn't look like a list, fall through to line list parsing
    }

    // Check for Clash YAML (top-level proxies:)
    if (body.find("proxies:") != std::string::npos) {
        // We'll try to parse as YAML
        int skipped = 0;
        std::vector<std::string> uris = parseClashYaml(body, skipped);
        if (!uris.empty() || skipped > 0) {
            result.used_clash_yaml = true;
            result.uris = std::move(uris);
            result.skipped += skipped;
            // If we successfully parsed something, return early
            if (!result.uris.empty()) {
                // Deduplicate while preserving order
                std::vector<std::string> deduped;
                std::set<std::string> seen;
                for (const auto& uri : result.uris) {
                    if (seen.insert(uri).second) {
                        deduped.push_back(uri);
                    }
                }
                result.uris = std::move(deduped);
                return result;
            }
        }
        // If YAML parsing didn't yield anything, fall through to line list
    }

    // Line list mode
    std::vector<std::string> lines = splitLines(body);
    for (const std::string& line : lines) {
        std::string trimmed = trimLine(line);
        if (trimmed.empty()) {
            continue; // Skip empty lines
        }
        if (hasSupportedScheme(trimmed)) {
            result.uris.push_back(trimmed);
        } else {
            result.skipped++;
        }
    }

    // Deduplicate while preserving order
    std::vector<std::string> deduped;
    std::set<std::string> seen;
    for (const auto& uri : result.uris) {
        if (seen.insert(uri).second) {
            deduped.push_back(uri);
        }
    }
    result.uris = std::move(deduped);

    return result;
}

} // namespace client
} // namespace hunter