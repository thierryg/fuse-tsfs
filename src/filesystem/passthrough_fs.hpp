// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/passthrough_fs.hpp
// Purpose : FUSE operations of the passthrough filesystem (descriptor-relative, symlink-safe)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <fuse.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_set>

#include "fsutil/backing_store.hpp"
#include "fsutil/open_file_registry.hpp"
#include "fsutil/path_lock_table.hpp"
#include "logging/logger.hpp"
#include "logging/rate_limiter.hpp"
#include "metrics/sharded_counter.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs {

/// FUSE operations counted in the statistics.
enum class FsOp : std::uint8_t {
    getattr,
    readlink,
    mknod,
    mkdir,
    unlink,
    rmdir,
    symlink,
    rename,
    link,
    chmod,
    chown,
    truncate,
    open,
    read,
    write,
    statfs,
    flush,
    release,
    fsync,
    opendir,
    readdir,
    releasedir,
    fsyncdir,
    access,
    create,
    utimens,
    fallocate,
    lseek,
    getxattr,
    listxattr,
    count_
};
inline constexpr std::size_t kFsOpCount = static_cast<std::size_t>(FsOp::count_);
/// Name of an operation ("getattr"...).
const char* to_string(FsOp op) noexcept;

/// Per-operation call and error counts.
struct FsStats {
    std::array<std::uint64_t, kFsOpCount> calls{};
    std::array<std::uint64_t, kFsOpCount> errors{};
    std::uint64_t bytes_read{0};
    std::uint64_t bytes_written{0};
    std::uint64_t open_handles{0};
};

/**
 * @brief The TSFS passthrough filesystem: exposes the backing directory at the mount point.
 *
 * Path semantics: the managed path "/a/b" handed by libfuse maps to
 * "<backing>/a/b". Parents are resolved through BackingStore (openat2 beneath the
 * root, no symlink, no mount crossing); the final component is accessed with
 * *at() calls and AT_SYMLINK_NOFOLLOW / O_NOFOLLOW. Symbolic links are stored and
 * returned verbatim (readlink) but never followed by TSFS itself; the kernel
 * follows them on the client side as for any filesystem.
 *
 * Permissions: the mount always uses "default_permissions", so the kernel checks
 * the mode bits/ownership reported by getattr against the caller's credentials.
 * When the daemon runs as root, new entries are chowned to the calling uid/gid
 * (gid of the parent directory when it is set-group-ID).
 *
 * Retention integration: create, mknod, rename, link and path-based truncate/utimens
 * notify the GarbageCollector at once (non-blocking); writes and handle-based
 * truncate/utimens/fallocate mark the handle and notify once, at release; open/create register the inode in the
 * OpenFileRegistry until release; namespace changes take the PathLockTable stripes.
 *
 * Not supported (ENOSYS to the kernel): extended attributes, ioctl, poll,
 * copy_file_range (the kernel falls back to read/write), BSD/POSIX locks are
 * handled locally by the kernel.
 *
 * Error handling: every callback returns 0 or a negative errno; C++ exceptions
 * are caught in the trampolines and become -EIO (-ENOMEM for bad_alloc).
 * Thread-safety: all methods are called concurrently by the libfuse worker threads.
 */
class PassthroughFs {
public:
    /// Collaborators (dependency injection).
    struct Dependencies {
        const BackingStore& store;
        Logger& log;
        PathLockTable& locks;
        OpenFileRegistry& open_files;
        /// Retention notification (GarbageCollector::notify_changed); may be empty.
        std::function<void(std::string_view path)> on_changed;
        bool read_only{false};
        /// Current retention policy, for the read-only "user.tsfs.*" extended
        /// attributes; may be empty (no attribute is then reported).
        std::function<std::shared_ptr<const PolicyEngine>()> policy;
    };

    explicit PassthroughFs(Dependencies deps);
    /// Frees the handles still open: libfuse does not call release() for files that
    /// are open when the session ends (unmount, SIGTERM before a pending RELEASE).
    /// Call only after the session loop has returned.
    ~PassthroughFs();
    PassthroughFs(const PassthroughFs&) = delete("owns the open handles");
    PassthroughFs& operator=(const PassthroughFs&) = delete("owns the open handles");

    /// @return the libfuse operation table (trampolines to the instance in private_data).
    /// @param with_xattrs Register getxattr/listxattr (filesystem.retention_xattrs).
    static const fuse_operations& operations(bool with_xattrs = true) noexcept;

    [[nodiscard]] FsStats stats() const;

