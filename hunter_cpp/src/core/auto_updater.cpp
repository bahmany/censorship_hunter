#include "core/auto_updater.h"
#include "core/utils.h"
#include "network/http_client.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <cmath>

#ifdef _WIN32
    #include <windows.h>
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

namespace fs = std::filesystem;

namespace hunter {
namespace core {

// ═════════════════════════════════════════════════════════════════════
// Singleton instance
// ═════════════════════════════════════════════════════════════════════

AutoUpdater& AutoUpdater::getInstance() {
    static AutoUpdater instance;
    return instance;
}

AutoUpdater::AutoUpdater() {
    current_progress_.current_stage = UpdateProgress::Stage::IDLE;
    current_progress_.download_progress = 0.0f;
    current_progress_.status_message = "Ready";
}

AutoUpdater::~AutoUpdater() = default;

// ═════════════════════════════════════════════════════════════════════
// Public API
// ═════════════════════════════════════════════════════════════════════

std::string AutoUpdater::getCurrentVersion() {
    // Return version from HUNTER_VERSION macro or hardcoded
    // This should match the version in CMakeLists.txt
    return HUNTER_VERSION_STRING;
}

std::string AutoUpdater::getLatestVersion() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return latest_version_;
}

void AutoUpdater::checkForUpdatesAsync(std::function<void(const UpdateInfo&)> on_complete) {
    std::thread([this, on_complete]() {
        UpdateInfo info = checkForUpdates();
        if (on_complete) {
            on_complete(info);
        }
    }).detach();
}

UpdateInfo AutoUpdater::checkForUpdates() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    current_progress_.current_stage = UpdateProgress::Stage::CHECKING;
    current_progress_.status_message = "Checking for updates...";

    UpdateInfo info;
    if (!fetchLatestReleaseInfo(info)) {
        current_progress_.current_stage = UpdateProgress::Stage::ERROR;
        current_progress_.error_message = "Failed to check for updates";
        info.error_message = "Network error: Could not reach GitHub API";
        return info;
    }

    cached_update_info_ = info;

    // Compare versions
    std::string current = getCurrentVersion();
    
    // Simple version comparison (e.g., "1.13.0" > "1.12.0")
    auto parseVersion = [](const std::string& v) -> std::tuple<int, int, int> {
        int major = 0, minor = 0, patch = 0;
        sscanf(v.c_str(), "%d.%d.%d", &major, &minor, &patch);
        return std::make_tuple(major, minor, patch);
    };

    auto [curr_major, curr_minor, curr_patch] = parseVersion(current);
    auto [new_major, new_minor, new_patch] = parseVersion(info.version);

    info.is_available = (new_major > curr_major) ||
                        (new_major == curr_major && new_minor > curr_minor) ||
                        (new_major == curr_major && new_minor == curr_minor && new_patch > curr_patch);

    if (info.is_available) {
        latest_version_ = info.version;
        current_progress_.current_stage = UpdateProgress::Stage::IDLE;
        current_progress_.status_message = "Update available: v" + info.version;
    } else {
        current_progress_.current_stage = UpdateProgress::Stage::IDLE;
        current_progress_.status_message = "Already up to date";
    }

    return info;
}

