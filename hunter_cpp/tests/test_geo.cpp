/**
 * @file test_geo.cpp
 * @brief Unit tests for offline country database and IP lookup engine (HCGEO1 format).
 *
 * Tests run completely offline with synthetic fixtures (zero network access):
 * - Header validation and corruption rejection
 * - Boundary conditions for IPv4 and IPv6
 * - IP range gaps and unallocated addresses
 * - Private, reserved, multicast, loopback, and CGNAT IP spaces
 * - Overlap and unsorted interval rejection
 * - Thread-safe LRU cache hits, misses, eviction, and clearing
 * - In-memory zstd decompression
 */

#include "geo/country_database.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

using namespace hunter::geo;

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

namespace {

// Self-contained SHA-256 helper for building synthetic fixtures
struct TestSha256 {
    uint32_t state[8];
    uint64_t count;
    uint8_t data[64];
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t kK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

void test_sha256_transform(TestSha256* c, const uint8_t* d) {
    uint32_t m[64];
    for (int i = 0; i < 16; ++i) {
        m[i] = (static_cast<uint32_t>(d[i * 4]) << 24) |
               (static_cast<uint32_t>(d[i * 4 + 1]) << 16) |
               (static_cast<uint32_t>(d[i * 4 + 2]) << 8) |
               static_cast<uint32_t>(d[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    uint32_t a = c->state[0], b = c->state[1], g = c->state[2], d_ = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g_ = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g_) + kK[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, g);
        h = g_; g_ = f; f = e; e = d_ + t1;
        d_ = g; g = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += g; c->state[3] += d_;
    c->state[4] += e; c->state[5] += f; c->state[6] += g_; c->state[7] += h;
}

void test_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    TestSha256 c;
    c.state[0] = 0x6a09e667; c.state[1] = 0xbb67ae85;
    c.state[2] = 0x3c6ef372; c.state[3] = 0xa54ff53a;
    c.state[4] = 0x510e527f; c.state[5] = 0x9b05688c;
    c.state[6] = 0x1f83d9ab; c.state[7] = 0x5be0cd19;
    c.count = 0;
    for (size_t i = 0; i < len; ++i) {
        c.data[c.count % 64] = data[i];
        c.count++;
        if (c.count % 64 == 0) test_sha256_transform(&c, c.data);
    }
    uint64_t total_bits = c.count * 8;
    size_t rem = c.count % 64;
    c.data[rem++] = 0x80;
    if (rem > 56) {
        while (rem < 64) c.data[rem++] = 0;
        test_sha256_transform(&c, c.data);
        rem = 0;
    }
    while (rem < 56) c.data[rem++] = 0;
    for (int i = 7; i >= 0; --i) {
        c.data[56 + (7 - i)] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    test_sha256_transform(&c, c.data);
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>((c.state[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((c.state[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((c.state[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(c.state[i] & 0xFF);
    }
}

#undef ROTR
#undef CH
#undef MAJ
#undef EP0
#undef EP1
#undef SIG0
#undef SIG1

struct TestIpv4Range {
    uint32_t start;
    uint32_t end;
    uint16_t country_idx;
};

struct TestIpv6Range {
    uint8_t start[16];
    uint8_t end[16];
    uint16_t country_idx;
};

std::vector<uint8_t> buildSyntheticDb(
    const std::vector<std::string>& countries,
    const std::vector<TestIpv4Range>& v4,
    const std::vector<TestIpv6Range>& v6,
    const std::string& source_date = "2026-10")
{
    std::vector<uint8_t> payload;

    // Dictionary (country_count * 2)
    for (const auto& c : countries) {
        payload.push_back(static_cast<uint8_t>(c[0]));
        payload.push_back(static_cast<uint8_t>(c[1]));
    }

    // IPv4 (count * 10)
    for (const auto& r : v4) {
        payload.push_back(r.start & 0xFF);
        payload.push_back((r.start >> 8) & 0xFF);
        payload.push_back((r.start >> 16) & 0xFF);
        payload.push_back((r.start >> 24) & 0xFF);

        payload.push_back(r.end & 0xFF);
        payload.push_back((r.end >> 8) & 0xFF);
        payload.push_back((r.end >> 16) & 0xFF);
        payload.push_back((r.end >> 24) & 0xFF);

        payload.push_back(r.country_idx & 0xFF);
        payload.push_back((r.country_idx >> 8) & 0xFF);
    }

    // IPv6 (count * 34)
    for (const auto& r : v6) {
        payload.insert(payload.end(), r.start, r.start + 16);
        payload.insert(payload.end(), r.end, r.end + 16);
        payload.push_back(r.country_idx & 0xFF);
        payload.push_back((r.country_idx >> 8) & 0xFF);
    }

    uint8_t sha[32];
    test_sha256(payload.data(), payload.size(), sha);

    // Header (70 bytes)
    std::vector<uint8_t> full_db(70, 0);
    std::memcpy(&full_db[0], "HCGEO1", 6);
    full_db[6] = 1;  // version low
    full_db[7] = 0;  // version high
    full_db[8] = 1;  // endianness (1 = LE)
    full_db[9] = 0;  // reserved

    uint32_t v4_cnt = static_cast<uint32_t>(v4.size());
    std::memcpy(&full_db[10], &v4_cnt, 4);

    uint32_t v6_cnt = static_cast<uint32_t>(v6.size());
    std::memcpy(&full_db[14], &v6_cnt, 4);

    uint16_t c_cnt = static_cast<uint16_t>(countries.size());
    std::memcpy(&full_db[18], &c_cnt, 2);

    full_db[20] = 0;
    full_db[21] = 0;

    std::memcpy(&full_db[22], source_date.c_str(), std::min<size_t>(15, source_date.size()));
    std::memcpy(&full_db[38], sha, 32);

    full_db.insert(full_db.end(), payload.begin(), payload.end());
    return full_db;
}

uint32_t ip4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           static_cast<uint32_t>(d);
}

// 122-byte pre-computed zstd compressed fixture matching:
// Countries: ZZ, AU, US
// IPv4: 1.1.1.0-1.1.1.255 -> AU, 8.8.8.0-8.8.8.255 -> US
// IPv6: 2001:db8:1::0-2001:db8:1::ffff -> US
const uint8_t kSyntheticCompressedDb[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x04, 0x68, 0x65, 0x03, 0x00, 0xe4, 0x05, 0x48, 0x43, 0x47, 0x45, 0x4f,
    0x31, 0x01, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x32, 0x30, 0x32, 0x36, 0x2d, 0x31, 0x30, 0x53, 0x3a, 0x79, 0xf0, 0xb1, 0x0e, 0xce, 0x98,
    0x60, 0xd0, 0x7d, 0x2f, 0xc2, 0x7e, 0x5f, 0x11, 0xc2, 0x2f, 0xbd, 0x0e, 0xa0, 0x90, 0xbf, 0xbe,
    0x45, 0x68, 0x60, 0xb8, 0x9a, 0xc8, 0x56, 0x2c, 0x5a, 0x5a, 0x41, 0x55, 0x55, 0x53, 0x00, 0x01,
    0x01, 0x01, 0xff, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x08, 0x08, 0x08, 0xff, 0x02, 0x00, 0x26,
    0x07, 0xf8, 0xb0, 0x40, 0x00, 0xff, 0xff, 0x02, 0x00, 0x04, 0x00, 0x33, 0x3b, 0x78, 0x0b,
    0x5e, 0x2f, 0x80, 0x2d, 0xae, 0x13, 0xac, 0x09, 0x2c, 0xa4
};

} // namespace

void test_load_valid_synthetic() {
    TEST("CountryDatabase::loadFromBuffer valid synthetic");

    std::vector<std::string> countries = {"ZZ", "AU", "CN", "DE", "US"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},  // AU
        {ip4(1, 1, 2, 0), ip4(1, 1, 2, 255), 2},  // CN
        {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 4},  // US
    };

    uint8_t v6_start[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 1};
    uint8_t v6_end[16]   = {0x20, 0x01, 0x0d, 0xb8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    std::vector<TestIpv6Range> v6;
    TestIpv6Range r6;
    std::memcpy(r6.start, v6_start, 16);
    std::memcpy(r6.end, v6_end, 16);
    r6.country_idx = 4; // US
    v6.push_back(r6);

    auto buf = buildSyntheticDb(countries, v4, v6, "2026-10");

    CountryDatabase db;
    CHECK(!db.isLoaded(), "should start unloaded");
    bool ok = db.loadFromBuffer(buf);
    CHECK(ok, "loadFromBuffer failed: " + db.lastError());
    CHECK(db.isLoaded(), "isLoaded should be true");
    CHECK(db.ipv4Count() == 3, "ipv4Count mismatch");
    CHECK(db.ipv6Count() == 1, "ipv6Count mismatch");
    CHECK(db.countryCount() == 5, "countryCount mismatch");
    CHECK(db.dbVersion() == "2026-10", "dbVersion mismatch");
    PASS();
}

void test_ipv4_boundaries() {
    TEST("CountryDatabase IPv4 boundary conditions");

    std::vector<std::string> countries = {"ZZ", "AU", "CN", "US"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},  // AU: 1.1.1.0 - 1.1.1.255
        {ip4(1, 1, 2, 0), ip4(1, 1, 2, 255), 2},  // CN: 1.1.2.0 - 1.1.2.255
        {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 3},  // US: 8.8.8.0 - 8.8.8.255
    };
    auto buf = buildSyntheticDb(countries, v4, {});

    CountryDatabase db;
    CHECK(db.loadFromBuffer(buf), "load failed");

    // Start boundary of AU
    CHECK(db.lookup("1.1.1.0") == "AU", "1.1.1.0 should be AU");
    // Inside AU
    CHECK(db.lookup("1.1.1.128") == "AU", "1.1.1.128 should be AU");
    // End boundary of AU
    CHECK(db.lookup("1.1.1.255") == "AU", "1.1.1.255 should be AU");

    // Immediately adjacent start boundary of CN
    CHECK(db.lookup("1.1.2.0") == "CN", "1.1.2.0 should be CN");
    CHECK(db.lookup("1.1.2.255") == "CN", "1.1.2.255 should be CN");

    // Another range
    CHECK(db.lookup("8.8.8.8") == "US", "8.8.8.8 should be US");
    CHECK(db.lookup("8.8.8.0") == "US", "8.8.8.0 should be US");
    CHECK(db.lookup("8.8.8.255") == "US", "8.8.8.255 should be US");

    PASS();
}

void test_ipv6_boundaries() {
    TEST("CountryDatabase IPv6 boundary conditions");

    std::vector<std::string> countries = {"ZZ", "JP", "US"};
    uint8_t start1[16] = {0x24, 0x00, 0xcb, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t end1[16]   = {0x24, 0x00, 0xcb, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    uint8_t start2[16] = {0x26, 0x07, 0xf8, 0xb0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t end2[16]   = {0x26, 0x07, 0xf8, 0xb0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

    std::vector<TestIpv6Range> v6 = {
        { {0}, {0}, 1 }, // JP
        { {0}, {0}, 2 }  // US
    };
    std::memcpy(v6[0].start, start1, 16);
    std::memcpy(v6[0].end, end1, 16);
    std::memcpy(v6[1].start, start2, 16);
    std::memcpy(v6[1].end, end2, 16);

    auto buf = buildSyntheticDb(countries, {}, v6);
    CountryDatabase db;
    CHECK(db.loadFromBuffer(buf), "load failed");

    // Exact start of range 1
    CHECK(db.lookup("2607:f8b0:4000::") == "US", "2607:f8b0:4000:: should be US");
    // Inside range 1
    CHECK(db.lookup("2607:f8b0:4000::1234") == "US", "2607:f8b0:4000::1234 should be US");
    // Exact end of range 1
    CHECK(db.lookup("2607:f8b0:4000::ffff") == "US", "2607:f8b0:4000::ffff should be US");

    // Range 2
    CHECK(db.lookup("2400:cb00::") == "JP", "2400:cb00:: should be JP");
    CHECK(db.lookup("2400:cb00::ffff") == "JP", "2400:cb00::ffff should be JP");

    PASS();
}

void test_gaps() {
    TEST("CountryDatabase unallocated gaps return Unknown");

    std::vector<std::string> countries = {"ZZ", "AU", "US"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},  // AU
        {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 2},  // US
    };
    auto buf = buildSyntheticDb(countries, v4, {});
    CountryDatabase db;
    CHECK(db.loadFromBuffer(buf), "load failed");

    // Address in gap between 1.1.1.255 and 8.8.8.0
    CHECK(db.lookup("1.1.2.1") == CountryDatabase::kUnknown, "1.1.2.1 should be Unknown");
    CHECK(db.lookup("8.8.7.255") == CountryDatabase::kUnknown, "8.8.7.255 should be Unknown");

    // Address before 1.1.1.0
    CHECK(db.lookup("1.1.0.255") == CountryDatabase::kUnknown, "1.1.0.255 should be Unknown");

    // Address after 8.8.8.255
    CHECK(db.lookup("8.8.9.0") == CountryDatabase::kUnknown, "8.8.9.0 should be Unknown");

    // Unmapped IPv6
    CHECK(db.lookup("2600::1") == CountryDatabase::kUnknown, "2600::1 should be Unknown");

    PASS();
}

void test_private_reserved() {
    TEST("CountryDatabase private/reserved spaces return Unknown");

    // Even if database somehow contained private ranges, isPrivateOrReserved must block it
    std::vector<std::string> countries = {"ZZ", "AU", "US"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(8, 8, 8, 0),  ip4(8, 8, 8, 255),       2}, // US
        {ip4(10, 0, 0, 0), ip4(10, 255, 255, 255), 1}, // Rogue 10.x entry
    };
    auto buf = buildSyntheticDb(countries, v4, {});
    CountryDatabase db;
    CHECK(db.loadFromBuffer(buf), "load failed");

    // Loopback
    CHECK(db.lookup("127.0.0.1") == CountryDatabase::kUnknown, "127.0.0.1 must be Unknown");
    CHECK(db.lookup("127.255.255.254") == CountryDatabase::kUnknown, "127.255.255.254 must be Unknown");

    // RFC 1918 Private
    CHECK(db.lookup("10.0.0.1") == CountryDatabase::kUnknown, "10.0.0.1 must be Unknown");
    CHECK(db.lookup("172.16.0.1") == CountryDatabase::kUnknown, "172.16.0.1 must be Unknown");
    CHECK(db.lookup("172.31.255.254") == CountryDatabase::kUnknown, "172.31.255.254 must be Unknown");
    CHECK(db.lookup("192.168.1.1") == CountryDatabase::kUnknown, "192.168.1.1 must be Unknown");

    // Link-local
    CHECK(db.lookup("169.254.1.1") == CountryDatabase::kUnknown, "169.254.1.1 must be Unknown");

    // CGNAT
    CHECK(db.lookup("100.64.0.1") == CountryDatabase::kUnknown, "100.64.0.1 must be Unknown");

    // Multicast & Reserved
    CHECK(db.lookup("224.0.0.1") == CountryDatabase::kUnknown, "224.0.0.1 must be Unknown");
    CHECK(db.lookup("240.0.0.1") == CountryDatabase::kUnknown, "240.0.0.1 must be Unknown");
    CHECK(db.lookup("255.255.255.255") == CountryDatabase::kUnknown, "255.255.255.255 must be Unknown");
    CHECK(db.lookup("0.0.0.0") == CountryDatabase::kUnknown, "0.0.0.0 must be Unknown");

    // IPv6 Private / Special
    CHECK(db.lookup("::") == CountryDatabase::kUnknown, ":: must be Unknown");
    CHECK(db.lookup("::1") == CountryDatabase::kUnknown, "::1 must be Unknown");
    CHECK(db.lookup("2001:db8::1") == CountryDatabase::kUnknown, "2001:db8::1 documentation space must be Unknown");
    CHECK(db.lookup("fe80::1") == CountryDatabase::kUnknown, "fe80::1 must be Unknown");
    CHECK(db.lookup("fc00::1") == CountryDatabase::kUnknown, "fc00::1 must be Unknown");
    CHECK(db.lookup("fd12:3456::1") == CountryDatabase::kUnknown, "fd12:3456::1 must be Unknown");
    CHECK(db.lookup("ff02::1") == CountryDatabase::kUnknown, "ff02::1 must be Unknown");

    // IPv4-mapped IPv6
    CHECK(db.lookup("::ffff:127.0.0.1") == CountryDatabase::kUnknown, "::ffff:127.0.0.1 must be Unknown");
    CHECK(db.lookup("::ffff:8.8.8.8") == "US", "::ffff:8.8.8.8 should resolve to US");

    PASS();
}

void test_corrupt_rejection() {
    TEST("CountryDatabase corrupt header and invalid data rejection");

    std::vector<std::string> countries = {"ZZ", "AU"};
    std::vector<TestIpv4Range> v4 = { {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1} };
    auto valid_buf = buildSyntheticDb(countries, v4, {});

    // 1. Buffer smaller than header (70 bytes)
    {
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(valid_buf.data(), 60), "should reject small buffer");
    }

    // 2. Bad magic
    {
        auto b = valid_buf;
        b[0] = 'X';
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject bad magic");
    }

    // 3. Bad version
    {
        auto b = valid_buf;
        b[6] = 2; // version 2 unsupported
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject bad version");
    }

    // 4. Bad endianness
    {
        auto b = valid_buf;
        b[8] = 2; // endian 2 unsupported
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject bad endianness");
    }

    // 5. Payload SHA-256 mismatch
    {
        auto b = valid_buf;
        b[72] ^= 0xFF; // flip bit in payload
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject SHA-256 mismatch");
    }

    // 6. Truncated buffer
    {
        auto b = valid_buf;
        b.pop_back();
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject truncated payload");
    }

    // 7. Trailing garbage
    {
        auto b = valid_buf;
        b.push_back(0);
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject trailing garbage");
    }

    // 8. Corrupt dictionary code (lowercase or numbers)
    {
        std::vector<std::string> bad_countries = {"ZZ", "aU"};
        auto b = buildSyntheticDb(bad_countries, v4, {});
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(b), "should reject lowercase country code in dictionary");
    }

    PASS();
}

void test_overlap_and_unsorted_rejection() {
    TEST("CountryDatabase overlap and unsorted interval rejection");

    std::vector<std::string> countries = {"ZZ", "AU", "US"};

    // 1. Overlapping IPv4 ranges
    {
        std::vector<TestIpv4Range> v4_overlap = {
            {ip4(1, 1, 1, 0), ip4(1, 1, 1, 200), 1},
            {ip4(1, 1, 1, 150), ip4(1, 1, 1, 255), 2},
        };
        auto buf = buildSyntheticDb(countries, v4_overlap, {});
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(buf), "should reject overlapping IPv4 ranges");
    }

    // 2. Unsorted IPv4 ranges
    {
        std::vector<TestIpv4Range> v4_unsorted = {
            {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 2},
            {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},
        };
        auto buf = buildSyntheticDb(countries, v4_unsorted, {});
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(buf), "should reject unsorted IPv4 ranges");
    }

    // 3. Inverted IPv4 range (start > end)
    {
        std::vector<TestIpv4Range> v4_inverted = {
            {ip4(1, 1, 1, 255), ip4(1, 1, 1, 0), 1},
        };
        auto buf = buildSyntheticDb(countries, v4_inverted, {});
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(buf), "should reject inverted IPv4 range");
    }

    // 4. Out of bounds country index
    {
        std::vector<TestIpv4Range> v4_bad_idx = {
            {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 99},
        };
        auto buf = buildSyntheticDb(countries, v4_bad_idx, {});
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(buf), "should reject out-of-bounds country index");
    }

    // 5. Overlapping IPv6 ranges
    {
        uint8_t s1[16] = {0x20, 0x01, 0, 0};
        uint8_t e1[16] = {0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0};
        uint8_t s2[16] = {0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80, 0};
        uint8_t e2[16] = {0x20, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

        std::vector<TestIpv6Range> v6_overlap = {
            { {0}, {0}, 1 },
            { {0}, {0}, 2 }
        };
        std::memcpy(v6_overlap[0].start, s1, 16);
        std::memcpy(v6_overlap[0].end, e1, 16);
        std::memcpy(v6_overlap[1].start, s2, 16);
        std::memcpy(v6_overlap[1].end, e2, 16);

        auto buf = buildSyntheticDb(countries, {}, v6_overlap);
        CountryDatabase db;
        CHECK(!db.loadFromBuffer(buf), "should reject overlapping IPv6 ranges");
    }

    PASS();
}

void test_cache_hits_misses_eviction() {
    TEST("CountryDatabase LRU cache hits, misses, eviction, clearing");

    std::vector<std::string> countries = {"ZZ", "AU", "US", "DE"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},  // AU
        {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 2},  // US
        {ip4(9, 9, 9, 0), ip4(9, 9, 9, 255), 3},  // DE
    };
    auto buf = buildSyntheticDb(countries, v4, {}, "2026-10");

    CountryDatabase db(3); // Small capacity: 3 entries
    CHECK(db.loadFromBuffer(buf), "load failed");
    CHECK(db.cacheSize() == 0, "cache should be initially empty");

    // First lookup: AU -> cache miss, inserted
    CHECK(db.lookup("1.1.1.10") == "AU", "lookup failed");
    CHECK(db.cacheSize() == 1, "cache size should be 1");

    // Repeat lookup: AU -> cache hit, size unchanged
    CHECK(db.lookup("1.1.1.10") == "AU", "lookup failed");
    CHECK(db.cacheSize() == 1, "cache size should remain 1");

    // Second IP: US
    CHECK(db.lookup("8.8.8.8") == "US", "lookup failed");
    CHECK(db.cacheSize() == 2, "cache size should be 2");

    // Third IP: DE
    CHECK(db.lookup("9.9.9.9") == "DE", "lookup failed");
    CHECK(db.cacheSize() == 3, "cache size should be 3");

    // Touch AU again so it moves to front (most recently used)
    CHECK(db.lookup("1.1.1.10") == "AU", "lookup failed");

    // Fourth IP: 1.1.1.20 -> causes eviction of least recently used ("8.8.8.8")
    CHECK(db.lookup("1.1.1.20") == "AU", "lookup failed");
    CHECK(db.cacheSize() == 3, "cache size must not exceed capacity 3");

    // Clear cache
    db.clearCache();
    CHECK(db.cacheSize() == 0, "cache size should be 0 after clearCache()");

    PASS();
}

void test_thread_safe_cache() {
    TEST("CountryDatabase thread-safe concurrent lookups");

    std::vector<std::string> countries = {"ZZ", "AU", "US", "DE"};
    std::vector<TestIpv4Range> v4 = {
        {ip4(1, 1, 1, 0), ip4(1, 1, 1, 255), 1},  // AU
        {ip4(8, 8, 8, 0), ip4(8, 8, 8, 255), 2},  // US
        {ip4(9, 9, 9, 0), ip4(9, 9, 9, 255), 3},  // DE
    };
    auto buf = buildSyntheticDb(countries, v4, {}, "2026-10");

    CountryDatabase db(100);
    CHECK(db.loadFromBuffer(buf), "load failed");

    const int kThreadCount = 8;
    const int kLookupsPerThread = 500;
    std::vector<std::thread> workers;
    std::vector<bool> errors(kThreadCount, false);

    for (int t = 0; t < kThreadCount; ++t) {
        workers.emplace_back([&db, &errors, t, kLookupsPerThread] {
            for (int i = 0; i < kLookupsPerThread; ++i) {
                if (db.lookup("1.1.1.1") != "AU") errors[t] = true;
                if (db.lookup("8.8.8.8") != "US") errors[t] = true;
                if (db.lookup("9.9.9.9") != "DE") errors[t] = true;
                if (db.lookup("127.0.0.1") != CountryDatabase::kUnknown) errors[t] = true;
                if (db.lookup("1.1.3.1") != CountryDatabase::kUnknown) errors[t] = true;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    for (int t = 0; t < kThreadCount; ++t) {
        CHECK(!errors[t], "Thread reported lookup mismatch in concurrent test");
    }
    CHECK(db.cacheSize() <= 100, "cache exceeded capacity under concurrent load");

    PASS();
}

void test_zstd_decompression() {
    TEST("CountryDatabase in-memory zstd decompression");

    CountryDatabase db;
    bool ok = db.loadFromBuffer(kSyntheticCompressedDb, sizeof(kSyntheticCompressedDb));
    CHECK(ok, "loadFromBuffer with zstd compressed data failed: " + db.lastError());
    CHECK(db.isLoaded(), "isLoaded should be true");
    CHECK(db.ipv4Count() == 2, "ipv4Count should be 2");
    CHECK(db.ipv6Count() == 1, "ipv6Count should be 1");
    CHECK(db.countryCount() == 3, "countryCount should be 3");

    // Lookups
    CHECK(db.lookup("1.1.1.100") == "AU", "1.1.1.100 should be AU");
    CHECK(db.lookup("8.8.8.8") == "US", "8.8.8.8 should be US");
    CHECK(db.lookup("2607:f8b0:4000::10") == "US", "2607:f8b0:4000::10 should be US");
    CHECK(db.lookup("127.0.0.1") == CountryDatabase::kUnknown, "127.0.0.1 should be Unknown");

    PASS();
}

void test_lookup_by_bytes_and_formatting() {
    TEST("CountryDatabase lookup by raw bytes and formatting");

    CountryDatabase db;
    CHECK(db.loadFromBuffer(kSyntheticCompressedDb, sizeof(kSyntheticCompressedDb)), "load failed");

    // 4-byte IPv4: 1.1.1.100
    uint8_t ip4_bytes[4] = {1, 1, 1, 100};
    CHECK(db.lookup(ip4_bytes, 4) == "AU", "raw 4-byte IPv4 lookup failed");

    // 16-byte IPv6: 2001:db8:1::10
    uint8_t ip6_bytes[16] = {0x26, 0x07, 0xf8, 0xb0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
    CHECK(db.lookup(ip6_bytes, 16) == "US", "raw 16-byte IPv6 lookup failed");

    // Invalid length
    CHECK(db.lookup(ip4_bytes, 3) == CountryDatabase::kUnknown, "3-byte lookup should be Unknown");
    CHECK(db.lookup(ip4_bytes, 5) == CountryDatabase::kUnknown, "5-byte lookup should be Unknown");

    // Port stripping and whitespace
    CHECK(db.lookup(" 8.8.8.8:53 ") == "US", "IPv4 with port and whitespace should resolve to US");
    CHECK(db.lookup("[2607:f8b0:4000::10]:443") == "US", "Bracketed IPv6 with port should resolve to US");

    // Non-IP text
    CHECK(db.lookup("not_an_ip") == CountryDatabase::kUnknown, "Non-IP string should return Unknown");

    PASS();
}

void test_embedded_database() {
    TEST("CountryDatabase embedded database verification");
    if (CountryDatabase::hasEmbeddedDatabase()) {
        CountryDatabase& inst = CountryDatabase::instance();
        CHECK(inst.isLoaded(), "embedded database should be loaded in embedded build");
        CHECK(inst.ipv4Count() > 0, "embedded ipv4 count > 0");
        CHECK(!inst.dbVersion().empty(), "dbVersion should not be empty");
        CHECK(inst.lookup("8.8.8.8") == "US", "8.8.8.8 should resolve to US in embedded DB");
        std::cout << "[embedded active: " << inst.dbVersion() << " v4=" << inst.ipv4Count() << "] ";
    } else {
        CountryDatabase inst;
        CHECK(!CountryDatabase::hasEmbeddedDatabase(), "hasEmbeddedDatabase should be false");
        CHECK(!inst.loadEmbedded(), "loadEmbedded should return false when not embedded");
        std::cout << "[stub verified] ";
    }
    PASS();
}

int main() {
    std::cout << "=== Running Offline Geo Database Tests (Stage 4 C1) ===" << std::endl;

    test_load_valid_synthetic();
    test_ipv4_boundaries();
    test_ipv6_boundaries();
    test_gaps();
    test_private_reserved();
    test_corrupt_rejection();
    test_overlap_and_unsorted_rejection();
    test_cache_hits_misses_eviction();
    test_thread_safe_cache();
    test_zstd_decompression();
    test_lookup_by_bytes_and_formatting();
    test_embedded_database();

    std::cout << "=== Test Results: " << tests_passed << "/" << tests_run << " passed";
    if (tests_failed > 0) {
        std::cout << " (" << tests_failed << " FAILED)";
    }
    std::cout << " ===" << std::endl;

    return (tests_failed == 0) ? 0 : 1;
}
