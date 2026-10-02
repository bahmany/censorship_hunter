#pragma once
// Runtime watchdog + failover state machine for ONE user-facing proxy session (Stage 4 B).
//
// Design: D1 (watchdog/failover) as amended by M3 - no custom gateway. Failover = verify a
// candidate on a lease port with a full real-traffic probe, then restart the engine on the
// user-facing port (brief gap, state "Switching"). Traffic is NEVER routed direct.
//
// Threading: step() is the cheap 1 s watchdog tick (process liveness + scheduling). All blocking
// work (launch, probe, failover) runs as at most one in-flight job on the injected Executor and
// never holds the monitor mutex while waiting. Every job captures a generation (session) and an
// engine epoch; results arriving after stop()/restart/failover are ignored.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/health_score.h"
#include "network/connectivity_baseline.h"
#include "network/engine_launcher.h"
#include "network/port_lease.h"

namespace hunter {
namespace proxy {

enum class SessionState {
    Stopped,      // no engine, nothing scheduled
    Starting,     // engine launching / first traffic probe pending (never "connected")
    Connected,    // last probe on the USER port was a full Pass
    Degraded,     // engine alive but last probe(s) failed / unconfirmed; may recover
    Switching,    // failover in progress (candidate verification + engine restart on user port)
    Restarting,   // engine exited; restart scheduled with backoff
    Paused,       // local outage (baseline Offline): no switching/restarts until it clears
    Unavailable,  // no usable upstream (no verified candidate / pinned & dropped); NOT connected
    Failed,       // terminal until the user starts again (bad config, binary missing, port busy)
    Stopping      // stop requested, teardown in progress
};
const char* sessionStateName(SessionState s);

enum class SwitchMode { Auto, Pinned };   // Pinned = manual pin: never switch to another config

struct Candidate {
    std::string uri;
    std::string endpoint_key;
    bool stable = false;   // Stability::Stable; false => provisional (healthy, unstable) fallback
    double score = 0.0;
};

struct BuiltConfig {
    bool ok = false;
    std::string engine;        // "xray" | "sing-box" | "mihomo"
    std::string config_text;
    std::string error;
};

struct MonitorConfig {
    double process_check_s = 1.0;        // cadence the owner calls step() at (informational)
    double probe_interval_s = 15.0;
    double probe_jitter = 0.10;          // +-10 %
    double confirm_interval_s = 5.0;     // accelerated re-probe after a failure
    double bulk_interval_s = 300.0;      // 64 KiB transfer cadence
    double outage_poll_s = 10.0;         // probe cadence while Paused
    int fail_threshold = 2;              // attributable failures before failover
    int max_candidates_per_window = 3;
    double candidate_window_s = 60.0;
    double candidate_cooldown_s = 60.0;
    std::vector<double> restart_backoff_s = {1, 2, 4, 8, 16, 30};
    double restart_jitter = 0.20;        // +-20 %
    int max_restarts = 3;
    double restart_window_s = 300.0;
    double stable_connected_reset_s = 60.0;  // Connected this long => restart streak resets
    double unavailable_retry_s = 30.0;
    double indeterminate_wait_s = 30.0;
    int launch_timeout_ms = 10000;
};

// Cooperative job runner. InlineExecutor runs jobs synchronously (deterministic tests).
class Executor {
public:
    virtual ~Executor() = default;
    virtual void post(std::function<void()> fn) = 0;
};
class InlineExecutor : public Executor {
public:
    void post(std::function<void()> fn) override { fn(); }
};
class ThreadPoolExecutor : public Executor {
public:
    explicit ThreadPoolExecutor(size_t threads = 2);
    ~ThreadPoolExecutor() override;   // drains queued work; joins (detaches itself if destroyed on a worker)
    void post(std::function<void()> fn) override;
private:
    struct State {
        std::mutex mu;
        std::condition_variable cv;
        std::deque<std::function<void()>> q;
        bool stop = false;
    };
    std::shared_ptr<State> st_;
    std::vector<std::thread> th_;
};

struct MonitorDeps {
    std::function<double()> steady;      // monotonic seconds
    std::function<double()> utc;         // UTC seconds
    std::function<double()> rand01;      // [0,1) jitter source
    std::shared_ptr<network::EngineLauncher> launcher;
    std::shared_ptr<network::PortLeaseRegistry> leases;
    std::shared_ptr<Executor> executor;
    // One CLASSIFIED real-traffic round through 127.0.0.1:<port> (TrafficProbe + baseline attribution).
    std::function<ProbeResult(int port, bool with_bulk, const std::string& endpoint_key, const std::string& engine)> probe;
    // Optional: current direct-connectivity verdict (Offline pauses switching/restarts).
    std::function<network::BaselineState()> baseline;
    // Ranked failover candidates (best first); the monitor re-probes before using any.
    std::function<std::vector<Candidate>()> candidates;
    std::function<BuiltConfig(const std::string& uri, int port)> build_config;
    std::function<std::string(const std::string& uri)> key_for;   // endpoint key; default = identity
    std::function<void(const ProbeResult&)> sink;                  // e.g. ConfigDatabase::applyProbeResult
    std::function<int(const std::shared_ptr<void>& guard)> pid_of; // optional (display only)
};

struct LastProbe {
    bool valid = false;
    ProbeOutcome outcome = ProbeOutcome::Indeterminate;
    bool attributable = false;
    bool bulk_passed = false;
    double latency_ms = -1.0;
    double at_utc = 0.0;
    std::string exit_country;
};

struct SessionSnapshot {
    SessionState state = SessionState::Stopped;
    SwitchMode mode = SwitchMode::Auto;
    std::string uri;
    std::string endpoint_key;       // current key (changes on failover)
    std::string engine;
    int user_port = 0;
    int engine_pid = 0;             // display only
    uint64_t generation = 0;        // session generation (bumped by start/stop)
    bool provisional = false;       // current config came from the provisional (non-Stable) fallback tier
    bool local_outage = false;      // baseline Offline: switching/restarts are paused
    int consecutive_failures = 0;   // attributable failures since last Pass
    int restarts_in_window = 0;
    int candidates_tried_in_window = 0;
    int failovers = 0;
    std::string previous_key;       // key we failed over from ("" if none)
    LastProbe last_probe;
    std::string reason;             // human-readable cause of the current state
    double state_since_utc = 0.0;
    double connected_since_utc = 0.0;   // 0 unless state == Connected
    bool connected() const { return state == SessionState::Connected; }
};

enum class EventKind {
    StateChanged, ProbeCompleted, EngineExited, RestartScheduled,
    FailoverStarted, FailoverSucceeded, FailoverFailed, Stopped
};
const char* eventKindName(EventKind k);
struct SessionEvent {
    EventKind kind = EventKind::StateChanged;
    std::string detail;
    SessionSnapshot snapshot;   // state right after the event
};
using EventCallback = std::function<void(const SessionEvent&)>;

class HealthMonitor : public std::enable_shared_from_this<HealthMonitor> {
public:
    static std::shared_ptr<HealthMonitor> create(MonitorConfig cfg, MonitorDeps deps);
    ~HealthMonitor();

    /// Begin a session on `user_port` (async; returns immediately). Replaces any previous session
    /// state (generation bump). Connected is reported only after a Pass probe on `user_port`.
    void start(const std::string& uri, int user_port, SwitchMode mode = SwitchMode::Auto);
    /// Cancel everything (async): generation bump, pending jobs become stale, engine reaped.
    void stop();
    void setMode(SwitchMode m);
    /// Watchdog tick (call about once a second from the owner's thread). Never blocks.
    void step();
    SessionSnapshot snapshot() const;
    void setEventCallback(EventCallback cb);
    /// True once state==Stopped (teardown finished, no job in flight). Waits up to timeout_s.
    bool waitStopped(double timeout_s) const;
    bool active() const;   // state not Stopped / Failed

private:
    HealthMonitor(MonitorConfig cfg, MonitorDeps deps);
    struct Impl;
    std::shared_ptr<Impl> d_;
};


}  // namespace proxy
}  // namespace hunter
