#include "proxy/process_runner.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "core/utils.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace hunter {
namespace proxy {

using network::LaunchRequest;
using network::LaunchResult;
using network::LaunchStatus;

namespace {

std::mutex g_pid_mu;
std::map<const void*, int>& pidMap() {
    static std::map<const void*, int> m;
    return m;
}

std::string artifactPath(const std::string& prefix, int port, const char* ext) {
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

bool writePrivateFile(const std::string& path, const std::string& text) {
#ifdef _WIN32
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return (bool)f;
#else
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < text.size()) {
        ssize_t n = write(fd, text.data() + off, text.size() - off);
        if (n <= 0) { if (errno == EINTR) continue; close(fd); return false; }
        off += (size_t)n;
    }
    return close(fd) == 0;
#endif
}

#ifdef _WIN32
HANDLE killOnCloseJob() {
    static HANDLE job = []() -> HANDLE {
        HANDLE j = CreateJobObjectA(nullptr, nullptr);
        if (!j) return nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
            CloseHandle(j);
            return nullptr;
        }
        return j;   // intentionally never closed: it must live until this process exits
    }();
    return job;
}
#endif

struct ManagedProcess {
    std::string cfg, log;
    int grace_ms = 1500;
    std::mutex mu;
    bool reaped = false;
#ifdef _WIN32
    HANDLE proc = nullptr;
    DWORD pid = 0;
#else
    pid_t pid = -1;
#endif

    int osPid() const { return (int)pid; }

    // True while the child runs. Reaps the child when it has exited (no zombies).
    bool alive() {
        std::lock_guard<std::mutex> lk(mu);
        return aliveLocked();
    }

    bool aliveLocked() {
        if (reaped) return false;
#ifdef _WIN32
        DWORD code = 0;
        if (!proc || !GetExitCodeProcess(proc, &code) || code != STILL_ACTIVE) { releaseLocked(); return false; }
        return true;
#else
        if (pid <= 0) { reaped = true; return false; }
        int st = 0;
        pid_t w;
        do { w = waitpid(pid, &st, WNOHANG); } while (w < 0 && errno == EINTR);
        if (w == 0) return true;
        reaped = true;   // exited (reaped now) or already gone (ECHILD)
        return false;
#endif
    }

    void releaseLocked() {
        reaped = true;
#ifdef _WIN32
        if (proc) { CloseHandle(proc); proc = nullptr; }
#endif
    }

