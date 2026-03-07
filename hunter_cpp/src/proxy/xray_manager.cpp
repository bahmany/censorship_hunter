#include "proxy/xray_manager.h"
#include "core/utils.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace proxy {

XRayManager::XRayManager() {
    xray_path_ = "bin/xray.exe";
    temp_dir_ = "runtime/xray_tmp";
}

XRayManager::~XRayManager() {
    stopAll();
}

bool XRayManager::isAvailable() const {
    return utils::fileExists(xray_path_);
}

void XRayManager::ensureTempDir() {
    utils::mkdirRecursive(temp_dir_);
}

std::string XRayManager::writeConfigFile(const std::string& json_config) {
    ensureTempDir();
    std::string filename = temp_dir_ + "/xray_" +
                           utils::sha1Hex(json_config).substr(0, 8) + ".json";
    std::ofstream f(filename);
    if (f) f << json_config;
    return filename;
}

int XRayManager::startProcess(const std::string& config_path) {
    if (!isAvailable()) return -1;

#ifdef _WIN32
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::string cmd = xray_path_ + " run -c \"" + config_path + "\"";
    char cmd_buf[4096];
    strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;

    BOOL ok = CreateProcessA(nullptr, cmd_buf, nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return -1;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    int pid = (int)pi.dwProcessId;
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.push_back(pid);
    return pid;
#else
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        execl(xray_path_.c_str(), "xray", "run", "-c", config_path.c_str(), nullptr);
        _exit(1);
    }
    if (pid < 0) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.push_back(pid);
    return pid;
#endif
}

void XRayManager::stopProcess(int pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
#else
    kill(pid, SIGTERM);
    usleep(100000); // 100ms
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, WNOHANG);
#endif
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.erase(std::remove(managed_pids_.begin(), managed_pids_.end(), pid),
                         managed_pids_.end());
}

void XRayManager::stopAll() {
    std::vector<int> pids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pids = managed_pids_;
        managed_pids_.clear();
    }
    for (int pid : pids) {
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
        if (h) { TerminateProcess(h, 0); CloseHandle(h); }
#else
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, WNOHANG);
#endif
    }
    // Clean temp configs
    try {
        for (auto& entry : fs::directory_iterator(temp_dir_)) {
            try { fs::remove(entry.path()); } catch (...) {}
        }
    } catch (...) {}
}

bool XRayManager::isProcessAlive(int pid) const {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD exitCode = 0;
    GetExitCodeProcess(h, &exitCode);
    CloseHandle(h);
    return exitCode == STILL_ACTIVE;
#else
    return kill(pid, 0) == 0;
#endif
}

std::string XRayManager::generateConfig(const ParsedConfig& parsed, int socks_port) {
    std::string outbound = parsed.toXrayOutboundJson(socks_port);
    if (outbound.empty()) return "";  // Unsupported protocol/cipher/transport

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{"
       <<     "\"tag\":\"dns-module\","
       <<     "\"servers\":[\"1.1.1.1\",\"8.8.8.8\",\"https+local://1.1.1.1/dns-query\"],"
       <<     "\"queryStrategy\":\"UseIPv4\""
       <<   "},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"socks-in\","
       <<     "\"port\":" << socks_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"socks\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\"],\"routeOnly\":true}"
       <<   "}],\n"
       << "  \"outbounds\":[" << outbound 
       << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}],\n"
       << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
       <<     "{\"type\":\"field\",\"inboundTag\":[\"socks-in\"],\"port\":53,\"outboundTag\":\"dns-out\"},"
       <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"outboundTag\":\"proxy\"},"
       <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\"],\"outboundTag\":\"direct\"}"
       <<   "]}\n"
       << "}";
    return ss.str();
}

std::string XRayManager::generateBalancedConfig(
    const std::vector<std::pair<ParsedConfig, int>>& configs, int listen_port) {

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{"
       <<     "\"tag\":\"dns-module\","
       <<     "\"servers\":[\"1.1.1.1\",\"8.8.8.8\",\"https+local://1.1.1.1/dns-query\"],"
       <<     "\"queryStrategy\":\"UseIPv4\""
       <<   "},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"socks-in\","
       <<     "\"port\":" << listen_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"socks\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\"],\"routeOnly\":true}"
       <<   "}],\n"
       << "  \"outbounds\":[";

    std::vector<std::string> outbound_tags;
    bool first = true;
    int idx = 0;
    for (auto& [parsed, port] : configs) {
        if (!first) ss << ",";
        first = false;
        std::string tag = "proxy-" + std::to_string(idx);
        outbound_tags.push_back(tag);
        std::string ob = parsed.toXrayOutboundJson(port);
        // Replace existing "tag":"proxy" with indexed tag
        std::string tag_needle = "\"tag\":\"proxy\"";
        size_t tag_pos = ob.find(tag_needle);
        if (tag_pos != std::string::npos) {
            ob.replace(tag_pos, tag_needle.size(), "\"tag\":\"" + tag + "\"");
        }
        ss << ob;
        idx++;
    }
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}],\n";

    // Balancer + routing with DNS rules
    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
       <<     "{\"type\":\"field\",\"inboundTag\":[\"socks-in\"],\"port\":53,\"outboundTag\":\"dns-out\"},"
       <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"balancerTag\":\"proxy-balancer\"},"
       <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\"],\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"network\":\"tcp,udp\",\"balancerTag\":\"proxy-balancer\"}"
       << "],\"balancers\":[{\"tag\":\"proxy-balancer\",\"selector\":[";
    first = true;
    for (auto& t : outbound_tags) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << t << "\"";
    }
    ss << "],\"strategy\":{\"type\":\"leastPing\"}}]},\n";

    // Observatory
    ss << "  \"observatory\":{\"subjectSelector\":[";
    first = true;
    for (auto& t : outbound_tags) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << t << "\"";
    }
    ss << "],\"probeURL\":\"http://1.1.1.1/generate_204\",\"probeInterval\":\"30s\"}\n";
    ss << "}";
    return ss.str();
}

} // namespace proxy
} // namespace hunter
