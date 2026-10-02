#pragma once
// Country service (design D4, batch C2): server-IP country (offline DB; optional verified DoH for
// domain endpoints), exit-country (cloudflare trace from the probe, in-tunnel fallbacks), caches,
// invalidation, persistence through ConfigDatabase::applyCountryResult (timestamp CAS), and the
// country-targeted discovery policy. No network or threads are started by construction; every
// transport / resolver / clock is injectable so unit tests need no public network.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/health_score.h"
#include "core/models.h"
#include "network/country_provider.h"
#include "network/traffic_probe.h"

namespace hunter {
namespace network { class ConfigDatabase; }
namespace geo {

// Provenance tags stored in *_country_source.
constexpr const char* kSrcOfflineIp = "offline_ip";                 // literal server IP -> offline DB
constexpr const char* kSrcDohOffline = "doh_offline";               // DoH-resolved server IPs -> offline DB
constexpr const char* kSrcNone = "none";                            // could not resolve (Unknown)
constexpr const char* kSrcTrace = "cloudflare_trace";               // www.cloudflare.com trace through the tunnel
constexpr const char* kSrcTraceFallback = "cloudflare_trace_1111";  // one.one.one.one trace through the tunnel
constexpr const char* kSrcOfflineExit = "offline_exit";             // ipify exit IP -> offline DB (lower confidence)

// ── DoH (optional, verified TLS, never local DNS) ───────────────────────
struct DohAnswer {
    bool ok = false;
    std::vector<std::string> ips;   // literal A/AAAA answers
    double ttl_s = 0.0;             // min TTL of the answers
    std::string error;
};
class DohResolver {
public:
    virtual ~DohResolver() = default;
    virtual DohAnswer resolve(const std::string& host) = 0;
};
/// Cloudflare JSON DoH by IP literal (https://1.1.1.1/dns-query), TLS verified, direct (no tunnel).
std::shared_ptr<DohResolver> makeCloudflareDohResolver(std::shared_ptr<network::ProbeTransport> transport);
/// Strict JSON parse of a DoH answer for one record family (exposed for tests).
DohAnswer parseDohJson(const std::string& body);

struct ServerCountry {
    std::string country;            // ISO, "" = Unknown, "Mixed"
    std::vector<std::string> ips;
    std::string source;             // kSrcOfflineIp / kSrcDohOffline / kSrcNone
    std::string db_version;
    double at = 0.0;
    double expires_at = 0.0;        // 0 = valid while db_version is unchanged
    bool from_cache = false;
};

struct ExitCountry {
    bool ok = false;                // a country (or measured IP) was determined
    std::string country;            // ISO ("" only with ok=false)
    std::string ip;
    std::string source;
    double at = 0.0;
    bool from_cache = false;
};

struct CountryServiceOptions {
    bool doh_enabled = false;
    double doh_max_ttl_s = 3600.0;        // D4: TTL <= 1 h
    double server_max_ttl_s = 86400.0;    // endpoint server cache: min(DNS TTL, 24 h)
    double negative_ttl_s = 60.0;         // DNS failure => Unknown, retry soon
    double exit_fresh_s = 1800.0;         // D4: UI freshness / refresh interval (30 min)
    size_t max_domain_entries = 20000;
};

class CountryService {
public:
    using Clock = ClockFn;
    CountryService(std::shared_ptr<network::ICountryProvider> provider,
                   std::shared_ptr<DohResolver> doh,
                   std::shared_ptr<network::ProbeTransport> transport,
                   Clock clock = nullptr, CountryServiceOptions opt = CountryServiceOptions());

    // ── Server country ──────────────────────────────────────────────────
    ServerCountry resolveServer(const std::string& host);
    /// Fill missing / outdated server countries for up to `budget` records (resolution happens
    /// outside the DB lock). Returns the number of records written (CAS accepted).
    int refreshServerCountries(network::ConfigDatabase& db, int budget);

