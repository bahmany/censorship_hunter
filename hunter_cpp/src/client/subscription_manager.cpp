#include "client/subscription_manager.h"
#include "client/subscription_parse.h"
#include "network/import_filter.h"
#include "core/utils.h"
#include "core/constants.h"
#include "network/continuous_validator.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <cctype>
#include <functional>
#include <iomanip>

namespace hunter {
namespace client {

SubscriptionManager::SubscriptionManager(network::ConfigDatabase* db, network::HttpClient* http)
    : db_(db), http_(http) {}

void SubscriptionManager::bind(network::ConfigDatabase* db, network::HttpClient* http) {
    std::lock_guard<std::mutex> lock(mu_);
    db_ = db;
    http_ = http;
}

bool SubscriptionManager::load(const std::string& path) {
    std::lock_guard<std::mutex> lock(mu_);
    storage_path_ = path;  // persist-on-mutate uses this path from now on
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    subscriptions_.clear();
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string id, url, remarks, enabled_str, last_update_str, last_count_str, last_error;
        
        // Parse TSV fields
        if (!std::getline(iss, id, '\t')) continue;
        if (!std::getline(iss, url, '\t')) continue;
        if (!std::getline(iss, remarks, '\t')) continue;
        if (!std::getline(iss, enabled_str, '\t')) continue;
        if (!std::getline(iss, last_update_str, '\t')) continue;
        if (!std::getline(iss, last_count_str, '\t')) continue;
        std::getline(iss, last_error);  // Last field (may contain tabs if escaped)

        Subscription sub;
        sub.id = id;
        sub.url = url;
        sub.remarks = remarks;
        sub.enabled = (enabled_str == "true");
        try {
            sub.last_update = std::stod(last_update_str);
        } catch (...) {
            sub.last_update = 0;
        }
        try {
            sub.last_count = std::stoi(last_count_str);
        } catch (...) {
            sub.last_count = 0;
        }
        sub.last_error = last_error;

        subscriptions_.push_back(sub);
    }

    return true;
}

bool SubscriptionManager::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mu_);
    return saveLocked(path);
}

void SubscriptionManager::persistLocked() const {
    if (!storage_path_.empty()) saveLocked(storage_path_);
}

bool SubscriptionManager::saveLocked(const std::string& path) const {
    // Atomic save: write to temp file then rename
    std::string temp_path = path + ".tmp";
    std::ofstream file(temp_path);
    if (!file.is_open()) {
        return false;
    }

    for (const auto& sub : subscriptions_) {
        // Escape tabs and newlines in last_error for TSV format
        std::string escaped_error = sub.last_error;
        std::replace(escaped_error.begin(), escaped_error.end(), '\t', ' ');
        std::replace(escaped_error.begin(), escaped_error.end(), '\n', ' ');

        file << sub.id << '\t'
             << sub.url << '\t'
             << sub.remarks << '\t'
             << (sub.enabled ? "true" : "false") << '\t'
             << std::fixed << std::setprecision(0) << sub.last_update << '\t'
             << sub.last_count << '\t'
             << escaped_error << '\n';
    }

    file.close();
    if (!file) {
        return false;
    }

    // Rename temp file to actual file
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        return false;
    }

    return true;
}

std::vector<Subscription> SubscriptionManager::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subscriptions_;
}

std::string SubscriptionManager::add(const std::string& url, const std::string& remarks) {
    if (url.empty() || url.find("://") == std::string::npos) return "";
    std::lock_guard<std::mutex> lock(mu_);
    // Check for duplicate URL
    for (const auto& sub : subscriptions_) {
        if (sub.url == url) {
            return sub.id;  // Return existing ID
        }
    }

    // Generate ID from URL (8-hex of SHA-1)
    std::string id = utils::sha1Hex(url).substr(0, 8);

    Subscription sub;
    sub.id = id;
    sub.url = url;
    sub.remarks = remarks;
    if (sub.remarks.empty()) {
        // Derive remarks from URL host
        size_t pos = url.find("://");
        if (pos != std::string::npos) {
            pos += 3;
            size_t end = url.find('/', pos);
            if (end == std::string::npos) {
                end = url.length();
            }
            sub.remarks = url.substr(pos, end - pos);
        } else {
            sub.remarks = url;
        }
    }
    sub.enabled = true;
    sub.last_update = 0;
    sub.last_count = 0;
    sub.last_error.clear();

    subscriptions_.push_back(sub);
    persistLocked();
    return id;
}

