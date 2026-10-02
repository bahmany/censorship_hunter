#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "proxy/health_monitor.h"

namespace hunter {
namespace network { class ConfigDatabase; }
namespace proxy {

/**
 * @brief Legacy coarse status kept for source compatibility with the current GUI table.
 *        Mapping from SessionState: Connected->Running; Starting/Degraded/Switching/Restarting/
 *        Paused->Starting; Unavailable/Failed->Error; Stopped/Stopping->Stopped.
 *        "Running" therefore means "last real-traffic probe on the user port passed" - never just
 *        "process alive / port listening". New UI code should use snapshot().
 */
enum class ProxyStatus { Stopped = 0, Starting, Running, Error };

struct ProxyInstance {
    std::string uri;          // config URI the session was started with (GUI row key)
    int port = 0;             // user-facing local SOCKS5 port
    std::string engine;
    std::string config_path;  // unused (configs are owned by the engine guard); kept for compatibility
    ProxyStatus status = ProxyStatus::Stopped;
    std::string error_message;
    int pid = 0;
    double started_at = 0.0;
    // Informational only (Linux /proc rchar/wchar of the engine process): includes the engine's own
    // control/DNS I/O and is NOT used for any health decision.
    unsigned long long bytes_in = 0;
    unsigned long long bytes_out = 0;
    unsigned long long last_rchar = 0;
    unsigned long long last_wchar = 0;
    double last_traffic_poll = 0.0;
};

/// Injection points (all optional; empty = production default). Used by tests.
struct ManagerDeps {
    MonitorDeps monitor;                       // any empty field is filled with the production default
    std::function<bool(int)> port_free;        // default utils::isPortFree
    bool start_watchdog = true;                // false: tests drive step() themselves
    int port_first = 3110;
    int port_last = 3139;
};

/**
 * @brief Manages persistent local proxy servers with a health watchdog and automatic failover.
 *
 * startProxy()/stopProxy() never block the caller (no engine wait, no mutex held across waits).
 * Each session is a HealthMonitor (see health_monitor.h); a manager-owned watchdog thread ticks
 * every session once per second, independent of the GUI frame loop.
 */
class ProxyServerManager {
public:
    static constexpr int PORT_RANGE_START = 3110;
    static constexpr int PORT_RANGE_END = 3139;  // inclusive (was 3110-3120)

    ProxyServerManager();
    explicit ProxyServerManager(ManagerDeps deps, MonitorConfig cfg = MonitorConfig());
    ~ProxyServerManager();

    ProxyServerManager(const ProxyServerManager&) = delete;
    ProxyServerManager& operator=(const ProxyServerManager&) = delete;

    /// Asynchronous. Reserves a user-facing port and returns it immediately (0 + getError() if none
    /// is free). The session is "Starting" until a real-traffic probe through the port passes.
    int startProxy(const std::string& uri, SwitchMode mode = SwitchMode::Auto);
    /// Asynchronous cancel (generation bump). true if a session was active.
    bool stopProxy(const std::string& uri);
    bool stopProxyByPort(int port);
    void stopAll();
    /// Wait until every session finished teardown (children reaped, ports released).
    bool waitAllStopped(double timeout_s);

    bool isRunning(const std::string& uri) const;      // state == Connected
    int getPort(const std::string& uri) const;         // user port while the session is active, else 0
    ProxyStatus getStatus(const std::string& uri) const;
    std::string getError(const std::string& uri) const;
    std::pair<unsigned long long, unsigned long long> getTraffic(const std::string& uri) const;  // informational
    std::vector<ProxyInstance> getInstances() const;
    void poll();   // informational traffic counters only; health is owned by the watchdog

    // ── Snapshot + event API (for batch C2) ──
    bool snapshot(const std::string& uri, SessionSnapshot* out) const;
    std::vector<std::pair<std::string, SessionSnapshot>> snapshots() const;   // {session key (start URI), snapshot}
    /// Manual pin: true = never switch to another config (restarts of the same config still happen).
    bool setPinned(const std::string& uri, bool pinned);
    using SessionEventCallback = std::function<void(const std::string& session_uri, const SessionEvent&)>;
    void setEventCallback(SessionEventCallback cb);   // called from worker/watchdog threads, no locks held
    /// Use the database for failover candidates and to record every probe result (applyProbeResult).
    void attachDatabase(network::ConfigDatabase* db);
    void setCandidateProvider(std::function<std::vector<Candidate>()> fn);

    /// Test/driver hook: tick every session once (the internal watchdog does this every second).
    void tickAll();

private:
    struct Session {
        std::shared_ptr<HealthMonitor> mon;
        std::string uri;
        int port = 0;
        unsigned long long bytes_in = 0, bytes_out = 0, last_r = 0, last_w = 0;
        double last_poll = 0.0;
    };
    void init();
    int findFreePortLocked() const;
    static bool occupies(const Session& s);
    void watchdogLoop();
    void resolveEnginePaths();
    BuiltConfig buildEngineConfig(const std::string& uri, int port);
    std::pair<unsigned long long, unsigned long long> readProcessIo(int pid) const;
    ProxyInstance toInstance(const Session& s, const SessionSnapshot& snap) const;
    void pruneRetiredLocked();

    ManagerDeps deps_;
    MonitorConfig mcfg_;
    mutable std::mutex mutex_;     // guards sessions_/retired_/errors_ only; never held across waits
    std::map<std::string, Session> sessions_;
    std::vector<Session> retired_;
    std::map<std::string, std::string> errors_;
    SessionEventCallback event_cb_;
    std::function<std::vector<Candidate>()> candidate_fn_;
    std::atomic<network::ConfigDatabase*> db_{nullptr};
    std::shared_ptr<network::EngineLauncher> launcher_;

    std::string xray_path_, singbox_path_;
    bool paths_resolved_ = false;

    std::thread watchdog_;
    std::mutex wd_mu_;
    std::condition_variable wd_cv_;
    bool wd_stop_ = false;
};

}  // namespace proxy
}  // namespace hunter
