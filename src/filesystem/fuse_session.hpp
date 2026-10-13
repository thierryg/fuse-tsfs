// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/fuse_session.hpp
// Purpose : RAII mount lifecycle around libfuse (new, mount, loop, unmount, destroy)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <fuse.h>

#include <memory>
#include <string>
#include <string_view>

#include "common/error.hpp"
#include "filesystem/passthrough_fs.hpp"

namespace tsfs {

/**
 * @brief Owns one mounted libfuse filesystem.
 *
 * mount() creates the fuse object with the TSFS options (always
 * "default_permissions"; "allow_other" and "ro" from the configuration; fsname
 * "tsfs", subtype "tsfs") and mounts it. The destructor removes the signal handlers,
 * unmounts and destroys the fuse object, in that order, so an exception or an early
 * return never leaves a stale mount behind.
 *
 * Threading: loop() runs libfuse's multi-threaded loop in the calling thread (the
 * main thread, which must keep SIGINT/SIGTERM unblocked: libfuse's handlers end
 * the loop). invalidate() may be called from any thread while the loop runs.
 */
class FuseSession {
public:
    /// Mount options.
    struct Options {
        std::string mountpoint;
        bool allow_other{false};
        bool read_only{false};
        bool fuse_debug{false};       ///< libfuse "-d": prints every request on stderr.
        bool retention_xattrs{true};  ///< filesystem.retention_xattrs.
    };

    /// Create and mount. @return the session, or EIO/EINVAL with libfuse's reason on stderr.
    static Result<std::unique_ptr<FuseSession>> mount(const Options& options, PassthroughFs& fs);

    ~FuseSession();
    FuseSession(const FuseSession&) = delete("owns the mount");
    FuseSession& operator=(const FuseSession&) = delete("owns the mount");

    /// Install libfuse's SIGINT/SIGTERM/SIGHUP/SIGPIPE handlers (SIGHUP is taken over by the daemon afterwards).
    Result<void> install_signal_handlers();
    /// Run the request loop until unmount or a termination signal. @return 0 on a clean exit.
    int loop(unsigned idle_threads = 10);
    /// Ask the loop to return (async-signal-safe in libfuse).
    void request_exit() noexcept;
    /// Drop the kernel's cached entry/attributes of @p path (after a GC deletion).
    void invalidate(std::string_view path) noexcept;
    /// @return the mount point.
    [[nodiscard]] const std::string& mountpoint() const noexcept { return mountpoint_; }

private:
    FuseSession() = default;
    struct fuse* fuse_{nullptr};
    bool mounted_{false};
    bool handlers_{false};
    std::string mountpoint_;
};

}  // namespace tsfs
