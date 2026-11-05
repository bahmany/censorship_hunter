#include <gtest/gtest.h>
#include "network/service_accessibility.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @brief Tests for ServiceAccessibilityChecker
 */
class ServiceAccessibilityCheckerTest : public ::testing::Test {
protected:
    ServiceAccessibilityChecker checker_;
};

TEST_F(ServiceAccessibilityCheckerTest, ServiceNameConversion) {
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceName(ServiceAccessibilityChecker::Service::GEMINI), "gemini");
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceName(ServiceAccessibilityChecker::Service::CLAUDE), "claude");
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceName(ServiceAccessibilityChecker::Service::DEVIN), "devin");
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceName(ServiceAccessibilityChecker::Service::GOOGLE_DEVELOPER), "google_developer");
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceName(ServiceAccessibilityChecker::Service::AWS_CONSOLE), "aws_console");
}

TEST_F(ServiceAccessibilityCheckerTest, ServiceFromName) {
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceFromName("gemini"), ServiceAccessibilityChecker::Service::GEMINI);
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceFromName("claude"), ServiceAccessibilityChecker::Service::CLAUDE);
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceFromName("devin"), ServiceAccessibilityChecker::Service::DEVIN);
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceFromName("google_developer"), ServiceAccessibilityChecker::Service::GOOGLE_DEVELOPER);
    EXPECT_EQ(ServiceAccessibilityChecker::getServiceFromName("aws_console"), ServiceAccessibilityChecker::Service::AWS_CONSOLE);
}

TEST_F(ServiceAccessibilityCheckerTest, StatusConversion) {
    EXPECT_EQ(ServiceAccessibilityChecker::statusToString(ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE), "accessible");
    EXPECT_EQ(ServiceAccessibilityChecker::statusToString(ServiceAccessibilityChecker::AccessibilityStatus::BLOCKED), "blocked");
    EXPECT_EQ(ServiceAccessibilityChecker::statusToString(ServiceAccessibilityChecker::AccessibilityStatus::LOCATION_FORBIDDEN), "location_forbidden");
    EXPECT_EQ(ServiceAccessibilityChecker::statusToString(ServiceAccessibilityChecker::AccessibilityStatus::UNKNOWN), "unknown");
}

TEST_F(ServiceAccessibilityCheckerTest, StringToStatus) {
    EXPECT_EQ(ServiceAccessibilityChecker::stringToStatus("accessible"), ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE);
    EXPECT_EQ(ServiceAccessibilityChecker::stringToStatus("blocked"), ServiceAccessibilityChecker::AccessibilityStatus::BLOCKED);
    EXPECT_EQ(ServiceAccessibilityChecker::stringToStatus("location_forbidden"), ServiceAccessibilityChecker::AccessibilityStatus::LOCATION_FORBIDDEN);
    EXPECT_EQ(ServiceAccessibilityChecker::stringToStatus("unknown"), ServiceAccessibilityChecker::AccessibilityStatus::UNKNOWN);
}

/**
 * @brief Tests for ServiceAccessibilityDatabase
 */
class ServiceAccessibilityDatabaseTest : public ::testing::Test {
protected:
    ServiceAccessibilityDatabase db_;
};

TEST_F(ServiceAccessibilityDatabaseTest, UpdateServiceStatus) {
    std::string uri = "vless://test@example.com:443";
    
    db_.updateServiceStatus(
        uri,
        ServiceAccessibilityChecker::Service::GEMINI,
        ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE,
        150.0f
    );

    auto status = db_.getServiceStatus(uri, ServiceAccessibilityChecker::Service::GEMINI);
    EXPECT_EQ(status, ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE);
}

TEST_F(ServiceAccessibilityDatabaseTest, GetStats) {
    auto stats = db_.getStats();
    
    EXPECT_GE(stats.total_configs, 0);
    EXPECT_GE(stats.accessible_any, 0);
    EXPECT_GE(stats.location_forbidden, 0);
    EXPECT_GE(stats.all_blocked, 0);
    EXPECT_GE(stats.avg_latency, 0.0f);
}

TEST_F(ServiceAccessibilityDatabaseTest, GetAccessibleFor) {
    std::vector<ServiceAccessibilityChecker::Service> services = {
        ServiceAccessibilityChecker::Service::GEMINI
    };
    
    auto accessible = db_.getAccessibleFor(services);
    // Should return list of URIs accessible to Gemini
    EXPECT_GE(accessible.size(), 0);
}

/**
 * @brief Tests for AccessibilityAPI
 */
class AccessibilityAPITest : public ::testing::Test {
protected:
    // AccessibilityAPI api_;  // TODO: Create API instance
};

// TODO: Add API command handler tests

/**
 * @brief Integration tests
 */
class AccessibilityIntegrationTest : public ::testing::Test {
protected:
    ServiceAccessibilityChecker checker_;
    ServiceAccessibilityDatabase db_;
};

TEST_F(AccessibilityIntegrationTest, BasicWorkflow) {
    // 1. Create a mock config
    std::string uri = "vless://config@example.com:443";
    
    // 2. Update service status
    db_.updateServiceStatus(
        uri,
        ServiceAccessibilityChecker::Service::GEMINI,
        ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE,
        200.0f
    );
    
    db_.updateServiceStatus(
        uri,
        ServiceAccessibilityChecker::Service::CLAUDE,
        ServiceAccessibilityChecker::AccessibilityStatus::LOCATION_FORBIDDEN,
        500.0f
    );
    
    // 3. Verify statuses
    auto gemini_status = db_.getServiceStatus(uri, ServiceAccessibilityChecker::Service::GEMINI);
    auto claude_status = db_.getServiceStatus(uri, ServiceAccessibilityChecker::Service::CLAUDE);
    
    EXPECT_EQ(gemini_status, ServiceAccessibilityChecker::AccessibilityStatus::ACCESSIBLE);
    EXPECT_EQ(claude_status, ServiceAccessibilityChecker::AccessibilityStatus::LOCATION_FORBIDDEN);
}

/**
 * @brief Data model tests
 */
class ConfigHealthRecordTest : public ::testing::Test {
};

// Note: ConfigHealthRecord tests require the models.h header

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
