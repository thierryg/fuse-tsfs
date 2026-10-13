// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/app/application.cpp
// Purpose : Entry point logic: one-shot actions and the mounted daemon lifecycle
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "app/application.hpp"

#include <fcntl.h>
#include <fuse.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <print>
#include <sstream>
#include <thread>

#include "common/clock.hpp"
#include "common/escape.hpp"
#include "common/threads.hpp"
#include "common/units.hpp"
#include "config/config_loader.hpp"
#include "config/config_manager.hpp"
#include "config/config_watcher.hpp"
#include "diagnostics/crash_handler.hpp"
#include "filesystem/fuse_session.hpp"
#include "filesystem/passthrough_fs.hpp"
#include "fsutil/backing_store.hpp"
#include "gc/garbage_collector.hpp"
#include "logging/rotating_file_sink.hpp"
#include "metrics/status_report.hpp"
#include "tsfs/version.hpp"

namespace tsfs {
namespace {

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(WallClock::now().time_since_epoch()).count();
}

/// Print @p path for a terminal: as is when harmless, escaped and quoted otherwise.
std::string printable(std::string_view path) {
    std::string quoted;
    append_text_quoted(quoted, path);
    return quoted.size() == path.size() + 2 && path.find(' ') == std::string_view::npos ? std::string(path) : quoted;
}

std::shared_ptr<const GcSnapshot> make_gc_snapshot(const ConfigSnapshot& snapshot) {
    auto gc = std::make_shared<GcSnapshot>();
    gc->config = snapshot.config->gc;
    gc->policy = snapshot.policy;
    gc->generation = snapshot.generation;
    return gc;
}

void print_config_error(const Error& error) { std::println(stderr, "tsfs: {}", error.message); }

int action_check_config(const CliOptions& cli) {
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    const Config& c = *snapshot->config;
    std::println("{}: configuration valid (version {}, {} active rule(s), default retention {}, GC {}{})",
                 cli.config_path, c.version, snapshot->policy->active_rule_count(),
                 c.retention.default_retention ? format_duration(*c.retention.default_retention) : "never",
                 c.gc.enabled ? "enabled" : "disabled", c.gc.dry_run ? ", dry run" : "");
    return kExitOk;
}

int action_show_config(const CliOptions& cli) {
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    std::print("# effective configuration of {} (defaults included)\n{}", cli.config_path,
               dump_config(*snapshot->config));
    return kExitOk;
}

int action_status(const CliOptions& cli) {
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    const std::string& path = snapshot->config->metrics.status_file;
    std::ifstream in(path);
    if (!in) {
        std::println(stderr, "tsfs: no status file at {} (is the daemon running with metrics.enabled?)", path);
        return kExitState;
    }
    std::ostringstream content;
    content << in.rdbuf();
    std::print("{}", content.str());
    return kExitOk;
}

/// @return true when /etc/fuse.conf contains an active "user_allow_other" line.
bool fuse_conf_allows_other() {
    std::ifstream conf("/etc/fuse.conf");
    std::string line;
    while (std::getline(conf, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 16, "user_allow_other") == 0) {
            return true;
        }
    }
    return false;
}

/// Mount preflight: fuse_mount() reports no errno, so the privilege problems are
/// detected here, with an actionable message and exit code 3 (4 without FUSE).
int check_mount_privileges(const FilesystemConfig& fs) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        const int err = errno;
        if (err == ENOENT) {
            std::println(stderr,
                         "tsfs: /dev/fuse does not exist (load the fuse module; in a container, pass the device)");
            return kExitDependencies;
        }
        std::println(stderr, "tsfs: cannot use /dev/fuse: {} (run as root or grant access to the device)",
                     std::strerror(err));
        return kExitPrivileges;
    }
    if (fs.allow_other && ::geteuid() != 0 && !fuse_conf_allows_other()) {
        std::println(stderr, "tsfs: filesystem.allow_other needs root or 'user_allow_other' in /etc/fuse.conf");
        return kExitPrivileges;
    }
    return kExitOk;
}

/**
 * Mount point preflight: an existing directory that is not already the root of a
 * mount. Mounting over another mount (a second TSFS instance configured with the
 * same mount point, for example) would hide it; the kernel allows it for root.
 */
