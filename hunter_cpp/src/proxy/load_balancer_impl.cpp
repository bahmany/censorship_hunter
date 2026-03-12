#include "proxy/load_balancer.h"
#include "proxy/xray_manager.h"
#include "network/uri_parser.h"
#include "core/utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {

int reserveBackendPort() {
    static std::mutex port_mutex;
    static int next_port = 30000;
    std::lock_guard<std::mutex> lock(port_mutex);
    for (int attempts = 0; attempts < 4000; ++attempts) {
        const int candidate = next_port++;
        if (next_port > 45000) next_port = 30000;
        if (!hunter::utils::isPortAlive(candidate, 100)) {
            return candidate;
        }
    }
    return 0;
}

} // namespace

namespace hunter {
namespace proxy {

MultiProxyServer::MultiProxyServer(int port, int max_backends)
    : port_(port), max_backends_(max_backends) {}

MultiProxyServer::~MultiProxyServer() {
    stop();
}

bool MultiProxyServer::start(const std::vector<std::pair<std::string, float>>& initial_configs) {
    if (running_.load()) return true;
    bool has_initial_configs = false;
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        available_configs_ = initial_configs;
        has_initial_configs = !available_configs_.empty();
        tcp_alive_ = false;
        socks_ready_ = false;
        http_ready_ = false;
        last_probe_ts_ = 0.0;
    }
    running_ = true;
    refreshBackends();
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        if (has_initial_configs && xray_processes_.empty()) {
            running_ = false;
            utils::LogRingBuffer::instance().push(
                "[Balancer] Failed to start listener on port " + std::to_string(port_) +
                " using XRay path " + xray_path_);
            return false;
        }
    }
    server_thread_ = std::thread(&MultiProxyServer::serverLoop, this);
    health_thread_ = std::thread(&MultiProxyServer::healthMonitorLoop, this);
    return true;
}

void MultiProxyServer::stop() {
    running_ = false;
    if (health_thread_.joinable()) health_thread_.join();
    if (server_thread_.joinable()) server_thread_.join();

    std::lock_guard<std::mutex> lock(backends_mutex_);
    for (const auto& xp : xray_processes_) {
        if (xp.pid > 0) {
            stopXRayProcess(xp.pid);
        }
    }
    xray_processes_.clear();
    backends_.clear();
    tcp_alive_ = false;
    socks_ready_ = false;
    http_ready_ = false;
    last_probe_ts_ = utils::nowTimestamp();
}

void MultiProxyServer::updateAvailableConfigs(
    const std::vector<std::pair<std::string, float>>& configs, bool trusted) {
    bool should_refresh = false;
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        available_configs_ = configs;
        should_refresh = running_.load();
    }
    if (should_refresh) {
        refreshBackends();
        if (trusted) {
            std::lock_guard<std::mutex> lock(backends_mutex_);
            for (auto& b : backends_) {
                for (const auto& cfg : configs) {
                    if (b.uri == cfg.first) {
                        b.trusted = true;
                        break;
                    }
                }
            }
        }
    }
}

bool MultiProxyServer::setForcedBackend(const std::string& uri, bool permanent) {
    if (!network::UriParser::isValidScheme(uri)) return false;
    bool should_refresh = false;
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        forced_uri_ = uri;
        forced_permanent_ = permanent;
        should_refresh = running_.load();
    }
    if (should_refresh) {
        refreshBackends();
    }
    return true;
}

void MultiProxyServer::clearForcedBackend() {
    bool should_refresh = false;
    {
        std::lock_guard<std::mutex> lock(backends_mutex_);
        forced_uri_.clear();
        forced_permanent_ = false;
        should_refresh = running_.load();
    }
    if (should_refresh) {
        refreshBackends();
    }
}

BalancerStatus MultiProxyServer::getStatus() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    BalancerStatus s;
    s.port = port_;
    s.running = running_.load();
    s.backend_count = static_cast<int>(backends_.size());
    s.tcp_alive = tcp_alive_;
    s.socks_ready = socks_ready_;
    s.http_ready = http_ready_;
    s.last_probe_ts = last_probe_ts_;
    s.forced_uri = forced_uri_;
    s.backends = backends_;
    for (const auto& b : s.backends) {
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
    entries.reserve(available_configs_.size());
    for (const auto& item : available_configs_) {
        std::string engine_used;
        auto backend_it = std::find_if(backends_.begin(), backends_.end(), [&](const Backend& b) {
            return b.uri == item.first;
        });
        if (backend_it != backends_.end()) {
            engine_used = backend_it->engine_used;
        } else if (engine_hint_callback_) {
            engine_used = engine_hint_callback_(item.first);
        }
        entries.push_back({item.first, shortenUri(item.first), item.second, engine_used});
    }
    return entries;
}

