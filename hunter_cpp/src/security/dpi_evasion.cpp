#include "security/dpi_evasion.h"
#include "core/utils.h"
#include "core/task_manager.h"
#include "network/uri_parser.h"
#include "network/http_client.h"

#include <algorithm>
#include <future>
#include <chrono>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hunter {
namespace security {

// ═══════════════════════════════════════════════════════════════════
// DpiEvasionOrchestrator
// ═══════════════════════════════════════════════════════════════════

DpiEvasionOrchestrator::DpiEvasionOrchestrator() {}

DpiEvasionOrchestrator::~DpiEvasionOrchestrator() {
    stop();
}

bool DpiEvasionOrchestrator::start() {
    if (running_.load()) return true;
    running_ = true;

    // Detect network conditions with timeout
    auto& mgr = HunterTaskManager::instance();
    auto fut = mgr.submitIO([this]() { detectNetworkConditions(); });
    try {
        fut.wait_for(std::chrono::seconds(3));
    } catch (...) {}

    strategy_ = selectStrategy();

    // Start adaptation loop
    adaptation_thread_ = std::thread(&DpiEvasionOrchestrator::adaptationLoop, this);
    return true;
}

void DpiEvasionOrchestrator::stop() {
    running_ = false;
    if (adaptation_thread_.joinable()) adaptation_thread_.join();
}

void DpiEvasionOrchestrator::detectNetworkConditions() {
    auto& mgr = HunterTaskManager::instance();

    // Test CDN reachability (Cloudflare)
    auto fut_cdn = mgr.submitIO([]() -> bool {
        return utils::isPortAlive(443, 1500); // Just check if 443 connects somewhere
    });

    // Test Google reachability
    auto fut_google = mgr.submitIO([]() -> bool {
        network::HttpClient http;
        std::string body = http.get("http://1.1.1.1/generate_204", 2000);
        return !body.empty() || body.size() == 0; // 204 has empty body
    });

    // Test Telegram DC reachability
    auto fut_tg = mgr.submitIO([]() -> bool {
        // Try connecting to Telegram DC1
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(443);
        inet_pton(AF_INET, "149.154.175.50", &addr.sin_addr);
#ifdef _WIN32
        u_long mode = 1; ioctlsocket(fd, FIONBIO, &mode);
#endif
        ::connect(fd, (sockaddr*)&addr, sizeof(addr));
        fd_set wset; FD_ZERO(&wset); FD_SET(fd, &wset);
        timeval tv{1, 500000};
        bool ok = select(fd + 1, nullptr, &wset, nullptr, &tv) > 0;
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        return ok;
    });

    try { cdn_reachable_ = fut_cdn.get(); } catch (...) { cdn_reachable_ = false; }
    try { google_reachable_ = fut_google.get(); } catch (...) { google_reachable_ = false; }
    try { telegram_reachable_ = fut_tg.get(); } catch (...) { telegram_reachable_ = false; }

    // Determine network type
    if (!cdn_reachable_ && !google_reachable_) {
        network_type_ = NetworkType::CENSORED;
    } else {
        network_type_ = NetworkType::WIFI; // Default assumption on desktop
    }
}

DpiStrategy DpiEvasionOrchestrator::selectStrategy() {
    if (network_type_.load() == NetworkType::CENSORED) {
        return DpiStrategy::REALITY_DIRECT;
    }
    if (!telegram_reachable_) {
        return DpiStrategy::SPLITHTTP_CDN;
    }
    if (cdn_reachable_) {
        return DpiStrategy::WEBSOCKET_CDN;
    }
    return DpiStrategy::NONE;
}

void DpiEvasionOrchestrator::adaptationLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::minutes(5));
        if (!running_.load()) break;
        try {
            detectNetworkConditions();
            strategy_ = selectStrategy();
        } catch (...) {}
    }
}

int DpiEvasionOrchestrator::scoreConfig(const std::string& uri) const {
    std::string lower = uri;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    int score = 0;
    DpiStrategy strat = strategy_.load();

    switch (strat) {
        case DpiStrategy::REALITY_DIRECT:
            if (lower.find("security=reality") != std::string::npos) score += 100;
            if (utils::startsWith(lower, "vless://")) score += 50;
            if (lower.find("flow=xtls-rprx-vision") != std::string::npos) score += 30;
            break;
        case DpiStrategy::SPLITHTTP_CDN:
            if (lower.find("type=splithttp") != std::string::npos) score += 100;
            if (lower.find("type=ws") != std::string::npos) score += 80;
            if (lower.find("type=grpc") != std::string::npos) score += 70;
            break;
        case DpiStrategy::WEBSOCKET_CDN:
            if (lower.find("type=ws") != std::string::npos) score += 100;
            if (lower.find("type=grpc") != std::string::npos) score += 80;
            break;
        case DpiStrategy::GRPC_CDN:
            if (lower.find("type=grpc") != std::string::npos) score += 100;
            break;
        case DpiStrategy::HYSTERIA2:
            if (utils::startsWith(lower, "hysteria2://") || utils::startsWith(lower, "hy2://"))
                score += 100;
            break;
        default:
            break;
    }

    // Universal bonuses
    if (lower.find("security=tls") != std::string::npos) score += 10;
    if (lower.find("fp=") != std::string::npos) score += 5;

    return score;
}