    // Stop the child and wait until it is gone (bounded). Idempotent.
    void terminate() {
        std::lock_guard<std::mutex> lk(mu);
        if (reaped) return;
#ifdef _WIN32
        if (proc) {
            TerminateProcess(proc, 1);
            WaitForSingleObject(proc, (DWORD)std::max(grace_ms, 2000));
        }
        releaseLocked();
#else
        if (pid > 0) {
            kill(pid, SIGTERM);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(grace_ms);
            bool gone = false;
            while (true) {
                int st = 0;
                pid_t w = waitpid(pid, &st, WNOHANG);
                if (w == pid || (w < 0 && errno != EINTR)) { gone = true; break; }
                if (std::chrono::steady_clock::now() >= deadline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!gone) {
                kill(pid, SIGKILL);   // safe: pid not reaped yet, so it cannot have been recycled
                auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < d2) {
                    int st = 0;
                    pid_t w = waitpid(pid, &st, WNOHANG);
                    if (w == pid || (w < 0 && errno != EINTR)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
        reaped = true;
#endif
    }

    ~ManagedProcess() {
        terminate();
        {
            std::lock_guard<std::mutex> lk(g_pid_mu);
            pidMap().erase(this);
        }
        std::remove(cfg.c_str());
        std::remove(log.c_str());
    }
};

bool allListening(const std::vector<int>& ports) {
    for (int p : ports) if (!utils::isPortAlive(p, 100)) return false;
    return true;
}

}  // namespace

std::string ManagedEngineLauncher::pathFor(const std::string& engine) const {
    if (engine == "xray") return xray_;
    if (engine == "sing-box") return singbox_;
    if (engine == "mihomo") return mihomo_;
    return "";
}

bool ManagedEngineLauncher::available(const std::string& engine) const {
    std::string p = pathFor(engine);
    return !p.empty() && utils::fileExists(p);
}

int ManagedEngineLauncher::pidOf(const std::shared_ptr<void>& guard) {
    if (!guard) return 0;
    std::lock_guard<std::mutex> lk(g_pid_mu);
    auto it = pidMap().find(guard.get());
    return it == pidMap().end() ? 0 : it->second;
}

LaunchResult ManagedEngineLauncher::launch(const LaunchRequest& req) {
    LaunchResult out;
    const std::string bin = pathFor(req.engine);
    if (bin.empty() || !utils::fileExists(bin)) {
        out.status = LaunchStatus::BinaryMissing;
        out.detail = req.engine + " binary not found";
        return out;
    }
    try { utils::mkdirRecursive("runtime"); } catch (...) {}
    const int tag_port = req.ports.empty() ? 0 : req.ports.front();
    const bool mihomo = req.engine == "mihomo";
    auto proc = std::make_shared<ManagedProcess>();
    proc->grace_ms = kill_grace_ms_;
    proc->cfg = artifactPath("proxy_server_" + req.engine, tag_port, mihomo ? ".yaml" : ".json");
    proc->log = artifactPath("proxy_server_" + req.engine + "_out", tag_port, ".txt");
    if (!writePrivateFile(proc->cfg, req.config_text)) {
        out.status = LaunchStatus::Error;
        out.detail = "cannot write config";
        return out;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hOut = CreateFileA(proc->log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOEXA si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdOutput = hOut;
    si.StartupInfo.hStdError = hOut;
    si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    // Restrict handle inheritance to the log handle only (no leaked sockets/files in the child).
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    bool attr_ok = hOut != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
    HANDLE inherit[1] = {hOut};
    if (attr_ok) attr_ok = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr) != 0;
    PROCESS_INFORMATION pi = {};
    std::string cmd = "\"" + bin + "\" " + (mihomo ? "-f" : "run -c") + " \"" + proc->cfg + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (attr_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL ok = CreateProcessA(NULL, buf.data(), NULL, NULL, TRUE, flags, NULL, NULL,
                             attr_ok ? &si.StartupInfo : &si.StartupInfo, &pi);
    if (attr_ok) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (!ok) {
        out.status = LaunchStatus::Error;
        out.detail = "CreateProcess failed";
        return out;
    }
    HANDLE job = killOnCloseJob();
    if (!job || !AssignProcessToJobObject(job, pi.hProcess)) {
        // Without the job the child could outlive the GUI: refuse to run it.
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        out.status = LaunchStatus::Error;
        out.detail = "cannot assign engine to job object";
        return out;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);   // never needed again: no handle leak
    proc->proc = pi.hProcess;
    proc->pid = pi.dwProcessId;
#else
    std::vector<std::string> args;
    args.push_back(req.engine);
    if (mihomo) args.push_back("-f"); else { args.push_back("run"); args.push_back("-c"); }
    args.push_back(proc->cfg);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    int logfd = open(proc->log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);   // engine dies with the GUI
        if (getppid() != parent) _exit(127);
#endif
        if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); }
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) dup2(dn, STDIN_FILENO);
        execv(bin.c_str(), argv.data());
        _exit(127);
    }
    if (logfd >= 0) close(logfd);
    if (pid < 0) {
        out.status = LaunchStatus::Error;
        out.detail = "fork failed";
        return out;
    }
    proc->pid = pid;
#endif
    {
        std::lock_guard<std::mutex> lk(g_pid_mu);
        pidMap()[proc.get()] = proc->osPid();
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(req.startup_timeout_ms);
    while (true) {
        if (!proc->alive()) {
            std::string log = readTail(proc->log);
            if (looksLikeBindConflict(log)) { out.status = LaunchStatus::BindConflict; out.detail = "bind conflict"; }
            else { out.status = LaunchStatus::StartupFailed; out.detail = "engine exited at startup: " + log.substr(0, 300); }
            return out;   // proc destructor reaps/cleans files
        }
        if (allListening(req.ports)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            std::string log = readTail(proc->log);
            out.status = looksLikeBindConflict(log) ? LaunchStatus::BindConflict : LaunchStatus::StartupFailed;
            out.detail = "listeners did not come up: " + log.substr(0, 200);
            return out;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // A listener that appeared while the child is already gone is somebody else's: not ours.
    if (!proc->alive()) {
        out.status = LaunchStatus::StartupFailed;
        out.detail = "engine exited right after startup";
        return out;
    }
    out.status = LaunchStatus::Ok;
    out.guard = proc;
    std::weak_ptr<ManagedProcess> wp = proc;
    out.alive = [wp]() { auto p = wp.lock(); return p && p->alive(); };
    return out;
}

}  // namespace proxy
}  // namespace hunter
