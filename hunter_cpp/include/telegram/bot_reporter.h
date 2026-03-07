#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "network/http_client.h"

namespace hunter {
namespace telegram {

/**
 * @brief Telegram Bot API reporter
 * 
 * Sends config files and status messages to Telegram groups/channels
 * using the Bot API. Supports file upload, text messages, and
 * proxy fallback for censored networks.
 */
class BotReporter {
public:
    BotReporter();
    ~BotReporter() = default;

    /**
     * @brief Configure with bot token and chat ID
     */
    void configure(const std::string& bot_token, const std::string& chat_id);

    /**
     * @brief Check if configured
     */
    bool isConfigured() const;

    /**
     * @brief Send a text message
     */
    bool sendMessage(const std::string& text);

    /**
     * @brief Send a file (document)
     * @param filename Display name for the file
     * @param content File content
     */
    bool sendFile(const std::string& filename, const std::string& content);

    /**
     * @brief Report validated configs as a file
     * @param gold_uris Gold-tier URIs
     * @param gemini_uris Optional Gemini-specific URIs
     * @param max_lines Max URIs to include
     * @return true on success
     */
    bool reportConfigFiles(const std::vector<std::string>& gold_uris,
                           const std::vector<std::string>& gemini_uris = {},
                           int max_lines = 50);

    /**
     * @brief Report system status
     */
    bool reportStatus(const std::string& status_text);

    /**
     * @brief Set proxy for Telegram API requests
     */
    void setProxy(const std::string& proxy_url);

    /**
     * @brief Get last error
     */
    std::string getLastError() const { return last_error_; }

    /**
     * @brief Get report status
     */
    struct ReportStatus {
        bool bot_configured = false;
        std::string bot_token_preview;
        int publish_count = 0;
        double last_publish = 0.0;
    };
    ReportStatus getReportStatus() const;

private:
    network::HttpClient http_;
    std::string bot_token_;
    std::string chat_id_;
    std::string proxy_url_;
    std::string last_error_;
    int publish_count_ = 0;
    double last_publish_ = 0.0;
    mutable std::mutex mutex_;

    std::string apiUrl(const std::string& method) const;
    bool sendApiRequest(const std::string& method, const std::string& json_body);
};

} // namespace telegram
} // namespace hunter
