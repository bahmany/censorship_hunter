#include "core/endpoint_key.h"
#include "core/models.h"
#include "core/utils.h"
#include "network/uri_parser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace hunter {
namespace {

// ── SHA-256 ──
const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

std::string lowerAscii(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string lp(const std::string& s) { return std::to_string(s.size()) + ":" + s + ";"; }

bool isHex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// UUID text (8-4-4-4-12 or 32 hex) -> canonical lowercase dashed; else returns false.
bool canonicalUuid(const std::string& in, std::string* out) {
    std::string hex;
    if (in.size() == 36) {
        for (size_t i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) { if (in[i] != '-') return false; }
            else if (!isHex(in[i])) return false;
            else hex.push_back(in[i]);
        }
    } else if (in.size() == 32) {
        for (char c : in) { if (!isHex(c)) return false; hex.push_back(c); }
    } else return false;
    hex = lowerAscii(hex);
    *out = hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20);
    return true;
}

std::string formatIpv6(const uint8_t b[16]) {
    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = uint16_t((b[i * 2] << 8) | b[i * 2 + 1]);
    int best_start = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) { i++; continue; }
        int j = i;
        while (j < 8 && g[j] == 0) j++;
        if (j - i > best_len) { best_len = j - i; best_start = i; }
        i = j;
    }
    if (best_len < 2) best_start = -1;
    static const char* hexd = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; i++) {
        if (i == best_start) {
            out += "::";
            i += best_len - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') out += ':';
        char buf[5]; int n = 0;
        uint16_t v = g[i];
        bool started = false;
        for (int sh = 12; sh >= 0; sh -= 4) {
            int d = (v >> sh) & 0xF;
            if (d || started || sh == 0) { buf[n++] = hexd[d]; started = true; }
        }
        out.append(buf, n);
    }
    return out;
}

std::string urlDec(const std::string& s) { return utils::urlDecode(s); }

bool truthy(const std::string& v) {
    std::string l = lowerAscii(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

}  // namespace

std::string sha256Hex(const std::string& data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size(), i = 0;
    for (; i + 64 <= n; i += 64) sha256Block(h, p + i);
    uint8_t tail[128] = {0};
    size_t rem = n - i;
    if (rem) std::memcpy(tail, p + i, rem);
    tail[rem] = 0x80;
    size_t tl = (rem + 1 + 8 <= 64) ? 64 : 128;
    uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = uint8_t(bits >> (8 * k));
    sha256Block(h, tail);
    if (tl == 128) sha256Block(h, tail + 64);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    for (int k = 0; k < 8; k++)
        for (int sh = 28; sh >= 0; sh -= 4) out.push_back(hexd[(h[k] >> sh) & 0xF]);
    return out;
}

std::string canonicalProtocol(const std::string& proto) {
    std::string p = lowerAscii(utils::trim(proto));
    if (p == "hy2") return "hysteria2";
    if (p == "ss") return "shadowsocks";
    return p;
}

bool canonicalHost(const std::string& host_in, std::string* out) {
    std::string h = utils::trim(host_in);
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    if (h.empty()) return false;
    for (unsigned char c : h) if (c <= 0x20 || c == 0x7f || c == '/' || c == '@' || c == '\\') return false;
    if (h.find(':') != std::string::npos) {
        if (h.find('%') != std::string::npos) return false;  // zone ids unsupported
        in6_addr a6;
        if (inet_pton(AF_INET6, h.c_str(), &a6) != 1) return false;
        *out = formatIpv6(reinterpret_cast<const uint8_t*>(&a6));
        return true;
    }
    in_addr a4;
    if (inet_pton(AF_INET, h.c_str(), &a4) == 1) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&a4);
        *out = std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
               std::to_string(b[3]);
        return true;
    }
    h = lowerAscii(h);
    if (h.back() == '.') h.pop_back();
    if (h.empty() || h.size() > 253) return false;
    *out = h;
    return true;
}

