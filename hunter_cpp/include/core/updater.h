#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

namespace hunter {
namespace core {

/**
 * @brief Information about a GitHub release
 */
struct ReleaseInfo {
    std::string tag_name;           // e.g. "1.5.0"
    std::string release_name;       // e.g. "Hunter v1.5.0"
    std::string release_url;        // HTML URL of the release
    std::string body;               // Release notes markdown

    struct Asset {
        std::string name;                   // e.g. "hunter-windows-x64.exe"
        std::string browser_download_url;   // Direct download URL
        int size_bytes = 0;                 // File size
    };
    std::vector<Asset> assets;
};

/**
 * @brief Self-update manager — checks GitHub releases and updates the binary
 *
 * Fully automatic update flow:
 * 1. On startup, background thread checks GitHub releases API
 * 2. Compares latest tag_name with the compiled-in HUNTER_VERSION
 * 3. If newer, downloads the platform-appropriate asset
 * 4. Replaces the running binary (platform-specific)
 * 5. Relaunches the app
 *
 * Platform support:
 * - Windows: rename current .exe to .old, write new .exe, CreateProcess + exit
 * - Linux:   write new binary to temp, rename over current, exec
 * - Android: download .apk, trigger install Intent via JNI
 */
class SelfUpdateManager {
public:
    enum class UpdateState {
        Idle,               // Not checking
        Checking,           // Querying GitHub API
        UpToDate,           // Current version is latest
        UpdateAvailable,    // Newer version found, not yet downloading
        Downloading,        // Downloading the update asset
        DownloadComplete,   // Download finished, ready to apply
        Applying,           // Applying the update (replacing binary)
        UpdateComplete,     // Update applied, relaunching
        Failed,             // Update failed
    };

    /**
     * @brief Get the singleton instance
     */
    static SelfUpdateManager& instance();

    SelfUpdateManager(const SelfUpdateManager&) = delete;
    SelfUpdateManager& operator=(const SelfUpdateManager&) = delete;

    /**
     * @brief Start the automatic update check (background thread).
     *        Checks GitHub releases on startup, then periodically.
     * @param check_interval_seconds How often to re-check (default: 6 hours)
     * @param startup_delay_seconds  Delay before first check (default: 10s)
     */
    void startAutoCheck(int check_interval_seconds = 21600, int startup_delay_seconds = 10);

    /**
     * @brief Stop the automatic update check thread
     */
    void stopAutoCheck();

    /**
     * @brief Manually trigger an update check (synchronous)
     * @return True if an update is available
     */
    bool checkForUpdate();

    /**
     * @brief Download and apply the update (synchronous, blocks until done)
     * @return True if update was successfully applied
     */
    bool downloadAndApply();

    /**
     * @brief Get current update state
     */
    UpdateState state() const { return state_.load(); }

    /**
     * @brief Get human-readable status message
     */
    std::string statusMessage() const;

    /**
     * @brief Get download progress (0.0 - 1.0)
     */
    float downloadProgress() const { return download_progress_.load(); }

    /**
     * @brief Get the latest release info (if checked)
     */
    ReleaseInfo latestRelease() const;

    /**
     * @brief Get the current application version string
     */
    static std::string currentVersion();

    /**
     * @brief Compare two semver version strings
     * @return -1 if a < b, 0 if a == b, 1 if a > b
     */
    static int compareVersions(const std::string& a, const std::string& b);

    /**
     * @brief Set a callback that's called when state changes (for UI updates)
     */
    void setStateCallback(std::function<void(UpdateState)> callback);

    /**
     * @brief Get the path to the current executable
     */
    static std::string currentExecutablePath();

private:
    SelfUpdateManager();
    ~SelfUpdateManager();

    void autoCheckLoop(int interval, int startup_delay);
    void setState(UpdateState new_state, const std::string& message = "");

    /**
     * @brief Fetch latest release info from GitHub API
     */
    bool fetchLatestRelease(ReleaseInfo& out);

    /**
     * @brief Find the appropriate asset for the current platform
     */
    std::string findPlatformAsset(const ReleaseInfo& info) const;

    /**
     * @brief Download a file with progress tracking
     */
    bool downloadFile(const std::string& url, const std::string& dest_path);

    /**
     * @brief Apply the update: replace binary and relaunch
     */
    bool applyUpdate(const std::string& downloaded_path);

    // Platform-specific binary replacement
    bool replaceBinaryWindows(const std::string& new_binary_path);
    bool replaceBinaryLinux(const std::string& new_binary_path);
    bool replaceBinaryAndroid(const std::string& new_apk_path);

    // Platform-specific relaunch
    bool relaunchWindows();
    bool relaunchLinux();
    bool relaunchAndroid();

    std::atomic<UpdateState> state_{UpdateState::Idle};
    std::string status_message_;
    mutable std::mutex message_mutex_;

    std::atomic<float> download_progress_{0.0f};
    ReleaseInfo latest_release_;
    mutable std::mutex release_mutex_;

    std::thread check_thread_;
    std::atomic<bool> stop_flag_{false};
    std::function<void(UpdateState)> state_callback_;
    std::mutex callback_mutex_;

    std::string downloaded_file_path_;
};

} // namespace core
} // namespace hunter
