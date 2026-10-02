// Stand-in for xray/sing-box in process-runner tests: `fake_engine run -c <cfg>`.
// Config lines: port=N  exit_after_ms=M  ignore_term=1  no_listen=1  bind_fail=1
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string cfg;
    for (int i = 1; i + 1 < argc; i++) if (std::string(argv[i]) == "-c") cfg = argv[i + 1];
    std::ifstream f(cfg);
    if (!f) return 2;
    int port = 0, exit_ms = -1; bool ign = false, nolisten = false, bindfail = false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "port") port = atoi(v.c_str());
        else if (k == "exit_after_ms") exit_ms = atoi(v.c_str());
        else if (k == "ignore_term") ign = v == "1";
        else if (k == "no_listen") nolisten = v == "1";
        else if (k == "bind_fail") bindfail = v == "1";
    }
    if (bindfail) { fprintf(stderr, "listen tcp 127.0.0.1:%d: bind: address already in use\n", port); return 1; }
    if (ign) signal(SIGTERM, SIG_IGN);
    int ls = -1;
    if (!nolisten) {
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 8) != 0) { fprintf(stderr, "bind: address already in use\n"); return 1; }
    }
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (ls >= 0) { pollfd p{ls, POLLIN, 0}; if (poll(&p, 1, 50) > 0) { int c = accept(ls, nullptr, nullptr); if (c >= 0) close(c); } }
        else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (exit_ms >= 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(exit_ms)) return 3;
    }
}
