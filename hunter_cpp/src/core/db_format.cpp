#include "core/db_format.h"
#include "core/endpoint_key.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hunter {

const char* const kDbHeaderV4 = "#HUNTER_CONFIG_DB_V4";
const char* const kLiveHeaderV3 = "#HUNTER_LIVE_CACHE_V3";

const std::vector<std::string>& v4ExtensionColumns() {
    static const std::vector<std::string> cols = {
        "endpoint_key", "key_version", "health_state", "stability", "probe_profile", "eligible_count",
        "success_streak", "streak_started_at", "failure_streak_started_at", "last_full_success",
        "last_bulk_success", "last_attempt_at", "last_outcome", "probe_ring", "server_ips",
        "server_country", "server_country_source", "server_country_at", "geo_db_version", "exit_ip",
        "exit_country", "exit_country_source", "exit_country_at", "network_generation", "dead_since"};
    return cols;
}

std::string v4ColumnsLine() {
    std::string s = "#COLUMNS";
    for (const auto& c : v4ExtensionColumns()) s += "\t" + c;
    return s;
}

std::string escapeTsv(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            case '\n': o += "\\n"; break;
            default: o.push_back(c);
        }
    }
    return o;
}

bool unescapeTsv(const std::string& s, std::string* out) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\') { o.push_back(s[i]); continue; }
        if (++i >= s.size()) return false;
        switch (s[i]) {
            case '\\': o.push_back('\\'); break;
            case 't': o.push_back('\t'); break;
            case 'r': o.push_back('\r'); break;
            case 'n': o.push_back('\n'); break;
            default: return false;
        }
    }
    *out = std::move(o);
    return true;
}

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> f;
    size_t pos = 0;
    while (true) {
        size_t t = line.find('\t', pos);
        if (t == std::string::npos) { f.push_back(line.substr(pos)); break; }
        f.push_back(line.substr(pos, t - pos));
        pos = t + 1;
    }
    return f;
}

namespace {

std::string fmtD(double v) {
    if (!std::isfinite(v)) v = 0.0;
    char b[64];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

bool pD(const std::string& s, double* o, bool nonneg = true) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(s.c_str(), &end);
    if (errno || end != s.c_str() + s.size() || !std::isfinite(v)) return false;
    if (nonneg && v < 0) return false;
    *o = v;
    return true;
}
bool pI(const std::string& s, long long lo, long long hi, long long* o) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno || end != s.c_str() + s.size() || v < lo || v > hi) return false;
    *o = v;
    return true;
}
bool pU64(const std::string& s, uint64_t* o) {
    if (s.empty() || s[0] == '-') return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno || end != s.c_str() + s.size()) return false;
    *o = v;
    return true;
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o.push_back(char(c));
    }
    return o + "\"";
}

// Minimal strict JSON reader for the two shapes we write.
struct Json {
    const std::string& s;
    size_t i = 0;
    explicit Json(const std::string& str) : s(str) {}
    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t')) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    bool str(std::string* o) {
        ws();
        if (i >= s.size() || s[i] != '"') return false;
        i++;
        std::string r;
        while (i < s.size() && s[i] != '"') {
            unsigned char c = s[i];
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++i >= s.size()) return false;
                char e = s[i];
                if (e == '"' || e == '\\' || e == '/') r.push_back(e);
                else if (e == 'n') r.push_back('\n');
                else if (e == 't') r.push_back('\t');
                else if (e == 'r') r.push_back('\r');
                else if (e == 'u') {
                    if (i + 4 >= s.size()) return false;
                    unsigned v = 0;
                    for (int k = 1; k <= 4; k++) {
                        char h = s[i + k];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= h - '0';
                        else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                        else return false;
                    }
                    if (v > 0x7f) return false;
                    r.push_back(char(v));
                    i += 4;
                } else return false;
                i++;
            } else { r.push_back(char(c)); i++; }
        }
        if (i >= s.size()) return false;
        i++;
        *o = std::move(r);
        return true;
    }
    bool num(double* o) {
        ws();
        size_t st = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E')) i++;
        if (i == st) return false;
        return pD(s.substr(st, i - st), o, false);
    }
    bool lit(const char* w) {
        ws();
        size_t n = std::strlen(w);
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        return false;
    }
    bool end() { ws(); return i == s.size(); }
};

std::string ringJson(const HealthEvidence& ev) {
    std::string o = "[";
    bool first = true;
    for (const auto& s : ev.ring) {
        if (!first) o += ",";
        first = false;
        o += "{\"id\":" + jsonStr(s.id) + ",\"t\":" + fmtD(s.t) + ",\"y\":" + (s.success ? "1" : "0") +
             ",\"latency_ms\":" + (s.success ? fmtD(s.latency_ms) : "null") + "}";
    }
    return o + "]";
}

