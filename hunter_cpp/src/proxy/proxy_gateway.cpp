#include "proxy/proxy_gateway.h"
#include "core/utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace hunter {
namespace proxy {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

void closeSock(socket_t fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool sendAll(socket_t fd, const void* buf, size_t len) {
    const char* ptr = static_cast<const char*>(buf);
    size_t done = 0;
    while (done < len) {
#ifdef _WIN32
        int sent = send(fd, ptr + done, static_cast<int>(len - done), 0);
#else
        ssize_t sent = send(fd, ptr + done, len - done, 0);
#endif
        if (sent <= 0) return false;
        done += static_cast<size_t>(sent);
    }
    return true;
}

bool recvLine(socket_t fd, std::string& line, size_t max_len = 8192) {
    line.clear();
    char ch;
    while (line.size() < max_len) {
#ifdef _WIN32
        int got = recv(fd, &ch, 1, 0);
#else
        ssize_t got = recv(fd, &ch, 1, 0);
#endif
        if (got <= 0) return false;
        line.push_back(ch);
        if (line.size() >= 2 && line[line.size()-2] == '\r' && line[line.size()-1] == '\n') {
            line.resize(line.size() - 2);
            return true;
        }
    }
    return false;
}

std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end-1]))) end--;
    return s.substr(start, end - start);
}

std::string jsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

std::string generateRandomId(int length = 32) {
    static const char chars[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 15);
    std::string id;
    id.reserve(length);
    for (int i = 0; i < length; ++i) {
        id.push_back(chars[dist(gen)]);
    }
    return id;
}

} // namespace

ProxyGateway::ProxyGateway(int http_port)
    : http_port_(http_port) {}

ProxyGateway::~ProxyGateway() {
    stop();
}

bool ProxyGateway::start() {
    if (running_.load()) return true;
    running_ = true;
    server_thread_ = std::thread(&ProxyGateway::serverLoop, this);
    health_thread_ = std::thread(&ProxyGateway::healthMonitorLoop, this);
    std::cout << "[Gateway] Proxy Gateway started on port " << http_port_ << std::endl;
    return true;
}

void ProxyGateway::stop() {
    if (!running_.exchange(false)) return;
    closeSock(kInvalidSocket);
    if (server_thread_.joinable()) server_thread_.join();
    if (health_thread_.joinable()) health_thread_.join();
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
}

void ProxyGateway::serverLoop() {
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        std::cerr << "[Gateway] Failed to create listener socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(http_port_));

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Gateway] Failed to bind port " << http_port_ << std::endl;
        closeSock(listener);
        return;
    }

    if (listen(listener, 16) != 0) {
        std::cerr << "[Gateway] Failed to listen on port " << http_port_ << std::endl;
        closeSock(listener);
        return;
    }

    std::cout << "[Gateway] Listening on 0.0.0.0:" << http_port_ << std::endl;

    while (running_.load()) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        socket_t client = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client == kInvalidSocket) {
            if (!running_.load()) break;
            continue;
        }

        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        std::thread(&ProxyGateway::handleClient, this, static_cast<int>(client)).detach();
    }

    closeSock(listener);
}

