#pragma once

#include <string>

namespace hunter {
namespace gui {

/**
 * @brief Enforces one running Hunter instance per user, and lets a second
 *        launch raise the window of the first instead of starting over.
 *
 * The primary instance holds an exclusive lock for its whole lifetime. A
 * second launch fails to take that lock, drops a "show request" marker for
 * the primary to notice, and exits. The primary polls for that marker from
 * its render loop and un-minimizes/focuses its window when it appears.
 *
 * The lock is held by the OS, not written to a file we clean up, so a crash
 * or a kill -9 releases it — there is no stale lock to recover from.
 */
class SingleInstanceGuard {
public:
    SingleInstanceGuard() = default;
    ~SingleInstanceGuard();

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    /**
     * @brief Try to become the one running instance.
     * @return true if this process is the primary and should carry on.
     *         false if another instance is already running — it has been
     *         asked to show itself, and this process should exit.
     */
    bool acquire();

    /**
     * @brief Primary only: check whether another launch asked us to show up.
     * @return true exactly once per request (the marker is consumed).
     */
    bool consumeShowRequest();

private:
    bool primary_ = false;
    std::string show_path_;
#ifdef _WIN32
    void* mutex_ = nullptr;   // HANDLE
#else
    int lock_fd_ = -1;
#endif
};

} // namespace gui
} // namespace hunter
