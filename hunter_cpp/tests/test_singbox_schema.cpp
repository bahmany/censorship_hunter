// Opt-in (HUNTER_INTEGRATION_TESTS=ON, label integration): every protocol/transport/TLS variant of
// ParsedConfig::toSingBoxConfigJson must pass `sing-box check` of the pinned binary (HUNTER_SINGBOX_PATH).
// Exit 77 (skipped) when the binary is unavailable.
#include "test_support.h"
#include "network/uri_parser.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
using namespace hunter;
using namespace hunter::network;

static std::string b64(const std::string& s) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; unsigned val = 0; int valb = -6;
    for (unsigned char c : s) { val = (val << 8) + c; valb += 8; while (valb >= 0) { o.push_back(t[(val >> valb) & 0x3F]); valb -= 6; } }
    if (valb > -6) o.push_back(t[((val << 8) >> (valb + 8)) & 0x3F]);
    while (o.size() % 4) o.push_back('=');
    return o;
}
static std::string vmess(const std::string& net, const std::string& tls, const std::string& extra = "") {
    return "vmess://" + b64("{\"v\":\"2\",\"ps\":\"t\",\"add\":\"vm.example.com\",\"port\":\"443\",\"id\":\"11111111-2222-3333-4444-555555555555\",\"aid\":\"0\",\"scy\":\"auto\",\"net\":\"" +
                            net + "\",\"type\":\"none\",\"host\":\"vm.example.com\",\"path\":\"/p\",\"tls\":\"" + tls + "\",\"sni\":\"vm.example.com\"" + extra + "}");
}

int main() {
    const char* sb = std::getenv("HUNTER_SINGBOX_PATH");
    if (!sb || access(sb, X_OK) != 0) { std::cout << "SKIP: set HUNTER_SINGBOX_PATH to the pinned sing-box" << std::endl; return 77; }
    const std::string U = "11111111-2222-3333-4444-555555555555";
    const std::string S = "&sni=a.example.com";
    std::vector<std::pair<std::string, std::string>> cases = {
        {"vless tcp tls", "vless://" + U + "@a.example.com:443?security=tls&type=tcp" + S + "&fp=chrome"},
        {"vless tcp none", "vless://" + U + "@a.example.com:80?security=none&type=tcp"},
        {"vless reality vision", "vless://" + U + "@a.example.com:443?security=reality&type=tcp&flow=xtls-rprx-vision&pbk=" + std::string(43, 'A') + "&sid=abcd1234&fp=chrome" + S},
        {"vless ws tls", "vless://" + U + "@a.example.com:443?security=tls&type=ws&path=%2Fws&host=a.example.com" + S},
        {"vless grpc tls", "vless://" + U + "@a.example.com:443?security=tls&type=grpc&serviceName=svc" + S},
        {"vless httpupgrade tls", "vless://" + U + "@a.example.com:443?security=tls&type=httpupgrade&path=%2Fhu&host=a.example.com" + S},
        {"vless h2 tls", "vless://" + U + "@a.example.com:443?security=tls&type=h2&path=%2Fh" + S},
        {"vless insecure tls", "vless://" + U + "@a.example.com:443?security=tls&type=tcp&allowInsecure=1" + S},
        {"vless ipv6", "vless://" + U + "@[2001:db8::1]:443?security=tls&type=tcp" + S},
        {"vless alpn", "vless://" + U + "@a.example.com:443?security=tls&type=tcp&alpn=h2,http/1.1" + S},
        {"vmess tcp", vmess("tcp", "")}, {"vmess ws tls", vmess("ws", "tls")}, {"vmess grpc tls", vmess("grpc", "tls")},
        {"vmess httpupgrade tls", vmess("httpupgrade", "tls")}, {"vmess ws tls insecure", vmess("ws", "tls", ",\"allowInsecure\":\"1\"")},
        {"trojan tls", "trojan://pw@a.example.com:443?security=tls&type=tcp" + S},
        {"trojan ws tls", "trojan://pw@a.example.com:443?security=tls&type=ws&path=%2Fw&host=a.example.com" + S},
        {"trojan grpc tls", "trojan://pw@a.example.com:443?security=tls&type=grpc&serviceName=g" + S},
        {"trojan insecure", "trojan://pw@a.example.com:443?security=tls&allowInsecure=1" + S},
        {"ss plain", "ss://" + b64("aes-256-gcm:pw") + "@a.example.com:8388#n"},
        {"ss chacha", "ss://" + b64("chacha20-ietf-poly1305:pw") + "@1.2.3.4:8388#n"},
        {"hy2 basic", "hysteria2://pw@a.example.com:443?sni=a.example.com"},
        {"hy2 obfs", "hysteria2://pw@a.example.com:443?sni=a.example.com&obfs=salamander&obfs-password=op"},
        {"hy2 pin alpn", "hysteria2://pw@a.example.com:443?sni=a.example.com&pinSHA256=AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99&alpn=h3&insecure=1"},
        {"hy2 insecure", "hysteria2://pw@a.example.com:443?sni=a.example.com&insecure=1"},
        {"hy2 ipv6", "hysteria2://pw@[2001:db8::2]:443?sni=a.example.com"},
        {"tuic basic", "tuic://" + U + ":pw@a.example.com:443?sni=a.example.com&alpn=h3"},
        {"tuic insecure cc", "tuic://" + U + ":pw@a.example.com:443?sni=a.example.com&allow_insecure=1&congestion_control=cubic&udp_relay_mode=native"},
    };
    int n = 0;
    for (auto& [name, uri] : cases) {
        T_CASE("sing-box check: " + name);
        auto c = UriParser::parse(uri);
        CHECK(c && c->isValid(), "parsed");
        if (c && c->isValid()) {
            std::string cfg = c->toSingBoxConfigJson(31000 + n);
            CHECK(!cfg.empty(), "generated");
            std::string path = "/tmp/a2_sbcheck_" + std::to_string(getpid()) + "_" + std::to_string(n++) + ".json";
            std::ofstream(path) << cfg;
            std::string cmd = std::string(sb) + " check -c " + path + " > " + path + ".log 2>&1";
            int rc = std::system(cmd.c_str());
            if (rc != 0) { std::ifstream l(path + ".log"); std::string line, all; while (std::getline(l, line)) all += line + " | "; CHECK(false, name + ": " + all.substr(0, 400)); }
            else { std::remove(path.c_str()); std::remove((path + ".log").c_str()); }
        }
        T_END();
    }
    return T_SUMMARY();
}
