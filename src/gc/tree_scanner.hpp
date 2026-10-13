// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/tree_scanner.hpp
// Purpose : Resumable, time-sliced, descriptor-relative traversal of the backing tree
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <dirent.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "fsutil/backing_store.hpp"
#include "fsutil/entry_stat.hpp"

namespace tsfs {

/// Counters of one complete pass.
struct ScanTotals {
    std::uint64_t files{0};        ///< Regular files visited.
    std::uint64_t directories{0};  ///< Directories listed.
    std::uint64_t symlinks{0};     ///< Symbolic links seen (never followed, never deleted).
    std::uint64_t others{0};       ///< FIFOs, sockets, devices (ignored).
    std::uint64_t errors{0};       ///< Entries or directories that could not be read.
    std::uint64_t boundaries{0};   ///< Directories refused (mount point, symlink swap: EXDEV/ELOOP).
};

/**
 * @brief Walks the backing tree breadth-first in bounded time slices.
 *
 * A pass starts at "/" and visits every regular file once, calling the visitor with
 * its managed path and statx() data. run() returns when the deadline is reached
 * and the next call resumes exactly where it stopped, even in the middle of a huge
 * directory (the directory stream stays open between slices), so a tree of
 * millions of entries never blocks the GC coordinator for longer than one slice
 * (garbage_collector.max_scan_duration). Memory: the queue of directories still to
 * list, not the files.
 *
 * Directories are opened through BackingStore (no symlink, no mount crossing).
 * Symbolic links and special files are counted and skipped.
 * Not thread-safe: owned and driven by the GC coordinator thread.
 */
class TreeScanner {
public:
    using Visitor = std::function<void(std::string_view path, const EntryStat& stat)>;
    using ErrorHandler = std::function<void(std::string_view path, int error)>;

    TreeScanner(const BackingStore& store, bool want_birth) : store_(store), want_birth_(want_birth) {}

    /// Abandon the current pass; the next run() starts a new one from "/".
    void reset();
    /// @return true while a pass is started and not finished.
    [[nodiscard]] bool in_progress() const noexcept { return started_; }
    /// Request (or stop requesting) btime for the next entries.
    void set_want_birth(bool want) noexcept { want_birth_ = want; }

    /**
     * @brief Advance the pass until it completes or @p deadline passes.
     * @return true when the pass completed (totals() then describes it).
     */
    bool run(std::chrono::steady_clock::time_point deadline, const Visitor& visit, const ErrorHandler& on_error);

    /// @return counters of the pass in progress or of the pass that just completed.
    [[nodiscard]] const ScanTotals& totals() const noexcept { return totals_; }

private:
    struct DirCloser {
        void operator()(DIR* dir) const noexcept { ::closedir(dir); }
    };

    const BackingStore& store_;
    bool want_birth_;
    bool started_{false};
    std::deque<std::string> pending_;
    std::string current_path_;
    std::unique_ptr<DIR, DirCloser> current_;
    ScanTotals totals_;
};

}  // namespace tsfs
