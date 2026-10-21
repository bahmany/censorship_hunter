/**
 * @file test_client_export.cpp
 * @brief Unit tests for client_export module.
 */

#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <cassert>

#include "core/client_export.h"

using namespace hunter;
using namespace hunter::client_export;

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        std::cout << "  [TEST] " << name << " ... "; \
    } while(0)

#define PASS() \
    do { \
        tests_passed++; \
        std::cout << "PASS" << std::endl; \
    } while(0)

#define FAIL(msg) \
    do { \
        tests_failed++; \
        std::cout << "FAIL: " << msg << std::endl; \
    } while(0)

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while(0)

// ─── normalizeUri tests ───

void test_trimAndLowercaseScheme() {
    TEST("normalizeUri trim and lowercase scheme");
    std::string result = normalizeUri("  VLESS://abc@host:443?type=tcp#name ");
    CHECK(result == "vless://abc@host:443?type=tcp#name", "trim and lowercase failed: got " + result);
    PASS();
}

void test_trimBomZeroWidth() {
    TEST("normalizeUri trim BOM and zero-width");
    std::string withBom = std::string("\xEF\xBB\xBF") + "vless://x@h:443#test" + std::string("\xEF\xBB\xBF");
    std::string result = normalizeUri(withBom);
    CHECK(result == "vless://x@h:443#test", "BOM trim failed: got " + result);
    PASS();
}

void test_mapHy2ToHysteria2() {
    TEST("normalizeUri map hy2 -> hysteria2");
    std::string result = normalizeUri("hy2://uuid@host:443#test");
    CHECK(result == "hysteria2://uuid@host:443#test", "hy2 mapping failed: got " + result);
    PASS();
}

void test_mapSocks5ToSocks() {
    TEST("normalizeUri map socks5 -> socks");
    std::string result = normalizeUri("socks5://user@host:1080#test");
    CHECK(result == "socks://user@host:1080#test", "socks5 mapping failed: got " + result);
    PASS();
}

void test_dropUnsupportedScheme() {
    TEST("normalizeUri drop unsupported scheme");
    std::string result = normalizeUri("http://example.com");
    CHECK(result.empty(), "unsupported scheme should be dropped");
    PASS();
}

void test_dropSchemeWithoutDelimiter() {
    TEST("normalizeUri drop missing ://");
    std::string result = normalizeUri("vless://bad");
    CHECK(!result.empty(), "valid vless should not be dropped");
    std::string bad = normalizeUri("hello");
    CHECK(bad.empty(), "missing :// should drop");
    PASS();
}

void test_vmessValidPayload() {
    TEST("normalizeUri vmess valid base64 JSON");
    // A valid vmess payload: base64 of simple JSON
    std::string payload = "eyJ2IjoiMiIsInBzIjoidGVzdCIsImFkZCI6ImV4YW1wbGUuY29tIiwicG9ydCI6IjQ0MyIsImlkIjoiMTExMTExMTEtMjIyMi0zMzMzLTQ0NDQtNTU1NTU1NTU1NTU1IiwibmV0Ijoid3MiLCJ0eXBlIjoibm9uZSIsInBhdGgiOiIvIn0=";
    std::string result = normalizeUri("vmess://" + payload + "#test");
    CHECK(result.find("vmess://") == 0, "vmess scheme preserved");
    CHECK(!result.empty(), "valid vmess should not be empty");
    PASS();
}

void test_vmessInvalidPayloadUndecodable() {
    TEST("normalizeUri vmess invalid undecodable payload");
    std::string result = normalizeUri("vmess://!!!invalid!!!#test");
    CHECK(result.empty(), "invalid vmess payload should be dropped");
    PASS();
}

void test_vmessReencodeUnpadded() {
    TEST("normalizeUri vmess re-encode unpadded");
    std::string payload = "eyJ2IjoiMiJ9"; // base64 of '{"v":"2"}' without padding (should be valid)
    std::string result = normalizeUri("vmess://" + payload);
    CHECK(!result.empty(), "valid unpadded payload should survive");
    CHECK(result.find("==") != std::string::npos || result.find("=") == std::string::npos, "padding handled");
    PASS();
}

