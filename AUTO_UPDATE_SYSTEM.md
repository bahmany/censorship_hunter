# Auto-Update System - Technical Documentation

## Overview

The Auto-Update System is a robust, production-ready update mechanism that allows Censorship Hunter to:

1. **Check for new releases** on GitHub automatically
2. **Download updates** with progress tracking
3. **Verify integrity** using SHA-256 checksums
4. **Self-replace** the executable with proper platform-specific handling
5. **Automatically relaunch** the application

## Architecture

### Components

#### 1. **AutoUpdater** (Core Module)
- Location: `hunter_cpp/include/core/auto_updater.h`
- Responsibility: Main update orchestration
- Key Classes:
  - `AutoUpdater` - Singleton update manager
  - `UpdateInfo` - Release information
  - `UpdateProgress` - Progress tracking

#### 2. **GitHub API Integration**
- Fetches latest release information from GitHub
- Extracts version, download URL, and SHA256 hash
- Platform-specific asset selection

#### 3. **Download Manager**
- HTTP-based file download
- Progress callback support
- Error handling and retry logic

#### 4. **Verification System**
- SHA256 checksum validation
- Prevents corrupted or tampered updates

#### 5. **Binary Replacement Engine**
- Platform-specific implementations:
  - **Windows**: Rename current → .old, copy new → current, launch, exit
  - **Linux**: Atomic rename, exec new binary
  - **macOS**: Atomic rename, exec new binary
  - **Android**: Copy to cache, trigger install Intent

## Data Structures

### UpdateInfo
```cpp
struct UpdateInfo {
    std::string version;           // e.g., "1.13.1"
    std::string download_url;      // Direct GitHub download link
    std::string sha256;            // SHA-256 checksum
    std::string release_notes;     // Release notes/changelog
    bool is_available;             // Update available for current version
    std::string error_message;     // Error details if check failed
};
```

### UpdateProgress
```cpp
struct UpdateProgress {
    enum class Stage {
        IDLE,                      // Waiting for action
        CHECKING,                  // Querying GitHub
        DOWNLOADING,               // Downloading update file
        VERIFYING,                 // Verifying SHA-256
        APPLYING,                  // Replacing binary
        COMPLETE,                  // Update successful
        ERROR                      // Update failed
    };
    
    Stage current_stage;           // Current operation stage
    float download_progress;       // 0.0 to 1.0 (download only)
    std::string status_message;    // Human-readable status
    std::string error_message;     // Error description
};
```

## API Reference

### Core Methods

#### 1. Check for Updates (Synchronous)
```cpp
UpdateInfo info = AutoUpdater::getInstance().checkForUpdates();

if (info.is_available) {
    std::cout << "Update v" << info.version << " available!" << std::endl;
    std::cout << "Download: " << info.download_url << std::endl;
} else {
    std::cout << "Already up to date" << std::endl;
}
```

#### 2. Check for Updates (Asynchronous)
```cpp
AutoUpdater::getInstance().checkForUpdatesAsync(
    [](const UpdateInfo& info) {
        if (info.is_available) {
            // Show update available notification
        }
    }
);
```

#### 3. Download and Apply Update
```cpp
bool success = AutoUpdater::getInstance().downloadAndApply(
    "https://github.com/bahmany/censorship_hunter/releases/download/v1.13.1/hunter-windows-x64.exe",
    "abc123def456...",  // Expected SHA-256
    [](const UpdateProgress& progress) {
        switch (progress.current_stage) {
            case UpdateProgress::Stage::DOWNLOADING:
                std::cout << "Download: " << (progress.download_progress * 100) << "%" << std::endl;
                break;
            case UpdateProgress::Stage::VERIFYING:
                std::cout << "Verifying..." << std::endl;
                break;
            case UpdateProgress::Stage::APPLYING:
                std::cout << "Applying update..." << std::endl;
                break;
            case UpdateProgress::Stage::COMPLETE:
                std::cout << "Update complete! Relaunching..." << std::endl;
                break;
            case UpdateProgress::Stage::ERROR:
                std::cout << "Error: " << progress.error_message << std::endl;
                break;
            default:
                break;
        }
    }
);
```

