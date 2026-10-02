#include "proxy/proxy_server_manager.h"
#include "proxy/process_runner.h"
#include "proxy/xray_manager.h"
#include "network/uri_parser.h"
#include "network/traffic_probe.h"
#include "network/connectivity_baseline.h"
#include "network/continuous_validator.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/engine_embed.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hunter {
namespace proxy {

namespace {
bool isActiveState(SessionState s) {
    return s != SessionState::Stopped && s != SessionState::Failed && s != SessionState::Stopping;
}
ProxyStatus legacyStatus(SessionState s) {
    switch (s) {
        case SessionState::Connected: return ProxyStatus::Running;
        case SessionState::Starting: case SessionState::Degraded: case SessionState::Switching:
        case SessionState::Restarting: case SessionState::Paused: return ProxyStatus::Starting;
        case SessionState::Unavailable: case SessionState::Failed: return ProxyStatus::Error;
        default: return ProxyStatus::Stopped;
    }
}
}  // namespace

ProxyServerManager::ProxyServerManager() { init(); }
ProxyServerManager::ProxyServerManager(ManagerDeps deps, MonitorConfig cfg)
    : deps_(std::move(deps)), mcfg_(std::move(cfg)) { init(); }

void ProxyServerManager::init() {
    MonitorDeps& m = deps_.monitor;
    const bool production_launcher = !m.launcher;
    if (production_launcher) {
        resolveEnginePaths();
        launcher_ = std::make_shared<ManagedEngineLauncher>(xray_path_, singbox_path_, "");
        m.launcher = launcher_;
        if (!m.pid_of) m.pid_of = [](const std::shared_ptr<void>& g) { return ManagedEngineLauncher::pidOf(g); };
    } else {
        launcher_ = m.launcher;
    }
    if (!m.leases) m.leases = network::PortLeaseRegistry::global();
    if (!m.executor) m.executor = nullptr;   // per-session pool is created in startProxy
    if (!m.probe) {
        auto probe = std::make_shared<network::TrafficProbe>(network::makeCurlTransport());
        auto counter = std::make_shared<std::atomic<unsigned long>>(0);
        m.probe = [probe, counter](int port, bool bulk, const std::string& key, const std::string& engine) {
            using namespace network;
            auto base = ConnectivityBaseline::shared();
            RawProbe raw = probe->run(port, bulk);
            const uint64_t g = base->generation();
            BaselineSnapshot snap;
            const bool full = raw.bothPass() && !(raw.bulk_run && raw.bulk.status != CheckStatus::Pass);
            if (!full) snap = base->current(ConnectivityBaseline::kFailureRecheckSeconds);
            if (full) base->controls().record(key, kCheckA | kCheckB, raw.finished_at, g);
            Attribution at = classifyRound(raw, snap, &base->controls(), key, g);
            return buildProbeResult(raw, at, key, engine, "live-" + std::to_string(counter->fetch_add(1)), g);
        };
    }
    if (!m.baseline) {
        m.baseline = []() {
            auto base = network::ConnectivityBaseline::shared();
            if (!base->enabled()) return network::BaselineState::Indeterminate;
            return base->current(30.0).state;
        };
    }
    if (!m.key_for) m.key_for = [](const std::string& u) { return network::ConfigDatabase::keyFor(u); };
    if (!m.build_config) m.build_config = [this](const std::string& u, int port) { return buildEngineConfig(u, port); };
    if (!m.candidates) {
        m.candidates = [this]() -> std::vector<Candidate> {
            std::function<std::vector<Candidate>()> fn;
            { std::lock_guard<std::mutex> lk(mutex_); fn = candidate_fn_; }
            if (fn) return fn();
            network::ConfigDatabase* db = db_.load();
            std::vector<Candidate> out;
            if (!db) return out;
            std::set<std::string> seen;
            for (const auto& r : db->getRecommendedRecords(20)) {
                if (r.telegram_only || !seen.insert(r.endpoint_key).second) continue;
                out.push_back({r.uri, r.endpoint_key, true, db->evaluate(r).score});
            }
            for (const auto& r : db->getHealthyRecords(50)) {
                if (r.telegram_only || seen.count(r.endpoint_key)) continue;
                HealthEvaluation ev = db->evaluate(r);
                if (!ev.switch_eligible) continue;      // Healthy and recently confirmed only
                seen.insert(r.endpoint_key);
                out.push_back({r.uri, r.endpoint_key, false, ev.score});
            }
            return out;
        };
    }
    if (!m.sink) {
        m.sink = [this](const ProbeResult& r) {
            network::ConfigDatabase* db = db_.load();
            if (db) db->applyProbeResult(r);
        };
    }
    if (!deps_.port_free) deps_.port_free = [](int p) { return utils::isPortFree(p); };
    if (deps_.start_watchdog) watchdog_ = std::thread([this] { watchdogLoop(); });
}

ProxyServerManager::~ProxyServerManager() {
    stopAll();
    waitAllStopped(15.0);
    { std::lock_guard<std::mutex> lk(wd_mu_); wd_stop_ = true; }
    wd_cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();
}

void ProxyServerManager::watchdogLoop() {
    std::unique_lock<std::mutex> lk(wd_mu_);
    while (!wd_stop_) {
        wd_cv_.wait_for(lk, std::chrono::milliseconds((int)(mcfg_.process_check_s * 1000)), [&] { return wd_stop_; });
        if (wd_stop_) break;
        lk.unlock();
        tickAll();
        lk.lock();
    }
}

void ProxyServerManager::tickAll() {
    std::vector<std::shared_ptr<HealthMonitor>> mons;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) mons.push_back(s.mon);
        for (auto& s : retired_) mons.push_back(s.mon);
        pruneRetiredLocked();
    }
    for (auto& m : mons) m->step();   // non-blocking: heavy work runs on the session executor
}

