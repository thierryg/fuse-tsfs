// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/backing_store.cpp
// Purpose : Race-resistant, descriptor-relative access to the backing directory
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "fsutil/backing_store.hpp"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/managed_path.hpp"

namespace tsfs {
namespace {

long sys_openat2(int dirfd, const char* path, open_how* how) {
    return ::syscall(SYS_openat2, dirfd, path, how, sizeof(*how));
}

Error errno_error(int err, std::string_view what, std::string_view path) {
    return Error{err, std::string(what) + " '" + std::string(path) + "': " + std::strerror(err)};
}

}  // namespace

bool kernel_supports_openat2() noexcept {
    open_how how{};
    how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
    const long fd = sys_openat2(AT_FDCWD, "/", &how);
    if (fd >= 0) {
        ::close(static_cast<int>(fd));
        return true;
    }
    return errno != ENOSYS;
}

Result<std::unique_ptr<BackingStore>> BackingStore::open(const std::string& backing_directory, Options options) {
    auto store = std::unique_ptr<BackingStore>(new BackingStore());
    store->options_ = options;
    store->path_ = backing_directory;
    // The backing directory itself comes from the trusted configuration: it may be a symlink.
    store->root_.reset(::open(backing_directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!store->root_) {
        return propagate(errno_error(errno, "cannot open the backing directory", backing_directory));
    }
    struct stat st{};
    if (::fstat(store->root_.get(), &st) != 0) {
        return propagate(errno_error(errno, "cannot stat the backing directory", backing_directory));
    }
    store->root_device_ = st.st_dev;
    store->use_openat2_ = !options.force_fallback && kernel_supports_openat2();
    if (options.require_openat2 && !store->use_openat2_) {
        return make_error(ENOSYS, "openat2(2) is required (security.require_openat2) but not available");
    }
    return store;
}

Result<UniqueFd> BackingStore::resolve(std::string_view relative, int flags) const {
    if (relative == ".") {
        // Re-open the root with the requested flags.
        const int fd = ::openat(root_.get(), ".", flags | O_CLOEXEC);
        if (fd < 0) {
            return propagate(errno_error(errno, "cannot open", "/"));
        }
        return UniqueFd(fd);
    }
    if (!use_openat2_) {
        return resolve_fallback(relative, flags);
    }
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC | O_NOFOLLOW);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    if (options_.stay_on_filesystem) {
        how.resolve |= RESOLVE_NO_XDEV;
    }
    const std::string path(relative);
    for (;;) {
        const long fd = sys_openat2(root_.get(), path.c_str(), &how);
        if (fd >= 0) {
            return UniqueFd(static_cast<int>(fd));
        }
        // EAGAIN: a concurrent rename raced with the lookup; retrying is the documented answer.
        if (errno != EAGAIN && errno != EINTR) {
            return propagate(errno_error(errno, "cannot resolve", relative));
        }
    }
}

Result<UniqueFd> BackingStore::resolve_fallback(std::string_view relative, int flags) const {
    // Walk one component at a time: O_NOFOLLOW on every step refuses symlinks, the
    // device check refuses mount points, and ".." cannot appear in a managed path.
    UniqueFd current(::openat(root_.get(), ".", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!current) {
        return propagate(errno_error(errno, "cannot open", "/"));
    }
    std::size_t pos = 0;
    while (pos < relative.size()) {
        const std::size_t end = std::min(relative.find('/', pos), relative.size());
        const std::string component(relative.substr(pos, end - pos));
        const bool last = end == relative.size();
        const int step_flags =
            last ? (flags | O_NOFOLLOW | O_CLOEXEC) : (O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        UniqueFd next(::openat(current.get(), component.c_str(), step_flags));
        if (!next) {
            return propagate(errno_error(errno, "cannot resolve", relative));
        }
        if (options_.stay_on_filesystem) {
            struct stat st{};
            if (::fstat(next.get(), &st) != 0) {
                return propagate(errno_error(errno, "cannot stat", relative));
            }
            if (st.st_dev != root_device_) {
                return propagate(errno_error(EXDEV, "mount point crossed in", relative));
            }
        }
        current = std::move(next);
        pos = end + 1;
    }
    return current;
}

Result<ParentHandle> BackingStore::open_parent(std::string_view path) const {
    if (!managed_path::is_normalized(path)) {
        return make_error(EINVAL, "invalid managed path '" + std::string(path) + "'");
    }
    ParentHandle handle;
    if (path == "/") {
        auto root = resolve(".", O_PATH | O_DIRECTORY);
        if (!root) {
            return propagate(root.error());
        }
        handle.directory = std::move(*root);
        handle.name = ".";
        return handle;
    }
    auto parent = resolve(managed_path::relative(managed_path::parent(path)), O_PATH | O_DIRECTORY);
    if (!parent) {
        return propagate(parent.error());
    }
    handle.directory = std::move(*parent);
    handle.name = std::string(managed_path::basename(path));
    return handle;
}

Result<UniqueFd> BackingStore::open_directory(std::string_view path) const {
    if (!managed_path::is_normalized(path)) {
        return make_error(EINVAL, "invalid managed path '" + std::string(path) + "'");
    }
    return resolve(managed_path::relative(path), O_RDONLY | O_DIRECTORY);
}

}  // namespace tsfs
