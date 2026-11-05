#include "network/service_accessibility.h"
#include "core/utils.h"
#include <chrono>
#include <sstream>
#include <algorithm>

ServiceAccessibilityChecker::ServiceAccessibilityChecker() {
    // Initialize service test URLs
    // These are lightweight endpoints that test accessibility quickly
    service_urls_[Service::GEMINI] = "https://gemini.google.com/api/protos/google.ai.generativelanguage.v1beta.GenerativeService/GenerateContent:generateStream";
    service_urls_[Service::CLAUDE] = "https://claude.ai/api/auth/session";
    service_urls_[Service::DEVIN] = "https://devin.ai/api/health";
    service_urls_[Service::GOOGLE_DEVELOPER] = "https://console.cloud.google.com";
    service_urls_[Service::AWS_CONSOLE] = "https://console.aws.amazon.com/console/home";
}

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
    if (lower_name == "google_developer" || lower_name == "google") return Service::GOOGLE_DEVELOPER;
    if (lower_name == "aws_console" || lower_name == "aws") return Service::AWS_CONSOLE;
    
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

std::string ServiceAccessibilityChecker::getServiceTestUrl(Service service) const {
    auto it = service_urls_.find(service);
    if (it != service_urls_.end()) {
        return it->second;
    }
    return "";
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
    
    // TODO: Implement actual HTTP request through SOCKS proxy
    // This would use libcurl with SOCKS5 support
    // For now, return simulated results
    
    // Simulate testing (random result for demo)
    int test_result = rand() % 100;
    if (test_result < 40) {
        result.status = AccessibilityStatus::ACCESSIBLE;
        result.latency_ms = 50.0f + (rand() % 300);
    } else if (test_result < 70) {
        result.status = AccessibilityStatus::LOCATION_FORBIDDEN;
        result.error_message = "HTTP 451: Unavailable For Legal Reasons";
        result.latency_ms = 100.0f + (rand() % 500);
    } else {
        result.status = AccessibilityStatus::BLOCKED;
        result.error_message = "Connection refused or timeout";
        result.latency_ms = 5000.0f;
    }
    
    return result;
}

ServiceAccessibilityChecker::AccessibilityStatus ServiceAccessibilityChecker::parseHttpStatus(
    int http_status) const {
    
    if (http_status >= 200 && http_status < 300) {
        return AccessibilityStatus::ACCESSIBLE;
    } else if (http_status == 451) {
        // HTTP 451: Unavailable For Legal Reasons (geographic restriction)
        return AccessibilityStatus::LOCATION_FORBIDDEN;
    } else if (http_status == 403 || http_status == 403) {
        // HTTP 403: Forbidden (might be geographic)
        return AccessibilityStatus::LOCATION_FORBIDDEN;
    } else if (http_status >= 400 && http_status < 500) {
        return AccessibilityStatus::BLOCKED;
    } else if (http_status >= 500) {
        return AccessibilityStatus::BLOCKED;
    }
    
    return AccessibilityStatus::UNKNOWN;
}

bool ServiceAccessibilityChecker::isLocationForbidden(
    int http_status,
    const std::string& response_headers,
    const std::string& response_body) const {
    
    // HTTP 451: Unavailable For Legal Reasons (standard geo-block)
    if (http_status == 451) {
        return true;
    }
    
    // Check for geo-blocking headers
    std::string headers_lower = response_headers;
    std::transform(headers_lower.begin(), headers_lower.end(), headers_lower.begin(), ::tolower);
    
    if (headers_lower.find("x-geo-block") != std::string::npos ||
        headers_lower.find("x-restricted-location") != std::string::npos ||
        headers_lower.find("x-blocked-region") != std::string::npos) {
        return true;
    }
    
    // Check response body for common geo-block messages
    std::string body_lower = response_body;
    std::transform(body_lower.begin(), body_lower.end(), body_lower.begin(), ::tolower);
    
    if (body_lower.find("not available in your country") != std::string::npos ||
        body_lower.find("not available in your region") != std::string::npos ||
        body_lower.find("geographic restriction") != std::string::npos ||
        body_lower.find("location not allowed") != std::string::npos ||
        body_lower.find("access denied from your country") != std::string::npos) {
        return true;
    }
    
    return false;
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
