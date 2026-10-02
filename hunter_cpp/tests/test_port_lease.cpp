#include "test_support.h"
#include "network/port_lease.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
using namespace hunter::network;

int main() {
    T_CASE("ports within 29000-59999, unique, released on destruction");
    { auto reg = PortLeaseRegistry::create();
      std::set<int> seen;
      { auto v = reg->acquireMany(50);
        CHECK(v.size() == 50, "50 leases");
        for (auto& l : v) { CHECK(l.port() >= 29000 && l.port() <= 59999, "range"); CHECK(seen.insert(l.port()).second, "unique"); CHECK(l.socketHeld(), "socket held"); CHECK(reg->isLeased(l.port()), "leased"); }
        CHECK(reg->activeCount() == 50, "active"); }
      CHECK(reg->activeCount() == 0, "RAII release"); } T_END();

    T_CASE("releaseSocket frees the bind but keeps reservation; reacquire rebinds");
    { auto reg = PortLeaseRegistry::create();
      auto l = reg->acquire(); int p = l.port(); l.releaseSocket();
      CHECK(!l.socketHeld() && reg->isLeased(p), "reserved w/o socket");
      CHECK(reg->activeCount() == 1, "still counted");
      CHECK(l.reacquire(), "reacquire"); CHECK(l.valid(), "valid after reacquire"); } T_END();

    T_CASE("concurrent acquisition never hands out the same port");
    { auto reg = PortLeaseRegistry::create();
      std::mutex m; std::set<int> all; std::atomic<int> dups{0};
      std::vector<std::thread> th;
      std::vector<std::vector<PortLease>> keep(8);
      for (int t = 0; t < 8; t++) th.emplace_back([&, t] {
          auto v = reg->acquireMany(25);
          { std::lock_guard<std::mutex> g(m); for (auto& l : v) if (!all.insert(l.port()).second) dups++; }
          keep[t] = std::move(v); });
      for (auto& x : th) x.join();
      CHECK(dups == 0, "no duplicates"); CHECK(all.size() == 200, "200 distinct"); } T_END();

    T_CASE("foreign bound socket is skipped (collision), small range exhaustion");
    { int fs = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
      bind(fs, (sockaddr*)&a, sizeof a); listen(fs, 1); socklen_t len = sizeof a; getsockname(fs, (sockaddr*)&a, &len); int busy = ntohs(a.sin_port);
      auto reg = PortLeaseRegistry::create(nullptr, busy, busy + 1);
      auto l = reg->acquire(); CHECK(l.valid() && l.port() == busy + 1, "skips the busy port");
      auto l2 = reg->acquire(8); CHECK(!l2.valid(), "range exhausted -> invalid lease");
      close(fs); } T_END();

    T_CASE("runWithLeaseRetry: bounded to 3 attempts, zero penalty, fresh ports");
    { auto reg = PortLeaseRegistry::create(); int calls = 0; std::set<int> ports; bool done = false;
      int n = runWithLeaseRetry(reg, [&](PortLease& l) { calls++; ports.insert(l.port()); return LeaseAttempt::BindConflict; }, &done);
      CHECK(calls == 3 && n == 3 && !done, "3 attempts then give up");
      calls = 0; done = false;
      runWithLeaseRetry(reg, [&](PortLease&) { return ++calls < 2 ? LeaseAttempt::BindConflict : LeaseAttempt::Done; }, &done);
      CHECK(calls == 2 && done, "succeeds on retry"); CHECK(reg->activeCount() == 0, "all released"); } T_END();

    T_CASE("injected bind failure (setBindForTest) triggers retry path");
    { auto reg = PortLeaseRegistry::create(); std::atomic<int> n{0};
      reg->setBindForTest([&](int) -> long long { return n++ < 3 ? -1 : 0; });
      auto l = reg->acquire(); CHECK(l.valid(), "acquired after 3 failed binds"); CHECK(n >= 4, "bind retried"); } T_END();
    return T_SUMMARY();
}
