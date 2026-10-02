#include "core/endpoint_key.h"
#include "core/utils.h"
#include "test_support.h"

using namespace hunter;

static const char* U1 = "11111111-2222-3333-4444-555555555555";
static const char* U1_UP = "11111111-2222-3333-4444-555555555555";

static std::string K(const std::string& u) { return computeEndpointKey(u).key; }

int main() {
    std::cout << "=== EndpointKeyV1 ===\n";

    T_CASE("sha256 known vectors");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "empty");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "abc");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", "1000a");
    T_END();

    T_CASE("key format and determinism");
    auto ek = computeEndpointKey(std::string("vless://") + U1 + "@1.2.3.4:443?type=ws#a");
    CHECK(ek.valid, "valid");
    CHECK(ek.key.compare(0, 4, "ek1:") == 0 && ek.key.size() == 4 + 64, "ek1:<64 hex>");
    CHECK(ek.key == K(std::string("vless://") + U1 + "@1.2.3.4:443?type=ws#a"), "deterministic");
    CHECK(ek.protocol == "vless" && ek.host == "1.2.3.4" && ek.port == 443, "fields");
    T_END();

    T_CASE("IP alone is never the identity");
    const std::string base = std::string("vless://") + U1 + "@1.2.3.4:443";
    CHECK(K(base) != K("vless://99999999-2222-3333-4444-555555555555@1.2.3.4:443"), "different uuid");
    CHECK(K(base) != K(std::string("vless://") + U1 + "@1.2.3.4:8443"), "different port");
    CHECK(K("trojan://pw@1.2.3.4:443") != K("hysteria2://pw@1.2.3.4:443"), "different protocol");
    CHECK(K(base) != K(std::string("vless://") + U1 + "@1.2.3.5:443"), "different ip");
    CHECK(K(base) != K(base + "?type=grpc"), "different transport");
    CHECK(K(base + "?security=tls&sni=a.com") != K(base + "?security=tls&sni=b.com"), "different sni");
    CHECK(K(base + "?path=/a") != K(base + "?path=/A"), "path is case-sensitive");
    CHECK(K(base + "?security=reality&pbk=AbC&sid=1") != K(base + "?security=reality&pbk=abc&sid=1"), "reality key case");
    T_END();

    T_CASE("fragment, query order and documented defaults do not matter");
    CHECK(K(base + "#one") == K(base + "#two"), "fragment ignored");
    CHECK(K(base + "?type=ws&path=/x&sni=a.com") == K(base + "?sni=a.com&path=/x&type=ws"), "query order");
    CHECK(K(base) == K(base + "?type=tcp"), "default transport");
    CHECK(K(base) == K(base + "?encryption=none"), "default vless encryption");
    CHECK(K(base) == K(base + "?remark=zzz"), "remark query option ignored");
    T_END();

    T_CASE("unknown query options are preserved conservatively");
    CHECK(K(base + "?alpn=h2") != K(base), "alpn retained");
    CHECK(K(base + "?foo=1") != K(base + "?foo=2"), "unknown option value retained");
    CHECK(K("hy2://pw@1.2.3.4:443?obfs=salamander&obfs-password=x") != K("hy2://pw@1.2.3.4:443?obfs=salamander&obfs-password=y"), "hy2 obfs password");
    CHECK(K("hy2://pw@1.2.3.4:443?insecure=1") != K("hy2://pw@1.2.3.4:443"), "insecure policy");
    CHECK(K("hy2://pw@1.2.3.4:443?insecure=1") == K("hy2://pw@1.2.3.4:443?allowInsecure=true"), "insecure spellings");
    CHECK(K("hy2://pw@1.2.3.4:443?insecure=0") == K("hy2://pw@1.2.3.4:443"), "insecure=0 is default");
    CHECK(computeEndpointKey("hy2://pw@1.2.3.4:443?insecure=1").insecure_tls, "insecure flag surfaced");
    T_END();

    T_CASE("protocol aliases");
    CHECK(canonicalProtocol("HY2") == "hysteria2" && canonicalProtocol("hysteria2") == "hysteria2", "hy2");
    CHECK(canonicalProtocol("ss") == "shadowsocks" && canonicalProtocol("SS") == "shadowsocks", "ss");
    CHECK(K("hy2://pw@host.example:443#a") == K("hysteria2://pw@host.example:443#b"), "hy2 == hysteria2");
    CHECK(K("hy2://pw@host.example") == K("hysteria2://pw@host.example:443"), "hy2 default port 443");
    CHECK(computeEndpointKey("hy2://pw@host.example:443").protocol == "hysteria2", "canonical protocol");
    T_END();

    T_CASE("hostnames: ASCII case and trailing dot; no resolution");
    CHECK(K("trojan://pw@Example.COM:443") == K("trojan://pw@example.com:443"), "case");
    CHECK(K("trojan://pw@example.com.:443") == K("trojan://pw@example.com:443"), "trailing dot");
    CHECK(K("trojan://pw@example.com:443") != K("trojan://pw@example.org:443"), "different host");
    std::string h;
    CHECK(canonicalHost("EXAMPLE.com.", &h) && h == "example.com", "canonicalHost");
    CHECK(!canonicalHost("", &h) && !canonicalHost("a b", &h) && !canonicalHost("fe80::1%eth0", &h), "rejects");
    T_END();

    T_CASE("IPv6 canonical text");
    CHECK(canonicalHost("[2001:DB8:0:0:0:0:0:1]", &h) && h == "2001:db8::1", "compress+lower");
    CHECK(canonicalHost("2001:0db8:0000:0000:0001:0000:0000:0001", &h) && h == "2001:db8::1:0:0:1", "longest zero run, first on tie");
    CHECK(canonicalHost("::", &h) && h == "::", "all zero");
    CHECK(canonicalHost("::1", &h) && h == "::1", "loopback");
    CHECK(canonicalHost("1:0:3:4:5:6:7:8", &h) && h == "1:0:3:4:5:6:7:8", "single zero group not compressed");
    CHECK(!canonicalHost("12345::1", &h) && !canonicalHost("1:2:3", &h), "malformed");
    const std::string v6a = std::string("vless://") + U1 + "@[2001:DB8:0:0:0:0:0:1]:443";
    const std::string v6b = std::string("vless://") + U1 + "@[2001:db8::1]:443";
    CHECK(K(v6a) == K(v6b), "vless IPv6 variants equal");
    CHECK(K(v6a) != K(std::string("vless://") + U1 + "@[2001:db8::2]:443"), "different v6");
    CHECK(computeEndpointKey(v6a).valid, "v6 valid");
    T_END();

    T_CASE("credential identity: UUID folds case, passwords do not");
    const std::string upper = "AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE";
    const std::string lower = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    CHECK(K("vless://" + upper + "@1.2.3.4:443") == K("vless://" + lower + "@1.2.3.4:443"), "vless uuid case");
    CHECK(K("trojan://PassWord@1.2.3.4:443") != K("trojan://password@1.2.3.4:443"), "trojan password case");
    CHECK(K("hy2://AuthKey@1.2.3.4:443") != K("hy2://authkey@1.2.3.4:443"), "hy2 auth case");
    auto ss = [](const std::string& ui, const char* hp) { return "ss://" + utils::base64Encode(ui) + "@" + hp; };
    CHECK(computeEndpointKey(ss("aes-256-gcm:PwD", "1.2.3.4:8388")).valid, "ss base64 userinfo parses");
    CHECK(K(ss("aes-256-gcm:PwD", "1.2.3.4:8388")) != K(ss("aes-256-gcm:pwd", "1.2.3.4:8388")), "ss password case");
    CHECK(K(ss("aes-256-gcm:pw", "1.2.3.4:8388#x")) == K(ss("AES-256-GCM:pw", "1.2.3.4:8388#y")), "ss cipher name case + remark");
    CHECK(K(ss("aes-256-gcm:pw", "1.2.3.4:8388")) != K(ss("chacha20-ietf-poly1305:pw", "1.2.3.4:8388")), "ss cipher is identity");
    CHECK(computeEndpointKey(ss("aes-256-gcm:pw", "1.2.3.4:8388")).protocol == "shadowsocks", "ss -> shadowsocks");
    CHECK(K("tuic://" + upper + ":pw@1.2.3.4:443") == K("tuic://" + lower + ":pw@1.2.3.4:443"), "tuic uuid case");
    CHECK(K("tuic://" + lower + ":Pw@1.2.3.4:443") != K("tuic://" + lower + ":pw@1.2.3.4:443"), "tuic password case");
    (void)U1_UP;
    T_END();

    T_CASE("ambiguous/unparseable URIs get an exact-byte fallback key");
    auto g1 = computeEndpointKey("Garbage");
    auto g2 = computeEndpointKey("garbage");
    CHECK(!g1.valid && !g2.valid, "invalid");
    CHECK(g1.key != g2.key, "fallback is not lowercased");
    CHECK(g1.key.compare(0, 4, "ek1:") == 0, "still ek1 key");
    CHECK(computeEndpointKey("vless://@1.2.3.4:443").valid == false, "vless needs a credential (isValid)");
    CHECK(computeEndpointKey("trojan://pw@1.2.3.4:99999").valid == false, "port range");
    (void)computeEndpointKey("trojan://pw@fe80::1%25eth0:443");  // must not crash
    CHECK(g1.canonical != computeEndpointKey("trojan://pw@1.2.3.4:443").canonical, "domains separated");
    T_END();

    T_CASE("length-prefixed canonical form is collision-safe");
    CHECK(K("trojan://a@host:443?x=1&y=2") != K("trojan://a@host:443?x=1%262") , "delimiter inside value");
    CHECK(K("trojan://ab@host:443") != K("trojan://a@bhost:443"), "boundary shift");
    T_END();

    return T_SUMMARY();
}
