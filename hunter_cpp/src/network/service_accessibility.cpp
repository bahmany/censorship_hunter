#include "network/service_accessibility.h"
#include "core/utils.h"
#include "network/proxy_tester.h"
#include <chrono>
#include <sstream>
#include <algorithm>

ServiceAccessibilityChecker::ServiceTestResult ServiceAccessibilityChecker::testService(
    const std::string& proxy_uri,
    Service service,
    int timeout_seconds) {
    
    auto start = std::chrono::high_resolution_clock::now();
    
    std::string test_url = getServiceTestUrl(service);
    ServiceTestResult result = testHttpAccess(proxy_uri, test_url, timeout_seconds);
    
    auto end = std::chrono::high_resolution_clock::now();
    result.latency_ms = std::chrono::duration<float, std::milli>(end - start).count();
    result.service_name = getServiceName(service);
    result.timestamp = std::chrono::system_clock::now().time_since_epoch().count() / 1e9;
    
    return result;
}

std::vector<ServiceAccessibilityChecker::ServiceTestResult> ServiceAccessibilityChecker::testAllServices(
    const std::string& proxy_uri,
    int timeout_seconds) {
    
    std::vector<ServiceTestResult> results;
    results.push_back(testService(proxy_uri, Service::GEMINI, timeout_seconds));
    results.push_back(testService(proxy_uri, Service::CLAUDE, timeout_seconds));
    results.push_back(testService(proxy_uri, Service::DEVIN, timeout_seconds));
    results.push_back(testService(proxy_uri, Service::GOOGLE_DEVELOPER, timeout_seconds));
    results.push_back(testService(proxy_uri, Service::AWS_CONSOLE, timeout_seconds));
    
    return results;
}

std::string ServiceAccessibilityChecker::getServiceName(Service service) {
    switch (service) {
        case Service::GEMINI:
            return "gemini";
        case Service::CLAUDE:
            return "claude";
        case Service::DEVIN:
            return "devin";
        case Service::GOOGLE_DEVELOPER:
            return "google_developer";
        case Service::AWS_CONSOLE:
            return "aws_console";
        default:
            return "unknown";
    }
}

ServiceAccessibilityChecker::Service ServiceAccessibilityChecker::getServiceFromName(
    const std::string& name) {
    
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    
    if (lower_name == "gemini") return Service::GEMINI;
    if (lower_name == "claude") return Service::CLAUDE;
    if (lower_name == "devin") return Service::DEVIN;
    if (lower_name == "google_developer" || lower_name == "google developer" ||
        lower_name == "google_development" || lower_name == "google development" ||
        lower_name == "google_dev" || lower_name == "google") return Service::GOOGLE_DEVELOPER;
    if (lower_name == "aws_console" || lower_name == "aws console" || lower_name == "aws" ||
        lower_name == "amazon_web_services" || lower_name == "amazon web services") return Service::AWS_CONSOLE;
    
    throw std::invalid_argument("Unknown service: " + name);
}

std::string ServiceAccessibilityChecker::statusToString(AccessibilityStatus status) {
    switch (status) {
        case AccessibilityStatus::UNKNOWN:
            return "unknown";
        case AccessibilityStatus::BLOCKED:
            return "blocked";
        case AccessibilityStatus::ACCESSIBLE:
            return "accessible";
        case AccessibilityStatus::LOCATION_FORBIDDEN:
            return "location_forbidden";
        default:
            return "unknown";
    }
}

ServiceAccessibilityChecker::AccessibilityStatus ServiceAccessibilityChecker::stringToStatus(
    const std::string& status) {
    
    if (status == "accessible") return AccessibilityStatus::ACCESSIBLE;
    if (status == "blocked") return AccessibilityStatus::BLOCKED;
    if (status == "location_forbidden") return AccessibilityStatus::LOCATION_FORBIDDEN;
    return AccessibilityStatus::UNKNOWN;
}