void ProxyGateway::handleClient(int client_fd_raw) {
    socket_t client_fd = static_cast<socket_t>(client_fd_raw);

    std::string request_line;
    if (!recvLine(client_fd, request_line)) {
        closeSock(client_fd);
        return;
    }

    std::string method = parseMethod(request_line);
    std::string path = parsePath(request_line);

    std::map<std::string, std::string> headers;
    std::string header_line;
    size_t content_length = 0;
    std::string auth_header;

    while (recvLine(client_fd, header_line) && !header_line.empty()) {
        auto colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string name = toLower(trim(header_line.substr(0, colon)));
            std::string value = trim(header_line.substr(colon + 1));
            headers[name] = value;
            if (name == "content-length") {
                content_length = static_cast<size_t>(std::atol(value.c_str()));
            }
            if (name == "authorization") {
                auth_header = value;
            }
        }
    }

    std::string body;
    if (content_length > 0 && content_length < 1024 * 1024) {
        body.resize(content_length);
        size_t done = 0;
        while (done < content_length) {
#ifdef _WIN32
            int got = recv(client_fd, &body[done], static_cast<int>(content_length - done), 0);
#else
            ssize_t got = recv(client_fd, &body[done], content_length - done, 0);
#endif
            if (got <= 0) break;
            done += static_cast<size_t>(got);
        }
        body.resize(done);
    }

    if (method == "CONNECT") {
        std::string host_port = path;
        std::string host;
        int port = 443;
        auto colon = host_port.find(':');
        if (colon != std::string::npos) {
            host = host_port.substr(0, colon);
            port = std::atoi(host_port.substr(colon + 1).c_str());
        } else {
            host = host_port;
        }

        std::string session_id;
        auto auth_it = headers.find("x-session-id");
        if (auth_it != headers.end()) {
            session_id = auth_it->second;
        }
        if (session_id.empty()) {
            auto cookie_it = headers.find("cookie");
            if (cookie_it != headers.end()) {
                auto pos = cookie_it->second.find("session_id=");
                if (pos != std::string::npos) {
                    auto end = cookie_it->second.find(';', pos);
                    session_id = cookie_it->second.substr(pos + 11,
                                end != std::string::npos ? end - pos - 11 : std::string::npos);
                }
            }
        }
        if (session_id.empty()) {
            session_id = createSession("");
        }
        touchSession(session_id);

        handleProxyConnect(static_cast<int>(client_fd), host, port, session_id);
        return;
    }

    handleApiRequest(static_cast<int>(client_fd), method, path, body, auth_header);
    closeSock(client_fd);
}

void ProxyGateway::handleApiRequest(int client_fd_raw, const std::string& method,
                                     const std::string& path, const std::string& body,
                                     const std::string& auth_header) {
    socket_t fd = static_cast<socket_t>(client_fd_raw);

    if (path == "/api/gateway/status" || path == "/api/gateway/status/") {
        sendJsonResponse(fd, 200, buildStatusJson());
        return;
    }

    if (path == "/api/gateway/sessions" || path == "/api/gateway/sessions/") {
        if (method == "GET") {
            sendJsonResponse(fd, 200, buildSessionsJson());
        } else if (method == "POST") {
            std::string user_token;
            if (!body.empty()) {
                auto pos = body.find("\"user_token\"");
                if (pos != std::string::npos) {
                    auto colon = body.find(':', pos);
                    auto q1 = body.find('"', colon + 1);
                    auto q2 = body.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos) {
                        user_token = body.substr(q1 + 1, q2 - q1 - 1);
                    }
                }
            }
            std::string session_id = createSession(user_token);
            std::ostringstream json;
            json << "{\"session_id\":\"" << jsonEscape(session_id) << "\","
                 << "\"status\":\"created\"}";
            sendJsonResponse(fd, 201, json.str());
        } else {
            sendErrorResponse(fd, 405, "Method not allowed");
        }
        return;
    }

    if (path.rfind("/api/gateway/sessions/", 0) == 0) {
        std::string session_id = path.substr(22);
        if (method == "DELETE") {
            if (destroySession(session_id)) {
                sendJsonResponse(fd, 200, "{\"status\":\"destroyed\"}");
            } else {
                sendErrorResponse(fd, 404, "Session not found");
            }
        } else if (method == "GET") {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                std::ostringstream json;
                auto& s = it->second;
                json << "{\"session_id\":\"" << jsonEscape(s.session_id) << "\","
                     << "\"assigned_uri\":\"" << jsonEscape(s.assigned_uri) << "\","
                     << "\"assigned_engine\":\"" << jsonEscape(s.assigned_engine) << "\","
                     << "\"assigned_port\":" << s.assigned_port << ","
                     << "\"bytes_sent\":" << s.bytes_sent << ","
                     << "\"bytes_received\":" << s.bytes_received << ","
                     << "\"active\":" << (s.active ? "true" : "false") << "}";
                sendJsonResponse(fd, 200, json.str());
            } else {
                sendErrorResponse(fd, 404, "Session not found");
            }
        } else {
            sendErrorResponse(fd, 405, "Method not allowed");
        }
        return;
    }

    if (path == "/api/gateway/pool" || path == "/api/gateway/pool/") {
        sendJsonResponse(fd, 200, buildPoolJson());
        return;
    }

    if (path == "/api/gateway/proxy" || path == "/api/gateway/proxy/") {
        if (method == "GET") {
            auto proxy = selectBestProxy();
            if (proxy.uri.empty()) {
                sendErrorResponse(fd, 503, "No proxy instances available");
            } else {
                std::ostringstream json;
                json << "{\"proxy\":{\"uri\":\"" << jsonEscape(proxy.uri) << "\","
                     << "\"engine\":\"" << jsonEscape(proxy.engine) << "\","
                     << "\"local_port\":" << proxy.local_port << ","
                     << "\"latency_ms\":" << proxy.latency_ms << ","
                     << "\"healthy\":" << (proxy.healthy ? "true" : "false") << "}}";
                sendJsonResponse(fd, 200, json.str());
            }
        } else {
            sendErrorResponse(fd, 405, "Method not allowed");
        }
        return;
    }

    if (path == "/" || path == "/api/gateway" || path == "/api/gateway/") {
        std::ostringstream json;
        json << "{\"service\":\"proxy_gateway\","
             << "\"version\":\"1.0\","
             << "\"endpoints\":["
             << "\"/api/gateway/status\","
             << "\"/api/gateway/sessions\","
             << "\"/api/gateway/pool\","
             << "\"/api/gateway/proxy\""
             << "]}";
        sendJsonResponse(fd, 200, json.str());
        return;
    }

    sendErrorResponse(fd, 404, "Not found");
}

