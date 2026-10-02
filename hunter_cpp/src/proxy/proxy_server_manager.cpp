#include "proxy/proxy_server_manager.h"
#include "proxy/xray_manager.h"
#include "network/uri_parser.h"
#include "core/utils.h"
#include "core/constants.h"
#include "core/engine_embed.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

namespace hunter {
namespace proxy {

ProxyServerManager::ProxyServerManager() {
    resolveEnginePaths();
}

ProxyServerManager::~ProxyServerManager() {
    stopAll();
}

void ProxyServerManager::resolveEnginePaths() {
    if (paths_resolved_) return;
    paths_resolved_ = true;

    // Priority 1: Embedded or externally-placed engines (single-file build / Android).
    embed::ensureExtracted();
    {
        std::string ex = embed::xrayPath();
        std::string es = embed::singBoxPath();
        if (!ex.empty() && utils::fileExists(ex))    xray_path_    = ex;
        if (!es.empty() && utils::fileExists(es))    singbox_path_ = es;
        if (!xray_path_.empty() && !singbox_path_.empty()) return;
    }

    // Priority 2: env overrides.
    const char* env_xray = std::getenv("HUNTER_XRAY_PATH");
    const char* env_singbox = std::getenv("HUNTER_SINGBOX_PATH");
    if (env_xray && *env_xray && utils::fileExists(env_xray))
        xray_path_ = env_xray;
    if (env_singbox && *env_singbox && utils::fileExists(env_singbox))
        singbox_path_ = env_singbox;

    // Priority 3: classic bin/ lookup for anything not yet resolved.
#ifdef _WIN32
    if (xray_path_.empty()) {
        xray_path_ = "bin/xray.exe";
        if (!utils::fileExists(xray_path_) && utils::fileExists("xray.exe"))
            xray_path_ = "xray.exe";
    }
    if (singbox_path_.empty()) {
        singbox_path_ = "bin/sing-box.exe";
        if (!utils::fileExists(singbox_path_) && utils::fileExists("sing-box.exe"))
            singbox_path_ = "sing-box.exe";
    }
#else
    auto resolvePath = [](const char* env_val,
                          std::initializer_list<const char*> candidates) -> std::string {
        if (env_val && *env_val && utils::fileExists(env_val)) return env_val;
        for (const char* c : candidates) {
            if (utils::fileExists(c)) return c;
        }
        return candidates.begin()[0];
    };

    if (xray_path_.empty())
        xray_path_    = resolvePath(env_xray,    {"bin/xray",    "xray",    "./xray",    "/app/bin/xray"});
    if (singbox_path_.empty())
        singbox_path_ = resolvePath(env_singbox, {"bin/sing-box", "sing-box", "./sing-box", "/app/bin/sing-box"});
#endif
}

int ProxyServerManager::findFreePort() const {
    for (int port = PORT_RANGE_START; port <= PORT_RANGE_END; port++) {
        // Check if already used by one of our instances
        bool used_by_us = false;
        for (const auto& [uri, inst] : instances_) {
            if (inst.port == port && inst.status != ProxyStatus::Stopped) {
                used_by_us = true;
                break;
            }
        }
        if (used_by_us) continue;

        // Check if the port is actually free on the system
        if (utils::isPortFree(port)) return port;
    }
    return 0;  // No free port
}

std::string ProxyServerManager::generateConfig(const std::string& uri, int socks_port,
                                                  std::string& engine_out) {
    auto parsed_opt = network::UriParser::parse(uri);
    if (!parsed_opt.has_value() || !parsed_opt->isValid()) {
        return "";
    }
    ParsedConfig config = parsed_opt.value();

    // Try xray first (most configs work with xray)
    std::string xray_config = XRayManager::generateTestConfig(config, socks_port);
    if (!xray_config.empty() && utils::fileExists(xray_path_)) {
        engine_out = "xray";
        return xray_config;
    }

    // Fallback: try sing-box config (supports hysteria2, tuic, etc.)
    std::string singbox_config = config.toSingBoxConfigJson(socks_port);
    if (!singbox_config.empty() && utils::fileExists(singbox_path_)) {
        engine_out = "sing-box";
        return singbox_config;
    }

    // If xray config was generated but xray binary is missing, try it anyway
    if (!xray_config.empty()) {
        engine_out = "xray";
        return xray_config;
    }
    // If sing-box config was generated but sing-box binary is missing
    if (!singbox_config.empty()) {
        engine_out = "sing-box";
        return singbox_config;
    }

    return "";
}

int ProxyServerManager::startProxy(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);

