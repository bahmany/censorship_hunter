#pragma once
// Health evidence, state machine and scoring (designs D1/D2). Pure logic:
// no I/O, no wall-clock access (callers pass `now`; ConfigDatabase injects a clock).
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace hunter {

enum class ProbeOutcome {
    Pass, RemoteFailure, Partial, LocalNetworkDown, Indeterminate,
    EngineError, BindConflict, Unsupported, InvalidConfig, Cancelled
};
enum class HealthState { Unknown, Testing, Healthy, Degraded, Unstable, Dead };
enum class Stability { Unrated, Stable, Unstable, Dead };

const char* outcomeName(ProbeOutcome o);
bool parseOutcome(const std::string& s, ProbeOutcome* o);
const char* healthStateName(HealthState s);
bool parseHealthState(const std::string& s, HealthState* o);
const char* stabilityName(Stability s);
bool parseStability(const std::string& s, Stability* o);

// Every tunable lives here (amendment M4); defaults are the D1/D2 proposals.
struct HealthThresholds {
    int ring_size = 20;
    double ewma_alpha = 0.2;                 // weight of sample i = alpha * (1-alpha)^i
    double latency_scale_ms = 500.0;         // L = 1 / (1 + p90 / scale)
    double freshness_half_life_s = 300.0;    // F = 2^(-age / half_life)
    double protocol_factor_secure = 1.0;
    double protocol_factor_insecure = 0.8;
    int degraded_failures = 1;
    int unstable_failures = 2;
    int dead_failures = 3;
    double dead_min_span_s = 30.0;           // first-to-latest failure span required for Dead
    double recovery_min_gap_s = 15.0;        // two full passes at least this far apart
    // Stable criteria
    int stable_min_successes = 6;
    double stable_min_span_s = 300.0;
    double stable_max_gap_s = 90.0;
    double stable_min_ewma = 0.90;
    double stable_max_p90_ms = 3000.0;
    double stable_max_success_age_s = 180.0;
    double stable_max_bulk_age_s = 300.0;
    double dead_retention_s = 72.0 * 3600.0;   // measured from dead_since (entry into Dead)
    size_t recent_id_capacity = 64;
    double excluded_retry_base_s = 5.0;        // backoff for schedulable-but-excluded rounds
    double excluded_retry_max_s = 300.0;
};

struct ProbeResult {
    std::string endpoint_key;
    std::string run_id;          // empty => no duplicate suppression
    uint64_t generation = 0;     // 0 = unspecified; older than the record's generation => stale
    int port = 0;
    std::string engine;
    ProbeOutcome outcome = ProbeOutcome::Indeterminate;
    double started_at = 0.0;     // UTC seconds
    double finished_at = 0.0;    // UTC seconds (used as the sample time)
    double latency_ms = -1.0;    // required finite >= 0 for Pass
    uint64_t bytes = 0;
    uint32_t checks = 0;         // bit flags, see kCheck*
    bool bulk_passed = false;    // 64 KiB transfer succeeded in this round
    // Caller's baseline attribution (D1). RemoteFailure/Partial count against the server
    // only when the caller sets this after baseline classification (matching direct or
    // control-tunnel evidence). Default false = unclassified => excluded, never penalised.
    bool attributable = false;
    std::string exit_ip;
    std::string exit_country;
};
constexpr uint32_t kCheckA = 1u, kCheckB = 2u, kCheckBulk = 4u;

struct ProbeSample {
    std::string id;
    double t = 0.0;
    bool success = false;
    double latency_ms = -1.0;    // < 0 => none (failure)
};

struct HealthEvidence {
    HealthState state = HealthState::Unknown;
    int probe_profile = 1;
    uint32_t eligible_count = 0;
    uint32_t success_streak = 0;
    uint32_t failure_streak = 0;
    double streak_started_at = 0.0;           // start of current success run (0 = none)
    double failure_streak_started_at = 0.0;
    double last_full_success = 0.0;
    double last_bulk_success = 0.0;
    double last_attempt_at = 0.0;
    uint64_t generation = 0;                  // session-only: network/engine generation
    double dead_since = 0.0;                  // entry time into Dead (persisted); 0 otherwise
    double recovery_anchor = 0.0;             // first full pass of a recovery (session-only)
    bool session_confirmed = false;           // a full pass was applied in this process/generation
    std::deque<std::string> recent_ids;       // bounded completed run ids, incl. excluded (session-only)
    std::string last_outcome;                 // outcomeName or empty
    std::deque<ProbeSample> ring;             // oldest -> newest, bounded
    // Static attributes used by P
    std::string protocol;
    bool insecure_tls = false;
    bool unsupported = false;
    bool telegram_only = false;
};

enum class ApplyEffect { Applied, Excluded, Duplicate, Stale, Rejected, UnknownEndpoint };

bool isEligibleResult(const ProbeResult& r);  // Pass, or attributable RemoteFailure/Partial
void markTesting(HealthEvidence& ev);         // Unknown -> Testing
ApplyEffect applyProbe(HealthEvidence& ev, const ProbeResult& r, const HealthThresholds& th);
// Restart / network-generation boundary: drops session-dependent confirmation (success run,
// bulk confirmation, certification) while keeping the historical ring for display.
void resetSessionEvidence(HealthEvidence& ev);
// Checks internal consistency (states/streaks vs ring); used when loading files.
bool evidenceConsistent(const HealthEvidence& ev, const HealthThresholds& th, std::string* why);

struct HealthEvaluation {
    double ewma = 0.0;        // S
    bool has_p90 = false;
    double p90_ms = 0.0;
    double latency_factor = 0.0;    // L
    double freshness = 0.0;         // F
    double protocol_factor = 0.0;   // P
    double score = 0.0;             // 0..100
    Stability stability = Stability::Unrated;
    bool stable = false;            // meets every Stable criterion at `now`
    bool switch_eligible = false;   // Healthy and last full success <= stable_max_success_age_s
    int tier = 3;                   // 0 Stable,1 Healthy,2 Degraded,3 Unknown/Testing,4 Unstable,5 Dead
};

HealthEvaluation evaluateHealth(const HealthEvidence& ev, double now, const HealthThresholds& th);

struct RankKey {
    int tier = 3;
    double score = 0.0;
    double last_full_success = 0.0;
    std::string key;
};
// Strict weak order: lower tier first, higher score, later full success, key lexicographic.
bool rankedBefore(const RankKey& a, const RankKey& b);

using ClockFn = std::function<double()>;  // UTC seconds
ClockFn systemClock();

}  // namespace hunter
