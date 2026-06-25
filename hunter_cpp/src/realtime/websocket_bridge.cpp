#include "realtime/websocket_bridge.h"
#include "core/utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hunter {
namespace realtime {
namespace {

WebSocketBridge* g_active_bridge = nullptr;

#ifdef _WIN32
using native_socket_t = SOCKET;
static constexpr native_socket_t kInvalidSocket = INVALID_SOCKET;
#else
using native_socket_t = int;
static constexpr native_socket_t kInvalidSocket = -1;
#endif

native_socket_t toNative(intptr_t value) {
    return static_cast<native_socket_t>(value);
}

intptr_t toHandle(native_socket_t value) {
    return static_cast<intptr_t>(value);
}

void closeNativeSocket(native_socket_t fd) {
    if (fd == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool recvAll(native_socket_t fd, void* buf, size_t len) {
    char* out = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
#ifdef _WIN32
        int got = recv(fd, out + done, static_cast<int>(len - done), 0);
#else
        ssize_t got = recv(fd, out + done, len - done, 0);
#endif
        if (got <= 0) return false;
        done += static_cast<size_t>(got);
    }
    return true;
}

bool sendAll(native_socket_t fd, const void* buf, size_t len) {
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

std::string trimHeaderValue(const std::string& value) {
    return hunter::utils::trim(value);
}

std::string base64EncodeBytes(const uint8_t* data, size_t len) {
    static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) v |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kChars[(v >> 18) & 0x3F]);
        out.push_back(kChars[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kChars[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kChars[v & 0x3F] : '=');
    }
    return out;
}

std::array<uint8_t, 20> sha1Bytes(const std::string& input) {
    auto rol = [](uint32_t value, int bits) -> uint32_t {
        return (value << bits) | (value >> (32 - bits));
    };

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ULL;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));
    }

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        uint32_t w[80]{};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(data[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(data[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(data[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> out{};
    const uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>((words[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(words[i] & 0xFF);
    }
    return out;
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

std::string extractJsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) return "";
    const size_t colon = json.find(':', key_pos + needle.size());
    if (colon == std::string::npos) return "";
    size_t pos = colon + 1;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
    const size_t first_quote = json.find('"', pos);
    if (first_quote == std::string::npos) return "";
    std::string out;
    bool escape = false;
    for (size_t i = first_quote + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (!escape && ch == '"') return out;
        if (escape) {
            switch (ch) {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(ch); break;
            }
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
        } else {
            out.push_back(ch);
        }
    }
    return "";
}

} // namespace

WebSocketBridge::WebSocketBridge(int control_port, int monitor_port)
    : control_port_(control_port), monitor_port_(monitor_port) {}

WebSocketBridge::~WebSocketBridge() {
    stop();
}

void WebSocketBridge::setCommandHandler(CommandHandler handler) {
    command_handler_ = std::move(handler);
}

void WebSocketBridge::setStatusProvider(StatusProvider provider) {
    status_provider_ = std::move(provider);
}

void WebSocketBridge::setLogsProvider(LogsProvider provider) {
    logs_provider_ = std::move(provider);
}

bool WebSocketBridge::startListener(int port, intptr_t& listener_fd) {
    utils::ensureSocketLayer();
    native_socket_t fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) return false;

    int opt = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeNativeSocket(fd);
        return false;
    }
    if (listen(fd, 8) != 0) {
        closeNativeSocket(fd);
        return false;
    }
    listener_fd = toHandle(fd);
    return true;
}

bool WebSocketBridge::start() {
    if (running_.load()) return true;
    if (!startListener(control_port_, control_listener_)) return false;
    if (!startListener(monitor_port_, monitor_listener_)) {
        closeSocketFd(control_listener_);
        control_listener_ = -1;
        return false;
    }
    running_ = true;
    g_active_bridge = this;
    control_thread_ = std::thread(&WebSocketBridge::controlLoop, this);
    monitor_accept_thread_ = std::thread(&WebSocketBridge::monitorAcceptLoop, this);
    monitor_publish_thread_ = std::thread(&WebSocketBridge::monitorPublishLoop, this);
    heartbeat_thread_ = std::thread(&WebSocketBridge::heartbeatLoop, this);
    return true;
}

void WebSocketBridge::stop() {
    if (!running_.exchange(false)) return;
    if (g_active_bridge == this) g_active_bridge = nullptr;
    closeSocketFd(control_listener_);
    closeSocketFd(monitor_listener_);
    control_listener_ = -1;
    monitor_listener_ = -1;
    {
        std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
        for (const auto& client : monitor_clients_) {
            client->alive = false;
            closeSocketFd(client->fd);
        }
        monitor_clients_.clear();
    }
    if (control_thread_.joinable()) control_thread_.join();
    if (monitor_accept_thread_.joinable()) monitor_accept_thread_.join();
    if (monitor_publish_thread_.joinable()) monitor_publish_thread_.join();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
}

void WebSocketBridge::closeSocketFd(intptr_t fd) const {
    closeNativeSocket(toNative(fd));
}

std::string WebSocketBridge::httpWebSocketAcceptValue(const std::string& key) {
    static const std::string kMagic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = sha1Bytes(key + kMagic);
    return base64EncodeBytes(digest.data(), digest.size());
}

bool WebSocketBridge::performServerHandshake(intptr_t raw_fd) const {
    native_socket_t fd = toNative(raw_fd);
    std::string request;
    char buf[2048];
    while (request.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
        int got = recv(fd, buf, sizeof(buf), 0);
#else
        ssize_t got = recv(fd, buf, sizeof(buf), 0);
#endif
        if (got <= 0) return false;
        request.append(buf, buf + got);
        if (request.size() > 16384) return false;
    }

    std::istringstream ss(request);
    std::string line;
    std::string ws_key;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        const std::string value = trimHeaderValue(line.substr(colon + 1));
        if (name == "sec-websocket-key") {
            ws_key = value;
        }
    }
    if (ws_key.empty()) return false;

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << httpWebSocketAcceptValue(ws_key) << "\r\n\r\n";
    const std::string response = resp.str();
    return sendAll(fd, response.data(), response.size());
}

bool WebSocketBridge::readFrame(intptr_t raw_fd, std::string& out, uint8_t& opcode_out) const {
    native_socket_t fd = toNative(raw_fd);
    uint8_t header[2];
    if (!recvAll(fd, header, sizeof(header))) return false;
    const bool fin = (header[0] & 0x80) != 0;
    const uint8_t opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t len = static_cast<uint64_t>(header[1] & 0x7F);
    if (!fin) return false;
    if (opcode == 0x8) { opcode_out = 0x8; return false; }
    opcode_out = opcode;
    if (len == 126) {
        uint8_t ext[2];
        if (!recvAll(fd, ext, sizeof(ext))) return false;
        len = (static_cast<uint64_t>(ext[0]) << 8) | static_cast<uint64_t>(ext[1]);
    } else if (len == 127) {
        uint8_t ext[8];
        if (!recvAll(fd, ext, sizeof(ext))) return false;
        len = 0;
        for (uint8_t byte : ext) {
            len = (len << 8) | static_cast<uint64_t>(byte);
        }
    }
    if (len > (8ULL * 1024ULL * 1024ULL)) return false;

    std::string payload;
    payload.resize(static_cast<size_t>(len));
    if (len > 0) {
        if (masked) {
            uint8_t mask[4];
            if (!recvAll(fd, mask, sizeof(mask))) return false;
            if (!recvAll(fd, payload.data(), static_cast<size_t>(len))) return false;
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i % 4]);
            }
        } else {
            if (!recvAll(fd, payload.data(), static_cast<size_t>(len))) return false;
        }
    }
    out.swap(payload);
    return true;
}

