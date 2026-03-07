#include "telegram/bot_reporter.h"
#include "core/utils.h"

#include <sstream>
#include <algorithm>

namespace hunter {
namespace telegram {

BotReporter::BotReporter() {}

void BotReporter::configure(const std::string& bot_token, const std::string& chat_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bot_token_ = bot_token;
    chat_id_ = chat_id;
}

bool BotReporter::isConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !bot_token_.empty() && !chat_id_.empty();
}

std::string BotReporter::apiUrl(const std::string& method) const {
    return "https://api.telegram.org/bot" + bot_token_ + "/" + method;
}

bool BotReporter::sendApiRequest(const std::string& method, const std::string& json_body) {
    if (!isConfigured()) { last_error_ = "Not configured"; return false; }
    std::string url = apiUrl(method);
    std::string resp = http_.post(url, json_body, "application/json", 15000, proxy_url_);
    if (resp.empty()) { last_error_ = "Empty response"; return false; }
    // Check for "ok":true in response
    return resp.find("\"ok\":true") != std::string::npos ||
           resp.find("\"ok\": true") != std::string::npos;
}

bool BotReporter::sendMessage(const std::string& text) {
    utils::JsonBuilder jb;
    jb.add("chat_id", chat_id_);
    jb.add("text", text);
    jb.add("parse_mode", "HTML");
    return sendApiRequest("sendMessage", jb.build());
}

bool BotReporter::sendFile(const std::string& filename, const std::string& content) {
    if (!isConfigured()) return false;

    // Use multipart/form-data for file upload
    std::string boundary = "----HunterBoundary" + utils::sha1Hex(content).substr(0, 8);
    std::ostringstream body;
    body << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
         << chat_id_ << "\r\n"
         << "--" << boundary << "\r\n"
         << "Content-Disposition: form-data; name=\"document\"; filename=\"" << filename << "\"\r\n"
         << "Content-Type: application/octet-stream\r\n\r\n"
         << content << "\r\n"
         << "--" << boundary << "--\r\n";

    std::string url = apiUrl("sendDocument");
    std::string content_type = "multipart/form-data; boundary=" + boundary;
    std::string resp = http_.post(url, body.str(), content_type, 30000, proxy_url_);
    return !resp.empty() && (resp.find("\"ok\":true") != std::string::npos ||
                              resp.find("\"ok\": true") != std::string::npos);
}

bool BotReporter::reportConfigFiles(const std::vector<std::string>& gold_uris,
                                     const std::vector<std::string>& gemini_uris,
                                     int max_lines) {
    if (!isConfigured()) return false;

    // Build file content
    std::ostringstream content;
    int count = 0;
    for (const auto& uri : gold_uris) {
        if (count >= max_lines) break;
        content << uri << "\n";
        count++;
    }

    std::string filename = "npvt.txt";
    bool ok = sendFile(filename, content.str());
    if (ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        publish_count_++;
        last_publish_ = utils::nowTimestamp();
    }

    // Send gemini file if provided
    if (!gemini_uris.empty()) {
        std::ostringstream gem_content;
        count = 0;
        for (const auto& uri : gemini_uris) {
            if (count >= max_lines) break;
            gem_content << uri << "\n";
            count++;
        }
        sendFile("gemini.txt", gem_content.str());
    }

    return ok;
}

bool BotReporter::reportStatus(const std::string& status_text) {
    return sendMessage(status_text);
}

void BotReporter::setProxy(const std::string& proxy_url) {
    proxy_url_ = proxy_url;
}

BotReporter::ReportStatus BotReporter::getReportStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ReportStatus s;
    s.bot_configured = !bot_token_.empty() && !chat_id_.empty();
    if (!bot_token_.empty() && bot_token_.size() > 8) {
        s.bot_token_preview = bot_token_.substr(0, 4) + "..." + bot_token_.substr(bot_token_.size() - 4);
    }
    s.publish_count = publish_count_;
    s.last_publish = last_publish_;
    return s;
}

} // namespace telegram
} // namespace hunter