    // If already running, return the existing port
    auto it = instances_.find(uri);
    if (it != instances_.end() && it->second.status == ProxyStatus::Running) {
        return it->second.port;
    }

    // Find a free port
    int port = findFreePort();
    if (port == 0) {
        if (it != instances_.end()) {
            it->second.status = ProxyStatus::Error;
            it->second.error_message = "No free ports in range 3110-3120";
        } else {
            ProxyInstance inst;
            inst.uri = uri;
            inst.status = ProxyStatus::Error;
            inst.error_message = "No free ports in range 3110-3120";
            instances_[uri] = inst;
        }
        utils::LogRingBuffer::instance().push(
            "[ProxyServer] No free ports in range 3110-3120 for proxy");
        return 0;
    }

    // Generate config (tries xray first, then sing-box)
    std::string engine_name;
    std::string config_json = generateConfig(uri, port, engine_name);
    if (config_json.empty()) {
        ProxyInstance inst;
        inst.uri = uri;
        inst.port = port;
        inst.status = ProxyStatus::Error;
        inst.error_message = "Failed to generate proxy config (unsupported protocol?)";
        instances_[uri] = inst;
        utils::LogRingBuffer::instance().push(
            "[ProxyServer] Failed to generate config for port " + std::to_string(port));
        return 0;
    }

    // Write config to temp file
    std::string config_path = "runtime/proxy_server_" + std::to_string(port) + ".json";
    try { utils::mkdirRecursive("runtime"); } catch (...) {}
    if (!utils::saveJsonFile(config_path, config_json)) {
        ProxyInstance inst;
        inst.uri = uri;
        inst.port = port;
        inst.status = ProxyStatus::Error;
        inst.error_message = "Failed to write config file";
        instances_[uri] = inst;
        return 0;
    }

    // Use the engine selected by generateConfig
    std::string engine_path = (engine_name == "sing-box") ? singbox_path_ : xray_path_;
    if (!utils::fileExists(engine_path)) {
        ProxyInstance inst;
        inst.uri = uri;
        inst.port = port;
        inst.status = ProxyStatus::Error;
        inst.error_message = engine_name + " binary not found: " + engine_path;
        instances_[uri] = inst;
        std::remove(config_path.c_str());
        utils::LogRingBuffer::instance().push(
            "[ProxyServer] " + engine_name + " binary not found for port " + std::to_string(port));
        return 0;
    }

