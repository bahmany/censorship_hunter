#pragma once
// Long-lived engine process launcher for the user-facing proxy (Stage 4 B).
//
//  * POSIX: fork/exec, children are always reaped through waitpid (a dead child is never
//    reported alive, no zombies), PR_SET_PDEATHSIG(SIGKILL) so engines die with the GUI, SIGTERM then
//    SIGKILL only while the pid is still owned (not yet reaped => no pid-reuse hazard).
//  * Windows: every engine is assigned to one process-wide Job object created with
//    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (engines die with the GUI), process/thread handles are
//    always closed, only the log handle is inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
//  * Config files are created with owner-only permissions and removed with the process guard.
// Implements the same EngineLauncher interface as network::ProcessEngineLauncher so the watchdog
// can be tested with fakes.
#include <memory>
#include <string>

#include "network/engine_launcher.h"

namespace hunter {
namespace proxy {

class ManagedEngineLauncher : public network::EngineLauncher {
public:
    ManagedEngineLauncher(std::string xray, std::string singbox, std::string mihomo = "")
        : xray_(std::move(xray)), singbox_(std::move(singbox)), mihomo_(std::move(mihomo)) {}
    bool available(const std::string& engine) const override;
    network::LaunchResult launch(const network::LaunchRequest& req) override;

    /// OS process id behind a guard returned by launch() (0 when unknown / already released).
    static int pidOf(const std::shared_ptr<void>& guard);
    /// Grace period between SIGTERM/TerminateProcess request and forced kill (default 1500 ms).
    void setKillGraceMs(int ms) { kill_grace_ms_ = ms; }

private:
    std::string pathFor(const std::string& engine) const;
    std::string xray_, singbox_, mihomo_;
    int kill_grace_ms_ = 1500;
};

}  // namespace proxy
}  // namespace hunter
