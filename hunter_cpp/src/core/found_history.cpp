#include "core/found_history.h"

#include <fstream>

namespace hunter {
namespace core {

void FoundHistory::add(const std::string& uri, double now) {
    if (uri.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->uri == uri) {
            entries_.erase(it);
            break;
        }
    }
    entries_.push_front(Entry{now, uri});
    while (entries_.size() > kCapacity) entries_.pop_back();
}

std::vector<FoundHistory::Entry> FoundHistory::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<Entry>(entries_.begin(), entries_.end());
}

size_t FoundHistory::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void FoundHistory::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

bool FoundHistory::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::deque<Entry> loaded;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        double t = 0.0;
        try {
            t = std::stod(line.substr(0, tab));
        } catch (...) {
            continue;
        }
        std::string uri = line.substr(tab + 1);
        if (uri.empty()) continue;
        if (loaded.size() >= kCapacity) break;
        loaded.emplace_back(Entry{t, uri});
    }
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.swap(loaded);
    return true;
}

bool FoundHistory::save(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const auto& e : entries_) {
        f << e.seen_at << '\t' << e.uri << '\n';
    }
    return static_cast<bool>(f);
}

}  // namespace core
}  // namespace hunter
