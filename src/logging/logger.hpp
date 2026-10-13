// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/logger.hpp
// Purpose : Structured logging facade: levels, events, async pipeline, hot reconfiguration
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <source_location>
#include <string_view>

#include "common/error.hpp"
#include "config/config.hpp"
#include "logging/log_field.hpp"
#include "logging/notice_queue.hpp"

namespace tsfs {

/// Counters of the logging subsystem (cumulative since start).
struct LoggerStats {
    std::uint64_t queue_overruns{0};  ///< Messages lost by overrun_oldest / discard_new.
    std::uint64_t lost_writes{0};     ///< Messages the file sink could not write.
    std::uint64_t rotations{0};
    std::uint64_t reconfigurations{0};
    std::uint64_t notices_dropped{0};
};

/**
 * @brief The TSFS logger: one instance per process, owned by the daemon and passed
 *        by reference to every component (no global logger).
 *
 * Line format, text:
 * `2026-10-13T10:15:30.123+02:00 INFO pid=1234 tid=1235 component=gc event=gc_file_deleted path="/a" msg="..."`
 * JSON Lines:
 * `{"ts":"2026-10-13T10:15:30.123+02:00","level":"info","pid":1234,"tid":1235,"component":"gc",...}`
 *
 * Lifecycle: the constructor provides a synchronous stderr "bootstrap" logger usable
 * before the configuration is known; configure() builds a new pipeline (file sink,
 * optional stderr, optional async queue) and publishes it atomically only once it
 * is fully initialized. A failed configure() leaves the previous pipeline in place.
 * Queued messages of a replaced pipeline are written to the previous sink before
 * that pipeline is destroyed (spdlog drains its queue on destruction).
 *
 * Thread-safety: every method may be called concurrently. log() takes no lock of
 * its own; the level check is a relaxed atomic load, so disabled levels cost one
 * comparison.
 */
class Logger {
public:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete("the logger is shared by reference");
    Logger& operator=(const Logger&) = delete("the logger is shared by reference");

    /**
     * @brief Build and publish a pipeline for @p config.
     * @param config        The "logging" section to apply.
     * @param force_console Also write to stderr (CLI --foreground --debug).
     * @return EACCES/ENOENT... when the log file cannot be opened; the previous
     *         pipeline stays active in that case.
     */
    Result<void> configure(const LoggingConfig& config, bool force_console = false);

    /// Change the minimum level immediately (also applied by configure()).
    void set_level(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept {
        return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
    }
    /// @return true when a message of @p level would be emitted.
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return static_cast<int>(level) >= level_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Emit one structured event.
     * @param level     Severity; below the active level the call costs one comparison.
     * @param component Subsystem ("gc", "fs", "config"...): a trusted literal.
     * @param event     Stable event identifier ("gc_file_deleted"): a trusted literal.
     * @param message   Human-readable text (escaped).
     * @param fields    Typed values; strings are escaped (paths may contain anything).
     * @param location  Call site, recorded as src="file:line" (defaulted by the compiler).
     */
    void log(LogLevel level, std::string_view component, std::string_view event, std::string_view message,
             std::initializer_list<LogField> fields = {},
             std::source_location location = std::source_location::current());

    /// Log pending notices of the sink/compressor and queue-overflow counters. Call periodically.
    void maintenance();
    /// Write out everything queued so far.
    void flush();
    /// Flush and release the pipeline (the bootstrap stderr logger remains).
    void shutdown();

    [[nodiscard]] LoggerStats stats() const;
    /// @return the active configuration (bootstrap: logging disabled, stderr only).
    [[nodiscard]] LoggingConfig active_config() const;

    struct Pipeline;  ///< Opaque: spdlog logger + sinks + optional thread pool.

private:
    std::shared_ptr<Pipeline> make_bootstrap() const;
    void emit_notice(const LogNotice& notice);

    std::atomic<int> level_;
    std::atomic<std::shared_ptr<Pipeline>> pipeline_;
    std::shared_ptr<NoticeQueue> notices_{std::make_shared<NoticeQueue>()};
    std::mutex configure_mutex_;  ///< Serializes configure()/shutdown(); never held by log().
    std::atomic<std::uint64_t> reconfigurations_{0};
    std::atomic<std::uint64_t> overruns_reported_{0};
    std::atomic<std::uint64_t> retired_overruns_{0};
    std::atomic<std::uint64_t> retired_lost_{0};
    std::atomic<std::uint64_t> retired_rotations_{0};
};

}  // namespace tsfs
