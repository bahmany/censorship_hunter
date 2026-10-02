#include "geo/country_database.h"

// Vendored zstd decoder
#include "zstd.h"

// Build-time generated embedded header (stub or real)
#include "geo_embedded.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace hunter::geo {

namespace {

// ─── Self-contained SHA-256 Implementation ───

struct Sha256Ctx {
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

void sha256_transform(Sha256Ctx* c, const uint8_t* d) {
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

void sha256_init(Sha256Ctx* c) {
    c->state[0] = 0x6a09e667; c->state[1] = 0xbb67ae85;
    c->state[2] = 0x3c6ef372; c->state[3] = 0xa54ff53a;
    c->state[4] = 0x510e527f; c->state[5] = 0x9b05688c;
    c->state[6] = 0x1f83d9ab; c->state[7] = 0x5be0cd19;
    c->count = 0;
}

void sha256_update(Sha256Ctx* c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c->data[c->count % 64] = data[i];
        c->count++;
        if (c->count % 64 == 0) {
            sha256_transform(c, c->data);
        }
    }
}

void sha256_final(Sha256Ctx* c, uint8_t out[32]) {
    uint64_t total_bits = c->count * 8;
    size_t rem = c->count % 64;
    c->data[rem++] = 0x80;
    if (rem > 56) {
        while (rem < 64) c->data[rem++] = 0;
        sha256_transform(c, c->data);
        rem = 0;
    }
    while (rem < 56) c->data[rem++] = 0;
    for (int i = 7; i >= 0; --i) {
        c->data[56 + (7 - i)] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    sha256_transform(c, c->data);
    for (int i = 0; i < 8; ++i) {
        out[i * 4]     = static_cast<uint8_t>((c->state[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<uint8_t>((c->state[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((c->state[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(c->state[i] & 0xFF);
    }
}

void compute_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

#undef ROTR
#undef CH
#undef MAJ
#undef EP0
#undef EP1
#undef SIG0
#undef SIG1

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

CountryDatabase::CountryDatabase()
    : CountryDatabase(kDefaultCacheCapacity) {}

CountryDatabase::CountryDatabase(size_t cache_capacity)
    : max_cache_capacity_(cache_capacity) {}

CountryDatabase::~CountryDatabase() = default;

CountryDatabase::CountryDatabase(CountryDatabase&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.cache_mutex_);
    is_loaded_ = other.is_loaded_;
    db_version_ = std::move(other.db_version_);
    last_error_ = std::move(other.last_error_);
    countries_ = std::move(other.countries_);
    ipv4_entries_ = std::move(other.ipv4_entries_);
    ipv6_entries_ = std::move(other.ipv6_entries_);
    max_cache_capacity_ = other.max_cache_capacity_;
    cache_list_ = std::move(other.cache_list_);
    cache_map_ = std::move(other.cache_map_);
    other.is_loaded_ = false;
}

CountryDatabase& CountryDatabase::operator=(CountryDatabase&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(cache_mutex_, other.cache_mutex_);
        is_loaded_ = other.is_loaded_;
        db_version_ = std::move(other.db_version_);
        last_error_ = std::move(other.last_error_);
        countries_ = std::move(other.countries_);
        ipv4_entries_ = std::move(other.ipv4_entries_);
        ipv6_entries_ = std::move(other.ipv6_entries_);
        max_cache_capacity_ = other.max_cache_capacity_;
        cache_list_ = std::move(other.cache_list_);
        cache_map_ = std::move(other.cache_map_);
        other.is_loaded_ = false;
    }
    return *this;
}

bool CountryDatabase::fail(const std::string& reason) {
    countries_.clear();
    ipv4_entries_.clear();
    ipv6_entries_.clear();
    is_loaded_ = false;
    last_error_ = reason;
    clearCache();
    return false;
}

bool CountryDatabase::loadFromBuffer(const std::vector<uint8_t>& buffer) {
    return loadFromBuffer(buffer.data(), buffer.size());
}

bool CountryDatabase::loadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return fail("Failed to open file: " + path);
    }
    file.seekg(0, std::ios::end);
    std::streampos end_pos = file.tellg();
    if (end_pos < 0) {
        return fail("Failed to determine file size: " + path);
    }
    size_t size = static_cast<size_t>(end_pos);
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return fail("Failed to read file: " + path);
    }
    return loadFromBuffer(buffer.data(), buffer.size());
}

bool CountryDatabase::loadFromBuffer(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        return fail("Buffer too small");
    }

    std::vector<uint8_t> decomp_buffer;
    const uint8_t* raw_data = data;
    size_t raw_size = size;

    // Check for zstd frame magic: 0xFD2FB528
    if (readU32LE(data) == 0xFD2FB528) {
        unsigned long long orig_len = ZSTD_getFrameContentSize(data, size);
        if (orig_len == ZSTD_CONTENTSIZE_ERROR) {
            return fail("Corrupt zstd frame header");
        }
        if (orig_len != ZSTD_CONTENTSIZE_UNKNOWN && orig_len > 128 * 1024 * 1024) {
            return fail("Decompressed size exceeds 128 MB limit");
        }

        if (orig_len != ZSTD_CONTENTSIZE_UNKNOWN) {
            decomp_buffer.resize(static_cast<size_t>(orig_len));
            size_t d_res = ZSTD_decompress(decomp_buffer.data(), decomp_buffer.size(), data, size);
            if (ZSTD_isError(d_res)) {
                return fail(std::string("Zstd decompression failed: ") + ZSTD_getErrorName(d_res));
            }
            if (d_res != orig_len) {
                return fail("Decompressed byte length mismatch");
            }
        } else {
            // Frame content size omitted in zstd header; use streaming decompression
            ZSTD_DStream* dstream = ZSTD_createDStream();
            if (!dstream) {
                return fail("Failed to initialize ZSTD_DStream");
            }
            size_t init_res = ZSTD_initDStream(dstream);
            if (ZSTD_isError(init_res)) {
                ZSTD_freeDStream(dstream);
                return fail("ZSTD_initDStream failed");
            }

            ZSTD_inBuffer in_buf = { data, size, 0 };
            constexpr size_t kChunkSize = 128 * 1024;
            std::vector<uint8_t> chunk(kChunkSize);
            size_t ret = 0;
            do {
                ZSTD_outBuffer out_buf = { chunk.data(), chunk.size(), 0 };
                ret = ZSTD_decompressStream(dstream, &out_buf, &in_buf);
                if (ZSTD_isError(ret)) {
                    ZSTD_freeDStream(dstream);
                    return fail(std::string("Zstd streaming decompression failed: ") + ZSTD_getErrorName(ret));
                }
                decomp_buffer.insert(decomp_buffer.end(), chunk.data(), chunk.data() + out_buf.pos);
                if (decomp_buffer.size() > 128 * 1024 * 1024) {
                    ZSTD_freeDStream(dstream);
                    return fail("Streaming decompressed size exceeds 128 MB limit");
                }
            } while (ret != 0);
            ZSTD_freeDStream(dstream);
        }

        raw_data = decomp_buffer.data();
        raw_size = decomp_buffer.size();
    }

    constexpr size_t kHeaderSize = 70;
    if (raw_size < kHeaderSize) {
        return fail("Buffer size smaller than HCGEO1 header (70 bytes)");
    }

    // 1. Verify magic: "HCGEO1"
    if (std::memcmp(raw_data, "HCGEO1", 6) != 0) {
        return fail("Invalid HCGEO1 magic");
    }

    // 2. Verify version: 1
    uint16_t version = readU16LE(raw_data + 6);
    if (version != 1) {
        return fail("Unsupported HCGEO1 version: " + std::to_string(version));
    }

    // 3. Verify endianness: 1 (Little-endian)
    uint8_t endianness = raw_data[8];
    if (endianness != 1) {
        return fail("Unsupported endianness: " + std::to_string(endianness));
    }

    // 4. Counts
    uint32_t ipv4_count = readU32LE(raw_data + 10);
    uint32_t ipv6_count = readU32LE(raw_data + 14);
    uint16_t country_count = readU16LE(raw_data + 18);

    if (country_count < 1) {
        return fail("Country count must be >= 1 (index 0 is Unknown)");
    }

    uint64_t dict_bytes_len = static_cast<uint64_t>(country_count) * 2;
    uint64_t ipv4_bytes_len = static_cast<uint64_t>(ipv4_count) * 10;
    uint64_t ipv6_bytes_len = static_cast<uint64_t>(ipv6_count) * 34;
    uint64_t expected_payload_len = dict_bytes_len + ipv4_bytes_len + ipv6_bytes_len;
    uint64_t expected_total_size = kHeaderSize + expected_payload_len;

    if (raw_size != expected_total_size) {
        return fail("Payload size mismatch: expected " + std::to_string(expected_total_size) +
                    ", got " + std::to_string(raw_size));
    }

    // 5. Source date
    char date_buf[17] = {0};
    std::memcpy(date_buf, raw_data + 22, 16);
    date_buf[16] = '\0';
    std::string source_date(date_buf);
    while (!source_date.empty() && (source_date.back() == '\0' || source_date.back() == ' ')) {
        source_date.pop_back();
    }

    // 6. Verify payload SHA-256
    const uint8_t* header_sha256 = raw_data + 38;
    const uint8_t* payload_ptr = raw_data + kHeaderSize;
    uint8_t computed_sha256[32];
    compute_sha256(payload_ptr, static_cast<size_t>(expected_payload_len), computed_sha256);
    if (std::memcmp(header_sha256, computed_sha256, 32) != 0) {
        return fail("Payload SHA-256 mismatch");
    }

    // 7. Parse country dictionary
    std::vector<std::string> new_countries;
    new_countries.reserve(country_count);
    new_countries.push_back(kUnknown); // Index 0 is always Unknown

    const uint8_t* dict_ptr = payload_ptr;
    for (size_t i = 1; i < country_count; ++i) {
        char c0 = static_cast<char>(dict_ptr[i * 2]);
        char c1 = static_cast<char>(dict_ptr[i * 2 + 1]);
        if (c0 < 'A' || c0 > 'Z' || c1 < 'A' || c1 > 'Z') {
            return fail("Invalid country code in dictionary: " + std::string(1, c0) + std::string(1, c1));
        }
        new_countries.emplace_back(std::string({c0, c1}));
    }

    // 8. Parse IPv4 records (sorted, non-overlapping)
    const uint8_t* v4_ptr = dict_ptr + dict_bytes_len;
    std::vector<Ipv4Entry> new_v4;
    new_v4.reserve(ipv4_count);

    for (size_t i = 0; i < ipv4_count; ++i) {
        const uint8_t* rec = v4_ptr + i * 10;
        uint32_t start = readU32LE(rec);
        uint32_t end = readU32LE(rec + 4);
        uint16_t c_idx = readU16LE(rec + 8);

        if (start > end) {
            return fail("IPv4 range invalid: start > end at record " + std::to_string(i));
        }
        if (c_idx >= country_count) {
            return fail("IPv4 country index out of bounds at record " + std::to_string(i));
        }
        if (i > 0) {
            if (start <= new_v4.back().end) {
                return fail("IPv4 records not strictly sorted or overlapping at record " + std::to_string(i));
            }
        }
        new_v4.push_back({start, end, c_idx});
    }

    // 9. Parse IPv6 records (sorted, non-overlapping)
    const uint8_t* v6_ptr = v4_ptr + ipv4_bytes_len;
    std::vector<Ipv6Entry> new_v6;
    new_v6.reserve(ipv6_count);

    for (size_t i = 0; i < ipv6_count; ++i) {
        const uint8_t* rec = v6_ptr + i * 34;
        Ipv6Entry entry;
        std::memcpy(entry.start, rec, 16);
        std::memcpy(entry.end, rec + 16, 16);
        entry.country_idx = readU16LE(rec + 32);

        if (std::memcmp(entry.start, entry.end, 16) > 0) {
            return fail("IPv6 range invalid: start > end at record " + std::to_string(i));
        }
        if (entry.country_idx >= country_count) {
            return fail("IPv6 country index out of bounds at record " + std::to_string(i));
        }
        if (i > 0) {
            if (std::memcmp(entry.start, new_v6.back().end, 16) <= 0) {
                return fail("IPv6 records not strictly sorted or overlapping at record " + std::to_string(i));
            }
        }
        new_v6.push_back(entry);
    }

    // Commit state
    countries_ = std::move(new_countries);
    ipv4_entries_ = std::move(new_v4);
    ipv6_entries_ = std::move(new_v6);
    db_version_ = source_date.empty() ? "unknown" : source_date;
    is_loaded_ = true;
    last_error_.clear();

    clearCache();
    return true;
}

bool CountryDatabase::hasEmbeddedDatabase() {
#if defined(HUNTER_EMBED_HAS_GEO) && HUNTER_EMBED_HAS_GEO
    return true;
#else
    return false;
#endif
}

bool CountryDatabase::loadEmbedded() {
#if defined(HUNTER_EMBED_HAS_GEO) && HUNTER_EMBED_HAS_GEO
    return loadFromBuffer(kGeoZst, static_cast<size_t>(kGeoZstLen));
#else
    last_error_ = "Embedded country database not compiled in this build";
    return false;
#endif
}

CountryDatabase& CountryDatabase::instance() {
    static CountryDatabase inst;
    static std::once_flag init_flag;
    std::call_once(init_flag, [] {
        if (hasEmbeddedDatabase()) {
            inst.loadEmbedded();
        }
    });
    return inst;
}

bool CountryDatabase::isLoaded() const {
    return is_loaded_;
}

std::string CountryDatabase::dbVersion() const {
    return db_version_;
}

size_t CountryDatabase::ipv4Count() const {
    return ipv4_entries_.size();
}

size_t CountryDatabase::ipv6Count() const {
    return ipv6_entries_.size();
}

size_t CountryDatabase::countryCount() const {
    return countries_.size();
}

const std::string& CountryDatabase::lastError() const {
    return last_error_;
}

std::string CountryDatabase::normalizeIpString(const std::string& ip_str) {
    if (ip_str.empty()) return "";
    size_t start = ip_str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = ip_str.find_last_not_of(" \t\r\n");
    std::string s = ip_str.substr(start, end - start + 1);

    // [2001:db8::1]:port or [2001:db8::1]
    if (s.front() == '[') {
        size_t close_bracket = s.find(']');
        if (close_bracket != std::string::npos) {
            return s.substr(1, close_bracket - 1);
        }
    }

    // 1.2.3.4:port (single colon)
    size_t first_colon = s.find(':');
    if (first_colon != std::string::npos && s.find(':', first_colon + 1) == std::string::npos) {
        return s.substr(0, first_colon);
    }

    return s;
}

std::string CountryDatabase::lookup(const std::string& ip_str) const {
    std::string normalized = normalizeIpString(ip_str);
    if (normalized.empty()) {
        return kUnknown;
    }

    if (isPrivateOrReserved(normalized)) {
        return kUnknown;
    }

    std::string cache_key = normalized + "@" + db_version_;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_map_.find(cache_key);
        if (it != cache_map_.end()) {
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return it->second->second;
        }
    }

    if (!is_loaded_) {
        return kUnknown;
    }

    if (normalized.find(':') != std::string::npos) {
        uint8_t addr6[16];
        if (inet_pton(AF_INET6, normalized.c_str(), addr6) == 1) {
            if (isIpv4Mapped(addr6)) {
                uint32_t v4 = (static_cast<uint32_t>(addr6[12]) << 24) |
                              (static_cast<uint32_t>(addr6[13]) << 16) |
                              (static_cast<uint32_t>(addr6[14]) << 8) |
                              static_cast<uint32_t>(addr6[15]);
                return lookupIpv4(v4);
            }
            return lookupIpv6(addr6);
        }
    } else {
        struct in_addr addr4;
        if (inet_pton(AF_INET, normalized.c_str(), &addr4) == 1) {
            uint32_t ip = ntohl(addr4.s_addr);
            return lookupIpv4(ip);
        }
    }

    return kUnknown;
}

std::string CountryDatabase::lookup(const uint8_t* ip_bytes, size_t len) const {
    if (!ip_bytes) return kUnknown;
    if (len == 4) {
        uint32_t v4 = (static_cast<uint32_t>(ip_bytes[0]) << 24) |
                      (static_cast<uint32_t>(ip_bytes[1]) << 16) |
                      (static_cast<uint32_t>(ip_bytes[2]) << 8) |
                      static_cast<uint32_t>(ip_bytes[3]);
        return lookupIpv4(v4);
    }
    if (len == 16) {
        return lookupIpv6(ip_bytes);
    }
    return kUnknown;
}

std::string CountryDatabase::lookupIpv4(uint32_t ip) const {
    if (isPrivateOrReservedIpv4(ip)) {
        return kUnknown;
    }

    char ip_buf[32];
    std::snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    std::string cache_key = std::string(ip_buf) + "@" + db_version_;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_map_.find(cache_key);
        if (it != cache_map_.end()) {
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return it->second->second;
        }
    }

    if (!is_loaded_ || ipv4_entries_.empty()) {
        return kUnknown;
    }

    std::string result = kUnknown;
    size_t low = 0;
    size_t high = ipv4_entries_.size() - 1;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        const auto& entry = ipv4_entries_[mid];
        if (ip < entry.start) {
            if (mid == 0) break;
            high = mid - 1;
        } else if (ip > entry.end) {
            low = mid + 1;
        } else {
            if (entry.country_idx > 0 && entry.country_idx < countries_.size()) {
                result = countries_[entry.country_idx];
            }
            break;
        }
    }

    putInCache(cache_key, result);
    return result;
}

std::string CountryDatabase::lookupIpv6(const uint8_t ip16[16]) const {
    if (isPrivateOrReservedIpv6(ip16)) {
        return kUnknown;
    }

    if (isIpv4Mapped(ip16)) {
        uint32_t v4 = (static_cast<uint32_t>(ip16[12]) << 24) |
                      (static_cast<uint32_t>(ip16[13]) << 16) |
                      (static_cast<uint32_t>(ip16[14]) << 8) |
                      static_cast<uint32_t>(ip16[15]);
        return lookupIpv4(v4);
    }

    char ip_buf[INET6_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET6, ip16, ip_buf, sizeof(ip_buf));
    std::string cache_key = std::string(ip_buf) + "@" + db_version_;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_map_.find(cache_key);
        if (it != cache_map_.end()) {
            cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
            return it->second->second;
        }
    }

    if (!is_loaded_ || ipv6_entries_.empty()) {
        return kUnknown;
    }

    std::string result = kUnknown;
    size_t low = 0;
    size_t high = ipv6_entries_.size() - 1;

    while (low <= high) {
        size_t mid = low + (high - low) / 2;
        const auto& entry = ipv6_entries_[mid];
        int cmp_start = std::memcmp(ip16, entry.start, 16);
        if (cmp_start < 0) {
            if (mid == 0) break;
            high = mid - 1;
        } else {
            int cmp_end = std::memcmp(ip16, entry.end, 16);
            if (cmp_end > 0) {
                low = mid + 1;
            } else {
                if (entry.country_idx > 0 && entry.country_idx < countries_.size()) {
                    result = countries_[entry.country_idx];
                }
                break;
            }
        }
    }

    putInCache(cache_key, result);
    return result;
}

bool CountryDatabase::isIpv4Mapped(const uint8_t ip[16]) {
    for (int i = 0; i < 10; ++i) {
        if (ip[i] != 0) return false;
    }
    return (ip[10] == 0xFF && ip[11] == 0xFF);
}

bool CountryDatabase::isPrivateOrReservedIpv4(uint32_t ip) {
    // 0.0.0.0/8 (Current network)
    if ((ip & 0xFF000000) == 0x00000000) return true;
    // 10.0.0.0/8 (Private-Use RFC 1918)
    if ((ip & 0xFF000000) == 0x0A000000) return true;
    // 100.64.0.0/10 (Shared Address Space / CGNAT RFC 6598)
    if ((ip & 0xFFC00000) == 0x64400000) return true;
    // 127.0.0.0/8 (Loopback RFC 1122)
    if ((ip & 0xFF000000) == 0x7F000000) return true;
    // 169.254.0.0/16 (Link Local RFC 3927)
    if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;
    // 172.16.0.0/12 (Private-Use RFC 1918: 172.16.0.0 - 172.31.255.255)
    if ((ip & 0xFFF00000) == 0xAC100000) return true;
    // 192.0.0.0/24 (IETF Protocol Assignments RFC 6890)
    if ((ip & 0xFFFFFF00) == 0xC0000000) return true;
    // 192.0.2.0/24 (TEST-NET-1 RFC 5737)
    if ((ip & 0xFFFFFF00) == 0xC0000200) return true;
    // 192.88.99.0/24 (6to4 Relay Anycast RFC 7526)
    if ((ip & 0xFFFFFF00) == 0xC0586300) return true;
    // 192.168.0.0/16 (Private-Use RFC 1918)
    if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
    // 198.18.0.0/15 (Benchmarking RFC 2544: 198.18.0.0 - 198.19.255.255)
    if ((ip & 0xFFFE0000) == 0xC6120000) return true;
    // 198.51.100.0/24 (TEST-NET-2 RFC 5737)
    if ((ip & 0xFFFFFF00) == 0xC6336400) return true;
    // 203.0.113.0/24 (TEST-NET-3 RFC 5737)
    if ((ip & 0xFFFFFF00) == 0xCB007100) return true;
    // 224.0.0.0/4 (Multicast RFC 5771)
    if ((ip & 0xF0000000) == 0xE0000000) return true;
    // 240.0.0.0/4 (Reserved RFC 1112 / Broadcast 255.255.255.255)
    if ((ip & 0xF0000000) == 0xF0000000) return true;

    return false;
}

bool CountryDatabase::isPrivateOrReservedIpv6(const uint8_t ip[16]) {
    // :: (unspecified, 16 zero bytes)
    static const uint8_t kUnspecified[16] = {0};
    if (std::memcmp(ip, kUnspecified, 16) == 0) return true;

    // ::1 (loopback, 15 zero bytes then 1)
    static const uint8_t kLoopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (std::memcmp(ip, kLoopback, 16) == 0) return true;

    // 100::/64 (Discard-only RFC 6666): ip[0..1] == 0x01, 0x00, ip[2..7] == 0
    if (ip[0] == 0x01 && ip[1] == 0x00) {
        bool zero = true;
        for (int i = 2; i < 8; ++i) {
            if (ip[i] != 0) { zero = false; break; }
        }
        if (zero) return true;
    }

    // 2001:db8::/32 (Documentation RFC 3849): 2001:0db8::
    if (ip[0] == 0x20 && ip[1] == 0x01 && ip[2] == 0x0d && ip[3] == 0xb8) {
        return true;
    }

    // fc00::/7 (Unique Local Address ULA RFC 4193): high 7 bits are 1111110 -> 0xFC or 0xFD
    if ((ip[0] & 0xFE) == 0xFC) {
        return true;
    }

    // fe80::/10 (Link-Local Unicast RFC 4291): 1111111010 -> ip[0] == 0xFE, (ip[1] & 0xC0) == 0x80
    if (ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80) {
        return true;
    }

    // ff00::/8 (Multicast RFC 4291)
    if (ip[0] == 0xFF) {
        return true;
    }

    return false;
}

bool CountryDatabase::isPrivateOrReserved(const std::string& ip_str) {
    std::string s = normalizeIpString(ip_str);
    if (s.empty()) return true;

    if (s.find(':') != std::string::npos) {
        uint8_t addr6[16];
        if (inet_pton(AF_INET6, s.c_str(), addr6) == 1) {
            if (isIpv4Mapped(addr6)) {
                uint32_t v4 = (static_cast<uint32_t>(addr6[12]) << 24) |
                              (static_cast<uint32_t>(addr6[13]) << 16) |
                              (static_cast<uint32_t>(addr6[14]) << 8) |
                              static_cast<uint32_t>(addr6[15]);
                return isPrivateOrReservedIpv4(v4);
            }
            return isPrivateOrReservedIpv6(addr6);
        }
    } else {
        struct in_addr addr4;
        if (inet_pton(AF_INET, s.c_str(), &addr4) == 1) {
            uint32_t ip = ntohl(addr4.s_addr);
            return isPrivateOrReservedIpv4(ip);
        }
    }
    return true; // Unparseable string treated as private/Unknown
}

void CountryDatabase::putInCache(const std::string& cache_key, const std::string& result) const {
    if (max_cache_capacity_ == 0) return;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto it = cache_map_.find(cache_key);
    if (it != cache_map_.end()) {
        cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
        it->second->second = result;
        return;
    }
    if (cache_list_.size() >= max_cache_capacity_) {
        const auto& old_key = cache_list_.back().first;
        cache_map_.erase(old_key);
        cache_list_.pop_back();
    }
    cache_list_.emplace_front(cache_key, result);
    cache_map_[cache_key] = cache_list_.begin();
}

size_t CountryDatabase::cacheSize() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return cache_list_.size();
}

size_t CountryDatabase::cacheCapacity() const {
    return max_cache_capacity_;
}

void CountryDatabase::clearCache() {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    cache_list_.clear();
    cache_map_.clear();
}

void CountryDatabase::setCacheCapacity(size_t capacity) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    max_cache_capacity_ = capacity;
    while (cache_list_.size() > max_cache_capacity_) {
        const auto& old_key = cache_list_.back().first;
        cache_map_.erase(old_key);
        cache_list_.pop_back();
    }
}

} // namespace hunter::geo
