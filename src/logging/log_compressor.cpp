// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/log_compressor.cpp
// Purpose : Background gzip compression of rotated log files (bounded queue, verified, atomic)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "logging/log_compressor.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <optional>

#include "common/threads.hpp"

namespace tsfs {
namespace {

constexpr std::size_t kChunk = 64 * 1024;
constexpr std::uint32_t kMaxSearchIndex = 1000;

std::string temp_name(const std::string& filename, ino_t inode) {
    return std::format(".{}.compress-{}.tmp", filename, inode);
}

/// Index N such that "<filename>.N" has inode @p inode, if any. Caller holds the rotation lock.
std::optional<std::uint32_t> find_rotated(int dirfd, const std::string& filename, ino_t inode) {
    std::uint32_t misses = 0;
    for (std::uint32_t i = 1; i <= kMaxSearchIndex && misses < 3; ++i) {
        struct stat st{};
        if (::fstatat(dirfd, rotated_log_name(filename, i).c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
            // Gaps are possible (compressed entries only have the .gz name); stop after a few.
            struct stat gz{};
            misses +=
                ::fstatat(dirfd, rotated_log_name(filename, i, true).c_str(), &gz, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : 1;
            continue;
        }
        misses = 0;
        if (st.st_ino == inode && S_ISREG(st.st_mode)) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string rotated_log_name(const std::string& filename, std::uint32_t index, bool gz) {
    return std::format("{}.{}{}", filename, index, gz ? ".gz" : "");
}

bool gzip_stream_is_valid(int fd) {
    const int copy = ::dup(fd);
    if (copy < 0) {
        return false;
    }
    gzFile in = gzdopen(copy, "rb");
    if (in == nullptr) {
        ::close(copy);
        return false;
    }
    std::array<char, kChunk> buffer{};
    int n = 0;
    bool direct = false;
    while ((n = gzread(in, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
        // zlib reads non-gzip data "transparently": such a file is not a valid archive.
        direct = direct || gzdirect(in) != 0;
    }
    direct = direct || gzdirect(in) != 0;
    int err = Z_OK;
    gzerror(in, &err);
    const bool ok = n == 0 && err == Z_OK && !direct;
    return gzclose(in) == Z_OK && ok;
}

LogCompressor::LogCompressor(std::shared_ptr<NoticeQueue> notices, std::size_t capacity)
    : notices_(std::move(notices)), capacity_(capacity), thread_([this](std::stop_token st) { worker(st); }) {}

LogCompressor::~LogCompressor() {
    wait_idle();
    thread_.request_stop();
    cv_.notify_all();
}

bool LogCompressor::enqueue(Job job) {
    {
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void LogCompressor::wait_idle() {
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void LogCompressor::worker(std::stop_token stop) {
    prepare_helper_thread("tsfs-logzip");
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (queue_.empty()) {
                return;  // Stop requested and nothing left to do.
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        run_job(job);
        {
            std::scoped_lock lock(mutex_);
            busy_ = false;
        }
        idle_cv_.notify_all();
    }
}

bool LogCompressor::run_job(const Job& job) {
    const int dirfd = job.directory->get();
    const std::string tmp = temp_name(job.filename, job.inode);
    const auto fail = [&](const std::string& what, int err, int tmp_fd = -1) {
        if (tmp_fd >= 0) {
            ::close(tmp_fd);
        }
        ::unlinkat(dirfd, tmp.c_str(), 0);
        failed_.fetch_add(1);
        notices_->post({LogLevel::warning, "log_compression_failed",
                        std::format("{}: the rotated file is kept uncompressed{}{}", what, err != 0 ? ": " : "",
                                    err != 0 ? std::strerror(err) : ""),
                        job.filename, err});
        return false;
    };

    UniqueFd input;
    std::string source_name;
    {
        std::scoped_lock lock(*job.rotation_lock);
        const auto index = find_rotated(dirfd, job.filename, job.inode);
        if (!index) {
            return true;  // Already rotated out of retention: nothing to compress.
        }
        source_name = rotated_log_name(job.filename, *index);
        input.reset(::openat(dirfd, source_name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
        if (!input) {
            return fail("cannot open " + source_name, errno);
        }
    }
    notices_->post({LogLevel::debug, "log_compression_started", "compressing a rotated log file", source_name, 0});

    const int out = ::openat(dirfd, tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (out < 0) {
        return fail("cannot create " + tmp, errno);
    }
    const int gz_fd = ::dup(out);
    gzFile gz = gz_fd >= 0 ? gzdopen(gz_fd, std::format("wb{}", job.level).c_str()) : nullptr;
    if (gz == nullptr) {
        if (gz_fd >= 0) {
            ::close(gz_fd);
        }
        return fail("cannot initialize zlib", errno, out);
    }
    std::array<char, kChunk> buffer{};
    uLong crc = crc32(0L, Z_NULL, 0);
    std::uint64_t in_bytes = 0;
    for (;;) {
        const ssize_t n = ::read(input.get(), buffer.data(), buffer.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            gzclose(gz);
            return fail("read error on " + source_name, err, out);
        }
        if (n == 0) {
            break;
        }
        crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(n));
        in_bytes += static_cast<std::uint64_t>(n);
        if (gzwrite(gz, buffer.data(), static_cast<unsigned>(n)) != static_cast<int>(n)) {
            gzclose(gz);
            return fail("compression write error (disk full?)", ENOSPC, out);
        }
    }
    if (gzclose(gz) != Z_OK) {
        return fail("compression finalization error (disk full?)", ENOSPC, out);
    }
    if (::fsync(out) != 0) {
        return fail("fsync of the compressed file failed", errno, out);
    }

    // Verify: decompress what was written and compare with the source.
    {
        const int check_fd = ::openat(dirfd, tmp.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        gzFile check = check_fd >= 0 ? gzdopen(check_fd, "rb") : nullptr;
        if (check == nullptr) {
            if (check_fd >= 0) {
                ::close(check_fd);
            }
            return fail("cannot reopen the compressed file", errno, out);
        }
        uLong verify_crc = crc32(0L, Z_NULL, 0);
        std::uint64_t out_bytes = 0;
        int n = 0;
        while ((n = gzread(check, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
            verify_crc = crc32(verify_crc, reinterpret_cast<const Bytef*>(buffer.data()), static_cast<uInt>(n));
            out_bytes += static_cast<std::uint64_t>(n);
        }
        const bool read_ok = n == 0;
        gzclose(check);
        if (!read_ok || verify_crc != crc || out_bytes != in_bytes) {
            return fail("verification of the compressed file failed", EIO, out);
        }
    }
    ::close(out);

    {
        std::scoped_lock lock(*job.rotation_lock);
        const auto index = find_rotated(dirfd, job.filename, job.inode);
        if (!index) {
            ::unlinkat(dirfd, tmp.c_str(), 0);  // Rotated out meanwhile: drop the copy.
            return true;
        }
        const std::string raw = rotated_log_name(job.filename, *index);
        const std::string compressed = rotated_log_name(job.filename, *index, true);
        if (::renameat(dirfd, tmp.c_str(), dirfd, compressed.c_str()) != 0) {
            return fail("cannot rename the compressed file", errno);
        }
        // Only now is the original redundant.
        if (::unlinkat(dirfd, raw.c_str(), 0) != 0 && errno != ENOENT) {
            notices_->post({LogLevel::warning, "log_compression_failed",
                            "compressed copy written but the original could not be removed", raw, errno});
        }
        notices_->post({LogLevel::debug, "log_compression_completed", std::format("compressed {} bytes", in_bytes),
                        compressed, 0});
    }
    completed_.fetch_add(1);
    return true;
}

}  // namespace tsfs
