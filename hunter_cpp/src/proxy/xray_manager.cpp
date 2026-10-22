#include "proxy/xray_manager.h"
#include "core/utils.h"
#include "core/engine_embed.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace proxy {

namespace {

[[maybe_unused]] bool hasGeositeData() {
    return utils::fileExists("bin/geosite.dat");
}

} // namespace

XRayManager::XRayManager() {
    temp_dir_ = "runtime/xray_tmp";

    // Priority 1: Embedded extraction.
    embed::ensureExtracted();
    std::string embedded = embed::xrayPath();
    if (!embedded.empty() && utils::fileExists(embedded)) {
        xray_path_ = embedded;
        return;
    }

    // Priority 2: env override (absolute paths only).
    const char* env_xray = std::getenv("HUNTER_XRAY_PATH");
    if (env_xray && *env_xray) {
        std::string p(env_xray);
        bool absolute = !p.empty() && (p[0] == '/' || (p.size() > 2 && p[1] == ':' && (p[2] == '\\' || p[2] == '/')));
        if (absolute && utils::fileExists(p)) {
            xray_path_ = p;
            return;
        }
    }

    // Priority 3: safe executable-relative lookup (no bare CWD names).
    std::string found = utils::findEngineBinary("xray");
    if (!found.empty() && utils::fileExists(found)) {
        xray_path_ = found;
        return;
    }
}

XRayManager::~XRayManager() {
    stopAll();
}

bool XRayManager::isAvailable() const {
    return utils::fileExists(xray_path_);
}

void XRayManager::ensureTempDir() {
    utils::mkdirRecursive(temp_dir_);
#ifndef _WIN32
    chmod(temp_dir_.c_str(), 0700);
#endif
}

std::string XRayManager::writeConfigFile(const std::string& json_config) {
    ensureTempDir();
    std::string filename = temp_dir_ + "/xray_" +
                           utils::sha1Hex(json_config).substr(0, 8) + ".json";
#ifndef _WIN32
    int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return "";
    size_t off = 0;
    while (off < json_config.size()) {
        ssize_t n = write(fd, json_config.data() + off, json_config.size() - off);
        if (n <= 0) { if (errno == EINTR) continue; close(fd); return ""; }
        off += (size_t)n;
    }
    close(fd);
    chmod(filename.c_str(), 0600);
#else
    std::ofstream f(filename, std::ios::binary | std::ios::trunc);
    if (f) f << json_config;
#endif
    return filename;
}

int XRayManager::startProcess(const std::string& config_path) {
    if (!isAvailable()) {
        const std::string msg = "[XRay] Binary not found: " + xray_path_;
        utils::LogRingBuffer::instance().push(msg);
        std::cerr << msg << std::endl;
        return -1;
    }

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
    if (!ok) {
        DWORD err = GetLastError();
        const std::string msg = "[XRay] CreateProcess failed for " + xray_path_ +
                                " (config=" + config_path + ", error=" + std::to_string((int)err) + ")";
        utils::LogRingBuffer::instance().push(msg);
        std::cerr << msg << std::endl;
        return -1;
    }

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
    if (pid < 0) {
        const std::string msg = "[XRay] fork failed for " + xray_path_ + " (config=" + config_path + ")";
        utils::LogRingBuffer::instance().push(msg);
        std::cerr << msg << std::endl;
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    managed_pids_.push_back(pid);
    return pid;
#endif
}

void XRayManager::stopProcess(int pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) {
        DWORD err = GetLastError();
        std::cerr << "[XRayManager] OpenProcess failed for PID " << pid 
                  << " (error=" << err << ")" << std::endl;
        std::lock_guard<std::mutex> lock(mutex_);
        managed_pids_.erase(std::remove(managed_pids_.begin(), managed_pids_.end(), pid),
                             managed_pids_.end());
        return;
    }
    TerminateProcess(h, 0);
    CloseHandle(h);
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
    // Windows 7+ compatibility: Try LIMITED first (Vista+), fallback to QUERY_INFORMATION
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) {
        // Fallback for older Windows
        h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, (DWORD)pid);
    }
    if (!h) {
        // Final fallback - at least try SYNCHRONIZE to check if process exists
        h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
        if (h) {
            // Process exists but we can't query info - assume alive
            CloseHandle(h);
            return true;
        }
        return false;
    }
    DWORD exitCode = 0;
    BOOL ok = GetExitCodeProcess(h, &exitCode);
    if (!ok) {
        DWORD err = GetLastError();
        std::cerr << "[XRayManager] GetExitCodeProcess failed for PID " << pid << " (error=" << err << ")" << std::endl;
    }
    CloseHandle(h);
    return ok && exitCode == STILL_ACTIVE;
