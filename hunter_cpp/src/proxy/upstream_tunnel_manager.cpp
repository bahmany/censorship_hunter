#include "proxy/upstream_tunnel_manager.h"

#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#include "core/models.h"
#include "core/utils.h"

namespace hunter {
namespace proxy {

UpstreamTunnelManager::UpstreamTunnelManager() {}
UpstreamTunnelManager::~UpstreamTunnelManager() { stop(); }

void UpstreamTunnelManager::addUpstream(const UpstreamTunnelConfig& config) {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    configs_[config.name] = config;
    states_[config.name] = UpstreamTunnelState{};
    states_[config.name].name = config.name;
    states_[config.name].host = config.host;
    states_[config.name].local_socks_port = config.local_socks_port;
    states_[config.name].preferred = config.preferred;
}

void UpstreamTunnelManager::removeUpstream(const std::string& name) {
    stopTunnel(name);
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    configs_.erase(name);
    states_.erase(name);
}

void UpstreamTunnelManager::start() {
    if (running_.load()) return;
    running_.store(true);

    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    for (auto& [name, config] : configs_) {
        startTunnel(name);
    }

    manager_thread_ = std::thread(&UpstreamTunnelManager::managerLoop, this);
    utils::LogRingBuffer::instance().push(
        "[UpstreamTunnel] Started with " + std::to_string(configs_.size()) + " upstreams");
}

void UpstreamTunnelManager::stop() {
    if (!running_.load()) return;
    running_.store(false);

    {
        std::lock_guard<std::mutex> lock(tunnels_mutex_);
        for (auto& [name, _] : configs_) {
            stopTunnel(name);
        }
    }

    if (manager_thread_.joinable()) {
        manager_thread_.join();
    }
    utils::LogRingBuffer::instance().push("[UpstreamTunnel] Stopped");
}

void UpstreamTunnelManager::startTunnel(const std::string& name) {
    if (tunnel_threads_.count(name) && tunnel_threads_[name].joinable()) return;

    tunnel_threads_[name] = std::thread(&UpstreamTunnelManager::tunnelLoop, this, name);
}

void UpstreamTunnelManager::stopTunnel(const std::string& name) {
    auto it = states_.find(name);
    if (it == states_.end()) return;

    if (it->second.pid > 0) {
        kill(it->second.pid, SIGTERM);
        int status;
        int timeout = 50;
        while (timeout-- > 0 && waitpid(it->second.pid, &status, WNOHANG) == 0) {
            usleep(100000); // 100ms
        }
        if (timeout <= 0) {
            kill(it->second.pid, SIGKILL);
            waitpid(it->second.pid, &status, 0);
        }
        it->second.pid = -1;
    }
    it->second.connected = false;
    it->second.connecting = false;
}

void UpstreamTunnelManager::tunnelLoop(const std::string& name) {
    while (running_.load()) {
        auto cfg_it = configs_.find(name);
        if (cfg_it == configs_.end()) break;
        auto& cfg = cfg_it->second;

        auto& state = states_[name];
        state.connecting = true;
        state.last_error.clear();

        // Check if already connected
        if (checkSocksPort(cfg.local_socks_port)) {
            if (!state.connected) {
                state.connected = true;
                state.connecting = false;
                state.connected_since = utils::nowTimestamp();
                state.consecutive_failures = 0;
                utils::LogRingBuffer::instance().push(
                    "[UpstreamTunnel] " + name + " connected on port " +
                    std::to_string(cfg.local_socks_port));
                notifyStateChange();
            }
            // Measure latency periodically
            float lat = measureLatency(cfg.local_socks_port);
            if (lat > 0) {
                state.latency_ms = lat;
            }
        } else {
            // Need to establish tunnel
            if (state.connected) {
                state.connected = false;
                state.last_disconnect_ts = utils::nowTimestamp();
                state.consecutive_failures++;
                utils::LogRingBuffer::instance().push(
                    "[UpstreamTunnel] " + name + " disconnected, will reconnect");
                notifyStateChange();
            }

            // Kill old process if any
            if (state.pid > 0) {
                kill(state.pid, SIGTERM);
                int status;
                waitpid(state.pid, &status, WNOHANG);
                state.pid = -1;
            }

            // Build SSH command: ssh -D <port> -N -f -o ServerAliveInterval=30 -o ServerAliveCountMax=3
            //                     -o StrictHostKeyChecking=no -o ExitOnForwardFailure=yes
            //                     -i <key> user@host
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                // Close all file descriptors except stdin/stdout/stderr
                for (int fd = 3; fd < 256; fd++) close(fd);

                // Redirect stdin from /dev/null
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) { dup2(devnull, 0); close(devnull); }

                // Redirect stdout and stderr to /dev/null to prevent SSH
                // from holding the Docker log pipe open
                int devnull_w = open("/dev/null", O_WRONLY);
                if (devnull_w >= 0) {
                    dup2(devnull_w, 1);
                    dup2(devnull_w, 2);
                    close(devnull_w);
                }

                // Build args
                std::vector<std::string> args;
                args.push_back("ssh");
                args.push_back("-D");
                args.push_back("127.0.0.1:" + std::to_string(cfg.local_socks_port));
                args.push_back("-N");
                args.push_back("-f");
                args.push_back("-o");
                args.push_back("ServerAliveInterval=" + std::to_string(cfg.keepalive_interval_s));
                args.push_back("-o");
                args.push_back("ServerAliveCountMax=" + std::to_string(cfg.keepalive_count_max));
                args.push_back("-o");
                args.push_back("StrictHostKeyChecking=no");
                args.push_back("-o");
                args.push_back("UserKnownHostsFile=/dev/null");
                args.push_back("-o");
                args.push_back("ExitOnForwardFailure=yes");
                args.push_back("-o");
                args.push_back("ConnectTimeout=10");
                if (cfg.compression == "zlib") {
                    args.push_back("-C");
                }
                if (!cfg.identity_file.empty()) {
                    args.push_back("-i");
                    args.push_back(cfg.identity_file);
                }
                args.push_back(cfg.user + "@" + cfg.host);
                if (cfg.ssh_port != 22) {
                    args.push_back("-p");
                    args.push_back(std::to_string(cfg.ssh_port));
                }

                // Convert to char* array for execvp
                std::vector<char*> argv;
                for (auto& a : args) {
                    argv.push_back(const_cast<char*>(a.c_str()));
                }
                argv.push_back(nullptr);

                execvp("ssh", argv.data());
                _exit(127);
            } else if (pid > 0) {
                state.pid = pid;
                state.reconnect_count++;
                total_reconnects_++;
                // Wait for SSH to background itself (-f flag)
                int status;
                usleep(500000); // 500ms initial wait
                waitpid(pid, &status, WNOHANG); // Reap if already backgrounded

                // Wait for port to become available
                bool ready = false;
                for (int attempt = 0; attempt < 20 && running_.load(); attempt++) {
                    if (checkSocksPort(cfg.local_socks_port)) {
                        ready = true;
                        break;
                    }
                    usleep(500000); // 500ms
                }

                if (ready) {
                    state.connected = true;
                    state.connecting = false;
                    state.connected_since = utils::nowTimestamp();
                    state.consecutive_failures = 0;
                    state.last_error.clear();
                    utils::LogRingBuffer::instance().push(
                        "[UpstreamTunnel] " + name + " established on port " +
                        std::to_string(cfg.local_socks_port));
                } else {
                    state.connecting = false;
                    state.last_error = "SSH tunnel failed to establish";
                    state.consecutive_failures++;
                    utils::LogRingBuffer::instance().push(
                        "[UpstreamTunnel] " + name + " failed to establish tunnel");
                }
                notifyStateChange();
            }
        }

