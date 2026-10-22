#include "network/sys_proxy.h"

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#ifdef _MSC_VER
#pragma comment(lib, "wininet.lib")
#endif
#endif

#include "core/utils.h"
#include <string>
#include <iostream>
#include <algorithm>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdio>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace hunter {
namespace network {

using namespace utils;

static const std::string BACKUP_FILE = "runtime/sysproxy_backup.json";

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char hex[7];
            std::snprintf(hex, sizeof(hex), "\\u%04x", (int)c);
            out += hex;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

static std::string unescapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == '\\') { out += '\\'; i++; }
            else if (next == '"') { out += '"'; i++; }
            else if (next == 'n') { out += '\n'; i++; }
            else if (next == 'r') { out += '\r'; i++; }
            else if (next == 't') { out += '\t'; i++; }
            else if (next == 'u' && i + 5 < s.size()) {
                std::string hex = s.substr(i + 2, 4);
                try {
                    int val = std::stoi(hex, nullptr, 16);
                    out += static_cast<char>(val);
                    i += 5;
                } catch (...) {
                    out += '\\';
                }
            } else {
                out += next;
                i++;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

static bool validateStringAllowed(const std::string& s) {
    for (char ch : s) {
        if (std::isalnum(static_cast<unsigned char>(ch)) ||
            ch == '.' || ch == '*' || ch == ':' || ch == ';' ||
            ch == '<' || ch == '>' || ch == '_' || ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool hasBackup() {
    return fileExists(BACKUP_FILE);
}

static bool atomicWriteFile(const std::string& file, const std::string& content) {
    std::string tmp = file + ".tmp";
    try {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(content.c_str(), static_cast<std::streamsize>(content.size()));
        f.close();
        if (f.fail()) return false;
#ifdef _WIN32
        if (!MoveFileExA(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
#else
        if (std::rename(tmp.c_str(), file.c_str()) != 0) return false;
#endif
        return true;
    } catch (...) {
        return false;
    }
}

static size_t findClosingQuote(const std::string& s, size_t start) {
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            i++; // skip escaped character
        } else if (s[i] == '"') {
            return i;
        }
    }
    return std::string::npos;
}

static std::map<std::string, std::string> parseSimpleJson(const std::string& s) {
    std::map<std::string, std::string> result;
    size_t pos = 0;
    while (true) {
        size_t key_quote = s.find('"', pos);
        if (key_quote == std::string::npos) break;
        size_t key_end = findClosingQuote(s, key_quote + 1);
        if (key_end == std::string::npos) break;
        std::string key = unescapeJsonString(s.substr(key_quote + 1, key_end - key_quote - 1));
        size_t colon = s.find(':', key_end);
        if (colon == std::string::npos) break;
        size_t val_start = colon + 1;
        while (val_start < s.size() && (s[val_start] == ' ' || s[val_start] == '\t')) ++val_start;
        if (val_start < s.size() && s[val_start] == '"') {
            size_t val_end = findClosingQuote(s, val_start + 1);
            if (val_end == std::string::npos) break;
            result[key] = unescapeJsonString(s.substr(val_start + 1, val_end - val_start - 1));
            pos = val_end + 1;
        } else {
            size_t val_end = s.find_first_of(",}", val_start);
            if (val_end == std::string::npos) val_end = s.size();
            std::string val = s.substr(val_start, val_end - val_start);
            size_t trim_start = val.find_first_not_of(" \t\r\n");
            size_t trim_end = val.find_last_not_of(" \t\r\n");
            if (trim_start != std::string::npos) val = val.substr(trim_start, trim_end - trim_start + 1);
            else val = "";
            result[key] = val;
            pos = val_end;
        }
    }
    return result;
}

static std::string buildSimpleJson(const std::map<std::string, std::string>& m) {
    std::string out = "{";
    bool first = true;
    for (const auto& kv : m) {
        if (!first) out += ",";
        first = false;
        std::string escaped_key = escapeJsonString(kv.first);
        std::string escaped_val = escapeJsonString(kv.second);
        out += "\"" + escaped_key + "\":\"" + escaped_val + "\"";
    }
    out += "}";
    return out;
}

#ifdef _WIN32

static std::string readWindowsRegistryString(const char* valueName) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) return "";
    char buf[1024] = {};
    DWORD size = sizeof(buf);
    DWORD type = REG_SZ;
    RegQueryValueExA(hKey, valueName, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(hKey);
    return std::string(buf);
}

static int readWindowsRegistryDword(const char* valueName) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_READ, &hKey) != ERROR_SUCCESS) return 0;
    DWORD val = 0;
    DWORD size = sizeof(val);
    DWORD type = REG_DWORD;
    RegQueryValueExA(hKey, valueName, NULL, &type, (LPBYTE)&val, &size);
    RegCloseKey(hKey);
    return static_cast<int>(val);
}