bool parseRing(const std::string& txt, size_t cap, std::deque<ProbeSample>* out) {
    Json j(txt);
    if (!j.eat('[')) return false;
    std::deque<ProbeSample> r;
    if (!j.eat(']')) {
        do {
            if (r.size() >= cap) return false;
            if (!j.eat('{')) return false;
            ProbeSample s;
            bool hid = false, ht = false, hy = false, hl = false;
            do {
                std::string k;
                if (!j.str(&k) || !j.eat(':')) return false;
                if (k == "id" && !hid) { hid = true; if (!j.str(&s.id)) return false; }
                else if (k == "t" && !ht) { ht = true; if (!j.num(&s.t) || s.t < 0) return false; }
                else if (k == "y" && !hy) {
                    hy = true;
                    double y;
                    if (!j.num(&y) || (y != 0.0 && y != 1.0)) return false;
                    s.success = y == 1.0;
                } else if (k == "latency_ms" && !hl) {
                    hl = true;
                    if (j.lit("null")) s.latency_ms = -1.0;
                    else if (!j.num(&s.latency_ms) || s.latency_ms < 0) return false;
                } else return false;
            } while (j.eat(','));
            if (!j.eat('}') || !hid || !ht || !hy || !hl) return false;
            if (s.success == (s.latency_ms < 0)) return false;  // success needs latency, failure null
            r.push_back(std::move(s));
        } while (j.eat(','));
        if (!j.eat(']')) return false;
    }
    if (!j.end()) return false;
    *out = std::move(r);
    return true;
}

std::string ipsJson(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; o += jsonStr(v[i]); }
    return o + "]";
}

bool parseIps(const std::string& txt, std::vector<std::string>* out) {
    Json j(txt);
    if (!j.eat('[')) return false;
    std::vector<std::string> r;
    if (!j.eat(']')) {
        do {
            std::string s;
            if (r.size() >= 64 || !j.str(&s)) return false;
            r.push_back(std::move(s));
        } while (j.eat(','));
        if (!j.eat(']')) return false;
    }
    if (!j.end()) return false;
    *out = std::move(r);
    return true;
}

}  // namespace

void initRecordIdentity(ConfigHealthRecord* rec) {
    EndpointKey ek = computeEndpointKey(rec->uri);
    rec->endpoint_key = ek.key;
    rec->uri_hash = ek.key;
    rec->key_version = kEndpointKeyVersion;
    rec->ev.protocol = ek.protocol;
    rec->ev.insecure_tls = ek.insecure_tls;
    static const char* supported[] = {"vmess", "vless", "trojan", "shadowsocks", "hysteria2", "tuic"};
    bool sup = false;
    for (const char* p : supported) sup |= (ek.protocol == p);
    rec->ev.unsupported = !ek.valid || !sup;
    rec->ev.telegram_only = rec->telegram_only;
}

void syncLegacyFromEvidence(ConfigHealthRecord* rec) {
    const HealthEvidence& ev = rec->ev;
    rec->consecutive_fails = static_cast<int>(std::min<uint32_t>(ev.failure_streak, 1000000));
    if (ev.last_full_success > 0.0) rec->last_alive_time = ev.last_full_success;
    if (ev.state == HealthState::Unknown || ev.state == HealthState::Testing) return;  // hints stay
    // Alive means a full success actually happened; a never-working config is never alive.
    rec->alive = (ev.state == HealthState::Healthy || ev.state == HealthState::Degraded) &&
                 ev.last_full_success > 0.0;
    if (!rec->alive) { rec->latency_ms = 0.0f; rec->telegram_only = false; }
    else {
        for (auto it = ev.ring.rbegin(); it != ev.ring.rend(); ++it)
            if (it->success) { rec->latency_ms = static_cast<float>(it->latency_ms); break; }
    }
}

