#include "proxy/xray_manager.h"
#include "core/utils.h"

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
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace proxy {

namespace {

bool hasGeositeData() {
    return utils::fileExists("bin/geosite.dat");
}

} // namespace

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

std::string XRayManager::generateConfig(const ParsedConfig& parsed, int socks_port, int http_port) {
    std::string outbound = parsed.toXrayOutboundJson(socks_port);
    if (outbound.empty()) return "";  // Unsupported protocol/cipher/transport

    // Apply TLS fragmentation for Iranian DPI bypass
    // This splits ClientHello into small chunks to evade SNI-based filtering
    const bool is_tls = (parsed.security == "tls");
    const bool is_reality = (parsed.security == "reality");
    
    // Inject fragment settings into the outbound's streamSettings
    if (is_tls && outbound.find("\"streamSettings\"") != std::string::npos) {
        // Find tlsSettings and inject fragment
        size_t tls_pos = outbound.find("\"tlsSettings\"");
        if (tls_pos != std::string::npos) {
            size_t brace_pos = outbound.find("{", tls_pos);
            if (brace_pos != std::string::npos) {
                std::string fragment_json = 
                "\"fragment\":{\"packets\":\"tlshello\",\"length\":\"50-100\",\"interval\":\"30-50\"},";
                outbound.insert(brace_pos + 1, fragment_json);
            }
        }
    }

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

    // Outbounds with DPI bypass
    ss << "  \"outbounds\":[" << outbound 
       << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}"
       << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}],\n";

    // Build inbound tags for DNS routing
    std::string client_inbounds = "\"mixed-in\"";

    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":[";
    // 1. Client DNS queries → dns-out (intercept and resolve via XRay DNS)
    ss <<     "{\"type\":\"field\",\"inboundTag\":[" << client_inbounds << "],\"port\":53,\"outboundTag\":\"dns-out\"},";
    // 2. DNS module resolves through proxy (bypasses Iranian DNS censorship)
    ss <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"outboundTag\":\"proxy\"},";
    // 3. Internal DNS → direct
    ss <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},";
    // 4. Private/local IPs → direct
    ss <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"}";
    // 5. Iranian domains direct (optional, improves speed for local sites)
    if (hasGeositeData()) {
        ss << ",{\"type\":\"field\",\"domain\":[\"geosite:ir\"],\"outboundTag\":\"direct\"}";
    }
    ss << "]}\n";
    ss << "}";
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

    // Build inbound tags for DNS routing
    std::string client_inbounds = "\"mixed-in\"";

    // Balancer + routing with DNS rules + smart routing
    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
       <<     "{\"type\":\"field\",\"inboundTag\":[" << client_inbounds << "],\"port\":53,\"outboundTag\":\"dns-out\"},"
       <<     "{\"type\":\"field\",\"inboundTag\":[\"dns-module\"],\"balancerTag\":\"proxy-balancer\"},"
       <<     "{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"},"
       <<     "{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"}";
    if (hasGeositeData()) {
        ss << ",{\"type\":\"field\",\"domain\":[\"geosite:ir\"],\"outboundTag\":\"direct\"}";
    }
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
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}],\n";

    ss << "  \"routing\":{\"domainStrategy\":\"AsIs\",\"rules\":["
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
    const std::vector<std::pair<ParsedConfig, int>>& configs) {

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
    for (auto& [parsed, port] : configs) {
        std::string ob = parsed.toXrayOutboundJson(port);
        if (ob.empty()) continue;

        std::string ob_tag = "proxy-" + std::to_string(port);
        outbound_tags.push_back(ob_tag);

        // Replace default "proxy" tag with port-specific tag
        std::string tag_needle = "\"tag\":\"proxy\"";
        size_t tag_pos = ob.find(tag_needle);
        if (tag_pos != std::string::npos) {
            ob.replace(tag_pos, tag_needle.size(), "\"tag\":\"" + ob_tag + "\"");
        }

        // Apply TLS fragment for anti-DPI (Iranian censorship)
        if (parsed.security == "tls") {
            // Inject sockopt fragment into streamSettings if present
            std::string stream_needle = "\"streamSettings\":{";
            size_t stream_pos = ob.find(stream_needle);
            if (stream_pos != std::string::npos) {
                size_t insert_at = stream_pos + stream_needle.size();
                ob.insert(insert_at,
                    "\"sockopt\":{\"dialerProxy\":\"fragment-out\"},");
            }
        }

        if (!first) ss << ",";
        first = false;
        ss << ob;
    }

    // Direct outbound
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"direct\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}";
    // Fragment outbound for anti-DPI - AGGRESSIVE settings for Iranian DPI
    ss << ",{\"protocol\":\"freedom\",\"tag\":\"fragment-out\",\"settings\":{\"domainStrategy\":\"AsIs\""
       <<     ",\"fragment\":{\"packets\":\"tlshello\",\"length\":\"50-100\",\"interval\":\"30-50\"}"
       <<   "}}";
    // DNS outbound
    ss << ",{\"protocol\":\"dns\",\"tag\":\"dns-out\"}";
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

    // DNS module → direct (for internal XRay DNS resolution)
    ss << ",{\"type\":\"field\",\"port\":53,\"outboundTag\":\"direct\"}";
    // Private IPs → direct
    ss << ",{\"type\":\"field\",\"ip\":[\"10.0.0.0/8\",\"172.16.0.0/12\",\"192.168.0.0/16\",\"127.0.0.0/8\",\"169.254.0.0/16\"],\"outboundTag\":\"direct\"}";
    ss << "]}\n";
    ss << "}";
    return ss.str();
}

} // namespace proxy
} // namespace hunter
