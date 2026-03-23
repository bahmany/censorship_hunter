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
#include <cstdlib>
#include <future>
#include <sstream>
#include <vector>

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
static std::atomic<int> s_peak_tests{0};
static std::mutex s_port_mutex;

static int getEnvIntClamped(const char* name, int fallback, int min_value, int max_value) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    try {
        int value = std::stoi(raw);
        if (value < min_value) value = min_value;
        if (value > max_value) value = max_value;
        return value;
    } catch (...) {
        return fallback;
    }
}

static int getMaxConcurrentTests() {
    static int max_tests = 0;
    if (max_tests == 0) {
        const int forced = getEnvIntClamped("HUNTER_MAX_CONCURRENT_TEST_PROCESSES", 0, 1, 128);
        if (forced > 0) {
            max_tests = forced;
        } else {
            int cpus = static_cast<int>(std::thread::hardware_concurrency());
            if (cpus <= 0) cpus = 4;
            max_tests = std::min(96, std::max(32, cpus * 4));
        }
    }
    return max_tests;
}

static int getProcessHoldMs() {
    static int hold_ms = -1;
    if (hold_ms < 0) {
        hold_ms = getEnvIntClamped("HUNTER_TEST_PROCESS_HOLD_MS", 1200, 0, 5000);
    }
    return hold_ms;
}

static bool useTcpPreScreen() {
    static int enabled = getEnvIntClamped("HUNTER_ENABLE_TCP_PRESCREEN", 0, 0, 1);
    return enabled == 1;
}

static bool keepFailedXrayArtifacts() {
    static int enabled = getEnvIntClamped("HUNTER_KEEP_FAILED_XRAY_CONFIG", 0, 0, 1);
    return enabled == 1;
}

static uint32_t fingerprintText(const std::string& value) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

static std::string summarizeConfigForLog(const std::string& config_uri) {
    std::string label = "uri";
    auto parsed = UriParser::parse(config_uri);
    if (parsed.has_value() && parsed->isValid() && !parsed->protocol.empty()) {
        label = parsed->protocol;
    }
    std::ostringstream oss;
    oss << label << "#" << std::hex << std::uppercase << fingerprintText(config_uri);
    return oss.str();
}

static std::string makeRuntimeArtifactPath(const char* prefix, int port, const char* extension) {
    static std::atomic<uint32_t> nonce{0};
#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    std::random_device rd;
    std::ostringstream oss;
    oss << "runtime/" << prefix << "_" << port << "_"
        << std::hex << std::uppercase << utils::nowMs() << "_"
        << pid << "_" << nonce.fetch_add(1) << "_" << (rd() & 0xFFFFu)
        << extension;
    return oss.str();
}

static void logKeptArtifacts(const std::string& engine_name, int test_port) {
    TLOG("  [" << engine_name << ":" << test_port << "] KEEP failure artifacts enabled");
}

static void onTestStarted() {
    const int active = ++s_active_tests;
    int peak = s_peak_tests.load();
    while (active > peak && !s_peak_tests.compare_exchange_weak(peak, active)) {
    }
}

static void onTestFinished() {
    --s_active_tests;
}

static ProxyTestResult chooseBestResult(const std::vector<ProxyTestResult>& results) {
    ProxyTestResult best_success;
    bool have_success = false;
    ProxyTestResult best_telegram;
    bool have_telegram = false;
    ProxyTestResult best_failure;
    bool have_failure = false;
    bool have_unreachable = false;

    for (const auto& result : results) {
        if (result.success && !result.telegram_only) {
            if (!have_success || result.download_speed_kbps > best_success.download_speed_kbps) {
                best_success = result;
                have_success = true;
            }
            continue;
        }
        if (result.success && result.telegram_only) {
            if (!have_telegram) {
                best_telegram = result;
                have_telegram = true;
            }
            continue;
        }
        if (!have_failure) {
            best_failure = result;
            have_failure = true;
        }
        if (result.error_message == "Server unreachable") {
            have_unreachable = true;
        }
    }

    if (have_success) return best_success;
    if (have_telegram) return best_telegram;
    if (have_unreachable) {
        ProxyTestResult result;
        result.error_message = "Server unreachable";
        return result;
    }
    if (have_failure) return best_failure;

    ProxyTestResult result;
    result.error_message = "All engines failed or no engines available";
    return result;
}

