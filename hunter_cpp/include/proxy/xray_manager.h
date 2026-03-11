#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

#include "core/models.h"

namespace hunter {
namespace proxy {

/**
 * @brief XRay subprocess manager
 * 
 * Manages XRay binary execution for proxy benchmarking and
 * load balancer backend processes.
 */
class XRayManager {
public:
    XRayManager();
    ~XRayManager();

    XRayManager(const XRayManager&) = delete;
    XRayManager& operator=(const XRayManager&) = delete;

    /**
     * @brief Set path to XRay binary
     */
    void setXRayPath(const std::string& path) { xray_path_ = path; }

    /**
     * @brief Get XRay binary path
     */
    const std::string& xrayPath() const { return xray_path_; }

    /**
     * @brief Check if XRay binary exists and is executable
     */
    bool isAvailable() const;

    /**
     * @brief Start an XRay process with a config file
     * @param config_path Path to XRay JSON config
     * @return Process ID, or -1 on failure
     */
    int startProcess(const std::string& config_path);

    /**
     * @brief Stop an XRay process
     * @param pid Process ID to stop
     */
    void stopProcess(int pid);

    /**
     * @brief Stop all managed processes
     */
    void stopAll();

    /**
     * @brief Check if a process is still running
     */
    bool isProcessAlive(int pid) const;

    /**
     * @brief Generate XRay JSON config for a single outbound (dual-protocol: SOCKS + HTTP)
     * @param parsed Parsed proxy config
     * @param socks_port Local SOCKS port to bind
     * @param http_port Local HTTP port to bind (0 = no HTTP inbound)
     * @return JSON config string
     */
    static std::string generateConfig(const ParsedConfig& parsed, int socks_port, int http_port = 0);

    /**
     * @brief Generate XRay balanced config with multiple outbounds (dual-protocol: SOCKS + HTTP)
     * @param configs Parsed configs with their SOCKS ports
     * @param listen_port Main SOCKS listener port
     * @param http_port HTTP listener port (0 = no HTTP inbound)
     * @return JSON config string
     */
    static std::string generateBalancedConfig(
        const std::vector<std::pair<ParsedConfig, int>>& configs,
        int listen_port, int http_port = 0);

    /**
     * @brief Write config to temp file and return path
     */
    std::string writeConfigFile(const std::string& json_config);

private:
    std::string xray_path_;
    std::vector<int> managed_pids_;
    mutable std::mutex mutex_;
    std::string temp_dir_;

    void ensureTempDir();
};

} // namespace proxy
} // namespace hunter
