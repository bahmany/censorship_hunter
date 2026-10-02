#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

namespace hunter {
namespace proxy {

/**
 * @brief Status of a running proxy server instance
 */
enum class ProxyStatus {
    Stopped = 0,
    Starting,
    Running,
    Error
};

/**
 * @brief Represents one running proxy server instance
 */
struct ProxyInstance {
    std::string uri;          // Config URI being proxied
    int port = 0;             // Local SOCKS5 port (3110-3120)
    std::string engine;       // "xray" or "sing-box"
    std::string config_path;  // Path to temp config file
    ProxyStatus status = ProxyStatus::Stopped;
    std::string error_message;
    int pid = 0;              // Process ID (0 = not running)
    double started_at = 0.0;  // Timestamp when started
    // Traffic tracking (cumulative bytes since proxy start)
    unsigned long long bytes_in = 0;   // Total bytes received (download)
    unsigned long long bytes_out = 0;  // Total bytes sent (upload)
    unsigned long long last_rchar = 0;  // Previous /proc/<pid>/io rchar
    unsigned long long last_wchar = 0;  // Previous /proc/<pid>/io wchar
    double last_traffic_poll = 0.0;     // Timestamp of last traffic poll
};

/**
 * @brief Manages persistent local proxy servers for live configs.
 *
 * Allows the user to start a local SOCKS5 proxy on a port in the
 * range [3110, 3120] that routes traffic through a selected proxy
 * config. Multiple instances can run simultaneously on different
 * ports. External applications (AI clients, browsers, CLI tools)
 * can connect to localhost:<port> and use the proxy.
 */
class ProxyServerManager {
public:
    static constexpr int PORT_RANGE_START = 3110;
    static constexpr int PORT_RANGE_END = 3120;  // inclusive

    ProxyServerManager();
    ~ProxyServerManager();

    ProxyServerManager(const ProxyServerManager&) = delete;
    ProxyServerManager& operator=(const ProxyServerManager&) = delete;

    /**
     * @brief Start a proxy server for the given config URI.
     * @param uri Proxy config URI (vmess://, vless://, ss://, trojan://, etc.)
     * @return Port number assigned (3110-3120), or 0 on error.
     *         On error, check getError(uri) for details.
     */
    int startProxy(const std::string& uri);

    /**
     * @brief Stop a proxy server for the given config URI.
     * @param uri Proxy config URI
     * @return true if stopped, false if not running
     */
    bool stopProxy(const std::string& uri);

    /**
     * @brief Stop proxy by port number.
     * @param port The port to stop
     * @return true if stopped, false if not running on that port
     */
    bool stopProxyByPort(int port);

    /**
     * @brief Stop all running proxy servers.
     */
    void stopAll();

    /**
     * @brief Check if a proxy is running for the given URI.
     */
    bool isRunning(const std::string& uri) const;

    /**
     * @brief Get the port for a running proxy by URI.
     * @return Port number, or 0 if not running
     */
    int getPort(const std::string& uri) const;

    /**
     * @brief Get the status of a proxy by URI.
     */
    ProxyStatus getStatus(const std::string& uri) const;

    /**
     * @brief Get error message for a URI (if status is Error).
     */
    std::string getError(const std::string& uri) const;

    /**
     * @brief Get traffic stats (bytes_in, bytes_out) for a proxy by URI.
     * @return pair of {bytes_in, bytes_out}, or {0,0} if not running
     */
    std::pair<unsigned long long, unsigned long long> getTraffic(const std::string& uri) const;

    /**
     * @brief Get a snapshot of all running proxy instances.
     *        Used by the GUI to render the table.
     */
    std::vector<ProxyInstance> getInstances() const;

    /**
     * @brief Monitor running proxy processes and update status.
     *        Called periodically from the GUI refresh loop.
     */
    void poll();

private:
    /**
     * @brief Find the first available port in [3110, 3120].
     * @return Available port, or 0 if none available.
     */
    int findFreePort() const;

    /**
     * @brief Generate config JSON for a URI on a given SOCKS port.
     *        Tries xray first, then sing-box. Sets engine_out to the
     *        engine name that should be used.
     */
    std::string generateConfig(const std::string& uri, int socks_port,
                               std::string& engine_out);

    /**
     * @brief Kill a process by PID (platform-specific).
     */
    void killProcess(int pid);

    /**
     * @brief Check if a process is still alive.
     */
    bool isProcessAlive(int pid) const;

    /**
     * @brief Read process I/O counters from /proc/<pid>/io (Linux).
     *        Returns {rchar, wchar} = bytes read/written by the process.
     *        On non-Linux, returns {0,0}.
     */
    std::pair<unsigned long long, unsigned long long> readProcessIo(int pid) const;

    /**
     * @brief Resolve engine binary paths (same logic as ProxyTester).
     */
    void resolveEnginePaths();

    mutable std::mutex mutex_;
    std::map<std::string, ProxyInstance> instances_;  // keyed by URI

    std::string xray_path_;
    std::string singbox_path_;
    bool paths_resolved_ = false;
};

} // namespace proxy
} // namespace hunter
