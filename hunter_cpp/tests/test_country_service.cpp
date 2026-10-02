// Stage 4 C2 unit tests: country service (fake DoH / fake transport), cache invalidation, CAS,
// search/filter parser, country-targeted queue prioritization. No public network.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <algorithm>
#include <unistd.h>

#include "core/config.h"
#include "geo/country_query.h"
#include "geo/country_service.h"
#include "network/continuous_validator.h"
#include "fake_transport.h"
#include "test_support.h"

using namespace hunter;
using namespace hunter::geo;
using hunter::network::ConfigDatabase;

namespace {

double NOW = 1.7e9;

class FakeProvider : public network::ICountryProvider {
public:
    std::map<std::string, std::string> m;
    std::string ver = "v1";
    std::string lookupIp(const std::string& ip) const override {
        auto it = m.find(ip);
        return it == m.end() ? "" : it->second;
    }
    std::string version() const override { return ver; }
    std::string name() const override { return "fake"; }
};

class FakeDoh : public DohResolver {
public:
    std::map<std::string, DohAnswer> answers;
    int calls = 0;
    DohAnswer resolve(const std::string& host) override {
        calls++;
        auto it = answers.find(host);
        if (it == answers.end()) { DohAnswer a; a.error = "nx"; return a; }
        return it->second;
    }
};

DohAnswer ans(std::vector<std::string> ips, double ttl) {
    DohAnswer a; a.ok = true; a.ips = std::move(ips); a.ttl_s = ttl; return a;
}

std::string uuid(int i) {
    char b[64];
    std::snprintf(b, sizeof b, "%08x-2222-3333-4444-555555555555", i);
    return b;
}
std::string vless(int i, const std::string& host) {
    return "vless://" + uuid(i) + "@" + host + ":443?type=ws&path=/p#r" + std::to_string(i);
}

struct Rig {
    std::shared_ptr<FakeProvider> prov = std::make_shared<FakeProvider>();
    std::shared_ptr<FakeDoh> doh = std::make_shared<FakeDoh>();
    std::shared_ptr<fake::FakeTransport> tr = std::make_shared<fake::FakeTransport>();
    double now = NOW;
    std::unique_ptr<CountryService> svc;
    Rig(bool doh_on = true) {
        prov->m = {{"1.1.1.1", "AU"}, {"2.2.2.2", "DE"}, {"3.3.3.3", "DE"}, {"4.4.4.4", "US"}, {"93.184.216.34", "NL"}};
        CountryServiceOptions o;
        o.doh_enabled = doh_on;
        svc.reset(new CountryService(prov, doh, tr, [this] { return now; }, o));
    }
};

void testQueryParser() {
    T_CASE("names / labels");
    CHECK(countryName("DE") == "Germany", "DE");
    CHECK(countryLabel("") == "Unknown", "unknown label");
    CHECK(countryLabel("Mixed") == "Mixed", "mixed label");
    CHECK(countryLabel("IR") == "IR Iran", "IR");
    CHECK(isValidIso("US") && !isValidIso("ZZ") && !isValidIso("us"), "iso validity (no fabricated ZZ)");
    T_END();

    T_CASE("exit:/server: terms");
    auto q = parseSearchQuery("vless exit:DE server:US");
    CHECK(q.size() == 3 && q[1].kind == QueryTerm::Exit && q[1].value == "de" && q[2].kind == QueryTerm::Server, "parse");
    CHECK(matchesSearchQuery(q, "vless 1.2.3.4", CountryMode::All, "DE", "US"), "match both");
    CHECK(!matchesSearchQuery(q, "vless 1.2.3.4", CountryMode::All, "US", "DE"), "swapped does not match");
    CHECK(!matchesSearchQuery(q, "trojan", CountryMode::All, "DE", "US"), "text term must match");
    CHECK(parseSearchQuery("exit:").empty(), "dangling prefix ignored");
    T_END();

    T_CASE("names, unknown, mixed, ISO-only bare token");
    CHECK(matchesSearchQuery(parseSearchQuery("exit:germ"), "", CountryMode::All, "DE", ""), "name substring");
    CHECK(!matchesSearchQuery(parseSearchQuery("exit:ge"), "", CountryMode::All, "DE", ""), "2-letter must be ISO, not substring");
    CHECK(matchesSearchQuery(parseSearchQuery("exit:unknown"), "", CountryMode::All, "", "DE"), "exit unknown");
    CHECK(!matchesSearchQuery(parseSearchQuery("exit:unknown"), "", CountryMode::All, "DE", ""), "exit known");
    CHECK(matchesSearchQuery(parseSearchQuery("server:mixed"), "", CountryMode::All, "DE", "Mixed"), "server mixed");
    CHECK(!matchesSearchQuery(parseSearchQuery("exit:mixed"), "", CountryMode::All, "Mixed", ""), "mixed only applies to server hints");
    // "de" is a country query, not a text substring of e.g. "vless" hostnames containing "de"
    CHECK(!matchesSearchQuery(parseSearchQuery("de"), "code.example.com", CountryMode::All, "US", "US"), "bare ISO is not text");
    CHECK(matchesSearchQuery(parseSearchQuery("de"), "x", CountryMode::Exit, "DE", "US"), "bare ISO matches exit in Exit mode");
    CHECK(!matchesSearchQuery(parseSearchQuery("de"), "x", CountryMode::Server, "DE", "US"), "...but not in Server mode");
    CHECK(matchesSearchQuery(parseSearchQuery("germany"), "x", CountryMode::All, "", "DE"), "bare name matches server in All mode");
    T_END();

    T_CASE("mode/choice filter");
    CountryChoice any, de{CountryChoice::Specific, "DE"}, unk{CountryChoice::Unknown, ""}, mix{CountryChoice::Mixed, ""};
    CHECK(passesCountryFilter(CountryMode::All, any, "", ""), "any");
    CHECK(passesCountryFilter(CountryMode::All, de, "DE", "US") && passesCountryFilter(CountryMode::All, de, "US", "DE"), "All = either");
    CHECK(passesCountryFilter(CountryMode::Exit, de, "DE", "US") && !passesCountryFilter(CountryMode::Exit, de, "US", "DE"), "Exit");
    CHECK(passesCountryFilter(CountryMode::Server, de, "US", "DE") && !passesCountryFilter(CountryMode::Server, de, "DE", "US"), "Server");
    CHECK(passesCountryFilter(CountryMode::Exit, unk, "", "DE") && !passesCountryFilter(CountryMode::Exit, unk, "DE", ""), "Exit unknown");
    CHECK(passesCountryFilter(CountryMode::All, unk, "", "") && !passesCountryFilter(CountryMode::All, unk, "", "DE"), "All unknown = both unknown");
    CHECK(passesCountryFilter(CountryMode::Server, mix, "", "Mixed") && !passesCountryFilter(CountryMode::Exit, mix, "Mixed", ""), "mixed");
    CHECK(!passesCountryFilter(CountryMode::Server, de, "", "Mixed"), "Mixed is not DE");
    T_END();
}

void testServerCountry() {
    T_CASE("literal IPs: offline lookup, private/unmapped -> Unknown, no DoH needed");
    Rig r(false);
    CHECK(r.svc->resolveServer("1.1.1.1").country == "AU", "AU");
    CHECK(r.svc->resolveServer("1.1.1.1").source == kSrcOfflineIp, "source");
    CHECK(r.svc->resolveServer("[1.1.1.1]").country == "AU", "brackets");
    CHECK(r.svc->resolveServer("192.168.1.1").country.empty(), "private");
    CHECK(r.svc->resolveServer("9.9.9.9").country.empty(), "unmapped");
    CHECK(r.doh->calls == 0, "DoH never used for literals");
    T_END();

    T_CASE("domain with DoH disabled -> Unknown, never local DNS");
    auto s = r.svc->resolveServer("example.com");
    CHECK(s.country.empty() && s.source == kSrcNone, "unknown");
    CHECK(r.doh->calls == 0, "no resolver call");
    T_END();

    T_CASE("DoH: agree / Mixed / failure / private-only");
    Rig d(true);
    d.doh->answers["a.example"] = ans({"2.2.2.2", "3.3.3.3"}, 300);
    d.doh->answers["m.example"] = ans({"2.2.2.2", "4.4.4.4"}, 300);
    d.doh->answers["p.example"] = ans({"10.0.0.1"}, 300);
    d.doh->answers["h.example"] = ans({"2.2.2.2", "10.0.0.1"}, 300);   // private ignored => DE
    CHECK(d.svc->resolveServer("a.example").country == "DE", "agree => DE");
    CHECK(d.svc->resolveServer("a.example").source == kSrcDohOffline, "source doh");
    CHECK(d.svc->resolveServer("m.example").country == "Mixed", "disagree => Mixed");
    CHECK(d.svc->resolveServer("m.example").ips.size() == 2, "ip set kept");
    CHECK(d.svc->resolveServer("p.example").country.empty(), "only private => Unknown");
    CHECK(d.svc->resolveServer("h.example").country == "DE", "usable IPs agree");
    CHECK(d.svc->resolveServer("nx.example").country.empty(), "DNS failure => Unknown");
    T_END();

    T_CASE("TTL: cap 1h, cache hit, negative cache, version change recomputes without DoH");
    Rig t(true);
    t.doh->answers["long.example"] = ans({"2.2.2.2"}, 999999);   // TTL way above the 1 h cap
    t.svc->resolveServer("long.example");
    CHECK(t.doh->calls == 1, "first resolve");
    t.svc->resolveServer("long.example");
    CHECK(t.doh->calls == 1, "cached");
    t.now += 3601;
    auto again = t.svc->resolveServer("long.example");
    CHECK(t.doh->calls == 2 && !again.from_cache, "expired after <= 1 h");
    t.svc->resolveServer("gone.example");
    int c = t.doh->calls;
    t.svc->resolveServer("gone.example");
    CHECK(t.doh->calls == c, "negative result cached briefly");
    t.now += 61;
    t.svc->resolveServer("gone.example");
    CHECK(t.doh->calls == c + 1, "negative result retried after 60 s");
    t.prov->m["2.2.2.2"] = "FR";
    t.prov->ver = "v2";
    auto up = t.svc->resolveServer("long.example");
    CHECK(up.country == "FR" && up.from_cache, "snapshot change recomputes from stored IPs");
    t.svc->onGeoDbChanged();
    CHECK(t.svc->domainCacheSize() == 0, "explicit invalidation");
    T_END();

    T_CASE("DoH JSON parser (strict)");
    auto ok = parseDohJson("{\"Status\":0,\"Answer\":[{\"name\":\"x\",\"type\":5,\"TTL\":10,\"data\":\"y.\"},"
                           "{\"name\":\"y\",\"type\":1,\"TTL\":120,\"data\":\"1.1.1.1\"},"
                           "{\"name\":\"y\",\"type\":1,\"TTL\":60,\"data\":\"evil;rm\"}]}");
    CHECK(ok.ok && ok.ips.size() == 1 && ok.ips[0] == "1.1.1.1" && ok.ttl_s == 120, "A record only, CNAME/garbage skipped");
    CHECK(!parseDohJson("{\"Status\":3}").ok, "NXDOMAIN");
    CHECK(!parseDohJson("<html>captive</html>").ok, "html");
    CHECK(!parseDohJson("").ok, "empty");
    T_END();

    T_CASE("real DoH adapter uses fake transport, IP-literal URL, verified TLS, never a tunnel");
    auto ft = std::make_shared<fake::FakeTransport>();
    ft->handler = [](const network::TransportRequest& q) {
        network::TransportResponse r; r.status = 200;
        r.body = q.url.find("type=AAAA") != std::string::npos ? "{\"Status\":0}"
                 : "{\"Status\":0,\"Answer\":[{\"type\":1,\"TTL\":77,\"data\":\"4.4.4.4\"}]}";
        return r;
    };
    auto dr = makeCloudflareDohResolver(ft);
    auto a = dr->resolve("host.example");
    CHECK(a.ok && a.ips.size() == 1 && a.ttl_s == 77, "answer");
    CHECK(ft->calls.size() == 2 && ft->calls[0].url.rfind("https://1.1.1.1/", 0) == 0 && ft->calls[0].proxy_port == 0 &&
          ft->calls[0].tls_verify, "direct, IP-literal DoH host, TLS verify");
    CHECK(!dr->resolve("bad host;x").ok, "invalid name rejected");
    T_END();
}

void testPersistenceAndCas() {
    T_CASE("refreshServerCountries persists, is idempotent, and re-runs on snapshot change");
    Rig r(true);
    r.doh->answers["dom.example"] = ans({"2.2.2.2", "4.4.4.4"}, 600);
    ConfigDatabase db;
    db.setClock([&] { return r.now; });
    std::string u1 = vless(1, "1.1.1.1"), u2 = vless(2, "dom.example"), u3 = vless(3, "192.168.0.9");
    db.addConfigs({u1, u2, u3}, "t");
    CHECK(r.svc->refreshServerCountries(db, 100) == 3, "3 written");
    ConfigHealthRecord a, b, c;
    db.getRecord(u1, &a); db.getRecord(u2, &b); db.getRecord(u3, &c);
    CHECK(a.server_country == "AU" && a.server_country_source == kSrcOfflineIp && a.geo_db_version == "v1", "literal");
    CHECK(a.server_ips.size() == 1 && a.server_ips[0] == "1.1.1.1", "ip set stored");
    CHECK(b.server_country == "Mixed" && b.server_country_source == kSrcDohOffline && b.server_ips.size() == 2, "Mixed");
    CHECK(c.server_country.empty() && c.server_country_at > 0, "private => Unknown but marked attempted");
    CHECK(r.svc->refreshServerCountries(db, 100) == 0, "nothing to do the second time");
    r.prov->ver = "v2";
    r.prov->m["1.1.1.1"] = "NZ";
    r.svc->onGeoDbChanged();
    r.now += 1;
    CHECK(r.svc->refreshServerCountries(db, 100) == 3, "snapshot change re-evaluates");
    db.getRecord(u1, &a);
    CHECK(a.server_country == "NZ" && a.geo_db_version == "v2", "new snapshot value");
    CHECK(r.svc->refreshServerCountries(db, 1) == 0, "budget respected when nothing is due");
    T_END();

    T_CASE("timestamp CAS: older results never overwrite newer ones");
    ConfigDatabase db2;
    std::string u = vless(9, "1.1.1.1");
    db2.addConfigs({u}, "t");
    std::string key = ConfigDatabase::keyFor(u);
    ConfigDatabase::CountryUpdate e;
    e.is_exit = true; e.country = "DE"; e.ip = "2.2.2.2"; e.source = kSrcTrace; e.at = 1000; e.network_generation = 5;
    CHECK(db2.applyCountryResult(key, e), "first");
    e.country = "US"; e.at = 999;
    CHECK(!db2.applyCountryResult(key, e), "older timestamp rejected");
    e.at = 1001; e.network_generation = 4;
    CHECK(!db2.applyCountryResult(key, e), "older network generation rejected");
    e.network_generation = 5; e.at = 1002;
    CHECK(db2.applyCountryResult(key, e), "newer accepted");
    ConfigHealthRecord rec; db2.getRecord(u, &rec);
    CHECK(rec.exit_country == "US" && rec.exit_country_at == 1002, "value persisted");
    ConfigDatabase::CountryUpdate s;
    s.is_exit = false; s.country = "AU"; s.at = 50;
    CHECK(db2.applyCountryResult(key, s), "server first");
    s.country = "NZ"; s.at = 49;
    CHECK(!db2.applyCountryResult(key, s), "server older rejected");
    CHECK(!db2.applyCountryResult("ek1:nope", s), "unknown endpoint");
    // survives save/load (V4 columns)
    char tmpl[] = "/tmp/c2dbXXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string path = dir + "/db.tsv";
    CHECK(db2.saveToDisk(path) == 1, "saved");
    ConfigDatabase db3;
    db3.loadFromDisk(path);
    ConfigHealthRecord back; CHECK(db3.getRecord(u, &back), "loaded");
    CHECK(back.exit_country == "US" && back.server_country == "AU" && back.exit_country_source == kSrcTrace, "country columns roundtrip");
    std::remove(path.c_str());
    T_END();
}

void testExit() {
    T_CASE("exit from probe result is cached; invalid/non-pass ignored");
    Rig r;
    ProbeResult p; p.endpoint_key = "k1"; p.outcome = ProbeOutcome::Pass; p.exit_country = "NL"; p.exit_ip = "93.184.216.34";
    p.finished_at = r.now;
    CHECK(r.svc->onProbeResult(p, 1, 1), "recorded");
    CHECK(r.svc->exitFresh("k1", 1, 1), "fresh");
    CHECK(!r.svc->exitFresh("k1", 2, 1), "other network generation");
    CHECK(!r.svc->exitFresh("k1", 1, 2), "other engine/profile generation");
    ProbeResult bad = p; bad.exit_country = "ZZ"; bad.endpoint_key = "k2";
    CHECK(!r.svc->onProbeResult(bad, 1, 1), "unmapped code is Unknown, not cached");
    ProbeResult fail = p; fail.outcome = ProbeOutcome::RemoteFailure; fail.endpoint_key = "k3";
    CHECK(!r.svc->onProbeResult(fail, 1, 1), "failed round");
    r.now += 1801;
    CHECK(!r.svc->exitFresh("k1", 1, 1), "stale after 30 min");
    T_END();

    T_CASE("invalidation: IP change, activation, network generation");
    Rig i;
    ProbeResult q; q.endpoint_key = "kk"; q.outcome = ProbeOutcome::Pass; q.exit_country = "NL"; q.exit_ip = "93.184.216.34"; q.finished_at = i.now;
    i.svc->onProbeResult(q, 1, 1);
    i.svc->noteExitIp("kk", "93.184.216.34");
    CHECK(i.svc->exitFresh("kk", 1, 1), "same IP keeps entry");
    i.svc->noteExitIp("kk", "8.8.4.4");
    CHECK(!i.svc->exitFresh("kk", 1, 1), "IP change drops entry");
    i.svc->onProbeResult(q, 1, 1);
    i.svc->invalidateExit("kk");
    CHECK(i.svc->exitCacheSize() == 0, "activation/cutover drops entry");
    i.svc->onProbeResult(q, 1, 1);
    i.svc->onNetworkGeneration(2);
    CHECK(i.svc->exitCacheSize() == 0, "network change drops all");
    T_END();

    T_CASE("fallback chain: primary -> one.one.one.one -> ipify+offline; tagged; failure keeps stale");
    Rig f;
    ConfigDatabase db;
    db.setClock([&] { return f.now; });
    std::string uri = vless(7, "1.1.1.1");
    db.addConfigs({uri}, "t");
    std::string key = ConfigDatabase::keyFor(uri);
    ConfigDatabase::CountryUpdate old;
    old.is_exit = true; old.country = "FR"; old.ip = "5.5.5.5"; old.source = kSrcTrace; old.at = f.now - 7200; old.network_generation = 1;
    db.applyCountryResult(key, old);

    int mode = 0;   // 0: primary ok, 1: only 1.1.1.1 ok, 2: only ipify ok, 3: all fail, 4: bad bodies
    f.tr->handler = [&](const network::TransportRequest& q) {
        const bool primary = q.url.find("www.cloudflare.com") != std::string::npos;
        const bool one = q.url.find("one.one.one.one") != std::string::npos;
        const bool ipify = q.url.find("ipify") != std::string::npos;
        if (mode == 4) {
            if (primary) return fake::okB("<html>captive portal login page " + std::string(80, 'x') + "</html>");
            if (one) return fake::okB(fake::trace("93.184.216.34", "NL", "www.cloudflare.com"));   // wrong h=
            network::TransportResponse r; r.status = 200; r.body = "{\"ip\":\"10.0.0.1\"}"; return r;   // private ip
        }
        if (primary && mode == 0) return fake::okB(fake::trace("93.184.216.34", "NL", "www.cloudflare.com"));
        if (one && mode == 1) return fake::okB(fake::trace("93.184.216.34", "NL", "one.one.one.one"));
        if (ipify && mode == 2) { network::TransportResponse r; r.status = 200; r.body = "{\"ip\":\"2.2.2.2\"}"; return r; }
        return fake::err(network::TransportError::Timeout);
    };

    mode = 3;
    auto fl = f.svc->refreshExit(&db, key, 4000, 1, 1, true, true);
    CHECK(!fl.ok, "all providers fail");
    ConfigHealthRecord rec; db.getRecord(uri, &rec);
    CHECK(rec.exit_country == "FR" && rec.exit_country_source == kSrcTrace, "stale value retained, not replaced by a guess");
    mode = 4;
    CHECK(!f.svc->refreshExit(&db, key, 4000, 1, 1, true, true).ok, "captive page / wrong host / private ipify rejected");
    db.getRecord(uri, &rec);
    CHECK(rec.exit_country == "FR", "still retained");

    mode = 0;
    auto p0 = f.svc->refreshExit(&db, key, 4000, 1, 1, true, true);
    CHECK(p0.ok && p0.country == "NL" && p0.source == kSrcTrace, "primary");
    db.getRecord(uri, &rec);
    CHECK(rec.exit_country == "NL" && rec.exit_ip == "93.184.216.34" && rec.exit_country_source == kSrcTrace, "persisted");
    size_t calls = f.tr->calls.size();
    auto cached = f.svc->refreshExit(&db, key, 4000, 1, 1, true, false);
    CHECK(cached.from_cache && f.tr->calls.size() == calls, "fresh cache avoids network");

    f.svc->invalidateExit(key);
    f.now += 10;
    mode = 1;
    auto p1 = f.svc->refreshExit(&db, key, 4000, 1, 1, true, true);
    CHECK(p1.ok && p1.source == kSrcTraceFallback, "one.one.one.one fallback tagged");
    f.svc->invalidateExit(key);
    f.now += 10;
    mode = 2;
    auto p2 = f.svc->refreshExit(&db, key, 4000, 1, 1, false, true);
    CHECK(p2.ok && p2.country == "DE" && p2.source == kSrcOfflineExit && p2.ip == "2.2.2.2", "ipify + offline, lower-confidence tag");
    db.getRecord(uri, &rec);
    CHECK(rec.exit_country == "DE" && rec.exit_country_source == kSrcOfflineExit, "persisted with tag");
    // tunnel requests carry the leased port, TLS verification, never direct
    bool all_proxied = true;
    for (auto& c : f.tr->calls) if (c.proxy_port != 4000 || !c.tls_verify) all_proxied = false;
    CHECK(all_proxied, "all exit lookups go through the tunnel port with TLS verify");
    // CAS: a late result with an older timestamp must not win
    ConfigDatabase::CountryUpdate late; late.is_exit = true; late.country = "US"; late.at = f.now - 100; late.network_generation = 1;
    CHECK(!db.applyCountryResult(key, late), "late stale result rejected");
    CHECK(!f.svc->refreshExit(&db, key, 0, 1, 1, true, true).ok, "no port => no lookup");
    T_END();
}

ConfigHealthRecord rec(int i, const std::string& exit_c, double exit_at, const std::string& server_c) {
    ConfigHealthRecord r;
    r.uri = vless(i, "h" + std::to_string(i) + ".example");
    r.endpoint_key = "ek1:" + std::to_string(i);
    r.exit_country = exit_c; r.exit_country_at = exit_at; r.server_country = server_c;
    return r;
}

void testTargeting() {
    CountryTarget t; t.iso = "DE"; t.exit_fresh_s = 1800; t.exploration = 0.2;
    const double now = 1e6;

    T_CASE("classification");
    CHECK(classifyForTarget(rec(1, "DE", now - 10, ""), t, now) == TargetBucket::FreshExit, "fresh exit");
    CHECK(classifyForTarget(rec(2, "DE", now - 5000, ""), t, now) == TargetBucket::ServerHint, "stale matching exit = hint");
    CHECK(classifyForTarget(rec(3, "", 0, "DE"), t, now) == TargetBucket::ServerHint, "server hint");
    CHECK(classifyForTarget(rec(4, "", 0, "US"), t, now) == TargetBucket::Unknown, "unknown exit");
    CHECK(classifyForTarget(rec(5, "", 0, ""), t, now) == TargetBucket::Unknown, "all unknown");
    CHECK(classifyForTarget(rec(6, "US", now - 10, "DE"), t, now) == TargetBucket::Other, "known different exit beats server hint");
    T_END();

    T_CASE("priority order: fresh exits > server hints > Unknown; known others last");
    std::vector<ConfigHealthRecord> pool;
    int id = 0;
    for (int i = 0; i < 3; i++) pool.push_back(rec(++id, "US", now - 1, "US"));        // Other
    for (int i = 0; i < 3; i++) pool.push_back(rec(++id, "", 0, ""));                   // Unknown
    for (int i = 0; i < 2; i++) pool.push_back(rec(++id, "", 0, "DE"));                 // hint
    for (int i = 0; i < 2; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));         // fresh
    prioritizeForTarget(pool, 10, t, now);
    CHECK(pool.size() == 10, "all kept (pool 10, batch 10)");
    CHECK(classifyForTarget(pool[0], t, now) == TargetBucket::FreshExit && classifyForTarget(pool[1], t, now) == TargetBucket::FreshExit, "fresh first");
    CHECK(classifyForTarget(pool[2], t, now) == TargetBucket::ServerHint && classifyForTarget(pool[3], t, now) == TargetBucket::ServerHint, "hints next");
    CHECK(classifyForTarget(pool[4], t, now) == TargetBucket::Unknown, "then Unknown");
    CHECK(classifyForTarget(pool[9], t, now) == TargetBucket::Other, "known others last");
    T_END();

