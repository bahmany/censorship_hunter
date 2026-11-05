#include "realtime/accessibility_api.h"
#include <sstream>
#include <iomanip>

AccessibilityAPI::AccessibilityAPI()
    : db_(std::make_unique<ServiceAccessibilityDatabase>()) {
}

json AccessibilityAPI::handleCommand(const json& command) {
    try {
        if (!command.contains("method")) {
            return errorResponse(-32600, "Missing 'method' field");
        }

        std::string method = command["method"];
        json params = command.contains("params") ? command["params"] : json::object();

        if (method == "get_configs_with_services") {
            return handleGetConfigsWithServices(params);
        } else if (method == "filter_by_services") {
            return handleFilterByServices(params);
        } else if (method == "get_accessibility_stats") {
            return handleGetAccessibilityStats(params);
        } else if (method == "get_config_details") {
            return handleGetConfigDetails(params);
        } else if (method == "test_services") {
            return handleTestServices(params);
        } else if (method == "get_services_with_access") {
            return handleGetServicesWithAccess(params);
        } else if (method == "export_configs") {
            return handleExportConfigs(params);
        } else {
            return errorResponse(-32601, "Unknown method: " + method);
        }
    } catch (const std::exception& e) {
        return errorResponse(-32603, std::string("Internal error: ") + e.what());
    }
}

