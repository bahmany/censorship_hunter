#include "proxy/generated_config_manager.h"

#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sstream>
#include <iomanip>
#include <iostream>

#include "core/utils.h"

// ── Legacy protocol constants (DO NOT CHANGE) ──────────────────────────
static constexpr const char* LEGACY_HOST = "api.abharcable.com";
static constexpr int         LEGACY_PORT = 443;

static const std::string UUID_VLESS_1 = "d6011bfa-0de3-4169-91b5-7816d2f6dd0b";
static const std::string UUID_VLESS_2 = "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc";
static const std::string TROJAN_PW_1  = "3f8a2c1b9e4d4a7f8b6c2d5e9f1a3b7c";
static const std::string TROJAN_PW_2  = "7a9d5e2f1b8c4d3e9a6f2b7c8e5d1f9a";
static const std::string SS_METHOD_PW = "chacha20-ietf-poly1305:AbharCable2024Secure!";
static const std::string HY2_PW_1     = "71737668a9d19ee78af7098e";
static const std::string HY2_PW_2     = "88af1a84a3415bb270793802";

// VMess base64 payloads (standard V2Ray format)
static const std::string VMESS_1_B64 =
    "eyJhZGQiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJhaWQiOiIwIiwiYWxwbiI6IiIsImZwIjoiY2hyb21lIiwiaG9zdCI6ImFwaS5hYmhhcmNhYmxlLmNvbSIsImlkIjoiZDYwMTFiZmEtMGRlMy00MTY5LTkxYjUtNzgxNmQyZjZkZDBiIiwibmV0Ijoid3MiLCJwYXRoIjoiL3hyYXkvdm1lc3MvIiwicG9ydCI6IjQ0MyIsInBzIjoiQWJoYXItVk1lc3MtMSIsInNjeSI6ImF1dG8iLCJzbmkiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJ0bHMiOiJ0bHMiLCJ0eXBlIjoibm9uZSIsInYiOiIyIn0=";
static const std::string VMESS_2_B64 =
    "eyJhZGQiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJhaWQiOiIwIiwiYWxwbiI6IiIsImZwIjoiY2hyb21lIiwiaG9zdCI6ImFwaS5hYmhhcmNhYmxlLmNvbSIsImlkIjoiOTZlMTBkNzctOWMwYy00NzNjLTllNzAtNGMzZmIxYmFhN2RjIiwibmV0Ijoid3MiLCJwYXRoIjoiL3hyYXkvdm1lc3MvIiwicG9ydCI6IjQ0MyIsInBzIjoiQWJoYXItVk1lc3MtMiIsInNjeSI6ImF1dG8iLCJzbmkiOiJhcGkuYWJoYXJjYWJsZS5jb20iLCJ0bHMiOiJ0bHMiLCJ0eXBlIjoibm9uZSIsInYiOiIyIn0=";

