#ifndef SERVICE_ACCESSIBILITY_H
#define SERVICE_ACCESSIBILITY_H

#include <string>
#include <vector>
#include <map>
#include <memory>

/**
 * @brief Service accessibility checker
 * 
 * Tests whether a proxy can access various online services
 * that may be geographically or administratively restricted.
 */
class ServiceAccessibilityChecker {
public:
    enum class AccessibilityStatus {
        UNKNOWN = -1,
        BLOCKED = 0,
        ACCESSIBLE = 1,
        LOCATION_FORBIDDEN = 2
    };

    struct ServiceTestResult {
        std::string service_name;
        AccessibilityStatus status;
        float latency_ms;
        std::string error_message;
        double timestamp;
    };

    enum class Service {
        GEMINI,
        CLAUDE,
        DEVIN,
        GOOGLE_DEVELOPER,
        AWS_CONSOLE
    };

    ~ServiceAccessibilityChecker() = default;

    /**
     * @brief Test if a proxy can access a specific service
     * @param proxy_uri The proxy URI to test (e.g., "socks5://127.0.0.1:1080")
     * @param service The service to test
     * @param timeout_seconds Maximum time to wait for response
     * @return ServiceTestResult with status and latency
     */
    ServiceTestResult testService(
        const std::string& proxy_uri,
        Service service,
        int timeout_seconds = 10);

    /**
     * @brief Test all services for a proxy
     * @param proxy_uri The proxy URI to test
     * @param timeout_seconds Maximum time for each service test
     * @return Vector of results, one per service
     */
    std::vector<ServiceTestResult> testAllServices(
        const std::string& proxy_uri,
        int timeout_seconds = 10);

    /**
     * @brief Get service name from enum
     */
    static std::string getServiceName(Service service);

    /// Canonical lightweight HTTP endpoint used by both manual and scheduled tests.
    static std::string getServiceTestUrl(Service service);

    /**
     * @brief Get service enum from name
     */
    static Service getServiceFromName(const std::string& name);

    /**
     * @brief Convert status to string
     */
    static std::string statusToString(AccessibilityStatus status);

    /**
     * @brief Convert string to status
     */
    static AccessibilityStatus stringToStatus(const std::string& status);

private:
    /**
     * @brief Get the test URL for a service
     * Each service has a lightweight endpoint that can be tested for accessibility
     */
    /**
     * @brief Test HTTP(S) access through SOCKS proxy
     * @param socks_proxy SOCKS proxy address
     * @param test_url URL to test
     * @param timeout_seconds Maximum wait time
     * @return Result with status and latency
     */
    ServiceTestResult testHttpAccess(
        const std::string& socks_proxy,
        const std::string& test_url,
        int timeout_seconds);

};

/**
 * @brief Extension to ConfigDatabase for service accessibility
 */
class ServiceAccessibilityDatabase {
public:
    ServiceAccessibilityDatabase();
    ~ServiceAccessibilityDatabase() = default;

    /**
     * @brief Update service status for a configuration
     */
    void updateServiceStatus(
        const std::string& uri,
        ServiceAccessibilityChecker::Service service,
        ServiceAccessibilityChecker::AccessibilityStatus status,
        float latency_ms = 0.0f);

    /**
     * @brief Get configurations accessible to all specified services
     */
    std::vector<std::string> getAccessibleFor(
        const std::vector<ServiceAccessibilityChecker::Service>& services);

    /**
     * @brief Get configurations with at least one service accessible
     */
    std::vector<std::string> getAccessibleForAny();

    /**
     * @brief Get service status for a configuration
     */
    ServiceAccessibilityChecker::AccessibilityStatus getServiceStatus(
        const std::string& uri,
        ServiceAccessibilityChecker::Service service);

    /**
     * @brief Get all configurations and their service status
     */
    struct ServiceAccessibilityRow {
        std::string uri;
        std::string endpoint_key;
        std::string source;
        float latency_ms;
        bool alive;
        std::map<std::string, std::string> service_status;  // service_name -> status_string
        double last_updated;
    };

    std::vector<ServiceAccessibilityRow> getAllWithServiceStatus();

    /**
     * @brief Get statistics about service accessibility
     */
    struct ServiceAccessibilityStats {
        int total_configs;
        int accessible_any;      // Accessible to at least one service
        int location_forbidden;
        int all_blocked;
        float avg_latency;
        std::map<std::string, int> per_service_accessible;
    };

    ServiceAccessibilityStats getStats();

private:
    std::unique_ptr<ServiceAccessibilityChecker> checker_;
};

#endif  // SERVICE_ACCESSIBILITY_H
