#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <atomic>
#include <thread>
#include <unistd.h>

#include "core/db_format.h"
#include "core/endpoint_key.h"
#include "network/continuous_validator.h"
#include "test_support.h"

using namespace hunter;
using hunter::network::ConfigDatabase;

static std::string TMP;
static double NOW = 1.7e9;
static std::string P(const std::string& n) { return TMP + "/" + n; }
static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
static void spit(const std::string& p, const std::string& d) {
    std::ofstream f(p, std::ios::binary);
    f << d;
}
static bool exists(const std::string& p) { return std::ifstream(p).good(); }
static std::string uuid(int i) {
    char b[64];
    std::snprintf(b, sizeof b, "%08x-2222-3333-4444-555555555555", i);
    return b;
}
static std::string vless(int i, const char* host = "1.2.3.4", int port = 443) {
    return "vless://" + uuid(i) + "@" + host + ":" + std::to_string(port) + "?type=ws&path=/p#r" + std::to_string(i);
}
static void setup(ConfigDatabase& db) {
    db.setClock([]() { return NOW; });
}
static ProbeResult passFor(const std::string& uri, double t, double lat = 120.0, bool bulk = true) {
    ProbeResult r;
    r.endpoint_key = ConfigDatabase::keyFor(uri);
    r.outcome = ProbeOutcome::Pass;
    r.started_at = r.finished_at = t;
    r.latency_ms = lat;
    r.bulk_passed = bulk;
    return r;
}
static ProbeResult failFor(const std::string& uri, double t) {
    ProbeResult r;
    r.endpoint_key = ConfigDatabase::keyFor(uri);
    r.outcome = ProbeOutcome::RemoteFailure;
    r.attributable = true;
    r.started_at = r.finished_at = t;
    return r;
}
// V3 legacy file: 14 columns
static std::string v3Row(const std::string& uri, const std::string& tag, double first, int alive, double lat,
                         int tests, int passes, int fails = 0) {
    std::ostringstream o;
    o << std::fixed << uri << '\t' << tag << "\txray\t" << first << '\t' << first + 5 << '\t' << first + 5 << '\t' << alive
      << "\t0\t" << lat << '\t' << fails << '\t' << tests << '\t' << passes << "\t1\t" << first + 9 << '\n';
    return o.str();
}