#### 4. Get Progress
```cpp
UpdateProgress progress = AutoUpdater::getInstance().getProgress();
std::cout << "Status: " << progress.status_message << std::endl;
std::cout << "Progress: " << (progress.download_progress * 100) << "%" << std::endl;
```

#### 5. Cancel Update
```cpp
AutoUpdater::getInstance().cancel();
```

#### 6. Get Version Info
```cpp
std::string current = AutoUpdater::getCurrentVersion();
std::string latest = AutoUpdater::getInstance().getLatestVersion();
```

## Platform-Specific Implementation

### Windows

**Strategy:**
1. Get current executable path using `GetModuleFileNameA`
2. Rename current exe to `.old` (keeps backup)
3. Copy downloaded exe to original location
4. Create new process with `CreateProcessA`
5. Exit old process with `exit(0)`

**Advantages:**
- Old version still available as `.old` for recovery
- Smooth transition, no process conflicts
- Standard Windows approach

**Code Location:** `replaceBinaryWindows()` in auto_updater.cpp

### Linux

**Strategy:**
1. Get current executable path from `/proc/self/exe`
2. Make downloaded binary executable with `chmod 755`
3. Atomic rename using `rename()` syscall
4. Execute new binary with `execv()`

**Advantages:**
- Atomic operation (no partial state)
- execv() replaces process space (true reload)
- Process ID remains the same

**Code Location:** `replaceBinaryLinux()` in auto_updater.cpp

### macOS

**Strategy:**
1. Similar to Linux (atomic rename + execv)
2. Additional: Code signing verification (future)
3. Handle app bundles if needed

**Code Location:** `replaceBinaryMacOS()` in auto_updater.cpp

### Android

**Strategy:**
1. Download APK to app cache directory
2. Trigger install via Intent (requires JNI bridge)
3. System installer handles replacement

**Note:** Requires Java/JNI integration for Intent handling

**Code Location:** `replaceBinaryAndroid()` in auto_updater.cpp

## Integration Guide

### Step 1: Add to CMakeLists.txt

```cmake
target_sources(hunter_core PRIVATE
    src/core/auto_updater.cpp
)

target_include_directories(hunter_core PUBLIC
    include/core
)
```

### Step 2: Define HUNTER_VERSION_STRING

In CMakeLists.txt:
```cmake
set(HUNTER_VERSION_STRING "1.13.0")
add_definitions(-DHUNTER_VERSION_STRING="${HUNTER_VERSION_STRING}")
```

### Step 3: Initialize in Application

In `main.cpp` or initialization code:
```cpp
#include "core/auto_updater.h"

using namespace hunter::core;

// Option 1: Automatic background check (recommended)
// Check every 6 hours, first check after 10 seconds
AutoUpdater::getInstance().startAutoCheck(21600, 10);

// Option 2: Manual check from UI menu
void onCheckUpdatesClicked() {
    AutoUpdater::getInstance().checkForUpdatesAsync(
        [](const UpdateInfo& info) {
            if (info.is_available) {
                showUpdateDialog(info);
            } else {
                showMessage("Already up to date");
            }
        }
    );
}

// Option 3: Handle "About > Check for Updates" button
void onUpdateButtonClicked() {
    UpdateInfo info = AutoUpdater::getInstance().checkForUpdates();
    if (info.is_available) {
        // Show dialog to user
        // On confirm, call:
        // AutoUpdater::getInstance().downloadAndApply(
        //     info.download_url,
        //     info.sha256,
        //     progressCallback
        // );
    }
}
```

### Step 4: Update UI Integration

