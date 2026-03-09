#include "network/proxy_tester.h"
#include "network/uri_parser.h"
#include "core/utils.h"
#include "proxy/xray_manager.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <fstream>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#endif

// Helper: log to both stdout and ring buffer for dashboard
#define TLOG(msg) do { \
    std::ostringstream _tlog_ss; \
    _tlog_ss << msg; \
    utils::LogRingBuffer::instance().push(_tlog_ss.str()); \
} while(0)

namespace hunter {
namespace network {

// Limit concurrent XRay processes to prevent resource exhaustion
static std::atomic<int> s_active_tests{0};
static const int MAX_CONCURRENT_TESTS = 20;
static std::mutex s_port_mutex;

ProxyTester::ProxyTester() {
    xray_path_ = "bin/xray.exe";
    singbox_path_ = "bin/sing-box.exe";
    mihomo_path_ = "bin/mihomo-windows-amd64-compatible.exe";
}

ProxyTester::~ProxyTester() {}

int ProxyTester::getFreePort() {
    std::lock_guard<std::mutex> lock(s_port_mutex);
    static std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(20000, 30000);
    
    for (int i = 0; i < 50; i++) {
        int port = dist(gen);
        if (!utils::isPortAlive(port, 50)) {
            return port;
        }
    }
    return 20808 + (gen() % 1000);
}

std::string ProxyTester::generateXrayConfig(const std::string& config_uri, int socks_port) {
    auto parsed_opt = UriParser::parse(config_uri);
    if (!parsed_opt.has_value() || !parsed_opt->isValid()) {
        return "";
    }
    ParsedConfig config = parsed_opt.value();
    return proxy::XRayManager::generateConfig(config, socks_port);
}

// Multi-tier test URLs for Iranian censorship resilience
static const char* TEST_URLS[] = {
    "http://cp.cloudflare.com/generate_204",           // HTTP 204 - no TLS, fast, hard to DPI-block
    "http://1.1.1.1/generate_204",                     // IP-based HTTP - no DNS needed
    "http://www.gstatic.com/generate_204",             // Google HTTP 204
    "https://cachefly.cachefly.net/50mb.test",         // HTTPS download - may be DPI-blocked
};
static const int NUM_TEST_URLS = 4;

// Telegram DC IPs for connectivity fallback test
static const char* TELEGRAM_DCS[] = {
    "149.154.175.50", "149.154.167.51", "149.154.175.100",
    "149.154.167.91", "91.108.56.130"
};
static const int NUM_TELEGRAM_DCS = 5;

#ifdef _WIN32
// Test SOCKS5 connectivity by connecting to a Telegram DC through the proxy
static bool testSocks5TelegramDC(int socks_port, int timeout_ms = 8000) {
    for (int i = 0; i < NUM_TELEGRAM_DCS; i++) {
        SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == INVALID_SOCKET) continue;
        
        // Connect to local SOCKS5 proxy
        sockaddr_in proxy_addr{};
        proxy_addr.sin_family = AF_INET;
        proxy_addr.sin_port = htons((uint16_t)socks_port);
        inet_pton(AF_INET, "127.0.0.1", &proxy_addr.sin_addr);
        
        // Set timeout
        DWORD tv_ms = (DWORD)timeout_ms;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
        
        if (::connect(fd, (sockaddr*)&proxy_addr, sizeof(proxy_addr)) != 0) {
            closesocket(fd);
            continue;
        }
        
        // SOCKS5 handshake: no auth
        unsigned char hello[] = {0x05, 0x01, 0x00};
        send(fd, (const char*)hello, 3, 0);
        unsigned char resp[2] = {};
        int n = recv(fd, (char*)resp, 2, 0);
        if (n != 2 || resp[0] != 0x05 || resp[1] != 0x00) {
            closesocket(fd);
            continue;
        }
        
        // SOCKS5 CONNECT to Telegram DC:443 using ATYP=0x01 (IPv4)
        unsigned char connect_req[10] = {0x05, 0x01, 0x00, 0x01};
        inet_pton(AF_INET, TELEGRAM_DCS[i], &connect_req[4]);
        connect_req[8] = 0x01; // port 443 high byte
        connect_req[9] = 0xBB; // port 443 low byte
        send(fd, (const char*)connect_req, 10, 0);
        
        unsigned char connect_resp[10] = {};
        n = recv(fd, (char*)connect_resp, 10, 0);
        closesocket(fd);
        
        if (n >= 2 && connect_resp[0] == 0x05 && connect_resp[1] == 0x00) {
            return true; // Successfully connected through proxy to Telegram DC
        }
    }
    return false;
}
#endif

ProxyTestResult ProxyTester::testWithXray(const std::string& config_uri, 
                                          const std::string& test_url, 
                                          int timeout_seconds) {
    ProxyTestResult result;
    result.engine_used = "xray";
    
    // Wait for a slot (limit concurrent tests)
    int wait_count = 0;
    while (s_active_tests.load() >= MAX_CONCURRENT_TESTS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) { // 30 second max wait
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    // Extract short URI for logging
    std::string short_uri = config_uri.substr(0, 40);
    if (config_uri.size() > 40) short_uri += "...";
    
    // TCP pre-screening: quick connect to proxy server to skip dead IPs
    auto parsed_opt = UriParser::parse(config_uri);
    if (parsed_opt.has_value() && parsed_opt->isValid()) {
        if (!utils::tcpConnect(parsed_opt->address, parsed_opt->port, 2000)) {
            result.error_message = "Server unreachable";
            TLOG("  [Pre] DEAD " << parsed_opt->address << ":" << parsed_opt->port);
            s_active_tests--;
            return result;
        }
    }
    
    int test_port = getFreePort();
    std::string config_json = generateXrayConfig(config_uri, test_port);
    
    if (config_json.empty()) {
        result.error_message = "Unsupported config";
        TLOG("  [Test:" << test_port << "] SKIP " << short_uri << " - unsupported");
        s_active_tests--;
        return result;
    }
    
    // Write config to temp file
    std::string temp_config = "runtime/temp_xray_test_" + std::to_string(test_port) + ".json";
    if (!utils::saveJsonFile(temp_config, config_json)) {
        result.error_message = "Failed to write temp config";
        s_active_tests--;
        return result;
    }
    
#ifdef _WIN32
    // Capture XRay stdout (XRay writes errors to stdout, not stderr)
    std::string xray_out_path = "runtime/temp_xray_out_" + std::to_string(test_port) + ".txt";
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOutFile = CreateFileA(xray_out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutFile;
    si.hStdError = hOutFile;
    si.hStdInput = NULL;
    
    std::string cmd = xray_path_ + " run -c " + temp_config;
    TLOG("  [Test:" << test_port << "] Starting xray: " << cmd);
    char cmd_buf[4096];
    strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;
    
    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        result.error_message = "Failed to start XRay";
        TLOG("  [Test:" << test_port << "] FAIL " << short_uri << " - CreateProcess failed");
        if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
        std::remove(temp_config.c_str());
        std::remove(xray_out_path.c_str());
        s_active_tests--;
        return result;
    }
    if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
    
    // Wait for XRay to start and listen on port
    bool port_alive = false;
    for (int i = 0; i < 10; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (utils::isPortAlive(test_port, 500)) {
            port_alive = true;
            break;
        }
        // Check if process died
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) {
            break;
        }
    }
    
    if (!port_alive) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        // Read XRay output for error diagnosis
        std::string xray_err = utils::loadJsonFile(xray_out_path);
        if (!xray_err.empty()) {
            auto pos = xray_err.find("Failed to start");
            if (pos == std::string::npos) pos = xray_err.find("failed to");
            if (pos != std::string::npos) {
                std::string err_line = xray_err.substr(pos, 300);
                auto nl = err_line.find('\n');
                if (nl != std::string::npos) err_line = err_line.substr(0, nl);
                TLOG("  [Test:" << test_port << "] XRAY: " << err_line);
            }
        }
        std::remove(temp_config.c_str());
        std::remove(xray_out_path.c_str());
        result.error_message = "XRay port not listening";
        TLOG("  [Test:" << test_port << "] FAIL " << short_uri << " - port not alive");
        s_active_tests--;
        return result;
    }
    
