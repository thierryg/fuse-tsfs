// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/log_field.hpp
// Purpose : Typed key/value fields of structured log events and their text/JSON rendering
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "config/config.hpp"

namespace tsfs {

/**
 * @brief One structured field of a log event.
 *
 * Holds non-owning views: a field must not outlive the strings it refers to, which
 * is always the case because events are formatted synchronously by Logger::log()
 * before it returns.
 */
struct LogField {
    using Value = std::variant<std::string_view, std::int64_t, std::uint64_t, double, bool>;

    std::string_view key;
    Value value;

    LogField(std::string_view k, std::string_view v) : key(k), value(v) {}
    LogField(std::string_view k, const std::string& v) : key(k), value(std::string_view(v)) {}
    LogField(std::string_view k, const char* v) : key(k), value(std::string_view(v)) {}
    LogField(std::string_view k, bool v) : key(k), value(v) {}
    LogField(std::string_view k, double v) : key(k), value(v) {}
    template <typename T>
        requires std::is_integral_v<T> && std::is_signed_v<T> && (!std::is_same_v<T, bool>)
    LogField(std::string_view k, T v) : key(k), value(static_cast<std::int64_t>(v)) {}
    template <typename T>
        requires std::is_integral_v<T> && std::is_unsigned_v<T> && (!std::is_same_v<T, bool>)
    LogField(std::string_view k, T v) : key(k), value(static_cast<std::uint64_t>(v)) {}
};

/**
 * @brief Render the variable part of an event ("component=... event=... k=v msg=...").
 *
 * Text: `component=gc event=gc_file_deleted path="/a b" bytes=12 msg="deleted"`; string
 * values are always quoted and escaped (append_text_quoted); keys, component and event
 * are trusted identifiers written as is.
 * JSON: `"component":"gc","event":"gc_file_deleted","path":"/a b","bytes":12,"msg":"deleted"`
 * (the surrounding object braces and the timestamp/level are added by the sink pattern).
 */
std::string render_event(LogFormat format, std::string_view component, std::string_view event, std::string_view message,
                         std::span<const LogField> fields);

}  // namespace tsfs
