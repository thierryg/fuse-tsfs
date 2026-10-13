// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/entry_stat.cpp
// Purpose : statx() wrapper returning identity, type, size and timestamps without following links
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "fsutil/entry_stat.hpp"

#include <fcntl.h>
#include <sys/sysmacros.h>

#include <cerrno>
#include <cstring>

namespace tsfs {
namespace {

TimePoint to_time_point(const struct statx_timestamp& ts) {
    return TimePoint{std::chrono::milliseconds(ts.tv_sec * 1000LL + ts.tv_nsec / 1000000)};
}

}  // namespace

Result<EntryStat> stat_entry(int dirfd, const char* name, bool want_birth) {
    struct statx stx{};
    unsigned mask = STATX_TYPE | STATX_MODE | STATX_INO | STATX_SIZE | STATX_MTIME | STATX_NLINK;
    if (want_birth) {
        mask |= STATX_BTIME;
    }
    if (::statx(dirfd, name, AT_SYMLINK_NOFOLLOW | AT_STATX_SYNC_AS_STAT, mask, &stx) != 0) {
        const int err = errno;
        return make_error(err, std::strerror(err));
    }
    EntryStat entry;
    entry.identity.device = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    entry.identity.inode = stx.stx_ino;
    entry.mode = stx.stx_mode;
    entry.size = stx.stx_size;
    entry.links = stx.stx_nlink;
    entry.times.modification = to_time_point(stx.stx_mtime);
    if (want_birth && (stx.stx_mask & STATX_BTIME) != 0) {
        entry.times.birth = to_time_point(stx.stx_btime);
    }
    return entry;
}

}  // namespace tsfs
