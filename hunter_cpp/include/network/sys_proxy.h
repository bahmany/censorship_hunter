#pragma once

#include <string>

namespace hunter {
namespace network {

/**
 * @brief Windows system proxy management using WinInet API
 * 
 * Sets/clears the system-wide proxy (Internet Settings) using
 * the proper WinInet InternetSetOption API, same approach as v2rayN's
 * SysProxyHandler.cs but in native C++.
 * 
 * This is superior to registry-only changes because:
 * 1. WinInet immediately picks up changes (no need for rundll32 hack)
 * 2. Per-connection settings work correctly
 * 3. Properly notifies all WinInet consumers (browsers, etc.)
 */
class SysProxy {
public:
    /**
     * @brief Set system HTTP/SOCKS proxy
     * @param http_port HTTP proxy port (0 = skip)
     * @param socks_port SOCKS proxy port (0 = skip)
     * @param bypass Bypass list (default: local addresses)
     * @return true on success
     */
    static bool set(int http_port, int socks_port = 0,
                    const std::string& bypass = "localhost;127.*;10.*;192.168.*;172.16.*;<local>");

    /**
     * @brief Clear system proxy (disable)
     * @return true on success
     */
    static bool clear();

    /**
     * @brief Get current system proxy port
     * @return Active proxy port, or 0 if proxy is disabled
     */
    static int getActivePort();

    /**
     * @brief Check if system proxy is currently enabled
     */
    static bool isEnabled();
};

} // namespace network
} // namespace hunter
