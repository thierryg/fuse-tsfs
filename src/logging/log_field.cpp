// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/log_field.cpp
// Purpose : Rendering of structured log events (text and JSON Lines)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "logging/log_field.hpp"

#include <cmath>
#include <format>

#include "common/escape.hpp"

namespace tsfs {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

void append_value(std::string& out, LogFormat format, const LogField::Value& value) {
    std::visit(Overloaded{
                   [&](std::string_view s) {
                       format == LogFormat::json ? append_json_quoted(out, s) : append_text_quoted(out, s);
                   },
                   [&](std::int64_t v) { std::format_to(std::back_inserter(out), "{}", v); },
                   [&](std::uint64_t v) { std::format_to(std::back_inserter(out), "{}", v); },
                   [&](double v) {
                       // JSON has no NaN/Inf literal: emit null rather than an invalid document.
                       if (!std::isfinite(v) && format == LogFormat::json) {
                           out += "null";
                       } else {
                           std::format_to(std::back_inserter(out), "{:.3f}", v);
                       }
                   },
                   [&](bool v) { out += v ? "true" : "false"; },
               },
               value);
}

void append_key(std::string& out, LogFormat format, std::string_view key, bool first) {
    if (format == LogFormat::json) {
        if (!first) {
            out += ',';
        }
        out += '"';
        out += key;
        out += "\":";
    } else {
        if (!first) {
            out += ' ';
        }
        out += key;
        out += '=';
    }
}

}  // namespace

std::string render_event(LogFormat format, std::string_view component, std::string_view event, std::string_view message,
                         std::span<const LogField> fields) {
    std::string out;
    out.reserve(96 + message.size() + fields.size() * 24);
    // component and event are trusted identifiers ([a-z_]): unquoted in the text format
    // so that "grep event=gc_file_deleted" works; always quoted in JSON.
    append_key(out, format, "component", true);
    if (format == LogFormat::json) {
        append_value(out, format, component);
    } else {
        out += component;
    }
    append_key(out, format, "event", false);
    if (format == LogFormat::json) {
        append_value(out, format, event);
    } else {
        out += event;
    }
    for (const LogField& field : fields) {
        append_key(out, format, field.key, false);
        append_value(out, format, field.value);
    }
    if (!message.empty()) {
        append_key(out, format, "msg", false);
        append_value(out, format, message);
    }
    return out;
}

}  // namespace tsfs
