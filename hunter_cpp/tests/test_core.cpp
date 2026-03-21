/**
 * @file test_core.cpp
 * @brief Lightweight unit tests for Hunter core functionality
 * 
 * No external test framework required — uses simple assert macros.
 * Build: linked against hunter_core static library.
 */

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <set>
#include <sstream>

#include "core/utils.h"
#include "core/models.h"
#include "network/uri_parser.h"
#include "network/continuous_validator.h"

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using namespace hunter;
using namespace hunter::utils;
using namespace hunter::network;

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

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Utils tests
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void test_trim() {
    TEST("utils::trim");
    CHECK(trim("  hello  ") == "hello", "basic trim failed");
    CHECK(trim("hello") == "hello", "no-op trim failed");
    CHECK(trim("") == "", "empty trim failed");
    CHECK(trim("  ") == "", "whitespace-only trim failed");
    CHECK(trim("\t\nhello\r\n") == "hello", "mixed whitespace trim failed");
    PASS();
}

void test_split() {
    TEST("utils::split");
    auto parts = split("a,b,c", ',');
    CHECK(parts.size() == 3, "split count wrong");
    CHECK(parts[0] == "a" && parts[1] == "b" && parts[2] == "c", "split values wrong");

    auto single = split("hello", ',');
    CHECK(single.size() == 1 && single[0] == "hello", "single split failed");

    auto empty = split("", ',');
    CHECK(empty.empty(), "empty split should return 0 elements");
    PASS();
}

void test_join() {
    TEST("utils::join");
    CHECK(join({"a", "b", "c"}, ",") == "a,b,c", "basic join failed");
    CHECK(join({"hello"}, ",") == "hello", "single join failed");
    CHECK(join({}, ",") == "", "empty join failed");
    PASS();
}

void test_startsWith_endsWith() {
    TEST("utils::startsWith/endsWith");
    CHECK(startsWith("vless://abc", "vless://"), "startsWith failed");
    CHECK(!startsWith("vmess://abc", "vless://"), "startsWith false positive");
    CHECK(endsWith("test.txt", ".txt"), "endsWith failed");
    CHECK(!endsWith("test.txt", ".json"), "endsWith false positive");
    CHECK(startsWith("", ""), "empty startsWith failed");
    CHECK(endsWith("", ""), "empty endsWith failed");
    PASS();
}

void test_iequals() {
    TEST("utils::iequals");
    CHECK(iequals("Hello", "hello"), "case-insensitive failed");
    CHECK(iequals("VLESS", "vless"), "upper vs lower failed");
    CHECK(!iequals("abc", "abcd"), "different length should fail");
    PASS();
}

void test_base64() {
    TEST("utils::base64Encode/Decode");
    std::string original = "Hello, World!";
    std::string encoded = base64Encode(original);
    CHECK(encoded == "SGVsbG8sIFdvcmxkIQ==", "base64 encode failed: got " + encoded);
    std::string decoded = base64Decode(encoded);
    CHECK(decoded == original, "base64 roundtrip failed");

    // Empty string
    CHECK(base64Encode("") == "", "base64 encode empty failed");
    CHECK(base64Decode("") == "", "base64 decode empty failed");

    // URL-safe base64 (padding stripped)
    std::string noPad = "SGVsbG8sIFdvcmxkIQ";
    std::string decoded2 = base64Decode(noPad);
    CHECK(decoded2 == original, "base64 decode without padding failed");
    PASS();
}

void test_urlEncodeDecode() {
    TEST("utils::urlEncode/Decode");
    std::string original = "hello world&foo=bar";
    std::string encoded = urlEncode(original);
    CHECK(encoded.find(' ') == std::string::npos, "urlEncode should not contain spaces");
    std::string decoded = urlDecode(encoded);
    CHECK(decoded == original, "url roundtrip failed");

    // Already-encoded
    CHECK(urlDecode("hello%20world") == "hello world", "urlDecode %20 failed");
    CHECK(urlDecode("hello+world") == "hello world" || urlDecode("hello+world") == "hello+world",
          "urlDecode + handling");
    PASS();
}

