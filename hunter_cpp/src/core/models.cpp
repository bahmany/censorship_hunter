#include "core/models.h"
#include "core/utils.h"
#include "core/constants.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/sysinfo.h>
#endif

namespace hunter {

namespace {

int getEnvIntClamped(const char* name, int fallback, int min_value, int max_value) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
        int value = std::stoi(raw);
        if (value < min_value) value = min_value;
        if (value > max_value) value = max_value;
        return value;
    } catch (...) {
        return fallback;
    }
}

} // namespace

// ─── HardwareSnapshot ───

HardwareSnapshot HardwareSnapshot::detect() {
    HardwareSnapshot snap;
    snap.cpu_count = utils::getCpuCount();
    snap.cpu_percent = utils::getCpuPercent();
    snap.ram_percent = utils::getMemoryPercent();

#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        snap.ram_total_gb = (float)ms.ullTotalPhys / (1024.0f * 1024.0f * 1024.0f);
        snap.ram_used_gb = snap.ram_total_gb * (snap.ram_percent / 100.0f);
        snap.ram_free_gb = snap.ram_total_gb - snap.ram_used_gb;
    }
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        snap.ram_total_gb = (float)(si.totalram * si.mem_unit) / (1024.0f * 1024.0f * 1024.0f);
        snap.ram_used_gb = snap.ram_total_gb - (float)(si.freeram * si.mem_unit) / (1024.0f * 1024.0f * 1024.0f);
        snap.ram_free_gb = snap.ram_total_gb - snap.ram_used_gb;
    }