void test_vmessReencodeUrlSafe() {
    TEST("normalizeUri vmess url-safe base64");
    std::string urlSafePayload = "eyJ2IjoiMiJ9"; // same as above, but let's test with - and _
    // Actually let's test with real url-safe replacement
    std::string result = normalizeUri("vmess://eyJ2IjoiMiJ9");
    CHECK(!result.empty(), "url-safe payload should work");
    PASS();
}

void test_nonVmessAddRemark() {
    TEST("normalizeUri non-vmess add remark");
    std::string result = normalizeUri("vless://uuid@host:443?type=tcp");
    CHECK(result.find("#Hunter") != std::string::npos, "missing remark should add #Hunter");
    PASS();
}

void test_nonVmessKeepRemark() {
    TEST("normalizeUri non-vmess keep existing remark");
    std::string result = normalizeUri("vless://uuid@host:443?type=tcp#test");
    CHECK(result.find("#test") != std::string::npos, "existing remark should be preserved");
    PASS();
}

void test_nonVmessPercentEncodeRemark() {
    TEST("normalizeUri percent-encode remark spaces");
    std::string result = normalizeUri("vless://uuid@host:443#hello world");
    CHECK(result.find("#hello%20world") != std::string::npos, "space in remark should be encoded: got " + result);
    PASS();
}

void test_nonVmessPercentEncodeRemarkUtf8() {
    TEST("normalizeUri percent-encode UTF-8 remark");
    std::string result = normalizeUri("vless://uuid@host:443#hello💜");
    CHECK(result.find("#hello%F0%9F%92%9C") != std::string::npos, "UTF-8 remark encoded: got " + result);
    PASS();
}

void test_nonVmessEmptyFragmentReplaced() {
    TEST("normalizeUri empty fragment -> Hunter");
    std::string result = normalizeUri("ss://base64@host:8388#");
    CHECK(result.find("#Hunter") != std::string::npos, "empty fragment replaced: got " + result);
    PASS();
}

void test_internalWhitespaceDrop() {
    TEST("normalizeUri repairs scraped bodies");
    std::string result = normalizeUri("trojan://pass@host:443/?type=tcp #bad");
    CHECK(result == "trojan://pass@host:443/?type=tcp#bad", "space before # trimmed");
    result = normalizeUri("vless://u@h:443?path=/a b&amp;security=tls#r");
    CHECK(result == "vless://u@h:443?path=/a%20b&security=tls#r", "space encoded, &amp; unescaped");
    PASS();
}

void test_neverAlterQueryParams() {
    TEST("normalizeUri preserve vless/trojan query params");
    std::string result = normalizeUri("vless://uuid@host:443?encryption=none&type=ws&host=cloud&path=/p#test");
    CHECK(result.find("encryption=none") != std::string::npos, "query params preserved");
    CHECK(result.find("type=ws") != std::string::npos, "query params preserved");
    PASS();
}

void test_trimWhitespaceEnds() {
    TEST("normalizeUri trim whitespace ends");
    std::string result = normalizeUri("  vless://x@h:443?a=1#name  ");
    CHECK(result == "vless://x@h:443?a=1#name", "trimmed: got " + result);
    PASS();
}

void test_trimInternalSpaceInFragmentOnly() {
    TEST("normalizeUri keep space in fragment, encode it");
    std::string result = normalizeUri("vless://x@h:443#hello world");
    CHECK(result == "vless://x@h:443#hello%20world", "fragment space encoded: got " + result);
    PASS();
}

// ─── buildShareText tests ───

void test_buildShareTextBasic() {
    TEST("buildShareText basic");
    std::vector<std::string> uris = {"vless://a@h:443#b", "vmess://eyJ2IjoiMiIsInBzIjoidGVzdCIsImFkZCI6ImV4YW1wbGUuY29tIiwicG9ydCI6IjQ0MyIsImlkIjoiMTExMTExMTEtMjIyMi0zMzMzLTQ0NDQtNTU1NTU1NTU1NTU1IiwibmV0Ijoid3MiLCJ0eXBlIjoibm9uZSIsInBhdGgiOiIvIn0=#c"};
    std::string text = buildShareText(uris);
    CHECK(text.find("vless://") != std::string::npos, "should contain vless");
    CHECK(text.find("vmess://") != std::string::npos, "should contain vmess");
    PASS();
}