void ProxyServerManager::pruneRetiredLocked() {
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [](const Session& s) { return s.mon->snapshot().state == SessionState::Stopped; }), retired_.end());
}

void ProxyServerManager::resolveEnginePaths() {
    if (paths_resolved_) return;
    paths_resolved_ = true;

    // Environment overrides win over embedded copies (documented priority), then the embedded
    // single-file engines, then the classic bin/ lookup.
    const char* env_xray = std::getenv("HUNTER_XRAY_PATH");
    const char* env_singbox = std::getenv("HUNTER_SINGBOX_PATH");
    if (env_xray && *env_xray && utils::fileExists(env_xray)) xray_path_ = env_xray;
    if (env_singbox && *env_singbox && utils::fileExists(env_singbox)) singbox_path_ = env_singbox;

    if (xray_path_.empty() || singbox_path_.empty()) {
        embed::ensureExtracted();
        std::string ex = embed::xrayPath();
        std::string es = embed::singBoxPath();
        if (xray_path_.empty() && !ex.empty() && utils::fileExists(ex)) xray_path_ = ex;
        if (singbox_path_.empty() && !es.empty() && utils::fileExists(es)) singbox_path_ = es;
    }
#ifdef _WIN32
    if (xray_path_.empty()) {
        for (const char* c : {"bin/xray.exe", "xray.exe"}) if (utils::fileExists(c)) { xray_path_ = c; break; }
    }
    if (singbox_path_.empty()) {
        for (const char* c : {"bin/sing-box.exe", "sing-box.exe"}) if (utils::fileExists(c)) { singbox_path_ = c; break; }
    }
#else
    if (xray_path_.empty()) {
        for (const char* c : {"bin/xray", "xray", "./xray", "/app/bin/xray"}) if (utils::fileExists(c)) { xray_path_ = c; break; }
    }
    if (singbox_path_.empty()) {
        for (const char* c : {"bin/sing-box", "sing-box", "./sing-box", "/app/bin/sing-box"}) if (utils::fileExists(c)) { singbox_path_ = c; break; }
    }
#endif
    // Unresolved stays empty: the launcher reports BinaryMissing instead of exec'ing a bogus path.
}

BuiltConfig ProxyServerManager::buildEngineConfig(const std::string& uri, int port) {
    BuiltConfig out;
    auto parsed = network::UriParser::parse(uri);
    if (!parsed.has_value() || !parsed->isValid()) { out.error = "invalid or unsupported config URI"; return out; }
    ParsedConfig config = parsed.value();
    const bool quic = config.protocol == "hysteria2" || config.protocol == "tuic";
    const bool insecure = config.insecureTls();   // modern xray cannot honour allowInsecure
    auto avail = [&](const char* e) { return launcher_ && launcher_->available(e); };

    std::string xray_cfg = (!quic && !insecure) ? XRayManager::generateTestConfig(config, port) : std::string();
    std::string sb_cfg = config.toSingBoxConfigJson(port);
    if (!xray_cfg.empty() && (avail("xray") || sb_cfg.empty())) {
        out.ok = true; out.engine = "xray"; out.config_text = xray_cfg; return out;
    }
    if (!sb_cfg.empty()) { out.ok = true; out.engine = "sing-box"; out.config_text = sb_cfg; return out; }
    std::string why = config.unsupportedReason(quic || insecure ? "sing-box" : "xray");
    out.error = why.empty() ? "config cannot be expressed by any installed engine" : "unsupported: " + why;
    return out;
}

