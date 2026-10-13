// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/scheduler.hpp
// Purpose : Bounded expiration schedulers keyed by managed path (heap and ordered strategies)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/clock.hpp"
#include "config/config.hpp"
#include "fsutil/backing_store.hpp"

namespace tsfs {

/// A file the GC intends to examine at @ref due.
struct GcCandidate {
    std::string path;          ///< Managed path.
    FileIdentity identity;     ///< Identity observed when the candidate was evaluated.
    TimePoint due{};           ///< Expiry (or retry time).
    std::uint32_t attempt{0};  ///< Failed deletion attempts so far.
    int last_error{0};         ///< errno of the last failed attempt (0 if none).
};

/// Result of ExpirationScheduler::schedule().
enum class ScheduleResult { inserted, updated, rejected_full };

/**
 * @brief Strategy interface of the GC time queue (Strategy pattern).
 *
 * Entries are keyed by managed path: scheduling a path that is already present
 * replaces its entry (new identity, due time, attempt). Capacity is fixed: when
 * full, a new path is rejected (the periodic scan finds it again later), which
 * bounds memory regardless of the number of files on disk.
 *
 * Not thread-safe: the GarbageCollector serializes access with its own mutex.
 */
class ExpirationScheduler {
public:
    virtual ~ExpirationScheduler() = default;
    virtual ScheduleResult schedule(GcCandidate candidate) = 0;
    /// Remove @p path. @return true if it was present.
    virtual bool cancel(std::string_view path) = 0;
    /// @return the entry of @p path, or nullptr (valid until the next modification).
    [[nodiscard]] virtual const GcCandidate* find(std::string_view path) const = 0;
    /// @return the earliest due time, if any entry is present.
    [[nodiscard]] virtual std::optional<TimePoint> next_due() = 0;
    /// Move up to @p max entries due at or before @p now into @p out (earliest first).
    virtual std::size_t pop_due(TimePoint now, std::size_t max, std::vector<GcCandidate>& out) = 0;
    /// @return the number of live entries.
    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual std::size_t capacity() const = 0;
    virtual void clear() = 0;
};

/**
 * @brief Binary min-heap with lazy invalidation (default strategy).
 *
 * Live entries sit in slots addressed by an index map (path -> slot). The heap
 * holds (due, slot, generation) triples; cancelling or rescheduling bumps the
 * slot generation, which turns older heap triples into tombstones that pop_due()
 * discards. When tombstones outnumber live entries the heap is rebuilt in O(n),
 * so its size stays below 2 * live + 1024. schedule/cancel: O(log n) amortized,
 * next_due: O(1) amortized, memory: one path string per live entry.
 */
class HeapScheduler final : public ExpirationScheduler {
public:
    explicit HeapScheduler(std::size_t capacity) : capacity_(capacity) {}
    ScheduleResult schedule(GcCandidate candidate) override;
    bool cancel(std::string_view path) override;
    [[nodiscard]] const GcCandidate* find(std::string_view path) const override;
    [[nodiscard]] std::optional<TimePoint> next_due() override;
    std::size_t pop_due(TimePoint now, std::size_t max, std::vector<GcCandidate>& out) override;
    [[nodiscard]] std::size_t size() const override { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const override { return capacity_; }
    void clear() override;
    /// @return heap entries including tombstones (tests, metrics).
    [[nodiscard]] std::size_t heap_size() const noexcept { return heap_.size(); }

private:
    struct Slot {
        GcCandidate candidate;
        std::uint64_t generation{0};
        bool live{false};
    };
    struct HeapEntry {
        TimePoint due;
        std::uint32_t slot;
        std::uint64_t generation;
    };
    static bool later(const HeapEntry& a, const HeapEntry& b) noexcept { return a.due > b.due; }
    void push_heap(const HeapEntry& entry);
    void drop_tombstones();
    void maybe_compact();
    void release(std::uint32_t slot);

    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, std::uint32_t, StringHash, std::equal_to<>> index_;
    std::vector<HeapEntry> heap_;
};

/**
 * @brief Ordered tree (std::map keyed by (due, sequence)) plus a path index.
 *
 * Exact cancellation and update in O(log n) with no tombstones; higher constant
 * factor than the heap (node allocation per entry). Selectable with
 * garbage_collector.scheduler: ordered; compared in benchmarks/bench_scheduler.cpp.
 */
class OrderedScheduler final : public ExpirationScheduler {
public:
    explicit OrderedScheduler(std::size_t capacity) : capacity_(capacity) {}
    ScheduleResult schedule(GcCandidate candidate) override;
    bool cancel(std::string_view path) override;
    [[nodiscard]] const GcCandidate* find(std::string_view path) const override;
    [[nodiscard]] std::optional<TimePoint> next_due() override;
    std::size_t pop_due(TimePoint now, std::size_t max, std::vector<GcCandidate>& out) override;
    [[nodiscard]] std::size_t size() const override { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const override { return capacity_; }
    void clear() override;

private:
    using Key = std::pair<TimePoint, std::uint64_t>;
    std::size_t capacity_;
    std::uint64_t sequence_{0};
    std::map<Key, GcCandidate> by_due_;
    struct StringHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, Key, StringHash, std::equal_to<>> index_;
};

/// Factory: build the scheduler selected in the configuration.
std::unique_ptr<ExpirationScheduler> make_scheduler(SchedulerKind kind, std::size_t capacity);

}  // namespace tsfs
