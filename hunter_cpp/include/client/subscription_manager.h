#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include "network/http_client.h"
#include "network/continuous_validator.h"
#include "client/subscription_parse.h"

namespace hunter {
namespace network { class ConfigDatabase; class HttpClient; }

namespace client {

struct Subscription {
    std::string id;          // stable short id (e.g. 8-hex of fnv/sha of url)
    std::string url;
    std::string remarks;     // display name; empty => derive from url host
    bool enabled = true;
    double last_update = 0;  // epoch seconds; 0 = never fetched
    int last_count = 0;      // configs imported last fetch
    std::string last_error;  // empty = ok
};

class SubscriptionManager {
public:
    SubscriptionManager() = default;
    SubscriptionManager(hunter::network::ConfigDatabase* db, hunter::network::HttpClient* http);
    /// (Re)bind dependencies — used when the manager is a value member initialized before db/http exist.
    void bind(hunter::network::ConfigDatabase* db, hunter::network::HttpClient* http);
    bool load(const std::string& path);  // TSV: id url remarks enabled last_update last_count last_error(escaped)
    bool save(const std::string& path) const;   // atomic: tmp+rename
    std::vector<Subscription> list() const;
    std::string add(const std::string& url, const std::string& remarks);  // "" on invalid; dedup by url -> returns existing id
    bool remove(const std::string& id);  // also removes DB records tagged "sub:<id>"
    bool setEnabled(const std::string& id, bool on);
    bool rename(const std::string& id, const std::string& remarks);
    // Fetch one sub (v2rayN UA like "v2rayN/7.x" is fine or hunter UA), parse body, import into db with
    // tag "sub:<id>". Returns imported count or -1; sets last_error on failure.
    int update(const std::string& id, const std::string& proxy_url, std::string* err);
    int updateAll(const std::string& proxy_url, std::string* err);  // enabled subs only
    bool updateDue(double now, double interval_s) const;           // any enabled sub stale?

private:
    int updateLocked(const std::string& id, const std::string& proxy_url, std::string* err);
    bool saveLocked(const std::string& path) const;
    void persistLocked() const;   // saveLocked(storage_path_) when set; caller holds mu_

    network::ConfigDatabase* db_ = nullptr;
    network::HttpClient* http_ = nullptr;
    std::vector<Subscription> subscriptions_;
    std::string storage_path_;
    mutable std::mutex mu_;
};

}  // namespace client
}  // namespace hunter