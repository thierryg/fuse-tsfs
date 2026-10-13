// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/passthrough_fs.cpp
// Purpose : FUSE operations of the passthrough filesystem (descriptor-relative, symlink-safe)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "filesystem/passthrough_fs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <type_traits>

namespace tsfs {

/// State of an open file: the backing descriptor and what retention needs to know.
struct PassthroughFs::FileHandle {
    UniqueFd fd;
    FileIdentity identity;
    bool modified{false};  ///< Written, truncated or created: notify the GC on release.
};

/// State of an open directory stream (offsets follow telldir/seekdir).
struct PassthroughFs::DirHandle {
    std::mutex mutex;
    DIR* dir{nullptr};
    off_t offset{0};
    dirent* entry{nullptr};
    ~DirHandle() {
        if (dir != nullptr) {
            ::closedir(dir);
        }
    }
};

namespace {

constexpr int neg(int err) noexcept { return -err; }
int neg_errno() noexcept { return -errno; }

/// A buffer backed by a file descriptor at an explicit position (libfuse flag set: the
/// combined value is not an enumerator, which the analyzer reports).
// NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
constexpr auto kFdBufferFlags = static_cast<fuse_buf_flags>(FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK);

PassthroughFs::FileHandle* file_of(const fuse_file_info* fi) noexcept {
    return reinterpret_cast<PassthroughFs::FileHandle*>(fi->fh);  // NOLINT(performance-no-int-to-ptr)
}

/**
 * Generic C trampoline for a PassthroughFs member: fetches the instance from the
 * FUSE context, converts exceptions to errno values (nothing unwinds into libfuse),
 * counts the call and reports unexpected failures with the path argument, found by
 * C++26 pack indexing when the first parameter is a path.
 */
template <auto Method, FsOp Op>
struct Trampoline;

template <typename R, typename... Args, R (PassthroughFs::*Method)(Args...), FsOp Op>
struct Trampoline<Method, Op> {
    static R call(Args... args) noexcept {
        auto* self = static_cast<PassthroughFs*>(fuse_get_context()->private_data);
        R result{};
        try {
            result = (self->*Method)(args...);
        } catch (const std::bad_alloc&) {
            result = -ENOMEM;
        } catch (...) {
            result = -EIO;
        }
        const bool failed = result < 0;
        self->count(Op, failed);
        if (failed) {
            std::string_view path;
            if constexpr (sizeof...(Args) > 0) {
                if constexpr (std::is_same_v<Args...[0], const char*>) {
                    const char* first = args...[0];
                    path = first != nullptr ? first : "";
                }
            }
            self->report_failure(Op, path, static_cast<int>(-result));
        }
        return result;
    }
};

template <auto Method, FsOp Op>
constexpr auto tramp = &Trampoline<Method, Op>::call;

}  // namespace

const char* to_string(FsOp op) noexcept {
    static constexpr std::array<const char*, kFsOpCount> kNames{
        "getattr", "readlink",   "mknod",    "mkdir",  "unlink", "rmdir",   "symlink",   "rename",  "link",  "chmod",
        "chown",   "truncate",   "open",     "read",   "write",  "statfs",  "flush",     "release", "fsync", "opendir",
        "readdir", "releasedir", "fsyncdir", "access", "create", "utimens", "fallocate", "lseek"};
    const auto index = static_cast<std::size_t>(op);
    return index < kNames.size() ? kNames[index] : "unknown";
}

PassthroughFs::PassthroughFs(Dependencies deps) : deps_(std::move(deps)), chown_new_entries_(::geteuid() == 0) {}

PassthroughFs::~PassthroughFs() {
    const std::scoped_lock lock(handles_mutex_);
    if (!live_files_.empty() || !live_dirs_.empty()) {
        deps_.log.log(LogLevel::info, "fs", "filesystem_handles_released",
                      "handles still open at the end of the session released",
                      {{"files", live_files_.size()}, {"directories", live_dirs_.size()}});
    }
    for (FileHandle* handle : live_files_) {
        deps_.open_files.remove(handle->identity);
        delete handle;  // NOLINT(cppcoreguidelines-owning-memory): owned through live_files_.
    }
    for (DirHandle* handle : live_dirs_) {
        delete handle;  // NOLINT(cppcoreguidelines-owning-memory): owned through live_dirs_.
    }
}

std::uint64_t PassthroughFs::adopt(std::unique_ptr<FileHandle> handle) {
    const std::scoped_lock lock(handles_mutex_);
    live_files_.insert(handle.get());
    return reinterpret_cast<std::uint64_t>(handle.release());
}

std::uint64_t PassthroughFs::adopt(std::unique_ptr<DirHandle> handle) {
    const std::scoped_lock lock(handles_mutex_);
    live_dirs_.insert(handle.get());
    return reinterpret_cast<std::uint64_t>(handle.release());
}

std::unique_ptr<PassthroughFs::FileHandle> PassthroughFs::take_file(fuse_file_info* fi) {
    std::unique_ptr<FileHandle> handle(file_of(fi));
    fi->fh = 0;
    const std::scoped_lock lock(handles_mutex_);
    live_files_.erase(handle.get());
    return handle;
}

std::unique_ptr<PassthroughFs::DirHandle> PassthroughFs::take_dir(fuse_file_info* fi) {
    std::unique_ptr<DirHandle> handle(reinterpret_cast<DirHandle*>(fi->fh));  // NOLINT(performance-no-int-to-ptr)
    fi->fh = 0;
    const std::scoped_lock lock(handles_mutex_);
    live_dirs_.erase(handle.get());
    return handle;
}

const fuse_operations& PassthroughFs::operations() noexcept {
    static const fuse_operations ops = [] {
        fuse_operations o{};
        o.init = [](fuse_conn_info* conn, fuse_config* cfg) -> void* {
            auto* self = static_cast<PassthroughFs*>(fuse_get_context()->private_data);
            return self->init(conn, cfg);
        };
        o.getattr = tramp<&PassthroughFs::getattr, FsOp::getattr>;
        o.readlink = tramp<&PassthroughFs::readlink, FsOp::readlink>;
        o.mknod = tramp<&PassthroughFs::mknod, FsOp::mknod>;
        o.mkdir = tramp<&PassthroughFs::mkdir, FsOp::mkdir>;
        o.unlink = tramp<&PassthroughFs::unlink, FsOp::unlink>;
        o.rmdir = tramp<&PassthroughFs::rmdir, FsOp::rmdir>;
        o.symlink = tramp<&PassthroughFs::symlink, FsOp::symlink>;
        o.rename = tramp<&PassthroughFs::rename, FsOp::rename>;
        o.link = tramp<&PassthroughFs::link, FsOp::link>;
        o.chmod = tramp<&PassthroughFs::chmod, FsOp::chmod>;
        o.chown = tramp<&PassthroughFs::chown, FsOp::chown>;
        o.truncate = tramp<&PassthroughFs::truncate, FsOp::truncate>;
        o.open = tramp<&PassthroughFs::open, FsOp::open>;
        o.read_buf = tramp<&PassthroughFs::read_buf, FsOp::read>;
        o.write_buf = tramp<&PassthroughFs::write_buf, FsOp::write>;
        o.statfs = tramp<&PassthroughFs::statfs, FsOp::statfs>;
        o.flush = tramp<&PassthroughFs::flush, FsOp::flush>;
        o.release = tramp<&PassthroughFs::release, FsOp::release>;
        o.fsync = tramp<&PassthroughFs::fsync, FsOp::fsync>;
        o.opendir = tramp<&PassthroughFs::opendir, FsOp::opendir>;
        o.readdir = tramp<&PassthroughFs::readdir, FsOp::readdir>;
        o.releasedir = tramp<&PassthroughFs::releasedir, FsOp::releasedir>;
        o.fsyncdir = tramp<&PassthroughFs::fsyncdir, FsOp::fsyncdir>;
        o.access = tramp<&PassthroughFs::access, FsOp::access>;
        o.create = tramp<&PassthroughFs::create, FsOp::create>;
        o.utimens = tramp<&PassthroughFs::utimens, FsOp::utimens>;
        o.fallocate = tramp<&PassthroughFs::fallocate, FsOp::fallocate>;
        o.lseek = tramp<&PassthroughFs::lseek, FsOp::lseek>;
        return o;
    }();
    return ops;
}

void* PassthroughFs::init(fuse_conn_info* /*conn*/, fuse_config* cfg) {
    cfg->use_ino = 1;      // Report the backing inode numbers (stable identities for tools).
    cfg->hard_remove = 1;  // Real unlink of open files: no ".fuse_hidden*" files in the backing tree.
    cfg->nullpath_ok = 1;  // Open handles keep working after their name is removed.
    cfg->entry_timeout = 1.0;
    cfg->attr_timeout = 1.0;
    cfg->negative_timeout = 0.0;  // A file created directly in the backing tree is visible at once.
    deps_.log.log(LogLevel::info, "fs", "filesystem_mounted", "filesystem initialized",
                  {{"backing_directory", deps_.store.path()},
                   {"openat2", deps_.store.uses_openat2()},
                   {"read_only", deps_.read_only}});
    return this;
}

void PassthroughFs::report_failure(FsOp op, std::string_view path, int error) {
    switch (error) {
        case ENOENT:
        case EEXIST:
        case ENOTEMPTY:
        case EACCES:
        case EPERM:
        case ENOTDIR:
        case EISDIR:
        case ENAMETOOLONG:
        case EROFS:
        case EINVAL:
        case EOPNOTSUPP:
        case ENODATA:
        case ENOSYS:
        case EAGAIN:
        case ENOSPC:
        case EDQUOT:
        case EFBIG:
        case EBUSY:
        case ETXTBSY:
        case ESPIPE:
        case EBADF:
        case ERANGE:
        case EMLINK:
        case ENXIO:
            // Ordinary results of client requests.
            if (deps_.log.enabled(LogLevel::trace)) {
                deps_.log.log(LogLevel::trace, "fs", "filesystem_operation_failed", "",
                              {{"op", to_string(op)}, {"path", path}, {"errno", error}});
            }
            return;
        case ELOOP:
        case EXDEV:
            // Symlink in an intermediate component or mount point: refused on purpose.
            deps_.log.log(LogLevel::debug, "fs", "filesystem_operation_failed", "refused path resolution",
                          {{"op", to_string(op)}, {"path", path}, {"errno", error}, {"error", std::strerror(error)}});
            return;
        default:
            if (const auto v = limiter_.check(std::format("fs:{}:{}", to_string(op), error)); v.allowed) {
                deps_.log.log(LogLevel::warning, "fs", "filesystem_operation_failed", "unexpected error",
                              {{"op", to_string(op)},
                               {"path", path},
                               {"errno", error},
                               {"error", std::strerror(error)},
                               {"suppressed", v.suppressed_before}});
            }
    }
}

FsStats PassthroughFs::stats() const {
    FsStats s;
    for (std::size_t i = 0; i < kFsOpCount; ++i) {
        s.calls[i] = counters_.sum(i);
        s.errors[i] = counters_.sum(kFsOpCount + i);
    }
    s.bytes_read = counters_.sum(2 * kFsOpCount);
    s.bytes_written = counters_.sum(2 * kFsOpCount + 1);
    s.open_handles = deps_.open_files.handles();
    return s;
}

void PassthroughFs::changed(const char* path) {
    if (path != nullptr && deps_.on_changed) {
        deps_.on_changed(path);
    }
}

void PassthroughFs::give_to_caller(int dirfd, const char* name, mode_t type) const {
    if (!chown_new_entries_ || fuse_get_context()->uid == 0) {
        return;
    }
    const UniqueFd fd(::openat(dirfd, name, O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        deps_.log.log(LogLevel::debug, "fs", "filesystem_operation_failed", "cannot reopen a new entry",
                      {{"name", name}, {"errno", errno}});
        return;
    }
    give_fd_to_caller(fd.get(), dirfd, type);
}

void PassthroughFs::give_fd_to_caller(int fd, int dirfd, mode_t type) const {
    const fuse_context* ctx = fuse_get_context();
    if (!chown_new_entries_ || ctx->uid == 0) {
        return;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || (st.st_mode & S_IFMT) != type || st.st_uid != ::geteuid() ||
        (S_ISDIR(st.st_mode) && st.st_nlink != 2)) {
        // Not (or no longer) the entry created here: leave its ownership untouched.
        deps_.log.log(LogLevel::warning, "fs", "filesystem_operation_failed",
                      "new entry replaced before its ownership was set: ownership not changed",
                      {{"inode", static_cast<std::uint64_t>(st.st_ino)}, {"uid", ctx->uid}});
        return;
    }
    gid_t gid = ctx->gid;
    struct stat parent{};
    if (::fstat(dirfd, &parent) == 0 && (parent.st_mode & S_ISGID) != 0) {
        gid = parent.st_gid;  // BSD group semantics of set-group-ID directories.
    }
    if (::fchownat(fd, "", ctx->uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) != 0) {
        // Best effort: the entry stays owned by root, which is the safe direction.
        deps_.log.log(LogLevel::debug, "fs", "filesystem_operation_failed", "cannot give a new entry to its creator",
                      {{"inode", static_cast<std::uint64_t>(st.st_ino)}, {"errno", errno}});
    }
}

int PassthroughFs::check_device(const struct stat& st) const noexcept {
    return deps_.store.options().stay_on_filesystem && st.st_dev != deps_.store.root_device() ? -EXDEV : 0;
}

int PassthroughFs::check_name_device(int dirfd, const char* name) const noexcept {
    struct stat st{};
    if (::fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return neg_errno();
    }
    return check_device(st);
}

int PassthroughFs::getattr(const char* path, struct stat* st, fuse_file_info* fi) {
    if (fi != nullptr) {
        return ::fstat(file_of(fi)->fd.get(), st) == 0 ? 0 : neg_errno();
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const int flags = parent->name == "." ? AT_EMPTY_PATH : AT_SYMLINK_NOFOLLOW;
    const char* name = parent->name == "." ? "" : parent->name.c_str();
    return ::fstatat(parent->directory.get(), name, st, flags) == 0 ? check_device(*st) : neg_errno();
}

int PassthroughFs::readlink(const char* path, char* buf, size_t size) {
    if (size == 0) {
        return -EINVAL;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const ssize_t n = ::readlinkat(parent->directory.get(), parent->name.c_str(), buf, size - 1);
    if (n < 0) {
        return neg_errno();
    }
    buf[n] = '\0';  // NOLINT(clang-analyzer-security.ArrayBound): n <= size - 1 (readlinkat bound)
    return 0;
}

int PassthroughFs::mknod(const char* path, mode_t mode, dev_t rdev) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (S_ISCHR(mode) || S_ISBLK(mode)) {
        return -EPERM;  // Device nodes would bypass the permission model of the mount.
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    int rc = 0;
    if (S_ISREG(mode)) {
        const int fd = ::openat(parent->directory.get(), parent->name.c_str(),
                                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
        rc = fd < 0 ? -1 : ::close(fd);
    } else {
        rc = ::mknodat(parent->directory.get(), parent->name.c_str(), mode, rdev);
    }
    if (rc != 0) {
        return neg_errno();
    }
    give_to_caller(parent->directory.get(), parent->name.c_str(), mode & S_IFMT);
    changed(path);
    return 0;
}

int PassthroughFs::mkdir(const char* path, mode_t mode) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    if (::mkdirat(parent->directory.get(), parent->name.c_str(), mode) != 0) {
        return neg_errno();
    }
    give_to_caller(parent->directory.get(), parent->name.c_str(), S_IFDIR);
    return 0;
}

int PassthroughFs::unlink(const char* path) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    return ::unlinkat(parent->directory.get(), parent->name.c_str(), 0) == 0 ? 0 : neg_errno();
}

int PassthroughFs::rmdir(const char* path) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    return ::unlinkat(parent->directory.get(), parent->name.c_str(), AT_REMOVEDIR) == 0 ? 0 : neg_errno();
}

int PassthroughFs::symlink(const char* target, const char* path) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    if (::symlinkat(target, parent->directory.get(), parent->name.c_str()) != 0) {
        return neg_errno();
    }
    give_to_caller(parent->directory.get(), parent->name.c_str(), S_IFLNK);
    return 0;
}

int PassthroughFs::rename(const char* from, const char* to, unsigned int flags) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto src = deps_.store.open_parent(from);
    if (!src) {
        return neg(src.error().code);
    }
    auto dst = deps_.store.open_parent(to);
    if (!dst) {
        return neg(dst.error().code);
    }
    const auto guard = deps_.locks.lock(from, to);
    if (::renameat2(src->directory.get(), src->name.c_str(), dst->directory.get(), dst->name.c_str(), flags) != 0) {
        return neg_errno();
    }
    changed(to);
    if ((flags & RENAME_EXCHANGE) != 0) {
        changed(from);
    }
    return 0;
}