#else
    return kill(pid, 0) == 0;
#endif
}

std::string XRayManager::fragmentOutboundJson() {
    return "{\"protocol\":\"freedom\",\"tag\":\"fragment-out\",\"settings\":{\"domainStrategy\":\"AsIs\""
           ",\"fragment\":{\"packets\":\"tlshello\",\"length\":\"50-100\",\"interval\":\"30-50\"}}}";
}

bool XRayManager::needsFragmentOutbound(const ParsedConfig& parsed) {
    return parsed.security == "tls";
}

std::string XRayManager::buildProxyOutbound(const ParsedConfig& parsed, const std::string& tag) {
    std::string ob = parsed.toXrayOutboundJson(0);
    if (ob.empty()) return "";
    const std::string tag_needle = "\"tag\":\"proxy\"";
    size_t tag_pos = ob.find(tag_needle);
    if (tag_pos != std::string::npos && tag != "proxy") {
        ob.replace(tag_pos, tag_needle.size(), "\"tag\":\"" + tag + "\"");
    }
    if (needsFragmentOutbound(parsed)) {
        // xray has no tlsSettings.fragment; the working mechanism is a freedom dialer.
        const std::string stream_needle = "\"streamSettings\":{";
        size_t sp = ob.find(stream_needle);
        if (sp != std::string::npos) {
            ob.insert(sp + stream_needle.size(), "\"sockopt\":{\"dialerProxy\":\"fragment-out\"},");
        }
    }
    return ob;
}

std::string XRayManager::generateConfig(const ParsedConfig& parsed, int socks_port, int http_port) {
    std::string outbound = buildProxyOutbound(parsed, "proxy");
    if (outbound.empty()) return "";  // Unsupported protocol/cipher/transport

    std::ostringstream ss;
    const bool use_mixed_inbound = (http_port > 0 && http_port == socks_port);
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{"
       <<     "\"tag\":\"dns-module\","
       <<     "\"servers\":[\"1.1.1.1\",\"8.8.8.8\",\"https+local://1.1.1.1/dns-query\"],"
       <<     "\"queryStrategy\":\"UseIPv4\","
       <<     "\"disableCache\":false"
       <<   "},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"mixed-in\","
       <<     "\"port\":" << socks_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"" << (use_mixed_inbound ? "mixed" : "socks") << "\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\",\"quic\"],\"routeOnly\":true}"
       <<   "}],\n";

    // Outbounds with DPI bypass + blackhole to prevent traffic leakage
    ss << "  \"outbounds\":[" << outbound 
       << (needsFragmentOutbound(parsed) ? "," + fragmentOutboundJson() : std::string())
       << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}"
       << ",{\"protocol\":\"blackhole\",\"tag\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";
    ss << "],\n";

    // Build inbound tags for DNS routing
    std::string client_inbounds = "\"mixed-in\"";

    // Rules are evaluated in order; the last rule sends all remaining client
    // traffic through the proxy. Nothing falls through to a direct egress.
    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
       // 1. Client DNS queries -> dns-out (resolved by xray's DNS module)
       <<     "{\"type\":\"field\",\"inboundTag\":[" << client_inbounds << "],\"port\":53,\"outboundTag\":\"dns-out\"},"
       // 2. The DNS module itself resolves through the proxy (no plaintext local DNS)
       <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"outboundTag\":\"proxy\"},"
       // 3. Private/local destinations stay local
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"},"
       // 4. Everything else from the client -> proxy
       <<     "{\"type\":\"field\",\"inboundTag\":[" << client_inbounds << "],\"outboundTag\":\"proxy\"}"
       << "]}\n";
    ss << "}\n";
    return ss.str();
}

