// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/rotating_file_sink.hpp
// Purpose : spdlog sink writing tsfs.log with size-based rotation to tsfs.log.1..N
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <spdlog/sinks/base_sink.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

#include "common/error.hpp"
#include "common/unique_fd.hpp"
#include "config/config.hpp"
#include "logging/log_compressor.hpp"
#include "logging/notice_queue.hpp"

namespace tsfs {

/**
 * @brief The TSFS log file sink: single owner of the active file and of its rotation.
 *
 * Why not spdlog's rotating_file_sink: it names files "tsfs.1.log", cannot hand
 * rotated files to a compressor, and does not notice an externally deleted or
 * replaced active file. This sink replaces it (it is the only rotation mechanism).
 *
 * Semantics:
 * - The current size is tracked in memory (no stat() per message). A message that
 *   would make the active file exceed max_file_size triggers a rotation *before* it
 *   is written, so the active file never exceeds the limit, except when a single
 *   message is larger than the limit or another process appends to the file.
 * - Rotation: "<f>.N"/"<f>.N.gz" with N = max_files are removed, every "<f>.i"
 *   (and ".gz") is renamed to i+1 in descending order, the active file becomes
 *   "<f>.1", a new active file is created (mode 0640). At most max_files rotated
 *   files are kept, the active file not included.
 * - Every check_interval the sink compares the path with the open descriptor and
 *   reopens when the active file was deleted, replaced or truncated externally.
 * - Write failures (ENOSPC, EIO...) never throw and never block: the message is
 *   counted as lost, a rate-limited notice goes to stderr, and the next write retries.
 * - A failed rotation leaves logging on the current file and is retried after
 *   check_interval (no rotation storm).
 * - At startup leftovers of an interrupted rotation/compression are repaired.
 * Events are reported through the NoticeQueue (never through the logger itself).
 *
 * Thread-safety: spdlog's base_sink serializes sink_it_/flush_ with its mutex.
 * Lock order: sink mutex -> rotation lock (the compressor only takes the latter).
 */
class RotatingFileSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    /// Settings of one sink instance (taken from LoggingConfig).
    struct Options {
        std::string directory;
        std::string filename;
        LogRotationConfig rotation;
        mode_t file_mode{0640};
        mode_t directory_mode{0750};
    };

    /**
     * @brief Create the directory if needed, repair leftovers, open the active file.
     * @return the sink, or the errno of the failure (EACCES, ENOSPC, ENOTDIR...).
     */
    static Result<std::shared_ptr<RotatingFileSink>> create(Options options, std::shared_ptr<NoticeQueue> notices,
                                                            std::shared_ptr<LogCompressor> compressor);

    /// Check that the log file can be created/opened (startup preflight, no side effect but creation).
    static Result<void> check_writable(const std::string& directory, const std::string& filename);

    /// Force a rotation now (tests and administration).
    void rotate_now();

    [[nodiscard]] std::uint64_t rotations() const noexcept { return rotations_.load(); }
    [[nodiscard]] std::uint64_t lost_messages() const noexcept { return lost_.load(); }
    [[nodiscard]] std::uint64_t reopen_count() const noexcept { return reopens_.load(); }

    /// Constructed through create() only (public for std::make_shared).
    RotatingFileSink(Options options, std::shared_ptr<NoticeQueue> notices, std::shared_ptr<LogCompressor> compressor);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    Result<void> open_directory();
    Result<void> open_active();
    void repair_leftovers();
    void rotate_locked();
    void check_external_locked();
    bool write_all(const char* data, std::size_t size);
    void notice(LogLevel level, const char* event, std::string message, const std::string& path, int err);

    Options options_;
    std::shared_ptr<NoticeQueue> notices_;
    std::shared_ptr<LogCompressor> compressor_;
    std::shared_ptr<UniqueFd> dir_;  ///< Shared with compression jobs.
    std::shared_ptr<std::mutex> rotation_lock_{std::make_shared<std::mutex>()};
    UniqueFd fd_;
    ino_t inode_{0};
    std::uint64_t size_{0};
    std::chrono::steady_clock::time_point next_check_{};
    std::chrono::steady_clock::time_point rotation_retry_after_{};
    bool failing_{false};
    std::uint64_t lost_in_streak_{0};
    std::atomic<std::uint64_t> rotations_{0};
    std::atomic<std::uint64_t> lost_{0};
    std::atomic<std::uint64_t> reopens_{0};
};

}  // namespace tsfs
