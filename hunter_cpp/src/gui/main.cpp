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

int main() {
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
