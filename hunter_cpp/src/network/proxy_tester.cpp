#include "network/proxy_tester.h"
#include "network/uri_parser.h"
#include "core/endpoint_key.h"
#include "core/utils.h"
#include "core/win_compat.h"
#include "core/engine_embed.h"
#include "proxy/xray_manager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// Helper: log to both stdout and ring buffer for dashboard
#define TLOG(msg) do { \
    std::ostringstream _tlog_ss; \
    _tlog_ss << msg; \
    std::cerr << _tlog_ss.str() << std::endl; \
    utils::LogRingBuffer::instance().push(_tlog_ss.str()); \
} while(0)

namespace hunter {
namespace network {

// Limit concurrent XRay processes to prevent resource exhaustion
static std::atomic<int> s_active_tests{0};
static std::atomic<int> s_peak_tests{0};

// RAII guard for s_active_tests — ensures the counter is ALWAYS decremented,
// even if an exception is thrown or an early return is taken. Without this,
// a single leaked increment causes a death spiral: once s_active_tests reaches
// the max, all new tests block forever in the spin-wait loop.
struct ActiveTestGuard {
    ActiveTestGuard() {
        int active = ++s_active_tests;
        int peak = s_peak_tests.load();
        while (active > peak && !s_peak_tests.compare_exchange_weak(peak, active)) {}
    }
    ~ActiveTestGuard() {
        s_active_tests.fetch_sub(1);
    }
    ActiveTestGuard(const ActiveTestGuard&) = delete;
    ActiveTestGuard& operator=(const ActiveTestGuard&) = delete;
};

// ─── Bounded parallelism for batch tests ───
// batchTestWithXray previously spawned one std::async thread per config
// (up to 50+ at once). Each thread runs libcurl downloads that can take
// 5-30 seconds. Under load this creates hundreds of threads, each with
// an 8MB stack, exhausting memory and causing the 1-hour hang. The
// BatchSemaphore limits concurrent in-batch tests to a sane number
// derived from hardware concurrency, preventing thread explosion.
class BatchSemaphore {
public:
    explicit BatchSemaphore(int count) : count_(count) {}
    void acquire() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return count_ > 0; });
        count_--;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mu_);
        count_++;
        cv_.notify_one();
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    int count_;
};

struct BatchSlotGuard {
    BatchSemaphore& sem;
    explicit BatchSlotGuard(BatchSemaphore& s) : sem(s) { sem.acquire(); }
    ~BatchSlotGuard() { sem.release(); }
    BatchSlotGuard(const BatchSlotGuard&) = delete;
    BatchSlotGuard& operator=(const BatchSlotGuard&) = delete;
};

static int getEnvIntClamped(const char* name, int fallback, int min_value, int max_value) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
        int value = std::stoi(raw);
        if (value < min_value) value = min_value;
        if (value > max_value) value = max_value;
        return value;
    } catch (...) {
        return fallback;
    }
}

static int getMaxConcurrentTests() {
    static int max_tests = 0;
    if (max_tests == 0) {
        const int forced = getEnvIntClamped("HUNTER_MAX_CONCURRENT_TEST_PROCESSES", 0, 1, 128);
        if (forced > 0) {
            max_tests = forced;
        } else {
            // Use 20% of CPU cores for concurrent test processes.
            // Each xray/sing-box test process is CPU-light (mostly I/O wait),
            // but spawning too many causes RAM spikes and context-switch overhead.
            int cpus = static_cast<int>(std::thread::hardware_concurrency());
            if (cpus <= 0) cpus = 4;
            int budget = std::max(4, cpus / 5);  // 20% of cores
            max_tests = std::min(32, std::max(4, budget));
        }
    }
    return max_tests;
}

