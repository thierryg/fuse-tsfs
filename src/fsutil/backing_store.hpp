// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/fsutil/backing_store.hpp
// Purpose : Race-resistant, descriptor-relative access to the backing directory
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <memory>
#include <string>
#include <string_view>

#include "common/error.hpp"
#include "common/unique_fd.hpp"

namespace tsfs {

/// Identity of a file: device + inode. Stable across renames, changes on replacement.
struct FileIdentity {
    dev_t device{0};
    ino_t inode{0};
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

/// Parent directory descriptor + final component of a managed path.
struct ParentHandle {
    UniqueFd directory;  ///< O_PATH descriptor of the parent directory.
    std::string name;    ///< Last component ("." for the root itself).
};

/**
 * @brief The backing directory and the only way TSFS touches it.
 *
 * Every access is relative to a descriptor of the backing root opened once at
 * startup. Intermediate path components are resolved by the kernel with
 * openat2(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS
 * [| RESOLVE_NO_XDEV]); on kernels without openat2 (< 5.6) an equivalent
 * component-by-component walk with O_NOFOLLOW is used. As a consequence:
 * - no path can escape the backing root ("..", absolute symlinks, magic links);
 * - a symbolic link is never followed as an intermediate component, even if an
 *   external process swaps a directory for a link (the operation fails with ELOOP
 *   or ENOTDIR instead);
 * - with stay_on_filesystem, mount points below the root are not crossed (EXDEV)
 *   by intermediate components: openat2 refuses every mount crossing
 *   (RESOLVE_NO_XDEV, bind mounts included); the fallback walk compares st_dev, so
 *   it detects mounts of another filesystem but NOT a bind mount of the same
 *   filesystem (set security.require_openat2 on kernels where this matters). The
 *   final component is checked by the callers (PassthroughFs::check_device, the
 *   GC device check).
 * The final component is always handled by the caller with *at() calls and
 * AT_SYMLINK_NOFOLLOW / O_NOFOLLOW.
 *
 * Thread-safe: the root descriptor is immutable after open().
 */
class BackingStore {
public:
    /// Options fixed at startup.
    struct Options {
        bool stay_on_filesystem{true};
        bool require_openat2{false};
        bool force_fallback{false};  ///< Tests only: ignore openat2 even if available.
    };

    /**
     * @brief Open @p backing_directory (must exist and be a directory).
     * @return the store, or ENOENT/ENOTDIR/EACCES, or ENOSYS when openat2 is
     *         required but missing.
     */
    static Result<std::unique_ptr<BackingStore>> open(const std::string& backing_directory, Options options);

    BackingStore(const BackingStore&) = delete("the store owns the root descriptor");
    BackingStore& operator=(const BackingStore&) = delete("the store owns the root descriptor");

    /// @return the root descriptor (O_PATH | O_DIRECTORY), owned by the store.
    [[nodiscard]] int root_fd() const noexcept { return root_.get(); }
    /// @return st_dev of the root.
    [[nodiscard]] dev_t root_device() const noexcept { return root_device_; }
    /// @return the host path given to open().
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// @return true when openat2 is used, false for the fallback walk.
    [[nodiscard]] bool uses_openat2() const noexcept { return use_openat2_; }
    /// @return the options.
    [[nodiscard]] const Options& options() const noexcept { return options_; }

    /**
     * @brief Open the parent directory of a managed path.
     * @param path Normalized managed path. For "/" the root itself is returned with name ".".
     * @return the parent descriptor (O_PATH) and the final name, or a negative-free errno
     *         (ENOENT, ENOTDIR, ELOOP, EXDEV, EACCES...).
     */
    [[nodiscard]] Result<ParentHandle> open_parent(std::string_view path) const;

    /**
     * @brief Open a directory of the managed tree for reading (O_RDONLY | O_DIRECTORY).
     * Never follows a symbolic link, including the final component.
     */
    [[nodiscard]] Result<UniqueFd> open_directory(std::string_view path) const;

private:
    BackingStore() = default;
    [[nodiscard]] Result<UniqueFd> resolve(std::string_view relative, int flags) const;
    [[nodiscard]] Result<UniqueFd> resolve_fallback(std::string_view relative, int flags) const;

    UniqueFd root_;
    dev_t root_device_{0};
    std::string path_;
    Options options_;
    bool use_openat2_{false};
};

/// @return true when the running kernel implements openat2(2).
bool kernel_supports_openat2() noexcept;

}  // namespace tsfs
