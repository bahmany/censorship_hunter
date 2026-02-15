#include "scraper.h"
#include "core/utils.h"

#include <android/log.h>

#define LOG_TAG "HunterTelegram"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace hunter {

// ---------- TelegramScraper ----------

TelegramScraper::TelegramScraper() {}

void TelegramScraper::set_fetch_callback(TelegramFetchCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    fetch_cb_ = std::move(cb);
}

void TelegramScraper::set_send_callback(TelegramSendCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_cb_ = std::move(cb);
}

void TelegramScraper::set_send_file_callback(TelegramSendFileCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_file_cb_ = std::move(cb);
}

std::set<std::string> TelegramScraper::scrape_configs(
    const std::vector<std::string>& channels, int limit) {

    std::set<std::string> configs;
    int consecutive_errors = 0;
    constexpr int max_consecutive_errors = 3;

    TelegramFetchCallback fetch_cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fetch_cb = fetch_cb_;
    }

    if (!fetch_cb) {
        LOGW("Telegram fetch callback not set");
        return configs;
    }

    for (const auto& channel : channels) {
        if (consecutive_errors >= max_consecutive_errors) {
            LOGW("Too many consecutive errors (%d), stopping scrape", consecutive_errors);
            break;
        }

        try {
            int fetch_limit = std::max(1, std::min(200, limit * 4));
            auto messages = fetch_cb(channel, fetch_limit);

            std::set<std::string> channel_configs;
            std::vector<std::string> channel_configs_ordered;

            for (const auto& message : messages) {
                if (static_cast<int>(channel_configs.size()) >= limit) break;

                auto found = extract_raw_uris_from_text(message);
                for (const auto& uri : found) {
                    if (static_cast<int>(channel_configs.size()) >= limit) break;
                    if (channel_configs.find(uri) == channel_configs.end()) {
                        channel_configs.insert(uri);
                        channel_configs_ordered.push_back(uri);
                    }
                }
            }

            configs.insert(channel_configs_ordered.begin(), channel_configs_ordered.end());
            LOGI("Scraped %zu configs from %s", channel_configs.size(), channel.c_str());
            consecutive_errors = 0;

        } catch (...) {
            consecutive_errors++;
            LOGW("Error scraping channel %s", channel.c_str());
        }
    }

    return configs;
}

bool TelegramScraper::send_report(const std::string& report_text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!send_cb_) return false;

    try {
        return send_cb_(report_text);
    } catch (...) {
        LOGE("Failed to send report to Telegram");
        return false;
    }
}

bool TelegramScraper::send_file(const std::string& filename, const std::string& content,
                                  const std::string& caption) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!send_file_cb_) return false;

    try {
        return send_file_cb_(filename, content, caption);
    } catch (...) {
        LOGE("Failed to send file to Telegram");
        return false;
    }
}

bool TelegramScraper::is_connected() const {
    return heartbeat_.is_connected();
}

void TelegramScraper::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    heartbeat_.mark_disconnected();
    LOGI("Telegram disconnected");
}

// ---------- TelegramReporter ----------

TelegramReporter::TelegramReporter(TelegramScraper& scraper)
    : scraper_(scraper) {}

void TelegramReporter::report_gold_configs(const std::vector<nlohmann::json>& configs) {
    if (configs.empty()) return;

    std::string report = "\xF0\x9F\x8F\x86 **Hunter Gold Configs Report**\n\n";

    int count = 0;
    for (const auto& config : configs) {
        if (count >= 10) break;
        ++count;
        std::string ps = config.value("ps", "Unknown");
        double latency = config.value("latency_ms", 0.0);
        report += std::to_string(count) + ". " + ps + " - " +
                  std::to_string(static_cast<int>(latency)) + "ms\n";
    }

    report += "\nTotal: " + std::to_string(configs.size()) + " gold configs available";

    scraper_.send_report(report);
}

void TelegramReporter::report_config_files(
    const std::vector<std::string>& gold_uris,
    const std::vector<std::string>* gemini_uris,
    int max_lines) {

    if (gold_uris.empty() && (!gemini_uris || gemini_uris->empty())) return;

    try {
        if (!gold_uris.empty()) {
            std::string content;
            int lines = 0;
            for (const auto& uri : gold_uris) {
                if (lines >= max_lines) break;
                content += uri + "\n";
                ++lines;
            }

            std::string caption = "HUNTER Gold (top " +
                                  std::to_string(std::min(static_cast<int>(gold_uris.size()), max_lines)) +
                                  "/" + std::to_string(gold_uris.size()) + ")";
            scraper_.send_file("HUNTER_gold.txt", content, caption);
        }
    } catch (...) {}

    try {
        if (gemini_uris && !gemini_uris->empty()) {
            std::string content;
            int lines = 0;
            for (const auto& uri : *gemini_uris) {
                if (lines >= max_lines) break;
                content += uri + "\n";
                ++lines;
            }

            std::string caption = "HUNTER Gemini (top " +
                                  std::to_string(std::min(static_cast<int>(gemini_uris->size()), max_lines)) +
                                  "/" + std::to_string(gemini_uris->size()) + ")";
            scraper_.send_file("HUNTER_gemini.txt", content, caption);
        }
    } catch (...) {}
}

void TelegramReporter::report_status(const nlohmann::json& status) {
    std::string report = "\xF0\x9F\x93\x8A **Hunter Status Report**\n\n";
    report += "Balancer: " + std::string(status.value("running", false) ? "Running" : "Stopped") + "\n";
    report += "Backends: " + std::to_string(status.value("backends", 0)) + "\n";

    if (status.contains("stats")) {
        report += "Restarts: " + std::to_string(status["stats"].value("restarts", 0)) + "\n";
    }

    scraper_.send_report(report);
}

} // namespace hunter
