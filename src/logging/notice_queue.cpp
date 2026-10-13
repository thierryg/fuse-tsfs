// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/notice_queue.cpp
// Purpose : Out-of-band reports from the log sink/compressor, and a rate-limited stderr fallback
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "logging/notice_queue.hpp"

#include <unistd.h>

#include <cerrno>
#include <format>

namespace tsfs {
namespace {

void write_stderr(std::string_view text) {
    while (!text.empty()) {
        const ssize_t n = ::write(STDERR_FILENO, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;  // Nothing else can be done: stderr itself is unusable.
        }
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

}  // namespace

void NoticeQueue::post(LogNotice notice) {
    std::scoped_lock lock(mutex_);
    if (queue_.size() >= capacity_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_.push_back(std::move(notice));
}

std::vector<LogNotice> NoticeQueue::drain() {
    std::scoped_lock lock(mutex_);
    std::vector<LogNotice> out(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    queue_.clear();
    return out;
}

void StderrFallback::report(std::string_view line) {
    std::scoped_lock lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start_ >= window_) {
        if (suppressed_ > 0) {
            write_stderr(std::format("tsfs: {} similar logging error message(s) suppressed\n", suppressed_));
        }
        window_start_ = now;
        in_window_ = 0;
        suppressed_ = 0;
    }
    if (in_window_ >= burst_) {
        ++suppressed_;
        return;
    }
    ++in_window_;
    std::string out = "tsfs: ";
    out += line;
    out += '\n';
    write_stderr(out);
}

StderrFallback& stderr_fallback() {
    static StderrFallback instance;
    return instance;
}

}  // namespace tsfs
