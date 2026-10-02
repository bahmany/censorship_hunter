#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunter::geo {

/**
 * @brief Raw 16-byte IP address + db_version_id cache key.
 *
 * Eliminates heap allocations and string conversions on cache lookups.
 * IPv4 addresses are stored in standard IPv4-mapped IPv6 representation (RFC 4291).
 */
struct CacheKey {
    uint8_t ip[16];
    uint32_t db_version_id;

    bool operator==(const CacheKey& other) const noexcept {
        return db_version_id == other.db_version_id &&
               std::memcmp(ip, other.ip, 16) == 0;
    }
};

struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const noexcept {
        // 64-bit FNV-1a hash over raw 20-byte struct
        uint64_t h = 14695981039346656037ULL;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
        for (size_t i = 0; i < sizeof(CacheKey); ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};

/**
 * @brief Offline IP Geolocation Database using HCGEO1 format with LRU caching.
 *
 * Provides high-performance, thread-safe binary-search lookups for IPv4 and IPv6
 * addresses mapped to ISO-3166-1 alpha-2 country codes.
 *
 * Guarantees:
 * - Thread-safe LRU cache (default max 100k entries, keyed by raw IP bytes + db_version_id)
 * - Strict verification of header magic, version, endianness, payload SHA-256,
 *   dictionary codes, and non-overlapping sorted IP ranges.
 * - Private, reserved, loopback, multicast, and transition ranges handled:
 *   - NAT64 (64:ff9b::/96) -> mapped to embedded IPv4
 *   - 6to4 (2002::/16) -> mapped to embedded IPv4
 *   - Teredo (2001::/32) -> Unknown
 *   - IPv4-mapped (::ffff:0:0/96) -> mapped to IPv4
 * - Failures fail geo only, never crashing or interrupting network tasks.
 *
 * Concurrency:
 * - Database loading methods (loadFromBuffer, loadFromFile, loadEmbedded) mutate database
 *   state and should be called at startup or synchronized before concurrent lookups.
 * - All query methods (lookup, lookupIpv4, lookupIpv6, isPrivateOrReserved, cacheSize)
 *   are thread-safe and safe for concurrent calls from multiple threads.
 */
class CountryDatabase {
public:
    static constexpr const char* kUnknown = "Unknown";
    static constexpr size_t kDefaultCacheCapacity = 100000;

    CountryDatabase();
    explicit CountryDatabase(size_t cache_capacity);
    ~CountryDatabase();

    CountryDatabase(const CountryDatabase&) = delete;
    CountryDatabase& operator=(const CountryDatabase&) = delete;

    CountryDatabase(CountryDatabase&& other) noexcept;
    CountryDatabase& operator=(CountryDatabase&& other) noexcept;

    /**
     * @brief Load database from raw or zstd-compressed memory buffer.
     * Validates magic, version, endianness, payload SHA-256, dictionary,
     * and strictly sorted non-overlapping ranges.
     * @return true if valid and loaded, false on corrupt/invalid input.
     */
    bool loadFromBuffer(const uint8_t* data, size_t size);
    bool loadFromBuffer(const std::vector<uint8_t>& buffer);

    /**
     * @brief Load database from file path.
     */
    bool loadFromFile(const std::string& path);

    /**
     * @brief Check whether single-file build embedded a country database.
     */
    static bool hasEmbeddedDatabase();

    /**
     * @brief Load embedded database if available.
     */
    bool loadEmbedded();

    /**
     * @brief Shared global instance (initialized with embedded database if present).
     */
    static CountryDatabase& instance();

    // Query status and metadata
    bool isLoaded() const;
    std::string dbVersion() const;
    uint32_t dbVersionId() const;
    size_t ipv4Count() const;
    size_t ipv6Count() const;
    size_t countryCount() const;
    const std::string& lastError() const;

    /**
     * @brief Look up country for an IP string (IPv4 or IPv6, optional port).
     * @return ISO-2 country code (e.g. "US", "DE") or "Unknown".
     */
    std::string lookup(const std::string& ip_str) const;

    /**
     * @brief Look up country for raw network-order byte address.
     * @param ip_bytes Raw address (4 bytes for IPv4, 16 bytes for IPv6).
     * @param len Address length in bytes (must be 4 or 16).
     * @return ISO-2 country code or "Unknown".
     */
    std::string lookup(const uint8_t* ip_bytes, size_t len) const;

    /**
     * @brief Look up country for 32-bit IPv4 address in host numerical order.
     */
    std::string lookupIpv4(uint32_t ip_host_order) const;

    /**
     * @brief Look up country for 128-bit IPv6 address (16 bytes, big-endian).
     */
    std::string lookupIpv6(const uint8_t ip16[16]) const;

    /**
     * @brief Check if IP address belongs to private/reserved/multicast/loopback ranges.
     */
    static bool isPrivateOrReserved(const std::string& ip_str);
    static bool isPrivateOrReservedIpv4(uint32_t ip_host_order);
    static bool isPrivateOrReservedIpv6(const uint8_t ip16[16]);

    // Transition mechanism checkers
    static bool isIpv4Mapped(const uint8_t ip[16]) noexcept;
    static bool isNat64(const uint8_t ip[16]) noexcept;
    static bool is6to4(const uint8_t ip[16]) noexcept;
    static bool isTeredo(const uint8_t ip[16]) noexcept;

    // Cache management
    size_t cacheSize() const;
    size_t cacheCapacity() const;
    void clearCache();
    void setCacheCapacity(size_t capacity);

private:
    struct Ipv4Entry {
        uint32_t start;
        uint32_t end;
        uint16_t country_idx;
    };

    struct Ipv6Entry {
        uint8_t start[16];
        uint8_t end[16];
        uint16_t country_idx;
    };

    bool fail(const std::string& reason);
    void putInCache(const CacheKey& key, const std::string& result) const;
    static std::string normalizeIpString(const std::string& ip_str);

    bool is_loaded_{false};
    std::string db_version_;
    uint32_t db_version_id_{0};
    std::string last_error_;
    std::vector<std::string> countries_;
    std::vector<Ipv4Entry> ipv4_entries_;
    std::vector<Ipv6Entry> ipv6_entries_;

    std::atomic<size_t> max_cache_capacity_{kDefaultCacheCapacity};
    mutable std::mutex cache_mutex_;
    mutable std::list<std::pair<CacheKey, std::string>> cache_list_;
    mutable std::unordered_map<CacheKey, std::list<std::pair<CacheKey, std::string>>::iterator, CacheKeyHash> cache_map_;
};

} // namespace hunter::geo