    T_CASE("exploration >= 20% even when matching candidates would fill the batch");
    pool.clear();
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "", 0, ""));
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "US", now - 5, "US"));
    prioritizeForTarget(pool, 50, t, now);
    int fresh = 0, unk = 0, other = 0;
    for (auto& r : pool) {
        auto b = classifyForTarget(r, t, now);
        if (b == TargetBucket::FreshExit) fresh++; else if (b == TargetBucket::Unknown) unk++; else other++;
    }
    CHECK(pool.size() == 50 && unk == 10 && fresh == 40 && other == 0, "exactly ceil(0.2*50)=10 exploratory, 40 matching");
    // higher requested ratio honored; lower than 20% is clamped up
    pool.clear();
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "", 0, ""));
    CountryTarget low = t; low.exploration = 0.0;
    prioritizeForTarget(pool, 50, low, now);
    unk = 0; for (auto& r : pool) if (classifyForTarget(r, t, now) == TargetBucket::Unknown) unk++;
    CHECK(unk == 10, "ratio clamped to >= 0.2");
    pool.clear();
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));
    for (int i = 0; i < 100; i++) pool.push_back(rec(++id, "", 0, ""));
    CountryTarget hi = t; hi.exploration = 0.5;
    prioritizeForTarget(pool, 50, hi, now);
    unk = 0; for (auto& r : pool) if (classifyForTarget(r, t, now) == TargetBucket::Unknown) unk++;
    CHECK(unk == 25, "ratio 0.5");
    T_END();

    T_CASE("not enough matches => more exploration; no Unknown => matches only; no target => untouched");
    pool.clear();
    for (int i = 0; i < 3; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));
    for (int i = 0; i < 50; i++) pool.push_back(rec(++id, "", 0, ""));
    for (int i = 0; i < 50; i++) pool.push_back(rec(++id, "US", now - 5, "US"));
    prioritizeForTarget(pool, 20, t, now);
    unk = 0; other = 0; for (auto& r : pool) { auto b = classifyForTarget(r, t, now); if (b == TargetBucket::Unknown) unk++; if (b == TargetBucket::Other) other++; }
    CHECK(pool.size() == 20 && unk == 17 && other == 0, "3 matches + 17 unknown, known-other starved");
    pool.clear();
    for (int i = 0; i < 30; i++) pool.push_back(rec(++id, "DE", now - 5, "DE"));
    prioritizeForTarget(pool, 10, t, now);
    CHECK(pool.size() == 10, "no Unknown available");
    auto copy = pool; CountryTarget none;
    prioritizeForTarget(pool, 5, none, now);
    CHECK(pool.size() == copy.size(), "no target: untouched");
    T_END();

    T_CASE("strict exit target needs a fresh MEASURED exit; server hints never qualify");
    CountryTarget st = t; st.strict = true;
    CHECK(meetsStrictExitTarget(rec(1, "DE", now - 10, "US"), st, now), "fresh measured");
    CHECK(!meetsStrictExitTarget(rec(2, "DE", now - 4000, "DE"), st, now), "stale exit is unconfirmed");
    CHECK(!meetsStrictExitTarget(rec(3, "", 0, "DE"), st, now), "server hint is not an exit");
    CHECK(!meetsStrictExitTarget(rec(4, "US", now - 1, "DE"), st, now), "other exit");
    CHECK(meetsStrictExitTarget(rec(5, "", 0, ""), CountryTarget(), now), "no target => no constraint");
    T_END();

    T_CASE("ConfigDatabase::getUntestedBatch honors the prioritizer hook");
    ConfigDatabase db;
    db.setClock([&] { return now; });
    std::set<std::string> uris;
    for (int i = 0; i < 60; i++) uris.insert(vless(1000 + i, "10." + std::to_string(i) + ".0.1"));
    db.addConfigs(uris, "t");
    // Mark 5 of them as fresh DE exits, 5 as US.
    int n = 0;
    for (auto& u : uris) {
        ConfigDatabase::CountryUpdate cu; cu.is_exit = true; cu.at = now - 5; cu.network_generation = 1; cu.source = kSrcTrace;
        if (n < 5) { cu.country = "DE"; db.applyCountryResult(ConfigDatabase::keyFor(u), cu); }
        else if (n < 40) { cu.country = "US"; db.applyCountryResult(ConfigDatabase::keyFor(u), cu); }
        n++;
    }
    CountryTargetState state;
    CountryTarget cur; cur.iso = "DE"; cur.exit_fresh_s = 1800; state.set(cur);
    db.setBatchPrioritizer([&](std::vector<ConfigHealthRecord>& p, int b) { prioritizeForTarget(p, b, state.get(), now); });
    auto batch = db.getUntestedBatch(10);
    int de = 0, un = 0, us = 0;
    for (auto& r : batch) { if (r.exit_country == "DE") de++; else if (r.exit_country.empty()) un++; else us++; }
    CHECK(batch.size() == 10 && de == 5 && un == 5 && us == 0, "5 matching + 5 exploratory Unknown, no known-other");
    db.setBatchPrioritizer(nullptr);
    CHECK(db.getUntestedBatch(10).size() == 10, "hook removable");
    T_END();
}