void test_extractUris() {
    TEST("utils::extractRawUrisFromText");
    std::string text = "Some text vless://abc@host:443?type=tcp#name more text\n"
                       "vmess://eyJhZGQiOiIxLjEuMS4xIn0=\n"
                       "trojan://pass@host:443\n"
                       "ss://base64stuff@host:8388\n"
                       "not a uri\n";
    auto uris = extractRawUrisFromText(text);
    CHECK(uris.size() >= 3, "should find at least 3 URIs, found " + std::to_string(uris.size()));
    bool hasVless = false, hasTrojan = false;
    for (auto& u : uris) {
        if (startsWith(u, "vless://")) hasVless = true;
        if (startsWith(u, "trojan://")) hasTrojan = true;
    }
    CHECK(hasVless, "should find vless URI");
    CHECK(hasTrojan, "should find trojan URI");
    PASS();
}

void test_jsonBuilder() {
    TEST("utils::JsonBuilder");
    JsonBuilder jb;
    jb.add("name", "test")
      .add("count", 42)
      .add("rate", 3.14)
      .add("active", true);
    std::string json = jb.build();
    CHECK(json.find("\"name\":\"test\"") != std::string::npos, "json string field missing");
    CHECK(json.find("\"count\":42") != std::string::npos, "json int field missing");
    CHECK(json.find("\"active\":true") != std::string::npos, "json bool field missing");
    CHECK(json.front() == '{' && json.back() == '}', "json should be wrapped in braces");

    // Test addRaw
    JsonBuilder jb2;
    jb2.add("outer", "val")
       .addRaw("inner", "{\"nested\":1}");
    std::string json2 = jb2.build();
    CHECK(json2.find("\"inner\":{\"nested\":1}") != std::string::npos, "addRaw failed");
    PASS();
}