#endif

    // ─── Budget: use only 20% of FREE resources ───
    // This prevents the app from starving the system when RAM/CPU is scarce.
    // Free RAM = total - used. Free CPU = cores * (1 - cpu_usage%).
    snap.ram_budget_gb = snap.ram_free_gb * 0.20f;
    snap.cpu_budget_cores = (float)snap.cpu_count * (1.0f - snap.cpu_percent / 100.0f) * 0.20f;

    // Clamp budgets to sane minimums
    if (snap.ram_budget_gb < 0.1f) snap.ram_budget_gb = 0.1f;
    if (snap.cpu_budget_cores < 1.0f) snap.cpu_budget_cores = 1.0f;

    // ─── Resource tiers based on RAM budget (20% of free) ───
    // Each ConfigHealthRecord uses ~450 bytes, so:
    //   0.5 GB budget → ~1M configs max in memory
    //   1.0 GB budget → ~2M configs max in memory
    //   2.0 GB budget → ~4M configs max in memory
    // We cap at 10000 per-cycle to avoid overwhelming the test pipeline.

    if (snap.ram_budget_gb < 0.25f) {
        snap.mode = ResourceMode::ULTRA_MINIMAL;
        snap.io_pool_size = std::min(8, std::max(4, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = 1;
        snap.max_configs = 200;
        snap.scan_chunk = 16;
    } else if (snap.ram_budget_gb < 0.5f) {
        snap.mode = ResourceMode::MINIMAL;
        snap.io_pool_size = std::min(12, std::max(6, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = 2;
        snap.max_configs = 500;
        snap.scan_chunk = 24;
    } else if (snap.ram_budget_gb < 1.0f) {
        snap.mode = ResourceMode::REDUCED;
        snap.io_pool_size = std::min(20, std::max(8, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = std::max(2, (int)(snap.cpu_budget_cores / 2));
        snap.max_configs = 1000;
        snap.scan_chunk = 30;
    } else if (snap.ram_budget_gb < 2.0f) {
        snap.mode = ResourceMode::CONSERVATIVE;
        snap.io_pool_size = std::min(32, std::max(12, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = std::max(2, (int)(snap.cpu_budget_cores / 2));
        snap.max_configs = 2000;
        snap.scan_chunk = 40;
    } else if (snap.ram_budget_gb < 4.0f) {
        snap.mode = ResourceMode::SCALED;
        snap.io_pool_size = std::min(48, std::max(16, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = std::max(3, (int)(snap.cpu_budget_cores / 2));
        snap.max_configs = 4000;
        snap.scan_chunk = 50;
    } else if (snap.ram_budget_gb < 8.0f) {
        snap.mode = ResourceMode::MODERATE;
        snap.io_pool_size = std::min(64, std::max(24, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = std::max(4, (int)snap.cpu_budget_cores);
        snap.max_configs = 6000;
        snap.scan_chunk = 50;
    } else {
        snap.mode = ResourceMode::NORMAL;
        snap.io_pool_size = std::min(128, std::max(32, (int)snap.cpu_budget_cores));
        snap.cpu_pool_size = std::max(4, (int)snap.cpu_budget_cores);
        snap.max_configs = 10000;
        snap.scan_chunk = 50;
    }

    snap.io_pool_size = getEnvIntClamped("HUNTER_IO_POOL_SIZE", snap.io_pool_size, 4, 128);
    snap.cpu_pool_size = getEnvIntClamped("HUNTER_CPU_POOL_SIZE", snap.cpu_pool_size, 1, 64);

    return snap;
}

// ─── ParsedConfig::toXrayOutboundJson ───

namespace {
bool isLiteralIpAddress(const std::string& address) {
    if (address.empty()) return false;
    if (address.find(':') != std::string::npos) return true; // IPv6
    bool has_dot = false;
    for (unsigned char c : address) {
        if (c == '.') { has_dot = true; continue; }
        if (!std::isdigit(c)) return false;
    }
    return has_dot;
}
} // namespace

std::string ParsedConfig::toXrayOutboundJson(int socks_port) const {
    // Reject protocols XRay doesn't support
    if (protocol == "hysteria2" || protocol == "tuic") return "";
    
    // Reject unsupported SS ciphers
    if (protocol == "shadowsocks") {
        static const std::set<std::string> supported_ciphers = {
            "aes-128-gcm", "aes-256-gcm", "chacha20-ietf-poly1305",
            "xchacha20-ietf-poly1305", "2022-blake3-aes-128-gcm",
            "2022-blake3-aes-256-gcm", "2022-blake3-chacha20-poly1305",
            "none", "plain"
        };
        if (supported_ciphers.find(encryption) == supported_ciphers.end()) return "";
    }
    
    utils::JsonBuilder jb;
    jb.add("tag", "proxy");
    jb.add("protocol", protocol);
    
    std::string settings_json;
    if (protocol == "vmess") {
        settings_json = "{\"vnext\":[{\"address\":\"" + address + "\","
                        "\"port\":" + std::to_string(port) + ","
                        "\"users\":[{\"id\":\"" + uuid + "\","
                        "\"alterId\":0,\"security\":\"" + 
                        (encryption.empty() ? "auto" : encryption) + "\"}]}]}";
    } else if (protocol == "vless") {
        settings_json = "{\"vnext\":[{\"address\":\"" + address + "\","
                        "\"port\":" + std::to_string(port) + ","
                        "\"users\":[{\"id\":\"" + uuid + "\","
                        "\"encryption\":\"none\""
                        + (flow.empty() ? "" : ",\"flow\":\"" + flow + "\"") +
                        "}]}]}";
    } else if (protocol == "trojan") {
        settings_json = "{\"servers\":[{\"address\":\"" + address + "\","
                        "\"port\":" + std::to_string(port) + ","
                        "\"password\":\"" + uuid + "\"}]}";
    } else if (protocol == "shadowsocks") {
        settings_json = "{\"servers\":[{\"address\":\"" + address + "\","
                        "\"port\":" + std::to_string(port) + ","
                        "\"method\":\"" + encryption + "\","
                        "\"password\":\"" + uuid + "\"}]}";
    }

    jb.addRaw("settings", settings_json.empty() ? "{}" : settings_json);

    // Stream settings
    std::string net = network.empty() ? "tcp" : network;
    // Reject unsupported transport types
    static const std::set<std::string> supported_nets = {
        "tcp", "raw", "ws", "grpc", "h2", "httpupgrade", "splithttp", "kcp", "quic", "xhttp"
    };
    if (supported_nets.find(net) == supported_nets.end()) return "";
    // XRay 25.x: "raw" is alias for "tcp"
    if (net == "raw") net = "tcp";
    
    // Validate security type - reject anything XRay doesn't support
    std::string sec = security;
    if (sec != "tls" && sec != "reality" && sec != "none" && !sec.empty()) {
        // xtls is deprecated and removed in recent XRay; reject unknown security types
        return "";
    }
    
    std::string stream = "{\"network\":\"" + net + "\"";
    if (sec == "tls") {
        // NOTE: Xray 26.x removed "allowInsecure" — it now causes a fatal
        // config error. We omit it entirely; xray will validate certs by
        // default. For testing free proxies with self-signed/expired certs,
        // this means some TLS configs that would have worked with
        // allowInsecure=true will now fail — but that's better than ALL
        // TLS configs failing due to the config parse error.
        stream += ",\"security\":\"tls\",\"tlsSettings\":{\"serverName\":\"" +
                  (sni.empty() ? address : sni) + "\""
                  + (fingerprint.empty() ? "" : ",\"fingerprint\":\"" + fingerprint + "\"")
                  + (net == "h2" ? ",\"alpn\":[\"h2\",\"http/1.1\"]" : "")
                  + "}";
    } else if (sec == "reality") {
        stream += ",\"security\":\"reality\",\"realitySettings\":{"
                  "\"serverName\":\"" + sni + "\""
                  ",\"publicKey\":\"" + public_key + "\""
                  + (short_id.empty() ? "" : ",\"shortId\":\"" + short_id + "\"")
                  + (fingerprint.empty() ? ",\"fingerprint\":\"chrome\"" : ",\"fingerprint\":\"" + fingerprint + "\"")
                  + "}";
    } else {
        stream += ",\"security\":\"none\"";
    }
    if (net == "ws") {
        stream += ",\"wsSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"headers\":{\"Host\":\"" + host + "\"}") + "}";
    } else if (net == "httpupgrade") {
        stream += ",\"httpupgradeSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"host\":\"" + host + "\"") + "}";
    } else if (net == "splithttp" || net == "xhttp") {
        stream += ",\"splithttpSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"host\":\"" + host + "\"") + "}";
    } else if (net == "grpc") {
        // Skip gRPC+TLS configs where the server is a raw IP with no SNI domain.
        // Xray 1.8.x has a bug: allowInsecure does not bypass IP SAN validation for gRPC transport.
        if (sec == "tls" && sni.empty() && isLiteralIpAddress(address)) return "";
        std::string sn = !path.empty() ? path : (extra.count("serviceName") ? extra.at("serviceName") : "");
        stream += ",\"grpcSettings\":{\"serviceName\":\"" + sn + "\"}";
    } else if (net == "h2") {
        stream += ",\"httpSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"host\":[\"" + host + "\"]") + "}";
    } else if (net == "kcp") {
        std::string header_type = type.empty() ? "none" : type;
        stream += ",\"kcpSettings\":{\"header\":{\"type\":\"" + header_type + "\"}}";
    } else if (net == "quic") {
        std::string header_type = type.empty() ? "none" : type;
        stream += ",\"quicSettings\":{\"security\":\"none\",\"key\":\"\",\"header\":{\"type\":\"" + header_type + "\"}}";
    }
    stream += "}";
    jb.addRaw("streamSettings", stream);

    return jb.build();
}

// ─── ParsedConfig::toXrayConfigJson ───

std::string ParsedConfig::toXrayConfigJson(int socks_port) const {
    std::string outbound = toXrayOutboundJson(socks_port);
    if (outbound.empty()) return "";
    
    std::ostringstream ss;
    ss << "{"
       << "\"log\":{\"loglevel\":\"warning\"},"
       << "\"dns\":{\"tag\":\"dns-module\",\"servers\":[\"1.1.1.1\",\"8.8.8.8\"],\"queryStrategy\":\"UseIPv4\"},"
       << "\"inbounds\":[{\"tag\":\"socks-in\",\"port\":" << socks_port << ",\"listen\":\"127.0.0.1\",\"protocol\":\"socks\",\"settings\":{\"auth\":\"noauth\",\"udp\":true},\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\"],\"routeOnly\":true}}],"
       << "\"outbounds\":["
       << outbound
       << ",{\"tag\":\"direct\",\"protocol\":\"freedom\",\"settings\":{}}"
       << ",{\"tag\":\"dns-out\",\"protocol\":\"dns\",\"settings\":{}}"
       << ",{\"tag\":\"blackhole\",\"protocol\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}"
       << "],"
       << "\"routing\":{\"domainStrategy\":\"AsIs\",\"final\":\"blackhole\",\"rules\":["
       << "{\"type\":\"field\",\"inboundTag\":[\"socks-in\"],\"port\":53,\"outboundTag\":\"dns-out\"},"
       << "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"outboundTag\":\"proxy\"},"
       << "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       << "{\"type\":\"field\",\"ip\":[\"geoip:private\"],\"outboundTag\":\"direct\"},"
       << "{\"type\":\"field\",\"inboundTag\":[\"socks-in\"],\"balancerTag\":\"proxy-balancer\"}"
       << "],\"balancers\":[{\"tag\":\"proxy-balancer\",\"selector\":[\"proxy\"],\"strategy\":{\"type\":\"leastPing\"}}]}"
       << ",\"observatory\":{\"subjectSelector\":[\"proxy\"],\"probeURL\":\"http://1.1.1.1/generate_204\",\"probeInterval\":\"30s\"}"
       << "}";
    return ss.str();
}

// ─── ParsedConfig::toSingBoxConfigJson ───

std::string ParsedConfig::toSingBoxConfigJson(int socks_port) const {
    // sing-box outbound JSON format
    // Supports: vmess, vless, trojan, shadowsocks, hysteria2, tuic
    
    std::string proto = protocol;
    if (proto == "shadowsocks") proto = "shadowsocks";
    
    // Reject unsupported protocols
    if (proto != "vmess" && proto != "vless" && proto != "trojan" && 
        proto != "shadowsocks" && proto != "hysteria2" && proto != "tuic") {
        return "";
    }
    
    std::string net = network.empty() ? "tcp" : network;
    // sing-box uses different transport names
    if (net == "raw") net = "tcp";
    
    std::ostringstream ob;
    ob << "{\"type\":\"" << proto << "\",\"tag\":\"proxy\"";
    ob << ",\"server\":\"" << address << "\",\"server_port\":" << port;
    
    if (proto == "vmess") {
        ob << ",\"uuid\":\"" << uuid << "\",\"security\":\"" 
           << (encryption.empty() ? "auto" : encryption) << "\",\"alter_id\":0";
    } else if (proto == "vless") {
        ob << ",\"uuid\":\"" << uuid << "\"";
        if (!flow.empty()) ob << ",\"flow\":\"" << flow << "\"";
    } else if (proto == "trojan") {
        ob << ",\"password\":\"" << uuid << "\"";
    } else if (proto == "shadowsocks") {
        ob << ",\"method\":\"" << encryption << "\",\"password\":\"" << uuid << "\"";
    } else if (proto == "hysteria2") {
        ob << ",\"password\":\"" << uuid << "\"";
        if (extra.count("up_mbps")) ob << ",\"up_mbps\":" << extra.at("up_mbps");
        if (extra.count("down_mbps")) ob << ",\"down_mbps\":" << extra.at("down_mbps");
    } else if (proto == "tuic") {
        ob << ",\"uuid\":\"" << uuid << "\"";
        if (extra.count("password")) ob << ",\"password\":\"" << extra.at("password") << "\"";
        ob << ",\"congestion_control\":\"bbr\"";
    }
    
    // TLS settings
    if (security == "tls" || security == "reality") {
        ob << ",\"tls\":{\"enabled\":true";
        if (!sni.empty()) ob << ",\"server_name\":\"" << sni << "\"";
        else if (!host.empty()) ob << ",\"server_name\":\"" << host << "\"";
        else ob << ",\"server_name\":\"" << address << "\"";
        
        if (security == "reality") {
            ob << ",\"reality\":{\"enabled\":true";
            if (!public_key.empty()) ob << ",\"public_key\":\"" << public_key << "\"";
            if (!short_id.empty()) ob << ",\"short_id\":\"" << short_id << "\"";
            ob << "}";
        }
        if (!fingerprint.empty()) {
            ob << ",\"utls\":{\"enabled\":true,\"fingerprint\":\"" << fingerprint << "\"}";
        } else {
            ob << ",\"utls\":{\"enabled\":true,\"fingerprint\":\"chrome\"}";
        }
        ob << ",\"insecure\":true}";
    }
    
    // Transport settings
    if (net == "ws") {
        ob << ",\"transport\":{\"type\":\"ws\"";
        if (!path.empty()) ob << ",\"path\":\"" << path << "\"";
        if (!host.empty()) ob << ",\"headers\":{\"Host\":\"" << host << "\"}";
        ob << "}";
    } else if (net == "grpc") {
        std::string sn = path.empty() ? (extra.count("serviceName") ? extra.at("serviceName") : "") : path;
        ob << ",\"transport\":{\"type\":\"grpc\",\"service_name\":\"" << sn << "\"}";
    } else if (net == "h2" || net == "http") {
        ob << ",\"transport\":{\"type\":\"http\"";
        if (!path.empty()) ob << ",\"path\":\"" << path << "\"";
        if (!host.empty()) ob << ",\"host\":[\"" << host << "\"]";
        ob << "}";
    } else if (net == "httpupgrade") {
        ob << ",\"transport\":{\"type\":\"httpupgrade\"";
        if (!path.empty()) ob << ",\"path\":\"" << path << "\"";
        if (!host.empty()) ob << ",\"host\":\"" << host << "\"";
        ob << "}";
    }
    
    ob << "}";
    
    // Full sing-box config with blackhole kill switch
    std::ostringstream ss;
    ss << "{"
       << "\"log\":{\"level\":\"warn\"},"
       << "\"dns\":{\"servers\":[{\"tag\":\"dns-direct\",\"address\":\"1.1.1.1\"},{\"tag\":\"dns-google\",\"address\":\"8.8.8.8\"}]},"
       << "\"inbounds\":[{\"type\":\"mixed\",\"tag\":\"mixed-in\",\"listen\":\"127.0.0.1\",\"listen_port\":" << socks_port << ",\"sniff\":true,\"sniff_override_destination\":true}],"
       << "\"outbounds\":["
       << ob.str()
       << ",{\"type\":\"direct\",\"tag\":\"direct\"}"
       << ",{\"type\":\"block\",\"tag\":\"blackhole\"}"
       << "],"
       << "\"route\":{\"rules\":["
       << "{\"protocol\":\"dns\",\"outbound\":\"direct\"},"
       << "{\"ip_is_private\":true,\"outbound\":\"direct\"},"
       << "{\"inbound\":[\"mixed-in\"],\"outbound\":\"proxy\"}"
       << "],\"final\":\"blackhole\"}"
       << "}";
    return ss.str();
}

// ─── ParsedConfig::toMihomoConfigYaml ───

std::string ParsedConfig::toMihomoConfigYaml(int socks_port) const {
    // mihomo (Clash Meta) YAML format
    // Supports: vmess, vless, trojan, ss, hysteria2, tuic
    
    if (protocol != "vmess" && protocol != "vless" && protocol != "trojan" && 
        protocol != "shadowsocks" && protocol != "hysteria2" && protocol != "tuic") {
        return "";
    }
    
    std::string net = network.empty() ? "tcp" : network;
    if (net == "raw") net = "tcp";
    
    std::ostringstream ss;
    ss << "mixed-port: " << socks_port << "\n"
       << "mode: global\n"
       << "log-level: warning\n"
       << "allow-lan: false\n"
       << "dns:\n"
       << "  enable: true\n"
       << "  nameserver:\n"
       << "    - 1.1.1.1\n"
       << "    - 8.8.8.8\n"
       << "proxies:\n";
    
    std::string type_str;
    if (protocol == "vmess") type_str = "vmess";
    else if (protocol == "vless") type_str = "vless";
    else if (protocol == "trojan") type_str = "trojan";
    else if (protocol == "shadowsocks") type_str = "ss";
    else if (protocol == "hysteria2") type_str = "hysteria2";
    else if (protocol == "tuic") type_str = "tuic";
    
    ss << "  - name: proxy\n"
       << "    type: " << type_str << "\n"
       << "    server: " << address << "\n"
       << "    port: " << port << "\n";
    
    if (protocol == "vmess") {
        ss << "    uuid: " << uuid << "\n"
           << "    alterId: 0\n"
           << "    cipher: " << (encryption.empty() ? "auto" : encryption) << "\n";
    } else if (protocol == "vless") {
        ss << "    uuid: " << uuid << "\n";
        if (!flow.empty()) ss << "    flow: " << flow << "\n";
    } else if (protocol == "trojan") {
        ss << "    password: " << uuid << "\n";
    } else if (protocol == "shadowsocks") {
        ss << "    cipher: " << encryption << "\n"
           << "    password: " << uuid << "\n";
    } else if (protocol == "hysteria2") {
        ss << "    password: " << uuid << "\n";
    } else if (protocol == "tuic") {
        ss << "    uuid: " << uuid << "\n";
        if (extra.count("password")) ss << "    password: " << extra.at("password") << "\n";
        ss << "    congestion-controller: bbr\n";
    }
    
    // Network/transport
    if (net != "tcp") {
        ss << "    network: " << net << "\n";
    }
    
    // TLS
    if (security == "tls") {
        ss << "    tls: true\n"
           << "    skip-cert-verify: true\n";
        if (!sni.empty()) ss << "    servername: " << sni << "\n";
        else if (!host.empty()) ss << "    servername: " << host << "\n";
        if (!fingerprint.empty()) ss << "    client-fingerprint: " << fingerprint << "\n";
        else ss << "    client-fingerprint: chrome\n";
    } else if (security == "reality") {
        ss << "    tls: true\n"
           << "    skip-cert-verify: true\n";
        if (!sni.empty()) ss << "    servername: " << sni << "\n";
        ss << "    reality-opts:\n";
        if (!public_key.empty()) ss << "      public-key: " << public_key << "\n";
        if (!short_id.empty()) ss << "      short-id: " << short_id << "\n";
        if (!fingerprint.empty()) ss << "    client-fingerprint: " << fingerprint << "\n";
        else ss << "    client-fingerprint: chrome\n";
    }
    
    // Transport options
    if (net == "ws") {
        ss << "    ws-opts:\n";
        if (!path.empty()) ss << "      path: " << path << "\n";
        if (!host.empty()) ss << "      headers:\n        Host: " << host << "\n";
    } else if (net == "grpc") {
        ss << "    grpc-opts:\n";
        std::string sn = path.empty() ? (extra.count("serviceName") ? extra.at("serviceName") : "") : path;
        if (!sn.empty()) ss << "      grpc-service-name: " << sn << "\n";
    } else if (net == "h2" || net == "http") {
        ss << "    h2-opts:\n";
        if (!path.empty()) ss << "      path: " << path << "\n";
        if (!host.empty()) ss << "      host:\n        - " << host << "\n";
    }
    
    // Proxy groups and rules with BLACKHOLE kill switch
    ss << "proxy-groups:\n"
       << "  - name: GLOBAL\n"
       << "    type: fallback\n"
       << "    proxies:\n"
       << "      - proxy\n"
       << "      - BLACKHOLE\n"
       << "    url: http://1.1.1.1/generate_204\n"
       << "    interval: 30\n"
       << "  - name: BLACKHOLE\n"
       << "    type: select\n"
       << "    proxies:\n"
       << "      - REJECT\n"
       << "rules:\n"
       << "  - MATCH,GLOBAL\n";
    
    return ss.str();
}

} // namespace hunter