static bool applyWindowsRegistry(bool enable, const std::string& proxy_server, const std::string& bypass) {
    HKEY hKey;
    LONG res = RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_WRITE, &hKey);
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] applyWindowsRegistry: RegOpenKeyExA failed with " << res << std::endl;
        return false;
    }

    DWORD dwEnable = enable ? 1 : 0;
    res = RegSetValueExA(hKey, "ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] applyWindowsRegistry: RegSetValueExA ProxyEnable failed with " << res << std::endl;
        RegCloseKey(hKey);
        return false;
    }

    if (!proxy_server.empty()) {
        res = RegSetValueExA(hKey, "ProxyServer", 0, REG_SZ, (const BYTE*)proxy_server.c_str(), static_cast<DWORD>(proxy_server.size() + 1));
        if (res != ERROR_SUCCESS) {
            std::cerr << "[SysProxy] applyWindowsRegistry: RegSetValueExA ProxyServer failed with " << res << std::endl;
            RegCloseKey(hKey);
            return false;
        }
    }

    if (!bypass.empty()) {
        res = RegSetValueExA(hKey, "ProxyOverride", 0, REG_SZ, (const BYTE*)bypass.c_str(), static_cast<DWORD>(bypass.size() + 1));
        if (res != ERROR_SUCCESS) {
            std::cerr << "[SysProxy] applyWindowsRegistry: RegSetValueExA ProxyOverride failed with " << res << std::endl;
            RegCloseKey(hKey);
            return false;
        }
    }

    RegCloseKey(hKey);
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
    return true;
}

static bool saveWindowsBackup() {
    std::map<std::string, std::string> data;
    data["proxy_enable"] = std::to_string(readWindowsRegistryDword("ProxyEnable"));
    data["proxy_server"] = readWindowsRegistryString("ProxyServer");
    data["proxy_override"] = readWindowsRegistryString("ProxyOverride");
    std::string json = buildSimpleJson(data);
    return atomicWriteFile(BACKUP_FILE, json);
}

static bool restoreWindowsBackup() {
    std::string content = loadJsonFile(BACKUP_FILE);
    std::map<std::string, std::string> data = parseSimpleJson(content);

    int enable = 0;
    try { enable = std::stoi(data["proxy_enable"]); } catch (...) {}
    std::string server = data["proxy_server"];
    std::string override = data["proxy_override"];

    HKEY hKey;
    LONG res = RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
        0, KEY_WRITE, &hKey);
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] restoreWindowsBackup: RegOpenKeyExA failed with " << res << std::endl;
        return false;
    }

    DWORD dwEnable = enable ? 1 : 0;
    res = RegSetValueExA(hKey, "ProxyEnable", 0, REG_DWORD, (const BYTE*)&dwEnable, sizeof(dwEnable));
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] restoreWindowsBackup: RegSetValueExA ProxyEnable failed with " << res << std::endl;
        RegCloseKey(hKey);
        return false;
    }
    res = RegSetValueExA(hKey, "ProxyServer", 0, REG_SZ, (const BYTE*)server.c_str(), static_cast<DWORD>(server.size() + 1));
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] restoreWindowsBackup: RegSetValueExA ProxyServer failed with " << res << std::endl;
        RegCloseKey(hKey);
        return false;
    }
    res = RegSetValueExA(hKey, "ProxyOverride", 0, REG_SZ, (const BYTE*)override.c_str(), static_cast<DWORD>(override.size() + 1));
    if (res != ERROR_SUCCESS) {
        std::cerr << "[SysProxy] restoreWindowsBackup: RegSetValueExA ProxyOverride failed with " << res << std::endl;
        RegCloseKey(hKey);
        return false;
    }

    RegCloseKey(hKey);
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
    return true;
}