    // --- operations (managed paths, negative errno results) ---------------------
    void* init(fuse_conn_info* conn, fuse_config* cfg);
    int getattr(const char* path, struct stat* st, fuse_file_info* fi);
    int readlink(const char* path, char* buf, size_t size);
    int mknod(const char* path, mode_t mode, dev_t rdev);
    int mkdir(const char* path, mode_t mode);
    int unlink(const char* path);
    int rmdir(const char* path);
    int symlink(const char* target, const char* path);
    int rename(const char* from, const char* to, unsigned int flags);
    int link(const char* from, const char* to);
    int chmod(const char* path, mode_t mode, fuse_file_info* fi);
    int chown(const char* path, uid_t uid, gid_t gid, fuse_file_info* fi);
    int truncate(const char* path, off_t size, fuse_file_info* fi);
    int open(const char* path, fuse_file_info* fi);
    int read_buf(const char* path, fuse_bufvec** bufp, size_t size, off_t offset, fuse_file_info* fi);
    int write_buf(const char* path, fuse_bufvec* buf, off_t offset, fuse_file_info* fi);
    int statfs(const char* path, struct statvfs* st);
    int flush(const char* path, fuse_file_info* fi);
    int release(const char* path, fuse_file_info* fi);
    int fsync(const char* path, int datasync, fuse_file_info* fi);
    int opendir(const char* path, fuse_file_info* fi);
    int readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, fuse_file_info* fi,
                fuse_readdir_flags flags);
    int releasedir(const char* path, fuse_file_info* fi);
    int fsyncdir(const char* path, int datasync, fuse_file_info* fi);
    int access(const char* path, int mask);
    int create(const char* path, mode_t mode, fuse_file_info* fi);
    int utimens(const char* path, const struct timespec tv[2], fuse_file_info* fi);
    int fallocate(const char* path, int mode, off_t offset, off_t length, fuse_file_info* fi);
    /**
     * @brief Read-only virtual extended attributes describing the retention of a
     *        regular file, computed from the current policy and fresh timestamps:
     *        "user.tsfs.state" (expires, expired, excluded, never),
     *        "user.tsfs.expires" (ISO 8601 UTC, only when the file expires) and
     *        "user.tsfs.rule" (deciding rule id, or "default") and "user.tsfs.created"
     *        (ISO 8601 UTC birth time, or st_mtime when the backing filesystem does not
     *        report it).
     * Every other name answers ENODATA without touching the disk (the kernel asks for
     * security.capability on writes). Real extended attributes of the backing files
     * are not exposed, and setxattr/removexattr stay unsupported.
     */
    int getxattr(const char* path, const char* name, char* value, size_t size);
    /// Lists no name (see the implementation: GNU ls would show a "+" for every file).
    int listxattr(const char* path, char* list, size_t size);
    off_t lseek(const char* path, off_t offset, int whence, fuse_file_info* fi);

    /// Account a call (used by the trampolines).
    void count(FsOp op, bool failed) noexcept {
        counters_.add(static_cast<std::size_t>(op));
        if (failed) {
            counters_.add(kFsOpCount + static_cast<std::size_t>(op));
        }
    }
    /// Report an unexpected failure (rate-limited warning).
    void report_failure(FsOp op, std::string_view path, int error);

    struct FileHandle;  ///< Opaque state behind fuse_file_info::fh for files.
    struct DirHandle;   ///< Opaque state behind fuse_file_info::fh for directories.

private:
    void changed(const char* path);
    /**
     * @brief Give an entry this daemon just created (root only) to the calling user.
     *
     * The entry is re-opened by name with O_PATH | O_NOFOLLOW and the ownership is
     * changed on that descriptor (no second name resolution). It is changed only if
     * the opened inode is still the one created here: type @p type, owned by the
     * daemon's effective uid, and, for a directory, empty (link count 2). A name
     * swapped in by a process writing directly into the backing directory therefore
     * keeps its owner (see docs/security/threat-model.md).
     */
    void give_to_caller(int dirfd, const char* name, mode_t type) const;
    /// Same checks and change on an open descriptor of the new entry (create()).
    void give_fd_to_caller(int fd, int dirfd, mode_t type) const;
    /**
     * @brief Mount boundary of the final path component (filesystem.stay_on_filesystem).
     *
     * The intermediate components are resolved without crossing mount points by the
     * BackingStore; this covers the last one (a file or directory bind-mounted inside
     * the backing tree). Only a privileged user can create such a mount in the host
     * namespace, so checking before acting is sufficient (no race to win).
     * @return 0, or -EXDEV when @p st is on another device than the backing root.
     */
    [[nodiscard]] int check_device(const struct stat& st) const noexcept;
    /// check_device() for the entry @p name of @p dirfd (not followed). @return 0 or -errno.
    [[nodiscard]] int check_name_device(int dirfd, const char* name) const noexcept;

    /// Take ownership of a new handle: @return the value for fuse_file_info::fh.
    std::uint64_t adopt(std::unique_ptr<FileHandle> handle);
    std::uint64_t adopt(std::unique_ptr<DirHandle> handle);
    /// Give back the ownership of the handle in @p fi (release / releasedir).
    std::unique_ptr<FileHandle> take_file(fuse_file_info* fi);
    std::unique_ptr<DirHandle> take_dir(fuse_file_info* fi);

    Dependencies deps_;
    bool chown_new_entries_{false};
    /// Handles currently owned by the kernel side (opened, not yet released). Only
    /// open/release take this mutex; read/write use the handle directly.
    std::mutex handles_mutex_;
    std::unordered_set<FileHandle*> live_files_;
    std::unordered_set<DirHandle*> live_dirs_;
    LogRateLimiter limiter_;
    ShardedCounters<2 * kFsOpCount + 2> counters_;  ///< calls, errors, bytes read, bytes written.
};

}  // namespace tsfs
