#include "network/port_lease.h"

#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hunter {
namespace network {

namespace {
long long bindLoopback(int port) {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); });
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    BOOL excl = TRUE;   // exclusive: no other process may share the port
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    // No SO_REUSEADDR / SO_REUSEPORT: any existing listener or TIME_WAIT remnant => collision.
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(s, (sockaddr*)&a, sizeof a) != 0) {
#ifdef _WIN32
        closesocket(s);
#else
        ::close(s);
#endif
        return -1;
    }
    return (long long)s;
}
void closeFd(long long fd) {
    if (fd < 0) return;
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    ::close((int)fd);
#endif
}
int defaultRandom(int lo, int hi) {
    static std::mutex m;
    static std::mt19937 gen{std::random_device{}()};
    std::lock_guard<std::mutex> lk(m);
    return std::uniform_int_distribution<int>(lo, hi)(gen);
}
}  // namespace

// ── PortLease ───────────────────────────────────────────────────────────
PortLease::PortLease(PortLease&& o) noexcept : reg_(std::move(o.reg_)), port_(o.port_), sock_(o.sock_) {
    o.port_ = 0; o.sock_ = -1;
}
PortLease& PortLease::operator=(PortLease&& o) noexcept {
    if (this != &o) {
        reset();
        reg_ = std::move(o.reg_); port_ = o.port_; sock_ = o.sock_;
        o.port_ = 0; o.sock_ = -1;
    }
    return *this;
}
PortLease::~PortLease() { reset(); }
void PortLease::closeSock() { closeFd(sock_); sock_ = -1; }
void PortLease::releaseSocket() { closeSock(); }
void PortLease::reset() {
    closeSock();
    if (reg_ && port_ > 0) reg_->release(port_);
    port_ = 0;
    reg_.reset();
}
bool PortLease::reacquire() {
    auto reg = reg_;
    if (!reg) return false;
    reset();
    PortLease fresh = reg->acquire();
    if (!fresh.valid()) return false;
    *this = std::move(fresh);
    return true;
}

// ── Registry ────────────────────────────────────────────────────────────
PortLeaseRegistry::PortLeaseRegistry(RandomFn rnd, int lo, int hi)
    : rnd_(rnd ? std::move(rnd) : RandomFn(defaultRandom)), bind_(bindLoopback), lo_(lo), hi_(hi) {}

std::shared_ptr<PortLeaseRegistry> PortLeaseRegistry::create(RandomFn rnd, int lo, int hi) {
    return std::shared_ptr<PortLeaseRegistry>(new PortLeaseRegistry(std::move(rnd), lo, hi));
}
std::shared_ptr<PortLeaseRegistry> PortLeaseRegistry::global() {
    static std::shared_ptr<PortLeaseRegistry> g = create();
    return g;
}
void PortLeaseRegistry::setBindForTest(BindFn f) {
    std::lock_guard<std::mutex> lk(mu_);
    bind_ = f ? std::move(f) : BindFn(bindLoopback);
}
void PortLeaseRegistry::release(int port) {
    std::lock_guard<std::mutex> lk(mu_);
    leased_.erase(port);
}
size_t PortLeaseRegistry::activeCount() const { std::lock_guard<std::mutex> lk(mu_); return leased_.size(); }
bool PortLeaseRegistry::isLeased(int port) const { std::lock_guard<std::mutex> lk(mu_); return leased_.count(port) > 0; }

PortLease PortLeaseRegistry::acquire(int max_tries) {
    PortLease lease;
    for (int i = 0; i < max_tries; i++) {
        int port;
        {
            std::lock_guard<std::mutex> lk(mu_);
            port = rnd_(lo_, hi_);
            if (port < lo_ || port > hi_ || leased_.count(port)) continue;
            leased_.insert(port);          // reserve first so concurrent callers never pick it
        }
        long long fd = bind_(port);        // bind outside the lock
        if (fd < 0) {
            std::lock_guard<std::mutex> lk(mu_);
            leased_.erase(port);
            continue;
        }
        lease.reg_ = shared_from_this();
        lease.port_ = port;
        lease.sock_ = fd;
        return lease;
    }
    return lease;
}

std::vector<PortLease> PortLeaseRegistry::acquireMany(size_t n) {
    std::vector<PortLease> out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        PortLease l = acquire();
        if (!l.valid()) break;
        out.push_back(std::move(l));
    }
    return out;
}

int runWithLeaseRetry(const std::shared_ptr<PortLeaseRegistry>& reg,
                      const std::function<LeaseAttempt(PortLease&)>& fn, bool* done, int max_attempts) {
    if (done) *done = false;
    int attempts = 0;
    PortLease lease = reg->acquire();
    while (attempts < max_attempts) {
        if (!lease.valid()) break;
        attempts++;
        if (fn(lease) == LeaseAttempt::Done) { if (done) *done = true; return attempts; }
        if (attempts >= max_attempts) break;
        if (!lease.reacquire()) break;
    }
    return attempts;
}

}  // namespace network
}  // namespace hunter