void testConfigAndRegistry() {
    T_CASE("config: target/strict persisted and validated; defaults");
    HunterConfig c;
    CHECK(c.exitCountryTarget().empty() && !c.exitCountryStrict() && !c.countryDohEnabled() && c.directBaselineEnabled(), "defaults");
    CHECK(c.countryExplorationRatio() >= 0.2f, "ratio floor");
    CHECK(c.setExitCountryTarget(" de "), "normalized");
    CHECK(c.exitCountryTarget() == "DE", "upper-cased");
    CHECK(!c.setExitCountryTarget("Germany") && c.exitCountryTarget() == "DE", "invalid rejected, value kept");
    c.setExitCountryStrict(true);
    c.set("direct_baseline_enabled", false);
    c.set("country_exploration_ratio", 0.05f);
    CHECK(c.countryExplorationRatio() >= 0.2f, "ratio < 0.2 clamped");
    char tmpl[] = "/tmp/c2cfgXXXXXX";
    std::string dir = mkdtemp(tmpl);
    std::string path = dir + "/hunter_config.json";
    CHECK(c.saveToFile(path), "save");
    HunterConfig d;
    CHECK(d.loadFromFile(path), "load");
    CHECK(d.exitCountryTarget() == "DE" && d.exitCountryStrict() && !d.directBaselineEnabled(), "round-trip");
    CHECK(c.setExitCountryTarget(""), "clear");
    CHECK(c.exitCountryTarget().empty(), "cleared");
    std::remove(path.c_str());
    T_END();

    T_CASE("provider registry: null default, explicit registration, no work at creation");
    auto& reg = network::CountryProviderRegistry::instance();
    CHECK(reg.create("")->lookupIp("1.1.1.1").empty(), "null provider by default");
    CHECK(reg.create("does-not-exist")->version().empty(), "unknown name => null provider, never nullptr");
    network::registerBuiltinCountryProviders();
    network::registerBuiltinCountryProviders();   // idempotent
    auto names = reg.names();
    CHECK(std::find(names.begin(), names.end(), "offline") != names.end(), "offline registered");
    auto off = reg.create("offline");
    CHECK(off && off->name() == "offline", "offline provider");
    CHECK(off->lookupIp("8.8.8.8").size() <= 2, "safe without an embedded DB (Unknown)");
    reg.registerFactory("fake", [] { return std::make_shared<FakeProvider>(); });
    reg.setDefault("fake");
    CHECK(reg.create("")->name() == "fake", "default switch");
    reg.setDefault("null");
    T_END();
}

}  // namespace

int main() {
    std::cout << "test_country_service\n";
    testQueryParser();
    testServerCountry();
    testPersistenceAndCas();
    testExit();
    testTargeting();
    testConfigAndRegistry();
    return T_SUMMARY();
}
