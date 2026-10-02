#pragma once
// Tester/backend port leases (design D1 as amended by M2): random port in 29000-59999,
// process-wide registry, RAII bound loopback reservation socket. No ownership verification;
// a bind collision just means "retry another port" and never costs an endpoint any penalty.
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace hunter {
namespace network {

constexpr int kLeasePortMin = 29000;
constexpr int kLeasePortMax = 59999;

class PortLeaseRegistry;

class PortLease {
public:
    PortLease() = default;
    PortLease(PortLease&& o) noexcept;
    PortLease& operator=(PortLease&& o) noexcept;
    PortLease(const PortLease&) = delete;
    PortLease& operator=(const PortLease&) = delete;
    ~PortLease();

    bool valid() const { return port_ > 0; }
    int port() const { return port_; }
    // Close the reservation socket just before launching the engine. The registry
    // reservation (no other lease may take the port) remains until destruction/reset.
    void releaseSocket();
    bool socketHeld() const { return sock_ >= 0; }
    // Give up this port and take a fresh one (after an engine bind collision).
    bool reacquire();
    void reset();

private:
    friend class PortLeaseRegistry;
    void closeSock();
    std::shared_ptr<PortLeaseRegistry> reg_;
    int port_ = 0;
    long long sock_ = -1;
};

class PortLeaseRegistry : public std::enable_shared_from_this<PortLeaseRegistry> {
public:
    using RandomFn = std::function<int(int lo, int hi)>;       // injectable for tests
    using BindFn = std::function<long long(int port)>;          // returns socket fd or -1 (tests)

    static std::shared_ptr<PortLeaseRegistry> create(RandomFn rnd = nullptr, int lo = kLeasePortMin, int hi = kLeasePortMax);
    static std::shared_ptr<PortLeaseRegistry> global();

    // Random free port with a bound loopback socket. valid()==false only if the range is exhausted.
    PortLease acquire(int max_tries = 256);
    std::vector<PortLease> acquireMany(size_t n);
    size_t activeCount() const;
    bool isLeased(int port) const;

    // Test hook: replace the bind attempt (e.g. to simulate foreign listeners).
    void setBindForTest(BindFn f);

private:
    friend class PortLease;
    PortLeaseRegistry(RandomFn rnd, int lo, int hi);
    void release(int port);
    mutable std::mutex mu_;
    std::set<int> leased_;
    RandomFn rnd_;
    BindFn bind_;
    int lo_, hi_;
};

enum class LeaseAttempt { Done, BindConflict };

// Runs fn(lease) with a fresh lease; on BindConflict re-leases and retries (default max 3 attempts).
// Returns the number of attempts used; *done tells whether fn ever reported Done.
// A conflict is infrastructure noise: callers must not turn it into an endpoint penalty.
int runWithLeaseRetry(const std::shared_ptr<PortLeaseRegistry>& reg,
                      const std::function<LeaseAttempt(PortLease&)>& fn, bool* done, int max_attempts = 3);

}  // namespace network
}  // namespace hunter
