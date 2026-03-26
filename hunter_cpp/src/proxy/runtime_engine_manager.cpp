#include "proxy/runtime_engine_manager.h"

#include "core/utils.h"
#include "proxy/xray_manager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace proxy {

RuntimeEngineManager::RuntimeEngineManager() {
    xray_path_ = "bin/xray.exe";
    singbox_path_ = "bin/sing-box.exe";
    mihomo_path_ = "bin/mihomo-windows-amd64-compatible.exe";
    temp_dir_ = "runtime/engine_tmp";
}

RuntimeEngineManager::~RuntimeEngineManager() {
    stopAll();
}

void RuntimeEngineManager::ensureTempDir() {
    utils::mkdirRecursive(temp_dir_);
}

std::string RuntimeEngineManager::normalizedEngine(const std::string& engine) const {
    std::string out = engine;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (out == "singbox") return "sing-box";
    return out;
}

std::string RuntimeEngineManager::engineBinaryPath(const std::string& engine) const {
    const std::string normalized = normalizedEngine(engine);
    if (normalized == "xray") return xray_path_;
    if (normalized == "sing-box") return singbox_path_;
    if (normalized == "mihomo") return mihomo_path_;
    return "";
}

std::string RuntimeEngineManager::configExtensionForEngine(const std::string& engine) const {
    return normalizedEngine(engine) == "mihomo" ? ".yaml" : ".json";
}

bool RuntimeEngineManager::isEngineAvailable(const std::string& engine) const {
    const std::string path = engineBinaryPath(engine);
    return !path.empty() && utils::fileExists(path);
}

std::string RuntimeEngineManager::generateConfig(const ParsedConfig& parsed, int listen_port, const std::string& engine) const {
    const std::string normalized = normalizedEngine(engine);
    if (normalized == "sing-box") {
        return parsed.toSingBoxConfigJson(listen_port);
    }
    return "";
}

std::string RuntimeEngineManager::resolveEngine(const ParsedConfig& parsed, const std::string& preferred) const {
    const std::string normalized_preferred = normalizedEngine(preferred);
    if (normalized_preferred == "sing-box" && isEngineAvailable(normalized_preferred)) {
        if (!generateConfig(parsed, 10808, normalized_preferred).empty()) {
            return normalized_preferred;
        }
    }

    if (isEngineAvailable("sing-box") && !generateConfig(parsed, 10808, "sing-box").empty()) {
        return "sing-box";
    }
    return "";
}

std::string RuntimeEngineManager::writeConfigFile(const std::string& engine, const std::string& config_text) {
    if (config_text.empty()) return "";
    ensureTempDir();
    const std::string normalized = normalizedEngine(engine);
    const std::string ext = configExtensionForEngine(normalized);
    const std::string filename = temp_dir_ + "/" + normalized + "_" +
                                 utils::sha1Hex(normalized + ":" + config_text).substr(0, 10) + ext;
    std::ofstream f(filename, std::ios::binary);
    if (!f) return "";
    f << config_text;
    return filename;
}

int RuntimeEngineManager::startProcess(const std::string& engine, const std::string& config_path) {
    const std::string normalized = normalizedEngine(engine);
    const std::string binary = engineBinaryPath(normalized);
    if (binary.empty() || !utils::fileExists(binary) || config_path.empty()) {
        return -1;
    }

#ifdef _WIN32
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::string cmd;
    if (normalized == "sing-box") {
        cmd = "\"" + binary + "\" run -c \"" + config_path + "\"";
    } else {
        return -1;
    }

    char cmd_buf[4096];
    strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;

    BOOL ok = CreateProcessA(nullptr, cmd_buf, nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        // Log CreateProcess failure with error code
        std::cerr << "[RuntimeEngineManager] CreateProcess failed for " << engine 
                  << " with error " << err << std::endl;
        return -1;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    const int pid = static_cast<int>(pi.dwProcessId);
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.push_back(pid);
    return pid;
#else
    pid_t pid = fork();
    if (pid == 0) {
        if (normalized == "sing-box") {
            execl(binary.c_str(), "sing-box", "run", "-c", config_path.c_str(), nullptr);
        }
        _exit(1);
    }
    if (pid < 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.push_back(static_cast<int>(pid));
    return static_cast<int>(pid);
#endif
}

void RuntimeEngineManager::stopProcess(int pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        DWORD err = GetLastError();
        std::cerr << "[RuntimeEngineManager] OpenProcess failed for PID " << pid 
                  << " (error=" << err << ")" << std::endl;
        std::lock_guard<std::mutex> lock(mutex_);
        managed_pids_.erase(std::remove(managed_pids_.begin(), managed_pids_.end(), pid), managed_pids_.end());
        return;
    }
    TerminateProcess(h, 0);
    CloseHandle(h);
#else
    kill(pid, SIGTERM);
    usleep(100000);
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, WNOHANG);
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.erase(std::remove(managed_pids_.begin(), managed_pids_.end(), pid), managed_pids_.end());
}

void RuntimeEngineManager::stopAll() {
    std::vector<int> pids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pids = managed_pids_;
        managed_pids_.clear();
    }
    for (int pid : pids) {
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        } else {
            DWORD err = GetLastError();
            std::cerr << "[RuntimeEngineManager] OpenProcess failed for PID " << pid 
                      << " in stopAll (error=" << err << ")" << std::endl;
        }
#else
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, WNOHANG);
#endif
    }
    try {
        for (auto& entry : fs::directory_iterator(temp_dir_)) {
            try { fs::remove(entry.path()); } catch (...) {}
        }
    } catch (...) {}
}

bool RuntimeEngineManager::isProcessAlive(int pid) const {
    if (pid <= 0) return false;
#ifdef _WIN32
    // Windows 7+ compatibility: Try LIMITED first (Vista+), fallback to QUERY_INFORMATION
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        // Fallback for older Windows or restricted processes
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    }
    if (!h) {
        // Final fallback - try SYNCHRONIZE to check if process exists
        h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (h) {
            // Process exists but we can't query status - assume alive
            CloseHandle(h);
            return true;
        }
        return false;
    }
    DWORD exitCode = 0;
    BOOL ok = GetExitCodeProcess(h, &exitCode);
    if (!ok) {
        DWORD err = GetLastError();
        std::cerr << "[RuntimeEngineManager] GetExitCodeProcess failed for PID " << pid << " (error=" << err << ")" << std::endl;
    }
    CloseHandle(h);
    return ok && exitCode == STILL_ACTIVE;
#else
    return kill(pid, 0) == 0;
#endif
}

} // namespace proxy
} // namespace hunter
