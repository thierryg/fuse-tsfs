// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/policy/glob.hpp
// Purpose : Precompiled glob patterns for basenames and whole managed paths
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.hpp"

namespace tsfs {

/**
 * @brief A glob pattern compiled once and matched many times without allocation.
 *
 * Syntax (case-sensitive, never a regular expression):
 * - `*`      any sequence of characters inside one path component (also matches a
 *            leading '.', i.e. hidden files are not special);
 * - `?`      exactly one character (one UTF-8 sequence);
 * - `[abc]`, `[a-z]`, `[!a-z]`, `[^a-z]`  one byte from / not from the set
 *            (byte-oriented: use it for ASCII);
 * - `\x`     the character x literally;
 * - `**`     (path globs only, as a whole component) zero or more components.
 *
 * Basename globs (Mode::basename) match the last component only and may not
 * contain '/'. Path globs (Mode::path) start with '/' and are matched against the
 * whole managed path, component by component: `*` never crosses a '/'.
 *
 * Immutable after compile(); matches() is thread-safe.
 */
class Glob {
public:
    /// What the pattern is matched against.
    enum class Mode { basename, path };

    /**
     * @brief Compile @p pattern.
     * @return The compiled glob, or EINVAL (empty pattern, unterminated class,
     *         trailing backslash, '/' in a basename glob, misplaced "**", relative
     *         path glob, empty component).
     */
    static Result<Glob> compile(std::string_view pattern, Mode mode);

    /// @return true when @p text (a basename or a normalized managed path) matches.
    [[nodiscard]] bool matches(std::string_view text) const noexcept;

    /// @return the number of literal (non-wildcard) characters, used for specificity.
    [[nodiscard]] std::size_t literal_count() const noexcept { return literal_count_; }

    /// @return for a path glob, the longest wildcard-free directory prefix ("/" if none).
    [[nodiscard]] const std::string& literal_prefix() const noexcept { return literal_prefix_; }

    /// @return the source pattern.
    [[nodiscard]] const std::string& pattern() const noexcept { return pattern_; }

private:
    struct Token {
        enum class Kind : std::uint8_t { literal, any, star, set } kind{Kind::literal};
        char literal{0};
        bool negated{false};
        std::array<std::uint64_t, 4> bits{};  ///< 256-bit byte set for Kind::set.
    };
    struct Segment {
        std::vector<Token> tokens;
        bool globstar{false};
    };

    static bool match_segment(const Segment& segment, std::string_view text) noexcept;
    static Result<Segment> compile_segment(std::string_view text, std::string_view pattern);

    std::string pattern_;
    Mode mode_{Mode::basename};
    std::vector<Segment> segments_;
    std::size_t literal_count_{0};
    std::string literal_prefix_{"/"};
};

}  // namespace tsfs