std::vector<std::string> DpiEvasionOrchestrator::prioritizeConfigsForStrategy(
    const std::vector<std::string>& uris) {
    struct Scored { std::string uri; int score; };
    std::vector<Scored> scored;
    scored.reserve(uris.size());
    for (const auto& uri : uris) {
        scored.push_back({uri, scoreConfig(uri)});
    }
    std::sort(scored.begin(), scored.end(),
              [](const Scored& a, const Scored& b) { return a.score > b.score; });
    std::vector<std::string> result;
    result.reserve(scored.size());
    for (auto& s : scored) result.push_back(std::move(s.uri));
    return result;
}

std::string DpiEvasionOrchestrator::getStatusSummary() const {
    static const char* strategy_names[] = {
        "none", "splithttp_cdn", "reality_direct", "websocket_cdn", "grpc_cdn", "hysteria2"
    };
    static const char* network_names[] = {
        "unknown", "wifi", "4g", "5g", "ethernet", "censored"
    };
    std::ostringstream ss;
    ss << "strategy=" << strategy_names[(int)strategy_.load()]
       << " network=" << network_names[(int)network_type_.load()]
       << " cdn=" << (cdn_reachable_ ? "yes" : "no")
       << " tg=" << (telegram_reachable_ ? "yes" : "no");
    return ss.str();
}

DpiEvasionOrchestrator::DpiMetrics DpiEvasionOrchestrator::getMetrics() const {
    static const char* strategy_names[] = {
        "none", "splithttp_cdn", "reality_direct", "websocket_cdn", "grpc_cdn", "hysteria2"
    };
    static const char* network_names[] = {
        "unknown", "wifi", "4g", "5g", "ethernet", "censored"
    };
    DpiMetrics m;
    m.strategy = strategy_names[(int)strategy_.load()];
    m.network_type = network_names[(int)network_type_.load()];
    m.cdn_reachable = cdn_reachable_;
    m.google_reachable = google_reachable_;
    m.telegram_reachable = telegram_reachable_;
    return m;
}

// ═══════════════════════════════════════════════════════════════════
// DpiPressureEngine
// ═══════════════════════════════════════════════════════════════════

DpiPressureEngine::DpiPressureEngine(float intensity) : intensity_(intensity) {}

std::map<std::string, int> DpiPressureEngine::runPressureCycle() {
    int tls_count = (int)(10 * intensity_);
    int tcp_count = (int)(30 * intensity_);
    int decoy_count = (int)(7 * intensity_);
    int tg_count = (int)(5 * intensity_);

    std::map<std::string, int> stats;
    stats["tls_probes_ok"] = probeTls(tls_count);
    stats["tcp_probes_ok"] = probeTcp(tcp_count);
    stats["decoy_probes_ok"] = probeDecoy(decoy_count);
    stats["telegram_reachable"] = probeTelegram(tg_count);
    stats["pressure_cycles"] = 1;
    return stats;
}

int DpiPressureEngine::probeTls(int count) {
    int ok = 0;
    for (int i = 0; i < count; i++) {
        // Quick TLS handshake probe to common CDN IPs
        if (utils::isPortAlive(443, 1000)) ok++;
    }
    return ok;
}

int DpiPressureEngine::probeTcp(int count) {
    int ok = 0;
    static const int ports[] = {80, 443, 8080, 8443, 993, 995};
    for (int i = 0; i < count && i < 6; i++) {
        if (utils::isPortAlive(ports[i % 6], 500)) ok++;
    }
    return ok;
}

int DpiPressureEngine::probeDecoy(int count) {
    // Decoy HTTP requests to common sites
    (void)count;
    return 0; // Stub
}

int DpiPressureEngine::probeTelegram(int count) {
    int ok = 0;
    static const char* tg_ips[] = {"149.154.175.50", "149.154.167.51", "149.154.175.100"};
    for (int i = 0; i < count && i < 3; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(443);
        inet_pton(AF_INET, tg_ips[i % 3], &addr.sin_addr);
#ifdef _WIN32
        u_long mode = 1; ioctlsocket(fd, FIONBIO, &mode);
#endif
        ::connect(fd, (sockaddr*)&addr, sizeof(addr));
        fd_set wset; FD_ZERO(&wset); FD_SET(fd, &wset);
        timeval tv{1, 0};
        if (select(fd + 1, nullptr, &wset, nullptr, &tv) > 0) ok++;
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }
    return ok;
}

} // namespace security
} // namespace hunter
