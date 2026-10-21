#include "core/client_export.h"
#include "core/utils.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <set>

namespace hunter {
namespace client_export {

// ─── Internal helpers ───

static bool isWhitespaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static std::string trimWhitespaceBomZeroWidth(const std::string& s) {
    size_t start = 0;
    while (start < s.size()) {
        unsigned char ch = (unsigned char)s[start];
        // UTF-8 BOM: 0xEF 0xBB 0xBF
        if (start + 2 < s.size() && ch == 0xEF && (unsigned char)s[start+1] == 0xBB && (unsigned char)s[start+2] == 0xBF) {
            start += 3;
            continue;
        }
        // Zero-width space (U+200B) in UTF-8: E2 80 8B
        if (start + 2 < s.size() && ch == 0xE2 && (unsigned char)s[start+1] == 0x80 && (unsigned char)s[start+2] == 0x8B) {
            start += 3;
            continue;
        }
        // Zero-width no-break space / BOM char (U+FEFF): EF BB BF (already handled above as BOM) or FEFF in UTF-16; in UTF-8 FEFF = EF BB BF.
        // Zero-width joiner (U+200D): E2 80 8D
        if (start + 2 < s.size() && ch == 0xE2 && (unsigned char)s[start+1] == 0x80 && (unsigned char)s[start+2] == 0x8D) {
            start += 3;
            continue;
        }
        // Zero-width non-joiner (U+200C): E2 80 8C
        if (start + 2 < s.size() && ch == 0xE2 && (unsigned char)s[start+1] == 0x80 && (unsigned char)s[start+2] == 0x8C) {
            start += 3;
            continue;
        }
        if (isWhitespaceChar(s[start])) {
            start++;
        } else {
            break;
        }
    }
    size_t end = s.size();
    while (end > start) {
        size_t pos = end - 1;
        unsigned char ch = (unsigned char)s[pos];
        if (pos >= 2 && s.size() >= 3) {
            // Check for BOM at end
            if (pos >= 2 && ch == 0xBF && (unsigned char)s[pos-1] == 0xBB && (unsigned char)s[pos-2] == 0xEF) {
                end -= 3;
                continue;
            }
            // Check for zero-width chars at end
            if (pos >= 2 && ch == 0x8B && (unsigned char)s[pos-1] == 0x80 && (unsigned char)s[pos-2] == 0xE2) {
                end -= 3;
                continue;
            }
            if (pos >= 2 && ch == 0x8D && (unsigned char)s[pos-1] == 0x80 && (unsigned char)s[pos-2] == 0xE2) {
                end -= 3;
                continue;
            }
            if (pos >= 2 && ch == 0x8C && (unsigned char)s[pos-1] == 0x80 && (unsigned char)s[pos-2] == 0xE2) {
                end -= 3;
                continue;
            }
        }
        if (isWhitespaceChar(s[pos])) {
            end--;
        } else {
            break;
        }
    }
    return s.substr(start, end - start);
}

static bool hasInternalWhitespaceInBody(const std::string& body) {
    for (char c : body) {
        if (isWhitespaceChar(c)) return true;
    }
    return false;
}

static std::string lowercaseString(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = (char)std::tolower((unsigned char)c);
    }
    return out;
}

static bool isV2rayNScheme(const std::string& scheme) {
    return scheme == "vmess" || scheme == "ss" || scheme == "socks" || scheme == "socks5" ||
           scheme == "socks4" || scheme == "trojan" || scheme == "vless" ||
           scheme == "hysteria2" || scheme == "hy2" || scheme == "hysteria2+realm" ||
           scheme == "hysteria2+realm+http" || scheme == "tuic" || scheme == "wireguard" ||
           scheme == "anytls" || scheme == "naive+https" || scheme == "naive+quic";
}

static std::string percentEncodeFragment(const std::string& fragment) {
    std::string out;
    out.reserve(fragment.size() * 3);
    for (unsigned char ch : fragment) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += (char)ch;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", ch);
            out += buf;
        }
    }
    return out;
}

static bool isBalancedBraces(const std::string& s) {
    int brace = 0;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inStr) {
            if (c == '\\') { ++i; }
            else if (c == '"') { inStr = false; }
            continue;
        }
        if (c == '"') {
            inStr = true;
        } else if (c == '{') {
            ++brace;
        } else if (c == '}') {
            --brace;
            if (brace < 0) return false;
        }
    }
    return !inStr && brace == 0;
}

static bool isValidJsonPayload(const std::string& payload) {
    std::string trimmed = payload;
    // Trim whitespace from payload
    size_t pstart = 0;
    while (pstart < trimmed.size() && isWhitespaceChar(trimmed[pstart])) pstart++;
    size_t pend = trimmed.size();
    while (pend > pstart && isWhitespaceChar(trimmed[pend-1])) pend--;
    trimmed = trimmed.substr(pstart, pend - pstart);
    if (trimmed.empty()) return false;
    if (trimmed.front() != '{' || trimmed.back() != '}') return false;
    return isBalancedBraces(trimmed);
}

// ─── normalizeUri ───

