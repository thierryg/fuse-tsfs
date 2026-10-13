// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/fuse_control.hpp
// Purpose : The kernel FUSE connection of a mount (fusectl: /sys/fs/fuse/connections)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tsfs {

/// Where the fusectl filesystem exposes one directory per FUSE connection.
inline constexpr std::string_view kFuseConnectionsDir = "/sys/fs/fuse/connections";

/**
 * @brief Find the FUSE connection identifier of the mount at @p mountpoint.
 *
 * The identifier names the connection directory
 * /sys/fs/fuse/connections/<id>/ (files "waiting", "abort", "max_background",
 * "congestion_threshold"). The kernel names it after the device number of the
 * mount, in the kernel encoding (major << 20 | minor; FUSE mounts use an anonymous
 * device, major 0, so the identifier is the minor number, also visible as
 * `stat -c %d MOUNTPOINT`).
 *
 * @param mountinfo  Content of /proc/self/mountinfo.
 * @param mountpoint Normalized absolute mount point.
 * @return the identifier of the last (topmost) FUSE mount on @p mountpoint, or
 *         nullopt when there is none. Pure function: it never touches the mount
 *         (a stat() of its own mount from the daemon would send it a request).
 */
std::optional<std::uint32_t> fuse_connection_id(std::string_view mountinfo, std::string_view mountpoint);

/// fuse_connection_id() on the current /proc/self/mountinfo.
std::optional<std::uint32_t> fuse_connection_id(std::string_view mountpoint);

/**
 * @brief Read /sys/fs/fuse/connections/<id>/waiting: requests sent by the kernel
 *        and not answered yet by the daemon.
 * @return nullopt when fusectl is not mounted (containers often lack it) or the
 *         file cannot be read (the file belongs to the user who mounted).
 */
std::optional<std::uint64_t> fuse_connection_waiting(std::uint32_t id,
                                                     std::string_view connections_dir = kFuseConnectionsDir);

}  // namespace tsfs