EndpointKey computeEndpointKey(const std::string& uri) {
    EndpointKey ek;
    auto finish_raw = [&]() {
        ek.valid = false;
        ek.canonical = "EKV1RAW;" + lp(uri);
        ek.key = "ek1:" + sha256Hex(ek.canonical);
        return ek;
    };

    auto parsed = network::UriParser::parse(uri);
    if (!parsed.has_value() || !parsed->isValid()) return finish_raw();
    const ParsedConfig& pc = *parsed;

    const std::string proto = canonicalProtocol(pc.protocol);
    std::string host;
    if (proto.empty() || !canonicalHost(pc.address, &host)) return finish_raw();
    if (pc.port < 1 || pc.port > 65535) return finish_raw();

    // Credential identity: UUID protocols fold case; passwords/auth stay case-sensitive.
    std::string cred = pc.uuid;
    if (proto == "vmess" || proto == "vless" || proto == "tuic") {
        std::string u;
        if (canonicalUuid(cred, &u)) cred = u;
    }

    std::map<std::string, std::string> opts;
    auto put = [&](const std::string& k, const std::string& v) {
        if (!v.empty()) opts[k] = v;
    };
    auto put_default = [&](const std::string& k, const std::string& v, const std::string& dflt) {
        std::string vv = v.empty() ? dflt : v;
        if (vv != dflt) opts[k] = vv;
    };

    const bool is_tls_default = (proto == "trojan" || proto == "hysteria2" || proto == "tuic");
    put_default("security", lowerAscii(pc.security), is_tls_default ? "tls" : "none");
    put_default("transport", lowerAscii(pc.network), "tcp");
    put_default("headertype", lowerAscii(pc.type), "none");
    std::string enc_default = proto == "vmess" ? "auto" : (proto == "vless" ? "none" : "");
    put_default("cipher", lowerAscii(pc.encryption), enc_default);
    put("sni", lowerAscii(pc.sni));
    put("host", lowerAscii(pc.host));
    put("path", pc.path);
    put("flow", lowerAscii(pc.flow));
    put("fp", lowerAscii(pc.fingerprint));
    put("pbk", pc.public_key);
    put("sid", pc.short_id);
    for (const auto& [k, v] : pc.extra) {
        if (k == "ps") continue;
        put("x." + k, v);
    }

    // Retain query options the parser does not consume (alpn, obfs, plugin, insecure, ...).
    std::set<std::string> consumed;
    if (proto == "vless") consumed = {"encryption", "security", "type", "sni", "host", "path", "fp", "pbk", "sid", "flow"};
    else if (proto == "trojan") consumed = {"security", "type", "sni", "host", "path", "fp"};
    else if (proto == "hysteria2" || proto == "tuic") consumed = {"sni"};
    std::string raw = utils::trim(uri);
    if (proto != "vmess") {
        auto hp = raw.find('#');
        if (hp != std::string::npos) raw = raw.substr(0, hp);
        auto qp = raw.find('?');
        if (qp != std::string::npos) {
            std::string q = raw.substr(qp + 1);
            size_t pos = 0;
            while (pos <= q.size()) {
                size_t amp = q.find('&', pos);
                if (amp == std::string::npos) amp = q.size();
                std::string pair = q.substr(pos, amp - pos);
                pos = amp + 1;
                if (pair.empty()) continue;
                auto eq = pair.find('=');
                std::string k = urlDec(eq == std::string::npos ? pair : pair.substr(0, eq));
                std::string v = eq == std::string::npos ? "" : urlDec(pair.substr(eq + 1));
                std::string kl = lowerAscii(k);
                if (consumed.count(k) || kl == "ps" || kl == "remark" || kl == "remarks" || kl == "name") continue;
                if (kl == "insecure" || kl == "allowinsecure") {
                    if (truthy(v)) opts["insecure"] = "1";
                    continue;
                }
                if (kl == "alpn") v = lowerAscii(v);
                opts["q." + k] = v;
            }
        }
    }
    if (opts.count("insecure")) ek.insecure_tls = true;

    ek.valid = true;
    ek.protocol = proto;
    ek.host = host;
    ek.port = pc.port;
    ek.canonical = "EKV1;" + lp(proto) + lp(host) + lp(std::to_string(pc.port)) + lp(cred);
    for (const auto& [k, v] : opts) ek.canonical += lp(k) + lp(v);
    ek.key = "ek1:" + sha256Hex(ek.canonical);
    return ek;
}

std::string endpointKeyForUri(const std::string& uri) { return computeEndpointKey(uri).key; }

}  // namespace hunter