bool SysProxy::set(int http_port, int socks_port, const std::string& bypass) {
    if (http_port <= 0 && socks_port <= 0) return false;
    if (!bypass.empty() && !validateStringAllowed(bypass)) return false;

    if (!hasBackup()) {
        saveWindowsBackup();
    }

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

    bool ok = applyWindowsRegistry(true, proxy_str, bypass);
    if (ok) {
        std::cout << "[SysProxy] SET -> " << proxy_str << std::endl;
    } else {
        std::cout << "[SysProxy] FAILED to set proxy" << std::endl;
    }
    return ok;
}

bool SysProxy::clear() {
    if (hasBackup()) {
        bool ok = restoreWindowsBackup();
        std::remove(BACKUP_FILE.c_str());
        if (ok) {
            std::cout << "[SysProxy] RESTORED from backup" << std::endl;
        } else {
            std::cout << "[SysProxy] FAILED to restore backup" << std::endl;
        }
        return ok;
    }
    bool ok = applyWindowsRegistry(false, "", "");
    if (ok) {
        std::cout << "[SysProxy] CLEARED" << std::endl;
    }
    return ok;
}

int SysProxy::getActivePort() {
    int enabled = readWindowsRegistryDword("ProxyEnable");
    if (enabled == 0) return 0;
    std::string server = readWindowsRegistryString("ProxyServer");
    int port = 0;
    size_t colon = server.rfind(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(server.substr(colon + 1)); } catch (...) {}
    }
    return port;
}

bool SysProxy::isEnabled() {
    return getActivePort() > 0;
}

void SysProxy::recoverStale() {
    if (hasBackup()) {
        restoreWindowsBackup();
        std::remove(BACKUP_FILE.c_str());
    }
}

#else

namespace {

int runCmd(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        if (devnull >= 0) close(devnull);
        return -1;
    }
    if (pid == 0) {
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    int status = 0;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) == -1 && errno == EINTR) {
        // retry on EINTR
    }
    if (w < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string readCmd(const std::vector<std::string>& args) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return "";

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        if (devnull >= 0) close(devnull);
        return "";
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (devnull >= 0) close(devnull);
    close(pipefd[1]);
    std::string out;
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        out += buf;
    }
    close(pipefd[0]);
    int status = 0;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) == -1 && errno == EINTR) {
        // retry on EINTR
    }
    return out;
}

#ifdef __APPLE__
template <typename Fn>
void forEachNetworkService(Fn&& fn) {
    std::string out = readCmd({"networksetup", "-listallnetworkservices"});
    std::istringstream iss(out);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (first) { first = false; continue; }
        if (!line.empty() && line[0] == '*') continue;
        size_t b = line.find_first_not_of(" \t");
        size_t e = line.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        std::string svc = line.substr(b, e - b + 1);
        if (!svc.empty() && validateStringAllowed(svc)) {
            fn(svc);
        }
    }
}
#endif

} // namespace

static std::map<std::string, std::string> readGnomeBackupData() {
    std::map<std::string, std::string> data;
    std::string mode = readCmd({"gsettings", "get", "org.gnome.system.proxy", "mode"});
    data["mode"] = mode;
    data["socks_host"] = readCmd({"gsettings", "get", "org.gnome.system.proxy.socks", "host"});
    std::string socks_port_str = readCmd({"gsettings", "get", "org.gnome.system.proxy.socks", "port"});
    data["socks_port"] = socks_port_str;
    data["http_host"] = readCmd({"gsettings", "get", "org.gnome.system.proxy.http", "host"});
    std::string http_port_str = readCmd({"gsettings", "get", "org.gnome.system.proxy.http", "port"});
    data["http_port"] = http_port_str;
    data["https_host"] = readCmd({"gsettings", "get", "org.gnome.system.proxy.https", "host"});
    std::string https_port_str = readCmd({"gsettings", "get", "org.gnome.system.proxy.https", "port"});
    data["https_port"] = https_port_str;
    data["ignore_hosts"] = readCmd({"gsettings", "get", "org.gnome.system.proxy", "ignore-hosts"});
    return data;
}

