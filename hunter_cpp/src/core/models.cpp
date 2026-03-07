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
        snap.io_pool_size = std::max(2, base / 4);
        snap.cpu_pool_size = 2;
        snap.max_configs = 80;
        snap.scan_chunk = 20;
    } else if (snap.ram_percent >= 90) {
        snap.mode = ResourceMode::MINIMAL;
        snap.io_pool_size = std::max(4, base / 2);
        snap.cpu_pool_size = 2;
        snap.max_configs = 150;
        snap.scan_chunk = 30;
    } else if (snap.ram_percent >= 85) {
        snap.mode = ResourceMode::REDUCED;
        snap.io_pool_size = std::max(6, base);
        snap.cpu_pool_size = std::max(2, base / 2);
        snap.max_configs = 250;
        snap.scan_chunk = 40;
    } else if (snap.ram_percent >= 80) {
        snap.mode = ResourceMode::CONSERVATIVE;
        snap.io_pool_size = std::max(8, base + 2);
        snap.cpu_pool_size = std::max(2, base / 2);
        snap.max_configs = 400;
        snap.scan_chunk = 50;
    } else if (snap.ram_percent >= 70) {
        snap.mode = ResourceMode::SCALED;
        snap.io_pool_size = std::max(10, base + 4);
        snap.cpu_pool_size = std::max(3, base / 2);
        snap.max_configs = 600;
        snap.scan_chunk = 50;
    } else if (snap.ram_percent >= 60) {
        snap.mode = ResourceMode::MODERATE;
        snap.io_pool_size = std::max(12, base * 2);
        snap.cpu_pool_size = std::max(4, base);
        snap.max_configs = 800;
        snap.scan_chunk = 50;
    } else {
        snap.mode = ResourceMode::NORMAL;
        snap.io_pool_size = std::max(12, base * 2);
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

} // namespace hunter