// ── lifecycle ───────────────────────────────────────────────────────────
bool ProxyServerManager::occupies(const Session& s) {
    SessionState st = s.mon->snapshot().state;
    return st != SessionState::Stopped && st != SessionState::Failed;   // Failed/Error releases its port
}

int ProxyServerManager::findFreePortLocked() const {
    for (int port = deps_.port_first; port <= deps_.port_last; port++) {
        bool used = false;
        for (const auto& [u, s] : sessions_) if (s.port == port && occupies(s)) { used = true; break; }
        if (!used) for (const auto& s : retired_) if (s.port == port && occupies(s)) { used = true; break; }
        if (used) continue;
        if (deps_.port_free(port)) return port;
    }
    return 0;
}

int ProxyServerManager::startProxy(const std::string& uri, SwitchMode mode) {
    std::shared_ptr<HealthMonitor> mon;
    int port = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(uri);
        if (it != sessions_.end()) {
            SessionState st = it->second.mon->snapshot().state;
            if (isActiveState(st)) return it->second.port;   // already running / starting
            retired_.push_back(std::move(it->second));       // keep until its teardown finished
            sessions_.erase(it);
        }
        port = findFreePortLocked();
        if (port == 0) {
            errors_[uri] = "No free ports in range " + std::to_string(deps_.port_first) + "-" + std::to_string(deps_.port_last);
            utils::LogRingBuffer::instance().push("[ProxyServer] " + errors_[uri]);
            return 0;
        }
        errors_.erase(uri);
        MonitorDeps md = deps_.monitor;
        if (!md.executor) md.executor = std::make_shared<ThreadPoolExecutor>(2);
        mon = HealthMonitor::create(mcfg_, md);
        Session s; s.mon = mon; s.uri = uri; s.port = port;
        sessions_[uri] = std::move(s);
        mon->setEventCallback([this, uri](const SessionEvent& e) {
            SessionEventCallback cb;
            { std::lock_guard<std::mutex> lk(mutex_); cb = event_cb_; }
            if (e.kind == EventKind::StateChanged || e.kind == EventKind::FailoverSucceeded ||
                e.kind == EventKind::FailoverFailed || e.kind == EventKind::EngineExited)
                utils::LogRingBuffer::instance().push("[ProxyServer] " + std::string(eventKindName(e.kind)) + " " +
                    sessionStateName(e.snapshot.state) + (e.detail.empty() ? "" : ": " + e.detail));
            if (cb) cb(uri, e);
        });
    }
    mon->start(uri, port, mode);   // non-blocking: launch + first probe run on the session executor
    return port;
}

bool ProxyServerManager::stopProxy(const std::string& uri) {
    std::shared_ptr<HealthMonitor> mon;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(uri);
        if (it == sessions_.end()) return false;
        mon = it->second.mon;
    }
    SessionState st = mon->snapshot().state;
    if (st == SessionState::Stopped || st == SessionState::Stopping) return false;
    mon->stop();
    return true;
}

bool ProxyServerManager::stopProxyByPort(int port) {
    std::string uri;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) if (s.port == port && occupies(s)) { uri = u; break; }
    }
    return !uri.empty() && stopProxy(uri);
}

void ProxyServerManager::stopAll() {
    std::vector<std::shared_ptr<HealthMonitor>> mons;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) mons.push_back(s.mon);
    }
    for (auto& m : mons) m->stop();
}

bool ProxyServerManager::waitAllStopped(double timeout_s) {
    std::vector<std::shared_ptr<HealthMonitor>> mons;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) mons.push_back(s.mon);
        for (auto& s : retired_) mons.push_back(s.mon);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    bool ok = true;
    for (auto& m : mons) {
        double left = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (!m->waitStopped(std::max(0.0, left))) ok = false;
    }
    return ok;
}

void ProxyServerManager::setEventCallback(SessionEventCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    event_cb_ = std::move(cb);
}
void ProxyServerManager::attachDatabase(network::ConfigDatabase* db) { db_.store(db); }
void ProxyServerManager::setCandidateProvider(std::function<std::vector<Candidate>()> fn) {
    std::lock_guard<std::mutex> lk(mutex_);
    candidate_fn_ = std::move(fn);
}

// ── queries ─────────────────────────────────────────────────────────────
bool ProxyServerManager::snapshot(const std::string& uri, SessionSnapshot* out) const {
    std::shared_ptr<HealthMonitor> mon;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(uri);
        if (it == sessions_.end()) return false;
        mon = it->second.mon;
    }
    if (out) *out = mon->snapshot();
    return true;
}

std::vector<std::pair<std::string, SessionSnapshot>> ProxyServerManager::snapshots() const {
    std::vector<std::pair<std::string, std::shared_ptr<HealthMonitor>>> mons;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) mons.emplace_back(u, s.mon);
    }
    std::vector<std::pair<std::string, SessionSnapshot>> out;
    for (auto& [u, m] : mons) out.emplace_back(u, m->snapshot());
    return out;
}

