#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <functional>
#include <sstream>
#include <iomanip>
#include <algorithm>

// Switch vendor detection
enum class SwitchVendor { UNKNOWN, HUAWEI_NETENGINE, ZTE_ZXR10, CISCO_CATALYST, NOKIA_SRS, DORAN_DPI, YAFTAAR_DPI, HP_PROLIANT };

struct SwitchInfo {
    SwitchVendor vendor;
    std::string model;
    std::string description;
    bool is_dpi;
};

// Bypass method signatures
class SwitchBypassMethods {
public:
    static bool bypassHuaweiSRv6(const std::string& ip, const std::string& gw, DWORD if_index);
    static bool bypassZTEHyperConverged(const std::string& ip);
    static bool bypassCiscoNetFlow(const std::string& ip);
    static bool bypassNokiaSR(const std::string& ip);
    static bool bypassDoranWhitelist(const std::string& ip, const std::string& gw, DWORD if_index);
    static bool bypassYaftaarStarlink(const std::string& ip);
    static bool bypassHPMSSClamping(const std::string& ip, const std::string& gw, DWORD if_index);
    static bool bypassHuaweiAI(const std::string& ip);
    static bool bypassZTESRv6(const std::string& ip);
    static bool bypassCiscoACL(const std::string& ip, const std::string& gw, DWORD if_index);
    static bool bypassNokiaLSP(const std::string& ip);
    static bool bypassIranianDNSPoison(const std::string& ip);
    static bool bypassTieredInternet(const std::string& ip);
    static bool bypassSafeSearch(const std::string& ip);
    static bool bypassStealthBlackout(const std::string& ip);
};
