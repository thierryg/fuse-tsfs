// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/policy/glob.cpp
// Purpose : Precompiled glob patterns for basenames and whole managed paths
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "policy/glob.hpp"

#include <algorithm>
#include <cerrno>

#include "common/escape.hpp"

namespace tsfs {
namespace {

void set_bit(std::array<std::uint64_t, 4>& bits, unsigned char c) { bits[c >> 6U] |= std::uint64_t{1} << (c & 63U); }
bool test_bit(const std::array<std::uint64_t, 4>& bits, unsigned char c) { return (bits[c >> 6U] >> (c & 63U)) & 1U; }

std::unexpected<Error> glob_error(std::string_view pattern, const std::string& reason) {
    return make_error(EINVAL, "invalid glob '" + std::string(pattern) + "': " + reason);
}

}  // namespace

Result<Glob::Segment> Glob::compile_segment(std::string_view text, std::string_view pattern) {
    Segment segment;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        Token token;
        if (c == '\\') {
            if (i + 1 >= text.size()) {
                return glob_error(pattern, "trailing backslash");
            }
            token.literal = text[i + 1];
            i += 2;
        } else if (c == '*') {
            token.kind = Token::Kind::star;
            ++i;
            // Consecutive stars inside a component are equivalent to one.
            if (!segment.tokens.empty() && segment.tokens.back().kind == Token::Kind::star) {
                continue;
            }
        } else if (c == '?') {
            token.kind = Token::Kind::any;
            ++i;
        } else if (c == '[') {
            token.kind = Token::Kind::set;
            std::size_t j = i + 1;
            if (j < text.size() && (text[j] == '!' || text[j] == '^')) {
                token.negated = true;
                ++j;
            }
            bool first = true;
            bool closed = false;
            while (j < text.size()) {
                unsigned char lo = static_cast<unsigned char>(text[j]);
                if (lo == ']' && !first) {
                    closed = true;
                    ++j;
                    break;
                }
                first = false;
                if (lo == '\\') {
                    if (j + 1 >= text.size()) {
                        return glob_error(pattern, "trailing backslash in a character class");
                    }
                    lo = static_cast<unsigned char>(text[++j]);
                }
                ++j;
                if (j + 1 < text.size() && text[j] == '-' && text[j + 1] != ']') {
                    unsigned char hi = static_cast<unsigned char>(text[j + 1]);
                    j += 2;
                    if (hi == '\\') {
                        if (j >= text.size()) {
                            return glob_error(pattern, "trailing backslash in a character class");
                        }
                        hi = static_cast<unsigned char>(text[j++]);
                    }
                    if (hi < lo) {
                        return glob_error(pattern, "reversed range in a character class");
                    }
                    for (unsigned v = lo; v <= hi; ++v) {
                        set_bit(token.bits, static_cast<unsigned char>(v));
                    }
                } else {
                    set_bit(token.bits, lo);
                }
            }
            if (!closed) {
                return glob_error(pattern, "unterminated character class '['");
            }
            i = j;
        } else {
            token.literal = c;
            ++i;
        }
        segment.tokens.push_back(token);
    }
    return segment;
}

Result<Glob> Glob::compile(std::string_view pattern, Mode mode) {
    if (pattern.empty()) {
        return glob_error(pattern, "empty pattern");
    }
    if (pattern.find('\0') != std::string_view::npos) {
        return glob_error(pattern, "NUL byte");
    }
    Glob glob;
    glob.pattern_ = std::string(pattern);
    glob.mode_ = mode;
    if (mode == Mode::basename) {
        if (pattern.find('/') != std::string_view::npos) {
            return glob_error(pattern, "a basename glob cannot contain '/' (use path_glob)");
        }
        if (pattern.find("**") != std::string_view::npos) {
            return glob_error(pattern, "'**' is only valid in path_glob");
        }
        auto segment = compile_segment(pattern, pattern);
        if (!segment) {
            return propagate(segment.error());
        }
        glob.segments_.push_back(std::move(segment.value()));
    } else {
        if (pattern.front() != '/') {
            return glob_error(pattern, "a path glob must start with '/' (it is relative to the TSFS root)");
        }
        if (pattern.size() > 1 && pattern.back() == '/') {
            return glob_error(pattern, "a path glob must not end with '/'");
        }
        bool prefix_open = true;
        std::string prefix;
        std::size_t pos = 1;
        while (pos <= pattern.size()) {
            std::size_t end = pos;
            // Find the next unescaped '/'.
            while (end < pattern.size() && pattern[end] != '/') {
                end += pattern[end] == '\\' ? 2 : 1;
            }
            end = std::min(end, pattern.size());
            const std::string_view text = pattern.substr(pos, end - pos);
            if (text.empty()) {
                return glob_error(pattern, "empty path component");
            }
            if (text == "." || text == "..") {
                return glob_error(pattern, "'.' and '..' components are not allowed");
            }
            Segment segment;
            if (text == "**") {
                segment.globstar = true;
            } else {
                if (text.find("**") != std::string_view::npos) {
                    return glob_error(pattern, "'**' must be a whole path component");
                }
                auto compiled = compile_segment(text, pattern);
                if (!compiled) {
                    return propagate(compiled.error());
                }
                segment = std::move(compiled.value());
            }
            const bool literal_only =
                !segment.globstar && std::all_of(segment.tokens.begin(), segment.tokens.end(),
                                                 [](const Token& t) { return t.kind == Token::Kind::literal; });
            // The last component names files, so it never belongs to the directory prefix.
            if (prefix_open && literal_only && end < pattern.size()) {
                prefix += '/';
                for (const Token& t : segment.tokens) {
                    prefix += t.literal;
                }
            } else {
                prefix_open = false;
            }
            glob.segments_.push_back(std::move(segment));
            pos = end + 1;
        }
        glob.literal_prefix_ = prefix.empty() ? "/" : prefix;
    }
    for (const Segment& segment : glob.segments_) {
        for (const Token& token : segment.tokens) {
            glob.literal_count_ += token.kind == Token::Kind::literal ? 1 : 0;
        }
    }
    return glob;
}

