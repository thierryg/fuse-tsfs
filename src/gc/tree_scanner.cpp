// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/tree_scanner.cpp
// Purpose : Resumable, time-sliced, descriptor-relative traversal of the backing tree
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "gc/tree_scanner.hpp"

#include <cerrno>

#include "common/managed_path.hpp"

namespace tsfs {

void TreeScanner::reset() {
    started_ = false;
    pending_.clear();
    current_.reset();
    current_path_.clear();
}

bool TreeScanner::run(std::chrono::steady_clock::time_point deadline, const Visitor& visit,
                      const ErrorHandler& on_error) {
    if (!started_) {
        started_ = true;
        totals_ = {};
        pending_.assign(1, "/");
    }
    // The clock is read every kCheckEvery entries: cheap, and slices stay accurate.
    constexpr unsigned kCheckEvery = 64;
    unsigned since_check = 0;
    for (;;) {
        // Counted per loop iteration (entries and directory openings alike), so that
        // a tree of empty directories cannot overrun the slice either.
        if (++since_check >= kCheckEvery) {
            since_check = 0;
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
        }
        if (!current_) {
            if (pending_.empty()) {
                started_ = false;
                return true;
            }
            current_path_ = std::move(pending_.front());
            pending_.pop_front();
            auto fd = store_.open_directory(current_path_);
            if (!fd) {
                const int err = fd.error().code;
                if (err == EXDEV || err == ELOOP || err == ENOTDIR) {
                    ++totals_.boundaries;  // Mount point, or a directory swapped for a link.
                } else if (err != ENOENT) {
                    ++totals_.errors;
                    on_error(current_path_, err);
                }
                continue;
            }
            DIR* dir = ::fdopendir(fd->get());
            if (dir == nullptr) {
                ++totals_.errors;
                on_error(current_path_, errno);
                continue;
            }
            fd->release();  // Now owned by the DIR stream.
            current_.reset(dir);
            ++totals_.directories;
        }
        errno = 0;
        const dirent* entry = ::readdir(current_.get());
        if (entry == nullptr) {
            if (errno != 0) {
                ++totals_.errors;
                on_error(current_path_, errno);
            }
            current_.reset();
            continue;
        }
        const std::string_view name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        std::string child = managed_path::join(current_path_, name);
        if (entry->d_type == DT_DIR) {
            pending_.push_back(std::move(child));
        } else if (entry->d_type == DT_LNK) {
            ++totals_.symlinks;
        } else if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
            auto st = stat_entry(::dirfd(current_.get()), entry->d_name, want_birth_);
            if (!st) {
                if (st.error().code != ENOENT) {  // Vanished between readdir and stat: normal.
                    ++totals_.errors;
                    on_error(child, st.error().code);
                }
            } else if (st->is_regular()) {
                ++totals_.files;
                visit(child, *st);
            } else if (st->is_directory()) {
                pending_.push_back(std::move(child));
            } else if (S_ISLNK(st->mode)) {
                ++totals_.symlinks;
            } else {
                ++totals_.others;
            }
        } else {
            ++totals_.others;
        }
    }
}

}  // namespace tsfs
