#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hunter::geo {

/**
 * @brief Offline IP Geolocation Database using HCGEO1 format with LRU caching.
 *
 * Provides high-performance, thread-safe binary-search lookups for IPv4 and IPv6
 * addresses mapped to ISO-3166-1 alpha-2 country codes.
 *
 * Guarantees:
 * - Thread-safe LRU cache (default max 100k entries, keyed by ip + db_version)
 * - Strict verification of header magic, version, endianness, payload SHA-256,
 *   dictionary codes, and non-overlapping sorted IP ranges.
 * - Private/reserved/loopback/multicast ranges map to "Unknown".
 * - Failures fail geo only, never crashing or interrupting network tasks.
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
    void putInCache(const std::string& cache_key, const std::string& result) const;
    static bool isIpv4Mapped(const uint8_t ip[16]);
    static std::string normalizeIpString(const std::string& ip_str);

    bool is_loaded_{false};
    std::string db_version_;
    std::string last_error_;
    std::vector<std::string> countries_;
    std::vector<Ipv4Entry> ipv4_entries_;
    std::vector<Ipv6Entry> ipv6_entries_;

    size_t max_cache_capacity_{kDefaultCacheCapacity};
    mutable std::mutex cache_mutex_;
    mutable std::list<std::pair<std::string, std::string>> cache_list_;
    mutable std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator> cache_map_;
};

} // namespace hunter::geo
