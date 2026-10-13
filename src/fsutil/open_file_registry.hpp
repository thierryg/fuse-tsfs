// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/open_file_registry.hpp
// Purpose : Count of files currently open through the mount, by inode identity
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "fsutil/backing_store.hpp"

namespace tsfs {

/// Hash of a FileIdentity (device and inode mixed).
struct FileIdentityHash {
    std::size_t operator()(const FileIdentity& id) const noexcept {
        const std::uint64_t h = static_cast<std::uint64_t>(id.inode) * 0x9e3779b97f4a7c15ULL;
        return h ^ (static_cast<std::uint64_t>(id.device) + (h << 6U) + (h >> 2U));
    }
};

/**
 * @brief Which inodes are open through TSFS right now (open/create ... release).
 *
 * The GC consults it (garbage_collector.skip_open_files) so that a file being read
 * or written through the mount is never deleted underneath its user; it is
 * retried later instead. Sharded (64 mutex-protected maps) so that concurrent
 * open/release calls on different files rarely contend.
 * Thread-safe.
 */
class OpenFileRegistry {
public:
    void add(const FileIdentity& id) {
        Shard& shard = shard_for(id);
        std::scoped_lock lock(shard.mutex);
        ++shard.counts[id];
        total_.fetch_add(1, std::memory_order_relaxed);
    }

    void remove(const FileIdentity& id) {
        Shard& shard = shard_for(id);
        std::scoped_lock lock(shard.mutex);
        const auto it = shard.counts.find(id);
        if (it == shard.counts.end()) {
            return;
        }
        if (--it->second == 0) {
            shard.counts.erase(it);
        }
        total_.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_open(const FileIdentity& id) const {
        const Shard& shard = shard_for(id);
        std::scoped_lock lock(shard.mutex);
        return shard.counts.contains(id);
    }

    /// @return the number of open handles (approximate under concurrency).
    [[nodiscard]] std::uint64_t handles() const noexcept { return total_.load(std::memory_order_relaxed); }

private:
    struct alignas(64) Shard {
        mutable std::mutex mutex;
        std::unordered_map<FileIdentity, std::uint32_t, FileIdentityHash> counts;
    };
    Shard& shard_for(const FileIdentity& id) { return shards_[FileIdentityHash{}(id) % shards_.size()]; }
    const Shard& shard_for(const FileIdentity& id) const { return shards_[FileIdentityHash{}(id) % shards_.size()]; }

    std::array<Shard, 64> shards_;
    std::atomic<std::uint64_t> total_{0};
};

}  // namespace tsfs
