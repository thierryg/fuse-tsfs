// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/policy/policy_engine.cpp
// Purpose : Compiled retention rules: deterministic matching, precedence and expiry computation
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "policy/policy_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <numeric>
#include <ranges>
#include <tuple>

#include "common/managed_path.hpp"

namespace tsfs {
namespace {

char ascii_lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

/// Case-insensitive (ASCII) "basename ends with suffix", with at least one byte before it.
bool has_extension(std::string_view basename, std::string_view dotted) noexcept {
    if (basename.size() <= dotted.size()) {
        return false;
    }
    const std::string_view tail = basename.substr(basename.size() - dotted.size());
    return std::ranges::equal(tail, dotted, [](char a, char b) { return ascii_lower(a) == b; });
}

/// Precedence key of a rule: compared lexicographically, larger is more specific.
using Specificity = std::tuple<int, int, std::size_t, int, int, std::size_t>;

Specificity specificity(const RuleConfig& rule) {
    int scope_rank = 0;
    std::size_t depth = 0;
    int non_recursive = 0;
    if (rule.file) {
        scope_rank = 3;
        depth = managed_path::depth(*rule.file);
        non_recursive = 1;
    } else if (rule.path_glob) {
        scope_rank = 2;
        // Compile cannot fail here: the loader validated the pattern.
        if (auto g = Glob::compile(*rule.path_glob, Glob::Mode::path)) {
            depth = managed_path::depth(g->literal_prefix());
        }
    } else if (rule.path) {
        scope_rank = 1;
        depth = managed_path::depth(*rule.path);
        non_recursive = rule.recursive ? 0 : 1;
    }
    int pattern_rank = 0;
    std::size_t literals = 0;
    if (rule.glob) {
        pattern_rank = 1;
        if (auto g = Glob::compile(*rule.glob, Glob::Mode::basename)) {
            literals = g->literal_count();
        }
    }
    if (!rule.extensions.empty()) {
        pattern_rank = 1;
        // A list is as specific as its least specific member (+1 for the dot).
        const auto shortest = std::ranges::min(rule.extensions, {}, &std::string::size);
        literals = std::max(literals, shortest.size() + 1);
    }
    if (rule.path_glob) {
        if (auto g = Glob::compile(*rule.path_glob, Glob::Mode::path)) {
            literals = std::max(literals, g->literal_count());
        }
    }
    return {rule.priority, scope_rank, depth, non_recursive, pattern_rank, literals};
}

}  // namespace

TimePoint saturating_add(TimePoint base, Duration retention) noexcept {
    // C++26 saturation arithmetic: a huge retention clamps to the end of time instead
    // of wrapping around to a date in the past (which would delete the file).
    return TimePoint{Duration{std::add_sat(base.time_since_epoch().count(), retention.count())}};
}

bool PolicyEngine::CompiledRule::matches(std::string_view candidate, std::string_view basename) const noexcept {
    if (file) {
        return candidate == *file;
    }
    if (path_glob) {
        // Cheap prefix filter before running the component matcher.
        if (!managed_path::is_within(candidate, path_glob->literal_prefix()) || !path_glob->matches(candidate)) {
            return false;
        }
    }
    if (path) {
        const std::string_view parent = managed_path::parent(candidate);
        if (recursive ? !managed_path::is_within(parent, *path) : parent != *path) {
            return false;
        }
    }
    if (!extensions.empty() &&
        std::ranges::none_of(extensions, [&](const std::string& ext) { return has_extension(basename, ext); })) {
        return false;
    }
    if (glob && !glob->matches(basename)) {
        return false;
    }
    return true;
}

Result<std::shared_ptr<const PolicyEngine>> PolicyEngine::compile(const RetentionConfig& config) {
    auto engine = std::shared_ptr<PolicyEngine>(new PolicyEngine());
    engine->default_retention_ = config.default_retention;
    engine->default_timestamp_ = config.timestamp;
    engine->needs_birth_time_ = config.default_retention && config.timestamp == TimestampSource::birth;
    engine->never_expires_ = !config.default_retention.has_value();

    struct Ordered {
        Specificity key;
        int index;
    };
    std::vector<Ordered> order;
    for (int index = 0; const RuleConfig& rule : config.rules) {
        engine->ids_.push_back(rule.id);
        if (rule.enabled) {
            order.push_back({specificity(rule), index});
        }
        ++index;
    }
    // Most specific first; declaration order breaks ties (stable and explicit).
    std::ranges::sort(
        order, [](const Ordered& a, const Ordered& b) { return a.key != b.key ? a.key > b.key : a.index < b.index; });

    for (const auto& [_, index] : order) {
        const RuleConfig& rule = config.rules[static_cast<std::size_t>(index)];
        CompiledRule compiled;
        compiled.index = index;
        compiled.id = rule.id;
        compiled.action = rule.action;
        compiled.file = rule.file;
        compiled.path = rule.path;
        compiled.recursive = rule.recursive;
        if (rule.glob) {
            auto glob = Glob::compile(*rule.glob, Glob::Mode::basename);
            if (!glob) {
                return make_error(EINVAL, "rule '" + rule.id + "': " + glob.error().message);
            }
            compiled.glob = std::move(*glob);
        }
        if (rule.path_glob) {
            auto glob = Glob::compile(*rule.path_glob, Glob::Mode::path);
            if (!glob) {
                return make_error(EINVAL, "rule '" + rule.id + "': " + glob.error().message);
            }
            compiled.path_glob = std::move(*glob);
        }
        for (const std::string& ext : rule.extensions) {
            compiled.extensions.push_back("." + ext);
        }
        compiled.retention = rule.retention;
        compiled.timestamp = rule.timestamp.value_or(config.timestamp);
        if (rule.action == RuleAction::exclude) {
            engine->excludes_.push_back(std::move(compiled));
        } else {
            if (compiled.retention) {
                engine->never_expires_ = false;
                engine->needs_birth_time_ |= compiled.timestamp == TimestampSource::birth;
            }
            engine->expires_.push_back(std::move(compiled));
        }
    }
    return std::shared_ptr<const PolicyEngine>(std::move(engine));
}

// views::concat over two vectors cannot throw (its variant-based iterator is never
// valueless here); the analyzer cannot prove it.
int PolicyEngine::match(std::string_view path) const noexcept {  // NOLINT(bugprone-exception-escape)
    const std::string_view basename = managed_path::basename(path);
    // Exclusions first, then expire rules: both lists are already in precedence order.
    for (const CompiledRule& rule : std::views::concat(excludes_, expires_)) {
        if (rule.matches(path, basename)) {
            return rule.index;
        }
    }
    return -1;
}

Decision PolicyEngine::decide(const CompiledRule* rule, const std::optional<Duration>& retention,
                              TimestampSource timestamp, const FileTimes& times) noexcept {
    Decision decision;
    decision.rule_index = rule != nullptr ? rule->index : -1;
    if (!retention) {
        decision.kind = Decision::Kind::keep_forever;
        return decision;
    }
    TimePoint base = times.modification;
    if (timestamp == TimestampSource::birth) {
        if (times.birth) {
            base = *times.birth;
        } else {
            decision.birth_fallback = true;
        }
    }
    decision.kind = Decision::Kind::expires;
    decision.expiry = saturating_add(base, *retention);
    return decision;
}

Decision PolicyEngine::evaluate(std::string_view path, const FileTimes& times) const noexcept {
    const std::string_view basename = managed_path::basename(path);
    for (const CompiledRule& rule : excludes_) {
        if (rule.matches(path, basename)) {
            Decision decision;
            decision.kind = Decision::Kind::excluded;
            decision.rule_index = rule.index;
            return decision;
        }
    }
    for (const CompiledRule& rule : expires_) {
        if (rule.matches(path, basename)) {
            return decide(&rule, rule.retention, rule.timestamp, times);
        }
    }
    return decide(nullptr, default_retention_, default_timestamp_, times);
}

std::string_view PolicyEngine::rule_id(int index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= ids_.size()) {
        return "default";
    }
    return ids_[static_cast<std::size_t>(index)];
}

}  // namespace tsfs
