// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/rate_limiter.hpp
// Purpose : Per-key log rate limiting with suppressed-message accounting
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tsfs {

/**
 * @brief Lets at most @p burst messages per key and per @p window through.
 *
 * Used for repetitive failures (the same errno on thousands of files): the first
 * messages of a window are logged, the rest are counted, and the count is returned
 * with the first allowed message of the next window ("N similar messages
 * suppressed"). Keys are short strings such as "gc_delete_failed:13". Once the table
 * holds @p max_keys keys it is cleared at the next window change; within one window
 * it grows with the number of distinct keys, which the callers bound (event x errno).
 * Thread-safe.
 */
class LogRateLimiter {
public:
    /// Verdict for one message.
    struct Verdict {
        bool allowed{false};
        std::uint64_t suppressed_before{0};  ///< Messages suppressed since the previous allowed one.
    };

    LogRateLimiter(std::uint32_t burst = 10, std::chrono::steady_clock::duration window = std::chrono::minutes(1),
                   std::size_t max_keys = 1024)
        : burst_(burst), window_(window), max_keys_(max_keys) {}

    /// Account one message for @p key at @p now.
    Verdict check(const std::string& key,
                  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::scoped_lock lock(mutex_);
        if (now - window_start_ >= window_) {
            window_start_ = now;
            if (entries_.size() >= max_keys_) {
                entries_.clear();
            }
            for (auto& [_, entry] : entries_) {
                entry.count = 0;
            }
        }
        Entry& entry = entries_[key];
        if (entry.count < burst_) {
            ++entry.count;
            return {true, std::exchange(entry.suppressed, 0)};
        }
        ++entry.suppressed;
        return {false, 0};
    }

    /// @return the total number of suppressed messages not yet reported.
    [[nodiscard]] std::uint64_t pending_suppressed() const {
        std::scoped_lock lock(mutex_);
        std::uint64_t total = 0;
        for (const auto& [_, entry] : entries_) {
            total += entry.suppressed;
        }
        return total;
    }

private:
    struct Entry {
        std::uint32_t count{0};
        std::uint64_t suppressed{0};
    };
    mutable std::mutex mutex_;
    std::uint32_t burst_;
    std::chrono::steady_clock::duration window_;
    std::size_t max_keys_;
    std::chrono::steady_clock::time_point window_start_{};
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace tsfs
