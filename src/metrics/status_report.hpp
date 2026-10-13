// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/metrics/status_report.hpp
// Purpose : Runtime status document (JSON) assembled from every component, written atomically
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "common/error.hpp"
#include "filesystem/passthrough_fs.hpp"
#include "gc/garbage_collector.hpp"
#include "logging/logger.hpp"

namespace tsfs {

/// Everything published in the status file.
struct StatusInputs {
    std::string version;
    std::string mountpoint;
    std::string backing_directory;
    std::int64_t pid{0};
    std::int64_t started_unix_ms{0};
    std::int64_t now_unix_ms{0};
    std::uint64_t config_generation{0};
    std::uint64_t config_reloads{0};
    std::uint64_t config_reload_failures{0};
    GcStats gc;
    FsStats fs;
    LoggerStats logging;
};

/**
 * @brief Render the status as one JSON document.
 *
 * Schema (stable keys, documented in docs/user/monitoring.md): "version", "pid",
 * "uptime_s", "mountpoint", "backing_directory", "config" {generation, reloads,
 * reload_failures}, "gc" {...GcStats}, "filesystem" {"operations": {op: {calls,
 * errors}}, bytes_read, bytes_written, open_handles}, "logging" {...}.
 */
std::string render_status_json(const StatusInputs& inputs);

/**
 * @brief Write @p content to @p path atomically (temporary file in the same
 *        directory, then rename), mode 0640, creating the directory (0750) if needed.
 *
 * Readers (tsfs --status, monitoring agents) never see a partial document. No IPC
 * endpoint is opened: access control is the file mode and the directory.
 */
Result<void> write_file_atomically(const std::string& path, const std::string& content);

}  // namespace tsfs