static bool saveLinuxBackup() {
    std::map<std::string, std::string> data = readGnomeBackupData();
    std::string json = buildSimpleJson(data);
    return atomicWriteFile(BACKUP_FILE, json);
}

static bool restoreLinuxBackup() {
    std::string content = loadJsonFile(BACKUP_FILE);
    std::map<std::string, std::string> data = parseSimpleJson(content);
    if (data.find("mode") != data.end()) {
        std::string val = data["mode"];
        // Trim quotes if present
        if (!val.empty() && val.front() == '\'' && val.back() == '\'') val = val.substr(1, val.size() - 2);
        runCmd({"gsettings", "set", "org.gnome.system.proxy", "mode", val});
    }
    if (data.find("socks_host") != data.end()) {
        std::string val = data["socks_host"];
        if (!val.empty() && val.front() == '\'' && val.back() == '\'') val = val.substr(1, val.size() - 2);
        else val = val.empty() ? "''" : val;
        runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "host", val});
    }
    if (data.find("socks_port") != data.end()) {
        std::string val = data["socks_port"];
        runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "port", val});
    }
    if (data.find("http_host") != data.end()) {
        std::string val = data["http_host"];
        if (!val.empty() && val.front() == '\'' && val.back() == '\'') val = val.substr(1, val.size() - 2);
        else val = val.empty() ? "''" : val;
        runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "host", val});
    }
    if (data.find("http_port") != data.end()) {
        std::string val = data["http_port"];
        runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "port", val});
    }
    if (data.find("https_host") != data.end()) {
        std::string val = data["https_host"];
        if (!val.empty() && val.front() == '\'' && val.back() == '\'') val = val.substr(1, val.size() - 2);
        else val = val.empty() ? "''" : val;
        runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "host", val});
    }
    if (data.find("https_port") != data.end()) {
        std::string val = data["https_port"];
        runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "port", val});
    }
    if (data.find("ignore_hosts") != data.end()) {
        std::string val = data["ignore_hosts"];
        runCmd({"gsettings", "set", "org.gnome.system.proxy", "ignore-hosts", val});
    }
    return true;
}

#ifdef __APPLE__
static std::map<std::string, std::string> readMacBackupData(const std::vector<std::string>& services) {
    std::map<std::string, std::string> data;
    for (const std::string& svc : services) {
        std::string socks_proxy = readCmd({"networksetup", "-getsocksfirewallproxy", svc});
        std::string socks_state = readCmd({"networksetup", "-getsocksfirewallproxystate", svc});
        std::string web_proxy = readCmd({"networksetup", "-getwebproxy", svc});
        std::string web_state = readCmd({"networksetup", "-getwebproxystate", svc});
        std::string secure_web_proxy = readCmd({"networksetup", "-getsecurewebproxy", svc});
        std::string secure_web_state = readCmd({"networksetup", "-getsecurewebproxystate", svc});
        data["svc_" + svc + "_socks"] = socks_proxy;
        data["svc_" + svc + "_socks_state"] = socks_state;
        data["svc_" + svc + "_web"] = web_proxy;
        data["svc_" + svc + "_web_state"] = web_state;
        data["svc_" + svc + "_secure"] = secure_web_proxy;
        data["svc_" + svc + "_secure_state"] = secure_web_state;
    }
    return data;
}

static bool saveMacBackup() {
    std::vector<std::string> services;
    forEachNetworkService([&](const std::string& svc) { services.push_back(svc); });
    std::map<std::string, std::string> data = readMacBackupData(services);
    data["services"] = std::to_string(services.size());
    std::string json = buildSimpleJson(data);
    return atomicWriteFile(BACKUP_FILE, json);
}

