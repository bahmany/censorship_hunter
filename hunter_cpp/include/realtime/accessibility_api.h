#ifndef ACCESSIBILITY_API_H
#define ACCESSIBILITY_API_H

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include "network/service_accessibility.h"

using json = nlohmann::json;

/**
 * @brief WebSocket API handlers for service accessibility features
 * 
 * Provides JSON-RPC style commands for querying and filtering
 * proxy configurations by their service accessibility.
 */
class AccessibilityAPI {
public:
    AccessibilityAPI();
    ~AccessibilityAPI() = default;

    /**
     * @brief Process incoming WebSocket command
     * @param command JSON command with method and params
     * @return JSON response
     */
    json handleCommand(const json& command);

private:
    std::unique_ptr<ServiceAccessibilityDatabase> db_;

    // ─── Command Handlers ───

    /**
     * Get all configurations with their service accessibility status
     * Method: get_configs_with_services
     * Params: {
     *   "page": 1,
     *   "page_size": 25,
     *   "sort_by": "uri",  // "uri", "latency", "source"
     *   "sort_order": "asc"
     * }
     */
    json handleGetConfigsWithServices(const json& params);

    /**
     * Filter configurations by service accessibility
     * Method: filter_by_services
     * Params: {
     *   "services": ["gemini", "claude"],  // Services to filter by
     *   "require_all": false,  // true = all services accessible, false = any service
     *   "status": "accessible",  // Optional: filter by status
     *   "source": "github",  // Optional: filter by source
     *   "max_latency": 500,  // Optional: max latency in ms
     *   "page": 1,
     *   "page_size": 25
     * }
     */
    json handleFilterByServices(const json& params);

    /**
     * Get service accessibility statistics
     * Method: get_accessibility_stats
     * Params: {}
     */
    json handleGetAccessibilityStats(const json& params);

    /**
     * Get configuration details with all service statuses
     * Method: get_config_details
     * Params: {
     *   "uri_hash": "ek1:abc123..."
     * }
     */
    json handleGetConfigDetails(const json& params);

    /**
     * Trigger service accessibility testing for a configuration
     * Method: test_services
     * Params: {
     *   "uri": "vless://...",
     *   "services": ["gemini", "claude"],  // Empty = test all
     *   "timeout": 10  // seconds
     * }
     */
    json handleTestServices(const json& params);

    /**
     * Get services that have at least one accessible configuration
     * Method: get_services_with_access
     * Params: {}
     */
    json handleGetServicesWithAccess(const json& params);

    /**
     * Export configurations as CSV
     * Method: export_configs
     * Params: {
     *   "services": ["gemini", "claude"],  // Filter by services (optional)
     *   "status": "accessible",  // Filter by status (optional)
     *   "format": "csv"  // Only CSV for now
     * }
     */
    json handleExportConfigs(const json& params);

    // ─── Helper Methods ───

    /**
     * Convert ServiceTestResult to JSON
     */
    json serviceTestResultToJson(const ServiceAccessibilityChecker::ServiceTestResult& result);

    /**
     * Convert ServiceAccessibilityRow to JSON
     */
    json serviceRowToJson(const ServiceAccessibilityDatabase::ServiceAccessibilityRow& row);

    /**
     * Format error response
     */
    json errorResponse(int code, const std::string& message);

    /**
     * Format success response
     */
    json successResponse(const json& data);
};

#endif  // ACCESSIBILITY_API_H
