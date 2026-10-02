#pragma once
// Deterministic fakes for the Stage 4 B runtime suites: clock, engine launcher, probe, executors.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "proxy/health_monitor.h"

namespace fakes {
using namespace hunter;
using namespace hunter::proxy;

struct Clock {
    std::atomic<double> t{1000.0};
    double now() const { return t.load(); }
    void advance(double s) { t = t.load() + s; }
};

struct FakeEngine {
    int port = 0;
    std::string engine, config;
    std::atomic<bool> alive{true};
    std::atomic<bool> destroyed{false};
};

class FakeLauncher : public network::EngineLauncher {
public:
    std::mutex mu;
    std::vector<std::shared_ptr<FakeEngine>> launched;
    std::vector<std::string> log;                  // "launch:<port>" / "destroy:<port>"
    std::atomic<int> live{0}, destroyed_count{0};
    std::function<network::LaunchStatus(const network::LaunchRequest&)> hook;   // default Ok
    std::function<void()> before_launch;                                        // may block (async tests)
    bool available(const std::string&) const override { return true; }
    network::LaunchResult launch(const network::LaunchRequest& rq) override {
        if (before_launch) before_launch();
        network::LaunchResult r;
        network::LaunchStatus st = hook ? hook(rq) : network::LaunchStatus::Ok;
        if (st != network::LaunchStatus::Ok) { r.status = st; r.detail = "fake launch failure"; return r; }
        auto e = std::make_shared<FakeEngine>();
        e->port = rq.ports.empty() ? 0 : rq.ports.front();
        e->engine = rq.engine; e->config = rq.config_text;
        { std::lock_guard<std::mutex> lk(mu); launched.push_back(e); log.push_back("launch:" + std::to_string(e->port)); }
        live++;
        r.status = network::LaunchStatus::Ok;
        std::weak_ptr<FakeEngine> w = e;
        r.alive = [w] { auto p = w.lock(); return p && p->alive.load(); };
        FakeLauncher* self = this;
        r.guard = std::shared_ptr<void>(new int(0), [self, e](int* p) {
            delete p;
            e->alive = false; e->destroyed = true;
            self->live--; self->destroyed_count++;
            std::lock_guard<std::mutex> lk(self->mu);
            self->log.push_back("destroy:" + std::to_string(e->port));
        });
        return r;
    }
    std::shared_ptr<FakeEngine> last() { std::lock_guard<std::mutex> lk(mu); return launched.empty() ? nullptr : launched.back(); }
    int launches() { std::lock_guard<std::mutex> lk(mu); return (int)launched.size(); }
    int launchesOnPort(int p) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& e : launched) n += e->port == p; return n; }
};

inline ProbeResult mk(ProbeOutcome o, bool attributable = false) {
    ProbeResult r;
    r.outcome = o; r.attributable = attributable;
    if (o == ProbeOutcome::Pass) { r.checks = kCheckA | kCheckB | kCheckBulk; r.bulk_passed = true; r.latency_ms = 120; }
    return r;
}

// Scripted probe: behaviour per endpoint key; every call is recorded.
class FakeProbe {
public:
    struct Call { int port; bool bulk; std::string key; double at; };
    std::mutex mu;
    std::vector<Call> calls;
    std::map<std::string, ProbeResult> by_key;      // result for a key (default: Pass)
    std::function<ProbeResult(int, bool, const std::string&)> custom;
    Clock* clock = nullptr;
    std::vector<ProbeResult> sunk;                  // results handed to the sink
    ProbeResult operator()(int port, bool bulk, const std::string& key, const std::string&) {
        { std::lock_guard<std::mutex> lk(mu); calls.push_back({port, bulk, key, clock ? clock->now() : 0}); }
        if (custom) return custom(port, bulk, key);
        std::lock_guard<std::mutex> lk(mu);
        auto it = by_key.find(key);
        return it == by_key.end() ? mk(ProbeOutcome::Pass) : it->second;
    }
    void set(const std::string& key, ProbeResult r) { std::lock_guard<std::mutex> lk(mu); by_key[key] = r; }
    int count() { std::lock_guard<std::mutex> lk(mu); return (int)calls.size(); }
    int countOnPort(int p) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& c : calls) n += c.port == p; return n; }
    int countKey(const std::string& k) { std::lock_guard<std::mutex> lk(mu); int n = 0; for (auto& c : calls) n += c.key == k; return n; }
};

