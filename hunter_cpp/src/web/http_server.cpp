#include "web/http_server.h"
#include "core/utils.h"

#include <sstream>
#include <algorithm>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace hunter {
namespace web {

// ─── Response helpers ───

HttpServer::Response HttpServer::Response::json(const std::string& body, int code) {
    return {code, "application/json", body, {}};
}

HttpServer::Response HttpServer::Response::html(const std::string& body, int code) {
    return {code, "text/html; charset=utf-8", body, {}};
}

HttpServer::Response HttpServer::Response::notFound() {
    return {404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}", {}};
}

HttpServer::Response HttpServer::Response::error(const std::string& msg, int code) {
    return {code, "application/json", "{\"ok\":false,\"error\":\"" + msg + "\"}", {}};
}

// ─── HttpServer ───

HttpServer::HttpServer(int port) : port_(port) {
#ifdef _WIN32
    static bool ws_init = false;
    if (!ws_init) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); ws_init = true; }
#endif
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path, RouteHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    routes_.push_back({method, path, handler});
}

void HttpServer::get(const std::string& path, RouteHandler handler) {
    route("GET", path, handler);
}

void HttpServer::post(const std::string& path, RouteHandler handler) {
    route("POST", path, handler);
}

bool HttpServer::start() {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) return false;

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port_);

    if (bind(server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(server_fd_);
#else
        close(server_fd_);
#endif
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 16) < 0) {
#ifdef _WIN32
        closesocket(server_fd_);
#else
        close(server_fd_);
#endif
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    server_thread_ = std::thread(&HttpServer::serverLoop, this);
    std::cout << "Web admin dashboard: http://localhost:" << port_ << std::endl;
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
#ifdef _WIN32
        closesocket(server_fd_);
#else
        close(server_fd_);
#endif
        server_fd_ = -1;
    }
    if (server_thread_.joinable()) server_thread_.join();
}

void HttpServer::pushLog(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_buffer_.push_back({utils::nowTimestamp(), level, message});
    if ((int)log_buffer_.size() > MAX_LOG_BUFFER) {
        log_buffer_.erase(log_buffer_.begin());
    }
}

void HttpServer::serverLoop() {
    while (running_.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd_, &rfds);
        timeval tv{1, 0};

        int sel = select(server_fd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in client_addr{};
#ifdef _WIN32
        int addrlen = sizeof(client_addr);
#else
        socklen_t addrlen = sizeof(client_addr);
#endif
        int client_fd = accept(server_fd_, (sockaddr*)&client_addr, &addrlen);
        if (client_fd < 0) continue;

        // Handle in-thread (simple single-threaded server)
        try {
            handleClient(client_fd);
        } catch (...) {}

#ifdef _WIN32
        closesocket(client_fd);
#else
        close(client_fd);
#endif
    }
}

void HttpServer::handleClient(int client_fd) {
    char buf[8192];
    int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = 0;

    Request req = parseRequest(std::string(buf, n));
    Response resp = routeRequest(req);
    std::string raw_resp = buildResponse(resp);

    send(client_fd, raw_resp.c_str(), (int)raw_resp.size(), 0);
}

HttpServer::Request HttpServer::parseRequest(const std::string& raw) {
    Request req;
    std::istringstream ss(raw);
    std::string line;

    // Request line: GET /path HTTP/1.1
    if (std::getline(ss, line)) {
        auto parts = utils::split(utils::trim(line), ' ');
        if (parts.size() >= 2) {
            req.method = parts[0];
            std::string full_path = parts[1];
            auto q_pos = full_path.find('?');
            if (q_pos != std::string::npos) {
                req.path = full_path.substr(0, q_pos);
                std::string query = full_path.substr(q_pos + 1);
                for (auto& pair : utils::split(query, '&')) {
                    auto eq = pair.find('=');
                    if (eq != std::string::npos) {
                        req.query_params[pair.substr(0, eq)] = utils::urlDecode(pair.substr(eq + 1));
                    }
                }
            } else {
                req.path = full_path;
            }
        }
    }

    // Headers
    while (std::getline(ss, line)) {
        line = utils::trim(line);
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = utils::trim(line.substr(0, colon));
            std::string val = utils::trim(line.substr(colon + 1));
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            req.headers[key] = val;
        }
    }

    // Body (remaining)
    std::ostringstream body_ss;
    body_ss << ss.rdbuf();
    req.body = body_ss.str();

    return req;
}

std::string HttpServer::buildResponse(const Response& resp) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status_code << " ";
    switch (resp.status_code) {
        case 200: ss << "OK"; break;
        case 400: ss << "Bad Request"; break;
        case 404: ss << "Not Found"; break;
        case 500: ss << "Internal Server Error"; break;
        case 503: ss << "Service Unavailable"; break;
        default: ss << "Unknown"; break;
    }
    ss << "\r\n";
    ss << "Content-Type: " << resp.content_type << "\r\n";
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    ss << "Access-Control-Allow-Origin: *\r\n";
    ss << "Connection: close\r\n";
    for (auto& [k, v] : resp.headers) {
        ss << k << ": " << v << "\r\n";
    }
    ss << "\r\n";
    ss << resp.body;
    return ss.str();
}

HttpServer::Response HttpServer::routeRequest(const Request& req) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    for (auto& r : routes_) {
        if (r.method == req.method && r.path == req.path) {
            try {
                return r.handler(req);
            } catch (const std::exception& e) {
                return Response::error(e.what(), 500);
            }
        }
    }
    return Response::notFound();
}

std::string HttpServer::getLogStreamJson(int since_index) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    std::ostringstream ss;
    ss << "[";
    bool first = true;
    int idx = 0;
    for (auto& entry : log_buffer_) {
        if (idx++ < since_index) continue;
        if (!first) ss << ",";
        first = false;
        ss << "{\"timestamp\":" << entry.timestamp
           << ",\"level\":\"" << entry.level
           << "\",\"message\":\"" << utils::htmlEscape(entry.message) << "\"}";
    }
    ss << "]";
    return ss.str();
}

} // namespace web
} // namespace hunter