static bool restoreMacBackup() {
    std::string content = loadJsonFile(BACKUP_FILE);
    std::map<std::string, std::string> data = parseSimpleJson(content);
    std::vector<std::string> services;
    forEachNetworkService([&](const std::string& svc) { services.push_back(svc); });
    for (const std::string& svc : services) {
        std::string key_base = "svc_" + svc + "_";
        std::string socks_key = key_base + "socks";
        if (data.find(socks_key) != data.end()) {
            std::string line = data[socks_key];
            std::string host = "127.0.0.1";
            int port = 0;
            size_t pp = line.find("Port: ");
            if (pp != std::string::npos) {
                try { port = std::stoi(line.substr(pp + 6)); } catch (...) {}
            }
            std::string socks_state_key = key_base + "socks_state";
            bool socks_on = (data.find(socks_state_key) != data.end()) ?
                (data[socks_state_key].find("Yes") != std::string::npos) : false;
            if (port > 0 && socks_on) {
                runCmd({"networksetup", "-setsocksfirewallproxy", svc, host, std::to_string(port)});
                runCmd({"networksetup", "-setsocksfirewallproxystate", svc, "on"});
            } else {
                runCmd({"networksetup", "-setsocksfirewallproxystate", svc, "off"});
            }
        }
        std::string web_key = key_base + "web";
        if (data.find(web_key) != data.end()) {
            std::string line = data[web_key];
            std::string host = "127.0.0.1";
            int port = 0;
            size_t pp = line.find("Port: ");
            if (pp != std::string::npos) {
                try { port = std::stoi(line.substr(pp + 6)); } catch (...) {}
            }
            std::string web_state_key = key_base + "web_state";
            bool web_on = (data.find(web_state_key) != data.end()) ?
                (data[web_state_key].find("Yes") != std::string::npos) : false;
            if (port > 0 && web_on) {
                runCmd({"networksetup", "-setwebproxy", svc, host, std::to_string(port)});
                runCmd({"networksetup", "-setwebproxystate", svc, "on"});
            } else {
                runCmd({"networksetup", "-setwebproxystate", svc, "off"});
            }
        }
        std::string secure_key = key_base + "secure";
        if (data.find(secure_key) != data.end()) {
            std::string line = data[secure_key];
            std::string host = "127.0.0.1";
            int port = 0;
            size_t pp = line.find("Port: ");
            if (pp != std::string::npos) {
                try { port = std::stoi(line.substr(pp + 6)); } catch (...) {}
            }
            std::string secure_state_key = key_base + "secure_state";
            bool secure_on = (data.find(secure_state_key) != data.end()) ?
                (data[secure_state_key].find("Yes") != std::string::npos) : false;
            if (port > 0 && secure_on) {
                runCmd({"networksetup", "-setsecurewebproxy", svc, host, std::to_string(port)});
                runCmd({"networksetup", "-setsecurewebproxystate", svc, "on"});
            } else {
                runCmd({"networksetup", "-setsecurewebproxystate", svc, "off"});
            }
        }
    }
    return true;
}
#endif // __APPLE__

bool SysProxy::set(int http_port, int socks_port, const std::string& bypass) {
    if (http_port <= 0 && socks_port <= 0) return false;
    (void)bypass;

#ifdef __linux__
    if (!bypass.empty() && !validateStringAllowed(bypass)) return false;
    if (!hasBackup()) {
        saveLinuxBackup();
    }
    bool ok = true;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy", "mode", "manual"}) == 0;
    const int sp = socks_port > 0 ? socks_port : http_port;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "host", "127.0.0.1"}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "port", std::to_string(sp)}) == 0;
    if (http_port > 0) {
        ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "host", "127.0.0.1"}) == 0;
        ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "port", std::to_string(http_port)}) == 0;
        ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "host", "127.0.0.1"}) == 0;
        ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "port", std::to_string(http_port)}) == 0;
    }
    runCmd({"gsettings", "set", "org.gnome.system.proxy", "ignore-hosts", "['localhost', '127.0.0.1', '::1']"});
    return ok;

