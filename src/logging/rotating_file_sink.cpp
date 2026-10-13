// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/logging/rotating_file_sink.cpp
// Purpose : spdlog sink writing tsfs.log with size-based rotation to tsfs.log.1..N
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "logging/rotating_file_sink.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <map>

namespace tsfs {
namespace {

/// mkdir -p with @p mode for the missing components. @return 0 or errno.
int make_directories(const std::string& path, mode_t mode) {
    std::size_t pos = 1;
    while (pos <= path.size()) {
        const std::size_t end = std::min(path.find('/', pos), path.size());
        const std::string prefix = path.substr(0, end);
        if (::mkdir(prefix.c_str(), mode) != 0 && errno != EEXIST) {
            return errno;
        }
        pos = end + 1;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return errno;
    }
    return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
}

constexpr int kActiveFlags = O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC;

}  // namespace

RotatingFileSink::RotatingFileSink(Options options, std::shared_ptr<NoticeQueue> notices,
                                   std::shared_ptr<LogCompressor> compressor)
    : options_(std::move(options)), notices_(std::move(notices)), compressor_(std::move(compressor)) {}

Result<std::shared_ptr<RotatingFileSink>> RotatingFileSink::create(Options options,
                                                                   std::shared_ptr<NoticeQueue> notices,
                                                                   std::shared_ptr<LogCompressor> compressor) {
    auto sink = std::make_shared<RotatingFileSink>(std::move(options), std::move(notices), std::move(compressor));
    if (auto r = sink->open_directory(); !r) {
        return propagate(r.error());
    }
    sink->repair_leftovers();
    if (auto r = sink->open_active(); !r) {
        return propagate(r.error());
    }
    return sink;
}

Result<void> RotatingFileSink::check_writable(const std::string& directory, const std::string& filename) {
    if (const int err = make_directories(directory, 0750); err != 0) {
        return make_error(err, std::format("cannot create the log directory '{}': {}", directory, std::strerror(err)));
    }
    const std::string path = directory + "/" + filename;
    const int fd = ::open(path.c_str(), kActiveFlags, 0640);
    if (fd < 0) {
        const int err = errno;
        return make_error(err, std::format("cannot open the log file '{}': {}", path, std::strerror(err)));
    }
    ::close(fd);
    return {};
}

Result<void> RotatingFileSink::open_directory() {
    if (const int err = make_directories(options_.directory, options_.directory_mode); err != 0) {
        return make_error(
            err, std::format("cannot create the log directory '{}': {}", options_.directory, std::strerror(err)));
    }
    const int fd = ::open(options_.directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        return make_error(
            err, std::format("cannot open the log directory '{}': {}", options_.directory, std::strerror(err)));
    }
    dir_ = std::make_shared<UniqueFd>(fd);
    return {};
}

Result<void> RotatingFileSink::open_active() {
    UniqueFd fd(::openat(dir_->get(), options_.filename.c_str(), kActiveFlags, options_.file_mode));
    if (!fd) {
        const int err = errno;
        return make_error(err, std::format("cannot open the log file '{}/{}': {}", options_.directory,
                                           options_.filename, std::strerror(err)));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        const int err = errno;
        return make_error(err, std::format("cannot stat the log file: {}", std::strerror(err)));
    }
    if (!S_ISREG(st.st_mode)) {
        return make_error(EINVAL, std::format("'{}/{}' is not a regular file", options_.directory, options_.filename));
    }
    fd_ = std::move(fd);
    inode_ = st.st_ino;
    size_ = static_cast<std::uint64_t>(st.st_size);
    return {};
}

void RotatingFileSink::repair_leftovers() {
    // Recovery after a crash during rotation or compression:
    //  - temporary compression files are incomplete by definition: removed;
    //  - "<f>.N" and "<f>.N.gz" both present: the rename of the verified .gz happened
    //    but the unlink of the original did not; keep the .gz if it is a valid gzip
    //    stream, otherwise keep the original;
    //  - entries above max_files (max_files lowered): removed.
    const int dfd = ::openat(dir_->get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) {
        return;
    }
    DIR* dir = ::fdopendir(dfd);
    if (dir == nullptr) {
        ::close(dfd);
        return;
    }
    const std::string temp_prefix = "." + options_.filename + ".compress-";
    const std::string rotated_prefix = options_.filename + ".";
    std::map<std::uint32_t, std::pair<bool, bool>> rotated;  // index -> (raw, gz)
    std::vector<std::string> temporaries;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string_view name = entry->d_name;
        if (name.starts_with(temp_prefix) && name.ends_with(".tmp")) {
            temporaries.emplace_back(name);
            continue;
        }
        if (!name.starts_with(rotated_prefix)) {
            continue;
        }
        std::string_view rest = name.substr(rotated_prefix.size());
        const bool gz = rest.ends_with(".gz");
        if (gz) {
            rest.remove_suffix(3);
        }
        std::uint32_t index = 0;
        const auto [ptr, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), index);
        if (ec != std::errc{} || ptr != rest.data() + rest.size() || index == 0 || rest.starts_with('0')) {
            continue;  // Not ours (e.g. "tsfs.log.old"): never touched.
        }
        auto& [has_raw, has_gz] = rotated[index];
        (gz ? has_gz : has_raw) = true;
    }
    ::closedir(dir);

    const int d = dir_->get();
    for (const std::string& name : temporaries) {
        ::unlinkat(d, name.c_str(), 0);
    }
    for (const auto& [index, kinds] : rotated) {
        const auto& [has_raw, has_gz] = kinds;
        const std::string raw = rotated_log_name(options_.filename, index);
        const std::string gz = rotated_log_name(options_.filename, index, true);
        if (index > options_.rotation.max_files) {
            ::unlinkat(d, raw.c_str(), 0);
            ::unlinkat(d, gz.c_str(), 0);
            continue;
        }
        if (has_raw && has_gz) {
            UniqueFd fd(::openat(d, gz.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
            if (fd && gzip_stream_is_valid(fd.get())) {
                ::unlinkat(d, raw.c_str(), 0);
            } else {
                ::unlinkat(d, gz.c_str(), 0);
            }
            notice(LogLevel::warning, "log_repair_completed", "repaired an interrupted compression", raw, 0);
        }
    }
}

void RotatingFileSink::notice(LogLevel level, const char* event, std::string message, const std::string& path,
                              int err) {
    notices_->post({level, event, std::move(message), path, err});
}

bool RotatingFileSink::write_all(const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd_.get(), data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
        size_ += static_cast<std::uint64_t>(n);
    }
    return true;
}

