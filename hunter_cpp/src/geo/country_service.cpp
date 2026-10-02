#include "geo/country_service.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "geo/country_query.h"
#include "network/continuous_validator.h"
#include "network/uri_parser.h"

namespace hunter {
namespace geo {

namespace {

bool validDnsName(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    for (char c : h)
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_')) return false;
    return true;
}

std::string stripBrackets(std::string h) {
    if (h.size() > 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    return h;
}

// Minimal scanner for flat JSON string/number members inside one object text.
bool jsonStr(const std::string& obj, const char* key, std::string* out) {
    std::string k = std::string("\"") + key + "\"";
    auto p = obj.find(k);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
    if (p >= obj.size() || obj[p] != '"') return false;
    auto e = obj.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = obj.substr(p + 1, e - p - 1);
    return true;
}
bool jsonNum(const std::string& obj, const char* key, double* out) {
    std::string k = std::string("\"") + key + "\"";
    auto p = obj.find(k);
    if (p == std::string::npos) return false;
    p = obj.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < obj.size() && std::isspace((unsigned char)obj[p])) p++;
    char* end = nullptr;
    double v = std::strtod(obj.c_str() + p, &end);
    if (end == obj.c_str() + p || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

}  // namespace

bool CountryService::isIpLiteral(const std::string& s0) {
    std::string s = stripBrackets(s0);
    unsigned char buf[16];
    return inet_pton(AF_INET, s.c_str(), buf) == 1 || inet_pton(AF_INET6, s.c_str(), buf) == 1;
}

DohAnswer parseDohJson(const std::string& body) {
    DohAnswer a;
    if (body.empty() || body.size() > 16384) { a.error = "bad size"; return a; }
    double status = -1;
    if (!jsonNum(body, "Status", &status) || status != 0) { a.error = "dns status"; return a; }
    auto p = body.find("\"Answer\"");
    if (p == std::string::npos) { a.ok = true; return a; }   // NOERROR with no usable records
    auto lb = body.find('[', p);
    auto rb = body.find(']', lb == std::string::npos ? p : lb);
    if (lb == std::string::npos || rb == std::string::npos) { a.error = "bad answer"; return a; }
    double min_ttl = -1;
    size_t pos = lb;
    int objs = 0;
    while (true) {
        auto ob = body.find('{', pos);
        if (ob == std::string::npos || ob > rb) break;
        auto oe = body.find('}', ob);
        if (oe == std::string::npos) { a.error = "bad answer"; return a; }
        std::string obj = body.substr(ob, oe - ob + 1);
        pos = oe + 1;
        if (++objs > 64) break;
        double type = 0, ttl = 0;
        std::string data;
        if (!jsonNum(obj, "type", &type) || !jsonStr(obj, "data", &data)) continue;
        if (type != 1 && type != 28) continue;   // skip CNAME etc.
        if (!CountryService::isIpLiteral(data)) continue;
        if (!jsonNum(obj, "TTL", &ttl) || ttl < 0) ttl = 0;
        a.ips.push_back(data);
        min_ttl = min_ttl < 0 ? ttl : std::min(min_ttl, ttl);
    }
    a.ok = true;
    a.ttl_s = min_ttl < 0 ? 0.0 : min_ttl;
    return a;
}

namespace {
class CloudflareDoh : public DohResolver {
public:
    explicit CloudflareDoh(std::shared_ptr<network::ProbeTransport> t) : t_(std::move(t)) {}
    DohAnswer resolve(const std::string& host) override {
        DohAnswer out;
        if (!t_ || !validDnsName(host)) { out.error = "bad host"; return out; }
        bool any_ok = false;
        for (const char* type : {"A", "AAAA"}) {
            network::TransportRequest rq;
            rq.url = "https://1.1.1.1/dns-query?name=" + host + "&type=" + type + "&ct=application/dns-json";
            rq.proxy_port = 0;   // direct; destination is an IP literal => no local DNS involved
            rq.connect_timeout_ms = 3000;
            rq.total_timeout_ms = 5000;
            rq.max_body_bytes = 16384;
            rq.tls_verify = true;
            auto resp = t_->fetch(rq);
            if (resp.error != network::TransportError::None || resp.status != 200) continue;
            DohAnswer a = parseDohJson(resp.body);
            if (!a.ok) continue;
            any_ok = true;
            for (auto& ip : a.ips) out.ips.push_back(ip);
            if (!a.ips.empty()) out.ttl_s = out.ttl_s == 0 ? a.ttl_s : std::min(out.ttl_s, a.ttl_s);
        }
        out.ok = any_ok;
        if (!any_ok) out.error = "doh failed";
        return out;
    }
private:
    std::shared_ptr<network::ProbeTransport> t_;
};
}  // namespace

std::shared_ptr<DohResolver> makeCloudflareDohResolver(std::shared_ptr<network::ProbeTransport> t) {
    return std::make_shared<CloudflareDoh>(std::move(t));
}

// ── CountryService ──────────────────────────────────────────────────────

CountryService::CountryService(std::shared_ptr<network::ICountryProvider> provider,
                               std::shared_ptr<DohResolver> doh,
                               std::shared_ptr<network::ProbeTransport> transport,
                               Clock clock, CountryServiceOptions opt)
    : provider_(provider ? std::move(provider) : network::makeNullCountryProvider()),
      doh_(std::move(doh)), transport_(std::move(transport)),
      clock_(clock ? std::move(clock) : systemClock()), opt_(opt) {}

void CountryService::setDohEnabled(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    opt_.doh_enabled = on;
}

ServerCountry CountryService::fromIps(const std::vector<std::string>& ips, const char* source) const {
    ServerCountry s;
    s.source = source;
    s.ips = ips;
    s.db_version = provider_->version();
    std::string first;
    bool mixed = false;
    for (const auto& ip : ips) {
        std::string c = provider_->lookupIp(ip);
        if (c.empty()) continue;   // private / reserved / unmapped IPs are not "usable"
        if (first.empty()) first = c;
        else if (c != first) mixed = true;
    }
    s.country = mixed ? std::string(kMixed) : first;
    return s;
}

ServerCountry CountryService::resolveServer(const std::string& host_in) {
    const double now = clock_();
    std::string host = stripBrackets(host_in);
    if (host.empty()) { ServerCountry s; s.source = kSrcNone; s.at = now; return s; }

    if (isIpLiteral(host)) {
        ServerCountry s = fromIps({host}, kSrcOfflineIp);
        s.at = now;
        return s;
    }

    // Domain endpoint: never local DNS. Only the optional verified DoH adapter may resolve it.
    std::string lh = toLowerAscii(host);
    while (!lh.empty() && lh.back() == '.') lh.pop_back();
    bool doh_on;
    {
        std::lock_guard<std::mutex> lk(mu_);
        doh_on = opt_.doh_enabled && doh_;
        auto it = domain_cache_.find(lh);
        if (it != domain_cache_.end() && it->second.expires_at > now) {
            ServerCountry s = it->second;
            if (s.db_version != provider_->version() && !s.ips.empty()) {
                // Snapshot changed: recompute from the stored IP set (offline-derived only).
                ServerCountry r = fromIps(s.ips, s.source.c_str());
                r.at = s.at; r.expires_at = s.expires_at;
                it->second = r;
                s = r;
            }
            s.from_cache = true;
            return s;
        }
    }
    ServerCountry s;
    s.at = now;
    s.db_version = provider_->version();
    if (!doh_on) {
        s.source = kSrcNone;
        s.expires_at = now + opt_.doh_max_ttl_s;
    } else {
        DohAnswer a = doh_->resolve(lh);   // network I/O outside the lock
        if (a.ok && !a.ips.empty()) {
            s = fromIps(a.ips, kSrcDohOffline);
            s.at = now;
            double ttl = std::min({a.ttl_s > 0 ? a.ttl_s : opt_.doh_max_ttl_s, opt_.doh_max_ttl_s, opt_.server_max_ttl_s});
            s.expires_at = now + std::max(ttl, 1.0);
        } else {
            s.source = kSrcNone;   // DNS failure => Unknown, retried after the negative TTL
            s.expires_at = now + opt_.negative_ttl_s;
        }
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (domain_cache_.size() >= opt_.max_domain_entries) domain_cache_.clear();
        domain_cache_[lh] = s;
    }
    return s;
}

int CountryService::refreshServerCountries(network::ConfigDatabase& db, int budget) {
    struct Work { std::string key, uri; };
    std::vector<Work> work;
    const double now = clock_();
    const std::string ver = provider_->version();
    const double recheck = opt_.doh_max_ttl_s;
    db.forEachRecord([&](const ConfigHealthRecord& r) {
        if ((int)work.size() >= budget) return;
        bool need = r.server_country_at <= 0.0;
        if (!need) {
            if (r.server_country_source == kSrcOfflineIp) need = (r.geo_db_version != ver);
            else need = (now - r.server_country_at) > recheck || r.geo_db_version != ver;
        }
        if (need) work.push_back({r.endpoint_key.empty() ? r.uri_hash : r.endpoint_key, r.uri});
    });
    int written = 0;
    for (const auto& w : work) {
        std::string host;
        if (auto pc = network::UriParser::parse(w.uri)) host = pc->address;
        ServerCountry s = resolveServer(host);
        network::ConfigDatabase::CountryUpdate u;
        u.is_exit = false;
        u.country = s.country;
        u.source = s.source;
        u.at = now;
        u.geo_db_version = s.db_version;
        u.server_ips = s.ips;
        if (db.applyCountryResult(w.key, u)) written++;
    }
    return written;
}

// ── exit ────────────────────────────────────────────────────────────────

void CountryService::storeExit(const std::string& key, uint64_t net, uint64_t prof, const ExitCountry& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (exit_cache_.size() > 50000) exit_cache_.clear();
    exit_cache_[ExitKey{key, net, prof}] = e;
}

bool CountryService::onProbeResult(const ProbeResult& r, uint64_t net, uint64_t prof) {
    if (r.outcome != ProbeOutcome::Pass || r.exit_country.empty() || !isValidIso(r.exit_country)) return false;
    ExitCountry e;
    e.ok = true;
    e.country = r.exit_country;
    e.ip = r.exit_ip;
    e.source = kSrcTrace;
    e.at = r.finished_at > 0 ? r.finished_at : clock_();
    noteExitIp(r.endpoint_key, r.exit_ip);
    storeExit(r.endpoint_key, net, prof, e);
    return true;
}

ExitCountry CountryService::cachedExit(const std::string& key, uint64_t net, uint64_t prof) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = exit_cache_.find(ExitKey{key, net, prof});
    if (it == exit_cache_.end()) return ExitCountry();
    ExitCountry e = it->second;
    e.from_cache = true;
    return e;
}

bool CountryService::exitFresh(const std::string& key, uint64_t net, uint64_t prof) const {
    ExitCountry e = cachedExit(key, net, prof);
    return e.ok && (clock_() - e.at) <= opt_.exit_fresh_s && clock_() >= e.at - 1.0;
}

namespace {
bool fetchTrace(network::ProbeTransport& t, const std::string& host, int port,
                network::TraceInfo* info) {
    network::TransportRequest rq;
    rq.url = "https://" + host + "/cdn-cgi/trace";
    rq.proxy_port = port;
    rq.connect_timeout_ms = 3000;
    rq.total_timeout_ms = 8000;
    rq.max_body_bytes = 4096;
    rq.tls_verify = true;
    auto resp = t.fetch(rq);
    if (resp.error != network::TransportError::None || resp.status != 200) return false;
    if (resp.body.size() < 64 || resp.body.size() > 4096) return false;
    if (resp.content_type.find("text/plain") == std::string::npos) return false;
    *info = network::parseCloudflareTrace(resp.body, host);
    return info->valid;
}

bool parseIpifyJson(const std::string& body, std::string* ip) {
    if (body.empty() || body.size() > 1024) return false;
    static const std::regex re("^\\s*\\{\\s*\"ip\"\\s*:\\s*\"([0-9a-fA-F:.]{2,45})\"\\s*\\}\\s*$");
    std::smatch m;
    if (!std::regex_match(body, m, re)) return false;
    *ip = m[1].str();
    return network::isPublicIpLiteral(*ip);
}
}  // namespace

ExitCountry CountryService::refreshExit(network::ConfigDatabase* db, const std::string& key, int port,
                                        uint64_t net, uint64_t prof, bool try_primary, bool force) {
    if (!force && exitFresh(key, net, prof)) return cachedExit(key, net, prof);
    ExitCountry result;
    if (!transport_ || port <= 0) return result;   // no live tunnel: keep the previous (stale) value

    network::TraceInfo info;
    if (try_primary && fetchTrace(*transport_, "www.cloudflare.com", port, &info) && isValidIso(info.loc)) {
        result.ok = true; result.country = info.loc; result.ip = info.ip; result.source = kSrcTrace;
    }
    if (!result.ok && fetchTrace(*transport_, "one.one.one.one", port, &info) && isValidIso(info.loc)) {
        result.ok = true; result.country = info.loc; result.ip = info.ip; result.source = kSrcTraceFallback;
    }
    if (!result.ok) {
        // Independent fallback: exit IP from ipify through the tunnel, country from the offline DB.
        network::TransportRequest rq;
        rq.url = "https://api64.ipify.org?format=json";
        rq.proxy_port = port;
        rq.connect_timeout_ms = 3000;
        rq.total_timeout_ms = 8000;
        rq.max_body_bytes = 1024;
        rq.tls_verify = true;
        auto resp = transport_->fetch(rq);
        std::string ip;
        if (resp.error == network::TransportError::None && resp.status == 200 && parseIpifyJson(resp.body, &ip)) {
            std::string c = provider_->lookupIp(ip);
            if (!c.empty() && isValidIso(c)) {
                result.ok = true; result.country = c; result.ip = ip; result.source = kSrcOfflineExit;
            }
        }
    }
    if (!result.ok) return result;

    result.at = clock_();
    noteExitIp(key, result.ip);
    storeExit(key, net, prof, result);
    if (db) {
        network::ConfigDatabase::CountryUpdate u;
        u.is_exit = true;
        u.country = result.country;
        u.ip = result.ip;
        u.source = result.source;
        u.at = result.at;
        u.network_generation = net;
        db->applyCountryResult(key, u);
    }
    return result;
}

// ── invalidation ────────────────────────────────────────────────────────

void CountryService::invalidateExit(const std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = exit_cache_.begin(); it != exit_cache_.end();) {
        if (it->first.key == key) it = exit_cache_.erase(it); else ++it;
    }
}

void CountryService::noteExitIp(const std::string& key, const std::string& ip) {
    if (ip.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = exit_cache_.begin(); it != exit_cache_.end();) {
        if (it->first.key == key && !it->second.ip.empty() && it->second.ip != ip) it = exit_cache_.erase(it);
        else ++it;
    }
}

void CountryService::onNetworkGeneration(uint64_t gen) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = exit_cache_.begin(); it != exit_cache_.end();) {
        if (it->first.net != gen) it = exit_cache_.erase(it); else ++it;
    }
}