int main() {
    char tmpl[] = "/tmp/hunter_a1_XXXXXX";
    TMP = mkdtemp(tmpl);
    std::cout << "=== ConfigDatabase V4 ===  (tmp " << TMP << ")\n";

    T_CASE("configs sharing an IP are separate records");
    {
        ConfigDatabase db;
        setup(db);
        std::set<std::string> u = {vless(1), vless(2), vless(3, "1.2.3.4", 8443), "trojan://pw@1.2.3.4:443"};
        CHECK(db.addConfigs(u) == 4, "4 distinct endpoints on one IP");
        CHECK(db.addConfigs({"vless://" + uuid(1) + "@1.2.3.4:443?type=ws&path=/p#another-remark"}) == 0, "remark variant is the same endpoint");
        CHECK(db.size() == 4, "size");
        ConfigHealthRecord r;
        CHECK(db.getRecord(vless(1), &r) && r.endpoint_key == ConfigDatabase::keyFor(vless(1)) && r.uri_hash == r.endpoint_key, "record key");
        CHECK(r.ev.protocol == "vless" && !r.ev.unsupported, "protocol attrs");
        CHECK(r.ev.state == HealthState::Unknown, "starts Unknown");
    }
    T_END();

    T_CASE("applyProbeResult: typed evidence, legacy fields derived");
    {
        ConfigDatabase db;
        setup(db);
        db.addConfigs({vless(1)});
        auto u = vless(1);
        CHECK(db.applyProbeResult(passFor(u, NOW - 100)) == ApplyEffect::Applied, "applied");
        ConfigHealthRecord r;
        db.getRecord(u, &r);
        CHECK(r.alive && r.ev.state == HealthState::Healthy && r.total_tests == 1 && r.total_passes == 1, "healthy");
        CHECK(!r.needs_retest && r.latency_ms == 120.0f && r.last_alive_time == NOW - 100, "legacy derived");
        CHECK(db.applyProbeResult(failFor(u, NOW - 90)) == ApplyEffect::Applied, "failure");
        db.getRecord(u, &r);
        CHECK(r.alive && r.ev.state == HealthState::Degraded && r.consecutive_fails == 1, "degraded still alive");
        db.applyProbeResult(failFor(u, NOW - 80));
        db.getRecord(u, &r);
        CHECK(!r.alive && r.ev.state == HealthState::Unstable && r.consecutive_fails == 2, "unstable not alive");
        ProbeResult local = failFor(u, NOW - 70);
        local.outcome = ProbeOutcome::LocalNetworkDown;
        CHECK(db.applyProbeResult(local) == ApplyEffect::Excluded, "excluded");
        db.getRecord(u, &r);
        CHECK(r.total_tests == 3 && r.consecutive_fails == 2, "excluded changes no counters");
        ProbeResult unk = passFor("vless://x@9.9.9.9:1", NOW);
        CHECK(db.applyProbeResult(unk) == ApplyEffect::UnknownEndpoint, "unknown endpoint");
        ProbeResult dup = passFor(u, NOW - 60);
        dup.run_id = "run-1";
        CHECK(db.applyProbeResult(dup) == ApplyEffect::Applied, "run-1");
        dup.finished_at = NOW - 55;
        CHECK(db.applyProbeResult(dup) == ApplyEffect::Duplicate, "duplicate discarded");
        ProbeResult exitp = passFor(u, NOW - 50);
        exitp.exit_country = "DE";
        exitp.exit_ip = "5.6.7.8";
        db.applyProbeResult(exitp);
        db.getRecord(u, &r);
        CHECK(r.exit_country == "DE" && r.exit_ip == "5.6.7.8" && r.exit_country_at == NOW - 50, "exit country recorded");
    }
    T_END();

    T_CASE("fake-clock Stable via database and ranking through the shared evaluator");
    {
        ConfigDatabase db;
        setup(db);
        std::string a = vless(1), b = vless(2), c = vless(3), d = vless(4);
        db.addConfigs({a, b, c, d});
        for (int i = 0; i < 7; i++) db.applyProbeResult(passFor(a, NOW - 400 + 60 * i, 150.0));       // stable, p90 150
        for (int i = 0; i < 7; i++) db.applyProbeResult(passFor(b, NOW - 400 + 60 * i, 900.0));       // stable, slower
        db.applyProbeResult(passFor(c, NOW - 5, 50.0, false));                                         // healthy, not stable
        // d untested
        auto rec = db.getRecommendedRecords();
        CHECK(rec.size() == 2 && rec[0].uri == a && rec[1].uri == b, "recommended = Stable only, ranked by score");
        auto healthy = db.getHealthyRecords();
        CHECK(healthy.size() == 3 && healthy[0].uri == a && healthy[1].uri == b && healthy[2].uri == c,
              "stable ahead of provisional despite c's lower latency");
        auto alive = db.getAliveRecords();
        CHECK(alive.size() == 3 && alive[0].uri == a, "alive list ranked");
        auto all = db.getAllRecords();
        CHECK(all.size() == 4 && all[3].uri == d, "unrated last");
        ConfigHealthRecord r;
        db.getRecord(a, &r);
        CHECK(db.evaluate(r).stability == Stability::Stable, "evaluate Stable");
        NOW += 200;  // certification goes stale (>180 s)
        CHECK(db.getRecommendedRecords().empty(), "stale certification is not recommended");
        NOW -= 200;
    }
    T_END();

    T_CASE("legacy updateHealth adapter behaves sanely");
    {
        ConfigDatabase db;
        setup(db);
        std::string u = "vless://test@host:443";
        db.addConfigs({u});
        db.updateHealth(u, true, 150.0f, "xray");
        auto s = db.getStats();
        CHECK(s.alive == 1 && s.total_tested == 1 && s.total_passed == 1, "pass");
        NOW += 1;
        db.updateHealth(u, false, 0.0f);
        CHECK(db.getStats().alive == 1, "one failure keeps it alive (Degraded)");
        NOW += 1;
        db.updateHealth(u, false, 0.0f);
        s = db.getStats();
        CHECK(s.alive == 0 && s.total_tested == 3 && s.total_passed == 1, "two failures -> not alive");
        NOW += 1;
        db.updateHealth(u, true, 90.0f);
        CHECK(db.getStats().alive == 1, "recovers to alive");
        db.updateHealth(u, false, 0.0f, "", true);
        ConfigHealthRecord r;
        db.getRecord(u, &r);
        CHECK(!r.alive && r.ev.state != HealthState::Dead && r.legacy_fails >= 3, "force_dead: legacy hint only, no fabricated evidence");
        std::string why;
        CHECK(evidenceConsistent(r.ev, db.thresholds(), &why), why);
        db.saveToDisk(P("fd.tsv"));
        ConfigDatabase fd; setup(fd);
        CHECK(fd.loadFromDisk(P("fd.tsv")) == db.size(), "forced-dead record survives save/reload");
        std::string tg = "vless://tg@host:444";
        db.addConfigs({tg});
        db.updateHealth(tg, true, 300.0f, "", false, true);
        db.getRecord(tg, &r);
        CHECK(r.alive && r.telegram_only && r.ev.eligible_count == 0 && db.evaluate(r).score == 0.0, "telegram-only is a capability hint, score 0");
        CHECK(db.getTelegramOnlyRecords().size() == 1 && db.getHealthyRecords().empty(), "lists");
        db.updateHealth("vless://never@added:1", true, 1.0f);
        CHECK(db.size() == 2, "unknown uri ignored");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("eviction: Dead retained >= 72 h; local outage cannot mass-delete");
    {
        ConfigDatabase db(1000);
        setup(db);
        std::string a = vless(1), b = vless(2);
        db.addConfigs({a, b});
        db.applyProbeResult(passFor(a, NOW - 1000));
        db.applyProbeResult(passFor(b, NOW - 1000));
        for (double t : {0.0, 15.0, 30.0}) db.applyProbeResult(failFor(a, NOW - 900 + t));
        ProbeResult lo = failFor(b, NOW - 100);
        lo.outcome = ProbeOutcome::LocalNetworkDown;
        for (int i = 0; i < 5; i++) { lo.finished_at += 1; db.applyProbeResult(lo); }
        CHECK(db.evictDead() == 0, "nothing evicted inside retention");
        NOW += 71 * 3600;
        CHECK(db.evictDead() == 0, "71 h: retained");
        NOW += 2 * 3600;
        CHECK(db.evictDead() == 1, "73 h: Dead evicted");
        ConfigHealthRecord r;
        CHECK(!db.getRecord(a, &r) && db.getRecord(b, &r), "only the Dead record went");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("V4 save/load roundtrip preserves evidence, countries and escaping");
    {
        ConfigDatabase db;
        setup(db);
        std::string a = vless(1), b = "hy2://Pw@host.example:443?insecure=1#x";
        db.addConfigs({a, b}, "tag\twith\ttabs\\and\nnewline");
        for (int i = 0; i < 8; i++) {
            ProbeResult r = passFor(a, NOW - 500 + 60 * i, 100.0 + i);
            r.run_id = "run\"" + std::to_string(i);
            db.applyProbeResult(r);
        }
        db.applyProbeResult(failFor(b, NOW - 10));
        ConfigDatabase::CountryUpdate cu;
        cu.country = "NL"; cu.at = NOW - 5; cu.source = "offline"; cu.geo_db_version = "2026-09";
        cu.server_ips = {"1.2.3.4", "2001:db8::1"};
        CHECK(db.applyCountryResult(ConfigDatabase::keyFor(a), cu), "server country");
        ConfigDatabase::CountryUpdate eu;
        eu.is_exit = true; eu.country = "DE"; eu.ip = "5.6.7.8"; eu.source = "cloudflare_trace"; eu.at = NOW - 4; eu.network_generation = 3;
        CHECK(db.applyCountryResult(ConfigDatabase::keyFor(a), eu), "exit country");
        eu.at = NOW - 100;
        CHECK(!db.applyCountryResult(ConfigDatabase::keyFor(a), eu), "older timestamp rejected (CAS)");
        eu.at = NOW; eu.network_generation = 2;
        CHECK(!db.applyCountryResult(ConfigDatabase::keyFor(a), eu), "older network generation rejected");
        CHECK(db.saveToDisk(P("rt.tsv")) == 2, "saved 2");
        std::string text = slurp(P("rt.tsv"));
        CHECK(text.compare(0, 21, "#HUNTER_CONFIG_DB_V4\n") == 0, "V4 header");
        CHECK(text.find(v4ColumnsLine()) != std::string::npos, "named columns line");
        CHECK(!exists(P("rt.tsv.tmp")), "no temp file left");
        ConfigDatabase db2;
        setup(db2);
        CHECK(db2.loadFromDisk(P("rt.tsv")) == 2, "loaded 2");
        auto rep = db2.lastLoadReport();
        CHECK(rep.ok && rep.source_version == 4 && !rep.migrated && rep.rejected == 0, "report");
        ConfigHealthRecord x, y;
        db.getRecord(a, &x);
        db2.getRecord(a, &y);
        CHECK(y.tag == x.tag && y.tag.find('\t') != std::string::npos, "tag escaping roundtrip");
        CHECK(y.ev.state == x.ev.state && y.ev.eligible_count == x.ev.eligible_count &&
              y.ev.ring.size() == x.ev.ring.size() && y.needs_retest, "evidence + retest required after restart");
        CHECK(y.ev.success_streak == 0 && y.ev.last_bulk_success == 0.0 && !y.ev.session_confirmed, "session confirmation dropped on load");
        CHECK(db2.evaluate(y).stability != Stability::Stable && !db2.evaluate(y).switch_eligible, "loaded Stable is not certified");
        CHECK(y.ev.ring.size() == 8 && y.ev.ring.front().id == x.ev.ring.front().id && y.ev.ring.front().id.find('"') != std::string::npos, "ring ids with quotes");
        CHECK(y.ev.ring.back().latency_ms == x.ev.ring.back().latency_ms && y.ev.last_full_success == x.ev.last_full_success, "ring latencies");
        CHECK(y.server_country == "NL" && y.server_ips.size() == 2 && y.exit_country == "DE" && y.network_generation == 3 &&
              y.geo_db_version == "2026-09" && y.exit_ip == "5.6.7.8", "country columns");
        CHECK(db2.evaluate(y).ewma == db.evaluate(x).ewma && db2.evaluate(y).p90_ms == db.evaluate(x).p90_ms, "S and p90 recomputed identically");
        db2.getRecord(b, &y);
        CHECK(y.ev.insecure_tls && y.ev.state == HealthState::Degraded, "static attrs from URI + state");
        // save again -> byte-identical (deterministic)
        CHECK(db2.saveToDisk(P("rt2.tsv")) == 2, "re-save");
        ConfigDatabase db2b; setup(db2b);
        CHECK(db2b.loadFromDisk(P("rt2.tsv")) == 2 && db2b.saveToDisk(P("rt3.tsv")) == 2 && slurp(P("rt3.tsv")) == slurp(P("rt2.tsv")), "load/save is a fixed point");
        ConfigDatabase::CountryUpdate bad;
        CHECK(!db.applyCountryResult("ek1:nope", bad), "unknown key");
    }
    T_END();

    T_CASE("V3 -> V4 migration with .v3.bak, atomic write, idempotence");
    {
        // two configs sharing an IP (previously collapsed), plus remark-variant duplicate, plus junk lines
        std::string v3 = "#HUNTER_CONFIG_DB_V3\n";
        v3 += v3Row(vless(1), "scrape", 1000.0, 1, 150.0, 7, 5);
        v3 += v3Row(vless(2), "harvest", 2000.0, 0, 0.0, 3, 0, 3);
        v3 += v3Row("vless://" + uuid(1) + "@1.2.3.4:443?type=ws&path=/p#dup", "later", 500.0, 0, 0.0, 9, 2);
        v3 += "#comment\n\n";
        spit(P("m.tsv"), v3);
        ConfigDatabase db;
        setup(db);
        CHECK(db.loadFromDisk(P("m.tsv")) == 2, "2 canonical endpoints survive (dup merged)");
        auto rep = db.lastLoadReport();
        CHECK(rep.ok && rep.migrated && rep.source_version == 3 && rep.rejected == 0, "report: migrated, nothing rejected");
        CHECK(rep.error.empty(), rep.error);
        CHECK(slurp(P("m.tsv.v3.bak")) == v3, ".v3.bak is the byte-exact original");
        std::string nowv4 = slurp(P("m.tsv"));
        CHECK(nowv4.compare(0, 20, "#HUNTER_CONFIG_DB_V4") == 0, "file rewritten as V4");
        CHECK(!exists(P("m.tsv.tmp")), "no temp left");
        ConfigHealthRecord r;
        db.getRecord(vless(1), &r);
        CHECK(r.ev.state == HealthState::Unknown && r.ev.eligible_count == 0 && r.ev.ring.empty() &&
              r.needs_retest && r.ev.failure_streak == 0 && r.ev.last_full_success == 0.0, "Unknown/Unrated, empty ring, retest");
        CHECK(db.evaluate(r).stability == Stability::Unrated && db.evaluate(r).score == 0.0, "legacy hint is not evidence");
        CHECK(r.first_seen == 500.0, "earliest first_seen");
        CHECK(r.total_tests == 9 && r.total_passes == 5, "legacy totals: max, not additive");
        CHECK(r.gemini_status == 1 && r.tag == "scrape", "gemini + metadata preserved");
        CHECK(r.alive && r.latency_ms == 150.0f, "alive/latency kept as hints");
        CHECK(r.uri.find("#r1") != std::string::npos || r.uri.find("#dup") != std::string::npos, "uri retained");
        // idempotence: loading the migrated file again changes nothing
        ConfigDatabase db2;
        setup(db2);
        CHECK(db2.loadFromDisk(P("m.tsv")) == 2, "reload");
        CHECK(!db2.lastLoadReport().migrated && db2.lastLoadReport().source_version == 4, "no second migration");
        CHECK(slurp(P("m.tsv")) == nowv4, "file unchanged by reload");
        CHECK(slurp(P("m.tsv.v3.bak")) == v3, "backup unchanged");
        CHECK(db2.saveToDisk(P("m.tsv")) == 2 && slurp(P("m.tsv")) == nowv4, "save after load is byte-identical");
        // a second, later V3 file at the same path must not overwrite an existing backup
        spit(P("m2.tsv"), v3);
        spit(P("m2.tsv.v3.bak"), "OLDER BACKUP");
        ConfigDatabase db3;
        setup(db3);
        db3.loadFromDisk(P("m2.tsv"));
        CHECK(slurp(P("m2.tsv.v3.bak")) == "OLDER BACKUP", "unrelated existing backup preserved");
        CHECK(slurp(P("m2.tsv.v3.bak.1")) == v3 && db3.lastLoadReport().backup_path == P("m2.tsv.v3.bak.1"), "current original gets its own exclusive backup");
        CHECK(slurp(P("m2.tsv")).compare(0, 20, "#HUNTER_CONFIG_DB_V4") == 0, "migrated after safe backup");
    }
    T_END();

    T_CASE("V2/V1 migration and saveToDisk over a legacy file keeps a backup");
    {
        std::string v2 = "#HUNTER_CONFIG_DB_V2\n" + std::string("vless://") + uuid(5) + "@8.8.8.8:443\tt\txray\t10\t11\t12\t1\t0\t99.5\t0\t4\t3\n";
        spit(P("v2.tsv"), v2);
        ConfigDatabase db;
        setup(db);
        CHECK(db.loadFromDisk(P("v2.tsv")) == 1 && db.lastLoadReport().source_version == 2, "V2 loaded");
        CHECK(exists(P("v2.tsv.v2.bak")), "backup named by source version");
        std::string v1 = "#HUNTER_CONFIG_DB_V1\n" + std::string("vless://") + uuid(6) + "@8.8.8.8:443\tt\txray\t10\t11\t12\t1\t99.5\t0\t4\t3\n";
        spit(P("v1.tsv"), v1);
        ConfigDatabase db1;
        setup(db1);
        CHECK(db1.loadFromDisk(P("v1.tsv")) == 1 && db1.lastLoadReport().source_version == 1, "V1 loaded");
        ConfigHealthRecord r;
        db1.getRecord(std::string("vless://") + uuid(6) + "@8.8.8.8:443", &r);
        CHECK(r.latency_ms == 99.5f && r.total_tests == 4 && r.total_passes == 3 && !r.telegram_only, "V1 columns");
        // saveToDisk onto an un-migrated legacy file
        spit(P("v3s.tsv"), "#HUNTER_CONFIG_DB_V3\n" + v3Row(vless(9), "t", 1.0, 1, 5.0, 1, 1));
        ConfigDatabase db4;
        setup(db4);
        db4.addConfigs({vless(1)});
        CHECK(db4.saveToDisk(P("v3s.tsv")) == 1, "save");
        CHECK(slurp(P("v3s.tsv.v3.bak")).find("#HUNTER_CONFIG_DB_V3") == 0, "legacy file backed up before overwrite");
    }
    T_END();

    T_CASE("corruption rejection and unknown versions");
    {
        ConfigDatabase db;
        setup(db);
        std::string a = vless(1), b = vless(2), c = vless(3);
        db.addConfigs({a, b, c});
        for (int i = 0; i < 3; i++) { db.applyProbeResult(passFor(a, NOW - 100 + 10 * i)); db.applyProbeResult(passFor(b, NOW - 100 + 10 * i)); }
        db.saveToDisk(P("c.tsv"));
        std::string good = slurp(P("c.tsv"));
        std::vector<std::string> lines;
        {
            std::istringstream is(good);
            std::string l;
            while (std::getline(is, l)) lines.push_back(l);
        }
        CHECK(lines.size() == 2 + 3, "header + columns + 3 rows");
        auto rowFor = [&](const std::string& uri) { for (auto& l : lines) if (l.find(uri.substr(0, 60)) == 0) return l; return std::string(); };
        auto mutateCol = [&](const std::string& row, size_t col, const std::string& v) {
            auto f = splitTabs(row);
            f[col] = v;
            std::string o;
            for (size_t i = 0; i < f.size(); i++) { if (i) o += "\t"; o += f[i]; }
            return o;
        };
        std::string ra = rowFor(a), rb = rowFor(b), rc = rowFor(c);
        CHECK(!ra.empty() && !rb.empty() && !rc.empty(), "rows found");
        HealthThresholds th;
        ConfigHealthRecord tmp;
        std::string e;
        CHECK(parseRecordV4(ra, th, &tmp, &e), "baseline parses: " + e);
        const size_t K0 = kLegacyColumns;  // first extension column
        struct Case { const char* name; std::string row; };
        std::vector<Case> bad = {
            {"truncated row", ra.substr(0, ra.size() / 2)},
            {"extra column", ra + "\textra"},
            {"tampered endpoint key", mutateCol(ra, K0, "ek1:" + std::string(64, '0'))},
            {"unsupported key_version", mutateCol(ra, K0 + 1, "2")},
            {"bad health state", mutateCol(ra, K0 + 2, "Zombie")},
            {"Dead without failures", mutateCol(ra, K0 + 2, "Dead")},
            {"negative eligible", mutateCol(ra, K0 + 5, "-1")},
            {"streak > eligible", mutateCol(ra, K0 + 6, "99")},
            {"non-numeric timestamp", mutateCol(ra, K0 + 9, "yesterday")},
            {"NaN timestamp", mutateCol(ra, K0 + 9, "nan")},
            {"bad ring json", mutateCol(ra, K0 + 13, "[{\"id\":\"x\",")},
            {"ring not array", mutateCol(ra, K0 + 13, "{}")},
            {"ring success without latency", mutateCol(ra, K0 + 13, "[{\"id\":\"\",\"t\":1.0,\"y\":1,\"latency_ms\":null}]")},
            {"ring larger than N", mutateCol(ra, K0 + 13, [] { std::string s = "["; for (int i = 0; i < 25; i++) { if (i) s += ","; s += "{\"id\":\"\",\"t\":1.0,\"y\":0,\"latency_ms\":null}"; } return s + "]"; }())},
            {"bad server_ips", mutateCol(ra, K0 + 14, "[1,2]")},
            {"bad escape", mutateCol(ra, 1, "bad\\qescape")},
            {"bad uri", mutateCol(ra, 0, "not a uri")},
            {"bad alive flag", mutateCol(ra, 6, "2")},
            {"bad last_outcome", mutateCol(ra, K0 + 12, "Exploded")},
            {"bad network_generation", mutateCol(ra, K0 + 23, "-5")},
            {"oversized row", mutateCol(ra, 1, std::string(kMaxRowBytes + 10, 'x'))},
        };
        for (auto& cse : bad) {
            ConfigHealthRecord t;
            std::string er;
            CHECK(!parseRecordV4(cse.row, th, &t, &er), std::string("must reject: ") + cse.name);
        }
        // whole-file: bad rows skipped, good rows kept, and counted
        std::string file = lines[0] + "\n" + lines[1] + "\n" + ra + "\n" + bad[0].row + "\n" + rb + "\n" + bad[3].row + "\n" + rc + "\n";
        spit(P("c2.tsv"), file);
        ConfigDatabase d2;
        setup(d2);
        CHECK(d2.loadFromDisk(P("c2.tsv")) == 3, "good rows loaded");
        CHECK(d2.lastLoadReport().rejected == 2 && d2.lastLoadReport().ok, "rejections counted");
        CHECK(slurp(P("c2.tsv")) == file, "V4 load never rewrites the file");
        // unknown future version: visible failure, no overwrite
        std::string v5 = "#HUNTER_CONFIG_DB_V5\n" + lines[1] + "\n" + ra + "\n";
        spit(P("v5.tsv"), v5);
        ConfigDatabase d3;
        setup(d3);
        d3.addConfigs({vless(7)});
        CHECK(d3.loadFromDisk(P("v5.tsv")) == 0 && !d3.lastLoadReport().ok && !d3.lastLoadReport().error.empty(), "V5 load fails visibly");
        CHECK(d3.saveToDisk(P("v5.tsv")) == -1 && !d3.lastSaveError().empty(), "save refuses to overwrite V5");
        CHECK(slurp(P("v5.tsv")) == v5 && !exists(P("v5.tsv.tmp")), "V5 file untouched");
        // wrong column header / garbage file / missing file
        spit(P("hdr.tsv"), "#HUNTER_CONFIG_DB_V4\n#COLUMNS\tfoo\n" + ra + "\n");
        ConfigDatabase d4;
        CHECK(d4.loadFromDisk(P("hdr.tsv")) == 0 && !d4.lastLoadReport().ok, "bad column header rejected");
        spit(P("junk.tsv"), "hello world\n");
        CHECK(d4.loadFromDisk(P("junk.tsv")) == 0 && !d4.lastLoadReport().ok, "non-db file rejected");
        CHECK(d4.loadFromDisk(P("missing.tsv")) == 0 && !d4.lastLoadReport().ok, "missing file");
        // a legacy-migration file with a bad generated row can't happen, but corrupt legacy numbers are skipped
        spit(P("lc.tsv"), "#HUNTER_CONFIG_DB_V3\n" + v3Row(vless(1), "a", 1.0, 1, 5.0, 1, 1) + vless(2) + "\ta\tb\tNaNx\t1\t1\t1\t0\t1\t0\t1\t1\t-1\t0\n");
        ConfigDatabase d5;
        setup(d5);
        CHECK(d5.loadFromDisk(P("lc.tsv")) == 1 && d5.lastLoadReport().rejected == 1, "corrupt legacy row skipped");
    }
    T_END();

    T_CASE("live cache V3: save, load, merge never resurrects or overrides newer evidence");
    {
        ConfigDatabase src;
        setup(src);
        std::string a = vless(1), b = vless(2), c = vless(3), d = vless(4);
        src.addConfigs({a, b, c, d});
        src.applyProbeResult(passFor(a, NOW - 100));
        src.applyProbeResult(passFor(b, NOW - 100));
        for (double t : {0.0, 15.0, 30.0}) src.applyProbeResult(failFor(c, NOW - 1000 + t));  // dead, never alive -> not cached
        CHECK(src.saveLiveToDisk(P("live.tsv")) == 2, "alive records cached (dead/never-alive omitted)");
        CHECK(slurp(P("live.tsv")).compare(0, 22, "#HUNTER_LIVE_CACHE_V3\n") == 0, "live V3 header");
        // destination has b dead with NEWER failure evidence, and nothing else
        ConfigDatabase dst;
        setup(dst);
        dst.addConfigs({b});
        dst.applyProbeResult(passFor(b, NOW - 50));
        for (double t : {0.0, 15.0, 30.0}) dst.applyProbeResult(failFor(b, NOW - 40 + t));
        ConfigHealthRecord before;
        dst.getRecord(b, &before);
        CHECK(before.ev.state == HealthState::Dead, "dst b is Dead");
        int n = dst.loadLiveFromDisk(P("live.tsv"));
        CHECK(n >= 1 && dst.lastLoadReport().ok, "live loaded");
        ConfigHealthRecord rb, ra;
        dst.getRecord(b, &rb);
        CHECK(rb.ev.state == HealthState::Dead && rb.ev.last_attempt_at == before.ev.last_attempt_at && !rb.alive,
              "older cached evidence did not resurrect Dead record");
        CHECK(dst.getRecord(a, &ra) && ra.ev.state == HealthState::Healthy && ra.needs_retest, "new record added with evidence + retest flag");
        // newer cached evidence is adopted
        ConfigDatabase old;
        setup(old);
        old.addConfigs({a});
        old.applyProbeResult(passFor(a, NOW - 5000));
        old.loadLiveFromDisk(P("live.tsv"));
        old.getRecord(a, &ra);
        CHECK(ra.ev.last_attempt_at == NOW - 100, "newer cache evidence adopted");
        // newer country in DB is not overridden by older cache
        ConfigDatabase cdb;
        setup(cdb);
        cdb.addConfigs({a});
        ConfigDatabase::CountryUpdate eu;
        eu.is_exit = true; eu.country = "FR"; eu.at = NOW + 10; eu.network_generation = 1; eu.source = "x";
        cdb.applyCountryResult(ConfigDatabase::keyFor(a), eu);
        cdb.loadLiveFromDisk(P("live.tsv"));
        cdb.getRecord(a, &ra);
        CHECK(ra.exit_country == "FR", "country timestamp not overridden");
        // legacy live cache V2: alive hint never touches existing Dead record
        std::string live2 = "#HUNTER_LIVE_CACHE_V2\n" + v3Row(b, "t", 1.0, 1, 10.0, 1, 1) + v3Row(vless(8), "t", 1.0, 1, 10.0, 1, 1);
        spit(P("live2.tsv"), live2);
        int n2 = dst.loadLiveFromDisk(P("live2.tsv"));
        dst.getRecord(b, &rb);
        CHECK(rb.ev.state == HealthState::Dead && !rb.alive, "legacy alive hint does not resurrect");
        ConfigHealthRecord r8;
        CHECK(n2 >= 1 && dst.getRecord(vless(8), &r8) && r8.ev.state == HealthState::Unknown && r8.alive && r8.needs_retest,
              "legacy new record is a hint only");
        spit(P("live9.tsv"), "#HUNTER_LIVE_CACHE_V9\n");
        CHECK(dst.loadLiveFromDisk(P("live9.tsv")) == 0 && !dst.lastLoadReport().ok, "unknown live version rejected");
        CHECK(src.saveLiveToDisk(P("live9.tsv")) == -1 && slurp(P("live9.tsv")) == "#HUNTER_LIVE_CACHE_V9\n", "unknown live version not overwritten");
    }
    T_END();

    T_CASE("capacity and file permissions");
    {
        ConfigDatabase db(3);
        setup(db);
        db.addConfigs({vless(1), vless(2), vless(3), vless(4), vless(5)});
        CHECK(db.size() <= 3, "bounded by max_size");
        db.saveToDisk(P("perm.tsv"));
        CHECK(access(P("perm.tsv").c_str(), R_OK) == 0, "readable");
        FILE* f = popen(("stat -c %a " + P("perm.tsv")).c_str(), "r");
        char buf[16] = {0};
        if (f) { if (!fgets(buf, sizeof buf, f)) buf[0] = 0; pclose(f); }
        CHECK(std::string(buf).compare(0, 3, "600") == 0, std::string("restrictive permissions, got ") + buf);
    }
    T_END();

    T_CASE("migration with rejected rows never replaces the only original");
    {
        std::string bad = "#HUNTER_CONFIG_DB_V3\n" + v3Row(vless(1), "t", 1.0, 1, 5.0, 1, 1) +
                          vless(2) + "\tt\txray\tinvalid\t2\t2\t1\t0\t100\t0\t1\t1\t-1\t0\n";
        spit(P("rj.tsv"), bad);
        spit(P("rj.tsv.v3.bak"), "unrelated prior backup\n");
        ConfigDatabase db; setup(db);
        CHECK(db.loadFromDisk(P("rj.tsv")) == 1, "valid row still usable in memory");
        auto rep = db.lastLoadReport();
        CHECK(rep.ok && !rep.migrated && rep.rejected == 1 && !rep.error.empty(), "migration refused visibly");
        CHECK(slurp(P("rj.tsv")) == bad && slurp(P("rj.tsv.v3.bak")) == "unrelated prior backup\n", "original and stale backup untouched");
        CHECK(db.saveToDisk(P("rj.tsv")) == 1, "explicit save later");
        CHECK(slurp(P("rj.tsv.v3.bak.1")) == bad, "explicit save backs the original up first (no clobbered stale backup)");
        // V4 file with a rejected row: bytes preserved before any later save drops it
        ConfigDatabase a; setup(a); a.addConfigs({vless(1), vless(2)});
        a.saveToDisk(P("v4rej.tsv"));
        std::string t = slurp(P("v4rej.tsv"));
        spit(P("v4rej.tsv"), t + "garbage\trow\n");
        ConfigDatabase b; setup(b);
        CHECK(b.loadFromDisk(P("v4rej.tsv")) == 2 && b.lastLoadReport().rejected == 1, "rejection counted");
        CHECK(slurp(b.lastLoadReport().backup_path) == t + "garbage\trow\n", "rejected rows backed up byte-exact");
    }
    T_END();

    T_CASE("every saved row reloads; oversized text is capped, never lost");
    {
        ConfigDatabase db; setup(db);
        db.addConfigs({vless(1)}, std::string(kMaxRowBytes + 10, 'x'));
        db.updateHealth(vless(1), true, 100.0f);
        NOW += 10;
        db.updateHealth(vless(1), false, 0.0f, "", true);
        CHECK(db.saveToDisk(P("long.tsv")) == 1, "save ok");
        ConfigDatabase x; setup(x);
        CHECK(x.loadFromDisk(P("long.tsv")) == 1 && x.lastLoadReport().rejected == 0, "reload ok");
        // a row the format cannot represent makes the save refuse instead of publishing it
        ConfigDatabase y; setup(y);
        y.addConfigs({vless(1)});
        ConfigHealthRecord r; y.getRecord(vless(1), &r);
        ProbeResult pr = passFor(vless(1), NOW - 1);
        pr.engine = std::string(kMaxRowBytes + 10, 'e');  // engine name is stored in the row
        y.applyProbeResult(pr);
        int sv = y.saveToDisk(P("long2.tsv"));
        ConfigDatabase z; setup(z);
        CHECK(sv == -1 || z.loadFromDisk(P("long2.tsv")) == 1, "unloadable content is refused or round-trips");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("restart requires fresh confirmation; generation cannot bridge");
    {
        ConfigDatabase d; setup(d);
        std::string u = vless(1);
        d.addConfigs({u});
        for (int i = 0; i < 6; i++) d.applyProbeResult(passFor(u, NOW - 300 + 60 * i));
        ConfigHealthRecord r; d.getRecord(u, &r);
        CHECK(d.evaluate(r).stable && d.evaluate(r).switch_eligible, "stable before restart");
        d.saveToDisk(P("st.tsv"));
        ConfigDatabase x; setup(x);
        x.loadFromDisk(P("st.tsv"));
        x.getRecord(u, &r);
        CHECK(r.needs_retest && !x.evaluate(r).switch_eligible && !x.evaluate(r).stable, "loaded record needs a fresh probe");
        CHECK(x.getRecommendedRecords().empty(), "not recommended until re-confirmed");
        x.applyProbeResult(passFor(u, NOW));
        x.getRecord(u, &r);
        CHECK(x.evaluate(r).switch_eligible && !x.evaluate(r).stable, "one fresh pass: switchable, not Stable");
        ProbeResult g = passFor(u, NOW + 15);
        g.generation = 1;
        d.applyProbeResult(passFor(u, NOW));  // keep d current
        d.applyProbeResult(g);
        g.finished_at = NOW + 30; g.started_at = NOW + 30; g.generation = 2;
        NOW += 30;
        d.applyProbeResult(g);
        d.getRecord(u, &r);
        CHECK(r.ev.success_streak == 1 && !d.evaluate(r).stable, "new generation cannot bridge a previous streak");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("excluded rounds keep configs schedulable (bounded backoff), unclassified legacy failures do not strand");
    {
        spit(P("retry.tsv"), "#HUNTER_CONFIG_DB_V3\n" + vless(1) + "\tt\txray\t1\t2\t0\t0\t0\t0\t3\t4\t0\t-1\t0\n");
        ConfigDatabase d; setup(d);
        d.loadFromDisk(P("retry.tsv"));
        ProbeResult ex = passFor(vless(1), NOW);
        ex.outcome = ProbeOutcome::Indeterminate;
        d.applyProbeResult(ex);
        CHECK(d.getUntestedBatch(10).empty(), "backoff: not hot-looping immediately");
        NOW += 6;
        auto b = d.getUntestedBatch(10);
        CHECK(b.size() == 1 && b[0].needs_retest, "retryable again after the backoff; retest flag kept");
        ex.finished_at = ex.started_at = NOW;
        d.applyProbeResult(ex);
        NOW += 6;
        CHECK(d.getUntestedBatch(10).empty(), "backoff doubles (10 s)");
        NOW += 5;
        CHECK(d.getUntestedBatch(10).size() == 1, "still schedulable");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("legacy adapter: never-working configs are not alive; failures do not penalize evidence");
    {
        ConfigDatabase d; setup(d);
        std::string u = vless(1);
        d.addConfigs({u});
        d.updateHealth(u, false, 0);
        ConfigHealthRecord r; d.getRecord(u, &r);
        CHECK(!r.alive && d.getAliveRecords().empty(), "first failure of a never-passing config: not alive");
        CHECK(r.ev.eligible_count == 0 && r.ev.state == HealthState::Unknown, "no typed penalty from an unclassified failure");
        NOW += 60;
        d.updateHealth(u, true, 100);
        d.getRecord(u, &r);
        CHECK(r.alive && !d.getHealthyRecords().empty(), "working recovery visible");
        for (int i = 0; i < 3; i++) { NOW += 1; d.updateHealth(u, false, 0); }
        d.getRecord(u, &r);
        CHECK(!r.alive && r.ev.state == HealthState::Healthy, "legacy alive hint drops, typed state untouched");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("Dead retention counts from entering Dead, only attributed Dead is removed");
    {
        ConfigDatabase d; setup(d);
        std::string u = vless(1), w = vless(2);
        d.addConfigs({u, w});
        d.applyProbeResult(passFor(u, NOW - 4 * 86400));
        for (double t : {0.0, 15.0, 30.0}) d.applyProbeResult(failFor(u, NOW - 60 + t));
        CHECK(d.evictDead() == 0 && d.removeDeadLive(1) == 0, "freshly Dead record kept despite 4-day-old success");
        ConfigHealthRecord r; d.getRecord(u, &r);
        CHECK(r.ev.dead_since == NOW - 30, "dead_since persisted value");
        d.saveToDisk(P("dead.tsv"));
        ConfigDatabase x; setup(x); x.loadFromDisk(P("dead.tsv"));
        x.getRecord(u, &r);
        CHECK(r.ev.state == HealthState::Dead && r.ev.dead_since == NOW - 30, "dead_since survives reload");
        NOW += 72 * 3600 + 31;
        CHECK(d.removeDeadLive(1) == 1, "removed after 72 h in Dead");
        // legacy-adapter failures never create an evictable Dead record
        ConfigDatabase l; setup(l);
        l.addConfigs({u});
        l.updateHealth(u, true, 10);
        NOW += 5 * 86400;
        for (int i = 0; i < 4; i++) { NOW += 1; l.updateHealth(u, false, 0, "", true); }
        CHECK(l.evictDead() == 0 && l.removeDeadLive(1) == 0 && l.size() == 1, "outage via legacy callers cannot mass-evict");
        NOW = 1.7e9;
    }
    T_END();

    T_CASE("concurrent saves to one path from separate instances (unique temps, no corruption)");
    {
        std::vector<std::thread> ts;
        std::atomic<int> bad{0};
        for (int k = 0; k < 4; k++)
            ts.emplace_back([&, k] {
                ConfigDatabase d;
                d.setClock([]() { return 1.7e9; });
                d.addConfigs({vless(k + 1)});
                for (int i = 0; i < 25; i++) if (d.saveToDisk(P("conc.tsv")) != 1) bad++;
            });
        for (auto& t : ts) t.join();
        ConfigDatabase x; setup(x);
        CHECK(bad == 0 && x.loadFromDisk(P("conc.tsv")) == 1 && x.lastLoadReport().rejected == 0, "file valid after concurrent writers");
        CHECK(!exists(P("conc.tsv.tmp")), "no fixed-name temp");
    }
    T_END();

    int rc = T_SUMMARY();
    std::string cmd = "rm -rf '" + TMP + "'";
    if (std::system(cmd.c_str()) != 0) {}
    return rc;
}
