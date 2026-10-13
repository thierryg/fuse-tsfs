// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/units.hpp
// Purpose : Validated parsing and formatting of durations ("1d12h") and sizes ("50MiB")
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/error.hpp"

namespace tsfs {

/// Millisecond resolution is the finest granularity exposed in the configuration.
using Duration = std::chrono::milliseconds;

/**
 * @brief Parse a duration such as "90s", "30m", "1d12h", "500ms" or "2w".
 *
 * Grammar: one or more `<digits><unit>` groups, units `ms`, `s`, `m`, `h`, `d`, `w`
 * (case-sensitive), each unit at most once and in decreasing order. No sign, no
 * whitespace, no fractional values. The total is checked for overflow.
 *
 * @param text The configuration value.
 * @return The duration, or an EINVAL error describing the problem (empty value,
 *         missing unit, unknown unit, overflow, unit repeated or out of order).
 */
Result<Duration> parse_duration(std::string_view text);

/**
 * @brief Format a duration with the largest exact units ("1d12h", "90s" -> "1m30s").
 * @return "0s" for zero.
 */
std::string format_duration(Duration value);

/**
 * @brief Parse a byte size such as "50MiB", "1GB", "4096" or "512B".
 *
 * Binary suffixes: KiB, MiB, GiB, TiB (powers of 1024). Decimal suffixes: KB, MB,
 * GB, TB (powers of 1000). "B" or no suffix means bytes. Suffixes are case-sensitive
 * to keep "MB" and "MiB" visibly different. Overflow and empty values are rejected.
 *
 * @return The size in bytes, or an EINVAL error.
 */
Result<std::uint64_t> parse_size(std::string_view text);

/// Format a byte count with the largest exact binary unit ("50MiB", "1536B" -> "1536B").
std::string format_size(std::uint64_t bytes);

}  // namespace tsfs
