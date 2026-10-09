#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace hunter {
namespace core {

// Latest-N discovered config URIs, newest-first, persisted as TSV.
// Thread-safe; the validator calls add() from many threads.
class FoundHistory {
public:
    struct Entry {
        double seen_at = 0.0;
        std::string uri;
    };

    static constexpr size_t kCapacity = 200;

    FoundHistory() = default;

    void add(const std::string& uri, double now);
    std::vector<Entry> snapshot() const;
    size_t size() const;
    void clear();

    bool load(const std::string& path);
    bool save(const std::string& path) const;

private:
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;  // newest first
};

}  // namespace core
}  // namespace hunter