bool SubscriptionManager::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                             [&id](const Subscription& sub) { return sub.id == id; });
    bool removed = (it != subscriptions_.end());
    if (removed) {
        subscriptions_.erase(it, subscriptions_.end());

        // Remove DB records imported from this subscription (tag "sub:<id>")
        if (db_) {
            const std::string want_tag = "sub:" + id;
            std::set<std::string> tagged_uris;
            db_->forEachRecord([&](const ConfigHealthRecord& rec) {
                if (rec.tag == want_tag) tagged_uris.insert(rec.uri);
            });
            if (!tagged_uris.empty()) db_->removeUris(tagged_uris);
        }
        persistLocked();
    }
    return removed;
}

bool SubscriptionManager::setEnabled(const std::string& id, bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& sub : subscriptions_) {
        if (sub.id == id) {
            sub.enabled = on;
            persistLocked();
            return true;
        }
    }
    return false;
}

bool SubscriptionManager::rename(const std::string& id, const std::string& remarks) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& sub : subscriptions_) {
        if (sub.id == id) {
            sub.remarks = remarks;
            persistLocked();
            return true;
        }
    }
    return false;
}

int SubscriptionManager::update(const std::string& id, const std::string& proxy_url, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    return updateLocked(id, proxy_url, err);
}

int SubscriptionManager::updateLocked(const std::string& id, const std::string& proxy_url, std::string* err) {
    if (!db_ || !http_) {
        if (err) *err = "Subscription manager not initialized";
        return -1;
    }
    // Find subscription
    auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                           [&id](const Subscription& sub) { return sub.id == id; });
    if (it == subscriptions_.end()) {
        if (err) *err = "Subscription not found";
        return -1;
    }

    Subscription& sub = *it;
    
    // Fetch subscription content (v2rayN-style UA; many endpoints gate on it)
    std::string body = http_->get(sub.url, 30000, proxy_url, std::string("Hunter/") + constants::HUNTER_VERSION);
    if (body.empty()) {
        sub.last_error = "Failed to fetch subscription";
        if (err) *err = sub.last_error;
        sub.last_update = utils::nowTimestamp();
        sub.last_count = 0;
        return -1;
    }

    // Parse subscription text
    SubscriptionParseResult result = parseSubscriptionText(std::move(body));
    if (result.uris.empty()) {
        sub.last_error = "No valid configs found in subscription";
        if (err) *err = sub.last_error;
        sub.last_update = utils::nowTimestamp();
        sub.last_count = 0;
        return -1;
    }

    // Import valid URIs into database with tag "sub:<id>" (importable schemes only)
    std::set<std::string> uris_set;
    for (const auto& u : result.uris) {
        if (network::isImportableProxyUri(u)) uris_set.insert(u);
    }
    int imported = db_ ? db_->addConfigsWithPriority(uris_set, "sub:" + id) : 0;
    
    // Update subscription stats
    sub.last_update = utils::nowTimestamp();
    sub.last_count = imported;
    sub.last_error.clear();
    persistLocked();

    if (err) *err = "";
    return imported;
}

int SubscriptionManager::updateAll(const std::string& proxy_url, std::string* err) {
    std::lock_guard<std::mutex> lock(mu_);
    int total_imported = 0;
    std::string first_error;

    for (auto& sub : subscriptions_) {
        if (!sub.enabled) continue;

        std::string sub_err;
        int imported = updateLocked(sub.id, proxy_url, &sub_err);
        if (imported >= 0) {
            total_imported += imported;
        } else {
            // Keep first error for reporting
            if (first_error.empty()) {
                first_error = sub_err;
            }
            // Update sub's last_error (already set in update())
        }
    }
    
    if (err) {
        if (!first_error.empty()) {
            *err = first_error;
        } else {
            *err = "";
        }
    }
    
    return total_imported;
}

bool SubscriptionManager::updateDue(double now, double interval_s) const {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& sub : subscriptions_) {
        if (sub.enabled && 
            (sub.last_update == 0 || (now - sub.last_update) >= interval_s)) {
            return true;
        }
    }
    return false;
}

}  // namespace client
}  // namespace hunter