bool WebSocketBridge::readTextFrame(intptr_t raw_fd, std::string& out) const {
    uint8_t opcode = 0;
    if (!readFrame(raw_fd, out, opcode)) return false;
    if (opcode != 0x1) return false;
    return true;
}

bool WebSocketBridge::sendPingFrame(intptr_t raw_fd) const {
    native_socket_t fd = toNative(raw_fd);
    uint8_t frame[2] = {0x89, 0x00};
    return sendAll(fd, frame, sizeof(frame));
}

bool WebSocketBridge::sendPongFrame(intptr_t raw_fd, const std::string& payload) const {
    native_socket_t fd = toNative(raw_fd);
    std::vector<uint8_t> frame;
    frame.push_back(0x8A);
    uint64_t len = static_cast<uint64_t>(payload.size());
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sendAll(fd, frame.data(), frame.size());
}

bool WebSocketBridge::sendPingFrame(const std::shared_ptr<ClientConn>& client) const {
    if (!client || !client->alive.load()) return false;
    std::lock_guard<std::mutex> lock(client->write_mutex);
    if (!sendPingFrame(client->fd)) {
        client->alive = false;
        closeSocketFd(client->fd);
        return false;
    }
    stats_.pings_sent.fetch_add(1);
    return true;
}

bool WebSocketBridge::sendTextFrame(intptr_t raw_fd, const std::string& payload) const {
    native_socket_t fd = toNative(raw_fd);
    std::vector<uint8_t> frame;
    frame.reserve(payload.size() + 16);
    frame.push_back(0x81);
    const uint64_t len = static_cast<uint64_t>(payload.size());
    if (len < 126) {
        frame.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sendAll(fd, frame.data(), frame.size());
}

bool WebSocketBridge::sendTextFrame(const std::shared_ptr<ClientConn>& client, const std::string& payload) const {
    if (!client || !client->alive.load()) return false;
    std::lock_guard<std::mutex> lock(client->write_mutex);
    if (!sendTextFrame(client->fd, payload)) {
        client->alive = false;
        closeSocketFd(client->fd);
        return false;
    }
    stats_.messages_sent.fetch_add(1);
    return true;
}

std::string WebSocketBridge::makeEvent(const std::string& type, const std::string& raw_json) const {
    hunter::utils::JsonBuilder jb;
    jb.add("type", type);
    jb.addRaw("payload", raw_json.empty() ? "{}" : raw_json);
    return jb.build();
}

// Returns a JSON object string like {"lines":["line1","line2"]}
static std::string makeLogLinesJson(const std::vector<std::string>& lines) {
    std::ostringstream arr;
    arr << "[";
    bool first = true;
    for (const auto& line : lines) {
        if (line.empty()) continue;
        if (!first) arr << ",";
        first = false;
        // Strip ANSI escape sequences before JSON-encoding
        std::string clean;
        clean.reserve(line.size());
        bool in_escape = false;
        for (size_t i = 0; i < line.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(line[i]);
            if (ch == 0x1B) { in_escape = true; continue; }
            if (in_escape) {
                if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) in_escape = false;
                continue;
            }
            clean.push_back(line[i]);
        }
        arr << '"' << jsonEscape(clean) << '"';
    }
    arr << "]";
    hunter::utils::JsonBuilder jb;
    jb.addRaw("lines", arr.str());
    return jb.build();
}

