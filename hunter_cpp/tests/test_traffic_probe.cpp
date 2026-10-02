#include "test_support.h"
#include "fake_transport.h"
using namespace hunter::network;
using namespace fake;

static std::shared_ptr<FakeTransport> make(FakeTransport::Handler h) {
    auto t = std::make_shared<FakeTransport>(); t->handler = h; return t;
}
static RawProbe run(std::shared_ptr<FakeTransport> t, bool bulk = false, int port = 31000) {
    TrafficProbe p(t); return p.run(port, bulk);
}

int main() {
    T_CASE("both checks pass, exit ip/country captured");
    { auto t = make([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      auto r = run(t);
      CHECK(r.bothPass(), "both pass"); CHECK(r.exit_ip == "93.184.216.34", "ip"); CHECK(r.exit_country == "NL", "loc");
      CHECK(r.latency_ms >= 59.9 && r.latency_ms <= 60.1, "latency = max(A,B)");
      CHECK(t->calls[0].tls_verify, "tls verify on"); CHECK(t->calls[0].proxy_port == 31000, "exact leased port"); } T_END();

    T_CASE("captive portal HTML 200 on Check A fails");
    { auto t = make([](const TransportRequest& q) { if (isA(q)) { auto r = okB("<html>login</html>"); return r; } return okB(); });
      auto r = run(t); CHECK(r.a.status == CheckStatus::Fail, "A fails"); CHECK(r.a.failure == CheckFailure::BadStatus, "bad status");
      CHECK(!r.bothPass(), "not pass"); } T_END();

    T_CASE("redirect 302 is a failure, not followed");
    { auto t = make([](const TransportRequest& q) { if (isA(q)) { TransportResponse r; r.status = 302; r.location = "http://portal/"; return r; } return okB(); });
      auto r = run(t); CHECK(r.a.failure == CheckFailure::Redirect || r.a.failure == CheckFailure::BadStatus, "redirect fails"); } T_END();

    T_CASE("204 with a body fails");
    { auto t = make([](const TransportRequest& q) { if (isA(q)) { auto r = okA(); r.body = "x"; r.body_bytes = 1; return r; } return okB(); });
      CHECK(run(t).a.status == CheckStatus::Fail, "A fails"); } T_END();

    T_CASE("wrong h= / bad trace rejected");
    { for (std::string body : {trace("93.184.216.34", "NL", "evil.example"), std::string("<html>hello</html>"), std::string(""),
                               trace("10.0.0.5"), trace("93.184.216.34", "nl"), trace("93.184.216.34", "NLD"),
                               std::string(5000, 'a')}) {
          auto t = make([&](const TransportRequest& q) { return isA(q) ? okA() : okB(body); });
          auto r = run(t); CHECK(r.b.status == CheckStatus::Fail, "B must fail for: " + body.substr(0, 30)); CHECK(r.exit_country.empty(), "no country stored"); } } T_END();

    T_CASE("trace parser strictness");
    { CHECK(parseCloudflareTrace(trace(), "www.cloudflare.com").valid, "valid");
      auto dup = trace() + "h=www.cloudflare.com\n"; CHECK(!parseCloudflareTrace(dup, "www.cloudflare.com").valid, "duplicate h");
      CHECK(!parseCloudflareTrace(std::string("h=www.cloudflare.com\nip=1.1.1.1\nts=abc\nloc=NL\n") + std::string(60, 'z'), "www.cloudflare.com").valid, "bad ts");
      CHECK(isPublicIpLiteral("8.8.8.8") && !isPublicIpLiteral("192.168.1.1") && !isPublicIpLiteral("127.0.0.1") && !isPublicIpLiteral("nope"), "ip classes"); } T_END();

    T_CASE("TLS error and proxy connect classification");
    { auto t = make([](const TransportRequest&) { return err(TransportError::Tls, "bad cert"); });
      auto r = run(t); CHECK(r.a.failure == CheckFailure::Tls && r.b.failure == CheckFailure::Tls, "tls"); CHECK(!r.engine_unreachable, "engine reachable");
      auto t2 = make([](const TransportRequest&) { return err(TransportError::ProxyConnect); });
      auto r2 = run(t2); CHECK(r2.engine_unreachable, "engine unreachable"); } T_END();

    T_CASE("bulk: real KiB/s, truncated and oversize rejected");
    { auto t = make([](const TransportRequest& q) { return isA(q) ? okA() : isB(q) ? okB() : okBulk(65536, 500); });
      auto r = run(t, true); CHECK(r.bulk_run && r.bulk.status == CheckStatus::Pass, "bulk pass"); CHECK_NEAR(r.bulk_kibps, 128.0, 0.5, "64KiB/0.5s = 128 KiB/s");
      auto t2 = make([](const TransportRequest& q) { return isA(q) ? okA() : isB(q) ? okB() : okBulk(30000, 500); });
      auto r2 = run(t2, true); CHECK(r2.bulk.status == CheckStatus::Fail && r2.bulk_kibps == 0.0, "truncated");
      auto t3 = make([](const TransportRequest& q) { return isA(q) ? okA() : isB(q) ? okB() : err(TransportError::TooLarge); });
      CHECK(run(t3, true).bulk.status == CheckStatus::Fail, "oversize");
      auto t4 = make([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      CHECK(!run(t4, false).bulk_run, "bulk off by default"); } T_END();

    T_CASE("port 0 means direct");
    { auto t = make([](const TransportRequest& q) { return isA(q) ? okA() : okB(); });
      TrafficProbe p(t); p.run(0, false); CHECK(t->calls[0].proxy_port == 0, "direct"); } T_END();
    return T_SUMMARY();
}