// Queues jobs; tests run them explicitly (to interleave stop()/failover with in-flight work).
class ManualExecutor : public Executor {
public:
    std::mutex mu;
    std::deque<std::function<void()>> q;
    void post(std::function<void()> fn) override { std::lock_guard<std::mutex> lk(mu); q.push_back(std::move(fn)); }
    size_t pending() { std::lock_guard<std::mutex> lk(mu); return q.size(); }
    bool runOne() {
        std::function<void()> f;
        { std::lock_guard<std::mutex> lk(mu); if (q.empty()) return false; f = std::move(q.front()); q.pop_front(); }
        f(); return true;
    }
    void runAll() { while (runOne()) {} }
};

struct Env {
    Clock clock;
    std::shared_ptr<FakeLauncher> launcher = std::make_shared<FakeLauncher>();
    FakeProbe probe;
    std::shared_ptr<network::PortLeaseRegistry> leases = network::PortLeaseRegistry::create();
    std::shared_ptr<Executor> exec = std::make_shared<InlineExecutor>();
    std::vector<Candidate> cands;
    std::mutex cmu;
    std::vector<ProbeResult> sunk;
    std::mutex smu;
    network::BaselineState baseline_state = network::BaselineState::Online;
    double rand = 0.5;
    std::atomic<int> build_calls{0};
    bool build_fails = false;

    MonitorDeps deps() {
        MonitorDeps d;
        probe.clock = &clock;
        d.steady = [this] { return clock.now(); };
        d.utc = [this] { return clock.now(); };
        d.rand01 = [this] { return rand; };
        d.launcher = launcher;
        d.leases = leases;
        d.executor = exec;
        d.probe = [this](int p, bool b, const std::string& k, const std::string& e) { return probe(p, b, k, e); };
        d.baseline = [this] { return baseline_state; };
        d.candidates = [this] { std::lock_guard<std::mutex> lk(cmu); return cands; };
        d.build_config = [this](const std::string& u, int port) {
            build_calls++;
            BuiltConfig b;
            if (build_fails) { b.error = "unsupported test config"; return b; }
            b.ok = true; b.engine = "xray"; b.config_text = "uri=" + u + ";port=" + std::to_string(port);
            return b;
        };
        d.key_for = [](const std::string& u) { return "ek:" + u; };
        d.sink = [this](const ProbeResult& r) { std::lock_guard<std::mutex> lk(smu); sunk.push_back(r); };
        return d;
    }
    void setCands(std::vector<Candidate> c) { std::lock_guard<std::mutex> lk(cmu); cands = std::move(c); }
    size_t sunkCount() { std::lock_guard<std::mutex> lk(smu); return sunk.size(); }
};

inline Candidate cand(const std::string& uri, bool stable = true, double score = 90) {
    Candidate c; c.uri = uri; c.endpoint_key = "ek:" + uri; c.stable = stable; c.score = score; return c;
}

// Advance the fake clock one second at a time, ticking the watchdog (like the 1 s thread).
inline void run(const std::shared_ptr<HealthMonitor>& m, Clock& c, int seconds, std::vector<SessionState>* seen = nullptr) {
    for (int i = 0; i < seconds; i++) {
        c.advance(1.0);
        m->step();
        if (seen) seen->push_back(m->snapshot().state);
    }
}
inline bool contains(const std::vector<SessionState>& v, SessionState s) { for (auto x : v) if (x == s) return true; return false; }

}  // namespace fakes