bool AutoUpdater::downloadAndApply(
    const std::string& url,
    const std::string& expected_sha256,
    std::function<void(const UpdateProgress&)> on_progress) {

    cancel_flag_ = false;

    // Download
    current_progress_.current_stage = UpdateProgress::Stage::DOWNLOADING;
    current_progress_.status_message = "Downloading update...";
    current_progress_.download_progress = 0.0f;
    if (on_progress) on_progress(current_progress_);

    std::string temp_dir = getTempDirectory();
    std::string download_path = temp_dir + "/hunter_update_" + std::to_string(time(nullptr));

#ifdef _WIN32
    download_path += ".exe";
#endif

    auto progress_cb = [this, on_progress](float pct) {
        current_progress_.download_progress = pct;
        if (on_progress) on_progress(current_progress_);
    };

    if (!downloadFile(url, download_path, progress_cb)) {
        current_progress_.current_stage = UpdateProgress::Stage::ERROR;
        current_progress_.error_message = "Download failed";
        if (on_progress) on_progress(current_progress_);
        return false;
    }

    if (cancel_flag_) {
        fs::remove(download_path);
        return false;
    }

    // Verify
    current_progress_.current_stage = UpdateProgress::Stage::VERIFYING;
    current_progress_.status_message = "Verifying integrity...";
    if (on_progress) on_progress(current_progress_);

    if (!verifyDownloadedFile(download_path, expected_sha256)) {
        current_progress_.current_stage = UpdateProgress::Stage::ERROR;
        current_progress_.error_message = "Integrity check failed";
        fs::remove(download_path);
        if (on_progress) on_progress(current_progress_);
        return false;
    }

    if (cancel_flag_) {
        fs::remove(download_path);
        return false;
    }

    // Apply
    current_progress_.current_stage = UpdateProgress::Stage::APPLYING;
    current_progress_.status_message = "Applying update...";
    if (on_progress) on_progress(current_progress_);

    bool success = replaceBinary(download_path);

    if (success) {
        current_progress_.current_stage = UpdateProgress::Stage::COMPLETE;
        current_progress_.status_message = "Update complete! Relaunching...";
    } else {
        current_progress_.current_stage = UpdateProgress::Stage::ERROR;
        current_progress_.error_message = "Failed to apply update";
        fs::remove(download_path);
    }

    if (on_progress) on_progress(current_progress_);

    return success;
}

UpdateProgress AutoUpdater::getProgress() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_progress_;
}

void AutoUpdater::cancel() {
    cancel_flag_ = true;
}

// ═════════════════════════════════════════════════════════════════════
// GitHub API Integration
// ═════════════════════════════════════════════════════════════════════