std::string WebSocketBridge::makeLogEvent(const std::vector<std::string>& lines) const {
    return makeEvent("logs", makeLogLinesJson(lines));
}

void WebSocketBridge::broadcastMonitorJson(const std::string& json_payload) {
    std::vector<std::shared_ptr<ClientConn>> clients;
    {
        std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
        clients = monitor_clients_;
    }
    for (const auto& client : clients) {
        sendTextFrame(client, json_payload);
    }
}

void WebSocketBridge::broadcastMonitorEvent(const std::string& type, const std::string& raw_json) {
    broadcastMonitorJson(makeEvent(type, raw_json));
}

void WebSocketBridge::removeDeadMonitorClients() {
    std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
    int removed = 0;
    monitor_clients_.erase(
        std::remove_if(monitor_clients_.begin(), monitor_clients_.end(), [&](const auto& client) {
            if (!client || !client->alive.load()) {
                removed++;
                return true;
            }
            return false;
        }),
        monitor_clients_.end());
    if (removed > 0) {
        stats_.total_disconnections.fetch_add(removed);
        stats_.active_monitor_clients.fetch_sub(removed);
    }
}

void WebSocketBridge::handleControlClient(intptr_t client_fd) {
    if (!performServerHandshake(client_fd)) {
        closeSocketFd(client_fd);
        return;
    }
    stats_.active_control_clients.fetch_add(1);
    std::string message;
    while (running_.load()) {
        uint8_t opcode = 0;
        if (!readFrame(client_fd, message, opcode)) break;
        if (opcode == 0x9) {
            sendPongFrame(client_fd, message);
            continue;
        }
        if (opcode == 0xA) {
            continue;
        }
        if (opcode != 0x1) continue;
        stats_.messages_received.fetch_add(1);
        const std::string request_id = extractJsonStringField(message, "request_id");
        if (!request_id.empty()) {
            std::ostringstream ack;
            ack << "{\"type\":\"command_ack\",\"request_id\":\"" << jsonEscape(request_id) << "\"}";
            if (!sendTextFrame(client_fd, ack.str())) break;
        }
        std::string response;
        try {
            response = command_handler_ ? command_handler_(message) : std::string("{\"type\":\"command_result\",\"ok\":false,\"message\":\"No command handler\"}");
        } catch (const std::exception& e) {
            response = std::string("{\"type\":\"command_result\",\"ok\":false,\"message\":\"") + jsonEscape(e.what()) + "\"}";
        } catch (...) {
            response = "{\"type\":\"command_result\",\"ok\":false,\"message\":\"Unknown command error\"}";
        }
        if (!sendTextFrame(client_fd, response)) break;
        const bool quiet_command = response.find("\"quiet\":true") != std::string::npos;
        if (!quiet_command && status_provider_) {
            const std::string status = status_provider_();
            if (!status.empty()) {
                broadcastMonitorJson(makeEvent("status", status));
            }
        }
    }
    stats_.active_control_clients.fetch_sub(1);
    closeSocketFd(client_fd);
}

