// Regression: BaseWorker::join(timeout) must honour its timeout. It used to
// ignore it and block until the worker's current execute() finished, which
// hung Stop / window close for minutes while a scan cycle was running.
#include "orchestrator/thread_manager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono;
using hunter::orchestrator::BaseWorker;

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            std::fprintf(stderr, "FAIL: %s (%s)\n", msg, #cond); \
            ++g_failures;                                        \
        }                                                        \
    } while (0)

// A worker whose execute() ignores the stop flag for `busy_ms`.
class BusyWorker : public BaseWorker {
public:
    BusyWorker(std::atomic<bool>& stop, int busy_ms)
        : BaseWorker("busy", 3600, stop), busy_ms_(busy_ms) {}
    ~BusyWorker() override = default;

protected:
    void execute() override { std::this_thread::sleep_for(milliseconds(busy_ms_)); }

private:
    int busy_ms_;
};

void testJoinTimesOutWhileBusy() {
    std::atomic<bool> stop{false};
    BusyWorker w(stop, 1500);
    w.start();
    std::this_thread::sleep_for(milliseconds(100));  // let execute() begin
    stop = true;
    const auto t0 = steady_clock::now();
    const bool joined = w.join(0.2f);
    const auto took = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    CHECK(!joined, "join must report a still-busy worker");
    CHECK(took < 800, "join(0.2) must return promptly, not wait for execute()");
    CHECK(w.join(5.0f), "join must succeed once execute() returns");
    CHECK(w.finished(), "finished() must be set after the loop exits");
}

void testJoinIsIdempotent() {
    std::atomic<bool> stop{false};
    BusyWorker w(stop, 10);
    w.start();
    stop = true;
    CHECK(w.join(5.0f), "first join");
    CHECK(w.join(0.1f), "second join on an already-joined worker is a no-op");
}

}  // namespace

int main() {
    testJoinTimesOutWhileBusy();
    testJoinIsIdempotent();
    if (g_failures) return 1;
    std::printf("test_worker_join: all passed\n");
    return 0;
}
