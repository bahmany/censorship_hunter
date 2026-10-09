#include "network/sys_proxy.h"

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#endif
#endif

#include <string>
#include <iostream>
#include <algorithm>

namespace hunter {
namespace network {

#ifdef _WIN32

// ═══════════════════════════════════════════════════════════════
// WinInet-based system proxy (same approach as v2rayN SysProxyHandler.cs)
// Uses InternetSetOption with INTERNET_OPTION_PER_CONNECTION_OPTION
// for immediate, reliable proxy changes without registry hacks.
//
// Uses wide-char (W) API because the project defines UNICODE.
// ═══════════════════════════════════════════════════════════════

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring ws(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], len);
    return ws;
}

static std::string fromWide(const wchar_t* ws) {
    if (!ws || !ws[0]) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, NULL, NULL);
    return s;
}

static bool applyWinInetOptions(bool enable, const std::string& proxy_server,
                                 const std::string& bypass) {
    std::wstring w_proxy = toWide(proxy_server);
    std::wstring w_bypass = toWide(bypass);
    std::wstring w_empty = L"";

    INTERNET_PER_CONN_OPTIONW options[3] = {};
    INTERNET_PER_CONN_OPTION_LISTW list = {};

    list.dwSize = sizeof(list);
    list.pszConnection = NULL; // default/LAN connection
    list.dwOptionCount = 3;
    list.pOptions = options;

    // Option 1: Proxy flags
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[0].Value.dwValue = enable
        ? (PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY)
        : PROXY_TYPE_DIRECT;

    // Option 2: Proxy server string
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[1].Value.pszValue = enable
        ? const_cast<wchar_t*>(w_proxy.c_str())
        : const_cast<wchar_t*>(w_empty.c_str());

    // Option 3: Bypass list
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[2].Value.pszValue = enable
        ? const_cast<wchar_t*>(w_bypass.c_str())
        : const_cast<wchar_t*>(w_empty.c_str());

    DWORD listSize = sizeof(list);
    bool ok = InternetSetOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION,
                                  &list, listSize) != FALSE;

    // Notify WinInet consumers (browsers etc.) that settings changed
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);

    return ok;
}

bool SysProxy::set(int http_port, int socks_port, const std::string& bypass) {
    if (http_port <= 0 && socks_port <= 0) return false;

    // Build proxy string matching v2rayN format
    std::string proxy_str;
    if (http_port > 0 && socks_port > 0) {
        proxy_str = "http=127.0.0.1:" + std::to_string(http_port) +
                    ";https=127.0.0.1:" + std::to_string(http_port) +
                    ";socks=127.0.0.1:" + std::to_string(socks_port);
    } else if (http_port > 0) {
        proxy_str = "127.0.0.1:" + std::to_string(http_port);
    } else {
        proxy_str = "socks=127.0.0.1:" + std::to_string(socks_port);
    }

    bool ok = applyWinInetOptions(true, proxy_str, bypass);
    if (ok) {
        std::cout << "[SysProxy] SET -> " << proxy_str << std::endl;
    } else {
        std::cout << "[SysProxy] FAILED to set proxy" << std::endl;
    }
    return ok;
}

bool SysProxy::clear() {
    bool ok = applyWinInetOptions(false, "", "");
    if (ok) {
        std::cout << "[SysProxy] CLEARED" << std::endl;
    }
    return ok;
}

int SysProxy::getActivePort() {
    INTERNET_PER_CONN_OPTIONW options[2] = {};
    INTERNET_PER_CONN_OPTION_LISTW list = {};

    list.dwSize = sizeof(list);
    list.pszConnection = NULL;
    list.dwOptionCount = 2;
    list.pOptions = options;

    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;

    DWORD listSize = sizeof(list);
    if (!InternetQueryOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION,
                               &list, &listSize)) {
        return 0;
    }

    bool proxy_enabled = (options[0].Value.dwValue & PROXY_TYPE_PROXY) != 0;
    if (!proxy_enabled || options[1].Value.pszValue == NULL) {
        if (options[1].Value.pszValue) GlobalFree(options[1].Value.pszValue);
        return 0;
    }

    // Parse port from proxy server string
    std::string server_str = fromWide(options[1].Value.pszValue);
    GlobalFree(options[1].Value.pszValue);

    int port = 0;
    size_t colon = server_str.rfind(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(server_str.substr(colon + 1)); } catch (...) {}
    }
    return port;
}

bool SysProxy::isEnabled() {
    return getActivePort() > 0;
}

#else
// Non-Windows implementations
namespace {

int runCmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

std::string readCmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[256];
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return out;
}

#ifdef __APPLE__
// Iterate enabled network services, invoking fn(service_name) for each.
template <typename Fn>
void forEachNetworkService(Fn&& fn) {
    std::string out = readCmd("networksetup -listallnetworkservices 2>/dev/null");
    std::istringstream iss(out);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (first) { first = false; continue; }  // header line
        if (!line.empty() && line[0] == '*') continue;  // disabled service
        size_t b = line.find_first_not_of(" \t");
        size_t e = line.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        fn(line.substr(b, e - b + 1));
    }
}
#endif

} // namespace

