// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/managed_path.hpp
// Purpose : Validation and manipulation of managed paths ("/dir/file", relative to the TSFS root)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <string>
#include <string_view>

#include "common/error.hpp"

namespace tsfs {

/**
 * @brief Helpers for *managed paths*.
 *
 * A managed path names an entry inside the TSFS root (the backing directory, seen
 * as "/" through the mount). It always starts with '/', has no empty, "." or ".."
 * component, no trailing slash (except the root itself) and no NUL byte. FUSE
 * hands such paths to the callbacks; configuration paths are normalized into the
 * same form. A managed path is never a host path: "/tmp/record" in a rule means
 * "<backing directory>/tmp/record".
 */
namespace managed_path {

/**
 * @brief Validate and normalize a path written in the configuration.
 *
 * Repeated slashes are collapsed and a trailing slash is removed. "." and ".."
 * components are rejected (not resolved) so that a rule never silently changes
 * scope.
 *
 * @return The normalized path, or EINVAL with the reason.
 */
Result<std::string> normalize(std::string_view path);

/// @return true when @p path is already a normalized managed path.
bool is_normalized(std::string_view path) noexcept;

/// @return the last component ("" for the root). Precondition: normalized path.
std::string_view basename(std::string_view path) noexcept;

/// @return the parent path ("/" for top-level entries and for the root). Precondition: normalized.
std::string_view parent(std::string_view path) noexcept;

/// @return the number of components ("/" -> 0, "/a/b" -> 2). Precondition: normalized.
std::size_t depth(std::string_view path) noexcept;

/// @return "/a/b" for ("/a", "b") and "/b" for ("/", "b").
std::string join(std::string_view directory, std::string_view name);

/**
 * @return true when @p path is @p directory itself or below it (component-wise,
 *         "/ab" is not below "/a"). Both arguments normalized.
 */
bool is_within(std::string_view path, std::string_view directory) noexcept;

/// @return the path without its leading '/' ("." for the root), usable with *at() syscalls.
std::string_view relative(std::string_view path) noexcept;

}  // namespace managed_path
}  // namespace tsfs