    // ═══ Multi-tier connectivity test ═══
    // Tier 1: HTTP 204 tests (fast, no TLS overhead, hard to DPI-block)
    float speed = -1.0f;
    for (int t = 0; t < 3 && speed <= 0.0f; t++) {
        TLOG("  [Test:" << test_port << "] curl --socks5-hostname 127.0.0.1:" << test_port << " --max-time 3 " << TEST_URLS[t]);
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, 3);
    }
    
    // Tier 2: HTTPS download test (slower but measures real speed)
    if (speed <= 0.0f) {
        TLOG("  [Test:" << test_port << "] curl --socks5-hostname 127.0.0.1:" << test_port << " --max-time 5 " << TEST_URLS[3]);
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[3], "127.0.0.1", test_port, 5);
    }
    
    // Tier 3: Telegram DC connectivity test (proves proxy works even if HTTP is DPI-blocked)
    bool telegram_ok = false;
    if (speed <= 0.0f) {
        telegram_ok = testSocks5TelegramDC(test_port, 8000);
        if (telegram_ok) {
            TLOG("  [Test:" << test_port << "] TG-ONLY " << short_uri << " (HTTP download failed, Telegram TCP works)");
        }
    }
    
    // Kill XRay process
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::remove(temp_config.c_str());
    std::remove(xray_out_path.c_str());
    
    if (speed > 0.0f) {
        result.success = true;
        result.download_speed_kbps = speed;
        TLOG("  [Test:" << test_port << "] OK   " << short_uri << " - " << speed << " KB/s");
    } else if (telegram_ok) {
        // Telegram-only: proxy can route TCP to Telegram DCs but cannot download HTTP content.
        // Mark as success but flag telegram_only so it is NOT treated as a fully alive config.
        result.success = true;
        result.telegram_only = true;
        result.download_speed_kbps = 0.0f;
    } else {
        result.error_message = "All connectivity tests failed";
        TLOG("  [Test:" << test_port << "] FAIL " << short_uri << " - all tests failed");
    }
    