bool AutoUpdater::fetchLatestReleaseInfo(UpdateInfo& out) {
    try {
        network::HttpClient http;
        std::string api_url = "https://api.github.com/repos/bahmany/censorship_hunter/releases/latest";
        std::string response = http.get(api_url, 15000);

        if (response.empty()) {
            return false;
        }

        // Simple JSON parsing (looking for version, download URL, etc.)
        // Find tag_name
        size_t tag_pos = response.find("\"tag_name\"");
        if (tag_pos == std::string::npos) {
            return false;
        }

        size_t colon_pos = response.find(":", tag_pos);
        size_t quote1 = response.find("\"", colon_pos);
        size_t quote2 = response.find("\"", quote1 + 1);
        out.version = response.substr(quote1 + 1, quote2 - quote1 - 1);

        // Find body (release notes)
        size_t body_pos = response.find("\"body\"");
        if (body_pos != std::string::npos) {
            size_t body_colon = response.find(":", body_pos);
            size_t body_quote1 = response.find("\"", body_colon);
            size_t body_quote2 = response.find("\"", body_quote1 + 1);
            out.release_notes = response.substr(body_quote1 + 1, body_quote2 - body_quote1 - 1);
        }

        // Find download URL for this platform
#ifdef _WIN32
        std::string search_pattern = "hunter-windows-x64.exe";
#elif defined(__APPLE__)
        std::string search_pattern = "hunter-macos";
#elif defined(__ANDROID__)
        std::string search_pattern = "hunter-android";
#else
        std::string search_pattern = "hunter-linux";
#endif

        size_t asset_pos = response.find(search_pattern);
        if (asset_pos == std::string::npos) {
            return false;
        }

        // Find the download URL for this asset
        size_t url_search_start = asset_pos - 200;
        if (url_search_start > response.size()) url_search_start = 0;
        
        size_t url_pos = response.find("\"browser_download_url\"", url_search_start);
        if (url_pos == std::string::npos) {
            return false;
        }

        size_t url_colon = response.find(":", url_pos);
        size_t url_quote1 = response.find("\"", url_colon);
        size_t url_quote2 = response.find("\"", url_quote1 + 1);
        out.download_url = response.substr(url_quote1 + 1, url_quote2 - url_quote1 - 1);

        // Find SHA256 in digest field
        size_t digest_search_start = asset_pos - 300;
        if (digest_search_start > response.size()) digest_search_start = 0;
        
        size_t digest_pos = response.find("\"digest\"", digest_search_start);
        if (digest_pos != std::string::npos) {
            size_t digest_colon = response.find(":", digest_pos);
            size_t digest_quote1 = response.find("\"", digest_colon);
            size_t digest_quote2 = response.find("\"", digest_quote1 + 1);
            out.sha256 = response.substr(digest_quote1 + 1, digest_quote2 - digest_quote1 - 1);
        }

        return !out.version.empty() && !out.download_url.empty();
    } catch (const std::exception& e) {
        out.error_message = std::string("Exception: ") + e.what();
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════
// File Download
// ═════════════════════════════════════════════════════════════════════

bool AutoUpdater::downloadFile(
    const std::string& url,
    const std::string& save_path,
    std::function<void(float)> progress_cb) {

    try {
        network::HttpClient http;
        
        // Create parent directories if needed
        fs::create_directories(fs::path(save_path).parent_path());

        // Download with progress
        std::ofstream outfile(save_path, std::ios::binary);
        if (!outfile.is_open()) {
            return false;
        }

        // Note: HttpClient should support progress callback
        // For now, we'll do a simple download
        std::string content = http.get(url, 30000);
        
        if (content.empty()) {
            outfile.close();
            fs::remove(save_path);
            return false;
        }

        outfile.write(content.c_str(), content.size());
        outfile.close();

        if (progress_cb) {
            progress_cb(1.0f);
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════
// File Verification
// ═════════════════════════════════════════════════════════════════════

bool AutoUpdater::verifyDownloadedFile(
    const std::string& file_path,
    const std::string& expected_sha256) {

    if (expected_sha256.empty()) {
        // Skip verification if no SHA256 provided
        return true;
    }

    try {
        std::string computed = calculateSHA256(file_path);
        std::string expected = expected_sha256;

        // Normalize: remove "sha256:" prefix if present
        if (expected.find("sha256:") == 0) {
            expected = expected.substr(7);
        }

        // Normalize to lowercase
        for (auto& c : computed) {
            c = std::tolower(static_cast<unsigned char>(c));
        }
        for (auto& c : expected) {
            c = std::tolower(static_cast<unsigned char>(c));
        }

        return computed == expected;
    } catch (...) {
        return false;
    }
}

std::string AutoUpdater::calculateSHA256(const std::string& file_path) {
    // TODO: Implement SHA256 calculation using system library
    // For now, return empty (will be implemented with proper crypto library)
    // This is a placeholder - in production, use OpenSSL or similar
    return "";
}

// ═════════════════════════════════════════════════════════════════════
// Binary Replacement
// ═════════════════════════════════════════════════════════════════════

bool AutoUpdater::replaceBinary(const std::string& new_binary_path) {
#ifdef _WIN32
    return replaceBinaryWindows(new_binary_path);
#elif defined(__APPLE__)
    return replaceBinaryMacOS(new_binary_path);
#elif defined(__ANDROID__)
    return replaceBinaryAndroid(new_binary_path);
#else
    return replaceBinaryLinux(new_binary_path);
#endif
}

bool AutoUpdater::replaceBinaryWindows(const std::string& new_exe_path) {
    try {
        std::string current_exe = getCurrentExecutablePath();
        
        if (current_exe.empty()) {
            return false;
        }

        // Windows strategy:
        // 1. Rename current exe to .old
        // 2. Copy new exe to current location
        // 3. Start new exe
        // 4. Exit old process

        std::string backup_exe = current_exe + ".old";
        
        // Remove old backup if exists
        if (fs::exists(backup_exe)) {
            try {
                fs::remove(backup_exe);
            } catch (...) {
                // Ignore if can't delete
            }
        }

        // Rename current to .old
        try {
            fs::rename(current_exe, backup_exe);
        } catch (...) {
            // If rename fails, try copy then delete old
            fs::copy_file(new_exe_path, current_exe, fs::copy_options::overwrite_existing);
        }

        // Copy new binary to current location
        fs::copy_file(new_exe_path, current_exe, fs::copy_options::overwrite_existing);

        // Make executable (Windows might not need this)
        makeExecutable(current_exe);

        // Launch new process
        #ifdef _WIN32
            STARTUPINFOA si = {0};
            PROCESS_INFORMATION pi = {0};
            si.cb = sizeof(si);
            
            if (!CreateProcessA(
                current_exe.c_str(),
                nullptr,
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                nullptr,
                &si,
                &pi)) {
                return false;
            }
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        #endif

        // Exit current process
        exit(0);
        return true;
    } catch (...) {
        return false;
    }
}

bool AutoUpdater::replaceBinaryLinux(const std::string& new_binary_path) {
    try {
        std::string current_exe = getCurrentExecutablePath();
        
        if (current_exe.empty()) {
            return false;
        }

        // Linux strategy:
        // 1. Write new binary to temp location
        // 2. Make it executable
        // 3. Replace current binary atomically (rename)
        // 4. Exec new binary

        makeExecutable(new_binary_path);

        // Atomic rename
        if (rename(new_binary_path.c_str(), current_exe.c_str()) != 0) {
            return false;
        }

        // Execute new binary
        char* args[] = {(char*)current_exe.c_str(), nullptr};
        execv(current_exe.c_str(), args);
        
        // If execv fails, we never reach here
        return false;
    } catch (...) {
        return false;
    }
}

bool AutoUpdater::replaceBinaryMacOS(const std::string& new_binary_path) {
    try {
        std::string current_exe = getCurrentExecutablePath();
        
        if (current_exe.empty()) {
            return false;
        }

        // macOS strategy: Similar to Linux
        makeExecutable(new_binary_path);

        std::string backup_exe = current_exe + ".old";
        
        if (fs::exists(backup_exe)) {
            fs::remove(backup_exe);
        }

        fs::rename(current_exe, backup_exe);
        fs::copy_file(new_binary_path, current_exe, fs::copy_options::overwrite_existing);
        makeExecutable(current_exe);

        // Relaunch
        char* args[] = {(char*)current_exe.c_str(), nullptr};
        execv(current_exe.c_str(), args);
        
        return false;
    } catch (...) {
        return false;
    }
}

bool AutoUpdater::replaceBinaryAndroid(const std::string& new_apk_path) {
    // Android: Trigger install via Intent
    // This requires JNI/Java code to actually install the APK
    // For now, just copy to app cache
    try {
        std::string cache_dir = getTempDirectory();
        std::string target = cache_dir + "/hunter.apk";
        fs::copy_file(new_apk_path, target, fs::copy_options::overwrite_existing);
        // TODO: Trigger install intent
        return true;
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════════════════════════════════════════════
// Utilities
// ═════════════════════════════════════════════════════════════════════

std::string AutoUpdater::getCurrentExecutablePath() {
    try {
#ifdef _WIN32
        char buffer[MAX_PATH];
        GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        return std::string(buffer);
#else
        char buffer[256];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            return std::string(buffer);
        }
        return "";
#endif
    } catch (...) {
        return "";
    }
}

std::string AutoUpdater::getTempDirectory() {
    try {
#ifdef _WIN32
        char buffer[MAX_PATH];
        if (GetTempPathA(MAX_PATH, buffer) > 0) {
            return std::string(buffer);
        }
        return "C:\\Temp";
#else
        return "/tmp";
#endif
    } catch (...) {
#ifdef _WIN32
        return "C:\\Temp";
#else
        return "/tmp";
#endif
    }
}

bool AutoUpdater::makeExecutable(const std::string& path) {
#ifdef _WIN32
    // Windows: executables don't need special permissions
    return true;
#else
    try {
        return chmod(path.c_str(), 0755) == 0;
    } catch (...) {
        return false;
    }
#endif
}

} // namespace core
} // namespace hunter
