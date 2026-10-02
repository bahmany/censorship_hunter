// ConfigDatabase persistence: TSV V4 (config DB) / V3 (live cache), legacy migration,
// atomic writes. Pure file logic; no network or process code.
#include "network/continuous_validator.h"
#include "core/db_format.h"
#include "core/utils.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hunter {
namespace network {
namespace {

bool readWholeFile(const std::string& path, std::string* out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    *out = ss.str();
    return true;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err) {
    try { utils::mkdirRecursive(utils::dirName(path)); } catch (...) {}
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { *err = "cannot open " + tmp; return false; }
#ifndef _WIN32
    fchmod(fileno(f), 0600);
#endif
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fflush(f) == 0) && ok;
#ifdef _WIN32
    ok = (_commit(_fileno(f)) == 0) && ok;
#else
    ok = (fsync(fileno(f)) == 0) && ok;
#endif
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { std::remove(tmp.c_str()); *err = "write failed: " + tmp; return false; }
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(tmp.c_str());
        *err = "rename failed: " + path;
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        *err = "rename failed: " + path;
        return false;
    }
#endif
    return true;
}

std::string firstLine(const std::string& data) {
    size_t nl = data.find('\n');
    std::string l = data.substr(0, nl);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    return l;
}

// Parses "<prefix><N>" returning N, or -1 if the line does not start with the prefix.
int headerVersion(const std::string& line, const std::string& prefix) {
    if (line.compare(0, prefix.size(), prefix) != 0) return -1;
    std::string n = line.substr(prefix.size());
    if (n.empty() || n.size() > 6) return 0;
    for (char c : n) if (c < '0' || c > '9') return 0;
    return std::stoi(n);
}

// Iterate non-empty, non-comment lines after the first one (and after an optional
// "#COLUMNS" line, which is validated by the caller).
std::vector<std::string> bodyLines(const std::string& data) {
    std::vector<std::string> lines;
    size_t pos = data.find('\n');
    if (pos == std::string::npos) return lines;
    pos++;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string l = data.substr(pos, nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        pos = nl + 1;
        if (l.empty()) continue;
        lines.push_back(std::move(l));
    }
    return lines;
}

// Metadata merge for duplicate canonical keys (migration): earliest first_seen, latest hints,
// counts are the max of the two (not additive).
void mergeDuplicate(ConfigHealthRecord& into, const ConfigHealthRecord& r) {
    if (r.first_seen > 0 && (into.first_seen == 0 || r.first_seen < into.first_seen)) into.first_seen = r.first_seen;
    if (r.last_tested > into.last_tested) { into.last_tested = r.last_tested; into.alive = r.alive;
                                            into.latency_ms = r.latency_ms; into.engine_used = r.engine_used; }
    into.last_alive_time = std::max(into.last_alive_time, r.last_alive_time);
    into.total_tests = std::max(into.total_tests, r.total_tests);
    into.total_passes = std::max(into.total_passes, r.total_passes);
    if (r.gemini_checked_at > into.gemini_checked_at) {
        into.gemini_checked_at = r.gemini_checked_at;
        into.gemini_status = r.gemini_status;
    }
}

std::string buildV4Content(const char* header, const std::vector<const ConfigHealthRecord*>& recs,
                           double now, const HealthThresholds& th) {
    std::string out = header;
    out += "\n" + v4ColumnsLine() + "\n";
    for (const auto* r : recs) out += serializeRecordV4(*r, now, th) + "\n";
    return out;
}

// Strict full validation of a generated file (used before replacing anything on disk).
bool validateV4Content(const std::string& content, const char* header, size_t expect_rows,
                       const HealthThresholds& th, std::string* err) {
    if (firstLine(content) != header) { *err = "generated header mismatch"; return false; }
    auto lines = bodyLines(content);
    if (lines.empty() || lines[0] != v4ColumnsLine()) { *err = "generated columns mismatch"; return false; }
    if (lines.size() - 1 != expect_rows) { *err = "generated row count mismatch"; return false; }
    for (size_t i = 1; i < lines.size(); i++) {
        ConfigHealthRecord r;
        std::string e;
        if (!parseRecordV4(lines[i], th, &r, &e)) { *err = "generated row invalid: " + e; return false; }
    }
    return true;
}

}  // namespace

ConfigDatabase::LoadReport ConfigDatabase::lastLoadReport() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_report_;
}

std::string ConfigDatabase::lastSaveError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return save_error_;
}