#else
    pid_t pid = fork();
    if (pid == 0) {
        execl(xray_path_.c_str(), "xray", "run", "-c", temp_config.c_str(), NULL);
        exit(1);
    } else if (pid > 0) {
        bool port_alive = false;
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (utils::isPortAlive(test_port, 500)) { port_alive = true; break; }
        }
        
        if (!port_alive) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            std::remove(temp_config.c_str());
            result.error_message = "XRay port not listening";
            TLOG("  [Test:" << test_port << "] FAIL - port not alive");
            s_active_tests--;
            return result;
        }
        
        // Multi-tier test
        float speed = -1.0f;
        for (int t = 0; t < 3 && speed <= 0.0f; t++) {
            speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, 10);
        }
        if (speed <= 0.0f) {
            speed = utils::downloadSpeedViaSocks5(TEST_URLS[3], "127.0.0.1", test_port, timeout_seconds);
        }
        
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        std::remove(temp_config.c_str());
        
        if (speed > 0.0f) {
            result.success = true;
            result.download_speed_kbps = speed;
            TLOG("  [Test:" << test_port << "] OK   - " << speed << " KB/s");
        } else {
            result.error_message = "Download failed";
            TLOG("  [Test:" << test_port << "] FAIL - download failed");
        }
    } else {
        result.error_message = "Failed to fork";
    }
#endif
    
    s_active_tests--;
    return result;
}