void WebSocketBridge::controlLoop() {
    native_socket_t listener = toNative(control_listener_);
    while (running_.load()) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addr_len = sizeof(addr);
#else
        socklen_t addr_len = sizeof(addr);
#endif
        native_socket_t client = accept(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (client == kInvalidSocket) {
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::thread(&WebSocketBridge::handleControlClient, this, toHandle(client)).detach();
    }
}

void WebSocketBridge::monitorAcceptLoop() {
    native_socket_t listener = toNative(monitor_listener_);
    while (running_.load()) {
        sockaddr_in addr{};
#ifdef _WIN32
        int addr_len = sizeof(addr);
#else
        socklen_t addr_len = sizeof(addr);
#endif
        native_socket_t client = accept(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        if (client == kInvalidSocket) {
            if (!running_.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const intptr_t handle = toHandle(client);
        if (!performServerHandshake(handle)) {
            closeSocketFd(handle);
            continue;
        }
        auto conn = std::make_shared<ClientConn>();
        conn->fd = handle;
        conn->connected_at = std::chrono::steady_clock::now();
        conn->last_activity = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
            monitor_clients_.push_back(conn);
        }
        stats_.total_connections.fetch_add(1);
        stats_.active_monitor_clients.fetch_add(1);
        // Send initial data immediately — avoid calling status_provider_ here because
        // it acquires the orchestrator mutex (which the publish loop also holds).
        // Sending both concurrently causes multi-second serialisation delays.
        // The publish loop will deliver a full status update within one cycle (~500ms).
        {
            // 1. Send the log history immediately (ring buffer, no heavy mutex)
            try {
                const std::vector<std::string> recent =
                    hunter::utils::LogRingBuffer::instance().recent(200);
                if (!recent.empty() && conn->alive.load()) {
                    sendTextFrame(conn, makeLogEvent(recent));
                }
            } catch (...) {}
            // 2. Send current WS stats as a quick "connected" signal
            try {
                if (conn->alive.load()) {
                    sendTextFrame(conn, makeEvent("ws_stats", makeStatsJson()));
                }
            } catch (...) {}
        }
    }
}

void WebSocketBridge::monitorPublishLoop() {
    std::string last_status;
    int stats_counter = 0;
    int status_skip_counter = 0;

    while (running_.load()) {
        // === LOGS: always fast — ring buffer needs no heavy mutex ===
        try {
            std::vector<std::string> newLines =
                hunter::utils::LogRingBuffer::instance().fetchSince(log_since_, 200);
            if (!newLines.empty()) {
                broadcastMonitorJson(makeEvent("log_append", makeLogLinesJson(newLines)));
            }
        } catch (...) {}

        // === STATUS: direct call (provideStatus reads from file, no heavy mutex) ===
        // Only fetch status every ~1s (5 * 200ms) to avoid excessive file reads
        if (++status_skip_counter >= 5) {
            status_skip_counter = 0;
            try {
                if (status_provider_) {
                    std::string status = status_provider_();
                    if (!status.empty() && status != last_status) {
                        last_status = status;
                        broadcastMonitorJson(makeEvent("status", status));
                        auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
                        for (auto& c : monitor_clients_) {
                            if (c && c->alive.load()) c->last_activity = now;
                        }
                    }
                }
            } catch (...) {}
        }

        // === WS_STATS: every ~5 seconds (25 * 200ms) ===
        if (++stats_counter >= 25) {
            stats_counter = 0;
            broadcastMonitorJson(makeEvent("ws_stats", makeStatsJson()));
        }

        removeDeadMonitorClients();
        for (int i = 0; i < 2 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void WebSocketBridge::heartbeatLoop() {
    while (running_.load()) {
        for (int i = 0; i < 10 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<ClientConn>> clients;
        {
            std::lock_guard<std::mutex> lock(monitor_clients_mutex_);
            clients = monitor_clients_;
        }

        for (const auto& client : clients) {
            if (!client || !client->alive.load()) continue;

            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - client->last_activity).count();
            if (idle > 90) {
                client->alive = false;
                closeSocketFd(client->fd);
                continue;
            }

            if (idle > 30) {
                sendPingFrame(client);
            }
        }

        removeDeadMonitorClients();
    }
}

std::string WebSocketBridge::makeStatsJson() const {
    hunter::utils::JsonBuilder jb;
    jb.add("active_monitor_clients", stats_.active_monitor_clients.load())
      .add("active_control_clients", stats_.active_control_clients.load())
      .add("total_connections", stats_.total_connections.load())
      .add("total_disconnections", stats_.total_disconnections.load())
      .add("messages_sent", stats_.messages_sent.load())
      .add("messages_received", stats_.messages_received.load())
      .add("pings_sent", stats_.pings_sent.load())
      .add("pongs_received", stats_.pongs_received.load());
    return jb.build();
}

std::string WebSocketBridge::getStatsJson() const {
    hunter::utils::JsonBuilder jb;
    jb.add("active_monitor_clients", stats_.active_monitor_clients.load())
      .add("active_control_clients", stats_.active_control_clients.load())
      .add("total_connections", stats_.total_connections.load())
      .add("total_disconnections", stats_.total_disconnections.load())
      .add("messages_sent", stats_.messages_sent.load())
      .add("messages_received", stats_.messages_received.load())
      .add("pings_sent", stats_.pings_sent.load())
      .add("pongs_received", stats_.pongs_received.load());
    return jb.build();
}

int WebSocketBridge::getActiveMonitorClientCount() const {
    return stats_.active_monitor_clients.load();
}

bool broadcastGlobalMonitorEvent(const std::string& type, const std::string& raw_json) {
    if (!g_active_bridge || !g_active_bridge->isRunning()) return false;
    g_active_bridge->broadcastMonitorEvent(type, raw_json);
    return true;
}

} // namespace realtime
} // namespace hunter
