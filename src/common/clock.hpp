// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/clock.hpp
// Purpose : Injectable wall clock (system clock in production, manual clock in tests)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>

namespace tsfs {

using WallClock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<WallClock, std::chrono::milliseconds>;

/**
 * @brief Source of the current wall-clock time (dependency-injection seam).
 *
 * Expiration is computed from file timestamps, which are wall-clock values, so the
 * GC compares them with a wall clock. Waiting (sleeps, condition variables) always
 * uses std::chrono::steady_clock and is not affected by this interface.
 *
 * Implementations must be thread-safe.
 */
class Clock {
public:
    virtual ~Clock() = default;
    /// @return the current wall-clock time, millisecond resolution.
    [[nodiscard]] virtual TimePoint now() const = 0;
};

/// Production clock: std::chrono::system_clock.
class SystemClock final : public Clock {
public:
    [[nodiscard]] TimePoint now() const override {
        return std::chrono::time_point_cast<std::chrono::milliseconds>(WallClock::now());
    }
};

/// Test clock: time only moves when the test says so. Thread-safe.
class ManualClock final : public Clock {
public:
    explicit ManualClock(TimePoint start = TimePoint{std::chrono::hours(24 * 365 * 50)})
        : now_(start.time_since_epoch().count()) {}
    [[nodiscard]] TimePoint now() const override { return TimePoint{std::chrono::milliseconds(now_.load())}; }
    /// Move the clock by @p delta (may be negative to simulate a clock step backwards).
    void advance(std::chrono::milliseconds delta) { now_.fetch_add(delta.count()); }
    /// Set the clock to @p value.
    void set(TimePoint value) { now_.store(value.time_since_epoch().count()); }

private:
    std::atomic<std::chrono::milliseconds::rep> now_;
};

}  // namespace tsfs