#elif defined(__APPLE__)
    if (!bypass.empty() && !validateStringAllowed(bypass)) return false;
    bool ok = true;
    std::vector<std::string> services;
    forEachNetworkService([&](const std::string& svc) { services.push_back(svc); });
    for (const std::string& svc : services) {
        if (socks_port > 0) {
            ok &= runCmd({"networksetup", "-setsocksfirewallproxy", svc, "127.0.0.1", std::to_string(socks_port)}) == 0;
            ok &= runCmd({"networksetup", "-setsocksfirewallproxystate", svc, "on"}) == 0;
        }
        if (http_port > 0) {
            ok &= runCmd({"networksetup", "-setwebproxy", svc, "127.0.0.1", std::to_string(http_port)}) == 0;
            ok &= runCmd({"networksetup", "-setwebproxystate", svc, "on"}) == 0;
            ok &= runCmd({"networksetup", "-setsecurewebproxy", svc, "127.0.0.1", std::to_string(http_port)}) == 0;
            ok &= runCmd({"networksetup", "-setsecurewebproxystate", svc, "on"}) == 0;
        }
    }
    if (!hasBackup()) {
        saveMacBackup();
    }
    return ok;

#else
    return false;
#endif
}

bool SysProxy::clear() {
#ifdef __linux__
    if (hasBackup()) {
        bool ok = restoreLinuxBackup();
        std::remove(BACKUP_FILE.c_str());
        return ok;
    }
    bool ok = true;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy", "mode", "none"}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "host", ""}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.socks", "port", "0"}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "host", ""}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.http", "port", "0"}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "host", ""}) == 0;
    ok &= runCmd({"gsettings", "set", "org.gnome.system.proxy.https", "port", "0"}) == 0;
    return ok;
#elif defined(__APPLE__)
    if (hasBackup()) {
        bool ok = restoreMacBackup();
        std::remove(BACKUP_FILE.c_str());
        return ok;
    }
    bool ok = true;
    std::vector<std::string> services;
    forEachNetworkService([&](const std::string& svc) { services.push_back(svc); });
    for (const std::string& svc : services) {
        ok &= runCmd({"networksetup", "-setsocksfirewallproxystate", svc, "off"}) == 0;
        ok &= runCmd({"networksetup", "-setwebproxystate", svc, "off"}) == 0;
        ok &= runCmd({"networksetup", "-setsecurewebproxystate", svc, "off"}) == 0;
    }
    return ok;
#else
    return false;
#endif
}

int SysProxy::getActivePort() {
#ifdef __linux__
    std::string mode = readCmd({"gsettings", "get", "org.gnome.system.proxy", "mode"});
    if (mode.find("manual") == std::string::npos) return 0;
    std::string port_str = readCmd({"gsettings", "get", "org.gnome.system.proxy.socks", "port"});
    try {
        int p = std::stoi(port_str);
        return p > 0 ? p : 0;
    } catch (...) {
        return 0;
    }
#elif defined(__APPLE__)
    int port = 0;
    std::vector<std::string> services;
    forEachNetworkService([&](const std::string& svc) { services.push_back(svc); });
    for (const std::string& svc : services) {
        if (port > 0) break;
        std::string out = readCmd({"networksetup", "-getsocksfirewallproxy", svc});
        if (out.find("Enabled: Yes") == std::string::npos) continue;
        size_t pp = out.find("Port: ");
        if (pp == std::string::npos) continue;
        try {
            int p = std::stoi(out.substr(pp + 6));
            if (p > 0) port = p;
        } catch (...) {}
    }
    return port;
#else
    return 0;
#endif
}

bool SysProxy::isEnabled() {
    return getActivePort() > 0;
}

void SysProxy::recoverStale() {
    if (hasBackup()) {
#ifdef _WIN32
        restoreWindowsBackup();
#else
        std::map<std::string, std::string> content_map;
        std::string content = loadJsonFile(BACKUP_FILE);
        // Try to detect platform from content
        if (content.find("mode") != std::string::npos) {
            restoreLinuxBackup();
        } else if (content.find("proxy_enable") != std::string::npos) {
#ifdef _WIN32
            restoreWindowsBackup();
#endif
        } else {
#ifdef __APPLE__
            restoreMacBackup();
#endif
        }
#endif
        std::remove(BACKUP_FILE.c_str());
    }
}

#endif

} // namespace network
} // namespace hunter
