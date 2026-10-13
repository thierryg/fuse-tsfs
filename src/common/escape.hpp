// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/escape.hpp
// Purpose : Lossless, injection-safe representation of untrusted strings (paths) in logs
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <string>
#include <string_view>

namespace tsfs {

/**
 * @brief Append @p value to @p out as a double-quoted text-log value.
 *
 * Escapes: `"` -> `\"`, `\` -> `\\`, newline/tab/CR -> `\n` `\t` `\r`, any other
 * control byte (< 0x20, 0x7f) and every byte that is not part of valid UTF-8 ->
 * `\xNN`. Valid UTF-8 is copied as is. The result never contains a raw newline,
 * so a crafted file name cannot forge a log line.
 */
void append_text_quoted(std::string& out, std::string_view value);

/**
 * @brief Append @p value to @p out as a JSON string literal (with quotes).
 *
 * Control characters become `\uXXXX` (or the short forms). Bytes that are not valid
 * UTF-8 cannot be represented in JSON; they are written as the six characters
 * `\\xNN` (an escaped backslash followed by "xNN"), so decoding the JSON yields the
 * text `\xNN` and the original bytes remain recoverable.
 */
void append_json_quoted(std::string& out, std::string_view value);

/// @return the length of the valid UTF-8 sequence starting at @p s[0], or 0 if invalid.
std::size_t utf8_sequence_length(std::string_view s) noexcept;

}  // namespace tsfs
