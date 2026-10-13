// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/policy/policy_engine.hpp
// Purpose : Compiled retention rules: deterministic matching, precedence and expiry computation
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/clock.hpp"
#include "common/error.hpp"
#include "config/config.hpp"
#include "policy/glob.hpp"

namespace tsfs {

/// Timestamps of a file needed to evaluate its lifetime.
struct FileTimes {
    TimePoint modification;          ///< st_mtime.
    std::optional<TimePoint> birth;  ///< statx btime, when the filesystem reports it.
};

/// Outcome of a policy evaluation for one file.
struct Decision {
    enum class Kind {
        keep_forever,  ///< Matched rule (or default) has retention "never".
        excluded,      ///< An exclude rule matched: never delete.
        expires,       ///< The file expires at @ref expiry.
    };
    Kind kind{Kind::keep_forever};
    TimePoint expiry{};          ///< Valid when kind == expires (saturated, never overflows).
    int rule_index{-1};          ///< Index in the configuration rule list, -1 for the default.
    bool birth_fallback{false};  ///< birth requested but unavailable: st_mtime was used.

    [[nodiscard]] bool expired_at(TimePoint now) const noexcept { return kind == Kind::expires && expiry <= now; }
};

/**
 * @brief Immutable, precompiled set of retention rules.
 *
 * Built once per configuration generation by compile() and shared read-only by the
 * GC threads (std::shared_ptr<const PolicyEngine>): every method is const and
 * thread-safe, and matching allocates nothing.
 *
 * Precedence (documented in docs/architecture/policy-engine.md):
 *  1. any enabled exclude rule that matches wins (fail-safe: exclusions always override);
 *  2. otherwise the matching expire rules are ordered by
 *     a. higher priority,
 *     b. more specific scope (file > path_glob > path; deeper first; non-recursive
 *        before recursive),
 *     c. more specific pattern (with a glob/extension before without; more literal
 *        characters first),
 *     d. declaration order;
 *     the first one decides;
 *  3. no match: retention.default applies.
 * The order is computed once at compile time, so evaluation is a linear scan that
 * stops at the first match.
 */
class PolicyEngine {
public:
    /// Compile @p config. @return the engine, or EINVAL if a pattern is invalid.
    static Result<std::shared_ptr<const PolicyEngine>> compile(const RetentionConfig& config);

    /**
     * @brief Evaluate the policy for the regular file at @p path.
     * @param path  Normalized managed path ("/dir/file").
     * @param times File timestamps.
     */
    [[nodiscard]] Decision evaluate(std::string_view path, const FileTimes& times) const noexcept;

    /// @return the index (configuration order) of the deciding rule, or -1 for the default.
    [[nodiscard]] int match(std::string_view path) const noexcept;

    /// @return the identifier of rule @p index, or "default" for -1.
    [[nodiscard]] std::string_view rule_id(int index) const noexcept;

    /// @return true when at least one active rule (or the default) uses birth time.
    [[nodiscard]] bool needs_birth_time() const noexcept { return needs_birth_time_; }

    /// @return true when nothing can ever expire (no expire rule with a duration, default "never").
    [[nodiscard]] bool never_expires() const noexcept { return never_expires_; }

    /// @return number of enabled rules.
    [[nodiscard]] std::size_t active_rule_count() const noexcept { return excludes_.size() + expires_.size(); }

private:
    struct CompiledRule {
        int index{-1};
        std::string id;
        RuleAction action{RuleAction::expire};
        std::optional<std::string> file;
        std::optional<std::string> path;
        bool recursive{false};
        std::optional<Glob> glob;
        std::optional<Glob> path_glob;
        std::vector<std::string> extensions;  ///< ".mp4" form, lower case.
        std::optional<Duration> retention;
        TimestampSource timestamp{TimestampSource::modification};

        [[nodiscard]] bool matches(std::string_view path, std::string_view basename) const noexcept;
    };

    PolicyEngine() = default;
    static Decision decide(const CompiledRule* rule, const std::optional<Duration>& retention,
                           TimestampSource timestamp, const FileTimes& times) noexcept;

    std::vector<CompiledRule> excludes_;  ///< Enabled exclude rules, precedence order.
    std::vector<CompiledRule> expires_;   ///< Enabled expire rules, precedence order.
    std::vector<std::string> ids_;        ///< Every rule id, configuration order.
    std::optional<Duration> default_retention_;
    TimestampSource default_timestamp_{TimestampSource::modification};
    bool needs_birth_time_{false};
    bool never_expires_{true};
};

/// Saturating base + retention (no signed overflow, clamps to TimePoint::max()).
[[nodiscard]] TimePoint saturating_add(TimePoint base, Duration retention) noexcept;

}  // namespace tsfs