void ProxyGateway::handleProxyConnect(int client_fd_raw, const std::string& target_host,
                                       int target_port, const std::string& session_id) {
    socket_t client_fd = static_cast<socket_t>(client_fd_raw);

    auto proxy = selectBestProxy();
    if (proxy.uri.empty()) {
        // PRIORITY 3: DROP — no proxy and no fallback available
        std::string msg = "HTTP/1.1 503 Service Unavailable\r\n\r\nNo proxy available - traffic dropped";
        sendAll(client_fd, msg.data(), msg.size());
        closeSock(client_fd);
        fallback_events_.fetch_add(1);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.assigned_uri = proxy.uri;
            it->second.assigned_engine = proxy.engine;
            it->second.assigned_port = proxy.local_port;
            it->second.last_activity_ts = utils::nowTimestamp();
        }
    }

    socket_t proxy_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (proxy_fd == kInvalidSocket) {
        std::string msg = "HTTP/1.1 502 Bad Gateway\r\n\r\nCannot create socket";
        sendAll(client_fd, msg.data(), msg.size());
        closeSock(client_fd);
        return;
    }

    // Connect to proxy: local provisioned port (positive) or remote SOCKS5 fallback (negative)
    sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    if (proxy.local_port > 0) {
        // Local provisioned proxy
        proxy_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        proxy_addr.sin_port = htons(static_cast<uint16_t>(proxy.local_port));
    } else {
        // Remote SOCKS5 fallback (negative port signals remote)
        int remote_port = -proxy.local_port;
        // Extract host from uri: socks5://172.20.14.34:3100
        std::string host = "172.20.14.34";
        size_t host_start = proxy.uri.find("//");
        if (host_start != std::string::npos) {
            size_t colon = proxy.uri.find(':', host_start + 2);
            if (colon != std::string::npos) {
                host = proxy.uri.substr(host_start + 2, colon - host_start - 2);
            }
        }
        proxy_addr.sin_addr.s_addr = inet_addr(host.c_str());
        proxy_addr.sin_port = htons(static_cast<uint16_t>(remote_port));
    }

    if (connect(proxy_fd, reinterpret_cast<sockaddr*>(&proxy_addr), sizeof(proxy_addr)) != 0) {
        std::string msg = "HTTP/1.1 502 Bad Gateway\r\n\r\nCannot connect to proxy";
        sendAll(client_fd, msg.data(), msg.size());
        closeSock(proxy_fd);
        closeSock(client_fd);
        fallback_events_.fetch_add(1);
        return;
    }

    std::string socks_request;
    socks_request.push_back(static_cast<char>(0x05));
    socks_request.push_back(static_cast<char>(0x01));
    socks_request.push_back(static_cast<char>(0x00));

    if (!sendAll(proxy_fd, socks_request.data(), socks_request.size())) {
        sendAll(client_fd, "HTTP/1.1 502 Bad Gateway\r\n\r\nSOCKS handshake failed", 42);
        closeSock(proxy_fd);
        closeSock(client_fd);
        return;
    }

    char socks_reply[2];