bool SysProxy::set(int http_port, int socks_port, const std::string& bypass) {
    if (http_port <= 0 && socks_port <= 0) return false;
    (void)bypass;

#ifdef __linux__
    // GNOME: manual proxy mode with SOCKS (+HTTP when provided).
    bool ok = true;
    ok &= runCmd("gsettings set org.gnome.system.proxy mode 'manual' 2>/dev/null") == 0;
    const int sp = socks_port > 0 ? socks_port : http_port;
    ok &= runCmd("gsettings set org.gnome.system.proxy.socks host '127.0.0.1' 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.socks port " + std::to_string(sp) + " 2>/dev/null") == 0;
    if (http_port > 0) {
        ok &= runCmd("gsettings set org.gnome.system.proxy.http host '127.0.0.1' 2>/dev/null") == 0;
        ok &= runCmd("gsettings set org.gnome.system.proxy.http port " + std::to_string(http_port) + " 2>/dev/null") == 0;
        ok &= runCmd("gsettings set org.gnome.system.proxy.https host '127.0.0.1' 2>/dev/null") == 0;
        ok &= runCmd("gsettings set org.gnome.system.proxy.https port " + std::to_string(http_port) + " 2>/dev/null") == 0;
    }
    runCmd("gsettings set org.gnome.system.proxy ignore-hosts \"['localhost', '127.0.0.1', '::1']\" 2>/dev/null");
    return ok;

#elif defined(__APPLE__)
    bool ok = true;
    forEachNetworkService([&](const std::string& svc) {
        if (socks_port > 0) {
            ok &= runCmd("networksetup -setsocksfirewallproxy \"" + svc + "\" 127.0.0.1 " + std::to_string(socks_port) + " 2>/dev/null") == 0;
            ok &= runCmd("networksetup -setsocksfirewallproxystate \"" + svc + "\" on 2>/dev/null") == 0;
        }
        if (http_port > 0) {
            ok &= runCmd("networksetup -setwebproxy \"" + svc + "\" 127.0.0.1 " + std::to_string(http_port) + " 2>/dev/null") == 0;
            ok &= runCmd("networksetup -setwebproxystate \"" + svc + "\" on 2>/dev/null") == 0;
            ok &= runCmd("networksetup -setsecurewebproxy \"" + svc + "\" 127.0.0.1 " + std::to_string(http_port) + " 2>/dev/null") == 0;
            ok &= runCmd("networksetup -setsecurewebproxystate \"" + svc + "\" on 2>/dev/null") == 0;
        }
    });
    return ok;

#else
    return false;
#endif
}

bool SysProxy::clear() {
#ifdef __linux__
    bool ok = true;
    ok &= runCmd("gsettings set org.gnome.system.proxy mode 'none' 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.socks host '' 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.socks port 0 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.http host '' 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.http port 0 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.https host '' 2>/dev/null") == 0;
    ok &= runCmd("gsettings set org.gnome.system.proxy.https port 0 2>/dev/null") == 0;
    return ok;

#elif defined(__APPLE__)
    bool ok = true;
    forEachNetworkService([&](const std::string& svc) {
        ok &= runCmd("networksetup -setsocksfirewallproxystate \"" + svc + "\" off 2>/dev/null") == 0;
        ok &= runCmd("networksetup -setwebproxystate \"" + svc + "\" off 2>/dev/null") == 0;
        ok &= runCmd("networksetup -setsecurewebproxystate \"" + svc + "\" off 2>/dev/null") == 0;
    });
    return ok;

#else
    return false;
#endif
}

int SysProxy::getActivePort() {
#ifdef __linux__
    // Manual mode + a nonzero socks port means we (or another client) set it.
    std::string mode = readCmd("gsettings get org.gnome.system.proxy mode 2>/dev/null");
    if (mode.find("manual") == std::string::npos) return 0;
    std::string port = readCmd("gsettings get org.gnome.system.proxy.socks port 2>/dev/null");
    try {
        int p = std::stoi(port);
        return p > 0 ? p : 0;
    } catch (...) {
        return 0;
    }

#elif defined(__APPLE__)
    int port = 0;
    forEachNetworkService([&](const std::string& svc) {
        if (port > 0) return;
        std::string out = readCmd("networksetup -getsocksfirewallproxy \"" + svc + "\" 2>/dev/null");
        if (out.find("Enabled: Yes") == std::string::npos) return;
        size_t pp = out.find("Port: ");
        if (pp == std::string::npos) return;
        try {
            int p = std::stoi(out.substr(pp + 6));
            if (p > 0) port = p;
        } catch (...) {}
    });
    return port;

#else
    return 0;
#endif
}

bool SysProxy::isEnabled() {
    return getActivePort() > 0;
}
#endif

} // namespace network
} // namespace hunter
