#pragma once
#include <string>
#include <vector>
namespace hunter::client {
struct SubscriptionParseResult {
    std::vector<std::string> uris;   // deduplicated, order preserved
    int skipped = 0;                 // non-empty lines/blocks that yielded no usable URI
    bool used_base64 = false;        // body was a base64-encoded list
    bool used_clash_yaml = false;    // body was a Clash/mihomo YAML config
};
SubscriptionParseResult parseSubscriptionText(std::string body);
}