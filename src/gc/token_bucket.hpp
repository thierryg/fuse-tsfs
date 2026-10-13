// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/token_bucket.hpp
// Purpose : Deletion rate limiter shared by the GC workers (stop-aware waits)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace tsfs {

/**
 * @brief Token bucket enforcing garbage_collector.max_deletions_per_second.
 *
 * Tokens refill continuously at @p rate per second up to a burst of
 * max(1, rate / 10), i.e. at most 100 ms worth of deletions can happen back to
 * back: the long-run rate never exceeds the limit and short bursts stay small.
 * A rate of 0 disables limiting. acquire() sleeps on a condition variable and
 * returns false as soon as the stop token is triggered (fast shutdown).
 * Thread-safe.
 */
class TokenBucket {
public:
    explicit TokenBucket(std::uint32_t rate = 0) { set_rate(rate); }

    void set_rate(std::uint32_t rate) {
        std::scoped_lock lock(mutex_);
        rate_ = rate;
        burst_ = std::max(1.0, rate / 10.0);
        tokens_ = std::min(tokens_, burst_);
        last_ = std::chrono::steady_clock::now();
        cv_.notify_all();
    }

    /// Take one token, waiting if needed. @return false if @p stop was requested.
    bool acquire(std::stop_token stop) {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (stop.stop_requested()) {
                return false;
            }
            if (rate_ == 0) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            tokens_ = std::min(burst_, tokens_ + std::chrono::duration<double>(now - last_).count() * rate_);
            last_ = now;
            if (tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }
            const auto wait = std::chrono::duration<double>((1.0 - tokens_) / rate_);
            cv_.wait_for(lock, stop, std::chrono::duration_cast<std::chrono::nanoseconds>(wait), [] { return false; });
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::uint32_t rate_{0};
    double burst_{1.0};
    double tokens_{1.0};
    std::chrono::steady_clock::time_point last_{std::chrono::steady_clock::now()};
};

}  // namespace tsfs
