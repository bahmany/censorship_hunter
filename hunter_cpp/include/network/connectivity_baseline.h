#pragma once
// Direct (non-proxied) connectivity baseline + failure attribution (design D1, amendment M5).
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/health_score.h"
#include "network/traffic_probe.h"

namespace hunter {
namespace network {

enum class BaselineState { Online, Offline, Indeterminate };
const char* baselineStateName(BaselineState s);

struct BaselineSnapshot {
    BaselineState state = BaselineState::Indeterminate;
    bool enabled = true;      // false => user disabled direct probes (M5): no direct evidence
    bool a_ok = false;        // direct Check A validated
    bool b_ok = false;        // direct Check B validated
    bool link_up = true;      // OS reports a usable link / default route
    double at = 0.0;          // UTC seconds when the measurement finished
    uint64_t generation = 0;  // network generation it belongs to
};

// Evidence from OTHER, already fully-passing tunnels in the same network generation.
// A candidate is never its own control.
class ControlTracker {
public:
    void record(const std::string& endpoint_key, uint32_t check_mask, double now, uint64_t generation = 0);
    /// A control counts only if its own Pass happened inside [t0, t1] (the failed round's window)
    /// on the same network generation. No retroactive application of later controls.
    bool passed(const std::string& exclude_key, uint32_t check_bit, double t0, double t1, uint64_t generation) const;
    void clear();
private:
    mutable std::mutex mu_;
    struct Entry { uint32_t mask = 0; double at = 0.0; uint64_t gen = 0; };
    std::map<std::string, Entry> last_;
};

class ConnectivityBaseline {
public:
    using LinkFn = std::function<bool()>;
    using SleepFn = std::function<void(double seconds)>;
    ConnectivityBaseline(std::shared_ptr<ProbeTransport> transport = nullptr,
                         ProbeConfig cfg = ProbeConfig(), ClockFn clock = nullptr,
                         LinkFn link = nullptr, SleepFn sleep = nullptr);

    // Process-wide instance (shared 30 s cache across ProxyTester objects). Honors
    // HUNTER_DIRECT_BASELINE=0 as the initial value of the privacy toggle.
    static std::shared_ptr<ConnectivityBaseline> shared();

    void setEnabled(bool on);
    bool enabled() const;
    void bumpGeneration();                 // network change: drop cached evidence
    uint64_t generation() const;

    // Cached snapshot, refreshed when older than max_age_s (30 s default cache;
    // pass a small value right after a tunnel failure to force fresh evidence).
    BaselineSnapshot current(double max_age_s = 30.0);

    static constexpr double kCacheSeconds = 30.0;
    static constexpr double kFailureRecheckSeconds = 5.0;

    ControlTracker& controls() { return controls_; }

private:
    BaselineSnapshot measure();

    std::shared_ptr<ProbeTransport> transport_;
    ProbeConfig cfg_;
    ClockFn clock_;
    LinkFn link_;
    SleepFn sleep_;
    mutable std::mutex mu_;
    bool enabled_ = true;
    uint64_t generation_ = 0;
    bool have_ = false;
    BaselineSnapshot cached_;
    ControlTracker controls_;
};

struct Attribution {
    ProbeOutcome outcome = ProbeOutcome::Indeterminate;
    bool attributable = false;
};

// Rules (D1):
//  - both checks pass -> Pass.
//  - a failed tunnel check counts against the server only when its matching DIRECT check
//    succeeded, or another already-confirmed tunnel passed (control).
//  - >=1 attributable failed check: RemoteFailure (none passed) or Partial (one passed).
//  - otherwise: Offline baseline -> LocalNetworkDown; everything else -> Indeterminate
//    (Partial stays Partial, unattributed).
//  - every local proxy port refused -> EngineError (never a server failure).
/// Round window slack: baseline/control evidence must fall in [start-kWindowSlack, end+kWindowSlack].
constexpr double kWindowSlackSeconds = 5.0;

/// `generation` is the network generation the round ran under; baseline and controls must match it.
Attribution classifyRound(const RawProbe& raw, const BaselineSnapshot& base,
                          const ControlTracker* controls, const std::string& endpoint_key,
                          uint64_t generation);

// Assemble the typed result consumed by ConfigDatabase::applyProbeResult.
ProbeResult buildProbeResult(const RawProbe& raw, const Attribution& attr, const std::string& endpoint_key,
                             const std::string& engine, const std::string& run_id, uint64_t generation);

}  // namespace network
}  // namespace hunter