bool Glob::match_segment(const Segment& segment, std::string_view text) noexcept {
    // Classic linear wildcard matching with a single backtracking point: when a later
    // match fails, the most recent '*' absorbs one more byte. Correct because '*' is
    // the only variable-length token inside a component.
    const auto& tokens = segment.tokens;
    std::size_t ti = 0;
    std::size_t si = 0;
    std::size_t star_ti = tokens.size();
    std::size_t star_si = 0;
    while (si < text.size()) {
        if (ti < tokens.size()) {
            const Token& token = tokens[ti];
            if (token.kind == Token::Kind::star) {
                star_ti = ti++;
                star_si = si;
                continue;
            }
            std::size_t consumed = 0;
            const auto c = static_cast<unsigned char>(text[si]);
            switch (token.kind) {
                case Token::Kind::literal: consumed = token.literal == text[si] ? 1 : 0; break;
                case Token::Kind::any: {
                    const std::size_t length = utf8_sequence_length(text.substr(si));
                    consumed = length == 0 ? 1 : length;
                    break;
                }
                case Token::Kind::set: consumed = test_bit(token.bits, c) != token.negated ? 1 : 0; break;
                case Token::Kind::star: break;
            }
            if (consumed != 0) {
                ++ti;
                si += consumed;
                continue;
            }
        }
        if (star_ti != tokens.size()) {
            ti = star_ti + 1;
            si = ++star_si;
            continue;
        }
        return false;
    }
    while (ti < tokens.size() && tokens[ti].kind == Token::Kind::star) {
        ++ti;
    }
    return ti == tokens.size();
}

bool Glob::matches(std::string_view text) const noexcept {
    if (mode_ == Mode::basename) {
        return match_segment(segments_.front(), text);
    }
    if (text.empty() || text.front() != '/') {
        return false;
    }
    // Split the path into components without allocating for typical depths.
    constexpr std::size_t kInline = 64;
    std::array<std::string_view, kInline> inline_components{};
    std::vector<std::string_view> heap_components;
    std::size_t count = 0;
    std::size_t pos = 1;
    while (pos < text.size()) {
        const std::size_t end = std::min(text.find('/', pos), text.size());
        const std::string_view component = text.substr(pos, end - pos);
        if (count < kInline) {
            inline_components[count] = component;
        } else {
            if (heap_components.empty()) {
                heap_components.assign(inline_components.begin(), inline_components.end());
            }
            heap_components.push_back(component);
        }
        ++count;
        pos = end + 1;
    }
    const std::string_view* components = count <= kInline ? inline_components.data() : heap_components.data();

    // Same single-backtrack algorithm, at the component level, with "**" as the star.
    std::size_t pi = 0;
    std::size_t ci = 0;
    std::size_t star_pi = segments_.size();
    std::size_t star_ci = 0;
    while (ci < count) {
        if (pi < segments_.size()) {
            if (segments_[pi].globstar) {
                star_pi = pi++;
                star_ci = ci;
                continue;
            }
            if (match_segment(segments_[pi], components[ci])) {
                ++pi;
                ++ci;
                continue;
            }
        }
        if (star_pi != segments_.size()) {
            pi = star_pi + 1;
            ci = ++star_ci;
            continue;
        }
        return false;
    }
    while (pi < segments_.size() && segments_[pi].globstar) {
        ++pi;
    }
    return pi == segments_.size();
}

}  // namespace tsfs
