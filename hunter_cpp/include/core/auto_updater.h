#ifndef HUNTER_AUTO_UPDATER_H
#define HUNTER_AUTO_UPDATER_H

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <functional>

namespace hunter {
namespace core {

/**
 * @brief Auto-update manager with robust self-replacement capability
 * 
 * This module handles:
 * 1. Checking for new releases on GitHub
 * 2. Downloading updates
 * 3. Verifying integrity (SHA-256)
 * 4. Self-replacing the executable
 * 5. Relaunching the application
 */

struct UpdateInfo {
    std::string version;
    std::string download_url;
    std::string sha256;
    std::string release_notes;
    bool is_available = false;
    std::string error_message;
};

struct UpdateProgress {
    enum class Stage {
        IDLE,
        CHECKING,
        DOWNLOADING,
        VERIFYING,
        APPLYING,
        COMPLETE,
        ERROR
    };
    
    Stage current_stage = Stage::IDLE;
    float download_progress = 0.0f;  // 0.0 to 1.0
    std::string status_message;
    std::string error_message;
};

class AutoUpdater {
public:
    /**
     * @brief Get singleton instance
     */
    static AutoUpdater& getInstance();

    // Prevent copying
    AutoUpdater(const AutoUpdater&) = delete;
    AutoUpdater& operator=(const AutoUpdater&) = delete;

    /**
     * @brief Check for updates (async with callback)
     * @param on_complete Callback: void(const UpdateInfo&)
     */
    void checkForUpdatesAsync(std::function<void(const UpdateInfo&)> on_complete);

    /**
     * @brief Check for updates synchronously (blocking)
     * @return UpdateInfo with results
     */
    UpdateInfo checkForUpdates();

    /**
     * @brief Download and apply update
     * @param url Download URL
     * @param expected_sha256 SHA256 hash to verify
     * @param on_progress Progress callback
     * @return true if successful
     */
    bool downloadAndApply(
        const std::string& url,
        const std::string& expected_sha256,
        std::function<void(const UpdateProgress&)> on_progress = nullptr);

    /**
     * @brief Get current update progress
     */
    UpdateProgress getProgress() const;

    /**
     * @brief Cancel running update
     */
    void cancel();

    /**
     * @brief Get current application version
     */
    static std::string getCurrentVersion();

    /**
     * @brief Get latest version available
     */
    std::string getLatestVersion() const;

private:
    AutoUpdater();
    ~AutoUpdater();

    // GitHub API calls
    bool fetchLatestReleaseInfo(UpdateInfo& out);
    
    // Download with progress
    bool downloadFile(const std::string& url,
                      const std::string& save_path,
                      std::function<void(float)> progress_cb = nullptr);

    // Verification
    bool verifyDownloadedFile(const std::string& file_path,
                             const std::string& expected_sha256);

    // Platform-specific binary replacement
    bool replaceBinary(const std::string& new_binary_path);
    bool replaceBinaryWindows(const std::string& new_exe_path);
    bool replaceBinaryLinux(const std::string& new_binary_path);
    bool replaceBinaryMacOS(const std::string& new_binary_path);
    bool replaceBinaryAndroid(const std::string& new_apk_path);

    // Helper utilities
    std::string calculateSHA256(const std::string& file_path);
    std::string getCurrentExecutablePath();
    std::string getTempDirectory();
    bool makeExecutable(const std::string& path);

    // State management
    mutable std::mutex state_mutex_;
    std::atomic<bool> cancel_flag_{false};
    UpdateProgress current_progress_;
    UpdateInfo cached_update_info_;
    std::string latest_version_;
};

} // namespace core
} // namespace hunter

#endif // HUNTER_AUTO_UPDATER_H
