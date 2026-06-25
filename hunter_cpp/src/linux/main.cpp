#include "orchestrator/orchestrator.h"
#include "core/config.h"
#include "core/utils.h"
#include "realtime/websocket_bridge.h"
#include "proxy/proxy_gateway.h"
#include "proxy/proxy_hosting_manager.h"
#include "proxy/session_router.h"
#include "proxy/traffic_observer.h"
#include "proxy/upstream_tunnel_manager.h"
#include "proxy/generated_config_manager.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>

using namespace hunter;

std::unique_ptr<HunterOrchestrator> g_orchestrator;
std::unique_ptr<realtime::WebSocketBridge> g_ws_bridge;
std::unique_ptr<hunter::proxy::ProxyGateway> g_gateway;
std::unique_ptr<hunter::proxy::ProxyHostingManager> g_hosting_mgr;
std::unique_ptr<hunter::proxy::SessionRouterEngine> g_session_router;
std::unique_ptr<hunter::proxy::TrafficObserver> g_traffic_observer;
std::unique_ptr<hunter::proxy::UpstreamTunnelManager> g_upstream_mgr;
std::unique_ptr<hunter::proxy::GeneratedConfigManager> g_generated_cfg_mgr;
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
    if (g_orchestrator) {
        g_orchestrator->stop();
    }
    if (g_gateway) {
        g_gateway->stop();
    }
    if (g_traffic_observer) {
        g_traffic_observer->stop();
    }
    if (g_hosting_mgr) {
        g_hosting_mgr->stop();
    }
    if (g_generated_cfg_mgr) {
        g_generated_cfg_mgr->stop();
    }
    if (g_upstream_mgr) {
        g_upstream_mgr->stop();
    }
    if (g_ws_bridge) {
        g_ws_bridge->stop();
    }
}

std::string handleCommand(const std::string& json_cmd) {
    if (!g_orchestrator) {
        return R"({"error":"orchestrator not initialized"})";
    }
    return g_orchestrator->processRealtimeCommand(json_cmd);
}

static std::string jsonEscapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string readStatusFile() {
    const char* paths[] = {
        "/app/hunter_cpp/runtime/HUNTER_status.json",
        "runtime/HUNTER_status.json",
        "/app/runtime/HUNTER_status.json"
    };
    for (const char* p : paths) {
        std::ifstream f(p);
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            // Trim trailing whitespace/newlines
            while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                   content.back() == ' ' || content.back() == '\t'))
                content.pop_back();
            if (!content.empty() && content.front() == '{' && content.back() == '}')
                return content;
        }
    }
    return "";
}

std::string provideStatus() {
    if (!g_orchestrator) {
        return R"({"error":"orchestrator not initialized"})";
    }
    try {
        // Read from the status file written by the orchestrator thread,
        // instead of calling buildStatusJson() which can deadlock on
        // status_mutex_ when the orchestrator is busy testing 42K configs.
        std::string base = readStatusFile();
        if (base.empty()) {
            // Status file not written yet — return minimal status
            return R"({"error":"status file not ready","phase":"starting"})";
        }

        // Inject upstream tunnel states — skip if mutex is contended
        // (tunnelLoop modifies states_ without lock, causing race condition)
        if (g_upstream_mgr) {
            try {
                // Use a short timeout approach: run in a detached thread with a flag
                // For now, skip injection to avoid blocking the publish loop
            } catch (...) {}
        }

        // Inject generated config states — skip if mutex is contended
        if (g_generated_cfg_mgr) {
            try {
                // Skip injection to avoid blocking the publish loop
            } catch (...) {}
        }

        // Inject WebSocket connection stats
        if (g_ws_bridge) {
            try {
                std::string wsStatsJson = g_ws_bridge->getStatsJson();
                std::ostringstream ws_json;
                ws_json << ",\"ws_stats\":" << wsStatsJson;

                if (!base.empty() && base.back() == '}') {
                    base.pop_back();
                    base += ws_json.str() + "}";
                }
            } catch (...) {
                // WS stats injection failed - continue without it
            }
        }

        return base;
    } catch (...) {
        return R"({"error":"status build failed"})";
    }
}

