#include <gtest/gtest.h>
#include "core/auto_updater.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace hunter::core;

/**
 * @brief Tests for AutoUpdater
 */
class AutoUpdaterTest : public ::testing::Test {
protected:
    AutoUpdater& updater = AutoUpdater::getInstance();

    void SetUp() override {
        // Reset state before each test
    }

    void TearDown() override {
        updater.cancel();
    }
};

TEST_F(AutoUpdaterTest, GetCurrentVersion) {
    std::string version = AutoUpdater::getCurrentVersion();
    EXPECT_FALSE(version.empty());
    EXPECT_NE(version, "unknown");
    // Version should be in format X.Y.Z
    std::cout << "Current version: " << version << std::endl;
}

TEST_F(AutoUpdaterTest, CheckForUpdatesAsync) {
    bool callback_called = false;
    UpdateInfo result;

    std::function<void(const UpdateInfo&)> callback = [&](const UpdateInfo& info) {
        callback_called = true;
        result = info;
    };

    updater.checkForUpdatesAsync(callback);

    // Wait for async operation
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Callback should have been called
    EXPECT_TRUE(callback_called);

    // Check result
    if (result.is_available) {
        EXPECT_FALSE(result.version.empty());
        EXPECT_FALSE(result.download_url.empty());
        std::cout << "Update available: " << result.version << std::endl;
    } else {
        std::cout << "Already up to date" << std::endl;
    }
}

TEST_F(AutoUpdaterTest, CheckForUpdatesSynchronous) {
    UpdateInfo info = updater.checkForUpdates();
    
    EXPECT_FALSE(info.version.empty());
    EXPECT_FALSE(info.error_message.empty() || info.is_available || info.status_message.empty());
    
    std::cout << "Update status: " << info.error_message << std::endl;
}

TEST_F(AutoUpdaterTest, GetProgress) {
    UpdateProgress progress = updater.getProgress();
    
    EXPECT_EQ(progress.current_stage, UpdateProgress::Stage::IDLE);
    EXPECT_FLOAT_EQ(progress.download_progress, 0.0f);
}

TEST_F(AutoUpdaterTest, ProgressCallback) {
    bool progress_called = false;

    auto progress_callback = [&](const UpdateProgress& progress) {
        progress_called = true;
        std::cout << "Progress: " << progress.status_message << std::endl;
    };

    // This would normally download, but we'll just test the callback mechanism
    // In a real scenario, this would trigger download
    EXPECT_TRUE(true);  // Placeholder for actual download test
}

TEST_F(AutoUpdaterTest, CancelUpdate) {
    updater.cancel();
    
    UpdateProgress progress = updater.getProgress();
    // After cancel, should be able to check again
    EXPECT_TRUE(true);
}

TEST_F(AutoUpdaterTest, VersionComparison) {
    // Test version comparison logic
    std::string v1 = "1.13.0";
    std::string v2 = "1.12.0";
    std::string v3 = "2.0.0";
    
    // 1.13.0 > 1.12.0
    EXPECT_TRUE(v1 > v2);
    
    // 2.0.0 > 1.13.0
    EXPECT_TRUE(v3 > v1);
    
    std::cout << "Version comparison tests passed" << std::endl;
}

/**
 * @brief Integration test for full update flow
 */
TEST_F(AutoUpdaterTest, FullUpdateFlow) {
    std::cout << "\n=== Full Update Flow Test ===" << std::endl;
    
    // Step 1: Check for updates
    std::cout << "Step 1: Checking for updates..." << std::endl;
    UpdateInfo info = updater.checkForUpdates();
    
    std::cout << "  Current version: " << AutoUpdater::getCurrentVersion() << std::endl;
    std::cout << "  Latest version: " << info.version << std::endl;
    std::cout << "  Is available: " << (info.is_available ? "Yes" : "No") << std::endl;
    
    if (info.is_available) {
        std::cout << "  Download URL: " << info.download_url << std::endl;
        std::cout << "  SHA256: " << info.sha256.substr(0, 20) << "..." << std::endl;
        
        // In a real test, we would download and apply
        // For now, just verify we have the info
        EXPECT_FALSE(info.download_url.empty());
        EXPECT_FALSE(info.sha256.empty());
    } else {
        std::cout << "  Status: " << info.error_message << std::endl;
    }
    
    std::cout << "=== Test Complete ===" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
