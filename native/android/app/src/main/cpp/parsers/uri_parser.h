#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include "core/models.h"

namespace hunter {

class VMessParser {
public:
    static std::optional<HunterParsedConfig> parse(const std::string& uri);
};

class VLESSParser {
public:
    static std::optional<HunterParsedConfig> parse(const std::string& uri);
};

class TrojanParser {
public:
    static std::optional<HunterParsedConfig> parse(const std::string& uri);
};

class ShadowsocksParser {
public:
    static std::optional<HunterParsedConfig> parse(const std::string& uri);
};

class UniversalParser {
public:
    std::optional<HunterParsedConfig> parse(const std::string& uri) const;
};

// URL parsing helper
struct ParsedURL {
    std::string scheme;
    std::string username;
    std::string hostname;
    int port = 0;
    std::string path;
    std::string query;
    std::string fragment;
};

ParsedURL parse_url(const std::string& url);
std::unordered_map<std::string, std::string> parse_query_string(const std::string& query);

} // namespace hunter