std::vector<std::string> provideLogs() {
    return hunter::utils::LogRingBuffer::instance().recent(200);
}

int main(int argc, char* argv[]) {
    std::cout << "Hunter Backend - Linux/Docker Version" << std::endl;
    std::cout << "========================================" << std::endl;

    // Setup signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Load configuration
    HunterConfig config;
    config.loadFromFile("runtime/hunter_config.json");

    // Create orchestrator
    g_orchestrator = std::make_unique<HunterOrchestrator>(config);

    // Setup WebSocket bridge for real-time communication
    const char* env_control = std::getenv("HUNTER_REST_API_PORT");
    const char* env_monitor = std::getenv("HUNTER_WS_MONITOR_PORT");
    int control_port = env_control ? std::atoi(env_control) : 7801;
    int monitor_port = env_monitor ? std::atoi(env_monitor) : 7802;
    
    g_ws_bridge = std::make_unique<realtime::WebSocketBridge>(control_port, monitor_port);
    g_ws_bridge->setCommandHandler(handleCommand);
    g_ws_bridge->setStatusProvider(provideStatus);
    g_ws_bridge->setLogsProvider(provideLogs);

    if (!g_ws_bridge->start()) {
        std::cerr << "Failed to start WebSocket bridge" << std::endl;
        return 1;
    }

    std::cout << "WebSocket control server started on port " << control_port << std::endl;
    std::cout << "WebSocket monitor server started on port " << monitor_port << std::endl;

    // Start Proxy Gateway
    const char* env_gateway = std::getenv("HUNTER_GATEWAY_PORT");
    int gateway_port = env_gateway ? std::atoi(env_gateway) : 7805;
    g_gateway = std::make_unique<hunter::proxy::ProxyGateway>(gateway_port);

    g_gateway->setPoolUpdateCallback([&]() -> std::vector<hunter::proxy::ProxyPoolEntry> {
        std::vector<hunter::proxy::ProxyPoolEntry> pool;
        if (g_orchestrator && g_orchestrator->balancer()) {
            auto backends = g_orchestrator->balancer()->getWorkingBackends();
            for (auto& b : backends) {
                hunter::proxy::ProxyPoolEntry entry;
                entry.uri = b.uri;
                entry.engine = b.engine_used;
                entry.local_port = b.local_port;
                entry.latency_ms = b.latency_ms;
                entry.healthy = (b.state == BackendState::HEALTHY);
                pool.push_back(entry);
            }
        }
        return pool;
    });

    if (!g_gateway->start()) {
        std::cerr << "Failed to start Proxy Gateway" << std::endl;
    } else {
        std::cout << "Proxy Gateway started on port " << gateway_port << std::endl;
    }

    // Start Proxy Hosting Manager
    g_hosting_mgr = std::make_unique<hunter::proxy::ProxyHostingManager>();

    g_hosting_mgr->setHealthyConfigsProvider([&](int max_count) -> std::vector<ConfigHealthRecord> {
        if (g_orchestrator && g_orchestrator->configDb()) {
            return g_orchestrator->configDb()->getHealthyRecords(max_count);
        }
        return {};
    });

    g_hosting_mgr->setEngineHintProvider([&](const std::string& uri) -> std::string {
        if (g_orchestrator && g_orchestrator->configDb()) {
            return g_orchestrator->configDb()->getPreferredEngine(uri);
        }
        return "";
    });

    g_hosting_mgr->setBalancerUpdateCallback([&](const std::vector<std::pair<std::string, float>>& configs) {
        if (g_orchestrator && g_orchestrator->balancer()) {
            g_orchestrator->balancer()->updateAvailableConfigs(configs, true);
        }
    });

    g_hosting_mgr->setInstanceStateCallback([&](const std::vector<hunter::proxy::ProxyInstance>& instances) {
        if (g_session_router) {
            g_session_router->updateInstances(instances);
        }
        if (g_traffic_observer) {
            g_traffic_observer->updateEngineHealth("all",
                static_cast<int>(std::count_if(instances.begin(), instances.end(),
                    [](const auto& i) { return i.healthy; })));
        }
    });

    g_hosting_mgr->start();
    std::cout << "Proxy Hosting Manager started" << std::endl;

    // Start Session Router Engine
    g_session_router = std::make_unique<hunter::proxy::SessionRouterEngine>();
    std::cout << "Session Router Engine started" << std::endl;

    // Start Traffic Observer
    g_traffic_observer = std::make_unique<hunter::proxy::TrafficObserver>();

    g_traffic_observer->setBroadcastCallback([&](const std::string& json) {
        hunter::realtime::broadcastGlobalMonitorEvent("gateway_metrics", json);
    });

    g_traffic_observer->setMetricsProvider([&]() -> hunter::proxy::TrafficMetrics {
        hunter::proxy::TrafficMetrics m;
        if (g_gateway) m.active_sessions = 0;
        if (g_hosting_mgr) m.live_proxy_instances = g_hosting_mgr->getLiveCount();
        if (g_session_router) m.active_sessions = g_session_router->getActiveRouteCount();
        return m;
    });

    g_traffic_observer->start();
    std::cout << "Traffic Observer started" << std::endl;

    // Start Upstream Tunnel Manager (Linux-native SSH SOCKS tunnels)
    g_upstream_mgr = std::make_unique<hunter::proxy::UpstreamTunnelManager>();

    // PRIMARY_UPSTREAM_1: 18.194.88.88 (myvps)
    {
        hunter::proxy::UpstreamTunnelConfig up1;
        up1.name = "PRIMARY_UPSTREAM_1";
        up1.host = "18.194.88.88";
        up1.ssh_port = 22;
        up1.user = "ec2-user";
        up1.identity_file = "/app/runtime/ssh/upstream1_key";
        up1.local_socks_port = 3101;
        up1.preferred = true;
        up1.reconnect_interval_s = 5;
        up1.keepalive_interval_s = 30;
        up1.keepalive_count_max = 3;
        up1.compression = "zlib";
        g_upstream_mgr->addUpstream(up1);
    }

    // PRIMARY_UPSTREAM_2: 50.114.11.18 (to_canda)
    {
        hunter::proxy::UpstreamTunnelConfig up2;
        up2.name = "PRIMARY_UPSTREAM_2";
        up2.host = "50.114.11.18";
        up2.ssh_port = 22;
        up2.user = "deployer";
        up2.identity_file = "/app/runtime/ssh/upstream2_key";
        up2.local_socks_port = 3100;
        up2.preferred = true;
        up2.reconnect_interval_s = 5;
        up2.keepalive_interval_s = 30;
        up2.keepalive_count_max = 3;
        up2.compression = "zlib";
        g_upstream_mgr->addUpstream(up2);
    }

    g_upstream_mgr->setStateChangeCallback([&](const std::vector<hunter::proxy::UpstreamTunnelState>& states) {
        std::ostringstream json;
        json << "{\"upstream_states\":[";
        for (size_t i = 0; i < states.size(); i++) {
            if (i > 0) json << ",";
            auto& s = states[i];
            json << "{\"name\":\"" << s.name << "\""
                 << ",\"host\":\"" << s.host << "\""
                 << ",\"port\":" << s.local_socks_port
                 << ",\"connected\":" << (s.connected ? "true" : "false")
                 << ",\"latency_ms\":" << s.latency_ms
                 << ",\"reconnect_count\":" << s.reconnect_count
                 << ",\"preferred\":" << (s.preferred ? "true" : "false")
                 << "}";
        }
        json << "]}";
        hunter::realtime::broadcastGlobalMonitorEvent("upstream_states", json.str());
    });

    g_upstream_mgr->setMetricsCallback([&](const std::string& json) {
        hunter::realtime::broadcastGlobalMonitorEvent("upstream_metrics", json);
    });

    g_upstream_mgr->start();
    std::cout << "Upstream Tunnel Manager started with 2 upstreams" << std::endl;
    std::cout << "[Main] Upstream manager started, continuing..." << std::endl;
    fflush(stdout);

    // Start Generated Config Manager (20 permanent public configs)
    std::cout << "[Main] Creating GeneratedConfigManager..." << std::endl;
    fflush(stdout);
    g_generated_cfg_mgr = std::make_unique<hunter::proxy::GeneratedConfigManager>();

    g_generated_cfg_mgr->setPublicHost("api.abharcable.com");
    g_generated_cfg_mgr->setConfigCount(19);

    g_generated_cfg_mgr->setUpstreamProvider([&]() -> std::vector<std::pair<std::string, int>> {
        if (g_upstream_mgr) {
            return g_upstream_mgr->getConnectedSocksEndpoints();
        }
        return {};
    });

    g_generated_cfg_mgr->setHostnameProvider([&]() -> std::string {
        return std::string("api.abharcable.com");
    });

    g_generated_cfg_mgr->setStateChangeCallback([&](const std::vector<hunter::proxy::GeneratedConfig>& configs) {
        std::ostringstream json;
        json << "{\"generated_config_states\":[";
        for (size_t i = 0; i < configs.size(); i++) {
            if (i > 0) json << ",";
            auto& c = configs[i];
            json << "{\"name\":\"" << c.name << "\""
                 << ",\"active\":" << (c.active ? "true" : "false")
                 << ",\"upstream\":\"" << c.upstream_name << "\""
                 << ",\"port\":" << c.port
                 << ",\"latency_ms\":" << c.latency_ms
                 << "}";
        }
        json << "]}";
        hunter::realtime::broadcastGlobalMonitorEvent("generated_config_states", json.str());
    });

    std::cout << "[Main] Starting GeneratedConfigManager..." << std::endl;
    fflush(stdout);
    g_generated_cfg_mgr->start();
    std::cout << "Generated Config Manager started with 19 legacy configs" << std::endl;
    fflush(stdout);

    // Start orchestrator in background thread with large stack
    std::cout << "[Main] Creating orchestrator thread..." << std::endl;
    fflush(stdout);
    pthread_t orch_tid;
    pthread_attr_t orch_attr;
    pthread_attr_init(&orch_attr);
    pthread_attr_setstacksize(&orch_attr, 8 * 1024 * 1024); // 8MB stack
    auto orch_args = std::make_pair<std::function<void()>, std::atomic<bool>*>(
        [&]() {
            try {
                g_orchestrator->start();
            } catch (const std::exception& e) {
                std::cerr << "Orchestrator error: " << e.what() << std::endl;
                g_running = false;
            }
        },
        &g_running
    );
    auto* orch_args_ptr = new std::pair<std::function<void()>, std::atomic<bool>*>(std::move(orch_args));
    pthread_create(&orch_tid, &orch_attr, [](void* arg) -> void* {
        auto* p = static_cast<std::pair<std::function<void()>, std::atomic<bool>*>*>(arg);
        p->first();
        delete p;
        return nullptr;
    }, orch_args_ptr);
    pthread_attr_destroy(&orch_attr);

    // Main loop - keep alive; status broadcasts handled by WebSocketBridge::monitorPublishLoop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Cleanup
    std::cout << "Shutting down..." << std::endl;
    
    if (g_orchestrator) {
        g_orchestrator->stop();
    }

    if (g_traffic_observer) {
        g_traffic_observer->stop();
    }

    if (g_hosting_mgr) {
        g_hosting_mgr->stop();
    }

    if (g_gateway) {
        g_gateway->stop();
    }

    if (g_ws_bridge) {
        g_ws_bridge->stop();
    }

    pthread_join(orch_tid, nullptr);

    std::cout << "Hunter backend stopped" << std::endl;
    return 0;
}
