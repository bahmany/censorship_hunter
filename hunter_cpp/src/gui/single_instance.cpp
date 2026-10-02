#include "gui/single_instance.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace hunter {
namespace gui {

namespace {

// Both files live in a per-user runtime directory so two people on the same
// machine each get their own instance. XDG_RUNTIME_DIR is a per-user tmpfs the
// system clears on logout; the temp dir is the fallback when it is unset.
std::filesystem::path runtimeDir() {
#ifndef _WIN32
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR")) {
        if (*xdg) {
            std::error_code ec;
            std::filesystem::path p(xdg);
            if (std::filesystem::is_directory(p, ec)) return p;
        }
    }
#endif
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec) return std::filesystem::path(".");
    return tmp;
}

// Per-user basename, so the temp-dir fallback cannot collide between users.
std::string instanceKey() {
#ifdef _WIN32
    return "hunter";
#else
    return "hunter-" + std::to_string(static_cast<unsigned long>(getuid()));
#endif
}

} // namespace

bool SingleInstanceGuard::acquire() {
    const auto dir = runtimeDir();
    const auto key = instanceKey();
    show_path_ = (dir / (key + ".show")).string();
    const std::string lock_path = (dir / (key + ".lock")).string();

#ifdef _WIN32
    // A named mutex is the Windows equivalent: the kernel drops it when the
    // owning process ends, however it ends.
    //
    // Retry briefly before giving up: the self-relaunch path (GL fallback,
    // see gui/app.cpp) spawns its replacement process and then returns,
    // unwinding main()'s stack to destroy this guard and release the mutex —
    // but the child can start running and reach this exact acquire() call
    // before that unwind completes, seeing a mutex the parent is about to
    // drop in milliseconds as permanently "already running". A short retry
    // window closes that race without weakening real second-launch detection
    // (a genuine already-running instance holds the mutex indefinitely, so
    // it still fails after the same short wait either way).
    HANDLE h = nullptr;
    for (int attempt = 0; attempt < 20; ++attempt) {
        h = CreateMutexA(nullptr, TRUE, ("Local\\" + key).c_str());
        if (!h || GetLastError() != ERROR_ALREADY_EXISTS) break;
        CloseHandle(h);
        h = nullptr;
        Sleep(50);
    }
    if (!h) {
        h = CreateMutexA(nullptr, TRUE, ("Local\\" + key).c_str());
    }
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        std::FILE* f = std::fopen(show_path_.c_str(), "wb");
        if (f) std::fclose(f);
        return false;
    }
    if (!h) return true;  // cannot arbitrate — better to run than to refuse
    mutex_ = h;
#else
    lock_fd_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) return true;  // cannot arbitrate — better to run than to refuse

    // Non-blocking exclusive lock. Held until this process exits; the kernel
    // releases it on exit or crash, so a stale lock cannot lock the user out.
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(lock_fd_);
        lock_fd_ = -1;
        // Ask the running instance to show itself.
        int fd = ::open(show_path_.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) ::close(fd);
        return false;
    }
#endif

    // We are primary: clear any request left behind by an earlier run so we do
    // not immediately raise ourselves on startup.
    std::error_code ec;
    std::filesystem::remove(show_path_, ec);

    primary_ = true;
    return true;
}

bool SingleInstanceGuard::consumeShowRequest() {
    if (!primary_ || show_path_.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(show_path_, ec)) return false;
    std::filesystem::remove(show_path_, ec);
    return true;
}

SingleInstanceGuard::~SingleInstanceGuard() {
    if (!primary_) return;
    std::error_code ec;
    std::filesystem::remove(show_path_, ec);
#ifdef _WIN32
    if (mutex_) {
        ReleaseMutex(static_cast<HANDLE>(mutex_));
        CloseHandle(static_cast<HANDLE>(mutex_));
        mutex_ = nullptr;
    }
#else
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
        lock_fd_ = -1;
    }
#endif
}

} // namespace gui
} // namespace hunter