#ifdef _WIN32
// Shared helper: start a process, wait for SOCKS port, run connectivity tests, kill process
static ProxyTestResult runEngineTest(
    const std::string& cmd_line,
    const std::string& config_file,
    const std::string& log_file,
    int test_port,
    const std::string& short_uri,
    const std::string& engine_name,
    int timeout_seconds) 
{
    ProxyTestResult result;
    result.engine_used = engine_name;
    
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOutFile = CreateFileA(log_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutFile;
    si.hStdError = hOutFile;
    si.hStdInput = NULL;
    
    char cmd_buf[4096];
    strncpy(cmd_buf, cmd_line.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;
    
    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        result.error_message = "Failed to start " + engine_name;
        TLOG("  [" << engine_name << ":" << test_port << "] FAIL " << short_uri << " - CreateProcess failed");
        if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
        std::remove(config_file.c_str());
        std::remove(log_file.c_str());
        return result;
    }
    if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
    
    // Wait for engine to start and listen on port
    bool port_alive = false;
    for (int i = 0; i < 15; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (utils::isPortAlive(test_port, 500)) {
            port_alive = true;
            break;
        }
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) break;
    }
    
    if (!port_alive) {
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 2000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::string err_log = utils::loadJsonFile(log_file);
        if (!err_log.empty()) {
            auto pos = err_log.find("error");
            if (pos == std::string::npos) pos = err_log.find("failed");
            if (pos != std::string::npos) {
                std::string err_line = err_log.substr(pos, 200);
                auto nl = err_line.find('\n');
                if (nl != std::string::npos) err_line = err_line.substr(0, nl);
                TLOG("  [" << engine_name << ":" << test_port << "] ERR: " << err_line);
            }
        }
        std::remove(config_file.c_str());
        std::remove(log_file.c_str());
        result.error_message = engine_name + " port not listening";
        TLOG("  [" << engine_name << ":" << test_port << "] FAIL " << short_uri << " - port not alive");
        return result;
    }
    
    // Multi-tier connectivity test
    float speed = -1.0f;
    for (int t = 0; t < 3 && speed <= 0.0f; t++) {
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, 10);
    }
    if (speed <= 0.0f) {
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[3], "127.0.0.1", test_port, timeout_seconds);
    }
    
    bool telegram_ok = false;
    if (speed <= 0.0f) {
        telegram_ok = testSocks5TelegramDC(test_port, 8000);
        if (telegram_ok) {
            TLOG("  [" << engine_name << ":" << test_port << "] TG-ONLY " << short_uri << " (HTTP download failed, Telegram TCP works)");
        }
    }
    
    // Kill engine process
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::remove(config_file.c_str());
    std::remove(log_file.c_str());
    
    if (speed > 0.0f) {
        result.success = true;
        result.download_speed_kbps = speed;
        TLOG("  [" << engine_name << ":" << test_port << "] OK   " << short_uri << " - " << speed << " KB/s");
    } else if (telegram_ok) {
        result.success = true;
        result.telegram_only = true;
        result.download_speed_kbps = 0.0f;
    } else {
        result.error_message = "All connectivity tests failed";
        TLOG("  [" << engine_name << ":" << test_port << "] FAIL " << short_uri << " - all tests failed");
    }
    
    return result;
}
#endif

