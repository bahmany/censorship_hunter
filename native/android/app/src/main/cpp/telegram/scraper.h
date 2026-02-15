#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "cache/cache.h"

namespace hunter {

// JNI callback: fetches messages from a Telegram channel via Bot API or public scraping
// Returns raw text content of messages
using TelegramFetchCallback = std::function<std::vector<std::string>(
    const std::string& channel, int limit)>;

// JNI callback: sends a message to Telegram channel
using TelegramSendCallback = std::function<bool(
    const std::string& text)>;

// JNI callback: sends a file to Telegram channel
using TelegramSendFileCallback = std::function<bool(
    const std::string& filename, const std::string& content, const std::string& caption)>;

class TelegramScraper {
public:
    explicit TelegramScraper();

    void set_fetch_callback(TelegramFetchCallback cb);
    void set_send_callback(TelegramSendCallback cb);
    void set_send_file_callback(TelegramSendFileCallback cb);

    std::set<std::string> scrape_configs(const std::vector<std::string>& channels,
                                          int limit = 100);
    bool send_report(const std::string& report_text);
    bool send_file(const std::string& filename, const std::string& content,
                   const std::string& caption = "");

    bool is_connected() const;
    void disconnect();

private:
    ResilientHeartbeat heartbeat_;
    mutable std::mutex mutex_;

    TelegramFetchCallback fetch_cb_;
    TelegramSendCallback send_cb_;
    TelegramSendFileCallback send_file_cb_;
    bool connected_ = false;
};

class TelegramReporter {
public:
    explicit TelegramReporter(TelegramScraper& scraper);

    void report_gold_configs(const std::vector<nlohmann::json>& configs);
    void report_config_files(const std::vector<std::string>& gold_uris,
                              const std::vector<std::string>* gemini_uris = nullptr,
                              int max_lines = 200);
    void report_status(const nlohmann::json& status);

private:
    TelegramScraper& scraper_;
};

} // namespace hunter
