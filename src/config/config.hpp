// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config.hpp
// Purpose : Typed, validated configuration model (schema version 1)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/units.hpp"

namespace tsfs {

/// Default configuration file (overridable with --config).
inline constexpr const char* kDefaultConfigPath = TSFS_DEFAULT_CONFIG_PATH;

/// Only supported value of the top-level "version" key.
inline constexpr int kConfigVersion = 1;

/// Exposure model of the filesystem. Only passthrough is implemented.
enum class FilesystemMode { passthrough };

/// File timestamp used as the start of a file's lifetime.
enum class TimestampSource {
    modification,  ///< st_mtime: last content modification (default).
    birth,         ///< statx() btime when the backing filesystem reports it, else st_mtime.
};

/// What a rule does with the files it matches.
enum class RuleAction {
    expire,   ///< Delete matching files once their retention has elapsed ("delete" in YAML).
    exclude,  ///< Never delete matching files, whatever the other rules say.
};

/// Logging severity, from the most to the least verbose.
enum class LogLevel { trace, debug, info, warning, error, critical };

/// Output format of the log file.
enum class LogFormat { text, json };

/// Behavior of the asynchronous logging queue when it is full.
enum class LogOverflowPolicy {
    block,           ///< The producer waits for room (no message is lost).
    overrun_oldest,  ///< The oldest queued message is dropped (counted, reported).
    discard_new,     ///< The new message is dropped (counted, reported).
};

/// Data structure of the GC expiration scheduler.
enum class SchedulerKind {
    heap,     ///< Binary min-heap with lazy invalidation (default).
    ordered,  ///< Balanced tree (std::set) with an index: exact cancellation.
};

/// One retention rule as written in the configuration (validated).
struct RuleConfig {
    std::string id;                            ///< Unique identifier ([A-Za-z0-9._-], 1..64).
    bool enabled{true};                        ///< Disabled rules are ignored.
    RuleAction action{RuleAction::expire};     ///< expire (default) or exclude.
    int priority{0};                           ///< Higher wins (see policy-engine.md).
    std::optional<std::string> file;           ///< Exact managed path of one file.
    std::optional<std::string> path;           ///< Directory scope (managed path).
    bool recursive{false};                     ///< With path: include every sub-directory.
    std::optional<std::string> glob;           ///< Basename glob ("*.mp4").
    std::optional<std::string> path_glob;      ///< Whole-path glob ("/rec/*/*.mp4", "**" allowed).
    std::vector<std::string> extensions;       ///< Lower-case extensions without the dot.
    std::optional<Duration> retention;         ///< Lifetime; nullopt means "never" (expire rules).
    std::optional<TimestampSource> timestamp;  ///< Overrides retention.timestamp.
    int line{0};                               ///< 1-based source line, for messages.
};

/// "retention" section.
struct RetentionConfig {
    std::optional<Duration> default_retention;  ///< nullopt = "never" (the safe default).
    TimestampSource timestamp{TimestampSource::modification};
    std::vector<RuleConfig> rules;
};

/// "filesystem" section (restart required to change).
struct FilesystemConfig {
    std::string mountpoint{"/mnt/tsfs"};
    std::string backing_directory{"/var/lib/tsfs/data"};
    FilesystemMode mode{FilesystemMode::passthrough};
    bool allow_other{false};
    bool read_only{false};
    /// Read-only "user.tsfs.*" extended attributes (retention of each file, used by
    /// tsfs-ls). Costs one FUSE round trip per write (security.capability lookup).
    bool retention_xattrs{true};
};

/// "garbage_collector" section (reloadable).
struct GcConfig {
    bool enabled{true};
    bool dry_run{false};
    Duration scan_interval{std::chrono::minutes(1)};
    Duration max_scan_duration{std::chrono::seconds(5)};
    std::uint32_t max_workers{2};
    std::uint32_t max_deletions_per_second{100};  ///< Candidates examined per second (token each), 0 = unlimited.
    std::uint64_t max_deletions_per_cycle{0};     ///< Candidates dispatched per scan cycle, 0 = unlimited.
    std::uint64_t max_pending_tasks{100000};
    std::uint32_t batch_size{64};
    std::uint32_t retry_limit{5};
    Duration retry_initial_delay{std::chrono::seconds(1)};
    Duration retry_max_delay{std::chrono::minutes(1)};
    bool skip_open_files{true};
    SchedulerKind scheduler{SchedulerKind::heap};
};

/// "logging.rotation" sub-section.
struct LogRotationConfig {
    bool enabled{true};
    std::uint64_t max_file_size{50ULL << 20};
    std::uint32_t max_files{10};
    Duration check_interval{std::chrono::seconds(1)};
    bool compress{false};
    int compression_level{6};
    friend bool operator==(const LogRotationConfig&, const LogRotationConfig&) = default;
};

/// "logging.async" sub-section.
struct LogAsyncConfig {
    bool enabled{true};
    std::uint32_t queue_size{8192};
    LogOverflowPolicy overflow_policy{LogOverflowPolicy::block};
    friend bool operator==(const LogAsyncConfig&, const LogAsyncConfig&) = default;
};

/// "logging" section (reloadable).
struct LoggingConfig {
    bool enabled{true};
    std::string directory{"/var/log/tsfs"};
    std::string filename{"tsfs.log"};
    LogLevel level{LogLevel::info};
    LogFormat format{LogFormat::text};
    bool console{false};  ///< Also write to stderr.
    LogAsyncConfig async;
    LogRotationConfig rotation;
    friend bool operator==(const LoggingConfig&, const LoggingConfig&) = default;
};

/// "diagnostics" section (restart required to change).
struct DiagnosticsConfig {
    bool enabled{false};
    bool text_stack_trace{true};
    bool native_core_dump{false};
    std::string crash_directory{"/var/lib/tsfs/crash"};
    std::uint32_t max_crash_reports{10};
};

/// "metrics" section (reloadable).
struct MetricsConfig {
    bool enabled{true};
    std::string status_file{"/run/tsfs/status.json"};
    Duration interval{std::chrono::seconds(10)};
};

/// "security" section (restart required to change).
struct SecurityConfig {
    bool stay_on_filesystem{true};  ///< Never cross a mount point below the backing directory.
    bool require_openat2{false};    ///< Refuse to start when openat2(2) is unavailable.
};

/**
 * @brief A complete, validated configuration.
 *
 * Instances are immutable once published by ConfigManager: share them through
 * std::shared_ptr<const Config>.
 */
struct Config {
    int version{kConfigVersion};
    FilesystemConfig filesystem;
    RetentionConfig retention;
    GcConfig gc;
    LoggingConfig logging;
    DiagnosticsConfig diagnostics;
    MetricsConfig metrics;
    SecurityConfig security;
    std::string source_path;  ///< File the configuration was read from ("" for built-in).
};

/// @name Enumeration names as written in the YAML file.
///@{
const char* to_string(TimestampSource value) noexcept;
const char* to_string(RuleAction value) noexcept;
const char* to_string(LogLevel value) noexcept;
const char* to_string(LogFormat value) noexcept;
const char* to_string(LogOverflowPolicy value) noexcept;
const char* to_string(SchedulerKind value) noexcept;
std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;
///@}

}  // namespace tsfs
