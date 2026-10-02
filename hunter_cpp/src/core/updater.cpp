#include "core/updater.h"
#include "core/constants.h"
#include "core/utils.h"
#include "network/http_client.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif

#include <curl/curl.h>

namespace hunter {
namespace core {

namespace {

// ─── Minimal JSON field extractor ───
// Extracts string values from simple JSON without a full parser.
// Sufficient for the GitHub releases API response format.

std::string jsonExtractString(const std::string& json, const std::string& key) {
    // Search for "key": "value"
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos++; // skip ':'

    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;

    if (pos >= json.size() || json[pos] != '"') return "";

    pos++; // skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            switch (next) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                default: result += next; break;
            }
            pos += 2;
        } else {
            result += json[pos];
            pos++;
        }
    }
    return result;
}

int jsonExtractInt(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return 0;
    pos++;

    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        pos++;

    std::string num;
    while (pos < json.size() && (json[pos] >= '0' && json[pos] <= '9')) {
        num += json[pos];
        pos++;
    }
    if (num.empty()) return 0;
    try { return std::stoi(num); } catch (...) { return 0; }
}

// Extract all asset objects from the JSON (finds "assets": [ ... ])
std::vector<ReleaseInfo::Asset> extractAssets(const std::string& json) {
    std::vector<ReleaseInfo::Asset> assets;

    size_t assets_pos = json.find("\"assets\"");
    if (assets_pos == std::string::npos) return assets;

    size_t bracket_start = json.find('[', assets_pos);
    if (bracket_start == std::string::npos) return assets;

    // Find matching close bracket
    int depth = 0;
    size_t bracket_end = bracket_start;
    for (size_t i = bracket_start; i < json.size(); i++) {
        if (json[i] == '[') depth++;
        else if (json[i] == ']') {
            depth--;
            if (depth == 0) { bracket_end = i; break; }
        }
    }

    // Split by top-level object boundaries { ... }
    std::string assets_block = json.substr(bracket_start, bracket_end - bracket_start + 1);
    size_t pos = 0;
    while (pos < assets_block.size()) {
        size_t obj_start = assets_block.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching close brace
        int brace_depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < assets_block.size(); i++) {
            if (assets_block[i] == '{') brace_depth++;
            else if (assets_block[i] == '}') {
                brace_depth--;
                if (brace_depth == 0) { obj_end = i; break; }
            }
        }

        std::string obj = assets_block.substr(obj_start, obj_end - obj_start + 1);
        ReleaseInfo::Asset asset;
        asset.name = jsonExtractString(obj, "name");
        asset.browser_download_url = jsonExtractString(obj, "browser_download_url");
        asset.size_bytes = jsonExtractInt(obj, "size");
        if (!asset.name.empty() && !asset.browser_download_url.empty()) {
            assets.push_back(asset);
        }

        pos = obj_end + 1;
    }

    return assets;
}

// ─── CURL download with progress callback ───
struct DownloadContext {
    std::ofstream* file;
    std::atomic<float>* progress;
    std::string error;
};

size_t downloadWriteCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<DownloadContext*>(userdata);
    size_t total = size * nmemb;
    if (ctx->file && ctx->file->is_open()) {
        ctx->file->write(static_cast<const char*>(ptr), total);
        return ctx->file->good() ? total : 0;
    }
    return total;
}

int downloadProgressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* ctx = static_cast<DownloadContext*>(userdata);
    if (ctx->progress && dltotal > 0) {
        float pct = static_cast<float>(dlnow) / static_cast<float>(dltotal);
        ctx->progress->store(pct);
    }
    return 0;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════
// SelfUpdateManager implementation
// ═══════════════════════════════════════════════════════════════════

SelfUpdateManager& SelfUpdateManager::instance() {
    static SelfUpdateManager inst;
    return inst;
}

SelfUpdateManager::SelfUpdateManager() {}

SelfUpdateManager::~SelfUpdateManager() {
    stopAutoCheck();
}

void SelfUpdateManager::setState(UpdateState new_state, const std::string& message) {
    state_.store(new_state);
    {
        std::lock_guard<std::mutex> lock(message_mutex_);
        status_message_ = message;
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    if (state_callback_) state_callback_(new_state);
}

std::string SelfUpdateManager::statusMessage() const {
    std::lock_guard<std::mutex> lock(message_mutex_);
    return status_message_;
}

void SelfUpdateManager::setStateCallback(std::function<void(UpdateState)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = callback;
}

std::string SelfUpdateManager::currentVersion() {
    return constants::HUNTER_VERSION;
}

std::string SelfUpdateManager::currentExecutablePath() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) > 0) {
        return std::string(path);
    }
    return "";