std::string serializeRecordV4(const ConfigHealthRecord& rec, double now, const HealthThresholds& th) {
    const HealthEvidence& ev = rec.ev;
    HealthEvaluation e = evaluateHealth(ev, now, th);
    std::vector<std::string> f;
    f.reserve(kLegacyColumns + 24);
    f.push_back(rec.uri);
    f.push_back(rec.tag);
    f.push_back(rec.engine_used);
    f.push_back(fmtD(rec.first_seen));
    f.push_back(fmtD(rec.last_tested));
    f.push_back(fmtD(rec.last_alive_time));
    f.push_back(rec.alive ? "1" : "0");
    f.push_back(rec.telegram_only ? "1" : "0");
    f.push_back(fmtD(rec.latency_ms));
    f.push_back(std::to_string(ev.failure_streak));  // consecutive_fails == failure streak
    f.push_back(std::to_string(rec.total_tests));
    f.push_back(std::to_string(rec.total_passes));
    f.push_back(std::to_string(rec.gemini_status));
    f.push_back(fmtD(rec.gemini_checked_at));
    // V4 extension
    f.push_back(rec.endpoint_key);
    f.push_back(std::to_string(rec.key_version));
    f.push_back(healthStateName(ev.state));
    f.push_back(stabilityName(e.stability));
    f.push_back(std::to_string(ev.probe_profile));
    f.push_back(std::to_string(ev.eligible_count));
    f.push_back(std::to_string(ev.success_streak));
    f.push_back(fmtD(ev.streak_started_at));
    f.push_back(fmtD(ev.failure_streak_started_at));
    f.push_back(fmtD(ev.last_full_success));
    f.push_back(fmtD(ev.last_bulk_success));
    f.push_back(fmtD(ev.last_attempt_at));
    f.push_back(ev.last_outcome);
    f.push_back(ringJson(ev));
    f.push_back(ipsJson(rec.server_ips));
    f.push_back(rec.server_country);
    f.push_back(rec.server_country_source);
    f.push_back(fmtD(rec.server_country_at));
    f.push_back(rec.geo_db_version);
    f.push_back(rec.exit_ip);
    f.push_back(rec.exit_country);
    f.push_back(rec.exit_country_source);
    f.push_back(fmtD(rec.exit_country_at));
    f.push_back(std::to_string(rec.network_generation));
    f.push_back(fmtD(ev.dead_since));
    std::string line;
    for (size_t i = 0; i < f.size(); i++) {
        if (i) line.push_back('\t');
        line += escapeTsv(f[i]);
    }
    return line;
}

bool parseRecordV4(const std::string& line, const HealthThresholds& th, ConfigHealthRecord* out,
                   std::string* err) {
    auto bad = [&](const std::string& m) { if (err) *err = m; return false; };
    if (line.size() > kMaxRowBytes) return bad("row too long");
    auto raw = splitTabs(line);
    if (raw.size() != kLegacyColumns + v4ExtensionColumns().size()) return bad("wrong column count");
    std::vector<std::string> f(raw.size());
    for (size_t i = 0; i < raw.size(); i++)
        if (!unescapeTsv(raw[i], &f[i])) return bad("bad escape");

    ConfigHealthRecord r;
    long long iv;
    r.uri = f[0];
    if (r.uri.empty() || r.uri.find("://") == std::string::npos) return bad("bad uri");
    r.tag = f[1];
    r.engine_used = f[2];
    if (!pD(f[3], &r.first_seen) || !pD(f[4], &r.last_tested) || !pD(f[5], &r.last_alive_time))
        return bad("bad timestamp");
    if (f[6] != "0" && f[6] != "1") return bad("bad alive");
    r.alive = f[6] == "1";
    if (f[7] != "0" && f[7] != "1") return bad("bad telegram_only");
    r.telegram_only = f[7] == "1";
    double lat;
    if (!pD(f[8], &lat) || lat > 1e9) return bad("bad latency");
    r.latency_ms = static_cast<float>(lat);
    if (!pI(f[9], 0, 1000000, &iv)) return bad("bad consecutive_fails");
    r.ev.failure_streak = static_cast<uint32_t>(iv);
    if (!pI(f[10], 0, 2000000000LL, &iv)) return bad("bad total_tests");
    r.total_tests = static_cast<int>(iv);
    if (!pI(f[11], 0, 2000000000LL, &iv)) return bad("bad total_passes");
    r.total_passes = static_cast<int>(iv);
    if (!pI(f[12], -1, 1, &iv)) return bad("bad gemini_status");
    r.gemini_status = static_cast<int>(iv);
    if (!pD(f[13], &r.gemini_checked_at)) return bad("bad gemini_checked_at");

    size_t k = kLegacyColumns;
    const std::string stored_key = f[k++];
    if (!pI(f[k++], 1, 1, &iv)) return bad("unsupported key_version");
    r.key_version = 1;
    if (!parseHealthState(f[k++], &r.ev.state)) return bad("bad health_state");
    Stability st;
    if (!parseStability(f[k++], &st)) return bad("bad stability");
    if (!pI(f[k++], 0, 1000, &iv)) return bad("bad probe_profile");
    r.ev.probe_profile = static_cast<int>(iv);
    if (!pI(f[k++], 0, 4294967295LL, &iv)) return bad("bad eligible_count");
    r.ev.eligible_count = static_cast<uint32_t>(iv);
    if (!pI(f[k++], 0, 4294967295LL, &iv)) return bad("bad success_streak");
    r.ev.success_streak = static_cast<uint32_t>(iv);
    if (!pD(f[k++], &r.ev.streak_started_at) || !pD(f[k++], &r.ev.failure_streak_started_at) ||
        !pD(f[k++], &r.ev.last_full_success) || !pD(f[k++], &r.ev.last_bulk_success) ||
        !pD(f[k++], &r.ev.last_attempt_at))
        return bad("bad evidence timestamp");
    r.ev.last_outcome = f[k++];
    if (!r.ev.last_outcome.empty()) {
        ProbeOutcome o;
        if (!parseOutcome(r.ev.last_outcome, &o)) return bad("bad last_outcome");
    }
    if (!parseRing(f[k++], static_cast<size_t>(std::max(1, th.ring_size)), &r.ev.ring))
        return bad("bad probe_ring");
    if (!parseIps(f[k++], &r.server_ips)) return bad("bad server_ips");
    r.server_country = f[k++];
    r.server_country_source = f[k++];
    if (!pD(f[k++], &r.server_country_at)) return bad("bad server_country_at");
    r.geo_db_version = f[k++];
    r.exit_ip = f[k++];
    r.exit_country = f[k++];
    r.exit_country_source = f[k++];
    if (!pD(f[k++], &r.exit_country_at)) return bad("bad exit_country_at");
    if (!pU64(f[k++], &r.network_generation)) return bad("bad network_generation");
    if (!pD(f[k++], &r.ev.dead_since)) return bad("bad dead_since");

    // A Dead claim the history cannot certify is downgraded (never trusted, never evicted).
    bool downgraded_dead = false;
    if (r.ev.state == HealthState::Dead && !deadCertified(r.ev, th)) {
        r.ev.state = (r.ev.eligible_count == 0 && r.ev.ring.empty()) ? HealthState::Unknown : HealthState::Degraded;
        r.ev.dead_since = 0.0;
        downgraded_dead = true;
    }
    std::string why;
    if (!evidenceConsistent(r.ev, th, &why)) return bad("inconsistent evidence: " + why);
    (void)downgraded_dead;
    // Identity: recompute and compare; static attributes always come from the URI.
    initRecordIdentity(&r);
    if (r.endpoint_key != stored_key) return bad("endpoint_key mismatch");
    // Restart boundary: history stays for display, but every loaded record needs a fresh probe
    // and its session-dependent certification (success run, bulk confirmation) is dropped.
    r.needs_retest = true;
    if (r.ev.state == HealthState::Testing) r.ev.state = HealthState::Unknown;  // no round survives restart
    resetSessionEvidence(r.ev);
    // alive is derived from state once there is evidence; otherwise the stored value is a hint.
    const bool hint_alive = r.alive;
    const float hint_lat = r.latency_ms;
    syncLegacyFromEvidence(&r);
    if (r.ev.state == HealthState::Unknown) { r.alive = hint_alive; r.latency_ms = hint_lat; }
    return *out = std::move(r), true;
}

