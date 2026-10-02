#include "network/engine_launcher.h"
#include <cctype>
#include <vector>
#include "core/utils.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace hunter {
namespace network {

namespace {

std::string artifactPath(const char* prefix, int port, const char* ext) {
    static std::atomic<uint32_t> nonce{0};
#ifdef _WIN32
    const unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    const unsigned long pid = (unsigned long)getpid();
#endif
    std::ostringstream o;
    o << "runtime/" << prefix << "_" << port << "_" << std::hex << utils::nowMs() << "_" << pid << "_"
      << nonce.fetch_add(1) << ext;
    return o.str();
}

std::string readTail(const std::string& path, size_t max_bytes = 4096) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.size() > max_bytes) s = s.substr(s.size() - max_bytes);
    return s;
}

bool looksLikeBindConflict(const std::string& log) {
    std::string low = log;
    for (auto& c : low) c = (char)std::tolower((unsigned char)c);
    return low.find("address already in use") != std::string::npos ||
           low.find("only one usage of each socket address") != std::string::npos ||
           (low.find("bind: ") != std::string::npos && low.find("in use") != std::string::npos);
}

bool keepArtifacts() {
    const char* e = std::getenv("HUNTER_KEEP_FAILED_XRAY_CONFIG");
    return e && *e == '1';
}

struct ProcessGuard {
#ifdef _WIN32
    HANDLE proc = nullptr;
    HANDLE thread = nullptr;
#else
    int pid = -1;
#endif
    std::string cfg, log;
    std::atomic<bool> dead{false};
    // True while the child process is still running (detects crashes after startup).
    bool alive() {
        if (dead.load()) return false;
#ifdef _WIN32
        DWORD code = 0;
        if (!proc || !GetExitCodeProcess(proc, &code) || code != STILL_ACTIVE) { dead = true; return false; }
        return true;
#else
        if (pid <= 0) { dead = true; return false; }
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { pid = -1; dead = true; return false; }   // reaped: destructor will not kill
        if (w == -1) { dead = true; return false; }
        return true;
#endif
    }
    ~ProcessGuard() {
#ifdef _WIN32
        if (proc) { TerminateProcess(proc, 0); WaitForSingleObject(proc, 3000); CloseHandle(proc); }
        if (thread) CloseHandle(thread);
#else
        if (pid > 0) utils::killAndWait(pid);
#endif
        if (!keepArtifacts()) { std::remove(cfg.c_str()); std::remove(log.c_str()); }
    }
};

bool allListening(const std::vector<int>& ports) {
    for (int p : ports) if (!utils::isPortAlive(p, 100)) return false;
    return true;
}

}  // namespace

std::string ProcessEngineLauncher::pathFor(const std::string& engine) const {
    if (engine == "xray") return xray_;
    if (engine == "sing-box") return singbox_;
    if (engine == "mihomo") return mihomo_;
    return "";
}

bool ProcessEngineLauncher::available(const std::string& engine) const {
    std::string p = pathFor(engine);
    return !p.empty() && utils::fileExists(p);
}

LaunchResult ProcessEngineLauncher::launch(const LaunchRequest& req) {
    LaunchResult out;
    const std::string bin = pathFor(req.engine);
    if (bin.empty() || !utils::fileExists(bin)) {
        out.status = LaunchStatus::BinaryMissing;
        out.detail = req.engine + " binary not found";
        return out;
    }
    const int tag_port = req.ports.empty() ? 0 : req.ports.front();
    const char* ext = req.engine == "mihomo" ? ".yaml" : ".json";
    auto guard = std::make_shared<ProcessGuard>();
    guard->cfg = artifactPath(("tmp_" + req.engine).c_str(), tag_port, ext);
    guard->log = artifactPath(("tmp_" + req.engine + "_out").c_str(), tag_port, ".txt");
    if (!utils::saveJsonFile(guard->cfg, req.config_text)) {
        out.status = LaunchStatus::Error;
        out.detail = "cannot write config";
        return out;
    }
    const bool mihomo = req.engine == "mihomo";

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileA(guard->log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + bin + "\" " + (mihomo ? "-f" : "run -c") + " \"" + guard->cfg + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    BOOL ok = CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (!ok) {
        out.status = LaunchStatus::Error;
        out.detail = "CreateProcess failed";
        return out;
    }
    guard->proc = pi.hProcess;
    guard->thread = pi.hThread;
    auto exited = [&]() {
        DWORD code = 0;
        return GetExitCodeProcess(pi.hProcess, &code) && code != STILL_ACTIVE;
    };
    int exit_code_127 = 0;
#else
    std::vector<std::string> args;
    args.push_back(req.engine);
    if (mihomo) args.push_back("-f"); else { args.push_back("run"); args.push_back("-c"); }
    args.push_back(guard->cfg);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int logfd = open(guard->log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = fork();
    if (pid == 0) {
        if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); close(logfd); }
        execv(bin.c_str(), argv.data());
        _exit(127);
    }
    if (logfd >= 0) close(logfd);
    if (pid < 0) {
        out.status = LaunchStatus::Error;
        out.detail = "fork failed";
        return out;
    }
    guard->pid = pid;
    int exit_code_127 = 0;
    auto exited = [&]() {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            guard->pid = -1;   // reaped
            if (WIFEXITED(st) && WEXITSTATUS(st) == 127) exit_code_127 = 1;
            return true;
        }
        return w == -1;
    };
#endif

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(req.startup_timeout_ms);
    while (true) {
        if (exited()) {
            std::string log = readTail(guard->log);
            if (exit_code_127) { out.status = LaunchStatus::BinaryMissing; out.detail = "exec failed"; }
            else if (looksLikeBindConflict(log)) { out.status = LaunchStatus::BindConflict; out.detail = "bind conflict"; }
            else { out.status = LaunchStatus::StartupFailed; out.detail = log.substr(0, 300); }
            return out;   // guard cleans files
        }
        if (allListening(req.ports)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::string log = readTail(guard->log);
            out.status = looksLikeBindConflict(log) ? LaunchStatus::BindConflict : LaunchStatus::StartupFailed;
            out.detail = "listeners did not come up: " + log.substr(0, 200);
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    out.status = LaunchStatus::Ok;
    out.guard = guard;
    std::weak_ptr<ProcessGuard> wg = guard;
    out.alive = [wg]() { auto g = wg.lock(); return g && g->alive(); };
    return out;
}

}  // namespace network
}  // namespace hunter