#ifdef _WIN32
    int got = recv(proxy_fd, socks_reply, 2, 0);
#else
    ssize_t got = recv(proxy_fd, socks_reply, 2, 0);
#endif
    if (got != 2 || socks_reply[0] != 0x05) {
        sendAll(client_fd, "HTTP/1.1 502 Bad Gateway\r\n\r\nSOCKS auth failed", 43);
        closeSock(proxy_fd);
        closeSock(client_fd);
        return;
    }

    std::string connect_req;
    connect_req.push_back(static_cast<char>(0x05));
    connect_req.push_back(static_cast<char>(0x01));
    connect_req.push_back(static_cast<char>(0x00));
    connect_req.push_back(static_cast<char>(0x03));
    connect_req.push_back(static_cast<char>(target_host.size()));
    connect_req.append(target_host);
    connect_req.push_back(static_cast<char>((target_port >> 8) & 0xFF));
    connect_req.push_back(static_cast<char>(target_port & 0xFF));

    if (!sendAll(proxy_fd, connect_req.data(), connect_req.size())) {
        sendAll(client_fd, "HTTP/1.1 502 Bad Gateway\r\n\r\nSOCKS connect failed", 45);
        closeSock(proxy_fd);
        closeSock(client_fd);
        return;
    }

    char connect_reply[10];
#ifdef _WIN32
    int cr = recv(proxy_fd, connect_reply, 10, 0);
#else
    ssize_t cr = recv(proxy_fd, connect_reply, 10, 0);
#endif
    if (cr < 2 || connect_reply[1] != 0x00) {
        sendAll(client_fd, "HTTP/1.1 502 Bad Gateway\r\n\r\nSOCKS connection refused", 49);
        closeSock(proxy_fd);
        closeSock(client_fd);
        fallback_events_.fetch_add(1);
        return;
    }

    std::string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (!sendAll(client_fd, ok.data(), ok.size())) {
        closeSock(proxy_fd);
        closeSock(client_fd);
        return;
    }

    fd_set read_fds;
    char buf[65536];
    uint64_t bytes_sent = 0, bytes_received = 0;
    int max_fd = static_cast<int>(std::max(client_fd, proxy_fd)) + 1;

    while (running_.load()) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        FD_SET(proxy_fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = 120;
        tv.tv_usec = 0;

#ifdef _WIN32
        int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
        int ready = select(max_fd, &read_fds, nullptr, nullptr, &tv);
#endif
        if (ready <= 0) break;

        if (FD_ISSET(client_fd, &read_fds)) {
#ifdef _WIN32
            int n = recv(client_fd, buf, sizeof(buf), 0);
#else
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            if (!sendAll(proxy_fd, buf, static_cast<size_t>(n))) break;
            bytes_sent += static_cast<uint64_t>(n);
        }

        if (FD_ISSET(proxy_fd, &read_fds)) {
#ifdef _WIN32
            int n = recv(proxy_fd, buf, sizeof(buf), 0);
#else
            ssize_t n = recv(proxy_fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            if (!sendAll(client_fd, buf, static_cast<size_t>(n))) break;
            bytes_received += static_cast<uint64_t>(n);
        }
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second.bytes_sent += bytes_sent;
            it->second.bytes_received += bytes_received;
            it->second.last_activity_ts = utils::nowTimestamp();
        }
    }
    total_bytes_.fetch_add(bytes_sent + bytes_received);

    closeSock(proxy_fd);
    closeSock(client_fd);
}

std::string ProxyGateway::createSession(const std::string& user_token) {
    std::string session_id = generateSessionId();
    GatewaySession session;
    session.session_id = session_id;
    session.user_token = user_token;
    session.created_ts = utils::nowTimestamp();
    session.last_activity_ts = session.created_ts;

    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[session_id] = session;
    }
    total_sessions_.fetch_add(1);
    return session_id;
}