        // Sleep before next check
        for (int i = 0; i < cfg.reconnect_interval_s * 10 && running_.load(); i++) {
            usleep(100000); // 100ms increments
        }
    }
}

void UpstreamTunnelManager::managerLoop() {
    while (running_.load()) {
        // Periodic state notification
        notifyStateChange();

        // Build and broadcast metrics
        if (metrics_cb_) {
            metrics_cb_(buildMetricsJson());
        }

        for (int i = 0; i < 50 && running_.load(); i++) {
            usleep(100000); // 5 seconds total
        }
    }
}

bool UpstreamTunnelManager::checkSocksPort(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    // Set timeout
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    bool connected = (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);

    if (connected) {
        // Send SOCKS5 handshake: version=5, 1 method, no auth
        unsigned char hello[] = {0x05, 0x01, 0x00};
        if (send(sock, hello, sizeof(hello), 0) > 0) {
            unsigned char response[2];
            if (recv(sock, response, sizeof(response), 0) >= 2) {
                if (response[0] == 0x05 && response[1] == 0x00) {
                    close(sock);
                    return true;
                }
            }
        }
    }

    close(sock);
    return false;
}

float UpstreamTunnelManager::measureLatency(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0.0f;

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(sock);
        return 0.0f;
    }

    auto start = std::chrono::steady_clock::now();

    // SOCKS5 connect to Google DNS (8.8.8.8:53)
    unsigned char request[] = {
        0x05, 0x01, 0x00,  // Hello: version 5, 1 method, no auth
    };
    send(sock, request, sizeof(request), 0);

    unsigned char resp[2];
    if (recv(sock, resp, sizeof(resp), 0) < 2) { close(sock); return 0.0f; }

    // Connect request: 8.8.8.8:53
    unsigned char connect_req[] = {
        0x05, 0x01, 0x00, 0x01,  // SOCKS5, connect, reserved, IPv4
        0x08, 0x08, 0x08, 0x08,  // 8.8.8.8
        0x00, 0x35               // port 53
    };
    send(sock, connect_req, sizeof(connect_req), 0);

    unsigned char connect_resp[10];
    if (recv(sock, connect_resp, sizeof(connect_resp), 0) < 4) {
        close(sock);
        return 0.0f;
    }

    auto end = std::chrono::steady_clock::now();
    close(sock);

    if (connect_resp[1] == 0x00) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return static_cast<float>(ms);
    }
    return 0.0f;
}

