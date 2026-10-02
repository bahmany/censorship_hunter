#pragma once
// Process launching abstraction for the tester. ProxyTester talks to EngineLauncher so unit
// tests can inject a mock runner (batch bisect, bind conflicts) without real engines.
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hunter {
namespace network {

enum class LaunchStatus {
    Ok,             // process up and every requested port is listening
    StartupFailed,  // engine exited / rejected the config / listeners never came up
    BindConflict,   // a requested port was taken by someone else (retry on new ports, no penalty)
    BinaryMissing,
    Error           // spawn failed etc.
};

struct LaunchRequest {
    std::string engine;        // "xray" | "sing-box" | "mihomo"
    std::string config_text;   // complete config document
    std::vector<int> ports;    // every local listener that must come up
    int startup_timeout_ms = 10000;
};

struct LaunchResult {
    LaunchStatus status = LaunchStatus::Error;
    std::string detail;
    std::function<bool()> alive;   // empty = unknown (treated as alive); false once the child exited
    std::shared_ptr<void> guard;   // destroying it stops the process and removes its files
};

class EngineLauncher {
public:
    virtual ~EngineLauncher() = default;
    virtual bool available(const std::string& engine) const = 0;
    virtual LaunchResult launch(const LaunchRequest& req) = 0;
};

// Real fork/exec (POSIX) or CreateProcess (Windows) launcher.
class ProcessEngineLauncher : public EngineLauncher {
public:
    ProcessEngineLauncher(std::string xray, std::string singbox, std::string mihomo)
        : xray_(std::move(xray)), singbox_(std::move(singbox)), mihomo_(std::move(mihomo)) {}
    bool available(const std::string& engine) const override;
    LaunchResult launch(const LaunchRequest& req) override;
private:
    std::string pathFor(const std::string& engine) const;
    std::string xray_, singbox_, mihomo_;
};

}  // namespace network
}  // namespace hunter
