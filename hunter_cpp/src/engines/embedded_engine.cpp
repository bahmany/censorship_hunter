#include "engines/embedded_engine.h"
#include "core/utils.h"
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hunter {
namespace engines {

// ============================================================================
// SingBoxEngine Implementation
// ============================================================================

SingBoxEngine::SingBoxEngine() : EngineBase("sing-box"), initialized_(false) {
    initializeSingBox();
}

SingBoxEngine::~SingBoxEngine() {
    // Cleanup if needed
}

void SingBoxEngine::initializeSingBox() {
    // For embedded implementation, we'll simulate sing-box functionality
    // In a real implementation, this would initialize the actual sing-box library
    version_ = "1.10.5-embedded";
    initialized_ = true;
}

std::string SingBoxEngine::generateConfig(const ParsedConfig& config, int listen_port) {
    if (!initialized_) {
        return "";
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"log\": {\n";
    json << "    \"level\": \"info\",\n";
    json << "    \"timestamp\": true\n";
    json << "  },\n";
    
    // DNS configuration
    json << "  \"dns\": {\n";
    json << createDnsConfig();
    json << "  },\n";
    
    // Inbounds
    json << "  \"inbounds\": [\n";
    json << createInboundConfig(listen_port);
    json << "  ],\n";
    
    // Outbounds
    json << "  \"outbounds\": [\n";
    json << createOutboundConfig(config);
    json << "    ,{\"type\":\"direct\",\"tag\":\"direct\"}\n";
    json << "    ,{\"type\":\"block\",\"tag\":\"blackhole\"}\n";
    for (int p = 0; p < 5; ++p) {
        json << "    ,{\"type\":\"socks\",\"tag\":\"socks5-fb-" << p << "\",\"server\":\"172.20.14.34\",\"server_port\":" << (3100+p) << "}\n";
    }
    json << "  ],\n";
    
    // Routing
    json << "  \"route\": {\n";
    json << createRoutingConfig();
    json << "  }\n";
    json << "}";
    
    return json.str();
}

std::string SingBoxEngine::generateBalancedConfig(const std::vector<ParsedConfig>& configs, int listen_port) {
    if (!initialized_ || configs.empty()) {
        return "";
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"log\": {\n";
    json << "    \"level\": \"info\",\n";
    json << "    \"timestamp\": true\n";
    json << "  },\n";
    
    // DNS configuration
    json << "  \"dns\": {\n";
    json << createDnsConfig();
    json << "  },\n";
    
    // Inbounds
    json << "  \"inbounds\": [\n";
    json << createInboundConfig(listen_port);
    json << "  ],\n";
    
    // Outbounds with URL test
    json << "  \"outbounds\": [\n";
    
    // Add all proxy outbounds
    for (size_t i = 0; i < configs.size(); ++i) {
        std::string ob = createOutboundConfig(configs[i]);
        std::string tag_needle = "\"tag\": \"proxy\"";
        size_t tag_pos = ob.find(tag_needle);
        if (tag_pos != std::string::npos) {
            ob.replace(tag_pos, tag_needle.size(), "\"tag\": \"proxy-" + std::to_string(i) + "\"");
        }
        json << ob;
        if (i < configs.size() - 1) json << ",";
        json << "\n";
    }
    
    // Add selector
    json << "    ,{\n";
    json << "      \"type\": \"selector\",\n";
    json << "      \"tag\": \"proxy\",\n";
    json << "      \"outbounds\": [";
    for (size_t i = 0; i < configs.size(); ++i) {
        json << "\"proxy-" << i << "\"";
        if (i < configs.size() - 1) json << ",";
    }
    for (int p = 0; p < 5; ++p) {
        json << ",\"socks5-fb-" << p << "\"";
    }
    json << "],\n";
    json << "      \"default\": \"proxy-0\"\n";
    json << "    }\n";
    // Add direct, blackhole, and SOCKS5 fallback outbounds
    json << "    ,{\"type\":\"direct\",\"tag\":\"direct\"}\n";
    json << "    ,{\"type\":\"block\",\"tag\":\"blackhole\"}\n";
    for (int p = 0; p < 5; ++p) {
        json << "    ,{\"type\":\"socks\",\"tag\":\"socks5-fb-" << p << "\",\"server\":\"172.20.14.34\",\"server_port\":" << (3100+p) << "}\n";
    }
    json << "  ],\n";
    
    // Routing
    json << "  \"route\": {\n";
    json << createRoutingConfig();
    json << "  }\n";
    json << "}";
    
    return json.str();
}

std::string SingBoxEngine::createInboundConfig(int listen_port) {
    std::ostringstream json;
    json << "    {\n";
    json << "      \"type\": \"mixed\",\n";
    json << "      \"tag\": \"mixed-in\",\n";
    json << "      \"listen\": \"127.0.0.1\",\n";
    json << "      \"listen_port\": " << listen_port << ",\n";
    json << "      \"sniff\": true,\n";
    json << "      \"sniff_override_destination\": true\n";
    json << "    }";
    return json.str();
}

std::string SingBoxEngine::createOutboundConfig(const ParsedConfig& config) {
    std::ostringstream json;
    json << "    {\n";
    json << "      \"type\": \"" << config.protocol << "\",\n";
    json << "      \"tag\": \"proxy\",\n";
    json << "      \"server\": \"" << config.address << "\",\n";
    json << "      \"server_port\": " << config.port << ",\n";
    
    // Protocol-specific settings
    if (config.protocol == "vmess") {
        json << "      \"uuid\": \"" << config.uuid << "\",\n";
        if (!config.encryption.empty()) {
            json << "      \"alter_id\": 0,\n";
            json << "      \"security\": \"" << config.encryption << "\",\n";
        }
    } else if (config.protocol == "vless") {
        json << "      \"uuid\": \"" << config.uuid << "\",\n";
        if (!config.flow.empty()) {
            json << "      \"flow\": \"" << config.flow << "\",\n";
        }
    } else if (config.protocol == "trojan") {
        json << "      \"password\": \"" << config.uuid << "\",\n";
    } else if (config.protocol == "shadowsocks") {
        json << "      \"password\": \"" << config.uuid << "\",\n";
        json << "      \"cipher\": \"" << config.encryption << "\",\n";
    }
    
    // Transport settings
    if (!config.network.empty() && config.network != "tcp") {
        json << "      \"transport\": {\n";
        json << "        \"type\": \"" << config.network << "\",\n";
        
        if (config.network == "ws") {
            if (!config.path.empty()) {
                json << "        \"path\": \"" << config.path << "\",\n";
            }
            if (!config.host.empty()) {
                json << "        \"headers\": {\n";
                json << "          \"Host\": \"" << config.host << "\"\n";
                json << "        },\n";
            }
        } else if (config.network == "grpc") {
            if (!config.path.empty()) {
                json << "        \"service_name\": \"" << config.path << "\",\n";
            }
        }
        
        json << "      },\n";
    }
    
    // TLS settings
    if (config.isTLS() || config.isReality()) {
        json << "      \"tls\": {\n";
        json << "        \"enabled\": true,\n";
        if (!config.sni.empty()) {
            json << "        \"server_name\": \"" << config.sni << "\",\n";
        }
        if (config.isReality()) {
            json << "        \"reality\": {\n";
            json << "          \"enabled\": true,\n";
            if (!config.public_key.empty()) {
                json << "          \"public_key\": \"" << config.public_key << "\",\n";
            }
            if (!config.short_id.empty()) {
                json << "          \"short_id\": \"" << config.short_id << "\",\n";
            }
            json << "        }\n";
        } else {
            if (!config.fingerprint.empty()) {
                json << "        \"utls\": {\n";
                json << "          \"fingerprint\": \"" << config.fingerprint << "\"\n";
                json << "        },\n";
            }
        }
        json << "      },\n";
    }
    
    json << "    }";
    return json.str();
}

std::string SingBoxEngine::createDnsConfig() {
    std::ostringstream json;
    json << "    \"servers\": [\n";
    json << "      {\"address\": \"1.1.1.1\", \"tag\": \"cloudflare\"},\n";
    json << "      {\"address\": \"8.8.8.8\", \"tag\": \"google\"},\n";
    json << "      {\"address\": \"178.22.122.100\", \"tag\": \"shecan\"},\n";
    json << "      {\"address\": \"78.157.42.100\", \"tag\": \"electro\"}\n";
    json << "    ],\n";
    json << "    \"final\": \"cloudflare\",\n";
    json << "    \"strategy\": \"prefer_ipv4\"\n";
    return json.str();
}

std::string SingBoxEngine::createRoutingConfig() {
    std::ostringstream json;
    json << "    \"rules\": [\n";
    json << "      {\n";
    json << "        \"protocol\": \"dns\",\n";
    json << "        \"outbound\": \"direct\"\n";
    json << "      },\n";
    json << "      {\n";
    json << "        \"geoip\": [\"private\"],\n";
    json << "        \"outbound\": \"direct\"\n";
    json << "      },\n";
    json << "      {\n";
    json << "        \"inbound\": [\"mixed-in\"],\n";
    json << "        \"outbound\": \"proxy\"\n";
    json << "      }\n";
    json << "    ],\n";
    json << "    \"final\": \"blackhole\",\n";
    json << "    \"auto_detect_interface\": true\n";
    return json.str();
}

int SingBoxEngine::startProcess(const std::string& config_path) {
    // For embedded implementation, simulate process start
    // In real implementation, this would start the actual sing-box process
    std::cout << "[SingBox] Starting with config: " << config_path << std::endl;
    
#ifdef _WIN32
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    std::string cmd = "sing-box run -c \"" + config_path + "\"";
    
    if (CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL, FALSE, 
                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return static_cast<int>(pi.dwProcessId);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("sing-box", "sing-box", "run", "-c", config_path.c_str(), NULL);
        exit(1);
    } else if (pid > 0) {
        return pid;
    }
#endif
    
    return -1;
}

bool SingBoxEngine::stopProcess(int pid) {
    if (pid <= 0) return false;
    
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return true;
    }
#else
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
    return true;
#endif
    
    return false;
}

bool SingBoxEngine::isProcessAlive(int pid) {
    if (pid <= 0) return false;
    
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        DWORD exitCode;
        bool alive = GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
        CloseHandle(hProcess);
        return alive;
    }
#else
    return kill(pid, 0) == 0;
#endif
    
    return false;
}

std::string SingBoxEngine::getVersion() {
    return version_;
}

bool SingBoxEngine::isAvailable() {
    return initialized_;
}

// ============================================================================
// XRayEngine Implementation
// ============================================================================

XRayEngine::XRayEngine() : EngineBase("xray"), initialized_(false) {
    initializeXRay();
}

XRayEngine::~XRayEngine() {
    // Cleanup if needed
}

void XRayEngine::initializeXRay() {
    // For embedded implementation, we'll simulate XRay functionality
    version_ = "25.1.30-embedded";
    initialized_ = true;
}

std::string XRayEngine::generateConfig(const ParsedConfig& config, int listen_port) {
    if (!initialized_) {
        return "";
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"log\": {\n";
    json << "    \"loglevel\": \"info\"\n";
    json << "  },\n";
    
    // DNS configuration
    json << "  \"dns\": {\n";
    json << createDnsConfig();
    json << "  },\n";
    
    // Inbounds
    json << "  \"inbounds\": [\n";
    json << createInboundConfig(listen_port);
    json << "  ],\n";
    
    // Outbounds
    json << "  \"outbounds\": [\n";
    json << createOutboundConfig(config);
    json << "    ,{\"tag\":\"direct\",\"protocol\":\"freedom\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}";
    json << "    ,{\"tag\":\"dns-out\",\"protocol\":\"dns\",\"settings\":{}}";
    json << "    ,{\"tag\":\"blackhole\",\"protocol\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";
    // SOCKS5 fallback outbounds
    for (int p = 0; p < 5; ++p) {
        json << "    ,{\"tag\":\"socks5-fb-" << p << "\",\"protocol\":\"socks\",\"settings\":{\"servers\":[{\"address\":\"172.20.14.34\",\"port\":" << (3100+p) << "}]}}";
    }
    json << "\n  ],\n";
    
    // Routing
    json << "  \"routing\": {\n";
    json << createRoutingConfig();
    json << "  },\n";
    
    // Observatory for health checking
    json << "  \"observatory\": {\n";
    json << createObservatoryConfig();
    json << "  }\n";
    json << "}";
    
    return json.str();
}

std::string XRayEngine::generateBalancedConfig(const std::vector<ParsedConfig>& configs, int listen_port) {
    if (!initialized_ || configs.empty()) {
        return "";
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"log\": {\n";
    json << "    \"loglevel\": \"info\"\n";
    json << "  },\n";
    
    // DNS configuration
    json << "  \"dns\": {\n";
    json << createDnsConfig();
    json << "  },\n";
    
    // Inbounds
    json << "  \"inbounds\": [\n";
    json << createInboundConfig(listen_port);
    json << "  ],\n";
    
    // Outbounds
    json << "  \"outbounds\": [\n";
    
    // Add all proxy outbounds
    for (size_t i = 0; i < configs.size(); ++i) {
        std::string ob = createOutboundConfig(configs[i]);
        std::string tag_needle = "\"tag\": \"proxy\"";
        size_t tag_pos = ob.find(tag_needle);
        if (tag_pos != std::string::npos) {
            ob.replace(tag_pos, tag_needle.size(), "\"tag\": \"proxy-" + std::to_string(i) + "\"");
        }
        json << ob;
        if (i < configs.size() - 1) json << ",";
        json << "\n";
    }
    
    // Add balancer
    json << "    ,{\n";
    json << "      \"tag\": \"proxy\",\n";
    json << "      \"protocol\": \"balancer\",\n";
    json << "      \"selector\": [";
    for (size_t i = 0; i < configs.size(); ++i) {
        json << "\"proxy-" << i << "\"";
        if (i < configs.size() - 1) json << ",";
    }
    for (int p = 0; p < 5; ++p) {
        json << ",\"socks5-fb-" << p << "\"";
    }
    json << "]\n";
    json << "    }\n";
    json << "    ,{\"tag\":\"direct\",\"protocol\":\"freedom\",\"settings\":{\"domainStrategy\":\"UseIPv4\"}}";
    json << "    ,{\"tag\":\"dns-out\",\"protocol\":\"dns\",\"settings\":{}}";
    json << "    ,{\"tag\":\"blackhole\",\"protocol\":\"blackhole\",\"settings\":{\"response\":{\"type\":\"none\"}}}";
    // SOCKS5 fallback outbounds
    for (int p = 0; p < 5; ++p) {
        json << "    ,{\"tag\":\"socks5-fb-" << p << "\",\"protocol\":\"socks\",\"settings\":{\"servers\":[{\"address\":\"172.20.14.34\",\"port\":" << (3100+p) << "}]}}";
    }
    json << "\n  ],\n";
    
    // Routing
    json << "  \"routing\": {\n";
    json << createRoutingConfig();
    json << "  },\n";
    
    // Observatory
    json << "  \"observatory\": {\n";
    json << createObservatoryConfig();
    json << "  }\n";
    json << "}";
    
    return json.str();
}

std::string XRayEngine::createInboundConfig(int listen_port) {
    std::ostringstream json;
    json << "    {\n";
    json << "      \"tag\": \"socks-in\",\n";
    json << "      \"protocol\": \"socks\",\n";
    json << "      \"listen\": \"127.0.0.1\",\n";
    json << "      \"port\": " << listen_port << ",\n";
    json << "      \"sniffing\": {\n";
    json << "        \"enabled\": true,\n";
    json << "        \"destOverride\": [\"http\", \"tls\"]\n";
    json << "      }\n";
    json << "    }";
    return json.str();
}

std::string XRayEngine::createOutboundConfig(const ParsedConfig& config) {
    std::ostringstream json;
    json << "    {\n";
    json << "      \"tag\": \"proxy\",\n";
    json << "      \"protocol\": \"" << config.protocol << "\",\n";
    json << "      ";
    
    // Use existing XRay outbound generation
    json << config.toXrayOutboundJson(10808).substr(1); // Remove opening brace
    json << "\n";
    json << "    }";
    return json.str();
}

std::string XRayEngine::createDnsConfig() {
    std::ostringstream json;
    json << "    \"servers\": [\n";
    json << "      \"1.1.1.1\",\n";
    json << "      \"8.8.8.8\",\n";
    json << "      \"178.22.122.100\",\n";
    json << "      \"78.157.42.100\"\n";
    json << "    ],\n";
    json << "    \"queryStrategy\": \"UseIPv4\"\n";
    return json.str();
}

std::string XRayEngine::createRoutingConfig() {
    std::ostringstream json;
    json << "    \"rules\": [\n";
    json << "      {\n";
    json << "        \"type\": \"field\",\n";
    json << "        \"inboundTag\": [\"socks-in\"],\n";
    json << "        \"port\": 53,\n";
    json << "        \"outboundTag\": \"dns-out\"\n";
    json << "      },\n";
    json << "      {\n";
    json << "        \"type\": \"field\",\n";
    json << "        \"ip\": [\"geoip:private\"],\n";
    json << "        \"outboundTag\": \"direct\"\n";
    json << "      },\n";
    json << "      {\n";
    json << "        \"type\": \"field\",\n";
    json << "        \"inboundTag\": [\"socks-in\"],\n";
    json << "        \"outboundTag\": \"proxy\"\n";
    json << "      }\n";
    json << "    ],\n";
    json << "    \"final\": \"blackhole\"\n";
    return json.str();
}

std::string XRayEngine::createObservatoryConfig() {
    std::ostringstream json;
    json << "    \"subjectSelector\": [\"proxy-*\",\"socks5-fb-*\"],\n";
    json << "    \"probeUrl\": \"http://www.gstatic.com/generate_204\",\n";
    json << "    \"probeInterval\": \"30s\"\n";
    return json.str();
}

int XRayEngine::startProcess(const std::string& config_path) {
    // For embedded implementation, simulate process start
    std::cout << "[XRay] Starting with config: " << config_path << std::endl;
    
#ifdef _WIN32
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    std::string cmd = "xray run -c \"" + config_path + "\"";
    
    if (CreateProcessA(NULL, const_cast<char*>(cmd.c_str()), NULL, NULL, FALSE, 
                     CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return static_cast<int>(pi.dwProcessId);
    }
#else
    pid_t pid = fork();
    if (pid == 0) {
        execlp("xray", "xray", "run", "-c", config_path.c_str(), NULL);
        exit(1);
    } else if (pid > 0) {
        return pid;
    }
#endif
    
    return -1;
}

bool XRayEngine::stopProcess(int pid) {
    if (pid <= 0) return false;
    
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProcess) {
        TerminateProcess(hProcess, 0);
        CloseHandle(hProcess);
        return true;
    }
#else
    kill(pid, SIGTERM);
    int status;
    waitpid(pid, &status, 0);
    return true;
#endif
    
    return false;
}

bool XRayEngine::isProcessAlive(int pid) {
    if (pid <= 0) return false;
    
#ifdef _WIN32
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        DWORD exitCode;
        bool alive = GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE;
        CloseHandle(hProcess);
        return alive;
    }
#else
    return kill(pid, 0) == 0;
#endif
    
    return false;
}

std::string XRayEngine::getVersion() {
    return version_;
}

bool XRayEngine::isAvailable() {
    return initialized_;
}

// ============================================================================
// EngineManager Implementation
// ============================================================================

EngineManager& EngineManager::getInstance() {
    static EngineManager instance;
    return instance;
}

void EngineManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    
    // Initialize embedded engines
    engines_["sing-box"] = std::make_shared<SingBoxEngine>();
    engines_["xray"] = std::make_shared<XRayEngine>();
    
    initialized_ = true;
}

std::shared_ptr<EngineBase> EngineManager::getEngine(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = engines_.find(name);
    if (it != engines_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::string> EngineManager::getAvailableEngines() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> available;
    for (const auto& pair : engines_) {
        if (pair.second && pair.second->isAvailable()) {
            available.push_back(pair.first);
        }
    }
    return available;
}

std::string EngineManager::generateConfig(const std::string& engine_name, const ParsedConfig& config, int listen_port) {
    auto engine = getEngine(engine_name);
    if (engine) {
        return engine->generateConfig(config, listen_port);
    }
    return "";
}

std::string EngineManager::generateBalancedConfig(const std::string& engine_name, const std::vector<ParsedConfig>& configs, int listen_port) {
    auto engine = getEngine(engine_name);
    if (engine) {
        return engine->generateBalancedConfig(configs, listen_port);
    }
    return "";
}

int EngineManager::startProcess(const std::string& engine_name, const std::string& config_path) {
    auto engine = getEngine(engine_name);
    if (engine) {
        return engine->startProcess(config_path);
    }
    return -1;
}

bool EngineManager::stopProcess(const std::string& engine_name, int pid) {
    auto engine = getEngine(engine_name);
    if (engine) {
        return engine->stopProcess(pid);
    }
    return false;
}

bool EngineManager::isProcessAlive(const std::string& engine_name, int pid) {
    auto engine = getEngine(engine_name);
    if (engine) {
        return engine->isProcessAlive(pid);
    }
    return false;
}

} // namespace engines
} // namespace hunter
