// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/metrics/sharded_counter.hpp
// Purpose : Contention-free counters for hot paths (per-thread shards summed on read)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>

namespace tsfs {

/**
 * @brief A family of @p N counters split into cache-line-sized shards.
 *
 * A single global atomic incremented by every FUSE thread bounces its cache line
 * between cores. Here each thread increments the shard selected by a hash of its
 * id (relaxed, uncontended in practice) and readers sum the shards. Reads are not
 * a consistent snapshot across counters, which is acceptable for statistics.
 * Thread-safe.
 *
 * @tparam N Number of counters (e.g. one per FUSE operation).
 */
template <std::size_t N>
class ShardedCounters {
public:
    /// Add @p delta to counter @p index.
    void add(std::size_t index, std::uint64_t delta = 1) noexcept {
        shards_[shard_index()].values[index].fetch_add(delta, std::memory_order_relaxed);
    }

    /// @return the sum of counter @p index over every shard.
    [[nodiscard]] std::uint64_t sum(std::size_t index) const noexcept {
        std::uint64_t total = 0;
        for (const Shard& shard : shards_) {
            total += shard.values[index].load(std::memory_order_relaxed);
        }
        return total;
    }

private:
    static constexpr std::size_t kShards = 32;
    struct alignas(64) Shard {
        std::array<std::atomic<std::uint64_t>, N> values{};
    };
    static std::size_t shard_index() noexcept {
        thread_local const std::size_t index = std::hash<std::thread::id>{}(std::this_thread::get_id()) % kShards;
        return index;
    }
    std::array<Shard, kShards> shards_{};
};

}  // namespace tsfs