int check_mount_point(const std::string& path) {
    struct statx stx{};
    if (::statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_TYPE, &stx) != 0) {
        const int err = errno;
        if (err == EACCES || err == EPERM) {
            // A FUSE mount of another user (without allow_other) denies even stat().
            std::println(stderr, "tsfs: cannot access the mount point '{}': {} (already a FUSE mount of another user?)",
                         path, std::strerror(err));
            return kExitPrivileges;
        }
        std::println(stderr, "tsfs: the mount point '{}' does not exist: {}", path, std::strerror(err));
        return kExitState;
    }
    if (!S_ISDIR(stx.stx_mode)) {
        std::println(stderr, "tsfs: the mount point '{}' is not a directory", path);
        return kExitState;
    }
    if ((stx.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) != 0 && (stx.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0) {
        std::println(stderr,
                     "tsfs: '{}' is already a mount point (see: findmnt '{}'); unmount it or choose another "
                     "filesystem.mountpoint",
                     path, path);
        return kExitState;
    }
    return kExitOk;
}

int action_dry_run(const CliOptions& cli, Logger& log) {
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    const Config& config = *snapshot->config;
    log.set_level(config.logging.level < LogLevel::warning ? LogLevel::warning : config.logging.level);
    auto store = BackingStore::open(config.filesystem.backing_directory,
                                    {config.security.stay_on_filesystem, config.security.require_openat2, false});
    if (!store) {
        std::println(stderr, "tsfs: {}", store.error().message);
        return kExitState;
    }
    SystemClock clock;
    PathLockTable locks;
    GarbageCollector gc({**store, log, clock, locks, nullptr, {}}, make_gc_snapshot(*snapshot));
    const auto report = gc.dry_run_report();
    std::uint64_t bytes = 0;
    std::println("{:<24} {:>12}  {:<24} {}", "EXPIRED AT (UTC)", "SIZE", "RULE", "PATH");
    for (const DryRunEntry& entry : report) {
        bytes += entry.size;
        std::println("{:%FT%TZ} {:>12}  {:<24} {}", std::chrono::floor<std::chrono::seconds>(entry.expiry), entry.size,
                     entry.rule, printable(entry.path));
    }
    const GcStats stats = gc.stats();
    std::println("dry run: {} file(s), {} byte(s) would be deleted now; {} file(s) scanned in {}", report.size(), bytes,
                 stats.last_scan_files, config.filesystem.backing_directory);
    if (stats.queue_saturations > 0) {
        // The scheduler is bounded (memory): what did not fit is not listed.
        std::println(
            "dry run: INCOMPLETE: {} expiring file(s) did not fit in garbage_collector.max_pending_tasks ({}) "
            "and are not listed; raise it to see them all",
            stats.queue_saturations, config.gc.max_pending_tasks);
    }
    return kExitOk;
}

/// The mounted service: owns every component and the startup/shutdown order.
class Daemon {
public:
    Daemon(const CliOptions& cli, Logger& log) : cli_(cli), log_(log), config_(cli.config_path, cli.overrides, log) {}

    int run();

private:
    void housekeeping(std::stop_token stop);
    void write_status();

    const CliOptions& cli_;
    Logger& log_;
    ConfigManager config_;
    SystemClock clock_;
    std::unique_ptr<BackingStore> store_;
    PathLockTable locks_;
    OpenFileRegistry open_files_;
    std::unique_ptr<GarbageCollector> gc_;
    std::unique_ptr<PassthroughFs> fs_;
    std::unique_ptr<FuseSession> session_;
    std::atomic<FuseSession*> live_session_{nullptr};
    std::int64_t started_unix_ms_{now_unix_ms()};
    /// Serializes write_status(): the housekeeping thread and the SIGUSR1 callback of
    /// the configuration watcher both write the status file.
    std::mutex status_mutex_;
};

void Daemon::write_status() {
    const std::scoped_lock lock(status_mutex_);
    const auto snap = config_.current();
    if (!snap->config->metrics.enabled) {
        return;
    }
    StatusInputs in;
    in.version = kVersion;
    in.mountpoint = snap->config->filesystem.mountpoint;
    in.backing_directory = snap->config->filesystem.backing_directory;
    in.pid = ::getpid();
    in.started_unix_ms = started_unix_ms_;
    in.now_unix_ms = now_unix_ms();
    in.config_generation = snap->generation;
    in.config_reloads = config_.reload_successes();
    in.config_reload_failures = config_.reload_failures();
    in.gc = gc_->stats();
    in.fs = fs_->stats();
    in.logging = log_.stats();
    if (auto r = write_file_atomically(snap->config->metrics.status_file, render_status_json(in)); !r) {
        static LogRateLimiter limiter(1, std::chrono::minutes(10));
        if (limiter.check("status").allowed) {
            log_.log(LogLevel::warning, "metrics", "filesystem_operation_failed", "cannot write the status file",
                     {{"file", snap->config->metrics.status_file}, {"error", r.error().message}});
        }
    }
}