std::vector<UpstreamTunnelState> UpstreamTunnelManager::getStates() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    std::vector<UpstreamTunnelState> result;
    for (auto& [name, state] : states_) {
        result.push_back(state);
    }
    return result;
}

UpstreamTunnelState UpstreamTunnelManager::getState(const std::string& name) const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    auto it = states_.find(name);
    if (it != states_.end()) return it->second;
    return UpstreamTunnelState{};
}

int UpstreamTunnelManager::getConnectedCount() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    int count = 0;
    for (auto& [name, state] : states_) {
        if (state.connected) count++;
    }
    return count;
}

int UpstreamTunnelManager::getTotalCount() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    return static_cast<int>(configs_.size());
}

std::vector<std::pair<std::string, int>> UpstreamTunnelManager::getConnectedSocksEndpoints() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    std::vector<std::pair<std::string, int>> result;
    for (auto& [name, state] : states_) {
        if (state.connected && state.local_socks_port > 0) {
            result.emplace_back("127.0.0.1:" + std::to_string(state.local_socks_port),
                               state.local_socks_port);
        }
    }
    // Sort by port number (preferred upstreams have lower ports)
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.second < b.second;
    });
    return result;
}

std::string UpstreamTunnelManager::getBestSocksEndpoint() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    const UpstreamTunnelState* best = nullptr;
    float best_latency = 999999.0f;

    for (auto& [name, state] : states_) {
        if (!state.connected) continue;
        float score = state.latency_ms;
        if (!state.preferred) score += 1000.0f; // Penalize non-preferred
        if (score < best_latency) {
            best_latency = score;
            best = &state;
        }
    }

    if (best) {
        return "127.0.0.1:" + std::to_string(best->local_socks_port);
    }
    return "";
}

void UpstreamTunnelManager::notifyStateChange() {
    if (state_cb_) {
        state_cb_(getStates());
    }
}

std::string UpstreamTunnelManager::buildMetricsJson() const {
    std::lock_guard<std::mutex> lock(tunnels_mutex_);
    std::ostringstream json;
    json << "{\"upstreams\":[";
    bool first = true;
    for (auto& [name, state] : states_) {
        if (!first) json << ",";
        first = false;
        json << "{\"name\":\"" << name << "\""
             << ",\"host\":\"" << state.host << "\""
             << ",\"port\":" << state.local_socks_port
             << ",\"connected\":" << (state.connected ? "true" : "false")
             << ",\"connecting\":" << (state.connecting ? "true" : "false")
             << ",\"latency_ms\":" << state.latency_ms
             << ",\"reconnect_count\":" << state.reconnect_count
             << ",\"consecutive_failures\":" << state.consecutive_failures
             << ",\"preferred\":" << (state.preferred ? "true" : "false")
             << ",\"connected_since\":" << state.connected_since
             << ",\"last_error\":\"" << state.last_error << "\""
             << "}";
    }
    json << "],\"total_upstreams\":" << configs_.size()
         << ",\"connected_upstreams\":" << getConnectedCount()
         << ",\"total_reconnects\":" << total_reconnects_.load()
         << "}";
    return json.str();
}

} // namespace proxy
} // namespace hunter