#elif defined(__ANDROID__)
    // On Android, the "executable" is the APK; we return the APK path
    // This is typically /data/app/.../base.apk — but we'll use a known download dir
    return "";
#else
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0) {
        path[len] = '\0';
        return std::string(path);
    }
    // Fallback: use argv[0] equivalent
    return "";
#endif
}

int SelfUpdateManager::compareVersions(const std::string& a, const std::string& b) {
    // Parse semver: major.minor.patch (strip leading 'v' if present)
    auto parse = [](const std::string& s) -> std::vector<int> {
        std::string clean = s;
        if (!clean.empty() && clean[0] == 'v') clean = clean.substr(1);

        std::vector<int> parts;
        std::string num;
        for (char c : clean) {
            if (c >= '0' && c <= '9') {
                num += c;
            } else if (c == '.') {
                if (!num.empty()) {
                    try { parts.push_back(std::stoi(num)); } catch (...) {}
                    num.clear();
                }
            } else {
                // Non-numeric character (e.g., '-' for pre-release) — stop
                break;
            }
        }
        if (!num.empty()) {
            try { parts.push_back(std::stoi(num)); } catch (...) {}
        }
        return parts;
    };

    auto va = parse(a);
    auto vb = parse(b);

    // Compare component by component
    size_t max_len = std::max(va.size(), vb.size());
    for (size_t i = 0; i < max_len; i++) {
        int na = i < va.size() ? va[i] : 0;
        int nb = i < vb.size() ? vb[i] : 0;
        if (na < nb) return -1;
        if (na > nb) return 1;
    }
    return 0;
}

void SelfUpdateManager::startAutoCheck(int check_interval_seconds, int startup_delay_seconds) {
    if (check_thread_.joinable()) return;  // Already running

    stop_flag_.store(false);
    check_thread_ = std::thread([this, check_interval_seconds, startup_delay_seconds]() {
        autoCheckLoop(check_interval_seconds, startup_delay_seconds);
    });
}

void SelfUpdateManager::stopAutoCheck() {
    stop_flag_.store(true);
    if (check_thread_.joinable()) {
        check_thread_.join();
    }
}