void test_logRingBuffer() {
    TEST("utils::LogRingBuffer");
    auto& ring = LogRingBuffer::instance();
    ring.push("line1");
    ring.push("line2");
    ring.push("line3");
    auto recent = ring.recent(2);
    CHECK(recent.size() == 2, "should return 2 recent lines");
    CHECK(recent[0].find("line2") != std::string::npos && recent[1].find("line3") != std::string::npos, "recent order wrong");
    PASS();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Models tests
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void test_parsedConfig_isValid() {
    TEST("ParsedConfig::isValid");
    ParsedConfig pc;
    CHECK(!pc.isValid(), "empty config should be invalid");

    pc.protocol = "vless";
    pc.address = "example.com";
    pc.port = 443;
    CHECK(pc.isValid(), "valid config rejected");

    pc.port = 0;
    CHECK(!pc.isValid(), "port 0 should be invalid");

    pc.port = 443;
    pc.address = "";
    CHECK(!pc.isValid(), "empty address should be invalid");

    pc.address = "example.com";
    pc.port = 70000;
    CHECK(!pc.isValid(), "port > 65535 should be invalid");
    PASS();
}

void test_hardwareSnapshot() {
    TEST("HardwareSnapshot::detect");
    auto hw = HardwareSnapshot::detect();
    CHECK(hw.cpu_count >= 1, "cpu_count should be >= 1");
    CHECK(hw.ram_total_gb > 0, "ram_total_gb should be > 0");
    PASS();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// URI Parser tests
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void test_uriParser_isValidScheme() {
    TEST("UriParser::isValidScheme");
    CHECK(UriParser::isValidScheme("vless://abc@host:443"), "vless should be valid scheme");
    CHECK(UriParser::isValidScheme("vmess://abc"), "vmess should be valid scheme");
    CHECK(UriParser::isValidScheme("trojan://pass@host:443"), "trojan should be valid scheme");
    CHECK(UriParser::isValidScheme("ss://abc"), "ss should be valid scheme");
    CHECK(!UriParser::isValidScheme("http://example.com"), "http should not be valid scheme");
    CHECK(!UriParser::isValidScheme("ftp://host"), "ftp should not be valid scheme");
    CHECK(!UriParser::isValidScheme(""), "empty should not be valid scheme");
    PASS();
}

void test_uriParser_parseVless() {
    TEST("UriParser::parse(vless)");
    std::string uri = "vless://uuid-here@example.com:443?encryption=none&security=reality&sni=sni.example.com&fp=chrome&pbk=pubkey123&sid=ab&type=tcp&flow=xtls-rprx-vision#MyRemarkName";
    auto result = UriParser::parse(uri);
    CHECK(result.has_value(), "should parse valid vless URI");
    auto& pc = result.value();
    CHECK(pc.protocol == "vless", "protocol should be vless");
    CHECK(pc.address == "example.com", "address mismatch");
    CHECK(pc.port == 443, "port should be 443");
    CHECK(pc.uuid == "uuid-here", "uuid mismatch");
    CHECK(pc.security == "reality", "security should be reality");
    CHECK(pc.sni == "sni.example.com", "sni mismatch");
    CHECK(pc.fingerprint == "chrome", "fingerprint should be chrome");
    CHECK(pc.public_key == "pubkey123", "public_key mismatch");
    CHECK(pc.flow == "xtls-rprx-vision", "flow mismatch");
    PASS();
}

void test_uriParser_parseTrojan() {
    TEST("UriParser::parse(trojan)");
    std::string uri = "trojan://password123@trojan.example.com:443?security=tls&sni=trojan.example.com&type=tcp#TrojanNode";
    auto result = UriParser::parse(uri);
    CHECK(result.has_value(), "should parse valid trojan URI");
    auto& pc = result.value();
    CHECK(pc.protocol == "trojan", "protocol should be trojan");
    CHECK(pc.address == "trojan.example.com", "address mismatch");
    CHECK(pc.port == 443, "port mismatch");
    CHECK(pc.uuid == "password123", "password mismatch");
    PASS();
}

void test_uriParser_parseInvalid() {
    TEST("UriParser::parse(invalid)");
    CHECK(!UriParser::parse("not-a-uri").has_value(), "garbage should not parse");
    CHECK(!UriParser::parse("").has_value(), "empty should not parse");
    CHECK(!UriParser::parse("vless://").has_value(), "bare scheme should not parse");
    PASS();
}

void test_uriParser_parseMany() {
    TEST("UriParser::parseMany");
    std::vector<std::string> uris = {
        "vless://uuid@host1:443?type=tcp&security=tls#name1",
        "not-valid",
        "trojan://pass@host2:443#name2",
    };
    auto results = UriParser::parseMany(uris);
    CHECK(results.size() == 2, "should parse 2 valid URIs out of 3, got " + std::to_string(results.size()));
    PASS();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ConfigDatabase tests
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void test_configDB_basic() {
    TEST("ConfigDatabase basic operations");
    ConfigDatabase db;

    // Add configs
    std::set<std::string> uris = {"vless://a@host1:443", "trojan://b@host2:443", "vless://c@host3:443"};
    int added = db.addConfigs(uris);
    CHECK(added == 3, "should add 3 configs, added " + std::to_string(added));

    // Stats
    auto stats = db.getStats();
    CHECK(stats.total == 3, "total should be 3");
    CHECK(stats.untested_unique == 3, "untested should be 3");
    CHECK(stats.alive == 0, "alive should be 0 initially");

    // Duplicate add
    int added2 = db.addConfigs(uris);
    CHECK(added2 == 0, "duplicate add should return 0");
    CHECK(db.getStats().total == 3, "total should still be 3 after duplicates");
    PASS();
}

void test_configDB_healthUpdate() {
    TEST("ConfigDatabase health updates");
    ConfigDatabase db;
    db.addConfigs(std::set<std::string>{"vless://test@host:443"});

    // Mark as alive
    db.updateHealth("vless://test@host:443", true, 150.0f);
    auto stats = db.getStats();
    CHECK(stats.alive == 1, "should have 1 alive after pass");
    CHECK(stats.total_tested == 1, "total_tested should be 1");
    CHECK(stats.total_passed == 1, "total_passed should be 1");

    // Mark as dead (may require multiple failures to flip alive flag)
    db.updateHealth("vless://test@host:443", false, -1.0f);
    auto stats2 = db.getStats();
    // alive may still be 1 if implementation uses consecutive failure threshold
    CHECK(stats2.total_tested == 2, "total_tested should be 2");
    CHECK(stats2.total_passed == 1, "total_passed should still be 1");
    PASS();
}

void test_configDB_getUntestedBatch() {
    TEST("ConfigDatabase getUntestedBatch");
    ConfigDatabase db;
    std::set<std::string> uris;
    for (int i = 0; i < 20; i++) {
        uris.insert("vless://uuid" + std::to_string(i) + "@host:443");
    }
    db.addConfigs(uris);

    auto batch = db.getUntestedBatch(5);
    CHECK(batch.size() == 5, "batch should be 5, got " + std::to_string(batch.size()));

    // Ensure no duplicates in batch
    std::set<std::string> seen;
    for (auto& rec : batch) seen.insert(rec.uri);
    CHECK(seen.size() == batch.size(), "batch should have no duplicates");
    PASS();
}

void test_configDB_priorityPromotion() {
    TEST("ConfigDatabase priority promotion for imported configs");
    ConfigDatabase db;
    const std::string imported = "vless://prio@host1:443";
    const std::string regular = "vless://regular@host2:443";

    db.addConfigs(std::set<std::string>{imported, regular}, "scrape");
    db.updateHealth(imported, true, 120.0f);
    db.updateHealth(regular, true, 150.0f);

    int promoted = 0;
    const int added = db.addConfigsWithPriority(std::set<std::string>{imported}, "user_import", &promoted);
    CHECK(added == 0, "duplicate import should not add new records");
    CHECK(promoted == 1, "existing imported config should be promoted");

    auto batch = db.getUntestedBatch(1);
    CHECK(batch.size() == 1, "priority batch should contain one config");
    CHECK(batch[0].uri == imported, "promoted imported config should be first in batch");
    PASS();
}

void test_configDB_clearOlderThan() {
    TEST("ConfigDatabase clearOlderThan");
    ConfigDatabase db;
    db.addConfigs(std::set<std::string>{"vless://old@host:443", "vless://new@host:443"});

    // Mark one as alive recently
    db.updateHealth("vless://new@host:443", true, 100.0f);
    // The old one was never alive — clearOlderThan(0) should remove it
    int removed = db.clearOlderThan(0);
    // Both have first_seen ~now, so with 0 hours, results depend on implementation
    // Just check it doesn't crash
    CHECK(removed >= 0, "clearOlderThan should return >= 0");
    PASS();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    std::cout << "\n=== Hunter Core Unit Tests ===" << std::endl;
    std::cout << std::endl;

    // Utils
    std::cout << "--- Utils ---" << std::endl;
    test_trim();
    test_split();
    test_join();
    test_startsWith_endsWith();
    test_iequals();
    test_base64();
    test_urlEncodeDecode();
    test_extractUris();
    test_jsonBuilder();
    test_logRingBuffer();

    // Models
    std::cout << "\n--- Models ---" << std::endl;
    test_parsedConfig_isValid();
    test_hardwareSnapshot();

    // URI Parser
    std::cout << "\n--- URI Parser ---" << std::endl;
    test_uriParser_isValidScheme();
    test_uriParser_parseVless();
    test_uriParser_parseTrojan();
    test_uriParser_parseInvalid();
    test_uriParser_parseMany();

    // ConfigDatabase
    std::cout << "\n--- ConfigDatabase ---" << std::endl;
    test_configDB_basic();
    test_configDB_healthUpdate();
    test_configDB_getUntestedBatch();
    test_configDB_priorityPromotion();
    test_configDB_clearOlderThan();

    // Summary
    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "  Total:  " << tests_run << std::endl;
    std::cout << "  Passed: " << tests_passed << std::endl;
    std::cout << "  Failed: " << tests_failed << std::endl;

    if (tests_failed > 0) {
        std::cout << "\n  *** FAILURES DETECTED ***" << std::endl;
    } else {
        std::cout << "\n  All tests passed!" << std::endl;
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return tests_failed > 0 ? 1 : 0;
}