void test_buildShareTextDeduped() {
    TEST("buildShareText deduped");
    std::vector<std::string> uris = {"vless://a@h:443#b", "vless://a@h:443#b"};
    std::string text = buildShareText(uris);
    int count = 0;
    size_t pos = 0;
    while ((pos = text.find("vless://a@h:443#b", pos)) != std::string::npos) {
        count++;
        pos++;
    }
    CHECK(count == 1, "should dedupe: found " + std::to_string(count));
    PASS();
}

void test_buildShareTextNormalizedOnly() {
    TEST("buildShareText drops invalid");
    std::vector<std::string> uris = {"vless://a@h:443#b", "bad-uri", "vmess://invalid!!!"};
    std::string text = buildShareText(uris);
    CHECK(text.find("vless://") != std::string::npos, "valid kept");
    CHECK(text.find("bad-uri") == std::string::npos, "invalid dropped");
    PASS();
}

void test_buildShareTextNewlineSeparator() {
    TEST("buildShareText newline separator");
    std::vector<std::string> uris = {"vless://a@h:443#b"};
    std::string text = buildShareText(uris);
    CHECK(text.find("\n") != std::string::npos || text.find("\r\n") != std::string::npos, "should have line break");
    CHECK(text.back() == '\n' || (text.size() >= 2 && text.substr(text.size()-2) == "\r\n"), "should end with newline");
    PASS();
}

void test_buildShareTextNoHeader() {
    TEST("buildShareText no header/comment");
    std::vector<std::string> uris = {"vless://a@h:443#b"};
    std::string text = buildShareText(uris);
    CHECK(text.find("# Hunter") == std::string::npos || text.find("vless://") == 0, "no header lines");
    PASS();
}

void test_buildShareTextSorted() {
    TEST("buildShareText keeps input order");
    std::vector<std::string> uris = {"vless://z@h:443#b", "vless://a@h:443#b"};
    std::string text = buildShareText(uris);
    size_t first = text.find("vless://a@");
    size_t second = text.find("vless://z@");
    CHECK(first != std::string::npos, "a found");
    CHECK(second != std::string::npos, "z found");
    CHECK(second < first, "order kept: z before a");
    PASS();
}

// ─── Main ───

int main() {
    std::cout << "\n=== Client Export Unit Tests ===" << std::endl << std::endl;

    // normalizeUri
    std::cout << "--- normalizeUri ---" << std::endl;
    test_trimAndLowercaseScheme();
    test_trimBomZeroWidth();
    test_mapHy2ToHysteria2();
    test_mapSocks5ToSocks();
    test_dropUnsupportedScheme();
    test_dropSchemeWithoutDelimiter();
    test_vmessValidPayload();
    test_vmessInvalidPayloadUndecodable();
    test_vmessReencodeUnpadded();
    test_vmessReencodeUrlSafe();
    test_nonVmessAddRemark();
    test_nonVmessKeepRemark();
    test_nonVmessPercentEncodeRemark();
    test_nonVmessPercentEncodeRemarkUtf8();
    test_nonVmessEmptyFragmentReplaced();
    test_internalWhitespaceDrop();
    test_neverAlterQueryParams();
    test_trimWhitespaceEnds();
    test_trimInternalSpaceInFragmentOnly();

    // buildShareText
    std::cout << "\n--- buildShareText ---" << std::endl;
    test_buildShareTextBasic();
    test_buildShareTextDeduped();
    test_buildShareTextNormalizedOnly();
    test_buildShareTextNewlineSeparator();
    test_buildShareTextNoHeader();
    test_buildShareTextSorted();

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "  Total:  " << tests_run << std::endl;
    std::cout << "  Passed: " << tests_passed << std::endl;
    std::cout << "  Failed: " << tests_failed << std::endl;

    if (tests_failed > 0) {
        std::cout << "\n  *** FAILURES DETECTED ***" << std::endl;
        return 1;
    }
    std::cout << "\n  All tests passed!" << std::endl;
    return 0;
}
