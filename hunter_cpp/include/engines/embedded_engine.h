#pragma once

#include <string>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "core/models.h"

namespace hunter {
namespace engines {

/**
 * @brief Abstract base class for proxy engines
 */
class EngineBase {
public:
    EngineBase(const std::string& name) : name_(name) {}
    virtual ~EngineBase() = default;

    // Configuration generation
    virtual std::string generateConfig(const ParsedConfig& config, int listen_port) = 0;
    virtual std::string generateBalancedConfig(const std::vector<ParsedConfig>& configs, int listen_port) = 0;
    
    // Process management
    virtual int startProcess(const std::string& config_path) = 0;
    virtual bool stopProcess(int pid) = 0;
    virtual bool isProcessAlive(int pid) = 0;
    
    // Engine info
    virtual std::string getVersion() = 0;
    virtual bool isAvailable() = 0;
    virtual std::string getEngineName() const { return name_; }

protected:
    std::string name_;
};

/**
 * @brief Embedded sing-box engine implementation
 */
class SingBoxEngine : public EngineBase {
public:
    SingBoxEngine();
    ~SingBoxEngine() override;

    std::string generateConfig(const ParsedConfig& config, int listen_port) override;
    std::string generateBalancedConfig(const std::vector<ParsedConfig>& configs, int listen_port) override;
    
    int startProcess(const std::string& config_path) override;
    bool stopProcess(int pid) override;
    bool isProcessAlive(int pid) override;
    
    std::string getVersion() override;
    bool isAvailable() override;

private:
    void initializeSingBox();
    std::string createInboundConfig(int listen_port);
    std::string createOutboundConfig(const ParsedConfig& config);
    std::string createDnsConfig();
    std::string createRoutingConfig(bool useBalancer = false);
    
    bool initialized_;
    std::string version_;
};

/**
 * @brief Embedded XRay engine implementation
 */
class XRayEngine : public EngineBase {
public:
    XRayEngine();
    ~XRayEngine() override;

    std::string generateConfig(const ParsedConfig& config, int listen_port) override;
    std::string generateBalancedConfig(const std::vector<ParsedConfig>& configs, int listen_port) override;
    
    int startProcess(const std::string& config_path) override;
    bool stopProcess(int pid) override;
    bool isProcessAlive(int pid) override;
    
    std::string getVersion() override;
    bool isAvailable() override;

private:
    void initializeXRay();
    std::string createInboundConfig(int listen_port);
    std::string createOutboundConfig(const ParsedConfig& config);
    std::string createDnsConfig();
    std::string createRoutingConfig(bool useBalancer = false);
    std::string createObservatoryConfig();
    
    bool initialized_;
    std::string version_;
};

/**
 * @brief Engine factory and manager
 */
class EngineManager {
public:
    static EngineManager& getInstance();
    
    void initialize();
    std::shared_ptr<EngineBase> getEngine(const std::string& name);
    std::vector<std::string> getAvailableEngines();
    
    // Convenience methods
    std::string generateConfig(const std::string& engine_name, const ParsedConfig& config, int listen_port);
    std::string generateBalancedConfig(const std::string& engine_name, const std::vector<ParsedConfig>& configs, int listen_port);
    int startProcess(const std::string& engine_name, const std::string& config_path);
    bool stopProcess(const std::string& engine_name, int pid);
    bool isProcessAlive(const std::string& engine_name, int pid);

private:
    EngineManager() = default;
    std::map<std::string, std::shared_ptr<EngineBase>> engines_;
    std::mutex mutex_;
    bool initialized_ = false;
};

} // namespace engines
} // namespace hunter