    // ── Exit country ────────────────────────────────────────────────────
    /// Primary: the validated in-tunnel cloudflare trace already carried by a ProbeResult.
    /// Records it in the cache (the DB stores it via applyProbeResult). false if absent/invalid.
    bool onProbeResult(const ProbeResult& r, uint64_t network_gen, uint64_t profile_gen);
    /// Fallback chain through the live tunnel port: [www.cloudflare.com trace if try_primary] ->
    /// one.one.one.one trace -> ipify + offline DB. A failure never replaces the stored country.
    /// Persists through applyCountryResult (CAS) when `db` is given.
    ExitCountry refreshExit(network::ConfigDatabase* db, const std::string& endpoint_key, int port,
                            uint64_t network_gen, uint64_t profile_gen, bool try_primary, bool force);
    bool exitFresh(const std::string& endpoint_key, uint64_t network_gen, uint64_t profile_gen) const;
    ExitCountry cachedExit(const std::string& endpoint_key, uint64_t network_gen, uint64_t profile_gen) const;

    // ── Invalidation (D4) ───────────────────────────────────────────────
    void invalidateExit(const std::string& endpoint_key);            // activation / cutover
    void noteExitIp(const std::string& endpoint_key, const std::string& ip);   // IP change => drop
    void onNetworkGeneration(uint64_t gen);                           // network change: drop all exit entries
    void onGeoDbChanged();                                            // snapshot change: recompute offline entries
    void clearAll();

    size_t domainCacheSize() const;
    size_t exitCacheSize() const;
    network::ICountryProvider& provider() { return *provider_; }
    const CountryServiceOptions& options() const { return opt_; }
    void setDohEnabled(bool on);

    static bool isIpLiteral(const std::string& s);

private:
    struct ExitKey {
        std::string key; uint64_t net = 0; uint64_t prof = 0;
        bool operator<(const ExitKey& o) const {
            if (key != o.key) return key < o.key;
            if (net != o.net) return net < o.net;
            return prof < o.prof;
        }
    };
    ServerCountry fromIps(const std::vector<std::string>& ips, const char* source) const;
    void storeExit(const std::string& key, uint64_t net, uint64_t prof, const ExitCountry& e);

    std::shared_ptr<network::ICountryProvider> provider_;
    std::shared_ptr<DohResolver> doh_;
    std::shared_ptr<network::ProbeTransport> transport_;
    Clock clock_;
    CountryServiceOptions opt_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ServerCountry> domain_cache_;
    std::map<ExitKey, ExitCountry> exit_cache_;
};

// ── Country-targeted discovery (D4) ─────────────────────────────────────
struct CountryTarget {
    std::string iso;            // "" = no target
    bool strict = false;        // strict exit-country mode for recommendations / failover
    double exploration = 0.2;   // share of each batch reserved for Unknown exits (>= 0.2)
    double exit_fresh_s = 1800.0;
};
enum class TargetBucket { FreshExit = 0, ServerHint = 1, Unknown = 2, Other = 3 };
TargetBucket classifyForTarget(const ConfigHealthRecord& r, const CountryTarget& t, double now);
/// Reorders/trims `pool` (already in base-priority order) to at most `batch_size` records:
/// fresh matching exits, then matching server hints (and stale matching exits), then Unknown, with
/// at least ceil(exploration * batch) slots given to Unknown whenever such records exist.
/// Known non-matching records fill only what is left. No target => untouched.
void prioritizeForTarget(std::vector<ConfigHealthRecord>& pool, int batch_size, const CountryTarget& t, double now);
/// Strict mode: only a FRESH MEASURED exit in the target qualifies (server hints never do).
bool meetsStrictExitTarget(const ConfigHealthRecord& r, const CountryTarget& t, double now);

/// Thread-safe holder shared by the GUI, the discovery prioritizer and (later) batch B's failover.
class CountryTargetState {
public:
    CountryTarget get() const;
    void set(const CountryTarget& t);
private:
    mutable std::mutex mu_;
    CountryTarget t_;
};

}  // namespace geo
}  // namespace hunter
