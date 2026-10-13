// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/units.cpp
// Purpose : Validated parsing and formatting of durations and sizes
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "common/units.hpp"

#include <array>
#include <cerrno>
#include <limits>

namespace tsfs {
namespace {

struct Unit {
    std::string_view suffix;
    std::int64_t milliseconds;
};

// Ordered from the largest to the smallest unit: a duration must list its groups in
// this order ("1d2h", not "2h1d") so that every value has a single spelling.
constexpr std::array<Unit, 6> kDurationUnits{{
    {"w", 7LL * 24 * 3600 * 1000},
    {"d", 24LL * 3600 * 1000},
    {"h", 3600LL * 1000},
    {"m", 60LL * 1000},
    {"s", 1000},
    {"ms", 1},
}};

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Checked a * b + c on non-negative 64-bit integers.
bool checked_mul_add(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t limit, std::uint64_t& out) {
    if (b != 0 && a > (limit - c) / b) {
        return false;
    }
    out = a * b + c;
    return out <= limit;
}

}  // namespace

Result<Duration> parse_duration(std::string_view text) {
    if (text.empty()) {
        return make_error(EINVAL, "empty duration");
    }
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<Duration::rep>::max());
    std::uint64_t total = 0;
    std::size_t pos = 0;
    std::size_t last_unit = 0;  // index in kDurationUnits + 1 of the previous group
    while (pos < text.size()) {
        if (!is_digit(text[pos])) {
            return make_error(EINVAL, "invalid duration '" + std::string(text) + "': expected a number");
        }
        std::uint64_t number = 0;
        while (pos < text.size() && is_digit(text[pos])) {
            if (!checked_mul_add(number, 10, static_cast<std::uint64_t>(text[pos] - '0'), limit, number)) {
                return make_error(EINVAL, "duration '" + std::string(text) + "' overflows");
            }
            ++pos;
        }
        std::size_t end = pos;
        while (end < text.size() && !is_digit(text[end])) {
            ++end;
        }
        const std::string_view suffix = text.substr(pos, end - pos);
        if (suffix.empty()) {
            return make_error(EINVAL,
                              "invalid duration '" + std::string(text) + "': missing unit (ms, s, m, h, d or w)");
        }
        std::size_t index = 0;
        while (index < kDurationUnits.size() && kDurationUnits[index].suffix != suffix) {
            ++index;
        }
        if (index == kDurationUnits.size()) {
            return make_error(EINVAL, "invalid duration '" + std::string(text) + "': unknown unit '" +
                                          std::string(suffix) + "' (use ms, s, m, h, d or w)");
        }
        if (index + 1 <= last_unit) {
            return make_error(EINVAL, "invalid duration '" + std::string(text) +
                                          "': units must appear once, from the largest to the smallest");
        }
        last_unit = index + 1;
        std::uint64_t part = 0;
        if (!checked_mul_add(number, static_cast<std::uint64_t>(kDurationUnits[index].milliseconds), 0, limit, part) ||
            part > limit - total) {
            return make_error(EINVAL, "duration '" + std::string(text) + "' overflows");
        }
        total += part;
        pos = end;
    }
    return Duration{static_cast<Duration::rep>(total)};
}

std::string format_duration(Duration value) {
    auto remaining = value.count();
    if (remaining <= 0) {
        return remaining == 0 ? "0s" : "-" + format_duration(Duration{-remaining});
    }
    std::string out;
    for (const auto& unit : kDurationUnits) {
        if (remaining >= unit.milliseconds) {
            out += std::to_string(remaining / unit.milliseconds);
            out += unit.suffix;
            remaining %= unit.milliseconds;
        }
    }
    return out;
}

Result<std::uint64_t> parse_size(std::string_view text) {
    if (text.empty()) {
        return make_error(EINVAL, "empty size");
    }
    std::size_t pos = 0;
    std::uint64_t number = 0;
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    while (pos < text.size() && is_digit(text[pos])) {
        if (!checked_mul_add(number, 10, static_cast<std::uint64_t>(text[pos] - '0'), limit, number)) {
            return make_error(EINVAL, "size '" + std::string(text) + "' overflows");
        }
        ++pos;
    }
    if (pos == 0) {
        return make_error(EINVAL, "invalid size '" + std::string(text) + "': expected a number");
    }
    struct SizeUnit {
        std::string_view suffix;
        std::uint64_t factor;
    };
    static constexpr std::array<SizeUnit, 10> kUnits{{
        {"", 1},
        {"B", 1},
        {"KiB", 1ULL << 10},
        {"MiB", 1ULL << 20},
        {"GiB", 1ULL << 30},
        {"TiB", 1ULL << 40},
        {"KB", 1000ULL},
        {"MB", 1000ULL * 1000},
        {"GB", 1000ULL * 1000 * 1000},
        {"TB", 1000ULL * 1000 * 1000 * 1000},
    }};
    const std::string_view suffix = text.substr(pos);
    for (const auto& unit : kUnits) {
        if (unit.suffix == suffix) {
            std::uint64_t bytes = 0;
            if (!checked_mul_add(number, unit.factor, 0, limit, bytes)) {
                return make_error(EINVAL, "size '" + std::string(text) + "' overflows");
            }
            return bytes;
        }
    }
    return make_error(EINVAL, "invalid size '" + std::string(text) + "': unknown unit '" + std::string(suffix) +
                                  "' (use B, KiB, MiB, GiB, TiB, KB, MB, GB or TB)");
}

std::string format_size(std::uint64_t bytes) {
    static constexpr std::array<std::pair<std::string_view, std::uint64_t>, 4> kUnits{{
        {"TiB", 1ULL << 40},
        {"GiB", 1ULL << 30},
        {"MiB", 1ULL << 20},
        {"KiB", 1ULL << 10},
    }};
    for (const auto& [suffix, factor] : kUnits) {
        if (bytes != 0 && bytes % factor == 0) {
            return std::to_string(bytes / factor) + std::string(suffix);
        }
    }
    return std::to_string(bytes) + "B";
}

}  // namespace tsfs