namespace hunter {
namespace proxy {

GeneratedConfigManager::GeneratedConfigManager() {}
GeneratedConfigManager::~GeneratedConfigManager() { stop(); }

void GeneratedConfigManager::start() {
    if (running_.load()) return;
    running_.store(true);

    generateConfigs();

    manager_thread_ = std::thread(&GeneratedConfigManager::managerLoop, this);
    utils::LogRingBuffer::instance().push(
        "[GeneratedConfig] Started with " + std::to_string(config_count_) + " configs on host " + public_host_);
}

void GeneratedConfigManager::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (manager_thread_.joinable()) {
        manager_thread_.join();
    }
    utils::LogRingBuffer::instance().push("[GeneratedConfig] Stopped");
}

// Helper: URL-encode a string for URI parameters
static std::string urlEncode(const std::string& s) {
    std::ostringstream ss;
    for (char c : s) {
        if (c == '/') ss << "%2F";
        else if (c == ' ') ss << "%20";
        else if (c == ':') ss << "%3A";
        else if (c == '!') ss << "%21";
        else ss << c;
    }
    return ss.str();
}

void GeneratedConfigManager::generateConfigs() {
    std::vector<std::pair<std::string, int>> upstreams;
    if (upstream_provider_) {
        upstreams = upstream_provider_();
    }
    if (upstreams.empty()) {
        utils::LogRingBuffer::instance().push(
            "[GeneratedConfig] No upstreams available, configs will be inactive");
    }

    {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    configs_.clear();
    double now = utils::nowTimestamp();

    auto assignUpstream = [&](GeneratedConfig& cfg, int idx) {
        cfg.created_ts = now;
        if (!upstreams.empty()) {
            int up_idx = idx % static_cast<int>(upstreams.size());
            cfg.upstream_port = upstreams[up_idx].second;
            cfg.upstream_name = "upstream-" + std::to_string(up_idx + 1);
            cfg.active = true;
        } else {
            cfg.active = false;
        }
    };

    // ── 1. VMess-1 ──────────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-VMess-1";
        c.protocol = "vmess";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.uuid = UUID_VLESS_1;
        c.ws_path = "/xray/vmess/";
        c.sni = LEGACY_HOST;
        c.fingerprint = "chrome";
        c.uri = "vmess://" + VMESS_1_B64;
        assignUpstream(c, 0);
        configs_.push_back(c);
    }
    // ── 2. Trojan-1 ─────────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-Trojan-1";
        c.protocol = "trojan";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.password = TROJAN_PW_1;
        c.ws_path = "/xray/trojan/";
        c.sni = LEGACY_HOST;
        c.fingerprint = "chrome";
        c.uri = "trojan://" + TROJAN_PW_1 + "@" + LEGACY_HOST + ":443"
                "?path=%2Fxray%2Ftrojan%2F&security=tls&host=" + LEGACY_HOST +
                "&fp=chrome&type=ws&sni=" + LEGACY_HOST + "#Abhar-Trojan-1";
        assignUpstream(c, 1);
        configs_.push_back(c);
    }
    // ── 3. VMess-2 ──────────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-VMess-2";
        c.protocol = "vmess";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.uuid = UUID_VLESS_2;
        c.ws_path = "/xray/vmess/";
        c.sni = LEGACY_HOST;
        c.fingerprint = "chrome";
        c.uri = "vmess://" + VMESS_2_B64;
        assignUpstream(c, 2);
        configs_.push_back(c);
    }
    // ── 4. Trojan-2 ─────────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-Trojan-2";
        c.protocol = "trojan";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.password = TROJAN_PW_2;
        c.ws_path = "/xray/trojan/";
        c.sni = LEGACY_HOST;
        c.fingerprint = "chrome";
        c.uri = "trojan://" + TROJAN_PW_2 + "@" + LEGACY_HOST + ":443"
                "?path=%2Fxray%2Ftrojan%2F&security=tls&host=" + LEGACY_HOST +
                "&fp=chrome&type=ws&sni=" + LEGACY_HOST + "#Abhar-Trojan-2";
        assignUpstream(c, 3);
        configs_.push_back(c);
    }

    // ── 5-16: VLESS configs (12 total) ──────────────────────────
    // (name, uuid, ws_path) tuples in exact order from the legacy list
    struct VlessDef { const char* name; const char* uuid; const char* path; };
    static const VlessDef vlessDefs[] = {
        {"Abhar-VLESS-2-3", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2_2/"},
        {"Abhar-VLESS-1-6", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2_p/"},
        {"Abhar-VLESS-1-3", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2_2/"},
        {"Abhar-VLESS-2-6", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2_p/"},
        {"Abhar-VLESS-2-4", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2_3/"},
        {"Abhar-VLESS-1-4", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2_3/"},
        {"Abhar-VLESS-2-1", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2/"},
        {"Abhar-VLESS-2-5", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2_4/"},
        {"Abhar-VLESS-1-2", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2_1/"},
        {"Abhar-VLESS-1-5", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2_4/"},
        {"Abhar-VLESS-1-1", "d6011bfa-0de3-4169-91b5-7816d2f6dd0b", "/ws/proxy/tunnel_v2/"},
        {"Abhar-VLESS-2-2", "96e10d77-9c0c-473c-9e70-4c3fb1baa7dc", "/ws/proxy/tunnel_v2_1/"},
    };
    for (int i = 0; i < 12; i++) {
        GeneratedConfig c;
        c.name = vlessDefs[i].name;
        c.protocol = "vless";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.uuid = vlessDefs[i].uuid;
        c.ws_path = vlessDefs[i].path;
        c.sni = LEGACY_HOST;
        c.alpn = "http/1.1";
        c.fingerprint = "chrome";
        c.uri = std::string("vless://") + vlessDefs[i].uuid + "@" + LEGACY_HOST + ":443"
                "?path=" + urlEncode(vlessDefs[i].path) +
                "&security=tls&alpn=http%2F1.1&encryption=none"
                "&host=" + LEGACY_HOST +
                "&fp=chrome&type=ws&sni=" + LEGACY_HOST + "#" + vlessDefs[i].name;
        assignUpstream(c, 4 + i);
        configs_.push_back(c);
    }

    // ── 17. Shadowsocks-1 ───────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-Shadowsocks-1";
        c.protocol = "ss";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.password = SS_METHOD_PW;
        c.uri = "ss://Y2hhY2hhMjAtaWV0Zi1wb2x5MTMwNTpBYmhhckNhYmxlMjAyNFNlY3VyZSE%3D@" + std::string(LEGACY_HOST) + ":443#Abhar-Shadowsocks-1";
        assignUpstream(c, 16);
        configs_.push_back(c);
    }
    // ── 18. Hysteria2-1 ─────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-Hysteria2-1";
        c.protocol = "hysteria2";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.password = HY2_PW_1;
        c.sni = LEGACY_HOST;
        c.uri = "hysteria2://" + HY2_PW_1 + "@" + LEGACY_HOST + ":443"
                "?security=tls&insecure=0&sni=" + LEGACY_HOST + "#Abhar-Hysteria2-1";
        assignUpstream(c, 17);
        configs_.push_back(c);
    }
    // ── 19. Hysteria2-2 ─────────────────────────────────────────
    {
        GeneratedConfig c;
        c.name = "Abhar-Hysteria2-2";
        c.protocol = "hysteria2";
        c.host = LEGACY_HOST; c.port = LEGACY_PORT;
        c.password = HY2_PW_2;
        c.sni = LEGACY_HOST;
        c.uri = "hysteria2://" + HY2_PW_2 + "@" + LEGACY_HOST + ":443"
                "?security=tls&insecure=0&sni=" + LEGACY_HOST + "#Abhar-Hysteria2-2";
        assignUpstream(c, 18);
        configs_.push_back(c);
    }

    last_generation_ts_.store(now);
    utils::LogRingBuffer::instance().push(
        "[GeneratedConfig] Generated " + std::to_string(configs_.size()) + " legacy configs (VLESS/VMess/Trojan/SS/Hysteria2 on :443)");
    } // release configs_mutex_

    notifyStateChange();
}

void GeneratedConfigManager::managerLoop() {
    while (running_.load()) {
        checkHealth();

        // Periodic state notification
        notifyStateChange();

        // Regenerate if upstreams changed (every 60s)
        for (int i = 0; i < 600 && running_.load(); i++) {
            usleep(100000); // 100ms increments, 60s total
        }
    }
}

void GeneratedConfigManager::checkHealth() {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    double now = utils::nowTimestamp();

    for (auto& cfg : configs_) {
        if (cfg.upstream_port == 0) continue;

        cfg.total_health_checks++;
        cfg.last_health_check = now;

        // Check if the upstream SOCKS port is alive
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            cfg.consecutive_failures++;
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(cfg.upstream_port);

        bool healthy = false;
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            // SOCKS5 handshake
            unsigned char hello[] = {0x05, 0x01, 0x00};
            if (send(sock, hello, sizeof(hello), 0) > 0) {
                unsigned char resp[2];
                if (recv(sock, resp, sizeof(resp), 0) >= 2 && resp[0] == 0x05 && resp[1] == 0x00) {
                    healthy = true;
                    cfg.total_passes++;
                    cfg.consecutive_failures = 0;
                    cfg.active = true;
                }
            }
        }

        if (!healthy) {
            cfg.consecutive_failures++;
            if (cfg.consecutive_failures > 5) {
                cfg.active = false;
            }
        }

        close(sock);
    }
}

std::vector<GeneratedConfig> GeneratedConfigManager::getConfigs() const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    return configs_;
}

std::string GeneratedConfigManager::formatConfigUri(const GeneratedConfig& cfg) const {
    return cfg.uri;
}

std::string GeneratedConfigManager::getConfigsJson() const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    std::ostringstream json;
    json << "{\"generated_configs\":[";
    for (size_t i = 0; i < configs_.size(); i++) {
        if (i > 0) json << ",";
        auto& cfg = configs_[i];
        json << "{\"name\":\"" << cfg.name << "\""
             << ",\"protocol\":\"" << cfg.protocol << "\""
             << ",\"host\":\"" << cfg.host << "\""
             << ",\"port\":" << cfg.port
             << ",\"upstream\":\"" << cfg.upstream_name << "\""
             << ",\"active\":" << (cfg.active ? "true" : "false")
             << ",\"latency_ms\":" << cfg.latency_ms
             << ",\"consecutive_failures\":" << cfg.consecutive_failures
             << ",\"total_health_checks\":" << cfg.total_health_checks
             << ",\"total_passes\":" << cfg.total_passes
             << ",\"active_sessions\":" << cfg.active_sessions
             << ",\"total_bytes\":" << cfg.total_bytes
             << ",\"created_ts\":" << cfg.created_ts
             << ",\"last_health_check\":" << cfg.last_health_check
             << ",\"uri\":\"" << cfg.uri << "\""
             << "}";
    }
    json << "],\"total\":" << configs_.size()
         << ",\"active\":" << getActiveCount()
         << ",\"generation_ts\":" << last_generation_ts_.load()
         << "}";
    return json.str();
}

std::string GeneratedConfigManager::getConfigsText() const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    std::ostringstream ss;
    ss << "# Hunter Generated Configurations\n";
    ss << "# Generated: " << std::fixed << std::setprecision(0) << last_generation_ts_.load() << "\n";
    ss << "# Host: " << public_host_ << "\n";
    ss << "# Total: " << configs_.size() << "\n";
    ss << "# Active: " << getActiveCount() << "\n\n";
    for (auto& cfg : configs_) {
        if (cfg.active) {
            ss << cfg.uri << "\n";
        }
    }
    return ss.str();
}

std::string GeneratedConfigManager::getConfigByName(const std::string& name) const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    for (auto& cfg : configs_) {
        if (cfg.name == name) {
            return cfg.uri;
        }
    }
    return "";
}

int GeneratedConfigManager::getActiveCount() const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    int count = 0;
    for (auto& cfg : configs_) {
        if (cfg.active) count++;
    }
    return count;
}

int GeneratedConfigManager::getTotalCount() const {
    std::lock_guard<std::mutex> lock(configs_mutex_);
    return static_cast<int>(configs_.size());
}

void GeneratedConfigManager::notifyStateChange() {
    if (state_cb_) {
        state_cb_(getConfigs());
    }
}

} // namespace proxy
} // namespace hunter