void CountryService::onGeoDbChanged() {
    std::lock_guard<std::mutex> lk(mu_);
    domain_cache_.clear();   // offline-derived; recomputed (DoH TTL permitting) on demand
}

void CountryService::clearAll() {
    std::lock_guard<std::mutex> lk(mu_);
    domain_cache_.clear();
    exit_cache_.clear();
}

size_t CountryService::domainCacheSize() const { std::lock_guard<std::mutex> lk(mu_); return domain_cache_.size(); }
size_t CountryService::exitCacheSize() const { std::lock_guard<std::mutex> lk(mu_); return exit_cache_.size(); }

// ── targeting ───────────────────────────────────────────────────────────

TargetBucket classifyForTarget(const ConfigHealthRecord& r, const CountryTarget& t, double now) {
    const bool exit_match = !t.iso.empty() && r.exit_country == t.iso;
    const bool exit_fresh = r.exit_country_at > 0 && (now - r.exit_country_at) <= t.exit_fresh_s;
    if (exit_match && exit_fresh) return TargetBucket::FreshExit;
    if (exit_match) return TargetBucket::ServerHint;                          // stale matching exit = hint
    if (r.exit_country.empty() && r.server_country == t.iso) return TargetBucket::ServerHint;
    if (r.exit_country.empty()) return TargetBucket::Unknown;
    return TargetBucket::Other;                                               // known, different exit
}

