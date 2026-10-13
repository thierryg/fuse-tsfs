// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_manager.hpp
// Purpose : Immutable configuration snapshots, validated hot reload, observers
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/error.hpp"
#include "config/config.hpp"
#include "logging/logger.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs {

/// Command-line values that take precedence over the file for the whole process lifetime.
struct ConfigOverrides {
    std::optional<std::string> mountpoint;
    std::optional<std::string> backing_directory;
    std::optional<LogLevel> log_level;
    std::optional<bool> console;
    std::optional<bool> diagnostics_enabled;
    std::optional<bool> text_stack_trace;
    std::optional<bool> native_core_dump;
};

/// One published configuration generation: never modified after publication.
struct ConfigSnapshot {
    std::shared_ptr<const Config> config;
    std::shared_ptr<const PolicyEngine> policy;
    std::uint64_t generation{0};
};

/**
 * @brief Owns the active configuration and performs reloads.
 *
 * Reload protocol (docs/architecture/configuration-reload.md):
 *  1. read and fully validate the file, apply the CLI overrides, compile the
 *     policy — no lock held, nothing changed yet;
 *  2. settings that need a restart (filesystem, diagnostics, security) keep their
 *     running values; a changed value is reported as a warning;
 *  3. observers are called in registration order with (next, previous); the first
 *     one is the logger (the only fallible step: opening a new log file). If an
 *     observer fails, the reload is aborted and nothing is published;
 *  4. the snapshot is published atomically; readers holding the old snapshot keep a
 *     consistent view until they drop it.
 * A failed reload keeps the last valid configuration and increments reload_failures().
 *
 * Thread-safety: current() is lock-free (atomic shared_ptr). reload() calls are
 * serialized by an internal mutex; observers must not call reload().
 */
class ConfigManager {
public:
    /// Called during reload; returning an error aborts the reload.
    using Observer = std::function<Result<void>(const ConfigSnapshot& next, const ConfigSnapshot& previous)>;

    ConfigManager(std::string path, ConfigOverrides overrides, Logger& log);

    /// Load the file for the first time (no observer is called). @return the error on failure.
    Result<void> load_initial();
    /// Re-read the file. @param trigger "sighup", "inotify", "test"... (logged).
    Result<void> reload(std::string_view trigger);
    /// Register an observer (before reloads start).
    void add_observer(Observer observer);

    /// @return the active snapshot (never null after load_initial()).
    [[nodiscard]] std::shared_ptr<const ConfigSnapshot> current() const { return current_.load(); }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t reload_failures() const noexcept { return failures_.load(); }
    [[nodiscard]] std::uint64_t reload_successes() const noexcept { return successes_.load(); }

    /// Apply @p overrides to @p config (also used by --check-config/--show-config).
    static void apply_overrides(Config& config, const ConfigOverrides& overrides);
    /// Parse + validate + compile without publishing (used by load_initial and reload).
    static Result<ConfigSnapshot> build(const std::string& path, const ConfigOverrides& overrides);

private:
    std::vector<std::string> keep_static_settings(Config& next, const Config& running) const;

    std::string path_;
    ConfigOverrides overrides_;
    Logger& log_;
    std::mutex reload_mutex_;
    std::vector<Observer> observers_;
    std::atomic<std::shared_ptr<const ConfigSnapshot>> current_;
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> successes_{0};
};

}  // namespace tsfs