// Repair non-vmess bodies scraped from HTML/Telegram: un-escape "&amp;" and
// percent-encode bytes that are illegal in a URI (spaces, emoji, quotes, ...).
// v2rayN's importer rejects the whole line otherwise.
static std::string repairBody(const std::string& body) {
    std::string unescaped;
    unescaped.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
        if (body.compare(i, 5, "&amp;") == 0) {
            unescaped += '&';
            i += 4;
        } else {
            unescaped += body[i];
        }
    }
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(unescaped.size());
    for (unsigned char c : unescaped) {
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' ||
            c == '^' || c == '`' || c == '{' || c == '|' || c == '}') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        } else {
            out += (char)c;
        }
    }
    return out;
}

std::string normalizeUri(const std::string& uri) {
    std::string trimmed = trimWhitespaceBomZeroWidth(uri);
    if (trimmed.empty()) return "";

    // Find scheme boundary
    size_t schemeEnd = trimmed.find("://");
    if (schemeEnd == std::string::npos) return "";

    std::string scheme = lowercaseString(trimmed.substr(0, schemeEnd));

    // Map aliases
    if (scheme == "hy2") scheme = "hysteria2";
    else if (scheme == "socks5") scheme = "socks";

    if (!isV2rayNScheme(scheme)) return "";

    // For schemes that should not exist in v2rayN list (after mapping, socks is allowed)
    // The original isV2rayNScheme includes "socks" but not "socks5" (after mapping we keep socks)

    std::string rest = trimmed.substr(schemeEnd + 3);

    // Split body and fragment at first '#'
    size_t fragPos = rest.find('#');
    std::string body = rest;
    std::string fragment;
    bool hasFragment = false;
    if (fragPos != std::string::npos) {
        body = rest.substr(0, fragPos);
        fragment = rest.substr(fragPos + 1);
        hasFragment = true;
    }

    // Trailing whitespace before '#' is a common copy/paste artifact.
    while (!body.empty() && isWhitespaceChar(body.back())) body.pop_back();

    // Handle vmess
    if (scheme == "vmess") {
        std::string payload = body;
        // Drop vmess payloads that contain whitespace
        if (hasInternalWhitespaceInBody(payload)) {
            return "";
        }
        // Clean URL-safe base64 to standard base64
        std::string cleanedPayload;
        cleanedPayload.reserve(payload.size());
        for (char c : payload) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
            cleanedPayload += c;
        }
        // Strict check: payload must contain only base64 chars and padding
        for (char c : cleanedPayload) {
            bool ok = (std::isalnum((unsigned char)c) || c == '+' || c == '/' || c == '=');
            if (!ok) return "";
        }
        std::string rawDecoded = utils::base64Decode(cleanedPayload);
        if (rawDecoded.empty() && !cleanedPayload.empty()) {
            return "";
        }
        if (!isValidJsonPayload(rawDecoded)) {
            return "";
        }
        std::string reEncoded = utils::base64Encode(rawDecoded);
        std::string rebuilt = "vmess://" + reEncoded;
        if (hasFragment && !fragment.empty()) {
            rebuilt += "#" + percentEncodeFragment(fragment);
        }
        return rebuilt;
    }

    // For non-vmess: ensure fragment exists; encode fragment
    std::string rebuiltBody = repairBody(body);
    std::string rebuiltFragment;
    if (hasFragment) {
        // Trim whitespace from fragment
        size_t fstart = 0;
        while (fstart < fragment.size() && isWhitespaceChar(fragment[fstart])) fstart++;
        size_t fend = fragment.size();
        while (fend > fstart && isWhitespaceChar(fragment[fend-1])) fend--;
        std::string trimmedFrag = fragment.substr(fstart, fend - fstart);
        if (trimmedFrag.empty()) {
            rebuiltFragment = "Hunter";
        } else {
            rebuiltFragment = percentEncodeFragment(trimmedFrag);
        }
    } else {
        rebuiltFragment = "Hunter";
    }

    // Rebuild: scheme://body#fragment (always include fragment for non-vmess)
    std::string result = lowercaseString(trimmed.substr(0, schemeEnd)) + "://" + rebuiltBody + "#" + rebuiltFragment;
    // Ensure scheme mapping is preserved in the rebuilt result
    // Actually we already changed scheme variable; let's use the mapped scheme
    result = scheme + "://" + rebuiltBody + "#" + rebuiltFragment;
    return result;
}

// ─── buildShareText ───

std::string buildShareText(const std::vector<std::string>& uris) {
    // Keep caller order (lists are ranked best-first); drop duplicates only.
    std::set<std::string> seen;
    std::vector<std::string> ordered;
    for (const std::string& uri : uris) {
        std::string n = normalizeUri(uri);
        if (!n.empty() && seen.insert(n).second) {
            ordered.push_back(n);
        }
    }
    std::string lineSep = "\n";
#ifdef _WIN32
    lineSep = "\r\n";
#endif
    std::string out;
    for (const std::string& s : ordered) {
        out += s;
        out += lineSep;
    }
    return out;
}

} // namespace client_export
} // namespace hunter