int ConfigDatabase::saveToDisk(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    auto& err = const_cast<std::string&>(save_error_);
    err.clear();

    // Never clobber an unknown/newer file; keep a backup of any older format.
    std::string existing;
    if (readWholeFile(filepath, &existing)) {
        const std::string first = firstLine(existing);
        if (first.compare(0, 18, "#HUNTER_CONFIG_DB_") == 0) {
            int v = headerVersion(first, "#HUNTER_CONFIG_DB_V");
            if (v < 1 || v > 4) { err = "refusing to overwrite unsupported config DB version: " + first; return -1; }
            if (v < 4) {
                const std::string bak = filepath + ".v" + std::to_string(v) + ".bak";
                if (!fileExists(bak) && !writeFileAtomic(bak, existing, &err)) return -1;
            }
        }
    }

    const double now = clock_();
    std::vector<const ConfigHealthRecord*> recs;
    for (const auto& [k, rec] : db_) if (!rec.uri.empty()) recs.push_back(&rec);
    const std::string content = buildV4Content(kDbHeaderV4, recs, now, th_);
    if (!writeFileAtomic(filepath, content, &err)) return -1;
    return static_cast<int>(recs.size());
}

int ConfigDatabase::saveLiveToDisk(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    auto& err = const_cast<std::string&>(save_error_);
    err.clear();
    std::string existing;
    if (readWholeFile(filepath, &existing)) {
        const std::string first = firstLine(existing);
        if (first.compare(0, 20, "#HUNTER_LIVE_CACHE_V") == 0) {
            int v = headerVersion(first, "#HUNTER_LIVE_CACHE_V");
            if (v < 1 || v > 3) { err = "refusing to overwrite unsupported live cache version: " + first; return -1; }
        }
    }
    const double now = clock_();
    std::vector<const ConfigHealthRecord*> recs;
    for (const auto& [k, rec] : db_) {
        if (rec.uri.empty()) continue;
        if (!rec.alive) {
            // Keep recently-alive records only (3-day TTL); never-alive records are not cached.
            double anchor = std::max(rec.ev.last_full_success, rec.last_alive_time);
            if (anchor <= 0.0 || now - anchor > 259200.0) continue;
        }
        recs.push_back(&rec);
    }
    const std::string content = buildV4Content(kLiveHeaderV3, recs, now, th_);
    if (!writeFileAtomic(filepath, content, &err)) return -1;
    return static_cast<int>(recs.size());
}

int ConfigDatabase::loadFromDisk(const std::string& filepath) {
    LoadReport rep;
    std::string data;
    if (!readWholeFile(filepath, &data)) {
        std::lock_guard<std::mutex> lock(mutex_);
        rep.error = "cannot read " + filepath;
        load_report_ = rep;
        return 0;
    }
    const std::string first = firstLine(data);
    const int ver = headerVersion(first, "#HUNTER_CONFIG_DB_V");
    if (ver < 1 || ver > 4) {
        std::lock_guard<std::mutex> lock(mutex_);
        rep.error = first.compare(0, 19, "#HUNTER_CONFIG_DB_V") == 0 ? "unsupported config DB version: " + first
                                                                      : "not a config DB file";
        load_report_ = rep;
        return 0;
    }
    rep.source_version = ver;
    std::lock_guard<std::mutex> lock(mutex_);
    auto lines = bodyLines(data);
    size_t start = 0;
    std::vector<ConfigHealthRecord> parsed;

    if (ver == 4) {
        if (lines.empty() || lines[0] != v4ColumnsLine()) {
            rep.error = "V4 column header missing or different";
            load_report_ = rep;
            return 0;
        }
        start = 1;
        for (size_t i = start; i < lines.size(); i++) {
            if (lines[i][0] == '#') continue;
            ConfigHealthRecord r;
            std::string e;
            if (parseRecordV4(lines[i], th_, &r, &e)) parsed.push_back(std::move(r));
            else rep.rejected++;
        }
        for (auto& r : parsed) {
            if (db_.count(r.endpoint_key)) continue;
            if ((int)db_.size() >= max_size_) break;
            const std::string k = r.endpoint_key;
            db_[k] = std::move(r);
            rep.loaded++;
        }
        rep.ok = true;
        load_report_ = rep;
        return rep.loaded;
    }

    // ── Legacy V3/V2/V1: migrate ──
    const int layout = ver;  // 3 -> 14 cols, 2 -> 12, 1 -> 11
    std::map<std::string, ConfigHealthRecord> migrated;
    for (const auto& l : lines) {
        if (l[0] == '#') continue;
        ConfigHealthRecord r;
        if (l.size() > kMaxRowBytes || !parseRecordLegacy(layout, splitTabs(l), th_, &r)) { rep.rejected++; continue; }
        auto it = migrated.find(r.endpoint_key);
        if (it == migrated.end()) migrated.emplace(r.endpoint_key, std::move(r));
        else mergeDuplicate(it->second, r);
    }
    for (auto& [k, r] : migrated) {
        if (db_.count(k)) continue;
        if ((int)db_.size() >= max_size_) break;
        db_[k] = r;
        rep.loaded++;
    }
    rep.ok = true;

    // Rewrite as V4: validate the whole new file first, keep the original as .v<N>.bak.
    std::vector<const ConfigHealthRecord*> recs;
    for (const auto& [k, r] : migrated) recs.push_back(&r);
    const double now = clock_();
    const std::string content = buildV4Content(kDbHeaderV4, recs, now, th_);
    std::string err;
    if (!validateV4Content(content, kDbHeaderV4, recs.size(), th_, &err)) {
        rep.error = "migration not written: " + err;
    } else {
        const std::string bak = filepath + ".v" + std::to_string(ver) + ".bak";
        bool bak_ok = fileExists(bak) || writeFileAtomic(bak, data, &err);
        if (!bak_ok) rep.error = "migration not written (backup failed): " + err;
        else if (!writeFileAtomic(filepath, content, &err)) rep.error = "migration not written: " + err;
        else { rep.migrated = true; rep.backup_path = bak; }
    }
    load_report_ = rep;
    return rep.loaded;
}

