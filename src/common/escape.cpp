// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/escape.cpp
// Purpose : Lossless, injection-safe representation of untrusted strings in logs
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "common/escape.hpp"

#include <array>
#include <cstdint>

namespace tsfs {
namespace {

constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

void append_hex_byte(std::string& out, unsigned char byte) {
    out += kHex[byte >> 4U];
    out += kHex[byte & 0x0fU];
}

}  // namespace

std::size_t utf8_sequence_length(std::string_view s) noexcept {
    if (s.empty()) {
        return 0;
    }
    const auto b0 = static_cast<unsigned char>(s[0]);
    if (b0 < 0x80) {
        return 1;
    }
    std::size_t length = 0;
    std::uint32_t min = 0;
    std::uint32_t cp = 0;
    if ((b0 & 0xe0U) == 0xc0U) {
        length = 2;
        min = 0x80;
        cp = b0 & 0x1fU;
    } else if ((b0 & 0xf0U) == 0xe0U) {
        length = 3;
        min = 0x800;
        cp = b0 & 0x0fU;
    } else if ((b0 & 0xf8U) == 0xf0U) {
        length = 4;
        min = 0x10000;
        cp = b0 & 0x07U;
    } else {
        return 0;
    }
    if (s.size() < length) {
        return 0;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(s[i]);
        if ((b & 0xc0U) != 0x80U) {
            return 0;
        }
        cp = (cp << 6U) | (b & 0x3fU);
    }
    // Reject overlong forms, UTF-16 surrogates and code points above U+10FFFF.
    if (cp < min || (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) {
        return 0;
    }
    return length;
}

void append_text_quoted(std::string& out, std::string_view value) {
    out += '"';
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                ++i;
                continue;
            case '\\':
                out += "\\\\";
                ++i;
                continue;
            case '\n':
                out += "\\n";
                ++i;
                continue;
            case '\t':
                out += "\\t";
                ++i;
                continue;
            case '\r':
                out += "\\r";
                ++i;
                continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7f) {
            out += "\\x";
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        const std::size_t length = utf8_sequence_length(value.substr(i));
        if (length == 0) {
            out += "\\x";
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        out.append(value.substr(i, length));
        i += length;
    }
    out += '"';
}

void append_json_quoted(std::string& out, std::string_view value) {
    out += '"';
    std::size_t i = 0;
    while (i < value.size()) {
        const auto c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"':
                out += "\\\"";
                ++i;
                continue;
            case '\\':
                out += "\\\\";
                ++i;
                continue;
            case '\n':
                out += "\\n";
                ++i;
                continue;
            case '\t':
                out += "\\t";
                ++i;
                continue;
            case '\r':
                out += "\\r";
                ++i;
                continue;
            case '\b':
                out += "\\b";
                ++i;
                continue;
            case '\f':
                out += "\\f";
                ++i;
                continue;
            default: break;
        }
        if (c < 0x20 || c == 0x7f) {
            out += "\\u00";
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        const std::size_t length = utf8_sequence_length(value.substr(i));
        if (length == 0) {
            out += "\\\\x";
            append_hex_byte(out, c);
            ++i;
            continue;
        }
        out.append(value.substr(i, length));
        i += length;
    }
    out += '"';
}

}  // namespace tsfs
