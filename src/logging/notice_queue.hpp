// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/notice_queue.hpp
// Purpose : Out-of-band reports from the log sink/compressor, and a rate-limited stderr fallback
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "config/config.hpp"

namespace tsfs {

/// An event raised inside the logging machinery (rotation, compression, write failure).
struct LogNotice {
    LogLevel level{LogLevel::info};
    std::string event;  ///< log_rotation_completed, log_write_failed...
    std::string message;
    std::string path;   ///< File concerned, may be empty.
    int error_code{0};  ///< errno, 0 if none.
};

/**
 * @brief Bounded queue of LogNotice.
 *
 * The sink and the compressor cannot call the logger (they run inside it: a call
 * would deadlock on the sink mutex or recurse on a full queue). They post notices
 * here instead; Logger::maintenance() logs them later from a normal thread.
 * When full, new notices are counted as dropped instead of growing the queue.
 * Thread-safe.
 */
class NoticeQueue {
public:
    explicit NoticeQueue(std::size_t capacity = 256) : capacity_(capacity) {}

    /// Post a notice (never blocks on logging, never allocates beyond capacity).
    void post(LogNotice notice);
    /// @return and remove every queued notice.
    std::vector<LogNotice> drain();
    /// @return notices dropped because the queue was full (cumulative).
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    std::mutex mutex_;
    std::deque<LogNotice> queue_;
    std::size_t capacity_;
    std::atomic<std::uint64_t> dropped_{0};
};

/**
 * @brief Last-resort reporting channel: one line on stderr, at most @p burst lines per
 *        @p window, then a single "suppressed N" line when the window rolls over.
 *
 * Uses write(2) on fd 2 directly (no logger, no stdio lock), so it is safe to call
 * from the sink while the sink mutex is held. Thread-safe.
 */
class StderrFallback {
public:
    StderrFallback(std::uint32_t burst = 5, std::chrono::seconds window = std::chrono::seconds(60))
        : burst_(burst), window_(window) {}
    void report(std::string_view line);

private:
    std::mutex mutex_;
    std::uint32_t burst_;
    std::chrono::seconds window_;
    std::chrono::steady_clock::time_point window_start_{};
    std::uint32_t in_window_{0};
    std::uint64_t suppressed_{0};
};

/// Process-wide fallback used by the logging subsystem (stateless apart from rate limiting).
StderrFallback& stderr_fallback();

}  // namespace tsfs