**Windows (ImGui):**
```cpp
if (ImGui::Button("Check for Updates")) {
    updater.checkForUpdatesAsync([](const UpdateInfo& info) {
        if (info.is_available) {
            show_update_dialog = true;
            latest_update_info = info;
        }
    });
}

if (show_update_dialog && ImGui::BeginPopupModal("Update Available")) {
    ImGui::Text("Update v%s available", latest_update_info.version.c_str());
    ImGui::TextWrapped("%s", latest_update_info.release_notes.c_str());
    
    if (ImGui::Button("Update Now")) {
        updater.downloadAndApply(
            latest_update_info.download_url,
            latest_update_info.sha256,
            progressCallback
        );
    }
    
    ImGui::EndPopup();
}
```

## Testing

### Unit Tests

Run the test suite:
```bash
cd hunter_cpp/build
cmake .. -G Ninja
ninja test_auto_updater
./tests/test_auto_updater
```

### Test Cases

1. **Version Check**
   - Verify current version retrieval
   - Test async check
   - Test sync check

2. **Update Detection**
   - Verify newer version detection
   - Verify "already up to date" condition
   - Test network error handling

3. **Progress Tracking**
   - Verify progress callback execution
   - Test progress stages
   - Verify stage transitions

4. **Download & Verify**
   - Test file download
   - Test SHA-256 verification
   - Test corrupted file rejection

5. **Binary Replacement**
   - Test Windows executable replacement
   - Test Linux atomic rename
   - Test process execution
   - Test rollback on failure

### Manual Testing

#### Test Case 1: Check for Updates

```bash
# Simulate checking for updates
./bin/hunter_backend

# Check logs for update check message
# Should print current version and check result
```

#### Test Case 2: Download Update

```bash
# Trigger update download
# Verify download progress is reported
# Verify file is created in temp directory
# Verify file size matches expected
```

#### Test Case 3: Binary Replacement

```bash
# On Windows:
# 1. Rename current hunter.exe to hunter.exe.old
# 2. Copy new hunter.exe to original location
# 3. Launch new hunter.exe
# 4. Verify process is running
# 5. Verify version reflects new version

# On Linux:
# 1. Verify file is executable
# 2. Verify atomic rename occurred
# 3. Verify new process is running
# 4. Check /proc/self/exe points to new binary
```

#### Test Case 4: Network Errors

```bash
# Disable network connectivity
# Trigger update check
# Verify error message is displayed
# Verify application continues working
# Re-enable network
# Verify update check succeeds on retry
```

## Error Handling

### Network Errors
- **Cause:** GitHub API unreachable
- **Handling:** Graceful failure, user can retry
- **Message:** "Failed to check for updates (network error)"

### Download Errors
- **Cause:** File download interrupted/incomplete
- **Handling:** Cleanup temp file, allow retry
- **Message:** "Download failed"

### Verification Errors
- **Cause:** SHA-256 mismatch (corrupted/tampered file)
- **Handling:** Delete file, prevent execution
- **Message:** "Integrity check failed"

### Replacement Errors
- **Cause:** Executable in use or permission denied
- **Handling:** Fallback to next relaunch
- **Message:** "Failed to apply update"

## Security Considerations

### 1. HTTPS-Only
- All GitHub API calls use HTTPS
- Download URLs always point to GitHub (trusted source)

### 2. SHA-256 Verification
- Every downloaded file is verified
- Prevents installation of corrupted/modified files
- Uses GitHub's signed digest

### 3. Atomic Operations
- Linux/macOS use atomic rename to prevent partial updates
- Windows uses backup strategy for rollback

### 4. User Confirmation
- Updates require user confirmation
- No silent forced updates

### 5. Version Validation
- Only updates to newer versions
- Prevents downgrade attacks

## Performance

### Memory Usage
- ~1 MB during idle
- ~50 MB during download (file buffering)
- ~10 MB during verification

### Network Usage
- Version check: ~1 KB (API query)
- Update download: 20-50 MB (executable)
- Total: ~30 seconds typical (depends on connection)