bool ProxyGateway::destroySession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return sessions_.erase(session_id) > 0;
}

GatewaySession* ProxyGateway::findSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    return it != sessions_.end() ? &it->second : nullptr;
}

void ProxyGateway::touchSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_activity_ts = utils::nowTimestamp();
    }
}

ProxyPoolEntry ProxyGateway::selectBestProxy() {
    refreshProxyPool();

    std::lock_guard<std::mutex> lock(pool_mutex_);
    ProxyPoolEntry best;
    best.latency_ms = 1e9f;

    for (const auto& entry : proxy_pool_) {
        if (!entry.healthy || entry.local_port == 0) continue;
        if (entry.latency_ms < best.latency_ms) {
            best = entry;
        }
    }

    if (best.uri.empty() && !proxy_pool_.empty()) {
        for (const auto& entry : proxy_pool_) {
            if (entry.local_port > 0) {
                best = entry;
                break;
            }
        }
    }

    // PRIORITY 2: Fallback to internal SOCKS5 pool if no discovered proxy available
    if (best.uri.empty()) {
        static const char* fallback_hosts[] = {
            "172.20.14.34", "172.20.14.34", "172.20.14.34", "172.20.14.34", "172.20.14.34"
        };
        static const int fallback_ports[] = { 3100, 3101, 3102, 3103, 3104 };
        static std::atomic<int> rr_counter{0};
        int idx = rr_counter.fetch_add(1) % 5;
        best.uri = "socks5://";
        best.uri += fallback_hosts[idx];
        best.uri += ":";
        best.uri += std::to_string(fallback_ports[idx]);
        best.engine = "socks5-fallback";
        best.local_port = -fallback_ports[idx]; // Negative signals remote SOCKS5
        best.healthy = true;
        best.latency_ms = 500.0f;
    }

    return best;
}

void ProxyGateway::refreshProxyPool() {
    if (pool_callback_) {
        auto entries = pool_callback_();
        updateProxyPool(entries);
    }
}

void ProxyGateway::updateProxyPool(const std::vector<ProxyPoolEntry>& entries) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    proxy_pool_ = entries;
}

void ProxyGateway::healthMonitorLoop() {
    while (running_.load()) {
        refreshProxyPool();

        double now = utils::nowTimestamp();
        std::vector<std::string> expired;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (auto& [id, session] : sessions_) {
                if (now - session.last_activity_ts > 600.0) {
                    session.active = false;
                    expired.push_back(id);
                }
            }
            for (const auto& id : expired) {
                sessions_.erase(id);
            }
        }

        if (metrics_callback_) {
            metrics_callback_(getMetrics());
        }

        for (int i = 0; i < 30 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

GatewayMetrics ProxyGateway::getMetrics() const {
    GatewayMetrics m;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        m.active_sessions = static_cast<int>(sessions_.size());
    }
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        m.live_proxy_instances = static_cast<int>(
            std::count_if(proxy_pool_.begin(), proxy_pool_.end(),
                          [](const ProxyPoolEntry& e) { return e.healthy; }));
        double total_latency = 0;
        int count = 0;
        for (const auto& e : proxy_pool_) {
            if (e.healthy) {
                total_latency += e.latency_ms;
                count++;
            }
        }
        m.avg_latency_ms = count > 0 ? total_latency / count : 0;
    }
    m.total_sessions = total_sessions_.load();
    m.total_bytes_proxied = total_bytes_.load();
    m.fallback_events = fallback_events_.load();
    return m;
}

std::vector<GatewaySession> ProxyGateway::getActiveSessions() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::vector<GatewaySession> result;
    for (const auto& [id, session] : sessions_) {
        result.push_back(session);
    }
    return result;
}

std::vector<ProxyPoolEntry> ProxyGateway::getProxyPool() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return proxy_pool_;
}