ProxyTester::ProxyTester() {
    xray_path_ = "bin/xray.exe";
    singbox_path_ = "bin/sing-box.exe";
    mihomo_path_ = "bin/mihomo-windows-amd64-compatible.exe";
}

ProxyTester::~ProxyTester() {}

int ProxyTester::activeTestCount() {
    return s_active_tests.load();
}

int ProxyTester::peakTestCount() {
    return s_peak_tests.load();
}

int ProxyTester::maxConcurrentTestCount() {
    return getMaxConcurrentTests();
}

int ProxyTester::getFreePort() {
    std::lock_guard<std::mutex> lock(s_port_mutex);
    // Sequential scanning from a rotating base (v2rayN pattern)
    // Avoids random collisions and finds free ports faster
    static std::atomic<int> s_next_port{20000};
    int start = s_next_port.load();
    if (start >= 30000) start = 20000;

    for (int i = 0; i < 500; i++) {
        int port = start + i;
        if (port >= 30000) port -= 10000; // wrap around
        if (!utils::isPortAlive(port, 50)) {
            s_next_port.store(port + 1);
            return port;
        }
    }
    // Absolute fallback
    return 20000 + (s_next_port.fetch_add(1) % 10000);
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
    "https://www.gstatic.com/generate_204",
    "https://speed.cloudflare.com/__down?bytes=5120",
    "https://cachefly.cachefly.net/1mb.test",
};
static const int NUM_TEST_URLS = 3;

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
    while (s_active_tests.load() >= getMaxConcurrentTests()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) { // 30 second max wait
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    // Extract short URI for logging
    std::string short_uri = summarizeConfigForLog(config_uri);
    
    // TCP pre-screening: quick connect to proxy server to skip dead IPs
    auto parsed_opt = UriParser::parse(config_uri);
    if (useTcpPreScreen() && parsed_opt.has_value() && parsed_opt->isValid()) {
        if (!utils::tcpConnect(parsed_opt->address, parsed_opt->port, 2000)) {
            result.error_message = "Server unreachable";
            TLOG("  [Pre] DEAD " << short_uri);
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
    std::string temp_config = makeRuntimeArtifactPath("temp_xray_test", test_port, ".json");
    if (!utils::saveJsonFile(temp_config, config_json)) {
        result.error_message = "Failed to write temp config";
        s_active_tests--;
        return result;
    }
    
#ifdef _WIN32
    // Capture XRay stdout (XRay writes errors to stdout, not stderr)
    std::string xray_out_path = makeRuntimeArtifactPath("temp_xray_out", test_port, ".txt");
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
    
    std::string cmd = "\"" + xray_path_ + "\" run -c \"" + temp_config + "\"";
    TLOG("  [Test:" << test_port << "] Starting xray process");
    char cmd_buf[4096];
    strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;
    
    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE, 
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        result.error_message = "Failed to start XRay";
        TLOG("  [Test:" << test_port << "] FAIL " << short_uri << " - CreateProcess failed");
        if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
        if (keepFailedXrayArtifacts()) {
            logKeptArtifacts("Test", test_port);
        } else {
            std::remove(temp_config.c_str());
            std::remove(xray_out_path.c_str());
        }
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
        if (keepFailedXrayArtifacts()) {
            logKeptArtifacts("Test", test_port);
        } else {
            std::remove(temp_config.c_str());
            std::remove(xray_out_path.c_str());
        }
        result.error_message = "XRay port not listening";
        TLOG("  [Test:" << test_port << "] FAIL " << short_uri << " - port not alive");
        s_active_tests--;
        return result;
    }
    
    // ═══ Multi-tier connectivity test ═══
    // Use passed timeout (capped) instead of hardcoded values
    int quick_timeout = std::max(3, std::min(timeout_seconds, 8));
    int dl_timeout = std::max(5, timeout_seconds);
    
    float speed = -1.0f;
    for (int t = 0; t < NUM_TEST_URLS && speed <= 0.0f; t++) {
        const int url_timeout = t == 0 ? quick_timeout : dl_timeout;
        TLOG("  [Test:" << test_port << "] curl --socks5-hostname 127.0.0.1:" << test_port << " --max-time " << url_timeout << " " << TEST_URLS[t]);
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, url_timeout);
    }
    
    // Tier 3: Telegram DC connectivity test (proves proxy works even if HTTP is DPI-blocked)
    bool telegram_ok = false;
    if (speed <= 0.0f) {
        telegram_ok = testSocks5TelegramDC(test_port, 8000);
        if (telegram_ok) {
            TLOG("  [Test:" << test_port << "] TG-ONLY " << short_uri << " (HTTP download failed, Telegram TCP works)");
        }
    }
    const int hold_ms = getProcessHoldMs();
    if (hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
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
        result.success = true;
        result.telegram_only = true;
        result.download_speed_kbps = 0.1f;
        TLOG("  [Test:" << test_port << "] TG-OK " << short_uri << " (Telegram-only, HTTP blocked by DPI)");
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
        
        float speed = -1.0f;
        for (int t = 0; t < NUM_TEST_URLS && speed <= 0.0f; t++) {
            const int url_timeout = t == 0 ? 10 : timeout_seconds;
            speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, url_timeout);
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
    
    float speed = -1.0f;
    for (int t = 0; t < NUM_TEST_URLS && speed <= 0.0f; t++) {
        const int url_timeout = t == 0 ? 10 : timeout_seconds;
        speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", test_port, url_timeout);
    }
    
    bool telegram_ok = false;
    if (speed <= 0.0f) {
        telegram_ok = testSocks5TelegramDC(test_port, 8000);
        if (telegram_ok) {
            TLOG("  [" << engine_name << ":" << test_port << "] TG-ONLY " << short_uri << " (HTTP download failed, Telegram TCP works)");
        }
    }
    const int hold_ms = getProcessHoldMs();
    if (hold_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
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
        result.download_speed_kbps = 0.1f;
        TLOG("  [" << engine_name << ":" << test_port << "] TG-OK " << short_uri << " (Telegram-only, HTTP blocked by DPI)");
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
    while (s_active_tests.load() >= getMaxConcurrentTests()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) {
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    std::string short_uri = summarizeConfigForLog(config_uri);
    
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
    
    std::string temp_config = makeRuntimeArtifactPath("temp_singbox_test", test_port, ".json");
    if (!utils::saveJsonFile(temp_config, config_json)) {
        result.error_message = "Failed to write temp config";
        s_active_tests--;
        return result;
    }
    
#ifdef _WIN32
    std::string log_file = makeRuntimeArtifactPath("temp_singbox_out", test_port, ".txt");
    std::string cmd = "\"" + singbox_path_ + "\" run -c \"" + temp_config + "\"";
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
    while (s_active_tests.load() >= getMaxConcurrentTests()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (++wait_count > 60) {
            result.error_message = "Timeout waiting for test slot";
            return result;
        }
    }
    s_active_tests++;
    
    std::string short_uri = summarizeConfigForLog(config_uri);
    
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
    std::string temp_config = makeRuntimeArtifactPath("temp_mihomo_test", test_port, ".yaml");
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
    std::string log_file = makeRuntimeArtifactPath("temp_mihomo_out", test_port, ".txt");
    std::string cmd = "\"" + mihomo_path_ + "\" -f \"" + temp_config + "\"";
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

    // Sequential engine testing: try each engine in order, return on first success.
    // This avoids spawning 3 concurrent processes per config which exhausts resources.
    // Order: xray (most common) -> sing-box -> mihomo

    struct EngineEntry {
        std::string name;
        bool available;
        std::function<ProxyTestResult()> test_fn;
    };

    std::vector<EngineEntry> engines;
    if (!xray_unsupported && utils::fileExists(xray_path_)) {
        engines.push_back({"xray", true, [this, &config_uri, &test_url, timeout_seconds]() {
            return testWithXray(config_uri, test_url, timeout_seconds);
        }});
    }
    if (utils::fileExists(singbox_path_)) {
        engines.push_back({"sing-box", true, [this, &config_uri, &test_url, timeout_seconds]() {
            return testWithSingBox(config_uri, test_url, timeout_seconds);
        }});
    }
    if (utils::fileExists(mihomo_path_)) {
        engines.push_back({"mihomo", true, [this, &config_uri, &test_url, timeout_seconds]() {
            return testWithMihomo(config_uri, test_url, timeout_seconds);
        }});
    }

    if (engines.empty()) {
        ProxyTestResult result;
        result.error_message = "All engines failed or no engines available";
        return result;
    }

    ProxyTestResult best_result;
    best_result.error_message = "All engines failed";

    for (auto& engine : engines) {
        try {
            ProxyTestResult result = engine.test_fn();
            // Full success (HTTP download worked) — return immediately
            if (result.success && !result.telegram_only) {
                return result;
            }
            // Telegram-only success — keep as best candidate but try next engine
            if (result.success && result.telegram_only) {
                if (!best_result.success) {
                    best_result = result;
                }
                continue;
            }
            // Failure — record error but try next engine
            if (!best_result.success) {
                best_result = result;
            }
        } catch (const std::exception& ex) {
            if (!best_result.success) {
                best_result.error_message = std::string("Engine ") + engine.name + " exception: " + ex.what();
            }
        } catch (...) {
            if (!best_result.success) {
                best_result.error_message = std::string("Engine ") + engine.name + " unknown exception";
            }
        }
    }

    return best_result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Batch test: single xray process with N inbounds (v2rayN pattern)
// ═══════════════════════════════════════════════════════════════════════════

std::vector<ProxyTestResult> ProxyTester::batchTestWithXray(
    const std::vector<std::string>& config_uris,
    int base_port,
    int timeout_seconds)
{
    std::vector<ProxyTestResult> results(config_uris.size());

    if (config_uris.empty()) return results;

    // Initialize all results with URIs
    for (size_t i = 0; i < config_uris.size(); i++) {
        results[i].engine_used = "xray";
        results[i].uri = config_uris[i];
    }

    // Parse all URIs and assign sequential ports
    std::vector<std::pair<ParsedConfig, int>> valid_entries;
    std::vector<std::pair<size_t, int>> index_port_map; // original index → port

    int next_port = base_port;
    for (size_t i = 0; i < config_uris.size(); i++) {
        auto parsed = UriParser::parse(config_uris[i]);
        if (!parsed.has_value() || !parsed->isValid()) {
            results[i].error_message = "URI parse failed";
            continue;
        }
        // Skip protocols XRay doesn't support
        if (parsed->protocol == "hysteria2" || parsed->protocol == "tuic") {
            results[i].error_message = "Unsupported protocol for xray batch";
            continue;
        }
        if (parsed->toXrayOutboundJson(0).empty()) {
            results[i].error_message = "Unsupported config for xray batch";
            continue;
        }
        // Find a free port sequentially
        while (utils::isPortAlive(next_port, 50) && next_port < base_port + 500) {
            next_port++;
        }
        if (next_port >= base_port + 500) {
            results[i].error_message = "No free ports available";
            continue;
        }

        int port = next_port++;
        valid_entries.emplace_back(*parsed, port);
        index_port_map.emplace_back(i, port);
    }

    if (valid_entries.empty()) {
        TLOG("[BatchTest] No valid xray-compatible configs to test");
        return results;
    }

    TLOG("[BatchTest] Generating batch config for " << valid_entries.size()
         << " configs on ports " << base_port << "-" << (next_port - 1));

    // Generate single xray config with all inbounds
    std::string batch_config = proxy::XRayManager::generateBatchSpeedtestConfig(valid_entries);
    if (batch_config.empty()) {
        for (auto& r : results) {
            if (r.error_message.empty()) r.error_message = "Failed to generate batch config";
        }
        return results;
    }

    // Write config and start single xray process
    std::string config_path = makeRuntimeArtifactPath("temp_xray_batch", base_port, ".json");
    if (!utils::saveJsonFile(config_path, batch_config)) {
        for (auto& r : results) {
            if (r.error_message.empty()) r.error_message = "Failed to write batch config";
        }
        return results;
    }

#ifdef _WIN32
    std::string log_path = makeRuntimeArtifactPath("temp_xray_batch_out", base_port, ".txt");
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOutFile = CreateFileA(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOutFile;
    si.hStdError = hOutFile;
    si.hStdInput = NULL;

    std::string cmd = "\"" + xray_path_ + "\" run -c \"" + config_path + "\"";
    char cmd_buf[4096];
    strncpy(cmd_buf, cmd.c_str(), sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = 0;

    TLOG("[BatchTest] Starting single xray process");

    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        TLOG("[BatchTest] FAIL - CreateProcess failed");
        if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);
        std::remove(config_path.c_str());
        std::remove(log_path.c_str());
        for (auto& r : results) {
            if (r.error_message.empty()) r.error_message = "Failed to start xray batch process";
        }
        return results;
    }
    if (hOutFile != INVALID_HANDLE_VALUE) CloseHandle(hOutFile);

    // Wait for xray to start — check FIRST port in the batch
    bool any_port_alive = false;
    for (int attempt = 0; attempt < 20; attempt++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Check if any of the ports are alive
        for (auto& [idx, port] : index_port_map) {
            if (utils::isPortAlive(port, 200)) {
                any_port_alive = true;
                break;
            }
        }
        if (any_port_alive) break;
        // Check if process died
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        if (exitCode != STILL_ACTIVE) break;
    }

    if (!any_port_alive) {
        TLOG("[BatchTest] FAIL - no ports alive after startup");
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::string xray_err = utils::loadJsonFile(log_path);
        if (!xray_err.empty()) {
            auto pos = xray_err.find("Failed to start");
            if (pos == std::string::npos) pos = xray_err.find("failed to");
            if (pos == std::string::npos) pos = xray_err.find("error");
            if (pos != std::string::npos) {
                std::string err_line = xray_err.substr(pos, 300);
                auto nl = err_line.find('\n');
                if (nl != std::string::npos) err_line = err_line.substr(0, nl);
                TLOG("[BatchTest] XRAY: " << err_line);
            }
        }
        std::remove(config_path.c_str());
        std::remove(log_path.c_str());
        for (auto& r : results) {
            if (r.error_message.empty()) r.error_message = "Batch xray ports not listening";
        }
        return results;
    }

    // Give a brief extra moment for remaining ports to come up
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    TLOG("[BatchTest] Process alive, testing " << index_port_map.size() << " configs in parallel...");

    // ═══ Test all configs in parallel through their individual SOCKS ports ═══
    int quick_timeout = std::max(3, std::min(timeout_seconds, 8));
    int dl_timeout = std::max(5, timeout_seconds);

    std::vector<std::future<void>> futures;
    for (auto& [orig_idx, port] : index_port_map) {
        size_t idx = orig_idx;
        int p = port;
        futures.push_back(std::async(std::launch::async, [this, idx, p, quick_timeout, dl_timeout, &results]() {
            std::string short_uri = summarizeConfigForLog(results[idx].uri);

            float speed = -1.0f;
            for (int t = 0; t < NUM_TEST_URLS && speed <= 0.0f; t++) {
                const int url_timeout = t == 0 ? quick_timeout : dl_timeout;
                speed = utils::downloadSpeedViaSocks5(TEST_URLS[t], "127.0.0.1", p, url_timeout);
            }

            // Tier 3: Telegram DC connectivity
            bool telegram_ok = false;
            if (speed <= 0.0f) {
                telegram_ok = testSocks5TelegramDC(p, 8000);
            }

            if (speed > 0.0f) {
                results[idx].success = true;
                results[idx].download_speed_kbps = speed;
                TLOG("  [Batch:" << p << "] OK   " << short_uri << " - " << speed << " KB/s");
            } else if (telegram_ok) {
                results[idx].success = true;
                results[idx].telegram_only = true;
                results[idx].download_speed_kbps = 0.1f;
                TLOG("  [Batch:" << p << "] TG-OK " << short_uri << " (Telegram-only)");
            } else {
                results[idx].error_message = "All connectivity tests failed";
                TLOG("  [Batch:" << p << "] FAIL " << short_uri);
            }
        }));
    }

    // Wait for all parallel tests to complete
    for (auto& f : futures) {
        try { f.get(); } catch (...) {}
    }

    // Kill the single xray process
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::remove(config_path.c_str());
    std::remove(log_path.c_str());

    int passed = 0;
    for (auto& r : results) { if (r.success) passed++; }
    TLOG("[BatchTest] Done: " << passed << "/" << config_uris.size() << " passed (1 process, "
         << valid_entries.size() << " inbounds)");

#else
    // Non-Windows: fall back to individual testing
    TLOG("[BatchTest] Batch testing not yet implemented on this platform, using individual tests");
    for (size_t i = 0; i < config_uris.size(); i++) {
        results[i] = testConfig(config_uris[i], TEST_URLS[0], timeout_seconds);
        results[i].uri = config_uris[i];
    }
    std::remove(config_path.c_str());
#endif

    return results;
}

} // namespace network
} // namespace hunter
