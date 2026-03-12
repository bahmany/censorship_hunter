#pragma once

#include <string>
#include <vector>
#include <set>
#include <cstdint>
#include <chrono>
#include <random>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <regex>
#include <mutex>

// ─── Platform network headers (must be before namespace) ───
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace hunter {
namespace utils {

/**
 * @brief Get current Unix timestamp as double (seconds.microseconds)
 */
double nowTimestamp();

/**
 * @brief Get current time in milliseconds (steady clock)
 */
uint64_t nowMs();

/**
 * @brief SHA1 hash of a string, returned as hex
 */
std::string sha1Hex(const std::string& input);

/**
 * @brief Base64 decode
 */
std::string base64Decode(const std::string& encoded);

/**
 * @brief Base64 encode
 */
std::string base64Encode(const std::string& data);

/**
 * @brief URL decode
 */
std::string urlDecode(const std::string& encoded);

/**
 * @brief URL encode
 */
std::string urlEncode(const std::string& data);

/**
 * @brief Extract proxy config URIs from text
 * Supports vmess://, vless://, trojan://, ss://, ssr://, hysteria2://, hy2://, tuic://
 */
std::set<std::string> extractRawUrisFromText(const std::string& text);

/**
 * @brief Try to base64-decode text, then extract URIs
 */
std::set<std::string> tryDecodeAndExtract(const std::string& text);

/**
 * @brief Read all lines from a file (trimmed, non-empty)
 */
std::vector<std::string> readLines(const std::string& filepath);

/**
 * @brief Write lines to a file
 */
bool writeLines(const std::string& filepath, const std::vector<std::string>& lines);

/**
 * @brief Append unique lines to a file
 * @return Number of new lines added
 */
int appendUniqueLines(const std::string& filepath, const std::vector<std::string>& lines);

/**
 * @brief Load JSON string from file
 */
std::string loadJsonFile(const std::string& filepath);

/**
 * @brief Save JSON string to file
 */
bool saveJsonFile(const std::string& filepath, const std::string& json);

/**
 * @brief Get random element from vector
 */
template<typename T>
const T& randomChoice(const std::vector<T>& vec) {
    static thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, vec.size() - 1);
    return vec[dist(gen)];
}

// ─── Network helpers ───
#ifdef _WIN32
typedef SOCKET socket_t;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#else
typedef int socket_t;
#define INVALID_SOCKET_HANDLE -1
#endif

void ensureSocketLayer();

/**
 * @brief Create TCP socket with connection timeout
 */
socket_t createTcpSocket(const std::string& host, int port, double timeout_sec);

/**
 * @brief Close socket
 */
void closeSocket(socket_t fd);

/**
 * @brief Execute system command and get output
 */
std::string execCommand(const std::string& cmd);

/**
 * @brief Trim whitespace from both ends
 */
std::string trim(const std::string& s);

/**
 * @brief Split string by delimiter
 */
std::vector<std::string> split(const std::string& s, char delimiter);

/**
 * @brief Join strings with separator
 */
std::string join(const std::vector<std::string>& parts, const std::string& sep);

/**
 * @brief Case-insensitive string compare
 */
bool iequals(const std::string& a, const std::string& b);

/**
 * @brief Check if string starts with prefix
 */
bool startsWith(const std::string& s, const std::string& prefix);

/**
 * @brief Check if string ends with suffix
 */
bool endsWith(const std::string& s, const std::string& suffix);

/**
 * @brief Create directories recursively
 */
bool mkdirRecursive(const std::string& path);

/**
 * @brief Check if a file exists
 */
bool fileExists(const std::string& path);

/**
 * @brief Get directory of a file path
 */
std::string dirName(const std::string& path);

/**
 * @brief Check if a local TCP port is accepting connections
 */
bool isPortAlive(int port, int timeout_ms = 1000);

/**
 * @brief Wait until a local TCP port becomes responsive within the timeout window
 */
bool waitForPortAlive(int port, int timeout_ms = 5000, int probe_interval_ms = 100);

struct LocalProxyProbeResult {
    bool tcp_alive = false;
    bool socks_ready = false;
    bool http_ready = false;
    bool mixed_ready() const { return tcp_alive && socks_ready && http_ready; }
};

bool probeLocalSocks5(int port, int timeout_ms = 1500);
bool probeLocalHttpProxy(int port, int timeout_ms = 1500);
LocalProxyProbeResult probeLocalMixedPort(int port, int timeout_ms = 2000);

/**
 * @brief Test TCP connect to a remote host:port (for pre-screening proxy servers)
 * @return true if connection succeeds within timeout
 */
bool tcpConnect(const std::string& host, int port, int timeout_ms = 3000);

/**
 * @brief Measure TCP connect latency to local port
 * @return latency in ms, or -1 on failure
 */
float portLatency(int port, int timeout_ms = 2000);

/**
 * @brief Download file via SOCKS5 proxy and measure speed
 * @param url URL to download from
 * @param proxy_host SOCKS5 proxy host (e.g., "127.0.0.1")
 * @param proxy_port SOCKS5 proxy port
 * @param timeout_seconds Timeout for the entire operation
 * @return Download speed in KB/s, or -1.0 on failure
 */
float downloadSpeedViaSocks5(const std::string& url, const std::string& proxy_host, int proxy_port, int timeout_seconds = 30);

/**
 * @brief Test if a SOCKS5 proxy can download from a URL
 * @param url URL to test
 * @param proxy_host SOCKS5 proxy host
 * @param proxy_port SOCKS5 proxy port
 * @param timeout_seconds Timeout
 * @return true if download succeeds, false otherwise
 */
bool testProxyDownload(const std::string& url, const std::string& proxy_host, int proxy_port, int timeout_seconds = 15);

/**
 * @brief Get system RAM usage percentage
 */
float getMemoryPercent();

/**
 * @brief Get number of CPU cores
 */
int getCpuCount();

/**
 * @brief HTML escape for dashboard
 */
std::string htmlEscape(const std::string& s);

/**
 * @brief Simple JSON builder helpers
 */
class JsonBuilder {
public:
    JsonBuilder& add(const std::string& key, const std::string& value);
    JsonBuilder& add(const std::string& key, const char* value);
    JsonBuilder& add(const std::string& key, int value);
    JsonBuilder& add(const std::string& key, double value);
    JsonBuilder& add(const std::string& key, bool value);
    JsonBuilder& addRaw(const std::string& key, const std::string& raw_json);
    std::string build() const;

private:
    std::vector<std::string> pairs_;
};

/**
 * @brief Thread-safe ring buffer for recent log lines (dashboard display)
 */
class LogRingBuffer {
public:
    static LogRingBuffer& instance() {
        static LogRingBuffer inst;
        return inst;
    }

    void push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mu_);
        buf_[write_pos_ % CAPACITY] = line;
        write_pos_++;
    }

    std::vector<std::string> recent(int n) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<std::string> result;
        int count = (int)std::min((size_t)n, write_pos_);
        for (int i = count; i > 0; i--) {
            result.push_back(buf_[(write_pos_ - i) % CAPACITY]);
        }
        return result;
    }

private:
    LogRingBuffer() : write_pos_(0) {}
    static constexpr size_t CAPACITY = 200;
    std::string buf_[CAPACITY];
    size_t write_pos_;
    mutable std::mutex mu_;
};

} // namespace utils
} // namespace hunter