std::string ProxyGateway::buildStatusJson() const {
    auto m = getMetrics();
    std::ostringstream json;
    json << "{"
         << "\"service\":\"proxy_gateway\","
         << "\"running\":" << (running_.load() ? "true" : "false") << ","
         << "\"port\":" << http_port_ << ","
         << "\"active_sessions\":" << m.active_sessions << ","
         << "\"total_sessions\":" << m.total_sessions << ","
         << "\"live_proxy_instances\":" << m.live_proxy_instances << ","
         << "\"avg_latency_ms\":" << static_cast<int>(m.avg_latency_ms) << ","
         << "\"total_bytes_proxied\":" << m.total_bytes_proxied << ","
         << "\"fallback_events\":" << m.fallback_events
         << "}";
    return json.str();
}

std::string ProxyGateway::buildSessionsJson() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& [id, s] : sessions_) {
        if (!first) json << ",";
        first = false;
        json << "{\"session_id\":\"" << jsonEscape(s.session_id) << "\","
             << "\"assigned_uri\":\"" << jsonEscape(s.assigned_uri) << "\","
             << "\"assigned_engine\":\"" << jsonEscape(s.assigned_engine) << "\","
             << "\"assigned_port\":" << s.assigned_port << ","
             << "\"bytes_sent\":" << s.bytes_sent << ","
             << "\"bytes_received\":" << s.bytes_received << ","
             << "\"active\":" << (s.active ? "true" : "false") << ","
             << "\"created_ts\":" << static_cast<long long>(s.created_ts) << ","
             << "\"last_activity_ts\":" << static_cast<long long>(s.last_activity_ts)
             << "}";
    }
    json << "]";
    return json.str();
}

std::string ProxyGateway::buildPoolJson() const {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    std::ostringstream json;
    json << "[";
    bool first = true;
    for (const auto& e : proxy_pool_) {
        if (!first) json << ",";
        first = false;
        json << "{\"uri\":\"" << jsonEscape(e.uri) << "\","
             << "\"engine\":\"" << jsonEscape(e.engine) << "\","
             << "\"local_port\":" << e.local_port << ","
             << "\"latency_ms\":" << static_cast<int>(e.latency_ms) << ","
             << "\"healthy\":" << (e.healthy ? "true" : "false") << ","
             << "\"active_sessions\":" << e.active_sessions
             << "}";
    }
    json << "]";
    return json.str();
}

void ProxyGateway::sendHttpResponse(int fd_raw, int status_code,
                                     const std::string& content_type,
                                     const std::string& body) {
    socket_t fd = static_cast<socket_t>(fd_raw);
    std::string status_text;
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 201: status_text = "Created"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 503: status_text = "Service Unavailable"; break;
        default: status_text = "OK"; break;
    }

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status_code << " " << status_text << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
         << "Access-Control-Allow-Headers: Content-Type, Authorization, X-Session-Id\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;
    std::string response = resp.str();
    sendAll(fd, response.data(), response.size());
}

void ProxyGateway::sendJsonResponse(int fd_raw, int status_code, const std::string& json) {
    sendHttpResponse(fd_raw, status_code, "application/json", json);
}

void ProxyGateway::sendErrorResponse(int fd_raw, int status_code, const std::string& message) {
    std::ostringstream json;
    json << "{\"error\":\"" << jsonEscape(message) << "\",\"status\":" << status_code << "}";
    sendJsonResponse(fd_raw, status_code, json.str());
}

std::string ProxyGateway::generateSessionId() {
    return generateRandomId(32);
}

std::string ProxyGateway::urlDecode(const std::string& str) {
    std::string out;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int val = 0;
            std::istringstream iss(str.substr(i + 1, 2));
            iss >> std::hex >> val;
            out.push_back(static_cast<char>(val));
            i += 2;
        } else if (str[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(str[i]);
        }
    }
    return out;
}

std::string ProxyGateway::parsePath(const std::string& request_line) {
    auto first_space = request_line.find(' ');
    if (first_space == std::string::npos) return "/";
    auto second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string::npos) return "/";
    return request_line.substr(first_space + 1, second_space - first_space - 1);
}

std::string ProxyGateway::parseMethod(const std::string& request_line) {
    auto space = request_line.find(' ');
    if (space == std::string::npos) return "GET";
    return request_line.substr(0, space);
}

} // namespace proxy
} // namespace hunter
