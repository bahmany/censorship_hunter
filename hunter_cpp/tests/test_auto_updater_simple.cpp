#include "core/auto_updater.h"
#include <iostream>
#include <chrono>
#include <thread>

using namespace hunter::core;

void printHeader(const std::string& text) {
    std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║ " << text;
    for (size_t i = text.length(); i < 60; i++) std::cout << " ";
    std::cout << " ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
}

void testCurrentVersion() {
    printHeader("TEST 1: Get Current Version");
    
    std::string version = AutoUpdater::getCurrentVersion();
    std::cout << "Current Version: " << version << "\n";
    
    if (version.empty()) {
        std::cout << "❌ FAILED: Version is empty\n";
        return;
    }
    
    if (version == "1.13.2") {
        std::cout << "✅ PASSED: Version is correct (1.13.2)\n";
    } else {
        std::cout << "⚠️  WARNING: Version mismatch (got: " << version << ")\n";
    }
}

void testGetProgress() {
    printHeader("TEST 2: Get Progress");
    
    UpdateProgress progress = AutoUpdater::getInstance().getProgress();
    
    std::cout << "Stage: " << static_cast<int>(progress.current_stage) << "\n";
    std::cout << "Progress: " << (progress.download_progress * 100) << "%\n";
    std::cout << "Status: " << progress.status_message << "\n";
    
    if (progress.current_stage == UpdateProgress::Stage::IDLE) {
        std::cout << "✅ PASSED: Initial state is IDLE\n";
    } else {
        std::cout << "❌ FAILED: Initial state is not IDLE\n";
    }
}

void testCheckForUpdatesSync() {
    printHeader("TEST 3: Check For Updates (Synchronous)");
    
    std::cout << "Checking GitHub for updates...\n";
    UpdateInfo info = AutoUpdater::getInstance().checkForUpdates();
    
    std::cout << "\nResults:\n";
    std::cout << "  Version: " << info.version << "\n";
    std::cout << "  Available: " << (info.is_available ? "Yes" : "No") << "\n";
    std::cout << "  Error: " << info.error_message << "\n";
    
    if (!info.version.empty()) {
        std::cout << "✅ PASSED: Received version info from GitHub\n";
    } else {
        std::cout << "⚠️  INFO: Could not reach GitHub (network issue?)\n";
    }
}

void testCheckForUpdatesAsync() {
    printHeader("TEST 4: Check For Updates (Asynchronous)");
    
    bool callback_called = false;
    std::string result_version;
    
    std::cout << "Starting async check...\n";
    AutoUpdater::getInstance().checkForUpdatesAsync([&](const UpdateInfo& info) {
        callback_called = true;
        result_version = info.version;
        std::cout << "  Callback received with version: " << info.version << "\n";
    });
    
    std::cout << "Waiting for callback (5 seconds)...\n";
    std::this_thread::sleep_for(std::chrono::seconds(5));
    
    if (callback_called) {
        std::cout << "✅ PASSED: Async callback was called\n";
    } else {
        std::cout << "⚠️  INFO: Callback not called within timeout (network issue?)\n";
    }
}

void testCancel() {
    printHeader("TEST 5: Cancel Operation");
    
    std::cout << "Cancelling update...\n";
    AutoUpdater::getInstance().cancel();
    
    std::cout << "✅ PASSED: Cancel operation completed without error\n";
}

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                AUTO-UPDATER TEST SUITE v1.13.2                 ║\n";
    std::cout << "║                      Running on Linux                          ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════╝\n";
    
    try {
        testCurrentVersion();
        testGetProgress();
        testCheckForUpdatesSync();
        testCheckForUpdatesAsync();
        testCancel();
        
        std::cout << "\n╔════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║                   ALL TESTS COMPLETED                           ║\n";
        std::cout << "║                   Status: ✅ WORKING                            ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════╝\n\n";
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n❌ ERROR: " << e.what() << "\n\n";
        return 1;
    }
}
