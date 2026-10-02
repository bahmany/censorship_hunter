// Stage 4 B: ManagedEngineLauncher with a real child (tests/fake_engine): reaping, zombies,
// SIGTERM-ignoring child, bind conflicts, config file permissions. Loopback only.
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

#include "network/port_lease.h"
#include "proxy/process_runner.h"
#include "test_support.h"

using namespace hunter;
using namespace hunter::proxy;

static bool pidGone(int pid) { return kill(pid, 0) == -1 && errno == ESRCH; }
static bool zombie(int pid) {
    FILE* f = fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
    if (!f) return false;
    char buf[512]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    const char* rp = strrchr(buf, ')');
    return rp && rp[2] == 'Z';
}
static int countRuntimeFiles(const std::string& needle) {
    int n = 0;
    if (DIR* d = opendir("runtime")) {
        while (auto* e = readdir(d)) if (std::string(e->d_name).find(needle) != std::string::npos) n++;
        closedir(d);
    }
    return n;
}

int main() {
    std::cout << "test_process_runner" << std::endl;
    auto reg = network::PortLeaseRegistry::create();
    ManagedEngineLauncher L(HUNTER_FAKE_ENGINE_PATH, HUNTER_FAKE_ENGINE_PATH, "");
    L.setKillGraceMs(500);
    auto req = [&](int port, const std::string& extra, int timeout = 5000) {
        network::LaunchRequest r; r.engine = "xray"; r.ports = {port}; r.startup_timeout_ms = timeout;
        r.config_text = "port=" + std::to_string(port) + "\n" + extra + "\n"; return r;
    };

    T_CASE("launch -> listening, alive, pid known; config is owner-only; guard destroy kills + reaps + removes files");
    {
        auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
        auto lr = L.launch(req(port, ""));
        CHECK(lr.status == network::LaunchStatus::Ok, "ok: " + lr.detail);
        int pid = ManagedEngineLauncher::pidOf(lr.guard);
        CHECK(pid > 0 && lr.alive && lr.alive(), "alive with pid");
        bool perm_ok = false; int found = 0;
        if (DIR* d = opendir("runtime")) {
            while (auto* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.find("proxy_server_xray_" + std::to_string(port) + "_") == 0 && n.find("_out") == std::string::npos) {
                    struct stat st{}; if (stat(("runtime/" + n).c_str(), &st) == 0) { found++; perm_ok = (st.st_mode & 077) == 0; }
                }
            }
            closedir(d);
        }
        CHECK(found == 1 && perm_ok, "config file exists with 0600");
        lr.guard.reset(); lr.alive = nullptr;
        CHECK(pidGone(pid), "child killed AND reaped (no zombie)");
        CHECK(countRuntimeFiles("proxy_server_xray_" + std::to_string(port) + "_") == 0, "config + log files removed");
        CHECK(ManagedEngineLauncher::pidOf(lr.guard) == 0, "pid registry cleaned");
    }
    T_END();

    T_CASE("child exits by itself: alive() turns false and reaps it (zombie detection)");
    {
        auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
        auto lr = L.launch(req(port, "exit_after_ms=300"));
        CHECK(lr.status == network::LaunchStatus::Ok, "ok");
        int pid = ManagedEngineLauncher::pidOf(lr.guard);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(zombie(pid), "precondition: exited child is a zombie until waited (kill(pid,0) would still say alive)");
        CHECK(kill(pid, 0) == 0, "kill(pid,0) is fooled by the zombie");
        CHECK(!lr.alive(), "alive() says dead");
        CHECK(pidGone(pid) && !zombie(pid), "zombie reaped by alive()");
        CHECK(!lr.alive(), "stays dead");
        lr.guard.reset();
    }
    T_END();

    T_CASE("SIGTERM-ignoring engine is force-killed within the grace period");
    {
        auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
        auto lr = L.launch(req(port, "ignore_term=1"));
        CHECK(lr.status == network::LaunchStatus::Ok, "ok");
        int pid = ManagedEngineLauncher::pidOf(lr.guard);
        auto t0 = std::chrono::steady_clock::now();
        lr.guard.reset();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(pidGone(pid) && ms < 3000, "killed + reaped in " + std::to_string((int)ms) + " ms");
    }
    T_END();

    T_CASE("startup failures: never listens -> StartupFailed (child reaped); bind error -> BindConflict; missing binary");
    {
        auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
        auto lr = L.launch(req(port, "no_listen=1", 600));
        CHECK(lr.status == network::LaunchStatus::StartupFailed && !lr.guard, "StartupFailed");
        CHECK(countRuntimeFiles("proxy_server_xray_" + std::to_string(port) + "_") == 0, "files cleaned after failure");
        auto lr2 = L.launch(req(port, "bind_fail=1"));
        CHECK(lr2.status == network::LaunchStatus::BindConflict, "BindConflict (zero-penalty signal)");
        ManagedEngineLauncher bad("/nonexistent/xray", "", "");
        CHECK(!bad.available("xray") && bad.launch(req(port, "")).status == network::LaunchStatus::BinaryMissing, "BinaryMissing");
    }
    T_END();

    T_CASE("many launch/destroy cycles leave no children or descriptors behind");
    {
        auto count_fds = [] { int n = 0; if (DIR* d = opendir("/proc/self/fd")) { while (readdir(d)) n++; closedir(d); } return n; };
        int fd0 = count_fds();
        for (int i = 0; i < 8; i++) {
            auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
            auto lr = L.launch(req(port, i % 2 ? "exit_after_ms=100" : ""));
            CHECK(lr.status == network::LaunchStatus::Ok, "launch " + std::to_string(i));
            if (i % 2) std::this_thread::sleep_for(std::chrono::milliseconds(300));
            lr.guard.reset();
        }
        CHECK(count_fds() <= fd0 + 1, "no fd leak: " + std::to_string(fd0) + " -> " + std::to_string(count_fds()));
        CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD, "no unreaped children");
    }
    T_END();

    return T_SUMMARY();
}