int ConfigDatabase::loadLiveFromDisk(const std::string& filepath) {
    LoadReport rep;
    std::string data;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!readWholeFile(filepath, &data)) { rep.error = "cannot read " + filepath; load_report_ = rep; return 0; }
    const std::string first = firstLine(data);
    const int ver = headerVersion(first, "#HUNTER_LIVE_CACHE_V");
    if (ver < 1 || ver > 3) {
        rep.error = first.compare(0, 20, "#HUNTER_LIVE_CACHE_V") == 0 ? "unsupported live cache version: " + first
                                                                       : "not a live cache file";
        load_report_ = rep;
        return 0;
    }
    rep.source_version = ver;
    auto lines = bodyLines(data);
    size_t start = 0;
    if (ver == 3) {
        if (lines.empty() || lines[0] != v4ColumnsLine()) {
            rep.error = "live cache V3 column header missing or different";
            load_report_ = rep;
            return 0;
        }
        start = 1;
    }
    for (size_t i = start; i < lines.size(); i++) {
        if (lines[i][0] == '#') continue;
        ConfigHealthRecord r;
        bool ok;
        std::string e;
        if (ver == 3) ok = parseRecordV4(lines[i], th_, &r, &e);
        else ok = lines[i].size() <= kMaxRowBytes &&
                  parseRecordLegacy(ver == 2 ? 3 : 2, splitTabs(lines[i]), th_, &r);  // live V2 == 14 cols
        if (!ok) { rep.rejected++; continue; }
        auto it = db_.find(r.endpoint_key);
        if (it != db_.end()) {
            auto& cur = it->second;
            bool changed = false;
            // Evidence only moves forward in time; the cache can never resurrect health.
            if (ver == 3 && r.ev.last_attempt_at > cur.ev.last_attempt_at) {
                const std::string keep_tag = cur.tag;
                cur.ev = r.ev;
                cur.alive = r.alive; cur.latency_ms = r.latency_ms; cur.engine_used = r.engine_used;
                cur.last_tested = r.last_tested; cur.last_alive_time = r.last_alive_time;
                cur.total_tests = r.total_tests; cur.total_passes = r.total_passes;
                cur.consecutive_fails = r.consecutive_fails;
                cur.tag = keep_tag;
                changed = true;
            }
            if (ver == 3 && r.exit_country_at > cur.exit_country_at) {
                cur.exit_country = r.exit_country; cur.exit_ip = r.exit_ip;
                cur.exit_country_source = r.exit_country_source; cur.exit_country_at = r.exit_country_at;
                cur.network_generation = std::max(cur.network_generation, r.network_generation);
                changed = true;
            }
            if (ver == 3 && r.server_country_at > cur.server_country_at) {
                cur.server_country = r.server_country; cur.server_country_source = r.server_country_source;
                cur.server_country_at = r.server_country_at; cur.geo_db_version = r.geo_db_version;
                cur.server_ips = r.server_ips;
                changed = true;
            }
            if (r.gemini_checked_at > cur.gemini_checked_at) {
                cur.gemini_status = r.gemini_status;
                cur.gemini_checked_at = r.gemini_checked_at;
            }
            if (r.alive) { cur.needs_retest = true; changed = true; }
            if (changed) rep.loaded++;
            continue;
        }
        if ((int)db_.size() >= max_size_) break;
        r.needs_retest = true;
        const std::string k = r.endpoint_key;
        db_[k] = std::move(r);
        rep.loaded++;
    }
    rep.ok = true;
    load_report_ = rep;
    return rep.loaded;
}

}  // namespace network
}  // namespace hunter
