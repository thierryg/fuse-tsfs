// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/error.hpp
// Purpose : Error value and Result<T> alias over std::expected
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <expected>
#include <string>
#include <utility>

namespace tsfs {

/**
 * @brief An error value: a positive errno-style code and a human-readable message.
 *
 * The code is meaningful for system errors (ENOENT, EACCES...) and is translated
 * into a negative errno by the FUSE layer. Validation errors use EINVAL.
 */
struct Error {
    int code{0};          ///< Positive errno value (EINVAL for validation errors).
    std::string message;  ///< Human-readable description, US English.
};

/**
 * @brief Value-or-error return type used by every fallible TSFS function.
 *
 * Errors are values, never exceptions, so that nothing can unwind through the
 * libfuse C callbacks.
 */
template <typename T>
using Result = std::expected<T, Error>;

/// @return an unexpected Error, directly returnable from a function returning Result<T>.
[[nodiscard]] inline std::unexpected<Error> make_error(int code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

/// @return @p error wrapped for propagation (`return propagate(r.error());`).
[[nodiscard]] inline std::unexpected<Error> propagate(Error error) { return std::unexpected(std::move(error)); }

}  // namespace tsfs