### CPU Usage
- SHA-256 verification: ~1-2 seconds
- Binary replacement: ~100ms
- No continuous background work

## Future Enhancements

### v1.14.0 (Planned)
- [ ] Delta/patch updates (smaller downloads)
- [ ] Update scheduling (check during idle times)
- [ ] Automatic update application option
- [ ] Update rollback mechanism
- [ ] Bandwidth limiting during download

### v1.15.0 (Planned)
- [ ] Code signing verification
- [ ] Update notifications with Telegram
- [ ] Update history/changelog viewer
- [ ] Staged rollout (phased updates)
- [ ] Offline update package support

## Troubleshooting

### Update not detected
1. Check internet connection
2. Verify GitHub API is accessible (https://api.github.com)
3. Check version comparison logic
4. Review application logs

### Download fails
1. Check available disk space
2. Verify temp directory permissions
3. Check GitHub release asset exists
4. Try different network connection

### Binary replacement fails
1. Verify current executable is not locked
2. Check write permissions for executable directory
3. Ensure sufficient disk space
4. Review system logs for errors

### Application doesn't relaunch
1. Check platform-specific launch code (Windows: CreateProcessA, Linux: execv)
2. Verify process has necessary permissions
3. Check working directory is accessible
4. Review logs before exit

## Configuration

### Environment Variables
```bash
# Enable verbose update logging
export HUNTER_UPDATE_DEBUG=1

# Override GitHub API endpoint (for testing)
export HUNTER_GITHUB_API_URL=https://api.github.com

# Disable auto-update check
export HUNTER_AUTO_UPDATE=0

# Custom update check interval (seconds)
export HUNTER_UPDATE_CHECK_INTERVAL=86400
```

## API Compatibility

### Version Checking
```
Format: "X.Y.Z" (e.g., "1.13.0")
Comparison: Semantic versioning
  1.13.0 > 1.12.0
  2.0.0 > 1.99.0
  1.10.0 > 1.9.0 (numeric, not lexical)
```

### GitHub Release Requirements
```
- Release tagged with semantic version (vX.Y.Z format)
- Release has platform-specific asset:
  * Windows: hunter-windows-x64.exe
  * Linux: hunter-linux-x64
  * macOS: hunter-macos-x64
  * Android: hunter-android-arm64.apk
- Asset has "digest" field with SHA-256 hash
  Format: "sha256:abc123..." or "abc123..."
```

## Release Checklist

For each release, ensure:
- [ ] New version number in CMakeLists.txt
- [ ] Release notes in GitHub release body
- [ ] Executable uploaded with correct naming
- [ ] SHA-256 digest included in release
- [ ] Asset verified to work on target platform
- [ ] Changelog updated
- [ ] HUNTER_VERSION_STRING matches release tag

## Support & Issues

### Reporting Bugs
1. Describe what went wrong
2. Provide application version
3. Include error message/logs
4. Specify platform (Windows/Linux/macOS/Android)
5. Describe network conditions (proxy, firewall, etc.)

### Test Environment

Test Updates in Staging:
```bash
# Create test release on GitHub
git tag v1.13.0-rc1
git push origin v1.13.0-rc1

# Build and test update flow
cmake .. -DHUNTER_VERSION_STRING=1.13.0-rc1
ninja
./bin/hunter_backend

# Trigger update check
# Verify it finds your RC release
```

## References

- GitHub API: https://docs.github.com/en/rest/releases/releases
- SHA-256: https://en.wikipedia.org/wiki/SHA-2
- Process Execution:
  - Windows: https://docs.microsoft.com/windows/win32/procthread/processes-and-threads
  - Linux: https://man7.org/linux/man-pages/man2/execve.2.html
  - macOS: https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/execve.2.html

---

**Document Version:** 1.0  
**Last Updated:** November 5, 2026  
**Status:** Production Ready