void RotatingFileSink::check_external_locked() {
    if (!fd_) {
        if (open_active()) {
            reopens_.fetch_add(1);
            notice(LogLevel::warning, "log_file_reopened", "the log file could be opened again", options_.filename, 0);
        }
        return;
    }
    struct stat on_disk{};
    const bool exists = ::fstatat(dir_->get(), options_.filename.c_str(), &on_disk, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && on_disk.st_ino == inode_) {
        size_ = static_cast<std::uint64_t>(on_disk.st_size);  // Account for external truncation/appends.
        return;
    }
    // Deleted or replaced externally (e.g. by a misconfigured logrotate): start a new file.
    if (auto r = open_active(); r) {
        reopens_.fetch_add(1);
        notice(LogLevel::warning, "log_file_reopened",
               exists ? "the active log file was replaced externally; reopened"
                      : "the active log file was deleted externally; recreated",
               options_.filename, 0);
    } else {
        notice(LogLevel::error, "log_write_failed", r.error().message, options_.filename, r.error().code);
    }
}

void RotatingFileSink::rotate_now() {
    std::scoped_lock lock(mutex_);
    rotate_locked();
}

void RotatingFileSink::rotate_locked() {
    const auto now = std::chrono::steady_clock::now();
    const std::uint32_t max = options_.rotation.max_files;
    const std::string& f = options_.filename;
    const int d = dir_->get();
    ino_t rotated_inode = 0;
    notice(LogLevel::debug, "log_rotation_started", std::format("rotating {} (keeping {} files)", f, max), f, 0);
    {
        std::scoped_lock rotation(*rotation_lock_);
        const auto remove = [&](const std::string& name) {
            if (::unlinkat(d, name.c_str(), 0) != 0 && errno != ENOENT) {
                notice(LogLevel::warning, "log_rotation_failed", "cannot remove an expired rotated file", name, errno);
            }
        };
        const auto shift = [&](const std::string& from, const std::string& to) {
            if (::renameat(d, from.c_str(), d, to.c_str()) != 0 && errno != ENOENT) {
                notice(LogLevel::warning, "log_rotation_failed", "cannot rename " + from + " to " + to, from, errno);
            }
        };
        remove(rotated_log_name(f, max));
        remove(rotated_log_name(f, max, true));
        for (std::uint32_t i = max - 1; i >= 1; --i) {
            shift(rotated_log_name(f, i), rotated_log_name(f, i + 1));
            shift(rotated_log_name(f, i, true), rotated_log_name(f, i + 1, true));
        }
        if (::renameat(d, f.c_str(), d, rotated_log_name(f, 1).c_str()) != 0) {
            const int err = errno;
            rotation_retry_after_ = now + options_.rotation.check_interval;
            notice(LogLevel::error, "log_rotation_failed",
                   std::format("cannot rename the active log file: {}; logging continues in it", std::strerror(err)), f,
                   err);
            stderr_fallback().report(std::format("log rotation failed: {}", std::strerror(err)));
            return;
        }
        rotated_inode = inode_;
        UniqueFd previous = std::move(fd_);
        if (auto r = open_active(); !r) {
            // Keep writing into the file that is now "<f>.1"; the periodic check retries.
            fd_ = std::move(previous);
            rotation_retry_after_ = now + options_.rotation.check_interval;
            notice(LogLevel::error, "log_rotation_failed",
                   r.error().message + "; logging continues in " + rotated_log_name(f, 1), f, r.error().code);
            return;
        }
    }
    rotations_.fetch_add(1);
    notice(LogLevel::info, "log_rotation_completed", std::format("rotated to {}", rotated_log_name(f, 1)), f, 0);
    if (options_.rotation.compress && compressor_) {
        if (!compressor_->enqueue({dir_, f, rotated_inode, options_.rotation.compression_level, rotation_lock_})) {
            notice(LogLevel::warning, "log_compression_failed", "compression queue full; file kept uncompressed",
                   rotated_log_name(f, 1), 0);
        }
    }
}

void RotatingFileSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_check_) {
        check_external_locked();
        next_check_ = now + options_.rotation.check_interval;
    }
    if (options_.rotation.enabled && fd_ && size_ > 0 && size_ + formatted.size() > options_.rotation.max_file_size &&
        now >= rotation_retry_after_) {
        rotate_locked();
    }
    if (fd_ && write_all(formatted.data(), formatted.size())) {
        if (failing_) {
            failing_ = false;
            notice(LogLevel::warning, "log_write_failed",
                   std::format("log writes recovered after {} lost message(s)", lost_in_streak_), options_.filename, 0);
            lost_in_streak_ = 0;
        }
        return;
    }
    const int err = fd_ ? errno : EBADF;
    lost_.fetch_add(1);
    ++lost_in_streak_;
    if (!failing_) {
        failing_ = true;
        notice(LogLevel::error, "log_write_failed", std::format("cannot write the log file: {}", std::strerror(err)),
               options_.filename, err);
    }
    stderr_fallback().report(
        std::format("cannot write {}/{}: {}", options_.directory, options_.filename, std::strerror(err)));
}

void RotatingFileSink::flush_() {
    // Messages are written with write(2) as they arrive: there is no user-space buffer.
}

}  // namespace tsfs