int PassthroughFs::link(const char* from, const char* to) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto src = deps_.store.open_parent(from);
    if (!src) {
        return neg(src.error().code);
    }
    auto dst = deps_.store.open_parent(to);
    if (!dst) {
        return neg(dst.error().code);
    }
    const auto guard = deps_.locks.lock(from, to);
    if (::linkat(src->directory.get(), src->name.c_str(), dst->directory.get(), dst->name.c_str(), 0) != 0) {
        return neg_errno();
    }
    changed(to);
    return 0;
}

int PassthroughFs::chmod(const char* path, mode_t mode, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (fi != nullptr) {
        return ::fchmod(file_of(fi)->fd.get(), mode) == 0 ? 0 : neg_errno();
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    if (const int rc = check_name_device(parent->directory.get(), parent->name.c_str()); rc != 0) {
        return rc;
    }
    // AT_SYMLINK_NOFOLLOW: never change the mode of a link target (EOPNOTSUPP on links).
    return ::fchmodat(parent->directory.get(), parent->name.c_str(), mode, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : neg_errno();
}

int PassthroughFs::chown(const char* path, uid_t uid, gid_t gid, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (fi != nullptr) {
        return ::fchown(file_of(fi)->fd.get(), uid, gid) == 0 ? 0 : neg_errno();
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    if (const int rc = check_name_device(parent->directory.get(), parent->name.c_str()); rc != 0) {
        return rc;
    }
    return ::fchownat(parent->directory.get(), parent->name.c_str(), uid, gid, AT_SYMLINK_NOFOLLOW) == 0 ? 0
                                                                                                         : neg_errno();
}

int PassthroughFs::truncate(const char* path, off_t size, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (fi != nullptr) {
        FileHandle* handle = file_of(fi);
        if (::ftruncate(handle->fd.get(), size) != 0) {
            return neg_errno();
        }
        handle->modified = true;
        return 0;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    const UniqueFd fd(::openat(parent->directory.get(), parent->name.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!fd) {
        return neg_errno();
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        return neg_errno();
    }
    if (const int rc = check_device(st); rc != 0) {
        return rc;
    }
    if (::ftruncate(fd.get(), size) != 0) {
        return neg_errno();
    }
    changed(path);
    return 0;
}

int PassthroughFs::open(const char* path, fuse_file_info* fi) {
    if (deps_.read_only && (fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    // The path lock orders this open against a GC deletion of the same name: either
    // the GC sees the registered handle, or the open sees ENOENT.
    const auto guard = deps_.locks.lock(path);
    auto handle = std::make_unique<FileHandle>();
    handle->fd.reset(
        ::openat(parent->directory.get(), parent->name.c_str(), (fi->flags & ~O_CREAT) | O_NOFOLLOW | O_CLOEXEC));
    if (!handle->fd) {
        return neg_errno();
    }
    struct stat st{};
    if (::fstat(handle->fd.get(), &st) != 0) {
        return neg_errno();
    }
    if (const int rc = check_device(st); rc != 0) {
        return rc;
    }
    handle->identity = {st.st_dev, st.st_ino};
    handle->modified = (fi->flags & O_TRUNC) != 0;
    deps_.open_files.add(handle->identity);
    fi->fh = adopt(std::move(handle));
    return 0;
}

int PassthroughFs::create(const char* path, mode_t mode, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    auto handle = std::make_unique<FileHandle>();
    // O_EXCL first, to know whether this call created the file: only a file created
    // here is given to the caller. If it appeared meanwhile (written directly into the
    // backing directory) and the caller did not ask for O_EXCL, it is opened as is.
    handle->fd.reset(::openat(parent->directory.get(), parent->name.c_str(),
                              fi->flags | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    const bool created = static_cast<bool>(handle->fd);
    if (!created && errno == EEXIST && (fi->flags & O_EXCL) == 0) {
        handle->fd.reset(
            ::openat(parent->directory.get(), parent->name.c_str(), (fi->flags & ~O_CREAT) | O_NOFOLLOW | O_CLOEXEC));
    }
    if (!handle->fd) {
        return neg_errno();
    }
    if (created) {
        give_fd_to_caller(handle->fd.get(), parent->directory.get(), S_IFREG);
    }
    struct stat st{};
    if (::fstat(handle->fd.get(), &st) != 0) {
        return neg_errno();
    }
    if (const int rc = check_device(st); rc != 0) {
        return rc;
    }
    handle->identity = {st.st_dev, st.st_ino};
    handle->modified = true;
    deps_.open_files.add(handle->identity);
    fi->fh = adopt(std::move(handle));
    changed(path);
    return 0;
}

int PassthroughFs::read_buf(const char* /*path*/, fuse_bufvec** bufp, size_t size, off_t offset, fuse_file_info* fi) {
    // libfuse releases the vector with free(): it must come from malloc.
    auto* vec = static_cast<fuse_bufvec*>(std::malloc(sizeof(fuse_bufvec)));
    if (vec == nullptr) {
        return -ENOMEM;
    }
    *vec = fuse_bufvec{};
    vec->count = 1;
    vec->buf[0].size = size;
    vec->buf[0].flags = kFdBufferFlags;
    vec->buf[0].fd = file_of(fi)->fd.get();
    vec->buf[0].pos = offset;
    *bufp = vec;  // The data is read (or spliced) by libfuse straight from the backing file.
    counters_.add(2 * kFsOpCount, size);
    return 0;
}

int PassthroughFs::write_buf(const char* /*path*/, fuse_bufvec* buf, off_t offset, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    FileHandle* handle = file_of(fi);
    fuse_bufvec dst{};
    dst.count = 1;
    dst.buf[0].size = fuse_buf_size(buf);
    dst.buf[0].flags = kFdBufferFlags;
    dst.buf[0].fd = handle->fd.get();
    dst.buf[0].pos = offset;
    const ssize_t written = fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
    if (written < 0) {
        return static_cast<int>(written);
    }
    handle->modified = true;
    counters_.add(2 * kFsOpCount + 1, static_cast<std::uint64_t>(written));
    return static_cast<int>(written);
}

int PassthroughFs::statfs(const char* /*path*/, struct statvfs* st) {
    return ::fstatvfs(deps_.store.root_fd(), st) == 0 ? 0 : neg_errno();
}

int PassthroughFs::flush(const char* /*path*/, fuse_file_info* fi) {
    // close() of a duplicate reports deferred write errors (NFS, quota) to the caller
    // of close(2) without closing the handle itself.
    const int dup_fd = ::dup(file_of(fi)->fd.get());
    if (dup_fd < 0) {
        return neg_errno();
    }
    return ::close(dup_fd) == 0 ? 0 : neg_errno();
}

int PassthroughFs::release(const char* path, fuse_file_info* fi) {
    const std::unique_ptr<FileHandle> handle = take_file(fi);
    deps_.open_files.remove(handle->identity);
    const bool modified = handle->modified;
    handle->fd.reset();  // Close the backing descriptor before notifying.
    if (modified) {
        changed(path);  // New mtime: the expiry of the file moved.
    }
    return 0;
}

int PassthroughFs::fsync(const char* /*path*/, int datasync, fuse_file_info* fi) {
    const int fd = file_of(fi)->fd.get();
    return (datasync != 0 ? ::fdatasync(fd) : ::fsync(fd)) == 0 ? 0 : neg_errno();
}

int PassthroughFs::opendir(const char* path, fuse_file_info* fi) {
    auto fd = deps_.store.open_directory(path);
    if (!fd) {
        return neg(fd.error().code);
    }
    auto handle = std::make_unique<DirHandle>();
    handle->dir = ::fdopendir(fd->get());
    if (handle->dir == nullptr) {
        return neg_errno();
    }
    fd->release();
    fi->fh = adopt(std::move(handle));
    return 0;
}

int PassthroughFs::readdir(const char* /*path*/, void* buf, fuse_fill_dir_t filler, off_t offset, fuse_file_info* fi,
                           fuse_readdir_flags /*flags*/) {
    auto* handle = reinterpret_cast<DirHandle*>(fi->fh);  // NOLINT(performance-no-int-to-ptr)
    std::scoped_lock lock(handle->mutex);
    if (offset != handle->offset) {
        ::seekdir(handle->dir, offset);
        handle->entry = nullptr;
        handle->offset = offset;
    }
    for (;;) {
        if (handle->entry == nullptr) {
            errno = 0;
            handle->entry = ::readdir(handle->dir);
            if (handle->entry == nullptr) {
                return errno != 0 ? neg_errno() : 0;
            }
        }
        struct stat st{};
        st.st_ino = handle->entry->d_ino;
        st.st_mode = static_cast<mode_t>(DTTOIF(handle->entry->d_type));
        const off_t next = ::telldir(handle->dir);
        if (filler(buf, handle->entry->d_name, &st, next, static_cast<fuse_fill_dir_flags>(0)) != 0) {
            return 0;  // Buffer full: this entry is returned again at "next - 1".
        }
        handle->entry = nullptr;
        handle->offset = next;
    }
}

int PassthroughFs::releasedir(const char* /*path*/, fuse_file_info* fi) {
    const auto handle = take_dir(fi);  // Destroyed on return: closes the directory stream.
    return 0;
}

int PassthroughFs::fsyncdir(const char* /*path*/, int datasync, fuse_file_info* fi) {
    auto* handle = reinterpret_cast<DirHandle*>(fi->fh);  // NOLINT(performance-no-int-to-ptr)
    const int fd = ::dirfd(handle->dir);
    if (fd < 0) {
        return neg_errno();
    }
    return (datasync != 0 ? ::fdatasync(fd) : ::fsync(fd)) == 0 ? 0 : neg_errno();
}

int PassthroughFs::access(const char* path, int mask) {
    // With default_permissions the kernel checks the mode bits itself; this only
    // answers existence and the read-only mount.
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    struct stat st{};
    const int flags = parent->name == "." ? AT_EMPTY_PATH : AT_SYMLINK_NOFOLLOW;
    if (::fstatat(parent->directory.get(), parent->name == "." ? "" : parent->name.c_str(), &st, flags) != 0) {
        return neg_errno();
    }
    if (const int rc = check_device(st); rc != 0) {
        return rc;
    }
    if ((mask & W_OK) != 0 && deps_.read_only) {
        return -EROFS;
    }
    return 0;
}

int PassthroughFs::utimens(const char* path, const struct timespec tv[2], fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (fi != nullptr) {
        if (::futimens(file_of(fi)->fd.get(), tv) != 0) {
            return neg_errno();
        }
        file_of(fi)->modified = true;
        return 0;
    }
    auto parent = deps_.store.open_parent(path);
    if (!parent) {
        return neg(parent.error().code);
    }
    const auto guard = deps_.locks.lock(path);
    if (const int rc = check_name_device(parent->directory.get(), parent->name.c_str()); rc != 0) {
        return rc;
    }
    if (::utimensat(parent->directory.get(), parent->name.c_str(), tv, AT_SYMLINK_NOFOLLOW) != 0) {
        return neg_errno();
    }
    changed(path);  // touch -d moves the expiry.
    return 0;
}

int PassthroughFs::fallocate(const char* /*path*/, int mode, off_t offset, off_t length, fuse_file_info* fi) {
    if (deps_.read_only) {
        return -EROFS;
    }
    if (fi == nullptr) {
        return -EOPNOTSUPP;
    }
    if (::fallocate(file_of(fi)->fd.get(), mode, offset, length) != 0) {
        return neg_errno();
    }
    file_of(fi)->modified = true;
    return 0;
}

off_t PassthroughFs::lseek(const char* /*path*/, off_t offset, int whence, fuse_file_info* fi) {
    if (fi == nullptr) {
        return -EBADF;
    }
    const off_t result = ::lseek(file_of(fi)->fd.get(), offset, whence);
    return result < 0 ? -errno : result;
}

}  // namespace tsfs