ProxyTestResult ProxyTester::testWithSingBox(const std::string& config_uri, 
                                             const std::string& test_url, 
                                             int timeout_seconds) {
    ProxyTestResult result;
    result.engine_used = "sing-box";
    
    // Wait for a slot
    int wait_count = 0;
    while (s_active_tests.load() >= MAX_CONCURRENT_TESTS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) {
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    std::string short_uri = config_uri.substr(0, 40);
    if (config_uri.size() > 40) short_uri += "...";
    
    // Parse URI and generate sing-box config
    auto parsed_opt = UriParser::parse(config_uri);
    if (!parsed_opt.has_value() || !parsed_opt->isValid()) {
        result.error_message = "URI parse failed";
        s_active_tests--;
        return result;
    }
    
    int test_port = getFreePort();
    std::string config_json = parsed_opt->toSingBoxConfigJson(test_port);
    if (config_json.empty()) {
        result.error_message = "Unsupported config for sing-box";
        s_active_tests--;
        return result;
    }
    
    std::string temp_config = "runtime/temp_singbox_test_" + std::to_string(test_port) + ".json";
    if (!utils::saveJsonFile(temp_config, config_json)) {
        result.error_message = "Failed to write temp config";
        s_active_tests--;
        return result;
    }
    
#ifdef _WIN32
    std::string log_file = "runtime/temp_singbox_out_" + std::to_string(test_port) + ".txt";
    std::string cmd = singbox_path_ + " run -c " + temp_config;
    result = runEngineTest(cmd, temp_config, log_file, test_port, short_uri, "sing-box", timeout_seconds);
#else
    result.error_message = "sing-box testing not implemented on this platform";
    std::remove(temp_config.c_str());
#endif
    
    s_active_tests--;
    return result;
}

ProxyTestResult ProxyTester::testWithMihomo(const std::string& config_uri, 
                                            const std::string& test_url, 
                                            int timeout_seconds) {
    ProxyTestResult result;
    result.engine_used = "mihomo";
    
    // Wait for a slot
    int wait_count = 0;
    while (s_active_tests.load() >= MAX_CONCURRENT_TESTS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) {
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    std::string short_uri = config_uri.substr(0, 40);
    if (config_uri.size() > 40) short_uri += "...";
    
    // Parse URI and generate mihomo config
    auto parsed_opt = UriParser::parse(config_uri);
    if (!parsed_opt.has_value() || !parsed_opt->isValid()) {
        result.error_message = "URI parse failed";
        s_active_tests--;
        return result;
    }
    
    int test_port = getFreePort();
    std::string config_yaml = parsed_opt->toMihomoConfigYaml(test_port);
    if (config_yaml.empty()) {
        result.error_message = "Unsupported config for mihomo";
        s_active_tests--;
        return result;
    }
    
    // mihomo uses .yaml config files
    std::string temp_config = "runtime/temp_mihomo_test_" + std::to_string(test_port) + ".yaml";
    {
        std::ofstream ofs(temp_config);
        if (!ofs) {
            result.error_message = "Failed to write temp config";
            s_active_tests--;
            return result;
        }
        ofs << config_yaml;
    }
    
#ifdef _WIN32
    std::string log_file = "runtime/temp_mihomo_out_" + std::to_string(test_port) + ".txt";
    std::string cmd = mihomo_path_ + " -f " + temp_config;
    result = runEngineTest(cmd, temp_config, log_file, test_port, short_uri, "mihomo", timeout_seconds);
#else
    result.error_message = "mihomo testing not implemented on this platform";
    std::remove(temp_config.c_str());
#endif
    
    s_active_tests--;
    return result;
}

ProxyTestResult ProxyTester::testConfig(const std::string& config_uri, 
                                       const std::string& test_url, 
                                       int timeout_seconds) {
    // Determine which protocol this config uses
    auto parsed_opt = UriParser::parse(config_uri);
    std::string proto;
    if (parsed_opt.has_value() && parsed_opt->isValid()) {
        proto = parsed_opt->protocol;
    }
    
    // Protocols that XRay doesn't support but sing-box/mihomo do
    bool xray_unsupported = (proto == "hysteria2" || proto == "tuic");
    
    // Try XRay first (most compatible for vmess/vless/trojan/ss)
    if (!xray_unsupported && utils::fileExists(xray_path_)) {
        ProxyTestResult result = testWithXray(config_uri, test_url, timeout_seconds);
        if (result.success) return result;
        // If server is unreachable, no point trying other engines
        if (result.error_message == "Server unreachable") return result;
    }
    
    // Try sing-box (supports hysteria2, tuic, and all standard protocols)
    if (utils::fileExists(singbox_path_)) {
        ProxyTestResult result = testWithSingBox(config_uri, test_url, timeout_seconds);
        if (result.success) return result;
        if (result.error_message == "Server unreachable") return result;
    }
    
    // Try mihomo (Clash Meta — supports vless, vmess, trojan, ss, hysteria2, tuic)
    if (utils::fileExists(mihomo_path_)) {
        ProxyTestResult result = testWithMihomo(config_uri, test_url, timeout_seconds);
        if (result.success) return result;
    }
    
    ProxyTestResult result;
    result.error_message = "All engines failed or no engines available";
    return result;
}

} // namespace network
} // namespace hunter
