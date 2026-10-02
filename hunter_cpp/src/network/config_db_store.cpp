// ConfigDatabase persistence: TSV V4 (config DB) / V3 (live cache), legacy migration,
// atomic writes. Pure file logic; no network or process code.
#include "network/continuous_validator.h"
#include "core/db_format.h"
#include "core/utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace hunter {
namespace network {
namespace {

std::mutex& fileTxMutex() {  // serializes path-level save/migration transactions in-process
    static std::mutex m;
    return m;
}

#ifdef _WIN32
std::wstring widen(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    return w;
}
#endif

bool readWholeFile(const std::string& path, std::string* out) {
#ifdef _WIN32
    // MSVC accepts std::wstring here; portable form is std::filesystem::path
    std::ifstream ifs(std::filesystem::path(widen(path)), std::ios::binary);
#else
    std::ifstream ifs(path, std::ios::binary);
#endif
    if (!ifs) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad() || ss.bad()) return false;
    *out = ss.str();
    return true;
}

bool fileExists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return ::access(path.c_str(), F_OK) == 0;
#endif
}

std::string uniqueTempName(const std::string& path) {
    static std::atomic<unsigned> counter{0};
    std::ostringstream o;
#ifdef _WIN32
    o << path << ".tmp." << GetCurrentProcessId() << "." << counter++ << "." << GetTickCount64();
#else
    o << path << ".tmp." << getpid() << "." << counter++ << "." << std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    return o.str();
}

