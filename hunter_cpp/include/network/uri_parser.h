#pragma once

#include <string>
#include <vector>
#include <optional>
#include "core/models.h"

namespace hunter {
namespace network {

/**
 * @brief Universal proxy URI parser
 * 
 * Parses vmess://, vless://, trojan://, ss://, ssr://,
 * hysteria2://, hy2://, tuic:// URIs into ParsedConfig structs.
 */
class UriParser {
public:
    /**
     * @brief Parse a single URI string
     * @param uri Raw URI (e.g. "vless://uuid@host:port?params#remark")
     * @return Parsed config, or nullopt if invalid
     */
    static std::optional<ParsedConfig> parse(const std::string& uri);

    /**
     * @brief Parse multiple URIs, skipping invalid ones
     */
    static std::vector<ParsedConfig> parseMany(const std::vector<std::string>& uris);

    /**
     * @brief Check if a string looks like a supported URI
     */
    static bool isValidScheme(const std::string& uri);

private:
    static std::optional<ParsedConfig> parseVmess(const std::string& uri);
    static std::optional<ParsedConfig> parseVless(const std::string& uri);
    static std::optional<ParsedConfig> parseTrojan(const std::string& uri);
    static std::optional<ParsedConfig> parseShadowsocks(const std::string& uri);
    static std::optional<ParsedConfig> parseHysteria2(const std::string& uri);
    static std::optional<ParsedConfig> parseTuic(const std::string& uri);

    // Helper: parse query params from "key=val&key2=val2"
    static std::map<std::string, std::string> parseQueryParams(const std::string& query);
};

/**
 * @brief Prioritize configs by anti-censorship features
 * 
 * Orders configs with Reality > gRPC > CDN > TLS > plain
 */
std::vector<std::string> prioritizeConfigs(const std::vector<std::string>& uris);

} // namespace network
} // namespace hunter
