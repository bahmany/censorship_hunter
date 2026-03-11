#include "core/config.h"
#include "core/constants.h"
#include "core/utils.h"
#include "orchestrator/orchestrator.h"

#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <atomic>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

#include <fstream>
#include <ctime>

static std::atomic<bool> g_running{true};
static hunter::HunterOrchestrator* g_orchestrator = nullptr;

static void writeCrashLog(const std::string& reason) {
    try {
        std::ofstream f("runtime/hunter_crash.log", std::ios::app);
        if (f.is_open()) {
            std::time_t now = std::time(nullptr);
            char ts[64];
            std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            f << "[" << ts << "] CRASH: " << reason << std::endl;
        }
    } catch (...) {}
}

#ifdef _WIN32
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "Unhandled SEH code=0x%08lX addr=%p thread=%lu",
             ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress,
             (unsigned long)GetCurrentThreadId());
    std::cerr << "[Hunter] FATAL: " << buf << std::endl;
    writeCrashLog(buf);
    // Signal graceful shutdown instead of hard-terminating
    g_running = false;
    if (g_orchestrator) {
        try { g_orchestrator->stop(); } catch (...) {}
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#ifdef _WIN32
static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        std::cout << "\n[Hunter] Shutting down..." << std::endl;
        g_running = false;
        if (g_orchestrator) g_orchestrator->stop();
        return 1;
    }
    return 0;
}
#else
void signalHandler(int signum) {
    std::cout << "\n[Hunter] Shutting down..." << std::endl;
    g_running = false;
    if (g_orchestrator) g_orchestrator->stop();
}
#endif

void printUsage(const char* prog) {
    std::cout << "Hunter C++ — Autonomous Proxy Config Hunter\n"
              << "Version " << hunter::constants::HUNTER_VERSION << "\n\n"
              << "Usage:\n"
              << "  " << prog << " [options]\n\n"
              << "Options:\n"
              << "  --config <path>      Config file (default: runtime/hunter_config.json)\n"
              << "  --xray <path>        XRay binary path (default: bin/xray.exe)\n"
              << "  --help               Show this help\n\n"
              << "Environment variables:\n"
              << "  HUNTER_GITHUB_BG_CAP       GitHub background fetch cap (default: 150000)\n"
              << "  HUNTER_GITHUB_BG_ENABLED   Enable/disable GitHub BG (default: true)\n"
              << "  HUNTER_BG_VALIDATION_BATCH Background validation batch size (default: 120)\n"
              << "  TELEGRAM_BOT_TOKEN         Telegram bot token\n"
              << "  CHAT_ID                    Telegram chat/group ID\n"
              << std::endl;
}

int main(int argc, char* argv[]) {
    // Parse command line args
    std::string config_path = "runtime/hunter_config.json";
    std::string xray_path;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--xray" && i + 1 < argc) {
            xray_path = argv[++i];
        }
    }

    // Enable ANSI colors on Windows console
#ifdef _WIN32
    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            GetConsoleMode(hOut, &mode);
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        SetConsoleOutputCP(CP_UTF8);
    }
#endif

    // Setup signal handlers
#ifdef _WIN32
    SetConsoleCtrlHandler(consoleHandler, 1);
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#else
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#endif

    // Initialize libcurl globally BEFORE any threads start (not thread-safe)
    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "=== " << hunter::constants::HUNTER_NAME << " v"
              << hunter::constants::HUNTER_VERSION << " ===" << std::endl;

    // Load config
    hunter::HunterConfig config;
    if (hunter::utils::fileExists(config_path)) {
        config.loadFromFile(config_path);
        std::cout << "Config loaded from " << config_path << std::endl;
    } else {
        std::cout << "No config file at " << config_path << ", creating with defaults" << std::endl;
        config.saveToFile(config_path);
    }

    // Override from CLI
    if (!xray_path.empty()) config.set("xray_path", xray_path);

    // Create orchestrator
    hunter::HunterOrchestrator orchestrator(config);
    g_orchestrator = &orchestrator;

    std::cout << "Starting autonomous hunting service..." << std::endl;

    // Stdin reader thread: reads JSON-line commands from Flutter UI process stdin
    std::thread stdin_reader([&orchestrator]() {
        std::string line;
        while (g_running.load() && std::getline(std::cin, line)) {
            if (line.empty()) continue;
            try {
                orchestrator.processStdinCommand(line);
            } catch (...) {}
        }
    });
    stdin_reader.detach();

    // Start orchestrator (blocking)
    try {
        orchestrator.start();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        writeCrashLog(std::string("std::exception: ") + e.what());
    } catch (...) {
        std::cerr << "Fatal error: unknown exception" << std::endl;
        writeCrashLog("unknown exception (catch-all)");
    }

    // Cleanup
    g_orchestrator = nullptr;

    std::cout << "Hunter stopped." << std::endl;
    return 0;
}
