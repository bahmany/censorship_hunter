#include "test_support.h"
#include "network/uri_parser.h"
using namespace hunter;
using namespace hunter::network;

static std::optional<ParsedConfig> P(const std::string& u) { return UriParser::parse(u); }
static std::string b64(const std::string& s) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; unsigned val = 0; int valb = -6;
    for (unsigned char c : s) { val = (val << 8) + c; valb += 8; while (valb >= 0) { o.push_back(t[(val >> valb) & 0x3F]); valb -= 6; } }
    if (valb > -6) o.push_back(t[((val << 8) >> (valb + 8)) & 0x3F]);
    while (o.size() % 4) o.push_back('=');
    return o;
}

int main() {
    const std::string U = "11111111-2222-3333-4444-555555555555";
    T_CASE("vless grpc serviceName kept and emitted");
    { auto c = P("vless://" + U + "@example.com:443?type=grpc&security=tls&serviceName=my.Svc&sni=example.com#n");
      CHECK(c && c->isValid(), "parsed"); if (c) { CHECK(c->grpcServiceName() == "my.Svc", "serviceName");
        CHECK(c->toXrayOutboundJson(0).find("\"serviceName\":\"my.Svc\"") != std::string::npos, "xray");
        CHECK(c->toSingBoxConfigJson(1080).find("\"service_name\":\"my.Svc\"") != std::string::npos, "sing-box");
        CHECK(c->options.count("serviceName") == 1, "option map retained"); } } T_END();

    T_CASE("ss with ?plugin= keeps the port intact");
    { auto c = P("ss://" + b64("aes-256-gcm:pass") + "@1.2.3.4:8388/?plugin=v2ray-plugin%3Bhost%3Dx.com#n");
      CHECK(c && c->port == 8388 && c->address == "1.2.3.4", "port 8388");
      if (c) CHECK(c->extra.count("plugin") == 1, "plugin retained");
      auto d = P("ss://" + b64("aes-256-gcm:pass@5.6.7.8:443") + "#n"); CHECK(d && d->port == 443 && d->address == "5.6.7.8", "legacy full-b64");
      auto e = P("ss://" + std::string("aes-128-gcm:pw") + "@9.9.9.9:1234#n"); (void)e;
      auto url = P("ss://" + std::string("YWVzLTEyOC1nY206cHc") + "@9.9.9.9:1234#n"); CHECK(url && url->port == 1234, "unpadded base64url userinfo"); } T_END();

    T_CASE("IPv6 hosts for each scheme");
    { auto v = P("vless://" + U + "@[2001:db8::1]:443?security=tls&sni=a.com"); CHECK(v && v->address == "2001:db8::1" && v->port == 443, "vless v6");
      auto t = P("trojan://pw@[2001:db8::2]:8443?sni=a.com"); CHECK(t && t->address == "2001:db8::2" && t->port == 8443, "trojan v6");
      auto h = P("hysteria2://pw@[2001:db8::3]:443?sni=a.com"); CHECK(h && h->address == "2001:db8::3", "hy2 v6");
      auto s = P("ss://" + b64("aes-256-gcm:pw") + "@[2001:db8::4]:8388"); CHECK(s && s->address == "2001:db8::4" && s->port == 8388, "ss v6");
      auto vm = P("vmess://" + b64("{\"v\":\"2\",\"ps\":\"x\",\"add\":\"2001:db8::5\",\"port\":\"443\",\"id\":\"" + U + "\",\"aid\":\"0\",\"net\":\"ws\",\"path\":\"/p\",\"tls\":\"tls\"}"));
      CHECK(vm && vm->address == "2001:db8::5" && vm->port == 443, "vmess v6"); } T_END();

    T_CASE("junk ports rejected");
    { for (const char* port : {"443abc", "4-43", "0", "65536", "99999", "-1", ""}) {
          auto c = P("vless://" + U + "@example.com:" + std::string(port) + "?security=tls");
          CHECK(!c || !c->isValid() || (std::string(port).empty() && c->port == 443), std::string("rejected: ") + port); } } T_END();

    T_CASE("full option map retained; insecure only when asked");
    { auto c = P("hysteria2://pw@h.example.com:443?sni=h.example.com&obfs=salamander&obfs-password=op&alpn=h3&insecure=0#n");
      CHECK(c && c->option("obfs") == "salamander" && c->option("alpn") == "h3", "options"); if (c) {
        CHECK(!c->insecureTls(), "insecure=0 is verified");
        auto j = c->toSingBoxConfigJson(1080);
        CHECK(j.find("salamander") != std::string::npos && j.find("\"insecure\"") == std::string::npos, "no hard-coded insecure");
        CHECK(j.find("utls") == std::string::npos, "no utls on QUIC"); }
      auto d = P("tuic://" + U + ":pw@t.example.com:443?sni=t.example.com&allow_insecure=1&congestion_control=cubic&udp_relay_mode=native");
      CHECK(d && d->insecureTls(), "insecure honored when asked"); if (d) { auto j = d->toSingBoxConfigJson(1080);
        CHECK(j.find("cubic") != std::string::npos && j.find("native") != std::string::npos, "tuic opts"); CHECK(j.find("\"insecure\":true") != std::string::npos, "insecure emitted"); } } T_END();

    T_CASE("hy2 certificate pin is never dropped: Unsupported regardless of insecure");
    { auto c = P("hysteria2://pw@h.example.com:443?sni=h.example.com&pinSHA256=AA:BB"); CHECK(c && c->toSingBoxConfigJson(1080).empty(), "pin => unsupported");
      auto d = P("hysteria2://pw@h.example.com:443?sni=h.example.com&pinSHA256=AA:BB&insecure=1"); CHECK(d && d->toSingBoxConfigJson(1080).empty(), "pin + insecure => still unsupported");
      CHECK(d && d->unsupportedReason("sing-box").find("pin") != std::string::npos, "reason names the pin"); } T_END();

    T_CASE("per-engine allowlist: unrepresentable options are Unsupported, never silently substituted");
    { const std::string base = "vless://" + U + "@a.example.com:443?security=tls&sni=a.example.com&type=";
      struct Row { std::string uri; bool xray, sb; };
      std::vector<Row> rows = {
          {base + "tcp", true, true}, {base + "ws&path=%2Fw", true, true}, {base + "grpc&serviceName=s", true, true}, {base + "httpupgrade&path=%2Fh", true, true},
          {base + "h2&path=%2Fh", true, true}, {base + "xhttp&path=%2Fx", true, false}, {base + "splithttp&path=%2Fx", true, false}, {base + "kcp", true, false},
          {base + "quic", true, false}, {base + "madeup", false, false}, {base + "tcp&headerType=http", false, false},
          {"ss://" + b64("aes-256-gcm:pw") + "@a.example.com:8388/?plugin=v2ray-plugin%3Bhost%3Dx", false, false},
          {"hysteria2://pw@h.example.com:443?sni=h.example.com&obfs=weird&obfs-password=x", false, false},
          {"tuic://" + U + ":pw@h.example.com:443?sni=h.example.com&congestion_control=odd", false, false},
      };
      for (auto& r : rows) { auto c = P(r.uri); CHECK(c.has_value(), "parsed " + r.uri);
          if (!c) continue;
          bool qc = c->protocol == "hysteria2" || c->protocol == "tuic";
          CHECK(c->toXrayOutboundJson(0).empty() == !r.xray, "xray gate " + r.uri);
          CHECK(c->toSingBoxConfigJson(0).empty() == !r.sb && (qc ? true : true), "sing-box gate " + r.uri); } } T_END();

        T_CASE("no direct egress in sing-box test config");
    { auto c = P("hysteria2://pw@h.example.com:443?sni=h.example.com"); auto j = c->toSingBoxConfigJson(1080);
      CHECK(j.find("\"outbound\":\"direct\"") == std::string::npos, "no rule to direct"); } T_END();

    T_CASE("injection characters rejected");
    { auto c = P("vless://" + U + "@example.com:443?type=ws&path=%22%2C%22x&security=tls"); CHECK(!c || !c->isValid(), "quote in path"); } T_END();
    return T_SUMMARY();
}
