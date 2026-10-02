#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>
#include "core/utils.h"  // for utils::LogRingBuffer::isLogSuppressed()

namespace hunter {
namespace gui {

/**
 * @brief Fixed-capacity ring buffer for live log capture.
 *
 * Total storage is hard-capped at max_bytes_ (default 10 KB). When the
 * buffer would exceed the cap, the oldest lines are evicted whole — we
 * never cut a line in half, so the display always starts at a line
 * boundary. This keeps memory usage bounded regardless of how verbose
 * the orchestrator gets, which matters on memory-constrained machines.
 *
 * Thread-safe: the orchestrator runs on its own thread and writes via
 * the streambuf interface; the GUI thread reads via getLines().
 */
class LogRingBuffer : public std::streambuf {
public:
    explicit LogRingBuffer(size_t max_bytes = 10 * 1024)
        : max_bytes_(max_bytes) {
        // Reserve a small staging buffer for overflow() — one line at a
        // time is plenty; we flush on newline.
        buffer_.resize(256);
        setp(buffer_.data(), buffer_.data() + buffer_.size());
    }

    /// Get a snapshot of all complete lines currently in the ring.
    /// Returned as a single string with '\n' separators (no trailing newline).
    std::string getLines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out;
        out.reserve(current_bytes_);
        for (const auto& line : lines_) {
            out += line;
            out += '\n';
        }
        if (!out.empty()) out.pop_back(); // drop trailing newline
        return out;
    }

    /// Get a snapshot of all complete lines as a vector (one entry per line).
    /// This is the preferred API for the GUI — each line can be rendered
    /// individually with ImGui::TextUnformatted, which gives proper
    /// line-by-line rendering instead of relying on InputTextMultiline.
    std::vector<std::string> getLinesVector() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    /// Get the number of complete lines stored.
    size_t lineCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

    /// Total bytes currently stored (<= max_bytes_).
    size_t bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_bytes_;
    }

    /// Clear all stored lines.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
        current_bytes_ = 0;
    }

private:
    /// streambuf overflow: grow the internal line staging buffer if needed.
    int_type overflow(int_type ch) override {
        if (ch == traits_type::eof()) return traits_type::eof();
        // If the put area is full, grow it.
        if (pptr() == epptr()) {
            size_t old_size = buffer_.size();
            buffer_.resize(old_size * 2);
            setp(buffer_.data(), buffer_.data() + buffer_.size());
            pbump(static_cast<int>(old_size));
        }
        *pptr() = static_cast<char>(ch);
        pbump(1);
        if (ch == '\n') sync();
        return ch;
    }

    /// streambuf sync (flush): move staged bytes into the ring as complete lines.
    int sync() override {
        size_t n = pptr() - pbase();
        if (n == 0) return 0;

        // Check the global suppress flag — when set, discard all incoming
        // data. This is used by printDashboard() to prevent the terminal
        // dashboard frame (ANSI clear-screen + TUI layout) from polluting
        // the GUI log panel. The dashboard is a terminal UI element, not
        // a log message.
        if (utils::LogRingBuffer::instance().isLogSuppressed()) {
            setp(buffer_.data(), buffer_.data() + buffer_.size());
            return 0;
        }

        std::string chunk(pbase(), n);
        // Reset put area for next line.
        setp(buffer_.data(), buffer_.data() + buffer_.size());

        std::lock_guard<std::mutex> lock(mutex_);
        // Split chunk on '\n' and feed each piece as a separate line.
        size_t start = 0;
        for (size_t i = 0; i < chunk.size(); ++i) {
            if (chunk[i] == '\n') {
                appendToCurrentLine(chunk.substr(start, i - start));
                finishLine();
                start = i + 1;
            }
        }
        if (start < chunk.size()) {
            appendToCurrentLine(chunk.substr(start));
        }
        return 0;
    }

    void appendToCurrentLine(const std::string& s) {
        if (lines_.empty()) lines_.emplace_back();
        lines_.back() += s;
        current_bytes_ += s.size();
    }

    void finishLine() {
        // Account for the newline byte itself.
        current_bytes_ += 1;
        evictIfNeeded();
    }

    void evictIfNeeded() {
        while (current_bytes_ > max_bytes_ && !lines_.empty()) {
            current_bytes_ -= lines_.front().size() + 1; // +1 for newline
            lines_.erase(lines_.begin());
        }
    }

    mutable std::mutex mutex_;
    size_t max_bytes_;
    std::vector<std::string> lines_;  // each entry is one complete line (no '\n')
    size_t current_bytes_ = 0;
    std::vector<char> buffer_;        // streambuf put-area staging
};

/**
 * @brief RAII guard that redirects std::cout (and optionally stderr) to a
 *        LogRingBuffer for the duration of its lifetime.
 *
 * On construction, swaps std::cout's rdbuf to the ring buffer (keeping a
 * backup of the original). On destruction, restores the original rdbuf.
 * This lets the GUI capture all orchestrator logging without modifying
 * any orchestrator code — every std::cout << ... goes through us.
 */
class LogCapture {
public:
    explicit LogCapture(LogRingBuffer& ring, bool capture_stderr = false)
        : ring_(ring), capture_stderr_(capture_stderr) {
        old_cout_buf_ = std::cout.rdbuf(&ring_);
        if (capture_stderr_) {
            old_cerr_buf_ = std::cerr.rdbuf(&ring_);
        }
    }

    ~LogCapture() {
        std::cout.flush();
        std::cout.rdbuf(old_cout_buf_);
        if (capture_stderr_ && old_cerr_buf_) {
            std::cerr.flush();
            std::cerr.rdbuf(old_cerr_buf_);
        }
    }

    LogCapture(const LogCapture&) = delete;
    LogCapture& operator=(const LogCapture&) = delete;

private:
    LogRingBuffer& ring_;
    bool capture_stderr_;
    std::streambuf* old_cout_buf_ = nullptr;
    std::streambuf* old_cerr_buf_ = nullptr;
};

} // namespace gui
} // namespace hunter