std::vector<Backend> MultiProxyServer::getWorkingBackends() const {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    std::vector<Backend> working;
    for (const auto& b : backends_) {
        if (b.state == BackendState::HEALTHY) working.push_back(b);
    }
    return working;
}

void MultiProxyServer::serverLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void MultiProxyServer::healthMonitorLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!running_.load()) break;

        const auto probe = utils::probeLocalMixedPort(port_, 1500);
        {
            std::lock_guard<std::mutex> lock(backends_mutex_);
            tcp_alive_ = probe.tcp_alive;
            socks_ready_ = probe.socks_ready;
            http_ready_ = probe.http_ready;
            last_probe_ts_ = utils::nowTimestamp();
        }
        if (!probe.mixed_ready()) {
            refreshBackends();
        }

        if (status_callback_) {
            status_callback_(getStatus());
        }
    }
}

void MultiProxyServer::refreshBackends() {
    std::lock_guard<std::mutex> lock(backends_mutex_);
    refreshBackends_unlocked();
}

void MultiProxyServer::refreshBackends_unlocked() {
    std::vector<std::pair<std::string, float>> sorted = available_configs_;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
    last_probe_ts_ = utils::nowTimestamp();

    if (!forced_uri_.empty()) {
        auto forced_it = std::find_if(sorted.begin(), sorted.end(), [&](const auto& item) {
            return item.first == forced_uri_;
        });
        if (forced_it == sorted.end()) {
            sorted.insert(sorted.begin(), {forced_uri_, 0.0f});
        } else if (forced_it != sorted.begin()) {
            auto forced = *forced_it;
            sorted.erase(forced_it);
            sorted.insert(sorted.begin(), forced);
        }
    }

    const int count = std::min(static_cast<int>(sorted.size()), max_backends_);
    backends_.clear();
    for (int i = 0; i < count; ++i) {
        Backend b;
        b.uri = sorted[i].first;
        b.latency_ms = sorted[i].second;
        b.state = BackendState::UNKNOWN;
        b.last_check = utils::nowTimestamp();
        if (engine_hint_callback_) {
            b.engine_used = engine_hint_callback_(b.uri);
        }
        backends_.push_back(b);
    }

    for (const auto& xp : xray_processes_) {
        if (xp.pid > 0) stopXRayProcess(xp.pid);
    }
    xray_processes_.clear();

    std::vector<int> backend_ports;
    backend_ports.reserve(backends_.size());
    for (auto& backend : backends_) {
        auto parsed = network::UriParser::parse(backend.uri);
        if (!parsed.has_value() || !parsed->isValid()) continue;
        const std::string resolved_engine = runtime_engine_manager_.resolveEngine(
            *parsed, backend.engine_used);
        if (resolved_engine.empty()) {
            backend.state = BackendState::DEAD;
            continue;
        }

        const int local_port = reserveBackendPort();
        if (local_port <= 0) {
            backend.state = BackendState::DEAD;
            continue;
        }

        const std::string config_text = runtime_engine_manager_.generateConfig(*parsed, local_port, resolved_engine);
        const std::string config_path = runtime_engine_manager_.writeConfigFile(resolved_engine, config_text);
        const int pid = runtime_engine_manager_.startProcess(resolved_engine, config_path);

        utils::LocalProxyProbeResult probe;
        bool ready = false;
        if (pid > 0 && utils::waitForPortAlive(local_port, 5000, 100)) {
            probe = utils::probeLocalMixedPort(local_port, 2000);
            ready = probe.mixed_ready();
        }
        if (pid > 0 && !ready) {
            runtime_engine_manager_.stopProcess(pid);
        }
        if (!ready) {
            backend.state = BackendState::DEAD;
            continue;
        }

        backend.state = BackendState::HEALTHY;
        backend.engine_used = resolved_engine;
        backend.local_port = local_port;
        backend.last_check = utils::nowTimestamp();
        backend_ports.push_back(local_port);

        XRayProcess xp;
        xp.uri = backend.uri;
        xp.engine_used = resolved_engine;
        xp.socks_port = local_port;
        xp.pid = pid;
        xp.alive = true;
        xray_processes_.push_back(xp);
    }

    if (backend_ports.empty()) {
        utils::LogRingBuffer::instance().push(
            "[Balancer] No valid backends available for port " + std::to_string(port_));
        for (auto& b : backends_) b.state = BackendState::DEAD;
        tcp_alive_ = false;
        socks_ready_ = false;
        http_ready_ = false;
        return;
    }

    const std::string config_json = XRayManager::generateLocalSocksBalancedConfig(backend_ports, port_, port_);
    if (config_json.empty()) {
        utils::LogRingBuffer::instance().push(
            "[Balancer] Failed to generate XRay config for port " + std::to_string(port_));
        for (const auto& xp : xray_processes_) {
            if (xp.pid > 0) stopXRayProcess(xp.pid);
        }
        xray_processes_.clear();
        for (auto& b : backends_) b.state = BackendState::DEAD;
        tcp_alive_ = false;
        socks_ready_ = false;
        http_ready_ = false;
        return;
    }

    const std::string config_path = runtime_engine_manager_.writeConfigFile("xray", config_json);
    const int pid = runtime_engine_manager_.startProcess("xray", config_path);
    utils::LocalProxyProbeResult probe;
    bool ready = false;
    if (pid > 0 && utils::waitForPortAlive(port_, 5000, 100)) {
        probe = utils::probeLocalMixedPort(port_, 2000);
        ready = probe.mixed_ready();
    }
    tcp_alive_ = probe.tcp_alive;
    socks_ready_ = probe.socks_ready;
    http_ready_ = probe.http_ready;
    last_probe_ts_ = utils::nowTimestamp();
    if (pid > 0 && !ready) {
        runtime_engine_manager_.stopProcess(pid);
    }
    if (!ready) {
        for (const auto& xp : xray_processes_) {
            if (xp.pid > 0) stopXRayProcess(xp.pid);
        }
        xray_processes_.clear();
        utils::LogRingBuffer::instance().push(
            "[Balancer] Failed to spawn mixed listener on port " + std::to_string(port_) +
            " using " + xray_path_ +
            " tcp=" + std::string(tcp_alive_ ? "1" : "0") +
            " socks=" + std::string(socks_ready_ ? "1" : "0") +
            " http=" + std::string(http_ready_ ? "1" : "0"));
        for (auto& b : backends_) b.state = BackendState::DEAD;
        return;
    }

    XRayProcess xp;
    xp.uri = forced_uri_.empty() ? "balanced" : forced_uri_;
    xp.engine_used = "xray";
    xp.socks_port = port_;
    xp.pid = pid;
    xp.alive = true;
    xray_processes_.push_back(xp);
}

