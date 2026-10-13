// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/fuse_control.cpp
// Purpose : mountinfo parsing and fusectl reads for the status file
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "filesystem/fuse_control.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace tsfs {

namespace {

/// mountinfo escapes space, tab, newline and backslash as \ooo (octal).
std::string unescape_mountinfo(std::string_view field) {
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size()) {
            int value = 0;
            const auto [ptr, ec] = std::from_chars(field.data() + i + 1, field.data() + i + 4, value, 8);
            if (ec == std::errc{} && ptr == field.data() + i + 4) {
                out.push_back(static_cast<char>(value));
                i += 3;
                continue;
            }
        }
        out.push_back(field[i]);
    }
    return out;
}

/// Whole-field unsigned decimal parse.
bool parse_u32(std::string_view text, std::uint32_t& value) {
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    const auto [ptr, ec] = std::from_chars(first, last, value);
    return ec == std::errc{} && ptr == last && !text.empty();
}

std::vector<std::string_view> split(std::string_view line, char separator) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = std::min(line.find(separator, start), line.size());
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

}  // namespace

std::optional<std::uint32_t> fuse_connection_id(std::string_view mountinfo, std::string_view mountpoint) {
    // "36 35 0:52 / /mnt/x rw,nosuid shared:1 - fuse.tsfs tsfs rw,user_id=0,..."
    //  id parent major:minor root mount-point options [optional...] - fstype source super-options
    std::optional<std::uint32_t> found;
    std::size_t start = 0;
    while (start < mountinfo.size()) {
        const std::size_t end = std::min(mountinfo.find('\n', start), mountinfo.size());
        const std::string_view line = mountinfo.substr(start, end - start);
        start = end + 1;
        const auto fields = split(line, ' ');
        if (fields.size() < 10 || unescape_mountinfo(fields[4]) != mountpoint) {
            continue;
        }
        std::size_t dash = 6;
        while (dash < fields.size() && fields[dash] != "-") {
            ++dash;
        }
        if (dash + 1 >= fields.size() || !(fields[dash + 1] == "fuse" || fields[dash + 1].starts_with("fuse."))) {
            continue;
        }
        const std::string_view device = fields[2];
        const std::size_t colon = device.find(':');
        std::uint32_t major = 0;
        std::uint32_t minor = 0;
        if (colon == std::string_view::npos || !parse_u32(device.substr(0, colon), major) ||
            !parse_u32(device.substr(colon + 1), minor)) {
            continue;
        }
        found = (major << 20U) | minor;  // Kernel dev_t encoding (MKDEV), as used by fusectl.
    }
    return found;
}

std::optional<std::uint32_t> fuse_connection_id(std::string_view mountpoint) {
    std::ifstream in("/proc/self/mountinfo");
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream content;
    content << in.rdbuf();
    return fuse_connection_id(content.str(), mountpoint);
}

std::optional<std::uint64_t> fuse_connection_waiting(std::uint32_t id, std::string_view connections_dir) {
    std::ifstream in(std::format("{}/{}/waiting", connections_dir, id));
    std::uint64_t waiting = 0;
    if (!(in >> waiting)) {
        return std::nullopt;
    }
    return waiting;
}

}  // namespace tsfs
