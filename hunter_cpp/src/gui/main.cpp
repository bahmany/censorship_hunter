#include <csignal>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include "core/config.h"
#include "gui/app.h"
#include "gui/single_instance.h"
#include "orchestrator/orchestrator.h"

#ifdef _WIN32
#include <windows.h>
#include <process.h>
// SHGetKnownFolderPath declared manually for cross-compile environments
// that lack shlobj_core.h (MinGW cross on Linux).
extern "C" long __stdcall SHGetKnownFolderPath(const GUID*, unsigned int, void*, wchar_t**);
// FOLDERID_LocalAppData = {FDC04C6D-6A76-4B66-8F53-3F0A4F498C3D}
static const GUID FOLDERID_LocalAppData_ = {
    0xFDC04C6D, 0x6A76, 0x4B66, {0x8F, 0x53, 0x3F, 0x0A, 0x4F, 0x49, 0x8C, 0x3D}
};
#define FOLDERID_LocalAppData (&FOLDERID_LocalAppData_)
#endif
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif

using namespace hunter;

namespace {
gui::HunterGuiApp* g_app = nullptr;

void handleSignal(int) {
    if (g_app) g_app->requestClose();
}

// Local wall-clock stamp for the crash log (portable localtime_s/localtime_r).
std::string crashTimestamp() {
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char buf[64];
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0) return std::string("unknown-time");
    return std::string(buf);
}

// Append one line to runtime/hunter_crash.log. Must never throw or abort:
// it is called from the crash paths themselves.
void appendCrashLog(const std::string& line) {
    try {
        FILE* f = fopen("runtime/hunter_crash.log", "a");
        if (!f) return;
        fputs(line.c_str(), f);
        fputc('\n', f);
        fflush(f);
        fclose(f);
    } catch (...) {
        // never let the crash reporter itself crash
    }
}

// std::terminate handler: an exception escaped every catch (most often from a
// thread body, where it would otherwise kill the whole app silently).
void terminateHandler() {
    std::string what = "unknown exception";
    try {
        std::exception_ptr ep = std::current_exception();
        if (ep) {
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
                what = "unknown non-std exception";
            }
        }
    } catch (...) {
        what = "exception description unavailable";
    }
    appendCrashLog("[" + crashTimestamp() + "] TERMINATE: " + what);
    std::abort();
}

#ifdef _WIN32
// Windows crash handler
LONG WINAPI crashHandler(EXCEPTION_POINTERS* info) {
    // Get the executable directory
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path logPath = exeDir / "hunter_startup.log";
    
    // Open log file in append mode
    FILE* logFile = fopen(logPath.string().c_str(), "a");
    if (logFile) {
        time_t now = time(nullptr);
        fprintf(logFile, "[%s] CRASH: Exception 0x%08X at 0x%p\n", 
                ctime(&now), 
                static_cast<unsigned int>(info->ExceptionRecord->ExceptionCode),
                info->ExceptionRecord->ExceptionAddress);
        fflush(logFile);
        fclose(logFile);
    }

    // Same crash record in the shared runtime/hunter_crash.log
    {
        char record[160];
        snprintf(record, sizeof(record), "[%s] CRASH: Exception 0x%08X at 0x%p",
                 crashTimestamp().c_str(),
                 static_cast<unsigned int>(info->ExceptionRecord->ExceptionCode),
                 info->ExceptionRecord->ExceptionAddress);
        appendCrashLog(std::string(record));
    }
    
    // Show message box to user
    char msg[512];
    snprintf(msg, sizeof(msg), 
             "Hunter crashed (exception 0x%08X at 0x%p). Details written to hunter_startup.log next to this program.",
             static_cast<unsigned int>(info->ExceptionRecord->ExceptionCode),
             info->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, msg, "Hunter Crash", MB_OK | MB_ICONERROR);
    
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// Helper class for startup logging
class StartupLogger {
private:
    FILE* logFile_;
    
public:
    StartupLogger() : logFile_(nullptr) {
#ifdef _WIN32
        // Get the executable directory
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path logPath = exeDir / "hunter_startup.log";
        logFile_ = fopen(logPath.string().c_str(), "a");
#else
        logFile_ = fopen("runtime/hunter_startup.log", "a");
#endif
        if (logFile_) {
            time_t now = time(nullptr);
            fprintf(logFile_, "\n=== Hunter Startup Log - %s", ctime(&now));
            fflush(logFile_);
        }
    }
    
    ~StartupLogger() {
        if (logFile_) {
            time_t now = time(nullptr);
            fprintf(logFile_, "[%s] Process ending\n", ctime(&now));
            fflush(logFile_);
            fclose(logFile_);
        }
    }
    
    void log(const std::string& message) {
        if (logFile_) {
            time_t now = time(nullptr);
            fprintf(logFile_, "[%s] %s\n", ctime(&now), message.c_str());
            fflush(logFile_);
        }
    }
};

} // namespace

