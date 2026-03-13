#pragma once

#include <string>
#include <vector>
#include <memory>

namespace hunter {
namespace network {

/**
 * @brief Result of a proxy test
 */
struct ProxyTestResult {
    bool success = false;
    bool telegram_only = false;        // True if proxy works for Telegram but HTTP download failed
    float download_speed_kbps = 0.0f;  // Download speed in KB/s
    std::string error_message;
    std::string engine_used;  // "xray", "sing-box", "mihomo", etc.
    std::string uri;           // The config URI that was tested
};

/**
 * @brief Proxy engine tester
 * 
 * Tests proxy configurations by:
 * 1. Starting a proxy engine (xray, sing-box, mihomo) with the config
 * 2. Downloading a test file via the proxy
 * 3. Measuring download speed
 */
class ProxyTester {
public:
    ProxyTester();
    ~ProxyTester();

    /**
     * @brief Test a proxy config URI
     * @param config_uri The proxy config URI (vmess://, vless://, trojan://, etc.)
     * @param test_url URL to download for testing (default: cachefly)
     * @param timeout_seconds Timeout for the test
     * @return Test result with success status and download speed
     */
    ProxyTestResult testConfig(const std::string& config_uri, 
                               const std::string& test_url = "https://cachefly.cachefly.net/1mb.test",
                               int timeout_seconds = 30);

    /**
     * @brief Batch-test multiple configs using a SINGLE xray process (v2rayN pattern)
     * 
     * Generates one xray config with N inbounds (one per config on unique ports),
     * starts ONE process, tests all configs in parallel through their SOCKS ports.
     * Dramatically more efficient than spawning N separate processes.
     * 
     * @param config_uris Vector of proxy URI strings to test
     * @param base_port Starting port for sequential allocation
     * @param timeout_seconds Per-config test timeout
     * @return Vector of results (one per input URI, in same order)
     */
    std::vector<ProxyTestResult> batchTestWithXray(
        const std::vector<std::string>& config_uris,
        int base_port = 29100,
        int timeout_seconds = 10);

    static int activeTestCount();
    static int peakTestCount();
    static int maxConcurrentTestCount();

    /**
     * @brief Set the path to xray executable
     */
    void setXrayPath(const std::string& path) { xray_path_ = path; }

    /**
     * @brief Set the path to sing-box executable
     */
    void setSingBoxPath(const std::string& path) { singbox_path_ = path; }

    /**
     * @brief Set the path to mihomo executable
     */
    void setMihomoPath(const std::string& path) { mihomo_path_ = path; }

private:
    std::string xray_path_;
    std::string singbox_path_;
    std::string mihomo_path_;

    /**
     * @brief Test config using XRay
     */
    ProxyTestResult testWithXray(const std::string& config_uri, const std::string& test_url, int timeout_seconds);

    /**
     * @brief Test config using sing-box
     */
    ProxyTestResult testWithSingBox(const std::string& config_uri, const std::string& test_url, int timeout_seconds);

    /**
     * @brief Test config using mihomo
     */
    ProxyTestResult testWithMihomo(const std::string& config_uri, const std::string& test_url, int timeout_seconds);

    /**
     * @brief Get a free local port for testing
     */
    int getFreePort();

    /**
     * @brief Generate XRay config JSON for a URI
     */
    std::string generateXrayConfig(const std::string& config_uri, int socks_port);
};

} // namespace network
} // namespace hunter