bool parseRecordLegacy(int layout, const std::vector<std::string>& f, const HealthThresholds&,
                       ConfigHealthRecord* out) {
    const size_t need = layout == 3 ? 14 : (layout == 2 ? 12 : 11);
    if (f.size() < need) return false;
    ConfigHealthRecord r;
    r.uri = f[0];
    if (r.uri.empty() || r.uri.find("://") == std::string::npos) return false;
    r.tag = f[1].substr(0, 128);
    r.engine_used = f[2].substr(0, 128);
    long long iv;
    size_t shift = layout >= 2 ? 1 : 0;
    if (!pD(f[3], &r.first_seen) || !pD(f[4], &r.last_tested) || !pD(f[5], &r.last_alive_time)) return false;
    if (!pI(f[6], 0, 1, &iv)) return false;
    r.alive = iv != 0;
    if (shift) {
        if (!pI(f[7], 0, 1, &iv)) return false;
        r.telegram_only = iv != 0;
    }
    double lat;
    if (!pD(f[7 + shift], &lat, false) || lat > 1e9) return false;
    r.latency_ms = lat < 0 ? 0.0f : static_cast<float>(lat);
    if (!pI(f[8 + shift], 0, 2000000000LL, &iv)) return false;  // legacy consecutive_fails: hint, not evidence
    if (!pI(f[9 + shift], 0, 2000000000LL, &iv)) return false;
    r.total_tests = static_cast<int>(iv);
    if (!pI(f[10 + shift], 0, 2000000000LL, &iv)) return false;
    r.total_passes = static_cast<int>(iv);
    if (layout == 3) {
        if (!pI(f[11 + shift], -1, 1, &iv)) return false;
        r.gemini_status = static_cast<int>(iv);
        if (!pD(f[12 + shift], &r.gemini_checked_at)) return false;
    }
    r.consecutive_fails = 0;
    r.needs_retest = true;
    initRecordIdentity(&r);
    *out = std::move(r);
    return true;
}

}  // namespace hunter
