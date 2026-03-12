#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "core/models.h"

namespace hunter {
namespace proxy {

class RuntimeEngineManager {
public:
    RuntimeEngineManager();
    ~RuntimeEngineManager();

    RuntimeEngineManager(const RuntimeEngineManager&) = delete;
    RuntimeEngineManager& operator=(const RuntimeEngineManager&) = delete;

    void setXRayPath(const std::string& path) { xray_path_ = path; }
    void setSingBoxPath(const std::string& path) { singbox_path_ = path; }
    void setMihomoPath(const std::string& path) { mihomo_path_ = path; }

    const std::string& xrayPath() const { return xray_path_; }
    const std::string& singBoxPath() const { return singbox_path_; }
    const std::string& mihomoPath() const { return mihomo_path_; }

    bool isEngineAvailable(const std::string& engine) const;
    std::string resolveEngine(const ParsedConfig& parsed, const std::string& preferred = "") const;
    std::string generateConfig(const ParsedConfig& parsed, int listen_port, const std::string& engine) const;
    std::string writeConfigFile(const std::string& engine, const std::string& config_text);
    int startProcess(const std::string& engine, const std::string& config_path);
    void stopProcess(int pid);
    void stopAll();
    bool isProcessAlive(int pid) const;

private:
    std::string xray_path_;
    std::string singbox_path_;
    std::string mihomo_path_;
    std::string temp_dir_;
    mutable std::mutex mutex_;
    std::vector<int> managed_pids_;

    void ensureTempDir();
    std::string normalizedEngine(const std::string& engine) const;
    std::string engineBinaryPath(const std::string& engine) const;
    std::string configExtensionForEngine(const std::string& engine) const;
};

} // namespace proxy
} // namespace hunter