std::string ServiceAccessibilityChecker::getServiceTestUrl(Service service) {
    static const std::map<Service, std::string> urls = {
        {Service::GEMINI, "https://generativelanguage.googleapis.com/v1beta/models"},
        {Service::CLAUDE, "https://claude.ai/api/auth/session"},
        {Service::DEVIN, "https://devin.ai/"},
        {Service::GOOGLE_DEVELOPER, "https://console.cloud.google.com"},
        {Service::AWS_CONSOLE, "https://console.aws.amazon.com/console/home"},
    };
    const auto it = urls.find(service);
    return it == urls.end() ? std::string{} : it->second;
}

ServiceAccessibilityChecker::ServiceTestResult ServiceAccessibilityChecker::testHttpAccess(
    const std::string& socks_proxy,
    const std::string& test_url,
    int timeout_seconds) {
    
    ServiceTestResult result;
    result.status = AccessibilityStatus::UNKNOWN;
    result.latency_ms = 0.0f;
    result.error_message = "";
    result.timestamp = std::chrono::system_clock::now().time_since_epoch().count() / 1e9;
    
    // Run the same managed XRay/sing-box proxy path as config benchmarking.
    const int status = hunter::network::ProxyTester().checkServiceAccess(
        socks_proxy, test_url, timeout_seconds);
    if (status == 1) result.status = AccessibilityStatus::ACCESSIBLE;
    else if (status == 2) {
        result.status = AccessibilityStatus::LOCATION_FORBIDDEN;
        result.error_message = "Service denied access for this location";
    } else if (status == 0) {
        result.status = AccessibilityStatus::BLOCKED;
        result.error_message = "Service returned an access-denied or HTTP error response";
    } else {
        result.status = AccessibilityStatus::UNKNOWN;
        result.error_message = "Proxy could not be launched or service request failed";
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────
// ServiceAccessibilityDatabase Implementation
// ─────────────────────────────────────────────────────────────────────

ServiceAccessibilityDatabase::ServiceAccessibilityDatabase()
    : checker_(std::make_unique<ServiceAccessibilityChecker>()) {
}

void ServiceAccessibilityDatabase::updateServiceStatus(
    const std::string& uri,
    ServiceAccessibilityChecker::Service service,
    ServiceAccessibilityChecker::AccessibilityStatus status,
    float latency_ms) {
    
    // TODO: Update the configuration in the database
    // This would involve:
    // 1. Finding the config by URI
    // 2. Updating the appropriate service status field
    // 3. Setting the timestamp
    // 4. Persisting to storage if needed
}

std::vector<std::string> ServiceAccessibilityDatabase::getAccessibleFor(
    const std::vector<ServiceAccessibilityChecker::Service>& services) {
    
    // TODO: Query database for configs accessible to ALL specified services
    std::vector<std::string> results;
    
    return results;
}

std::vector<std::string> ServiceAccessibilityDatabase::getAccessibleForAny() {
    // TODO: Query database for configs accessible to ANY service
    std::vector<std::string> results;
    
    return results;
}

ServiceAccessibilityChecker::AccessibilityStatus ServiceAccessibilityDatabase::getServiceStatus(
    const std::string& uri,
    ServiceAccessibilityChecker::Service service) {
    
    // TODO: Query database and return service status
    return ServiceAccessibilityChecker::AccessibilityStatus::UNKNOWN;
}

std::vector<ServiceAccessibilityDatabase::ServiceAccessibilityRow> ServiceAccessibilityDatabase::getAllWithServiceStatus() {
    // TODO: Query all configs with their service accessibility status
    std::vector<ServiceAccessibilityRow> results;
    
    return results;
}

ServiceAccessibilityDatabase::ServiceAccessibilityStats ServiceAccessibilityDatabase::getStats() {
    ServiceAccessibilityStats stats;
    stats.total_configs = 0;
    stats.accessible_any = 0;
    stats.location_forbidden = 0;
    stats.all_blocked = 0;
    stats.avg_latency = 0.0f;
    
    // TODO: Calculate statistics from database
    
    return stats;
}