std::string XRayManager::generateTestConfig(const ParsedConfig& parsed, int socks_port) {
    // Lean test config (see header). Uses the SAME outbound construction as the live
    // config (buildProxyOutbound, incl. TLS-fragment policy). Test traffic can never
    // leave through a direct/private egress: everything not proxied is blackholed.
    std::string outbound = buildProxyOutbound(parsed, "proxy");
    if (outbound.empty()) return "";

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{\"servers\":[\"1.1.1.1\",\"8.8.8.8\"],\"queryStrategy\":\"UseIPv4\"},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"test-in\","
       <<     "\"port\":" << socks_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"socks\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\",\"quic\"],\"routeOnly\":true}"
       <<   "}],\n"
       << "  \"outbounds\":[" << outbound
       << (needsFragmentOutbound(parsed) ? "," + fragmentOutboundJson() : std::string())
       << ",{\"protocol\":\"blackhole\",\"tag\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}"
       << "],\n"
       << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
       <<     "{\"type\":\"field\",\"inboundTag\":[\"test-in\"],\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"blackhole\"},"
       <<     "{\"type\":\"field\",\"inboundTag\":[\"test-in\"],\"outboundTag\":\"proxy\"}"
       <<   "]}\n"
       << "}\n";
    return ss.str();
}

std::string XRayManager::generateBalancedConfig(
    const std::vector<std::pair<ParsedConfig, int>>& configs, int listen_port, int http_port) {

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{"
       <<     "\"tag\":\"dns-module\","
       <<     "\"servers\":[\"1.1.1.1\",\"8.8.8.8\",\"https+local://1.1.1.1/dns-query\"],"
       <<     "\"queryStrategy\":\"UseIPv4\","
       <<     "\"disableCache\":false"
       <<   "},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"mixed-in\","
       <<     "\"port\":" << listen_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"mixed\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\",\"quic\"],\"routeOnly\":true}"
       <<   "}],\n"
       << "  \"outbounds\":[";

    std::vector<std::string> outbound_tags;
    bool first = true;
    int idx = 0;
    bool balanced_need_fragment = false;
    for (auto& [parsed, port] : configs) {
        if (!first) ss << ",";
        first = false;
        std::string tag = "proxy-" + std::to_string(idx);
        outbound_tags.push_back(tag);
        std::string ob = buildProxyOutbound(parsed, tag);
        if (needsFragmentOutbound(parsed)) balanced_need_fragment = true;
        ss << ob;
        idx++;
    }
    if (balanced_need_fragment) ss << "," << fragmentOutboundJson();
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}"
       << ",{\"protocol\":\"blackhole\",\"tag\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";

    ss << "],\n";

    // Build inbound tags for DNS routing
    std::string client_inbounds = "\"mixed-in\"";

    // Balancer + routing with DNS rules + smart routing
    // final=blackhole ensures no traffic leaks to direct if balancer fails
    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"final\":\"blackhole\",\"rules\":["
       <<     "{\"type\":\"field\",\"inboundTag\":[" << client_inbounds << "],\"port\":53,\"outboundTag\":\"dns-out\"},"
       <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"balancerTag\":\"proxy-balancer\"},"
       <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"}";
    ss << ",{\"type\":\"field\",\"network\":\"tcp,udp\",\"balancerTag\":\"proxy-balancer\"}"
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

std::string XRayManager::generateLocalSocksBalancedConfig(
    const std::vector<int>& backend_ports, int listen_port, int http_port) {

    if (backend_ports.empty()) return "";

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"inbounds\":[{"
       <<     "\"tag\":\"mixed-in\"," 
       <<     "\"port\":" << listen_port << ","
       <<     "\"listen\":\"127.0.0.1\","
       <<     "\"protocol\":\"mixed\","
       <<     "\"settings\":{\"udp\":true},"
       <<     "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\",\"quic\"],\"routeOnly\":true}"
       <<   "}],\n"
       << "  \"outbounds\":[";

    bool first = true;
    std::vector<std::string> outbound_tags;
    for (size_t i = 0; i < backend_ports.size(); ++i) {
        if (!first) ss << ",";
        first = false;
        const std::string tag = "proxy-" + std::to_string(i);
        outbound_tags.push_back(tag);
        ss << "{\"protocol\":\"socks\",\"tag\":\"" << tag << "\",\"settings\":{\"servers\":[{\"address\":\"127.0.0.1\",\"port\":" << backend_ports[i] << "}]}}";
    }
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"blackhole\",\"tag\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";

    ss << "],\n";

    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"final\":\"blackhole\",\"rules\":["
       <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"network\":\"tcp,udp\",\"balancerTag\":\"proxy-balancer\"}"
       <<   "],\"balancers\":[{\"tag\":\"proxy-balancer\",\"selector\":[";

    first = true;
    for (const auto& tag : outbound_tags) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << tag << "\"";
    }
    ss << "],\"strategy\":{\"type\":\"leastPing\"}}]},\n";

    ss << "  \"observatory\":{\"subjectSelector\":[";
    first = true;
    for (const auto& tag : outbound_tags) {
        if (!first) ss << ",";
        first = false;
        ss << "\"" << tag << "\"";
    }
    ss << "],\"probeURL\":\"http://1.1.1.1/generate_204\",\"probeInterval\":\"30s\"}\n";
    ss << "}";
    return ss.str();
}

std::string XRayManager::generateBatchSpeedtestConfig(
    const std::vector<std::pair<ParsedConfig, int>>& all_configs) {

    // Only configs xray can express get an inbound/outbound/rule triple.
    std::vector<std::pair<ParsedConfig, int>> configs;
    for (const auto& c : all_configs)
        if (!buildProxyOutbound(c.first, "proxy-" + std::to_string(c.second)).empty()) configs.push_back(c);
    if (configs.empty()) return "";

    std::ostringstream ss;
    ss << "{\n"
       << "  \"log\":{\"loglevel\":\"warning\"},\n"
       << "  \"dns\":{"
       <<     "\"tag\":\"dns-module\","
       <<     "\"servers\":[\"1.1.1.1\",\"8.8.8.8\",\"https+local://1.1.1.1/dns-query\"],"
       <<     "\"queryStrategy\":\"UseIPv4\","
       <<     "\"disableCache\":false"
       <<   "},\n";

    // ═══ Inbounds: one mixed-protocol inbound per config ═══
    ss << "  \"inbounds\":[";
    bool first = true;
    for (auto& [parsed, port] : configs) {
        if (!first) ss << ",";
        first = false;
        std::string tag = "test-" + std::to_string(port);
        ss << "{"
           <<   "\"tag\":\"" << tag << "\","
           <<   "\"port\":" << port << ","
           <<   "\"listen\":\"127.0.0.1\","
           <<   "\"protocol\":\"mixed\","
           <<   "\"settings\":{\"udp\":true},"
           <<   "\"sniffing\":{\"enabled\":true,\"destOverride\":[\"http\",\"tls\",\"quic\"],\"routeOnly\":true}"
           << "}";
    }
    ss << "],\n";

    // ═══ Outbounds: one proxy outbound per config + direct + dns-out ═══
    ss << "  \"outbounds\":[";
    first = true;
    std::vector<std::string> outbound_tags;
    bool need_fragment = false;
    for (auto& [parsed, port] : configs) {
        std::string ob_tag = "proxy-" + std::to_string(port);
        std::string ob = buildProxyOutbound(parsed, ob_tag);
        if (ob.empty()) continue;
        outbound_tags.push_back(ob_tag);
        if (needsFragmentOutbound(parsed)) need_fragment = true;

        if (!first) ss << ",";
        first = false;
        ss << ob;
    }

    // Fragment dialer (anti-DPI) only when some outbound needs it; no direct egress at all.
    if (need_fragment) ss << "," << fragmentOutboundJson();
    ss << ",{\"protocol\":\"blackhole\",\"tag\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";
    ss << "],\n";

    // ═══ Routing: each inbound tag → its own outbound tag ═══
    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":[";

    // Per-config routing: inbound → outbound
    first = true;
    for (auto& [parsed, port] : configs) {
        std::string in_tag = "test-" + std::to_string(port);
        std::string out_tag = "proxy-" + std::to_string(port);
        if (!first) ss << ",";
        first = false;
        ss << "{\"type\":\"field\",\"inboundTag\":[\"" << in_tag << "\"],\"outboundTag\":\"" << out_tag << "\"}";
    }

    // Anything else (private ranges, unrouted inbounds) is blackholed: never direct.
    ss << ",{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"blackhole\"}";
    ss << "]}\n";
    ss << "}";
    return ss.str();
}

} // namespace proxy
} // namespace hunter
