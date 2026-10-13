// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/managed_path.cpp
// Purpose : Validation and manipulation of managed paths
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "common/managed_path.hpp"

#include <cerrno>

namespace tsfs::managed_path {

Result<std::string> normalize(std::string_view path) {
    if (path.empty()) {
        return make_error(EINVAL, "empty path");
    }
    if (path.find('\0') != std::string_view::npos) {
        return make_error(EINVAL, "path contains a NUL byte");
    }
    if (path.front() != '/') {
        return make_error(EINVAL,
                          "path '" + std::string(path) + "' must start with '/' (it is relative to the TSFS root)");
    }
    std::string out;
    out.reserve(path.size());
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && path[pos] == '/') {
            ++pos;
        }
        if (pos == path.size()) {
            break;
        }
        const std::size_t end = path.find('/', pos);
        const std::string_view component = path.substr(pos, end == std::string_view::npos ? end : end - pos);
        if (component == "." || component == "..") {
            return make_error(EINVAL, "path '" + std::string(path) + "' must not contain '.' or '..' components");
        }
        out += '/';
        out += component;
        pos = end == std::string_view::npos ? path.size() : end;
    }
    if (out.empty()) {
        out = "/";
    }
    return out;
}

bool is_normalized(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/') {
        return false;
    }
    if (path.size() == 1) {
        return true;
    }
    if (path.back() == '/') {
        return false;
    }
    std::size_t pos = 1;
    while (pos <= path.size()) {
        const std::size_t end = std::min(path.find('/', pos), path.size());
        const std::string_view component = path.substr(pos, end - pos);
        if (component.empty() || component == "." || component == ".." ||
            component.find('\0') != std::string_view::npos) {
            return false;
        }
        pos = end + 1;
    }
    return true;
}

std::string_view basename(std::string_view path) noexcept {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string_view parent(std::string_view path) noexcept {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

std::size_t depth(std::string_view path) noexcept {
    if (path.size() <= 1) {
        return 0;
    }
    std::size_t count = 0;
    for (const char c : path) {
        count += c == '/' ? 1 : 0;
    }
    return count;
}

std::string join(std::string_view directory, std::string_view name) {
    std::string out;
    out.reserve(directory.size() + name.size() + 1);
    out += directory;
    if (out.empty() || out.back() != '/') {
        out += '/';
    }
    out += name;
    return out;
}

bool is_within(std::string_view path, std::string_view directory) noexcept {
    if (directory == "/") {
        return true;
    }
    if (path.size() < directory.size() || path.compare(0, directory.size(), directory) != 0) {
        return false;
    }
    return path.size() == directory.size() || path[directory.size()] == '/';
}

std::string_view relative(std::string_view path) noexcept {
    if (path.size() <= 1) {
        return ".";
    }
    return path.substr(1);
}

}  // namespace tsfs::managed_path
