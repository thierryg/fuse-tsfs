// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/filesystem/fuse_session.cpp
// Purpose : RAII mount lifecycle around libfuse (new, mount, loop, unmount, destroy)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "filesystem/fuse_session.hpp"

#include <fuse_lowlevel.h>

#include <cerrno>
#include <string>
#include <vector>

namespace tsfs {

Result<std::unique_ptr<FuseSession>> FuseSession::mount(const Options& options, PassthroughFs& fs) {
    auto session = std::unique_ptr<FuseSession>(new FuseSession());
    session->mountpoint_ = options.mountpoint;

    std::string mount_options = "default_permissions,fsname=tsfs,subtype=tsfs";
    if (options.allow_other) {
        mount_options += ",allow_other";
    }
    if (options.read_only) {
        mount_options += ",ro";
    }
    std::vector<std::string> argv_storage{"tsfs", "-o", mount_options};
    if (options.fuse_debug) {
        argv_storage.emplace_back("-d");
    }
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage) {
        argv.push_back(arg.data());
    }
    fuse_args args = FUSE_ARGS_INIT(static_cast<int>(argv.size()), argv.data());
    session->fuse_ = fuse_new(&args, &PassthroughFs::operations(), sizeof(fuse_operations), &fs);
    fuse_opt_free_args(&args);
    if (session->fuse_ == nullptr) {
        return make_error(EINVAL, "fuse_new failed (invalid mount options, see the message above)");
    }
    if (fuse_mount(session->fuse_, options.mountpoint.c_str()) != 0) {
        return make_error(EIO,
                          "cannot mount on '" + options.mountpoint +
                              "' (does it exist? is /dev/fuse accessible? is user_allow_other set for allow_other?)");
    }
    session->mounted_ = true;
    return session;
}

FuseSession::~FuseSession() {
    if (fuse_ == nullptr) {
        return;
    }
    if (handlers_) {
        fuse_remove_signal_handlers(fuse_get_session(fuse_));
    }
    if (mounted_) {
        fuse_unmount(fuse_);
    }
    fuse_destroy(fuse_);
}

Result<void> FuseSession::install_signal_handlers() {
    if (fuse_set_signal_handlers(fuse_get_session(fuse_)) != 0) {
        return make_error(EIO, "cannot install the FUSE signal handlers");
    }
    handlers_ = true;
    return {};
}

int FuseSession::loop(unsigned idle_threads) {
    fuse_loop_config* config = fuse_loop_cfg_create();
    if (config == nullptr) {
        return -ENOMEM;
    }
    fuse_loop_cfg_set_idle_threads(config, idle_threads);
    fuse_loop_cfg_set_clone_fd(config, 0);
    const int rc = fuse_loop_mt(fuse_, config);
    fuse_loop_cfg_destroy(config);
    return rc;
}

void FuseSession::request_exit() noexcept {
    if (fuse_ != nullptr) {
        fuse_exit(fuse_);
    }
}

void FuseSession::invalidate(std::string_view path) noexcept {
    if (fuse_ == nullptr) {
        return;
    }
    const std::string copy(path);
    // ENOENT means the kernel had nothing cached: fine.
    (void)fuse_invalidate_path(fuse_, copy.c_str());
}

}  // namespace tsfs
