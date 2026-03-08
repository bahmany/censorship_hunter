#include "core/config.h"
#include "core/constants.h"
#include "core/utils.h"
#include "orchestrator/orchestrator.h"

#include <iostream>
#include <string>
#include <signal.h>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

static std::atomic<bool> g_running{true};
static hunter::HunterOrchestrator* g_orchestrator = nullptr;

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
#else
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#endif

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

    // Start orchestrator (blocking)
    try {
        orchestrator.start();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
    }

    // Cleanup
    g_orchestrator = nullptr;

    std::cout << "Hunter stopped." << std::endl;
    return 0;
}
