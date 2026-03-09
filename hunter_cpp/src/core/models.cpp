#include "core/models.h"
#include "core/utils.h"
#include "core/constants.h"

#include <algorithm>
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

// ─── HardwareSnapshot ───

HardwareSnapshot HardwareSnapshot::detect() {
    HardwareSnapshot snap;
    snap.cpu_count = utils::getCpuCount();
    snap.ram_percent = utils::getMemoryPercent();

#ifdef _WIN32
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        snap.ram_total_gb = (float)ms.ullTotalPhys / (1024.0f * 1024.0f * 1024.0f);
        snap.ram_used_gb = snap.ram_total_gb * (snap.ram_percent / 100.0f);
    }
#else
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        snap.ram_total_gb = (float)(si.totalram * si.mem_unit) / (1024.0f * 1024.0f * 1024.0f);
        snap.ram_used_gb = snap.ram_total_gb - (float)(si.freeram * si.mem_unit) / (1024.0f * 1024.0f * 1024.0f);
    }
#endif

    int base = std::max(4, snap.cpu_count);

    if (snap.ram_percent >= 95) {
        snap.mode = ResourceMode::ULTRA_MINIMAL;
        snap.io_pool_size = std::max(10, base / 2);
        snap.cpu_pool_size = 2;
        snap.max_configs = 80;
        snap.scan_chunk = 20;
    } else if (snap.ram_percent >= 90) {
        snap.mode = ResourceMode::MINIMAL;
        snap.io_pool_size = std::max(10, base);
        snap.cpu_pool_size = 2;
        snap.max_configs = 150;
        snap.scan_chunk = 30;
    } else if (snap.ram_percent >= 85) {
        snap.mode = ResourceMode::REDUCED;
        snap.io_pool_size = std::max(10, base + 2);
        snap.cpu_pool_size = std::max(2, base / 2);
        snap.max_configs = 250;
        snap.scan_chunk = 40;
    } else if (snap.ram_percent >= 80) {
        snap.mode = ResourceMode::CONSERVATIVE;
        snap.io_pool_size = std::max(12, base + 4);
        snap.cpu_pool_size = std::max(2, base / 2);
        snap.max_configs = 400;
        snap.scan_chunk = 50;
    } else if (snap.ram_percent >= 70) {
        snap.mode = ResourceMode::SCALED;
        snap.io_pool_size = std::max(15, base + 6);
        snap.cpu_pool_size = std::max(3, base / 2);
        snap.max_configs = 600;
        snap.scan_chunk = 50;
    } else if (snap.ram_percent >= 60) {
        snap.mode = ResourceMode::MODERATE;
        snap.io_pool_size = std::max(18, base * 2);
        snap.cpu_pool_size = std::max(4, base);
        snap.max_configs = 800;
        snap.scan_chunk = 50;
    } else {
        snap.mode = ResourceMode::NORMAL;
        snap.io_pool_size = std::max(20, base * 2);
        snap.cpu_pool_size = std::max(4, base);
        snap.max_configs = 1000;
        snap.scan_chunk = 50;
    }

    return snap;
}

// ─── ParsedConfig::toXrayOutboundJson ───

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
        stream += ",\"security\":\"tls\",\"tlsSettings\":{\"serverName\":\"" +
                  (sni.empty() ? address : sni) + "\""
                  ",\"allowInsecure\":true"
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
                  + (host.empty() ? "" : ",\"host\":\"" + host + "\"") + "}";
    } else if (net == "httpupgrade") {
        stream += ",\"httpupgradeSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"host\":\"" + host + "\"") + "}";
    } else if (net == "splithttp" || net == "xhttp") {
        stream += ",\"splithttpSettings\":{\"path\":\"" + (path.empty() ? "/" : path) + "\""
                  + (host.empty() ? "" : ",\"host\":\"" + host + "\"") + "}";
    } else if (net == "grpc") {
        std::string sn = path.empty() ? extra.count("serviceName") ? extra.at("serviceName") : "" : path;
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
    
    // Full sing-box config
    std::ostringstream ss;
    ss << "{"
       << "\"log\":{\"level\":\"warn\"},"
       << "\"dns\":{\"servers\":[{\"tag\":\"dns-direct\",\"address\":\"1.1.1.1\"},{\"tag\":\"dns-google\",\"address\":\"8.8.8.8\"}]},"
       << "\"inbounds\":[{\"type\":\"socks\",\"tag\":\"socks-in\",\"listen\":\"127.0.0.1\",\"listen_port\":" << socks_port << "}],"
       << "\"outbounds\":[" << ob.str() << ",{\"type\":\"direct\",\"tag\":\"direct\"}],"
       << "\"route\":{\"rules\":[{\"protocol\":\"dns\",\"outbound\":\"direct\"},{\"ip_is_private\":true,\"outbound\":\"direct\"}],\"final\":\"proxy\"}"
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
    
    // Proxy groups and rules
    ss << "proxy-groups:\n"
       << "  - name: GLOBAL\n"
       << "    type: select\n"
       << "    proxies:\n"
       << "      - proxy\n"
       << "rules:\n"
       << "  - MATCH,proxy\n";
    
    return ss.str();
}

} // namespace hunter