void prioritizeForTarget(std::vector<ConfigHealthRecord>& pool, int batch_size, const CountryTarget& t, double now) {
    if (t.iso.empty() || batch_size <= 0 || pool.empty()) return;
    std::vector<size_t> b[4];
    for (size_t i = 0; i < pool.size(); i++) b[(int)classifyForTarget(pool[i], t, now)].push_back(i);
    const size_t N = (size_t)batch_size;
    const double ratio = std::min(0.9, std::max(0.2, t.exploration));
    size_t explore = (size_t)std::ceil(ratio * (double)N);
    explore = std::min(explore, b[(int)TargetBucket::Unknown].size());
    const size_t exploit_quota = N > explore ? N - explore : 0;

    std::vector<size_t> order;
    order.reserve(N);
    size_t unk_used = 0;
    auto take = [&](std::vector<size_t>& src, size_t& from, size_t limit) {
        while (from < src.size() && order.size() < limit) order.push_back(src[from++]);
    };
    size_t i0 = 0, i1 = 0, i3 = 0;
    take(b[0], i0, exploit_quota);
    take(b[1], i1, exploit_quota);
    // Exploration slots (fresh matching first only up to the quota, so these are always reserved).
    take(b[2], unk_used, order.size() + explore);
    // Whatever is left: remaining matches first, then more Unknown, then known non-matching.
    take(b[0], i0, N);
    take(b[1], i1, N);
    take(b[2], unk_used, N);
    take(b[3], i3, N);

    std::vector<ConfigHealthRecord> out;
    out.reserve(order.size());
    for (size_t i : order) out.push_back(std::move(pool[i]));
    pool.swap(out);
}

bool meetsStrictExitTarget(const ConfigHealthRecord& r, const CountryTarget& t, double now) {
    if (t.iso.empty()) return true;   // no target => nothing to enforce
    return r.exit_country == t.iso && r.exit_country_at > 0 && (now - r.exit_country_at) <= t.exit_fresh_s &&
           now >= r.exit_country_at - 1.0;
}

CountryTarget CountryTargetState::get() const { std::lock_guard<std::mutex> lk(mu_); return t_; }
void CountryTargetState::set(const CountryTarget& t) { std::lock_guard<std::mutex> lk(mu_); t_ = t; }

}  // namespace geo
}  // namespace hunter
