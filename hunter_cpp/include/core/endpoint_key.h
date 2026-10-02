#pragma once
// EndpointKeyV1 (design D3): canonical endpoint/config identity shared by the
// database, orchestrator and validator. Pure logic, no I/O.
#include <string>

namespace hunter {

struct EndpointKey {
    std::string key;        // "ek1:" + lowercase hex SHA-256 of `canonical`
    std::string canonical;  // canonical serialization (kept for collision checks; do not log)
    bool valid = false;     // false => URI unparseable/ambiguous; key is an exact-byte fallback
    std::string protocol;   // canonical protocol (empty if !valid)
    std::string host;       // canonical host (empty if !valid)
    int port = 0;
    bool insecure_tls = false;  // allowInsecure/insecure option set truthy
};

constexpr int kEndpointKeyVersion = 1;

EndpointKey computeEndpointKey(const std::string& uri);
std::string endpointKeyForUri(const std::string& uri);  // computeEndpointKey(uri).key

// Helpers exposed for tests / other modules.
std::string canonicalProtocol(const std::string& proto);   // lowercase, hy2->hysteria2, ss->shadowsocks
bool canonicalHost(const std::string& host, std::string* out);  // false if unusable
std::string sha256Hex(const std::string& data);

}  // namespace hunter