json AccessibilityAPI::handleGetConfigsWithServices(const json& params) {
    try {
        int page = params.value("page", 1);
        int page_size = params.value("page_size", 25);
        std::string sort_by = params.value("sort_by", "uri");
        std::string sort_order = params.value("sort_order", "asc");

        // Get all configurations with service status
        auto rows = db_->getAllWithServiceStatus();

        // Apply pagination
        int total = rows.size();
        int start = (page - 1) * page_size;
        int end = std::min(start + page_size, total);
        
        std::vector<json> paginated;
        for (int i = start; i < end; ++i) {
            paginated.push_back(serviceRowToJson(rows[i]));
        }

        json response;
        response["configs"] = paginated;
        response["total"] = total;
        response["page"] = page;
        response["page_size"] = page_size;
        response["total_pages"] = (total + page_size - 1) / page_size;

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleFilterByServices(const json& params) {
    try {
        std::vector<std::string> services = params.value("services", std::vector<std::string>());
        bool require_all = params.value("require_all", false);
        std::string status = params.value("status", "");
        std::string source = params.value("source", "");
        int max_latency = params.value("max_latency", -1);
        int page = params.value("page", 1);
        int page_size = params.value("page_size", 25);

        // Get all rows
        auto rows = db_->getAllWithServiceStatus();

        // Apply filters
        std::vector<json> filtered;
        for (const auto& row : rows) {
            // Service filter
            bool service_match = true;
            if (!services.empty()) {
                if (require_all) {
                    // All services must be accessible
                    for (const auto& service : services) {
                        if (row.service_status.find(service) == row.service_status.end() ||
                            row.service_status.at(service) != "accessible") {
                            service_match = false;
                            break;
                        }
                    }
                } else {
                    // At least one service must be accessible
                    service_match = false;
                    for (const auto& service : services) {
                        if (row.service_status.find(service) != row.service_status.end() &&
                            row.service_status.at(service) == "accessible") {
                            service_match = true;
                            break;
                        }
                    }
                }
            }

            if (!service_match) continue;

            // Source filter
            if (!source.empty() && row.source != source) continue;

            // Latency filter
            if (max_latency > 0 && row.latency_ms > max_latency) continue;

            filtered.push_back(serviceRowToJson(row));
        }

        // Apply pagination
        int total = filtered.size();
        int start = (page - 1) * page_size;
        int end = std::min(start + page_size, total);

        json response;
        response["configs"] = json::array();
        for (int i = start; i < end; ++i) {
            response["configs"].push_back(filtered[i]);
        }
        response["total"] = total;
        response["page"] = page;
        response["page_size"] = page_size;
        response["total_pages"] = (total + page_size - 1) / page_size;

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleGetAccessibilityStats(const json& params) {
    try {
        auto stats = db_->getStats();

        json response;
        response["total_configs"] = stats.total_configs;
        response["accessible_any"] = stats.accessible_any;
        response["location_forbidden"] = stats.location_forbidden;
        response["all_blocked"] = stats.all_blocked;
        response["avg_latency"] = stats.avg_latency;
        response["per_service_accessible"] = stats.per_service_accessible;

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleGetConfigDetails(const json& params) {
    try {
        if (!params.contains("uri_hash")) {
            return errorResponse(-32602, "Missing required parameter: uri_hash");
        }

        std::string uri_hash = params["uri_hash"];
        
        // TODO: Fetch specific config from database
        json response;
        response["uri_hash"] = uri_hash;
        response["found"] = false;

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleTestServices(const json& params) {
    try {
        if (!params.contains("uri")) {
            return errorResponse(-32602, "Missing required parameter: uri");
        }

        std::string uri = params["uri"];
        std::vector<std::string> services = params.value("services", std::vector<std::string>());
        int timeout = params.value("timeout", 10);

        json response;
        response["uri"] = uri;
        response["results"] = json::array();

        // TODO: Actually test services
        // For now, return empty results
        response["status"] = "testing";
        response["message"] = "Service testing started asynchronously";

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleGetServicesWithAccess(const json& params) {
    try {
        // Get all configurations
        auto rows = db_->getAllWithServiceStatus();

        // Build map of services with accessible configs
        std::map<std::string, int> services_access;
        services_access["gemini"] = 0;
        services_access["claude"] = 0;
        services_access["devin"] = 0;
        services_access["google_developer"] = 0;
        services_access["aws_console"] = 0;

        for (const auto& row : rows) {
            for (const auto& [service, status] : row.service_status) {
                if (status == "accessible") {
                    services_access[service]++;
                }
            }
        }

        json response;
        for (const auto& [service, count] : services_access) {
            response[service] = count;
        }

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::handleExportConfigs(const json& params) {
    try {
        std::vector<std::string> services = params.value("services", std::vector<std::string>());
        std::string status_filter = params.value("status", "");
        std::string format = params.value("format", "csv");

        if (format != "csv") {
            return errorResponse(-32602, "Only CSV format is supported");
        }

        // Get all rows
        auto rows = db_->getAllWithServiceStatus();

        // Apply filters
        std::vector<json> filtered;
        for (const auto& row : rows) {
            // Service filter
            if (!services.empty()) {
                bool found = false;
                for (const auto& service : services) {
                    if (row.service_status.find(service) != row.service_status.end() &&
                        row.service_status.at(service) == "accessible") {
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
            }

            filtered.push_back(serviceRowToJson(row));
        }

        // Generate CSV
        std::stringstream csv;
        csv << "Config Hash,URI,Source,Latency (ms),Alive,Gemini,Claude,Devin,Google Developer,AWS Console,Last Updated\n";

        for (const auto& row : filtered) {
            csv << row["endpoint_key"].get<std::string>() << ",";
            csv << "\"" << row["uri"].get<std::string>() << "\",";
            csv << row["source"].get<std::string>() << ",";
            csv << row["latency_ms"].get<float>() << ",";
            csv << (row["alive"].get<bool>() ? "yes" : "no") << ",";
            csv << row["gemini"].get<std::string>() << ",";
            csv << row["claude"].get<std::string>() << ",";
            csv << row["devin"].get<std::string>() << ",";
            csv << row["google_developer"].get<std::string>() << ",";
            csv << row["aws_console"].get<std::string>() << ",";
            csv << row["last_updated"].get<double>() << "\n";
        }

        json response;
        response["format"] = "csv";
        response["data"] = csv.str();
        response["row_count"] = filtered.size();

        return successResponse(response);
    } catch (const std::exception& e) {
        return errorResponse(-32603, e.what());
    }
}

json AccessibilityAPI::serviceTestResultToJson(
    const ServiceAccessibilityChecker::ServiceTestResult& result) {
    
    json obj;
    obj["service"] = result.service_name;
    obj["status"] = ServiceAccessibilityChecker::statusToString(result.status);
    obj["latency_ms"] = result.latency_ms;
    obj["error"] = result.error_message;
    obj["timestamp"] = result.timestamp;

    return obj;
}

json AccessibilityAPI::serviceRowToJson(
    const ServiceAccessibilityDatabase::ServiceAccessibilityRow& row) {
    
    json obj;
    obj["uri"] = row.uri;
    obj["endpoint_key"] = row.endpoint_key;
    obj["source"] = row.source;
    obj["latency_ms"] = row.latency_ms;
    obj["alive"] = row.alive;
    obj["last_updated"] = row.last_updated;
    
    // Service status
    for (const auto& [service, status] : row.service_status) {
        obj[service] = status;
    }

    return obj;
}

json AccessibilityAPI::errorResponse(int code, const std::string& message) {
    json response;
    response["error"] = true;
    response["code"] = code;
    response["message"] = message;
    return response;
}

json AccessibilityAPI::successResponse(const json& data) {
    json response;
    response["error"] = false;
    response["data"] = data;
    return response;
}