bool writeFileAtomic(const std::string& path, const std::string& data, std::string* err) {
    try { utils::mkdirRecursive(utils::dirName(path)); } catch (...) {}
    const std::string tmp = uniqueTempName(path);
#ifdef _WIN32
    FILE* f = _wfopen(widen(tmp).c_str(), L"wb");
#else
    FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (!f) { *err = "cannot open " + tmp; return false; }
    bool ok = true;
#ifndef _WIN32
    ok = (fchmod(fileno(f), 0600) == 0) && ok;  // restrictive permissions (Windows ACLs: follow-up)
#endif
    ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && ok;
    ok = (std::fflush(f) == 0) && ok;
#ifdef _WIN32
    ok = (_commit(_fileno(f)) == 0) && ok;
#else
    ok = (fsync(fileno(f)) == 0) && ok;
#endif
    ok = (std::fclose(f) == 0) && ok;
    auto drop = [&]() {
#ifdef _WIN32
        _wremove(widen(tmp).c_str());
#else
        std::remove(tmp.c_str());
#endif
    };
    if (!ok) { drop(); *err = "write failed: " + tmp; return false; }
#ifdef _WIN32
    if (!MoveFileExW(widen(tmp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        drop();
        *err = "rename failed: " + path;
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) { drop(); *err = "rename failed: " + path; return false; }
    // Make the rename itself durable.
    std::string dir = utils::dirName(path);
    if (dir.empty()) dir = ".";
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) { *err = "cannot open directory for fsync: " + dir; return false; }
    if (fsync(dfd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        ::close(dfd);
        *err = "directory fsync failed: " + dir;
        return false;
    }
    ::close(dfd);
#endif
    return true;
}

// Backup `data` next to `base`: reuse an existing backup only if it holds identical bytes,
// otherwise use the next free numbered name (base.1, base.2, ...). Never overwrites.
bool makeBackup(const std::string& base, const std::string& data, std::string* used, std::string* err) {
    for (int i = 0; i < 100; i++) {
        const std::string cand = i == 0 ? base : base + "." + std::to_string(i);
        if (!fileExists(cand)) {
            if (!writeFileAtomic(cand, data, err)) return false;
            *used = cand;
            return true;
        }
        std::string cur;
        if (readWholeFile(cand, &cur) && cur == data) { *used = cand; return true; }
    }
    *err = "no free backup name for " + base;
    return false;
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

// ── Single-writer guard: one exclusive lock file per data directory, held for the process
// lifetime. In-process instances share the process-level lock (they are serialized by
// fileTxMutex); a second process is refused and must run read-only. ──
std::mutex& dirLockRegistryMutex() { static std::mutex m; return m; }

bool acquireDirLock(const std::string& dir_in, std::string* err) {
    std::string dir = dir_in.empty() ? "." : dir_in;
#ifndef _WIN32
    char buf[PATH_MAX];
    if (::realpath(dir.c_str(), buf)) dir = buf;
#endif
    static std::map<std::string, intptr_t> held;
    std::lock_guard<std::mutex> g(dirLockRegistryMutex());
    if (held.count(dir)) return true;
    const std::string lp = dir + "/.hunter_db.lock";
#ifdef _WIN32
    HANDLE h = CreateFileW(widen(lp).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED ov = {};
    if (h == INVALID_HANDLE_VALUE ||
        !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        *err = "another Hunter instance owns data directory " + dir + " (read-only mode)";
        return false;
    }
    held[dir] = (intptr_t)h;
#else
    int fd = ::open(lp.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { *err = "cannot open lock file " + lp; return false; }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        *err = "another Hunter instance owns data directory " + dir + " (read-only mode)";
        return false;
    }
    held[dir] = fd;
#endif
    return true;
}

int countRejectedV4(const std::string& data, const HealthThresholds& th) {
    int bad = 0;
    auto lines = bodyLines(data);
    for (size_t i = 1; i < lines.size(); i++) {
        if (lines[i][0] == '#') continue;
        ConfigHealthRecord r;
        std::string e;
        if (!parseRecordV4(lines[i], th, &r, &e)) bad++;
    }
    return bad;
}

}  // namespace

bool ConfigDatabase::readOnly() const { std::lock_guard<std::mutex> l(mutex_); return read_only_; }
std::string ConfigDatabase::readOnlyReason() const { std::lock_guard<std::mutex> l(mutex_); return read_only_reason_; }
void ConfigDatabase::clearReadOnly() { std::lock_guard<std::mutex> l(mutex_); read_only_ = false; read_only_reason_.clear(); }
void ConfigDatabase::setReadOnlyLocked(const std::string& why) const {
    if (!read_only_) read_only_reason_ = why;
    read_only_ = true;
}

// Caller holds mutex_ and fileTxMutex().
bool ConfigDatabase::acquireWriteAccessLocked(const std::string& filepath, std::string* err) const {
    if (read_only_) { *err = "read-only mode: " + read_only_reason_; return false; }
    try { utils::mkdirRecursive(utils::dirName(filepath)); } catch (...) {}
    std::string d = utils::dirName(filepath);
    std::string lerr;
    if (!acquireDirLock(d, &lerr)) { setReadOnlyLocked(lerr); *err = "read-only mode: " + lerr; return false; }
    return true;
}

// Inspect the bytes currently at the destination before replacing them. Never destructive:
// unknown/different layouts are refused; anything the new file would not preserve is backed up first.
bool ConfigDatabase::checkDestinationLocked(const std::string& filepath, bool is_live, std::string* err) const {
    std::string existing;
    if (!fileExists(filepath)) return true;
    if (!readWholeFile(filepath, &existing)) {
        *err = "cannot read existing file; refusing to replace it";
        if (!is_live) setReadOnlyLocked(*err);
        return false;
    }
    if (existing.empty() || existing.find_first_not_of(" \t\r\n") == std::string::npos) return true;
    const char* prefix = is_live ? "#HUNTER_LIVE_CACHE_V" : "#HUNTER_CONFIG_DB_V";
    const int cur = is_live ? 3 : 4;
    const std::string first = firstLine(existing);
    const int v = headerVersion(first, prefix);
    auto fail = [&](const std::string& why) {
        *err = why;
        if (!is_live) setReadOnlyLocked(why);
        return false;
    };
    std::string used;
    if (v < 1 || v > cur) {
        if (first.compare(0, std::strlen(prefix) - 1, std::string(prefix).substr(0, std::strlen(prefix) - 1)) == 0)
            return fail("refusing to overwrite unsupported file version: " + first);
        // Unrecognized non-empty content: keep it before replacing.
        if (!makeBackup(filepath + ".unknown.bak", existing, &used, err)) return fail("backup of unrecognized file failed: " + *err);
        return true;
    }
    if (is_live && v < 3) return true;  // derived cache; older layouts are safely regenerated
    if (v < cur) {
        if (!makeBackup(filepath + ".v" + std::to_string(v) + ".bak", existing, &used, err))
            return fail("backup of old-format file failed: " + *err);
        return true;
    }
    // Current version: the column layout must be exactly ours.
    auto lines = bodyLines(existing);
    if (lines.empty() || lines[0] != v4ColumnsLine())
        return fail("refusing to overwrite file with a different column layout");
    if (!is_live && std::hash<std::string>{}(existing) != last_written_hash_ && countRejectedV4(existing, th_) > 0) {
        if (!makeBackup(filepath + ".v4.bak", existing, &used, err))
            return fail("rows that cannot be parsed could not be backed up: " + *err);
    }
    return true;
}

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
    std::lock_guard<std::mutex> txl(fileTxMutex());
    auto& err = const_cast<std::string&>(save_error_);
    err.clear();
    if (!acquireWriteAccessLocked(filepath, &err)) return -1;
    if (!checkDestinationLocked(filepath, false, &err)) return -1;

    const double now = clock_();
    std::vector<const ConfigHealthRecord*> recs;
    for (const auto& [k, rec] : db_) if (!rec.uri.empty()) recs.push_back(&rec);
    const std::string content = buildV4Content(kDbHeaderV4, recs, now, th_);
    if (!validateV4Content(content, kDbHeaderV4, recs.size(), th_, &err)) { err = "save refused: " + err; return -1; }
    if (!writeFileAtomic(filepath, content, &err)) return -1;
    last_written_hash_ = std::hash<std::string>{}(content);
    return static_cast<int>(recs.size());
}

int ConfigDatabase::saveLiveToDisk(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    std::lock_guard<std::mutex> txl(fileTxMutex());
    auto& err = const_cast<std::string&>(save_error_);
    err.clear();
    if (!acquireWriteAccessLocked(filepath, &err)) return -1;
    if (!checkDestinationLocked(filepath, true, &err)) return -1;
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
    if (!validateV4Content(content, kLiveHeaderV3, recs.size(), th_, &err)) { err = "save refused: " + err; return -1; }
    if (!writeFileAtomic(filepath, content, &err)) return -1;
    return static_cast<int>(recs.size());
}

int ConfigDatabase::loadFromDisk(const std::string& filepath) {
    // One transaction: DB mutex, then path mutex, THEN read - the snapshot cannot go stale
    // between reading and any rewrite (migration) that follows.
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> txl(fileTxMutex());
    LoadReport rep;
    std::string data;
    if (!readWholeFile(filepath, &data)) {
        rep.error = "cannot read " + filepath;
        load_report_ = rep;
        return 0;
    }
    {
        std::string lerr;
        if (!read_only_ && !acquireDirLock(utils::dirName(filepath), &lerr)) {
            setReadOnlyLocked(lerr);
        }
        if (read_only_) rep.warning = "read-only mode: " + read_only_reason_;
    }
    const std::string first = firstLine(data);
    const int ver = headerVersion(first, "#HUNTER_CONFIG_DB_V");
    if (ver < 1 || ver > 4) {
        rep.error = first.compare(0, 19, "#HUNTER_CONFIG_DB_V") == 0 ? "unsupported config DB version: " + first
                                                                      : "not a config DB file";
        if (first.compare(0, 19, "#HUNTER_CONFIG_DB_V") == 0) setReadOnlyLocked(rep.error);
        load_report_ = rep;
        return 0;
    }
    rep.source_version = ver;
    auto lines = bodyLines(data);
    size_t start = 0;
    std::vector<ConfigHealthRecord> parsed;

    if (ver == 4) {
        if (lines.empty() || lines[0] != v4ColumnsLine()) {
            rep.error = "V4 column header missing or different";
            setReadOnlyLocked(rep.error);
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
        if (rep.rejected > 0) {
            // A later save would drop the rejected rows: keep the current file bytes first.
            std::string err;
            if (!makeBackup(filepath + ".v4.bak", data, &rep.backup_path, &err)) {
                rep.error = "rejected rows not backed up: " + err;
                setReadOnlyLocked(rep.error);  // fail closed: no later save may drop them
            }
        }
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
    // The new file holds the union of everything in memory (never drops records added since).
    std::vector<const ConfigHealthRecord*> recs;
    for (const auto& [k, r] : db_) if (!r.uri.empty()) recs.push_back(&r);
    const double now = clock_();
    const std::string content = buildV4Content(kDbHeaderV4, recs, now, th_);
    std::string err;
    if (read_only_) {
        rep.error = "migration not written: read-only mode: " + read_only_reason_;
    } else if (rep.rejected > 0) {
        // Rewriting would silently drop the rejected rows: leave the original file untouched.
        rep.error = "migration not written: " + std::to_string(rep.rejected) +
                    " row(s) could not be parsed; original kept";
    } else if (!validateV4Content(content, kDbHeaderV4, recs.size(), th_, &err)) {
        rep.error = "migration not written: " + err;
    } else {
        std::string bak;
        if (!makeBackup(filepath + ".v" + std::to_string(ver) + ".bak", data, &bak, &err))
            rep.error = "migration not written (backup failed): " + err;
        else if (!writeFileAtomic(filepath, content, &err)) rep.error = "migration not written: " + err;
        else { rep.migrated = true; rep.backup_path = bak; last_written_hash_ = std::hash<std::string>{}(content); }
    }
    load_report_ = rep;
    return rep.loaded;
}

int ConfigDatabase::loadLiveFromDisk(const std::string& filepath) {
    LoadReport rep;
    std::string data;
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> txl(fileTxMutex());
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
