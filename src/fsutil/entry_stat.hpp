// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/entry_stat.hpp
// Purpose : statx() wrapper returning identity, type, size and timestamps without following links
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/stat.h>

#include <cstdint>

#include "common/error.hpp"
#include "fsutil/backing_store.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs {

/// What the GC needs to know about a directory entry.
struct EntryStat {
    FileIdentity identity;
    mode_t mode{0};
    std::uint64_t size{0};
    std::uint64_t links{0};
    FileTimes times;

    [[nodiscard]] bool is_regular() const noexcept { return S_ISREG(mode); }
    [[nodiscard]] bool is_directory() const noexcept { return S_ISDIR(mode); }
};

/**
 * @brief statx(@p dirfd, @p name, AT_SYMLINK_NOFOLLOW) — never follows a link.
 * @param want_birth Request STATX_BTIME; times.birth stays empty when the
 *                   filesystem does not report it.
 * @return the entry, or the errno (ENOENT when it vanished).
 */
Result<EntryStat> stat_entry(int dirfd, const char* name, bool want_birth);

}  // namespace tsfs
