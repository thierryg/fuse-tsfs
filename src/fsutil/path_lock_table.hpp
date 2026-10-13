// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/path_lock_table.hpp
// Purpose : Striped per-path mutexes serializing namespace changes with GC deletions
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

#include "common/managed_path.hpp"

namespace tsfs {

/**
 * @brief Fixed array of mutexes indexed by a hash of the managed path.
 *
 * Purpose: close the race between a GC deletion and a change made *through the
 * mount* to the same name (open, create, rename, unlink, truncate, utimens).
 * - FUSE operations lock the stripe(s) of the path(s) they modify (one or two).
 * - The GC locks the stripes of the candidate *and of every ancestor directory*
 *   while it re-validates and unlinks, so that renaming a parent directory (which
 *   locks the parent's stripe) cannot move the file under a different policy
 *   between validation and unlink.
 * Deadlock freedom: every holder of several stripes acquires them in increasing
 * index order, and a stripe is locked at most once per holder (indices deduplicated).
 * Stripes are shared by unrelated paths (hash collisions): that only costs some
 * contention, never correctness.
 *
 * Changes made directly on the backing directory by other processes are not
 * covered: see docs/security/threat-model.md (residual TOCTOU window).
 */
class PathLockTable {
public:
    static constexpr std::size_t kStripes = 1024;

    /// RAII holder of a set of stripes, released in reverse order.
    class Guard {
    public:
        Guard() = default;
        Guard(Guard&& other) noexcept
            : table_(std::exchange(other.table_, nullptr)), indices_(std::move(other.indices_)) {}
        Guard& operator=(Guard&&) = delete("guards are not reassigned");
        Guard(const Guard&) = delete("a lock has a single owner");
        ~Guard() {
            if (table_ != nullptr) {
                for (auto it = indices_.rbegin(); it != indices_.rend(); ++it) {
                    table_->stripes_[*it].unlock();
                }
            }
        }

    private:
        friend class PathLockTable;
        Guard(PathLockTable* table, std::vector<std::size_t> indices) : table_(table), indices_(std::move(indices)) {
            std::ranges::sort(indices_);
            const auto [first, last] = std::ranges::unique(indices_);
            indices_.erase(first, last);
            for (const std::size_t i : indices_) {
                table_->stripes_[i].lock();
            }
        }
        PathLockTable* table_{nullptr};
        std::vector<std::size_t> indices_;
    };

    /// @return the stripe index of @p path.
    [[nodiscard]] static std::size_t index(std::string_view path) noexcept {
        return std::hash<std::string_view>{}(path) % kStripes;
    }

    /// Lock the stripes of the given paths (any number, any order).
    template <typename... Paths>
    [[nodiscard]] Guard lock(const Paths&... paths) {
        return Guard(this, {index(std::string_view(paths))...});
    }

    /// Lock @p path and every ancestor directory up to "/" (GC deletion protocol).
    [[nodiscard]] Guard lock_with_ancestors(std::string_view path) {
        std::vector<std::size_t> indices;
        indices.reserve(managed_path::depth(path) + 1);
        std::string_view current = path;
        for (;;) {
            indices.push_back(index(current));
            if (current == "/") {
                break;
            }
            current = managed_path::parent(current);
        }
        return Guard(this, std::move(indices));
    }

private:
    std::array<std::mutex, kStripes> stripes_;
};

}  // namespace tsfs