static bool useTcpPreScreen() {
    // Default OFF: in censored environments (Iran, China, etc.), ISPs use
    // DPI to block raw TCP connections to proxy server ports. A server
    // that fails a raw TCP connect can still work through a proxy engine
    // like xray, which uses TLS/obfuscation to bypass DPI. Enabling this
    // would filter out ALL configs as "dead" in censored environments.
    // Set HUNTER_ENABLE_TCP_PRESCREEN=1 to enable (useful in uncensored
    // environments where most servers are genuinely offline).
    static int enabled = getEnvIntClamped("HUNTER_ENABLE_TCP_PRESCREEN", 0, 0, 1);
    return enabled == 1;
}

static uint32_t fingerprintText(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

static std::string summarizeConfigForLog(const std::string& config_uri) {
    std::string label = "uri";
    auto parsed = UriParser::parse(config_uri);
    if (parsed.has_value() && parsed->isValid() && !parsed->protocol.empty()) {
        label = parsed->protocol;
    }
    std::ostringstream oss;
    oss << label << "#" << std::hex << std::uppercase << fingerprintText(config_uri);
    return oss.str();
}

ProxyTester::ProxyTester() {
    // Priority 1: Embedded or externally-placed engines (single-file build / Android).
    embed::ensureExtracted();
    {
        std::string ex = embed::xrayPath();
        std::string es = embed::singBoxPath();
        if (!ex.empty() && utils::fileExists(ex)) xray_path_ = ex;
        if (!es.empty() && utils::fileExists(es)) singbox_path_ = es;
    }

    // Priority 2: env overrides (for xray/sing-box not yet set, and always for mihomo).
    const char* env_xray = std::getenv("HUNTER_XRAY_PATH");
    const char* env_singbox = std::getenv("HUNTER_SINGBOX_PATH");
    const char* env_mihomo = std::getenv("HUNTER_MIHOMO_PATH");
    if (xray_path_.empty() && env_xray && *env_xray && utils::fileExists(env_xray))
        xray_path_ = env_xray;
    if (singbox_path_.empty() && env_singbox && *env_singbox && utils::fileExists(env_singbox))
        singbox_path_ = env_singbox;

    // Priority 3: classic bin/ lookup for anything not yet resolved.
    auto resolvePath = [](const char* env_val,
                          std::initializer_list<const char*> candidates) -> std::string {
        if (env_val && *env_val && utils::fileExists(env_val)) return env_val;
        for (const char* c : candidates) {
            if (utils::fileExists(c)) return c;
        }
        return candidates.begin()[0];
    };

#ifdef _WIN32
    if (xray_path_.empty()) {
        xray_path_ = "bin/xray.exe";
        if (!utils::fileExists(xray_path_) && utils::fileExists("xray.exe")) xray_path_ = "xray.exe";
    }
    if (singbox_path_.empty()) {
        singbox_path_ = "bin/sing-box.exe";
        if (!utils::fileExists(singbox_path_) && utils::fileExists("sing-box.exe")) singbox_path_ = "sing-box.exe";
    }
    mihomo_path_ = resolvePath(env_mihomo, {"bin/mihomo.exe", "mihomo.exe", "./mihomo.exe"});
#else
    if (xray_path_.empty())
        xray_path_    = resolvePath(env_xray,    {"bin/xray",    "xray",    "./xray",    "/app/bin/xray"});
    if (singbox_path_.empty())
        singbox_path_ = resolvePath(env_singbox, {"bin/sing-box", "sing-box", "./sing-box", "/app/bin/sing-box"});
    mihomo_path_  = resolvePath(env_mihomo,  {"bin/mihomo",   "mihomo",   "./mihomo",   "/app/bin/mihomo"});
#endif

    // Log resolved engine paths once per process (ProxyTester may be
    // constructed multiple times as a local variable in validator/bench).
    {
        static std::once_flag once;
        std::call_once(once, [this]() {
            std::ostringstream ss;
            ss << "[ProxyTester] Engine paths: xray=" << xray_path_
               << "(" << (utils::fileExists(xray_path_) ? "OK" : "MISSING") << ")"
               << " sing-box=" << singbox_path_
               << "(" << (utils::fileExists(singbox_path_) ? "OK" : "MISSING") << ")"
               << " mihomo=" << mihomo_path_
               << "(" << (utils::fileExists(mihomo_path_) ? "OK" : "MISSING") << ")";
            utils::LogRingBuffer::instance().push(ss.str());
        });
    }
}

ProxyTester::~ProxyTester() {}

int ProxyTester::activeTestCount() {
    return s_active_tests.load();
}

int ProxyTester::peakTestCount() {
    return s_peak_tests.load();
}

int ProxyTester::maxConcurrentTestCount() {
    return getMaxConcurrentTests();
}


// ═══════════════════════════════════════════════════════════════════
// Setters / dependency wiring
// ═══════════════════════════════════════════════════════════════════

bool ProxyTester::needsTcpPrescreen(const std::string& protocol) {
    // QUIC (UDP) upstreams: a raw TCP connect says nothing about reachability.
    return !(protocol == "hysteria2" || protocol == "hy2" || protocol == "tuic");
}

void ProxyTester::setXrayPath(const std::string& path) { xray_path_ = path; if (!custom_launcher_) launcher_.reset(); }
void ProxyTester::setSingBoxPath(const std::string& path) { singbox_path_ = path; if (!custom_launcher_) launcher_.reset(); }
void ProxyTester::setMihomoPath(const std::string& path) { mihomo_path_ = path; if (!custom_launcher_) launcher_.reset(); }
void ProxyTester::setLauncher(std::shared_ptr<EngineLauncher> l) { launcher_ = std::move(l); custom_launcher_ = launcher_ != nullptr; }

void ProxyTester::ensureDeps() {
    if (!launcher_) launcher_ = std::make_shared<ProcessEngineLauncher>(xray_path_, singbox_path_, mihomo_path_);
    if (!probe_) probe_ = std::make_shared<TrafficProbe>();
    if (!baseline_) baseline_ = ConnectivityBaseline::shared();
    if (!leases_) leases_ = PortLeaseRegistry::global();
    if (!clock_) clock_ = systemClock();
}

// ═══════════════════════════════════════════════════════════════════
// testBatch implementation
// ═══════════════════════════════════════════════════════════════════

struct ProxyTester::Impl {
    struct Item {
        size_t idx = 0;
        std::string uri;
        ParsedConfig cfg;
        std::string key;
        std::string engine;               // chosen engine
        RawProbe raw;
        bool probed = false;
        bool has_terminal = false;        // engine/config level outcome (no traffic evidence)
        ProbeOutcome terminal = ProbeOutcome::Indeterminate;
        std::string detail;
        double t_start = 0.0, t_end = 0.0;
    };

    ProxyTester& t;
    BatchTestOptions opts;
    std::string run_prefix;
    std::vector<Item> items;
    std::atomic<int> launches_left;
    std::atomic<int> ok_launches{0};
    int startup_ms = 10000;

    Impl(ProxyTester& tester, const BatchTestOptions& o) : t(tester), opts(o), launches_left(o.max_launches) {
        startup_ms = std::max(5000, std::min(15000, o.timeout_seconds * 1000));
        run_prefix = o.run_prefix;
        if (run_prefix.empty()) {
            static std::atomic<uint32_t> seq{0};
            std::ostringstream ss;
            ss << "r" << std::hex << utils::nowMs() << "-" << seq.fetch_add(1);
            run_prefix = ss.str();
        }
    }

    void terminalFail(Item& it, ProbeOutcome o, const std::string& why) {
        it.has_terminal = true;
        it.terminal = o;
        it.detail = why;
        it.t_end = t.clock_();
    }

    // ── engine selection ──
    std::string chooseEngine(Item& it, std::string* why) {
        const bool quic = it.cfg.protocol == "hysteria2" || it.cfg.protocol == "tuic";
        auto avail = [&](const char* e) { return t.launcher_->available(e); };
        const bool xray_ok = !quic && !it.cfg.toXrayOutboundJson(0).empty();
        const bool sb_ok = !it.cfg.toSingBoxConfigJson(0).empty();
        const bool mh_ok = !it.cfg.toMihomoConfigYaml(0).empty();
        if (!xray_ok && !sb_ok && !mh_ok) { *why = "config not expressible by any engine"; return ""; }
        if (xray_ok && avail("xray")) return "xray";
        if (sb_ok && avail("sing-box")) return "sing-box";
        if (mh_ok && avail("mihomo")) return "mihomo";
        *why = quic ? "sing-box (or mihomo) engine required for QUIC protocols is not installed"
                    : "no installed engine can run this config";
        return "";
    }

    // ── probing ──
    void probeGroup(const std::vector<Item*>& group, const std::vector<int>& ports) {
        int conc = std::max(4, std::min(32, utils::getCpuCount() * 2));
        const char* env = std::getenv("HUNTER_BATCH_CONCURRENCY");
        if (env && *env) { try { conc = std::max(1, std::min(64, std::stoi(env))); } catch (...) {} }
        BatchSemaphore sem(conc);
        std::vector<std::future<void>> fs;
        for (size_t i = 0; i < group.size(); i++) {
            Item* it = group[i];
            int port = ports[i];
            fs.push_back(std::async(std::launch::async, [this, it, port, &sem]() {
                BatchSlotGuard slot(sem);
                try {
                    it->raw = t.probe_->run(port, opts.bulk);
                    it->probed = true;
                } catch (const std::exception& e) {
                    terminalFail(*it, ProbeOutcome::EngineError, std::string("probe exception: ") + e.what());
                } catch (...) {
                    terminalFail(*it, ProbeOutcome::EngineError, "probe exception");
                }
            }));
        }
        for (auto& f : fs) { try { f.get(); } catch (...) {} }
    }

    // ── xray shared process with bisect ──
    void runXrayGroup(std::vector<Item*> g) {
        if (g.empty()) return;
        for (int attempt = 0; attempt < std::max(1, opts.bind_retries); attempt++) {
            if (launches_left.fetch_sub(1) <= 0) {
                for (auto* it : g) terminalFail(*it, ProbeOutcome::EngineError, "launch budget exhausted");
                return;
            }
            std::vector<PortLease> leases = t.leases_->acquireMany(g.size());
            if (leases.size() < g.size()) {
                for (auto* it : g) terminalFail(*it, ProbeOutcome::EngineError, "no free local ports");
                return;
            }
            std::vector<std::pair<ParsedConfig, int>> entries;
            std::vector<int> ports;
            for (size_t i = 0; i < g.size(); i++) {
                entries.emplace_back(g[i]->cfg, leases[i].port());
                ports.push_back(leases[i].port());
            }
            LaunchRequest req;
            req.engine = "xray";
            req.config_text = proxy::XRayManager::generateBatchSpeedtestConfig(entries);
            req.ports = ports;
            req.startup_timeout_ms = startup_ms;
            if (req.config_text.empty()) {
                for (auto* it : g) terminalFail(*it, ProbeOutcome::Unsupported, "xray config generation failed");
                return;
            }
            for (auto& l : leases) l.releaseSocket();   // hand the ports to the engine
            double t0 = t.clock_();
            for (auto* it : g) it->t_start = t0;
            ActiveTestGuard active;
            LaunchResult lr = t.launcher_->launch(req);
            switch (lr.status) {
                case LaunchStatus::Ok:
                    ok_launches++;
                    probeGroup(g, ports);
                    return;   // lr.guard + leases released here (engine stopped, ports freed)
                case LaunchStatus::BindConflict:
                    continue;  // fresh random ports, no penalty
                case LaunchStatus::BinaryMissing:
                    for (auto* it : g) terminalFail(*it, ProbeOutcome::Unsupported, "xray binary missing");
                    return;
                case LaunchStatus::Error:
                    for (auto* it : g) terminalFail(*it, ProbeOutcome::EngineError, "xray launch error: " + lr.detail);
                    return;
                case LaunchStatus::StartupFailed:
                    if (g.size() == 1) {
                        terminalFail(*g[0], ProbeOutcome::InvalidConfig, "engine rejected config: " + lr.detail);
                    } else {   // isolate the malformed outbound(s): neighbours must not fail with it
                        size_t mid = g.size() / 2;
                        std::vector<Item*> a(g.begin(), g.begin() + mid), b(g.begin() + mid, g.end());
                        lr = LaunchResult();
                        leases.clear();
                        runXrayGroup(std::move(a));
                        runXrayGroup(std::move(b));
                    }
                    return;
            }
        }
        for (auto* it : g) terminalFail(*it, ProbeOutcome::BindConflict, "port collisions persisted");
    }

    void runXrayTop(std::vector<Item*> g) {
        if (g.empty()) return;
        int before = ok_launches.load();
        runXrayGroup(g);
        // If not a single launch worked, the engine itself is broken, not these configs.
        if (g.size() >= 2 && ok_launches.load() == before) {
            for (auto* it : g)
                if (it->has_terminal && it->terminal == ProbeOutcome::InvalidConfig) {
                    it->terminal = ProbeOutcome::EngineError;
                    it->detail = "engine failed to start for every config: " + it->detail;
                }
        }
    }

    // ── isolated single-process worker (sing-box / mihomo) ──
    void runSingle(Item& it) {
        ActiveTestGuard active;
        for (int attempt = 0; attempt < std::max(1, opts.bind_retries); attempt++) {
            if (launches_left.fetch_sub(1) <= 0) { terminalFail(it, ProbeOutcome::EngineError, "launch budget exhausted"); return; }
            PortLease lease = t.leases_->acquire();
            if (!lease.valid()) { terminalFail(it, ProbeOutcome::EngineError, "no free local ports"); return; }
            LaunchRequest req;
            req.engine = it.engine;
            req.ports = {lease.port()};
            req.startup_timeout_ms = startup_ms;
            req.config_text = it.engine == "mihomo" ? it.cfg.toMihomoConfigYaml(lease.port())
                                                    : it.cfg.toSingBoxConfigJson(lease.port());
            if (req.config_text.empty()) { terminalFail(it, ProbeOutcome::Unsupported, it.engine + " config generation failed"); return; }
            lease.releaseSocket();
            it.t_start = t.clock_();
            LaunchResult lr = t.launcher_->launch(req);
            switch (lr.status) {
                case LaunchStatus::Ok: {
                    ok_launches++;
                    std::vector<Item*> one{&it};
                    probeGroup(one, {lease.port()});
                    return;
                }
                case LaunchStatus::BindConflict: continue;
                case LaunchStatus::BinaryMissing: terminalFail(it, ProbeOutcome::Unsupported, it.engine + " binary missing"); return;
                case LaunchStatus::Error: terminalFail(it, ProbeOutcome::EngineError, it.engine + " launch error: " + lr.detail); return;
                case LaunchStatus::StartupFailed: terminalFail(it, ProbeOutcome::InvalidConfig, "engine rejected config: " + lr.detail); return;
            }
        }
        terminalFail(it, ProbeOutcome::BindConflict, "port collisions persisted");
    }

    static std::string describe(const Item& it) {
        std::ostringstream ss;
        auto c = [&](const char* n, const CheckOutcome& o) {
            if (o.status == CheckStatus::Fail) ss << n << ":" << o.detail << " ";
        };
        c("A", it.raw.a);
        c("B", it.raw.b);
        if (it.raw.bulk_run && it.raw.bulk.status == CheckStatus::Fail) ss << "bulk:" << it.raw.bulk.detail;
        return ss.str();
    }

    void run(const std::vector<std::string>& uris, std::vector<ProxyTestResult>& results) {
        items.resize(uris.size());
        std::vector<Item*> xray_g, other_g;
        for (size_t i = 0; i < uris.size(); i++) {
            Item& it = items[i];
            it.idx = i;
            it.uri = uris[i];
            it.t_start = it.t_end = t.clock_();
            it.key = endpointKeyForUri(uris[i]);
            auto parsed = UriParser::parse(uris[i]);
            if (!parsed.has_value() || !parsed->isValid()) { terminalFail(it, ProbeOutcome::InvalidConfig, "URI parse failed"); continue; }
            it.cfg = *parsed;
            std::string why;
            it.engine = chooseEngine(it, &why);
            if (it.engine.empty()) { terminalFail(it, ProbeOutcome::Unsupported, why); continue; }
            if (useTcpPreScreen() && needsTcpPrescreen(it.cfg.protocol) &&
                !utils::tcpConnect(it.cfg.address, it.cfg.port, 2000)) {
                it.raw.a.status = it.raw.b.status = CheckStatus::Fail;
                it.raw.a.failure = it.raw.b.failure = CheckFailure::Remote;
                it.raw.a.detail = it.raw.b.detail = "tcp prescreen failed";
                it.raw.started_at = it.raw.finished_at = t.clock_();
                it.probed = true;   // goes through baseline attribution like any other failure
                continue;
            }
            (it.engine == "xray" ? xray_g : other_g).push_back(&it);
        }

        // Shared xray process (bisected) concurrently with isolated sing-box/mihomo workers.
        auto xfut = std::async(std::launch::async, [&] { runXrayTop(xray_g); });
        {
            std::atomic<size_t> next{0};
            int workers = std::max(1, std::min<int>((int)other_g.size(), std::min(8, getMaxConcurrentTests())));
            std::vector<std::thread> pool;
            for (int w = 0; w < workers && !other_g.empty(); w++) {
                pool.emplace_back([&] {
                    for (size_t i = next.fetch_add(1); i < other_g.size(); i = next.fetch_add(1)) {
                        try { runSingle(*other_g[i]); }
                        catch (const std::exception& e) { terminalFail(*other_g[i], ProbeOutcome::EngineError, e.what()); }
                        catch (...) { terminalFail(*other_g[i], ProbeOutcome::EngineError, "exception"); }
                    }
                });
            }
            for (auto& th : pool) th.join();
        }
        try { xfut.get(); } catch (...) {
            for (auto* it : xray_g) if (!it->probed && !it->has_terminal) terminalFail(*it, ProbeOutcome::EngineError, "xray group exception");
        }

        finalize(results);
    }

    void finalize(std::vector<ProxyTestResult>& results) {
        auto& ctl = t.baseline_->controls();
        bool any_fail = false;
        for (auto& it : items) {
            if (!it.probed) continue;
            if (it.raw.bothPass()) ctl.record(it.key, kCheckA | kCheckB, it.raw.finished_at);
            else if (!it.raw.engine_unreachable) any_fail = true;
        }
        BaselineSnapshot snap;
        if (any_fail) snap = t.baseline_->current(ConnectivityBaseline::kFailureRecheckSeconds);
        const double now = t.clock_();

        int pass = 0, excluded = 0, fail = 0;
        for (auto& it : items) {
            ProxyTestResult& r = results[it.idx];
            r.uri = it.uri;
            r.endpoint_key = it.key;
            r.engine_used = it.engine;
            std::string run_id = run_prefix + "-" + std::to_string(it.idx);
            ProbeResult pr;
            if (it.probed) {
                Attribution at = classifyRound(it.raw, snap, &ctl, it.key, now);
                pr = buildProbeResult(it.raw, at, it.key, it.engine, run_id, opts.generation);
                if (!it.raw.bothPass()) r.error_message = describe(it);
            } else {
                pr.endpoint_key = it.key;
                pr.run_id = run_id;
                pr.generation = opts.generation;
                pr.engine = it.engine;
                pr.outcome = it.has_terminal ? it.terminal : ProbeOutcome::EngineError;
                pr.started_at = it.t_start;
                pr.finished_at = it.t_end > 0.0 ? it.t_end : now;
                r.error_message = it.detail.empty() ? "not tested" : it.detail;
            }
            r.probe = pr;
            r.has_probe = true;
            r.success = pr.outcome == ProbeOutcome::Pass;
            if (r.success) {
                r.latency_ms = pr.latency_ms;
                r.download_speed_kbps = (float)it.raw.bulk_kibps;   // real KiB/s, 0 when not measured
                r.error_message.clear();
                pass++;
            } else if (pr.attributable && (pr.outcome == ProbeOutcome::RemoteFailure || pr.outcome == ProbeOutcome::Partial)) fail++;
            else excluded++;
        }
        TLOG("[BatchTest] " << items.size() << " configs: " << pass << " pass, " << fail << " server-failure, "
             << excluded << " excluded (engine/local/unclassified)");
    }
};

std::vector<ProxyTestResult> ProxyTester::testBatch(const std::vector<std::string>& config_uris,
                                                    const BatchTestOptions& opts) {
    std::vector<ProxyTestResult> results(config_uris.size());
    if (config_uris.empty()) return results;
    ensureDeps();
    Impl impl(*this, opts);
    impl.run(config_uris, results);
    return results;
}

ProxyTestResult ProxyTester::testConfig(const std::string& config_uri, const std::string& /*test_url*/,
                                        int timeout_seconds) {
    BatchTestOptions o;
    o.timeout_seconds = timeout_seconds;
    o.bulk = true;
    auto r = testBatch({config_uri}, o);
    return r.empty() ? ProxyTestResult() : r.front();
}

// ═══════════════════════════════════════════════════════════════════
// Gemini (Google AI) accessibility - optional capability metadata, never health
// ═══════════════════════════════════════════════════════════════════

static constexpr const char* GEMINI_API_URL = "https://generativelanguage.googleapis.com/v1beta/models";

static int checkGeminiViaSocks5(int socks_port, int timeout_seconds) {
    // 1 = reachable (any HTTP response, even 401/403), 0 = blocked, -1 = error. TLS is verified.
    TransportRequest rq;
    rq.url = GEMINI_API_URL;
    rq.proxy_port = socks_port;
    rq.connect_timeout_ms = std::min(timeout_seconds, 10) * 1000;
    rq.total_timeout_ms = timeout_seconds * 1000;
    rq.max_body_bytes = 64 * 1024;
    static std::shared_ptr<ProbeTransport> tr = makeCurlTransport();
    TransportResponse r = tr->fetch(rq);
    if (r.error == TransportError::None && r.status > 0) return 1;
    if (r.error == TransportError::TooLarge && r.status > 0) return 1;
    if (r.error == TransportError::ProxyConnect) return -1;
    return 0;
}

int ProxyTester::checkGeminiAccess(const std::string& config_uri, int timeout_seconds) {
    ensureDeps();
    auto parsed = UriParser::parse(config_uri);
    if (!parsed.has_value() || !parsed->isValid()) return -1;
    for (const char* engine : {"xray", "sing-box"}) {
        if (!launcher_->available(engine)) continue;
        PortLease lease = leases_->acquire();
        if (!lease.valid()) return -1;
        LaunchRequest req;
        req.engine = engine;
        req.ports = {lease.port()};
        req.config_text = std::string(engine) == "xray" ? proxy::XRayManager::generateTestConfig(*parsed, lease.port())
                                                         : parsed->toSingBoxConfigJson(lease.port());
        if (req.config_text.empty()) continue;
        lease.releaseSocket();
        LaunchResult lr = launcher_->launch(req);
        if (lr.status != LaunchStatus::Ok) continue;
        int res = checkGeminiViaSocks5(lease.port(), timeout_seconds);
        TLOG("  [Gemini:" << lease.port() << "] " << summarizeConfigForLog(config_uri) << " -> "
             << (res == 1 ? "ACCESSIBLE" : res == 0 ? "BLOCKED" : "UNKNOWN"));
        return res;
    }
    return -1;
}

} // namespace network
} // namespace hunter
