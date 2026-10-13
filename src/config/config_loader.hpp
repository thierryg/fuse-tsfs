// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_loader.hpp
// Purpose : YAML parsing, strict schema validation and serialization of the configuration
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/stat.h>

#include <string>
#include <string_view>

#include "common/error.hpp"
#include "config/config.hpp"

namespace tsfs {

/**
 * @brief Parse and validate a configuration document.
 *
 * The whole document is validated before anything is returned: unknown keys,
 * duplicate keys, wrong types, invalid durations/sizes/paths/patterns, out-of-range
 * numbers and inconsistent settings are rejected. Missing optional keys take the
 * documented defaults. The error message has the form
 * `<source>:<line>:<column>: <key>: <reason>`.
 *
 * Thread-safe (no shared state). Never throws.
 *
 * @param yaml_text   The YAML document.
 * @param source_name Name used in error messages (usually the file path).
 * @return The configuration, or EINVAL with a located message.
 */
Result<Config> parse_config(std::string_view yaml_text, const std::string& source_name);

/**
 * @brief Read @p path and parse it with parse_config().
 * @return The configuration, or the errno of the read failure (ENOENT, EACCES...)
 *         or EINVAL for a validation failure.
 */
Result<Config> load_config_file(const std::string& path);

/**
 * @brief Trust check of the configuration file opened by a privileged daemon.
 *
 * The configuration decides what a root process deletes and where it creates and
 * rotates files, so when @p euid is 0 the file must be a regular file owned by root
 * and not writable by its group or by others. Unprivileged daemons only need a
 * regular file (they cannot gain anything from their own configuration).
 * @param st   fstat() of the opened file (symbolic links already followed).
 * @param euid Effective uid of the daemon.
 * @return EACCES (with the reason) or EINVAL (not a regular file) on failure.
 */
Result<void> check_config_file_trust(const struct stat& st, uid_t euid, const std::string& path);

/**
 * @brief Normalize and validate filesystem paths set outside the file (-m, -b).
 *
 * Applies the same rules as the file: absolute normalized paths, neither may be "/",
 * they must not contain each other, and the log directory, crash directory and
 * status file must stay outside both.
 * @return EINVAL with the reason on failure; @p config is normalized on success.
 */
Result<void> validate_path_overrides(Config& config);

/**
 * @brief Serialize a configuration as YAML (every key, defaults included).
 *
 * The output is itself a valid configuration: parse_config(dump_config(c)) yields
 * an equivalent Config. Used by `tsfs --show-config`.
 */
std::string dump_config(const Config& config);

}  // namespace tsfs
