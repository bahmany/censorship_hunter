#include "proxy/load_balancer.h"
#include "proxy/xray_manager.h"
#include "network/uri_parser.h"
#include "core/utils.h"
#include "core/task_manager.h"

#include <algorithm>
#include <sstream>
#include <chrono>

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
namespace proxy {

MultiProxyServer::MultiProxyServer(int port, int max_backends)
    : port_(port), max_backends_(max_backends) {}

MultiProxyServer::~MultiProxyServer() {
    stop();
}

bool MultiProxyServer::start(const std::vector<std::pair<std::string, float>>& initial_configs) {
    if (running_.load()) return true;

    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        available_configs_ = initial_configs;
    }

    refreshBackends();
    running_ = true;

    // Start health monitor thread
    health_thread_ = std::thread(&MultiProxyServer::healthMonitorLoop, this);

    return true;
}

void MultiProxyServer::stop() {
    running_ = false;
    if (health_thread_.joinable()) health_thread_.join();
    if (server_thread_.joinable()) server_thread_.join();

    // Stop all XRay processes
    std::lock_guard<std::mutex> lock(backends_mutex_);
    for (auto& xp : xray_processes_) {
        if (xp.pid > 0) {
            XRayManager mgr;
            mgr.stopProcess(xp.pid);
        }
    }
    xray_processes_.clear();
    backends_.clear();
}

void MultiProxyServer::updateAvailableConfigs(
    const std::vector<std::pair<std::string, float>>& configs, bool trusted) {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    available_configs_ = configs;

    // Mark trust level on existing backends
    if (trusted) {
        for (auto& b : backends_) {
            for (auto& [uri, lat] : configs) {
                if (b.uri == uri) { b.trusted = true; break; }
            }
        }
    }

    // Trigger refresh if we have new configs
    if (!configs.empty() && running_.load()) {
        // Schedule refresh in background
        try {
            HunterTaskManager::instance().submitIO([this]() { refreshBackends(); });
        } catch (...) {}
    }
}

bool MultiProxyServer::setForcedBackend(const std::string& uri, bool permanent) {
    if (!network::UriParser::isValidScheme(uri)) return false;
    std::lock_guard<std::mutex> lock(backends_mutex_);
    forced_uri_ = uri;
    forced_permanent_ = permanent;
    return true;
}

void MultiProxyServer::clearForcedBackend() {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    forced_uri_.clear();
    forced_permanent_ = false;
}

BalancerStatus MultiProxyServer::getStatus() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    BalancerStatus s;
    s.port = port_;
    s.running = running_.load();
    s.backend_count = (int)backends_.size();
    s.forced_uri = forced_uri_;
    s.backends = backends_;
    for (auto& b : s.backends) {
        if (b.state == BackendState::HEALTHY) s.healthy_count++;
    }
    return s;
}

std::vector<Backend> MultiProxyServer::getAllBackends() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    return backends_;
}

std::vector<MultiProxyServer::PoolEntry> MultiProxyServer::getAvailableConfigsList() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    std::vector<PoolEntry> entries;
    for (auto& [uri, lat] : available_configs_) {
        entries.push_back({uri, shortenUri(uri), lat});
    }
    return entries;
}

std::vector<Backend> MultiProxyServer::getWorkingBackends() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    std::vector<Backend> working;
    for (auto& b : backends_) {
        if (b.state == BackendState::HEALTHY) working.push_back(b);
    }
    return working;
}

void MultiProxyServer::refreshBackends() {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    refreshBackends_unlocked();
}

void MultiProxyServer::refreshBackends_unlocked() {
    // Sort available configs by latency
    auto sorted = available_configs_;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    // Take top N
    int count = std::min((int)sorted.size(), max_backends_);
    backends_.clear();
    for (int i = 0; i < count; i++) {
        Backend b;
        b.uri = sorted[i].first;
        b.latency_ms = sorted[i].second;
        b.state = BackendState::HEALTHY;
        b.last_check = utils::nowTimestamp();
        backends_.push_back(b);
    }
}

void MultiProxyServer::healthMonitorLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        if (!running_.load()) break;

        // Check port OUTSIDE the lock (isPortAlive blocks up to 2s)
        bool port_ok = utils::isPortAlive(port_, 2000);

        std::lock_guard<std::mutex> lock(backends_mutex_);
        if (!port_ok) {
            // Port not responding — try to refresh (unlocked version since we hold the lock)
            refreshBackends_unlocked();
        }

        if (status_callback_) {
            BalancerStatus s;
            s.port = port_;
            s.running = running_.load();
            s.backend_count = (int)backends_.size();
            s.forced_uri = forced_uri_;
            s.backends = backends_;
            for (auto& b : s.backends) {
                if (b.state == BackendState::HEALTHY) s.healthy_count++;
            }
            status_callback_(s);
        }
    }
}

std::string MultiProxyServer::shortenUri(const std::string& uri) const {
    if (uri.size() <= 60) return uri;
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos) return uri.substr(0, 60) + "...";
    std::string scheme = uri.substr(0, scheme_end + 3);
    return scheme + uri.substr(scheme_end + 3, 20) + "..." + uri.substr(uri.size() - 15);
}

} // namespace proxy
} // namespace hunter
