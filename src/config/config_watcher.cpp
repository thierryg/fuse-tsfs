// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_watcher.cpp
// Purpose : Reload triggers: inotify on the configuration directory, SIGHUP and SIGUSR1 bridges
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "config/config_watcher.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "common/managed_path.hpp"
#include "common/threads.hpp"

namespace tsfs {
namespace {

std::atomic<int> g_reload_fd{-1};
std::atomic<int> g_status_fd{-1};

void notify_fd(int fd) noexcept {
    if (fd >= 0) {
        const std::uint64_t one = 1;
        // eventfd writes are atomic; a full counter (impossible in practice) is ignored.
        [[maybe_unused]] const ssize_t n = ::write(fd, &one, sizeof(one));
    }
}

void on_sighup(int) {
    const int saved = errno;
    notify_fd(g_reload_fd.load(std::memory_order_relaxed));
    errno = saved;
}

void on_sigusr1(int) {
    const int saved = errno;
    notify_fd(g_status_fd.load(std::memory_order_relaxed));
    errno = saved;
}

void drain_eventfd(int fd) noexcept {
    std::uint64_t value = 0;
    [[maybe_unused]] const ssize_t n = ::read(fd, &value, sizeof(value));
}

constexpr auto kDebounce = std::chrono::milliseconds(250);
constexpr auto kRewatchInterval = std::chrono::seconds(2);
constexpr std::uint32_t kWatchMask =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;

}  // namespace

void install_control_signal_handlers(int reload_fd, int status_fd) noexcept {
    g_reload_fd.store(reload_fd);
    g_status_fd.store(status_fd);
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    action.sa_handler = reload_fd >= 0 ? on_sighup : SIG_DFL;
    ::sigaction(SIGHUP, &action, nullptr);
    action.sa_handler = status_fd >= 0 ? on_sigusr1 : SIG_DFL;
    ::sigaction(SIGUSR1, &action, nullptr);
}

Result<std::unique_ptr<ConfigWatcher>> ConfigWatcher::create(std::string config_path, Callbacks callbacks) {
    auto watcher = std::unique_ptr<ConfigWatcher>(new ConfigWatcher());
    watcher->path_ = std::move(config_path);
    const std::size_t slash = watcher->path_.rfind('/');
    watcher->directory_ = slash == std::string::npos ? "." : (slash == 0 ? "/" : watcher->path_.substr(0, slash));
    watcher->callbacks_ = std::move(callbacks);
    watcher->reload_fd_.reset(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    watcher->status_fd_.reset(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    watcher->stop_fd_.reset(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!watcher->reload_fd_ || !watcher->status_fd_ || !watcher->stop_fd_) {
        const int err = errno;
        return make_error(err, std::string("eventfd: ") + std::strerror(err));
    }
    watcher->inotify_.reset(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
    watcher->add_watch();  // Failure is not fatal: SIGHUP still works.
    watcher->remember_current_content();
    return watcher;
}

ConfigWatcher::~ConfigWatcher() { stop(); }

bool ConfigWatcher::add_watch() {
    if (!inotify_) {
        return false;
    }
    const int wd = ::inotify_add_watch(inotify_.get(), directory_.c_str(), kWatchMask);
    watch_.store(wd, std::memory_order_relaxed);
    return wd >= 0;
}

std::uint64_t ConfigWatcher::content_hash() const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return 0;
    }
    std::ostringstream content;
    content << in.rdbuf();
    return std::hash<std::string>{}(content.str()) | 1U;  // Never 0 (reserved for "unreadable").
}

void ConfigWatcher::remember_current_content() { last_hash_ = content_hash(); }

void ConfigWatcher::start() {
    if (!thread_.joinable()) {
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }
}

void ConfigWatcher::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        notify_fd(stop_fd_.get());
        thread_.join();
    }
}

void ConfigWatcher::run(std::stop_token stop) {
    prepare_helper_thread("tsfs-config");
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> debounce_until;
    Clock::time_point next_rewatch = Clock::now() + kRewatchInterval;
    std::array<char, 16 * 1024> buffer{};
    while (!stop.stop_requested()) {
        std::array<pollfd, 4> fds{{{stop_fd_.get(), POLLIN, 0},
                                   {reload_fd_.get(), POLLIN, 0},
                                   {status_fd_.get(), POLLIN, 0},
                                   {inotify_.get(), POLLIN, 0}}};
        const nfds_t count = inotify_ ? 4 : 3;
        auto now = Clock::now();
        Clock::time_point wake =
            watch_.load(std::memory_order_relaxed) < 0 ? next_rewatch : now + std::chrono::hours(1);
        if (debounce_until) {
            wake = std::min(wake, *debounce_until);
        }
        const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(wake - now).count();
        const int ready = ::poll(fds.data(), count, static_cast<int>(std::max<long long>(0, timeout)));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            drain_eventfd(reload_fd_.get());
            callbacks_.reload("sighup");
            remember_current_content();
            debounce_until.reset();
        }
        if ((fds[2].revents & POLLIN) != 0) {
            drain_eventfd(status_fd_.get());
            if (callbacks_.dump_status) {
                callbacks_.dump_status();
            }
        }
        if (count == 4 && (fds[3].revents & POLLIN) != 0) {
            for (;;) {
                const ssize_t n = ::read(inotify_.get(), buffer.data(), buffer.size());
                if (n <= 0) {
                    break;
                }
                for (ssize_t off = 0; off < n;) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + off);
                    if ((event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                        watch_.store(-1, std::memory_order_relaxed);  // The directory went away: re-watch later.
                    }
                    off += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
                }
            }
            debounce_until = Clock::now() + kDebounce;
        }
        now = Clock::now();
        if (watch_.load(std::memory_order_relaxed) < 0 && now >= next_rewatch) {
            next_rewatch = now + kRewatchInterval;
            if (add_watch()) {
                debounce_until = now + kDebounce;  // The file may have changed meanwhile.
            }
        }
        if (debounce_until && now >= *debounce_until) {
            debounce_until.reset();
            const std::uint64_t hash = content_hash();
            if (hash != 0 && hash != last_hash_) {
                callbacks_.reload("inotify");
                last_hash_ = hash;
            }
        }
    }
}

}  // namespace tsfs
