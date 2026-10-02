// ProcessEngineLauncher against a fake engine script (no real proxy engine needed).
// Regression for review blocker #5: process exit after startup must be observable via LaunchResult::alive.
#include "test_support.h"
#include "network/engine_launcher.h"
#include "network/port_lease.h"
#include <fstream>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
using namespace hunter::network;

static std::string writeEngine(const std::string& name, const std::string& body) {
    std::string p = "/tmp/" + name + "_" + std::to_string(getpid());
    std::ofstream(p) << body; chmod(p.c_str(), 0755); return p;
}

int main() {
    if (access("/usr/bin/python3", X_OK) != 0 && access("/bin/python3", X_OK) != 0) { std::cout << "SKIP: python3 missing" << std::endl; return 0; }
    // Fake engine: parse port from the -c JSON, listen, exit(42) after `lifetime` seconds (argv: run -c cfg).
    auto script = [](const char* lifetime) { return std::string("#!/usr/bin/env python3\nimport json,socket,sys,time,os\n"
        "cfg=json.load(open(sys.argv[sys.argv.index('-c')+1]))\n"
        "ls=[]\nfor ib in cfg['inbounds']:\n s=socket.socket();s.bind(('127.0.0.1',ib['port']));s.listen(8);ls.append(s)\n"
        "time.sleep(") + lifetime + ");os._exit(42)\n"; };
    T_CASE("alive() reports false after the child exits post-startup; true while running");
    { auto reg = PortLeaseRegistry::create(); auto lease = reg->acquire(); int port = lease.port(); lease.releaseSocket();
      std::string eng = writeEngine("a2_fake_engine", script("1.5"));
      ProcessEngineLauncher pl(eng, "", "");
      LaunchRequest rq; rq.engine = "xray"; rq.ports = {port}; rq.startup_timeout_ms = 8000;
      rq.config_text = "{\"inbounds\":[{\"port\":" + std::to_string(port) + "}]}";
      auto lr = pl.launch(rq);
      CHECK(lr.status == LaunchStatus::Ok, "fake engine up: " + lr.detail);
      CHECK(lr.alive != nullptr, "alive hook provided");
      if (lr.alive) { CHECK(lr.alive(), "alive right after startup"); std::this_thread::sleep_for(std::chrono::milliseconds(2200)); CHECK(!lr.alive(), "dead after exit(42)"); }
      unlink(eng.c_str()); } T_END();

    T_CASE("engine that exits at startup => StartupFailed");
    { std::string eng = writeEngine("a2_bad_engine", "#!/bin/sh\necho 'bad config' >&2\nexit 1\n");
      ProcessEngineLauncher pl(eng, "", ""); LaunchRequest rq; rq.engine = "xray"; rq.ports = {45999}; rq.config_text = "{}"; rq.startup_timeout_ms = 3000;
      auto lr = pl.launch(rq); CHECK(lr.status == LaunchStatus::StartupFailed, "startup failed"); unlink(eng.c_str()); } T_END();
    return T_SUMMARY();
}
