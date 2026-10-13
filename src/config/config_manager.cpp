// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_manager.cpp
// Purpose : Immutable configuration snapshots, validated hot reload, observers
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "config/config_manager.hpp"

#include <format>

#include "config/config_loader.hpp"

namespace tsfs {

ConfigManager::ConfigManager(std::string path, ConfigOverrides overrides, Logger& log)
    : path_(std::move(path)), overrides_(std::move(overrides)), log_(log) {}

void ConfigManager::apply_overrides(Config& config, const ConfigOverrides& o) {
    if (o.mountpoint) {
        config.filesystem.mountpoint = *o.mountpoint;
    }
    if (o.backing_directory) {
        config.filesystem.backing_directory = *o.backing_directory;
    }
    if (o.log_level) {
        config.logging.level = *o.log_level;
    }
    if (o.console) {
        config.logging.console = *o.console;
    }
    if (o.diagnostics_enabled) {
        config.diagnostics.enabled = *o.diagnostics_enabled;
    }
    if (o.text_stack_trace) {
        config.diagnostics.text_stack_trace = *o.text_stack_trace;
    }
    if (o.native_core_dump) {
        config.diagnostics.native_core_dump = *o.native_core_dump;
    }
}

Result<ConfigSnapshot> ConfigManager::build(const std::string& path, const ConfigOverrides& overrides) {
    auto config = load_config_file(path);
    if (!config) {
        return propagate(config.error());
    }
    apply_overrides(*config, overrides);
    if (overrides.mountpoint || overrides.backing_directory) {
        if (auto valid = validate_path_overrides(*config); !valid) {
            return propagate(valid.error());
        }
    }
    auto policy = PolicyEngine::compile(config->retention);
    if (!policy) {
        return propagate(policy.error());
    }
    ConfigSnapshot snapshot;
    snapshot.config = std::make_shared<const Config>(std::move(*config));
    snapshot.policy = std::move(*policy);
    return snapshot;
}

Result<void> ConfigManager::load_initial() {
    std::scoped_lock lock(reload_mutex_);
    auto snapshot = build(path_, overrides_);
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    snapshot->generation = 1;
    current_.store(std::make_shared<const ConfigSnapshot>(std::move(*snapshot)));
    return {};
}

void ConfigManager::add_observer(Observer observer) {
    std::scoped_lock lock(reload_mutex_);
    observers_.push_back(std::move(observer));
}

std::vector<std::string> ConfigManager::keep_static_settings(Config& next, const Config& running) const {
    std::vector<std::string> ignored;
    const auto keep = [&](auto& field, const auto& value, const char* name) {
        if (field != value) {
            ignored.emplace_back(name);
            field = value;
        }
    };
    keep(next.filesystem.mountpoint, running.filesystem.mountpoint, "filesystem.mountpoint");
    keep(next.filesystem.backing_directory, running.filesystem.backing_directory, "filesystem.backing_directory");
    keep(next.filesystem.allow_other, running.filesystem.allow_other, "filesystem.allow_other");
    keep(next.filesystem.read_only, running.filesystem.read_only, "filesystem.read_only");
    keep(next.diagnostics.enabled, running.diagnostics.enabled, "diagnostics.enabled");
    keep(next.diagnostics.text_stack_trace, running.diagnostics.text_stack_trace, "diagnostics.text_stack_trace");
    keep(next.diagnostics.native_core_dump, running.diagnostics.native_core_dump, "diagnostics.native_core_dump");
    keep(next.diagnostics.crash_directory, running.diagnostics.crash_directory, "diagnostics.crash_directory");
    keep(next.diagnostics.max_crash_reports, running.diagnostics.max_crash_reports, "diagnostics.max_crash_reports");
    keep(next.security.stay_on_filesystem, running.security.stay_on_filesystem, "security.stay_on_filesystem");
    keep(next.security.require_openat2, running.security.require_openat2, "security.require_openat2");
    return ignored;
}

Result<void> ConfigManager::reload(std::string_view trigger) {
    std::scoped_lock lock(reload_mutex_);
    const auto previous = current_.load();
    log_.log(LogLevel::info, "config", "config_reload_started", "reloading the configuration",
             {{"file", path_}, {"trigger", trigger}});
    const auto fail = [&](const Error& error) -> Result<void> {
        failures_.fetch_add(1);
        log_.log(LogLevel::error, "config", "config_reload_failed",
                 "reload rejected; the previous configuration stays active",
                 {{"file", path_}, {"error", error.message}, {"generation", previous->generation}});
        return propagate(error);
    };
    auto built = build(path_, overrides_);
    if (!built) {
        return fail(built.error());
    }
    // Copy-on-write: the published Config is const, so build a modified copy.
    auto next_config = std::make_shared<Config>(*built->config);
    for (const std::string& name : keep_static_settings(*next_config, *previous->config)) {
        log_.log(LogLevel::warning, "config", "config_restart_required",
                 "this setting requires a restart; the running value is kept", {{"setting", name}});
    }
    ConfigSnapshot next;
    next.config = std::move(next_config);
    next.policy = built->policy;
    next.generation = previous->generation + 1;
    for (const Observer& observer : observers_) {
        if (auto r = observer(next, *previous); !r) {
            return fail(r.error());
        }
    }
    current_.store(std::make_shared<const ConfigSnapshot>(next));
    successes_.fetch_add(1);
    log_.log(LogLevel::info, "config", "config_reload_succeeded", "configuration reloaded",
             {{"file", path_}, {"generation", next.generation}, {"rules", next.policy->active_rule_count()}});
    return {};
}

}  // namespace tsfs