bool ProxyServerManager::setPinned(const std::string& uri, bool pinned) {
    std::shared_ptr<HealthMonitor> mon;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(uri);
        if (it == sessions_.end()) return false;
        mon = it->second.mon;
    }
    mon->setMode(pinned ? SwitchMode::Pinned : SwitchMode::Auto);
    return true;
}

bool ProxyServerManager::isRunning(const std::string& uri) const {
    SessionSnapshot s;
    return snapshot(uri, &s) && s.state == SessionState::Connected;
}
int ProxyServerManager::getPort(const std::string& uri) const {
    SessionSnapshot s;
    return snapshot(uri, &s) && isActiveState(s.state) ? s.user_port : 0;
}
ProxyStatus ProxyServerManager::getStatus(const std::string& uri) const {
    SessionSnapshot s;
    if (!snapshot(uri, &s)) return ProxyStatus::Stopped;
    return legacyStatus(s.state);
}
std::string ProxyServerManager::getError(const std::string& uri) const {
    SessionSnapshot s;
    if (snapshot(uri, &s)) {
        if (s.state == SessionState::Failed || s.state == SessionState::Unavailable) return s.reason;
        return "";
    }
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = errors_.find(uri);
    return it == errors_.end() ? std::string() : it->second;
}

ProxyInstance ProxyServerManager::toInstance(const Session& s, const SessionSnapshot& snap) const {
    ProxyInstance i;
    i.uri = s.uri;
    i.port = isActiveState(snap.state) ? snap.user_port : 0;
    i.engine = snap.engine;
    i.status = legacyStatus(snap.state);
    if (i.status == ProxyStatus::Error) i.error_message = snap.reason;
    i.pid = snap.engine_pid;
    i.started_at = snap.state_since_utc;
    i.bytes_in = s.bytes_in; i.bytes_out = s.bytes_out;
    i.last_rchar = s.last_r; i.last_wchar = s.last_w; i.last_traffic_poll = s.last_poll;
    return i;
}

std::vector<ProxyInstance> ProxyServerManager::getInstances() const {
    std::vector<std::pair<Session, std::shared_ptr<HealthMonitor>>> v;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) v.emplace_back(s, s.mon);
    }
    std::vector<ProxyInstance> out;
    for (auto& [s, m] : v) out.push_back(toInstance(s, m->snapshot()));
    return out;
}

std::pair<unsigned long long, unsigned long long> ProxyServerManager::getTraffic(const std::string& uri) const {
    SessionSnapshot snap;
    if (!snapshot(uri, &snap) || snap.state != SessionState::Connected) return {0, 0};
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(uri);
    return it == sessions_.end() ? std::make_pair(0ULL, 0ULL) : std::make_pair(it->second.bytes_in, it->second.bytes_out);
}

// Informational traffic counters (never feeds health decisions).
void ProxyServerManager::poll() {
    std::vector<std::pair<std::string, std::shared_ptr<HealthMonitor>>> mons;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [u, s] : sessions_) mons.emplace_back(u, s.mon);
    }
    const double now = utils::nowTimestamp();
    for (auto& [u, m] : mons) {
        SessionSnapshot snap = m->snapshot();
        if (snap.state != SessionState::Connected || snap.engine_pid <= 0) continue;
        auto io = readProcessIo(snap.engine_pid);
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = sessions_.find(u);
        if (it == sessions_.end()) continue;
        Session& s = it->second;
        if (now - s.last_poll < 2.0) continue;
        if (s.last_r == 0 && s.last_w == 0) { s.last_r = io.first; s.last_w = io.second; }
        else {
            s.bytes_in += io.first > s.last_r ? io.first - s.last_r : 0;
            s.bytes_out += io.second > s.last_w ? io.second - s.last_w : 0;
            s.last_r = io.first; s.last_w = io.second;
        }
        s.last_poll = now;
    }
}

std::pair<unsigned long long, unsigned long long> ProxyServerManager::readProcessIo(int pid) const {
    if (pid <= 0) return {0, 0};
#ifdef __linux__
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/io");
    if (!ifs.is_open()) return {0, 0};
    unsigned long long rchar = 0, wchar = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        try {
            if (line.compare(0, 6, "rchar:") == 0) rchar = std::stoull(line.substr(6));
            else if (line.compare(0, 6, "wchar:") == 0) wchar = std::stoull(line.substr(6));
        } catch (...) {}
    }
    return {rchar, wchar};
#else
    return {0, 0};
#endif
}

}  // namespace proxy
}  // namespace hunter