#ifndef __ANDROID__
namespace {

bool isDirWritable(const std::filesystem::path& p) {
    std::string test_path = (p / ".hunter_write_test").string();
    FILE* f = std::fopen(test_path.c_str(), "w");
    if (!f) return false;
    std::fclose(f);
    std::remove(test_path.c_str());
#ifndef _WIN32
    struct stat st;
    if (stat(p.string().c_str(), &st) == 0) {
        // Reject world-writable directories that lack the sticky bit.
        if ((st.st_mode & S_IWOTH) && !(st.st_mode & S_ISVTX)) return false;
    }
#endif
    return true;
}

std::filesystem::path getPerUserDataDir() {
#ifdef _WIN32
    PWSTR path = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path) == S_OK) {
        std::wstring w(path);
        CoTaskMemFree(path);
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], size_needed, nullptr, nullptr);
        return std::filesystem::path(s) / "Hunter";
    }
    return std::filesystem::path(".");
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home && *home) return std::filesystem::path(home) / "Library" / "Application Support" / "Hunter";
    return std::filesystem::path(".");
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return std::filesystem::path(xdg) / "hunter";
    const char* home = std::getenv("HOME");
    if (home && *home) return std::filesystem::path(home) / ".local" / "share" / "hunter";
    return std::filesystem::path(".");
#endif
}

} // namespace
#endif

int main() {
#ifndef __ANDROID__
    {
        std::filesystem::path data_dir;
        std::string exe_dir_str = utils::executableDir();
        std::filesystem::path exe_dir = exe_dir_str.empty() ? std::filesystem::current_path() : std::filesystem::path(exe_dir_str);
        bool writable = isDirWritable(exe_dir);
        if (writable) {
            data_dir = exe_dir;
        } else {
            data_dir = getPerUserDataDir();
            try {
                std::filesystem::create_directories(data_dir);
                std::filesystem::permissions(data_dir,
                    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                    std::filesystem::perm_options::replace);
            } catch (...) {}
        }
        std::string data_dir_str = data_dir.string();
        std::fprintf(stderr, "[startup] data directory: %s\n", data_dir_str.c_str());
#ifdef _WIN32
        SetCurrentDirectoryW(data_dir.wstring().c_str());
#else
        int chdir_rc = chdir(data_dir_str.c_str());
        (void)chdir_rc;
#endif
    }
#endif

#ifndef _WIN32
    // Writing to a closed pipe (engine subprocess, curl socket) must not kill
    // the whole app: ignore SIGPIPE and take the EPIPE error instead.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    // An exception that escapes every catch (most often from a thread body)
    // ends in std::terminate: record it before the process dies.
    std::set_terminate(terminateHandler);
#ifdef _WIN32
    // Install crash handler as early as possible
    SetUnhandledExceptionFilter(crashHandler);
#endif
    
    // Initialize startup logger
    StartupLogger logger;
    
    // Log process start
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
#else
    pid_t pid = getpid();
#endif
    logger.log("process started, PID=" + std::to_string(pid));
    
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    try {
        // One instance per user. Do this before loading the config database or
        // starting the orchestrator, so a second launch costs nothing and cannot
        // race the running instance over runtime/ files.
        gui::SingleInstanceGuard instance_guard;
        bool acquired = instance_guard.acquire();
        logger.log("SingleInstanceGuard::acquire() returned " + std::string(acquired ? "true" : "false"));
        
        if (!acquired) {
            logger.log("Process exiting because another instance holds the lock");
#ifdef _WIN32
            MessageBoxA(NULL, "Hunter is already running.", "Hunter", MB_OK | MB_ICONINFORMATION);
#endif
            std::fprintf(stderr, "Hunter is already running — showing the existing window.\n");
            return 0;
        }

        logger.log("config loading");
        HunterConfig config;
        config.loadFromFile("runtime/hunter_config.json");
        logger.log("config loaded");

        logger.log("orchestrator constructing");
        HunterOrchestrator orchestrator(config);
        logger.log("orchestrator constructed");

        logger.log("creating window");
        gui::HunterGuiApp app(orchestrator);
        app.setShowRequestPoll([&instance_guard]() { return instance_guard.consumeShowRequest(); });
        g_app = &app;

        logger.log("entering main loop");
        int rc = app.run();

        g_app = nullptr;
        return rc;
    }
#ifdef _WIN32
    catch (const std::exception& e) {
        logger.log(std::string("Exception caught: ") + e.what());
        MessageBoxA(NULL, e.what(), "Hunter Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (...) {
        logger.log("Unknown exception caught");
        MessageBoxA(NULL, "An unknown error occurred.", "Hunter Error", MB_OK | MB_ICONERROR);
        return 1;
    }
#else
    catch (const std::exception& e) {
        logger.log(std::string("Exception caught: ") + e.what());
        std::fprintf(stderr, "Exception: %s\n", e.what());
        return 1;
    }
    catch (...) {
        logger.log("Unknown exception caught");
        std::fprintf(stderr, "Unknown exception occurred\n");
        return 1;
    }
#endif
}