void Daemon::housekeeping(std::stop_token stop) {
    prepare_helper_thread("tsfs-housekeep");
    std::mutex mutex;
    std::condition_variable_any cv;
    auto next_status = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
        log_.maintenance();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status) {
            write_status();
            next_status = now + config_.current()->config->metrics.interval;
        }
        cv.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
    }
}

int Daemon::run() {
    // The kernel already applied the umask of the process creating a file through the
    // mount; the daemon's own umask (e.g. UMask= of the systemd unit) must not be
    // applied a second time. Every file the daemon creates for itself (logs, crash
    // reports, status) has an explicit restrictive mode.
    ::umask(0);
    if (auto r = config_.load_initial(); !r) {
        print_config_error(r.error());
        return kExitConfig;
    }
    const auto initial = config_.current();
    const Config& cfg = *initial->config;

    // --- preflight: everything that can fail is checked while stderr is a terminal ---
    if (cfg.logging.enabled) {
        if (auto r = RotatingFileSink::check_writable(cfg.logging.directory, cfg.logging.filename); !r) {
            std::println(stderr, "tsfs: {} (logging.directory; run as the service account or fix the permissions)",
                         r.error().message);
            return kExitState;
        }
    }
    auto store = BackingStore::open(cfg.filesystem.backing_directory,
                                    {cfg.security.stay_on_filesystem, cfg.security.require_openat2, false});
    if (!store) {
        std::println(stderr, "tsfs: {}", store.error().message);
        return store.error().code == ENOSYS ? kExitDependencies : kExitState;
    }
    store_ = std::move(*store);
    if (const int rc = check_mount_privileges(cfg.filesystem); rc != kExitOk) {
        return rc;
    }
    if (const int rc = check_mount_point(cfg.filesystem.mountpoint); rc != kExitOk) {
        return rc;
    }

    // --- components (no thread yet: fuse_daemonize() forks) ---------------------------
    gc_ = std::make_unique<GarbageCollector>(
        GarbageCollector::Dependencies{*store_, log_, clock_, locks_, &open_files_,
                                       [this](std::string_view path) {
                                           if (FuseSession* s = live_session_.load()) {
                                               s->invalidate(path);
                                           }
                                       }},
        make_gc_snapshot(*initial));
    fs_ = std::make_unique<PassthroughFs>(PassthroughFs::Dependencies{
        *store_, log_, locks_, open_files_, [this](std::string_view path) { gc_->notify_changed(path); },
        cfg.filesystem.read_only});
    log_.log(LogLevel::info, "fs", "filesystem_mount_started", "mounting",
             {{"mountpoint", cfg.filesystem.mountpoint}, {"backing_directory", cfg.filesystem.backing_directory}});
    auto session = FuseSession::mount(
        {cfg.filesystem.mountpoint, cfg.filesystem.allow_other, cfg.filesystem.read_only, cli_.fuse_debug}, *fs_);
    if (!session) {
        std::println(stderr, "tsfs: {}", session.error().message);
        return kExitState;
    }
    session_ = std::move(*session);
    if (!cli_.foreground && fuse_daemonize(0) != 0) {
        return kExitRuntime;
    }

    // Helper threads inherit this mask: process signals go to the main thread only.
    sigset_t async_signals;
    sigemptyset(&async_signals);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE}) {
        sigaddset(&async_signals, sig);
    }
    pthread_sigmask(SIG_BLOCK, &async_signals, nullptr);

    if (auto r = log_.configure(cfg.logging, cli_.debug); !r) {
        log_.log(LogLevel::critical, "logging", "log_write_failed", r.error().message);
        return kExitState;
    }
    log_.log(LogLevel::info, "config", "config_loaded", "configuration loaded",
             {{"file", cli_.config_path},
              {"generation", initial->generation},
              {"rules", initial->policy->active_rule_count()},
              {"version", kVersion}});

    if (cfg.diagnostics.enabled) {
        if (cfg.diagnostics.text_stack_trace) {
            if (auto r = diagnostics::CrashHandler::install(
                    {cfg.diagnostics.crash_directory, cfg.diagnostics.max_crash_reports, kVersion});
                r) {
                log_.log(LogLevel::info, "diagnostics", "diagnostics_enabled", "crash reports armed",
                         {{"directory", cfg.diagnostics.crash_directory},
                          {"symbols", diagnostics::CrashHandler::symbol_count()}});
            } else {
                log_.log(LogLevel::warning, "diagnostics", "diagnostics_unavailable", r.error().message);
            }
        }
        if (cfg.diagnostics.native_core_dump) {
            const auto core = diagnostics::enable_core_dumps();
            log_.log(core.enabled ? LogLevel::info : LogLevel::warning, "diagnostics", "diagnostics_core_dumps",
                     core.enabled ? "native core dumps enabled" : "native core dumps unavailable",
                     {{"rlimit_core", static_cast<std::uint64_t>(core.soft_limit)},
                      {"core_pattern", core.core_pattern},
                      {"handler", core.handler}});
            for (const std::string& note : core.notes) {
                log_.log(LogLevel::info, "diagnostics", "diagnostics_core_dumps", note);
            }
        }
    }

    // --- reload wiring: logger first (fallible), then the GC ---------------------------
    config_.add_observer([this](const ConfigSnapshot& next, const ConfigSnapshot& previous) -> Result<void> {
        if (next.config->logging != previous.config->logging) {
            return log_.configure(next.config->logging, cli_.debug);
        }
        return {};
    });
    config_.add_observer([this](const ConfigSnapshot& next, const ConfigSnapshot&) -> Result<void> {
        gc_->apply(make_gc_snapshot(next));
        return {};
    });
    auto watcher = ConfigWatcher::create(
        cli_.config_path, {[this](std::string_view trigger) {
                               // reload() logs its own failures.
                               (void)config_.reload(trigger);  // NOLINT(bugprone-unused-return-value)
                           },
                           [this] {
                               write_status();
                               const GcStats s = gc_->stats();
                               log_.log(LogLevel::info, "metrics", "status_requested", "status requested (SIGUSR1)",
                                        {{"deleted", s.deleted},
                                         {"scheduled", s.scheduled},
                                         {"failed", s.failed},
                                         {"managed_files", s.last_scan_files}});
                           }});
    if (!watcher) {
        log_.log(LogLevel::error, "config", "config_reload_failed", watcher.error().message);
        return kExitRuntime;
    }

    if (auto r = session_->install_signal_handlers(); !r) {
        log_.log(LogLevel::critical, "fs", "filesystem_operation_failed", r.error().message);
        return kExitRuntime;
    }
    install_control_signal_handlers((*watcher)->reload_fd(), (*watcher)->status_fd());

    live_session_.store(session_.get());
    gc_->start();
    (*watcher)->start();
    std::jthread housekeeper([this](std::stop_token st) { housekeeping(st); });
    log_.log(LogLevel::info, "fs", "filesystem_mounted", "TSFS is serving requests",
             {{"mountpoint", cfg.filesystem.mountpoint}, {"inotify", (*watcher)->inotify_active()}});

    pthread_sigmask(SIG_UNBLOCK, &async_signals, nullptr);
    const int rc = session_->loop();
    pthread_sigmask(SIG_BLOCK, &async_signals, nullptr);

    // --- shutdown: stop producers of work before tearing the mount down ---------------
    log_.log(LogLevel::info, "fs", "filesystem_unmounted", "shutting down", {{"loop_status", rc}});
    install_control_signal_handlers(-1, -1);
    (*watcher)->stop();
    housekeeper.request_stop();
    housekeeper.join();
    live_session_.store(nullptr);
    gc_->stop();
    write_status();
    session_.reset();  // Unmount.
    fs_.reset();       // Frees the handles left open (logged while the logger still runs).
    diagnostics::CrashHandler::uninstall();
    log_.shutdown();
    // libfuse >= 3.16 reports the terminating signal as the loop status: a SIGINT or
    // SIGTERM shutdown is a normal exit.
    const bool clean = rc == 0 || rc == SIGINT || rc == SIGTERM || rc == -SIGINT || rc == -SIGTERM;
    return clean ? kExitOk : kExitRuntime;
}

}  // namespace

int run_application(const CliOptions& cli) {
    switch (cli.action) {
        case CliAction::help: std::print("{}", usage_text()); return kExitOk;
        case CliAction::version: std::print("{}", version_text()); return kExitOk;
        case CliAction::print_example_config: std::print("{}", example_configuration()); return kExitOk;
        case CliAction::check_config: return action_check_config(cli);
        case CliAction::show_config: return action_show_config(cli);
        case CliAction::status: return action_status(cli);
        case CliAction::dry_run: {
            Logger log;
            return action_dry_run(cli, log);
        }
        case CliAction::run: {
            Logger log;
            Daemon daemon(cli, log);
            return daemon.run();
        }
    }
    return kExitUsage;
}

}  // namespace tsfs