void SelfUpdateManager::autoCheckLoop(int interval, int startup_delay) {
    // Initial delay before first check
    for (int i = 0; i < startup_delay && !stop_flag_.load(); i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (stop_flag_.load()) return;

    while (!stop_flag_.load()) {
        if (state_.load() == UpdateState::Idle || state_.load() == UpdateState::UpToDate) {
            if (checkForUpdate()) {
                // Update available — automatically download and apply
                setState(UpdateState::UpdateAvailable,
                         "Update " + latest_release_.tag_name + " available, downloading...");
                downloadAndApply();
            }
        }

        // Wait for next check interval
        for (int i = 0; i < interval && !stop_flag_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

bool SelfUpdateManager::checkForUpdate() {
    setState(UpdateState::Checking, "Checking for updates...");

    ReleaseInfo info;
    if (!fetchLatestRelease(info)) {
        setState(UpdateState::Failed, "Failed to check for updates (network error)");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(release_mutex_);
        latest_release_ = info;
    }

    std::string current = currentVersion();
    int cmp = compareVersions(info.tag_name, current);

    if (cmp <= 0) {
        setState(UpdateState::UpToDate, "Hunter is up to date (v" + current + ")");
        return false;
    }

    // Find platform asset
    std::string asset_url = findPlatformAsset(info);
    if (asset_url.empty()) {
        setState(UpdateState::Failed, "Update available (v" + info.tag_name +
                 ") but no matching asset for this platform");
        return false;
    }

    setState(UpdateState::UpdateAvailable, "Update available: v" + info.tag_name +
             " (current: v" + current + ")");
    return true;
}

bool SelfUpdateManager::fetchLatestRelease(ReleaseInfo& out) {
    network::HttpClient http;
    // Use GitHub API to get latest release
    std::string url = "https://api.github.com/repos/bahmany/censorship_hunter/releases/latest";

    // GitHub API requires a User-Agent header
    std::string response = http.get(url, 15000);

    if (response.empty()) {
        // Try without proxy (direct)
        response = http.get(url, 15000, "");
        if (response.empty()) return false;
    }

    // Parse JSON response
    out.tag_name = jsonExtractString(response, "tag_name");
    out.release_name = jsonExtractString(response, "name");
    out.release_url = jsonExtractString(response, "html_url");
    out.body = jsonExtractString(response, "body");
    out.assets = extractAssets(response);

    if (out.tag_name.empty()) return false;

    return true;
}

std::string SelfUpdateManager::findPlatformAsset(const ReleaseInfo& info) const {
    // Platform-specific asset naming convention:
    //   Windows: hunter-windows-x64.exe  or  huntercensor-Setup-v*.exe
    //   Linux:   hunter-linux-x64
    //   Android: hunter-android-arm64.apk  or  hunter-android-armv7.apk

#if defined(_WIN32)
    for (const auto& asset : info.assets) {
        if (asset.name.find("windows") != std::string::npos &&
            asset.name.find(".exe") != std::string::npos) {
            return asset.browser_download_url;
        }
    }
    // Fallback: any .exe (legacy naming)
    for (const auto& asset : info.assets) {
        if (asset.name.find(".exe") != std::string::npos) {
            return asset.browser_download_url;
        }
    }
#elif defined(__ANDROID__)
    for (const auto& asset : info.assets) {
        if (asset.name.find("android") != std::string::npos &&
            asset.name.find(".apk") != std::string::npos) {
            return asset.browser_download_url;
        }
    }
#else
    for (const auto& asset : info.assets) {
        if (asset.name.find("linux") != std::string::npos &&
            asset.name.find(".exe") == std::string::npos) {
            return asset.browser_download_url;
        }
    }
    // Fallback: any non-exe, non-apk asset
    for (const auto& asset : info.assets) {
        if (asset.name.find(".exe") == std::string::npos &&
            asset.name.find(".apk") == std::string::npos &&
            asset.name.find(".dmg") == std::string::npos) {
            return asset.browser_download_url;
        }
    }
#endif

    return "";
}

bool SelfUpdateManager::downloadFile(const std::string& url, const std::string& dest_path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    std::ofstream file(dest_path, std::ios::binary);
    if (!file.is_open()) {
        curl_easy_cleanup(curl);
        return false;
    }

    DownloadContext ctx;
    ctx.file = &file;
    ctx.progress = &download_progress_;

    download_progress_.store(0.0f);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, downloadWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, downloadProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // 5 min timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Hunter-Updater/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    file.close();

    if (res != CURLE_OK) {
        std::remove(dest_path.c_str());
        return false;
    }

    return true;
}

bool SelfUpdateManager::downloadAndApply() {
    ReleaseInfo info;
    {
        std::lock_guard<std::mutex> lock(release_mutex_);
        info = latest_release_;
    }

    if (info.tag_name.empty()) {
        setState(UpdateState::Failed, "No release info available");
        return false;
    }

    std::string asset_url = findPlatformAsset(info);
    if (asset_url.empty()) {
        setState(UpdateState::Failed, "No matching asset for this platform");
        return false;
    }

    setState(UpdateState::Downloading, "Downloading update v" + info.tag_name + "...");

    // Determine download path
    std::string download_dir = "runtime/";
#ifdef _WIN32
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH) > 0) {
        download_dir = std::filesystem::path(exe_path).parent_path().string() + "\\";
    }
#else
    // Use /tmp or runtime/ directory
    std::string exe = currentExecutablePath();
    if (!exe.empty()) {
        size_t slash = exe.find_last_of('/');
        if (slash != std::string::npos) {
            download_dir = exe.substr(0, slash + 1);
        }
    }
#endif

    // Extract filename from URL
    std::string filename;
    size_t slash_pos = asset_url.find_last_of('/');
    if (slash_pos != std::string::npos) {
        filename = asset_url.substr(slash_pos + 1);
    } else {
#ifdef _WIN32
        filename = "hunter_update.exe";
#elif defined(__ANDROID__)
        filename = "hunter_update.apk";
#else
        filename = "hunter_update";
#endif
    }

    std::string download_path = download_dir + "hunter_update_" + info.tag_name + "_" + filename;

    if (!downloadFile(asset_url, download_path)) {
        setState(UpdateState::Failed, "Failed to download update");
        return false;
    }

    setState(UpdateState::DownloadComplete, "Download complete, applying update...");
    downloaded_file_path_ = download_path;

    return applyUpdate(download_path);
}

bool SelfUpdateManager::applyUpdate(const std::string& downloaded_path) {
    setState(UpdateState::Applying, "Applying update...");

    bool success = false;
#ifdef _WIN32
    success = replaceBinaryWindows(downloaded_path);
#elif defined(__ANDROID__)
    success = replaceBinaryAndroid(downloaded_path);
#else
    success = replaceBinaryLinux(downloaded_path);
#endif

    if (!success) {
        setState(UpdateState::Failed, "Failed to apply update");
        return false;
    }

    setState(UpdateState::UpdateComplete, "Update applied, relaunching...");

    // Relaunch
#ifdef _WIN32
    relaunchWindows();
#elif defined(__ANDROID__)
    relaunchAndroid();
#else
    relaunchLinux();
#endif

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Platform-specific binary replacement
// ═══════════════════════════════════════════════════════════════════

bool SelfUpdateManager::replaceBinaryWindows(const std::string& new_binary_path) {
#ifdef _WIN32
    std::string exe_path = currentExecutablePath();
    if (exe_path.empty()) return false;

    // Rename current exe to .old, then move new exe to original name
    std::string old_path = exe_path + ".old";

    // Delete any existing .old file
    std::remove(old_path.c_str());

    // Rename current executable
    if (!MoveFileA(exe_path.c_str(), old_path.c_str())) {
        // If rename fails, try to delete and copy
        DeleteFileA(exe_path.c_str());
    }

    // Copy new binary to the exe path
    if (!CopyFileA(new_binary_path.c_str(), exe_path.c_str(), FALSE)) {
        // Try to restore old binary
        MoveFileA(old_path.c_str(), exe_path.c_str());
        return false;
    }

    // Clean up
    std::remove(new_binary_path.c_str());
    // .old will be cleaned up on next launch

    return true;
#else
    (void)new_binary_path;
    return false;  // Not Windows
#endif
}

bool SelfUpdateManager::replaceBinaryLinux(const std::string& new_binary_path) {
#ifndef _WIN32
    std::string exe_path = currentExecutablePath();
    if (exe_path.empty()) return false;

    // Make new binary executable
    chmod(new_binary_path.c_str(), 0755);

    // Rename current binary to .old
    std::string old_path = exe_path + ".old";
    std::remove(old_path.c_str());

    // Try rename first (works if same filesystem)
    if (rename(exe_path.c_str(), old_path.c_str()) != 0) {
        // If rename fails, try unlink + copy
        unlink(exe_path.c_str());
    }

    // Move new binary to exe path
    if (rename(new_binary_path.c_str(), exe_path.c_str()) != 0) {
        // Fallback: copy
        std::ifstream src(new_binary_path, std::ios::binary);
        std::ofstream dst(exe_path, std::ios::binary);
        if (!src.is_open() || !dst.is_open()) {
            // Try to restore
            rename(old_path.c_str(), exe_path.c_str());
            return false;
        }
        dst << src.rdbuf();
        src.close();
        dst.close();
        chmod(exe_path.c_str(), 0755);
    }

    // Clean up
    std::remove(new_binary_path.c_str());
    std::remove(old_path.c_str());

    return true;
#else
    (void)new_binary_path;
    return false;  // Not Linux
#endif
}

bool SelfUpdateManager::replaceBinaryAndroid(const std::string& new_apk_path) {
    // On Android, we can't replace the running APK directly.
    // The APK is downloaded and we trigger an install Intent.
    // The actual install happens via the system package installer.
    // This is handled in relaunchAndroid() which sends the Intent.
    // Here we just verify the file exists.
    std::ifstream test(new_apk_path, std::ios::binary);
    if (!test.is_open()) return false;
    test.close();
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Platform-specific relaunch
// ═══════════════════════════════════════════════════════════════════

bool SelfUpdateManager::relaunchWindows() {
#ifdef _WIN32
    std::string exe_path = currentExecutablePath();
    if (exe_path.empty()) return false;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (CreateProcessA(exe_path.c_str(), nullptr, nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        // Exit current process — the new one takes over
        ExitProcess(0);
    }
    return false;
#else
    return false;  // Not Windows
#endif
}

bool SelfUpdateManager::relaunchLinux() {
#ifndef _WIN32
    std::string exe_path = currentExecutablePath();
    if (exe_path.empty()) return false;

    // Fork and exec the new binary, then exit current process
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec the new binary
        execl(exe_path.c_str(), exe_path.c_str(), (char*)nullptr);
        _exit(127);  // exec failed
    } else if (pid > 0) {
        // Parent: exit immediately, child takes over
        _exit(0);
    }
    return false;
#else
    return false;  // Not Linux
#endif
}

bool SelfUpdateManager::relaunchAndroid() {
#ifdef __ANDROID__
    // On Android, we need to trigger an APK install Intent.
    // This requires JNI access to the Activity/Context.
    // The APK file is at downloaded_file_path_.
    // We use a file:// URI with the system package installer.

    // Note: This requires the app to have a JNI handle to the Activity.
    // In a full implementation, this would call:
    //   Intent intent = new Intent(Intent.ACTION_VIEW);
    //   intent.setDataAndType(uri, "application/vnd.android.package-archive");
    //   intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    //   startActivity(intent);

    // For now, we log that the APK is ready for manual install.
    // The UI will show a notification with the file path.
    utils::LogRingBuffer::instance().push(
        "[Updater] APK downloaded to " + downloaded_file_path_ +
        " — please install it manually or via the system installer.");
    return true;
#else
    return false;
#endif
}

ReleaseInfo SelfUpdateManager::latestRelease() const {
    std::lock_guard<std::mutex> lock(release_mutex_);
    return latest_release_;
}

} // namespace core
} // namespace hunter
