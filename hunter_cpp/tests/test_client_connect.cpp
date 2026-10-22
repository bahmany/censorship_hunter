// Regression: client proxy connects through a free high port and verifies
// both socks5h:// and http:// paths reach https://www.gstatic.com/generate_204.
#include "proxy/proxy_server_manager.h"
#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

using namespace hunter;

static std::string readEnv(const std::string& name) {
    auto* p = std::getenv(name.c_str());
    return p ? std::string(p) : "";
}

static int fetch_204(CURL* curl, const std::string& proxy_url, int timeout_ms) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "https://www.gstatic.com/generate_204");
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (CURLE_OK != curl_easy_perform(curl)) {
        return -1;
    }
    long code = 0;
    if (CURLE_OK != curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code)) {
        return -1;
    }
    return static_cast<int>(code);
}

int main() {
    const auto xray_path = readEnv("HUNTER_XRAY_PATH");
    const auto uris_file = readEnv("HUNTER_TEST_CONNECT_URIS");
    if (uris_file.empty() || xray_path.empty()) {
        std::printf("SKIP: HUNTER_XRAY_PATH or HUNTER_TEST_CONNECT_URIS not set\n");
        return 0;
    }

    std::ifstream f(uris_file);
    if (!f.is_open()) {
        std::printf("FAIL: cannot open %s\n", uris_file.c_str());
        return 1;
    }
    std::string uri;
    std::vector<std::string> candidates;
    while (std::getline(f, uri)) {
        if (!uri.empty() && uri[0] != '#') {
            candidates.push_back(uri);
        }
    }
    if (candidates.empty()) {
        std::printf("FAIL: no valid URIs in %s\n", uris_file.c_str());
        return 1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::printf("FAIL: curl_easy_init failed\n");
        return 1;
    }

    proxy::ProxyServerManager mgr;
    int working_port = -1;
    bool found = false;

    for (size_t i = 0; i < candidates.size(); ++i) {
        int port = mgr.startClientProxy(candidates[i], 18808);
        if (port < 0) {
            std::printf("SKIP URI %zu: startClientProxy returned %d, error=%s\n",
                i, port, mgr.getError(candidates[i]).c_str());
            continue;
        }

        // Poll socks5h:// up to ~10 s (every 500 ms) for 204
        std::string socks_proxy = "socks5h://127.0.0.1:" + std::to_string(port) + "/";
        bool socks_ok = false;
        for (int poll = 0; poll < 20; ++poll) {
            int code = fetch_204(curl, socks_proxy, 5000);
            if (code == 204) {
                socks_ok = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!socks_ok) {
            std::printf("SKIP URI %zu: socks5h:// did not yield 204 within 10s (port=%d)\n", i, port);
            mgr.stopAll();
            mgr.waitAllStopped(5.0);
            continue;
        }

        // Test http:// path on same port
        std::string http_proxy = "http://127.0.0.1:" + std::to_string(port) + "/";
        int http_code = fetch_204(curl, http_proxy, 15000);
        if (http_code != 204) {
            std::printf("SKIP URI %zu: http:// got %d, want 204 (port=%d)\n", i, http_code, port);
            mgr.stopAll();
            mgr.waitAllStopped(5.0);
            continue;
        }

        working_port = port;
        found = true;
        std::printf("PASS URI %zu (port=%d) both socks5h and http 204\n", i, port);
        break;
    }

    if (!found) {
        std::printf("FAIL: no URI yielded 204 on both socks5h and http\n");
        curl_easy_cleanup(curl);
        return 1;
    }

    // Session switch: start a second URI on the same port must still work.
    size_t second_idx = 1 % candidates.size();
    int p2 = mgr.startClientProxy(candidates[second_idx], 18808);
    if (p2 < 0) {
        std::printf("FAIL: second startClientProxy returned %d, error=%s\n",
            p2, mgr.getError(candidates[second_idx]).c_str());
        mgr.stopAll();
        mgr.waitAllStopped(5.0);
        curl_easy_cleanup(curl);
        return 1;
    }
    if (p2 != working_port) {
        std::printf("FAIL: second proxy bound to different port %d instead of %d\n", p2, working_port);
        mgr.stopAll();
        mgr.waitAllStopped(5.0);
        curl_easy_cleanup(curl);
        return 1;
    }

    std::string socks_proxy2 = "socks5h://127.0.0.1:" + std::to_string(p2) + "/";
    bool switch_ok = false;
    for (int poll = 0; poll < 20; ++poll) {
        int code = fetch_204(curl, socks_proxy2, 5000);
        if (code == 204) {
            switch_ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!switch_ok) {
        std::printf("FAIL: session switch socks5h:// did not yield 204 within 10s (port=%d)\n", p2);
        mgr.stopAll();
        mgr.waitAllStopped(5.0);
        curl_easy_cleanup(curl);
        return 1;
    }

    // Assert stopAll() + waitAllStopped(5) returns true
    mgr.stopAll();
    bool stopped = mgr.waitAllStopped(5.0);
    if (!stopped) {
        std::printf("FAIL: waitAllStopped(5.0) returned false\n");
        curl_easy_cleanup(curl);
        return 1;
    }

    curl_easy_cleanup(curl);
    std::printf("PASS\n");
    return 0;
}
