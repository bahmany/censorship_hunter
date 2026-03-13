#include "network/sys_proxy.h"

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#endif

#include <string>
#include <iostream>

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
// Non-Windows stubs
bool SysProxy::set(int, int, const std::string&) { return false; }
bool SysProxy::clear() { return false; }
int SysProxy::getActivePort() { return 0; }
bool SysProxy::isEnabled() { return false; }
#endif

} // namespace network
} // namespace hunter
