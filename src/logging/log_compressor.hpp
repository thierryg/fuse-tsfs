// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/log_compressor.hpp
// Purpose : Background gzip compression of rotated log files (bounded queue, verified, atomic)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/unique_fd.hpp"
#include "logging/notice_queue.hpp"

namespace tsfs {

/// Name of rotated file @p index ("tsfs.log.3"), optionally with ".gz".
std::string rotated_log_name(const std::string& filename, std::uint32_t index, bool gz = false);

/**
 * @brief Compresses rotated log files into "<name>.N.gz" on a dedicated thread.
 *
 * Protocol (see docs/architecture/logging.md):
 *  1. under the rotation lock, find the rotated file by inode and open it;
 *  2. compress into a private temporary file ".<filename>.compress-<inode>.tmp",
 *     fsync it, then decompress it again and compare length and CRC-32;
 *  3. under the rotation lock, find the file again (a rotation may have shifted
 *     it to N+1), rename the temporary file to "<name>.N.gz", then unlink "<name>.N".
 * The uncompressed file is removed only after its verified replacement exists, so a
 * failure (disk full, crash) never loses log data: the original is kept.
 * The active log file is never compressed.
 *
 * The queue is bounded (capacity jobs); enqueue() returns false when it is full and
 * the file simply stays uncompressed. The destructor finishes queued jobs.
 * Thread-safe.
 */
class LogCompressor {
public:
    /// One file to compress.
    struct Job {
        std::shared_ptr<UniqueFd> directory;        ///< Log directory (O_PATH), shared with the sink.
        std::string filename;                       ///< Active file name ("tsfs.log").
        ino_t inode{0};                             ///< Identity of the rotated file.
        int level{6};                               ///< zlib level 1..9.
        std::shared_ptr<std::mutex> rotation_lock;  ///< Serializes renames with the sink.
    };

    LogCompressor(std::shared_ptr<NoticeQueue> notices, std::size_t capacity = 8);
    ~LogCompressor();
    LogCompressor(const LogCompressor&) = delete("owns a worker thread");
    LogCompressor& operator=(const LogCompressor&) = delete("owns a worker thread");

    /// Queue a job. @return false when the queue is full (the job is not taken).
    bool enqueue(Job job);
    /// Block until every queued job has been processed (tests, shutdown).
    void wait_idle();
    /// @return number of files successfully compressed.
    [[nodiscard]] std::uint64_t completed() const noexcept { return completed_.load(); }
    /// @return number of failed compressions.
    [[nodiscard]] std::uint64_t failed() const noexcept { return failed_.load(); }

    /// Compress one job synchronously (exposed for tests). @return true on success.
    bool run_job(const Job& job);

private:
    void worker(std::stop_token stop);

    std::shared_ptr<NoticeQueue> notices_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::condition_variable idle_cv_;
    std::deque<Job> queue_;
    bool busy_{false};
    std::atomic<std::uint64_t> completed_{0};
    std::atomic<std::uint64_t> failed_{0};
    std::jthread thread_;
};

/// @return true when @p fd (positioned at 0) is a complete, valid gzip stream.
bool gzip_stream_is_valid(int fd);

}  // namespace tsfs