    // Create/update the instance record
    ProxyInstance& inst = instances_[uri];
    inst.uri = uri;
    inst.port = port;
    inst.engine = engine_name;
    inst.config_path = config_path;
    inst.status = ProxyStatus::Starting;
    inst.error_message.clear();
    inst.started_at = utils::nowTimestamp();
    // Reset traffic counters
    inst.bytes_in = 0;
    inst.bytes_out = 0;
    inst.last_rchar = 0;
    inst.last_wchar = 0;
    inst.last_traffic_poll = 0.0;

#ifdef _WIN32
    // Windows: CreateProcess
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + engine_path + "\" run -c \"" + config_path + "\"";
    if (!CreateProcessA(NULL, &cmd[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        inst.status = ProxyStatus::Error;
        inst.error_message = "Failed to create process";
        std::remove(config_path.c_str());
        return 0;
    }
    CloseHandle(pi.hThread);
    inst.pid = (int)pi.dwProcessId;
    // Store the process handle for later termination
    // (We'll use the PID + OpenProcess for termination on Windows)
#else
    // Unix: fork + execl
    pid_t pid = fork();
    if (pid == 0) {
        // Child process
        // Redirect stdout/stderr to /dev/null to suppress engine logs
        FILE* devnull_out = freopen("/dev/null", "w", stdout);
        (void)devnull_out;
        FILE* devnull_err = freopen("/dev/null", "w", stderr);
        (void)devnull_err;
        execl(engine_path.c_str(), engine_name.c_str(), "run", "-c", config_path.c_str(), NULL);
        // If we get here, execl failed
        _exit(127);
    } else if (pid < 0) {
        inst.status = ProxyStatus::Error;
        inst.error_message = "Failed to fork process";
        std::remove(config_path.c_str());
        return 0;
    }
    inst.pid = (int)pid;
#endif

    // Wait briefly for the proxy to start listening
    bool port_alive = false;
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (utils::isPortAlive(port, 500)) {
            port_alive = true;
            break;
        }
#ifndef _WIN32
        // Check if child died early
        int status = 0;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                inst.status = ProxyStatus::Error;
                inst.error_message = engine_name + " binary not found: " + engine_path;
                std::remove(config_path.c_str());
                utils::LogRingBuffer::instance().push(
                    "[ProxyServer] " + engine_name + " binary not found");
                return 0;
            }
            break;
        }
#endif
    }

    if (port_alive) {
        inst.status = ProxyStatus::Running;
        std::ostringstream ss;
        ss << "[ProxyServer] Started proxy on port " << port
           << " via " << engine_name << " for " << uri.substr(0, 40) << "...";
        utils::LogRingBuffer::instance().push(ss.str());
        return port;
    } else {
        inst.status = ProxyStatus::Error;
        inst.error_message = "Proxy engine failed to start (port not listening)";
        // Kill the process if it's still running
        killProcess(inst.pid);
        inst.pid = 0;
        std::remove(config_path.c_str());
        utils::LogRingBuffer::instance().push(
            "[ProxyServer] Failed to start proxy on port " + std::to_string(port));
        return 0;
    }
}

void ProxyServerManager::killProcess(int pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (hProc) {
        TerminateProcess(hProc, 0);
        CloseHandle(hProc);
    }
#else
    // Use the bounded killAndWait helper — never blocks indefinitely even if
    // the engine ignores SIGTERM or is stuck in uninterruptible sleep.
    utils::killAndWait(pid, 2000);
#endif
}

bool ProxyServerManager::isProcessAlive(int pid) const {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!hProc) return false;
    DWORD exit_code = 0;
    GetExitCodeProcess(hProc, &exit_code);
    CloseHandle(hProc);
    return exit_code == STILL_ACTIVE;
#else
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);
    // If waitpid returns 0, the process is still running
    // If it returns pid, the process has exited
    // If it returns -1 with ECHILD, the process doesn't exist (already reaped)
    if (w == 0) return true;
    return false;
#endif
}

bool ProxyServerManager::stopProxy(const std::string& uri) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    if (it == instances_.end()) return false;
    if (it->second.status == ProxyStatus::Stopped) return false;

    ProxyInstance& inst = it->second;
    if (inst.pid > 0) {
        killProcess(inst.pid);
        inst.pid = 0;
    }
    if (!inst.config_path.empty()) {
        std::remove(inst.config_path.c_str());
    }
    int port = inst.port;
    inst.status = ProxyStatus::Stopped;
    inst.port = 0;
    inst.config_path.clear();

    utils::LogRingBuffer::instance().push(
        "[ProxyServer] Stopped proxy on port " + std::to_string(port));
    return true;
}

bool ProxyServerManager::stopProxyByPort(int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [uri, inst] : instances_) {
        if (inst.port == port && inst.status != ProxyStatus::Stopped) {
            if (inst.pid > 0) {
                killProcess(inst.pid);
                inst.pid = 0;
            }
            if (!inst.config_path.empty()) {
                std::remove(inst.config_path.c_str());
            }
            inst.status = ProxyStatus::Stopped;
            inst.port = 0;
            inst.config_path.clear();
            utils::LogRingBuffer::instance().push(
                "[ProxyServer] Stopped proxy on port " + std::to_string(port));
            return true;
        }
    }
    return false;
}

void ProxyServerManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [uri, inst] : instances_) {
        if (inst.status == ProxyStatus::Stopped) continue;
        if (inst.pid > 0) {
            killProcess(inst.pid);
            inst.pid = 0;
        }
        if (!inst.config_path.empty()) {
            std::remove(inst.config_path.c_str());
        }
        inst.status = ProxyStatus::Stopped;
        inst.port = 0;
        inst.config_path.clear();
    }
    utils::LogRingBuffer::instance().push("[ProxyServer] Stopped all proxy servers");
}

bool ProxyServerManager::isRunning(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    return it != instances_.end() && it->second.status == ProxyStatus::Running;
}

int ProxyServerManager::getPort(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    if (it != instances_.end() && it->second.status == ProxyStatus::Running) {
        return it->second.port;
    }
    return 0;
}

ProxyStatus ProxyServerManager::getStatus(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    if (it != instances_.end()) return it->second.status;
    return ProxyStatus::Stopped;
}

std::string ProxyServerManager::getError(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    if (it != instances_.end()) return it->second.error_message;
    return "";
}

std::vector<ProxyInstance> ProxyServerManager::getInstances() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ProxyInstance> result;
    result.reserve(instances_.size());
    for (const auto& [uri, inst] : instances_) {
        result.push_back(inst);
    }
    return result;
}

void ProxyServerManager::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    double now = utils::nowTimestamp();
    for (auto& [uri, inst] : instances_) {
        if (inst.status != ProxyStatus::Running) continue;
        if (inst.pid > 0 && !isProcessAlive(inst.pid)) {
            // Process died — mark as error
            inst.status = ProxyStatus::Error;
            inst.error_message = "Proxy process exited unexpectedly";
            if (!inst.config_path.empty()) {
                std::remove(inst.config_path.c_str());
            }
            inst.pid = 0;
            utils::LogRingBuffer::instance().push(
                "[ProxyServer] Proxy on port " + std::to_string(inst.port) + " died");
            continue;
        }
        // Update traffic counters every 2 seconds
        if (inst.pid > 0 && (now - inst.last_traffic_poll) >= 2.0) {
            auto [rchar, wchar] = readProcessIo(inst.pid);
            if (inst.last_rchar == 0 && inst.last_wchar == 0) {
                // First reading — store baseline
                inst.last_rchar = rchar;
                inst.last_wchar = wchar;
            } else {
                // Compute delta (bytes transferred since last poll)
                unsigned long long delta_r = (rchar > inst.last_rchar) ? (rchar - inst.last_rchar) : 0;
                unsigned long long delta_w = (wchar > inst.last_wchar) ? (wchar - inst.last_wchar) : 0;
                inst.bytes_in += delta_r;
                inst.bytes_out += delta_w;
                inst.last_rchar = rchar;
                inst.last_wchar = wchar;
            }
            inst.last_traffic_poll = now;
        }
    }
}

std::pair<unsigned long long, unsigned long long> ProxyServerManager::getTraffic(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(uri);
    if (it != instances_.end() && it->second.status == ProxyStatus::Running) {
        return {it->second.bytes_in, it->second.bytes_out};
    }
    return {0, 0};
}

std::pair<unsigned long long, unsigned long long> ProxyServerManager::readProcessIo(int pid) const {
    if (pid <= 0) return {0, 0};
#ifdef __linux__
    std::string path = "/proc/" + std::to_string(pid) + "/io";
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {0, 0};
    unsigned long long rchar = 0, wchar = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.compare(0, 6, "rchar:") == 0) {
            rchar = std::stoull(line.substr(6));
        } else if (line.compare(0, 6, "wchar:") == 0) {
            wchar = std::stoull(line.substr(6));
        }
    }
    return {rchar, wchar};
#else
    return {0, 0};
#endif
}

} // namespace proxy
} // namespace hunter
