// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_watcher.hpp
// Purpose : Reload triggers: inotify on the configuration directory, SIGHUP and SIGUSR1 bridges
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "common/error.hpp"
#include "common/unique_fd.hpp"

namespace tsfs {

/**
 * @brief Thread that turns external events into reload/status callbacks.
 *
 * - inotify watches the *directory* of the configuration file, not the file: an
 *   editor or a deployment tool replacing the file atomically (rename over it, or
 *   a "..data" symlink swap) changes the inode, which would silently end a watch
 *   on the file itself. Events are debounced (250 ms of quiet) and the file content
 *   is compared with the last loaded content, so unrelated files in the directory
 *   and no-op saves do not cause reloads. If the directory itself disappears, the
 *   watch is re-established every 2 s.
 * - reload_fd()/status_fd()/purge_fd() are eventfds written by the SIGHUP/SIGUSR1/SIGUSR2 handlers
 *   (write(2) is async-signal-safe); SIGHUP always reloads, even if unchanged.
 * Callbacks run on the watcher thread, one at a time.
 */
class ConfigWatcher {
public:
    struct Callbacks {
        std::function<void(std::string_view trigger)> reload;
        std::function<void()> dump_status;
        std::function<void()> purge;  ///< SIGUSR2 (tsfs --purge); may be empty.
    };

    /// Create the descriptors (inotify may be unavailable: then only signals work).
    static Result<std::unique_ptr<ConfigWatcher>> create(std::string config_path, Callbacks callbacks);
    ~ConfigWatcher();
    ConfigWatcher(const ConfigWatcher&) = delete("owns a thread and descriptors");
    ConfigWatcher& operator=(const ConfigWatcher&) = delete("owns a thread and descriptors");

    void start();
    void stop();
    /// Record the content just loaded (so that an identical file does not reload again).
    void remember_current_content();

    [[nodiscard]] int reload_fd() const noexcept { return reload_fd_.get(); }
    [[nodiscard]] int status_fd() const noexcept { return status_fd_.get(); }
    [[nodiscard]] int purge_fd() const noexcept { return purge_fd_.get(); }
    [[nodiscard]] bool inotify_active() const noexcept { return watch_.load(std::memory_order_relaxed) >= 0; }

private:
    ConfigWatcher() = default;
    void run(std::stop_token stop);
    bool add_watch();
    std::uint64_t content_hash() const;

    std::string path_;
    std::string directory_;
    Callbacks callbacks_;
    UniqueFd inotify_;
    UniqueFd reload_fd_;
    UniqueFd status_fd_;
    UniqueFd purge_fd_;
    UniqueFd stop_fd_;
    std::atomic<int> watch_{-1};  ///< Written by the watcher thread, read by inotify_active().
    std::uint64_t last_hash_{0};
    std::jthread thread_;
};

/**
 * @brief Install SIGHUP -> write(@p reload_fd), SIGUSR1 -> write(@p status_fd) and
 *        SIGUSR2 -> write(@p purge_fd) (default disposition when -1).
 *
 * Process-wide (signal dispositions are global). The handlers only perform
 * write(2) on an eventfd. Pass -1 to restore the default dispositions.
 */
void install_control_signal_handlers(int reload_fd, int status_fd, int purge_fd = -1) noexcept;

}  // namespace tsfs