bool MultiProxyServer::startXRayProcess(const std::string& uri, int socks_port) {
    auto parsed = network::UriParser::parse(uri);
    if (!parsed.has_value() || !parsed->isValid()) return false;
    std::string preferred_engine;
    if (engine_hint_callback_) preferred_engine = engine_hint_callback_(uri);
    const std::string resolved_engine = runtime_engine_manager_.resolveEngine(*parsed, preferred_engine);
    const std::string config_json = runtime_engine_manager_.generateConfig(*parsed, socks_port, resolved_engine);
    if (config_json.empty()) return false;
    const std::string config_path = runtime_engine_manager_.writeConfigFile(resolved_engine, config_json);
    const int pid = runtime_engine_manager_.startProcess(resolved_engine, config_path);
    if (pid <= 0) return false;
    XRayProcess xp;
    xp.uri = uri;
    xp.engine_used = resolved_engine;
    xp.socks_port = socks_port;
    xp.pid = pid;
    xp.alive = true;
    xray_processes_.push_back(xp);
    return true;
}

void MultiProxyServer::stopXRayProcess(int pid) {
    if (pid <= 0) return;
    runtime_engine_manager_.stopProcess(pid);
}

std::string MultiProxyServer::generateXRayConfig(const std::string& uri, int socks_port) const {
    auto parsed = network::UriParser::parse(uri);
    if (!parsed.has_value() || !parsed->isValid()) return "";
    return XRayManager::generateConfig(*parsed, socks_port, socks_port);
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
