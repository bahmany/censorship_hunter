#include "test_support.h"
#include "fake_transport.h"
#include "network/connectivity_baseline.h"
using namespace hunter;
using namespace hunter::network;
using namespace fake;

struct Env {
    std::shared_ptr<FakeTransport> tr = std::make_shared<FakeTransport>();
    double now = 1000.0;
    bool link = true;
    std::shared_ptr<ConnectivityBaseline> bl;
    Env(FakeTransport::Handler h) {
        tr->handler = h;
        bl = std::make_shared<ConnectivityBaseline>(tr, ProbeConfig(), [this] { return now; },
                                                   [this] { return link; }, [this](double s) { now += s; });
    }
};
static RawProbe failedRaw(CheckFailure fa, CheckFailure fb, double t = 1000.0) {
    RawProbe r; r.port = 31000; r.started_at = t; r.finished_at = t;
    r.a.status = fa == CheckFailure::None ? CheckStatus::Pass : CheckStatus::Fail; r.a.failure = fa;
    r.b.status = fb == CheckFailure::None ? CheckStatus::Pass : CheckStatus::Fail; r.b.failure = fb;
    return r;
}
static RawProbe passRaw() { return failedRaw(CheckFailure::None, CheckFailure::None); }

int main() {
    T_CASE("online: both direct checks pass");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      auto s = e.bl->current(); CHECK(s.state == BaselineState::Online && s.a_ok && s.b_ok, "online");
      CHECK(e.tr->count(0) == 2, "direct only (port 0)"); } T_END();

    T_CASE("30 s cache; forced short recheck");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      e.bl->current(); int n = (int)e.tr->calls.size();
      e.now += 20; e.bl->current(); CHECK((int)e.tr->calls.size() == n, "cached within 30s");
      e.now += 20; e.bl->current(); CHECK((int)e.tr->calls.size() > n, "expired after 30s");
      n = (int)e.tr->calls.size(); e.now += 6; e.bl->current(ConnectivityBaseline::kFailureRecheckSeconds);
      CHECK((int)e.tr->calls.size() > n, "failure recheck uses 5s"); } T_END();

    T_CASE("offline: link down and direct fails => LocalNetworkDown, no penalty");
    { Env e([](const TransportRequest&) { return err(TransportError::Timeout); }); e.link = false;
      auto s = e.bl->current(); CHECK(s.state == BaselineState::Offline, "offline");
      auto a = classifyRound(failedRaw(CheckFailure::Timeout, CheckFailure::Timeout), s, &e.bl->controls(), "k1", e.now);
      CHECK(a.outcome == ProbeOutcome::LocalNetworkDown && !a.attributable, "local down");
      auto pr = buildProbeResult(failedRaw(CheckFailure::Timeout, CheckFailure::Timeout), a, "k1", "xray", "r-1", 1);
      CHECK(!pr.attributable && pr.outcome == ProbeOutcome::LocalNetworkDown, "no penalty result"); } T_END();

    T_CASE("link up but direct fails (censored/captive) => Indeterminate, not attributable");
    { Env e([](const TransportRequest&) { return err(TransportError::Timeout); });
      auto s = e.bl->current(); CHECK(s.state != BaselineState::Online, "not online");
      auto a = classifyRound(failedRaw(CheckFailure::Timeout, CheckFailure::Timeout), s, &e.bl->controls(), "k1", e.now);
      CHECK(a.outcome == ProbeOutcome::Indeterminate && !a.attributable, "indeterminate"); } T_END();

    T_CASE("online + failed tunnel => attributable RemoteFailure");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      auto s = e.bl->current();
      auto a = classifyRound(failedRaw(CheckFailure::Timeout, CheckFailure::Timeout), s, &e.bl->controls(), "k1", e.now);
      CHECK(a.outcome == ProbeOutcome::RemoteFailure && a.attributable, "remote failure");
      auto p = classifyRound(failedRaw(CheckFailure::None, CheckFailure::Timeout), s, &e.bl->controls(), "k1", e.now);
      CHECK(p.outcome == ProbeOutcome::Partial, "partial");
      auto ok = classifyRound(passRaw(), s, &e.bl->controls(), "k1", e.now);
      CHECK(ok.outcome == ProbeOutcome::Pass, "pass"); } T_END();

    T_CASE("engine unreachable => EngineError, never attributable");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      auto s = e.bl->current(); auto r = failedRaw(CheckFailure::ProxyConnect, CheckFailure::ProxyConnect); r.engine_unreachable = true;
      auto a = classifyRound(r, s, &e.bl->controls(), "k1", e.now); CHECK(a.outcome == ProbeOutcome::EngineError && !a.attributable, "engine error");
      auto r2 = failedRaw(CheckFailure::ProxyConnect, CheckFailure::ProxyConnect);
      CHECK(!classifyRound(r2, s, &e.bl->controls(), "k1", e.now).attributable, "ProxyConnect never attributable"); } T_END();

    T_CASE("M5 toggle off: no direct probes, failures not attributable");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      e.bl->setEnabled(false); CHECK(!e.bl->enabled(), "disabled");
      auto s = e.bl->current(); CHECK(e.tr->calls.empty(), "no direct traffic when disabled"); CHECK(!s.enabled, "snapshot says disabled");
      auto a = classifyRound(failedRaw(CheckFailure::Timeout, CheckFailure::Timeout), s, &e.bl->controls(), "k1", e.now);
      CHECK(!a.attributable && a.outcome != ProbeOutcome::RemoteFailure, "no attribution without evidence");
      e.bl->setEnabled(true); CHECK(e.bl->current().enabled, "re-enabled"); } T_END();

    T_CASE("control tunnel: other endpoint passing attributes failure; own key never counts");
    { Env e([](const TransportRequest&) { return err(TransportError::Timeout); });
      auto s = e.bl->current(); // direct failing, link up -> indeterminate
      auto fail = failedRaw(CheckFailure::Timeout, CheckFailure::Timeout);
      CHECK(!classifyRound(fail, s, &e.bl->controls(), "k1", e.now).attributable, "no control yet");
      e.bl->controls().record("k1", kCheckA | kCheckB, e.now);
      CHECK(!classifyRound(fail, s, &e.bl->controls(), "k1", e.now).attributable, "own key is not a control");
      e.bl->controls().record("k2", kCheckA | kCheckB, e.now);
      auto a = classifyRound(fail, s, &e.bl->controls(), "k1", e.now);
      CHECK(a.attributable && a.outcome == ProbeOutcome::RemoteFailure, "other tunnel works => server fault");
      CHECK(!e.bl->controls().passed("k1", kCheckA, e.now + 120), "control expires after 60s"); } T_END();

    T_CASE("generation bump drops cache");
    { Env e([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      e.bl->current(); int n = (int)e.tr->calls.size(); e.bl->bumpGeneration(); e.bl->current();
      CHECK((int)e.tr->calls.size() > n, "re-measured"); CHECK(e.bl->generation() == 1, "generation"); } T_END();
    return T_SUMMARY();
}
