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
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
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
#include "filesystem/fuse_control.hpp"
#include "filesystem/fuse_session.hpp"
#include "filesystem/passthrough_fs.hpp"
#include "fsutil/backing_store.hpp"
#include "gc/garbage_collector.hpp"
#include "gc/tree_scanner.hpp"
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

// --- tsfs --stats ------------------------------------------------------------------

/// Per-rule counters of --stats.
struct RuleStats {
    std::uint64_t files{0};
    std::uint64_t bytes{0};
    std::uint64_t expired{0};
    std::uint64_t within_24h{0};
    std::uint64_t excluded{0};
    std::uint64_t never{0};
    std::optional<TimePoint> next_expiry;
};

int action_stats(const CliOptions& cli) {
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    const Config& config = *snapshot->config;
    const PolicyEngine& policy = *snapshot->policy;
    auto store = BackingStore::open(config.filesystem.backing_directory,
                                    {config.security.stay_on_filesystem, config.security.require_openat2, false});
    if (!store) {
        std::println(stderr, "tsfs: {}", store.error().message);
        return kExitState;
    }
    const TimePoint now = SystemClock().now();
    const TimePoint horizon = now + std::chrono::hours(24);
    std::map<int, RuleStats> per_rule;  // Rule index (configuration order), -1 = default.
    RuleStats total;
    TreeScanner scanner(**store, policy.needs_birth_time());
    const auto visit = [&](std::string_view path, const EntryStat& st) {
        const Decision d = policy.evaluate(path, st.times);
        for (RuleStats* s : {&per_rule[d.rule_index], &total}) {
            ++s->files;
            s->bytes += st.size;
            if (d.kind == Decision::Kind::excluded) {
                ++s->excluded;
            } else if (d.kind == Decision::Kind::keep_forever) {
                ++s->never;
            } else if (d.expiry <= now) {
                ++s->expired;
            } else {
                if (d.expiry <= horizon) {
                    ++s->within_24h;
                }
                if (!s->next_expiry || d.expiry < *s->next_expiry) {
                    s->next_expiry = d.expiry;
                }
            }
        }
    };
    std::uint64_t errors = 0;
    scanner.run(std::chrono::steady_clock::time_point::max(), visit, [&](std::string_view, int) { ++errors; });
    const ScanTotals& t = scanner.totals();
    const auto next = [](const RuleStats& s) {
        return s.next_expiry ? std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(*s.next_expiry))
                             : std::string("-");
    };
    std::println("TSFS statistics of {} ({} active rule(s), default retention {})", config.filesystem.backing_directory,
                 policy.active_rule_count(),
                 config.retention.default_retention ? format_duration(*config.retention.default_retention) : "never");
    std::println("{:<24} {:>9} {:>10} {:>8} {:>8} {:>9} {:>8}  {}", "RULE", "FILES", "SIZE", "EXPIRED", "<24H",
                 "EXCLUDED", "NEVER", "NEXT EXPIRY (UTC)");
    const auto row = [&](std::string_view name, const RuleStats& s) {
        std::println("{:<24} {:>9} {:>10} {:>8} {:>8} {:>9} {:>8}  {}", name, s.files, format_size(s.bytes), s.expired,
                     s.within_24h, s.excluded, s.never, next(s));
    };
    for (const auto& [index, s] : per_rule) {
        row(policy.rule_id(index), s);
    }
    row("TOTAL", total);
    std::println("directories: {}, symbolic links: {}, other entries: {}, unreadable: {}, mount boundaries: {}",
                 t.directories, t.symlinks, t.others, errors, t.boundaries);
    return kExitOk;
}

// --- tsfs --purge ------------------------------------------------------------------

/// What the CLI reads back from the status file of a running daemon.
struct DaemonStatus {
    pid_t pid{0};
    std::string mountpoint;
    std::uint64_t purges_completed{0};
    std::uint64_t purging{0};
    std::uint64_t deleted{0};
    std::uint64_t bytes_deleted{0};
    std::uint64_t failed{0};
    std::uint64_t skipped_open{0};
    std::uint64_t last_purge_deleted{0};
    std::uint64_t last_purge_bytes{0};
    std::uint64_t last_purge_failed{0};
    std::uint64_t last_purge_skipped_open{0};
};

std::optional<DaemonStatus> read_daemon_status(const std::string& path) {
    try {
        const YAML::Node root = YAML::LoadFile(path);  // JSON is YAML.
        DaemonStatus s;
        s.pid = root["pid"].as<pid_t>();
        s.mountpoint = root["mountpoint"].as<std::string>();
        const YAML::Node gc = root["gc"];
        s.purges_completed = gc["purges_completed"] ? gc["purges_completed"].as<std::uint64_t>() : 0;
        s.purging = gc["purging"] ? gc["purging"].as<std::uint64_t>() : 0;
        s.deleted = gc["deleted"].as<std::uint64_t>();
        s.bytes_deleted = gc["bytes_deleted"].as<std::uint64_t>();
        s.failed = gc["failed"].as<std::uint64_t>();
        s.skipped_open = gc["skipped_open"].as<std::uint64_t>();
        if (const YAML::Node last = gc["last_purge"]) {
            s.last_purge_deleted = last["deleted"].as<std::uint64_t>();
            s.last_purge_bytes = last["bytes_deleted"].as<std::uint64_t>();
            s.last_purge_failed = last["failed"].as<std::uint64_t>();
            s.last_purge_skipped_open = last["skipped_open"].as<std::uint64_t>();
        }
        return s;
    } catch (const std::exception&) {
        return std::nullopt;  // Missing, partial or older format.
    }
}

/// @return true when @p pid is a live process named "tsfs".
bool is_tsfs_process(pid_t pid) {
    if (pid <= 0 || (::kill(pid, 0) != 0 && errno != EPERM)) {
        return false;
    }
    std::ifstream comm(std::format("/proc/{}/comm", pid));
    std::string name;
    return std::getline(comm, name) && name == "tsfs";
}

/// Ask for confirmation on the terminal. @return true for "y"/"yes".
bool confirm(std::string_view question) {
    std::print("{} [y/N] ", question);
    (void)std::fflush(stdout);
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        return false;
    }
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

int action_purge(const CliOptions& cli, Logger& log) {
    if (cli.purge_preview) {
        return action_dry_run(cli, log);  // Exactly what --dry-run lists.
    }
    auto snapshot = ConfigManager::build(cli.config_path, cli.overrides);
    if (!snapshot) {
        print_config_error(snapshot.error());
        return kExitConfig;
    }
    const Config& config = *snapshot->config;
    if (config.gc.dry_run) {
        std::println(stderr,
                     "tsfs: garbage_collector.dry_run is true: a purge would delete nothing. Preview with "
                     "'tsfs --purge --dry-run', set dry_run to false to delete.");
        return kExitState;
    }
    // Who purges: the running daemon (it alone knows the files open through the mount),
    // or this process when no daemon serves the mount.
    std::optional<DaemonStatus> daemon;
    if (config.metrics.enabled) {
        daemon = read_daemon_status(config.metrics.status_file);
        if (daemon && (!is_tsfs_process(daemon->pid) || daemon->mountpoint != config.filesystem.mountpoint)) {
            daemon.reset();  // Stale status file of a stopped daemon.
        }
    }
    if (!daemon && fuse_connection_id(config.filesystem.mountpoint)) {
        std::println(stderr,
                     "tsfs: {} is mounted but its daemon cannot be found through metrics.status_file; refusing an "
                     "offline purge (files open through the mount would not be protected). Send SIGUSR2 to the "
                     "daemon instead.",
                     config.filesystem.mountpoint);
        return kExitState;
    }
    if (!cli.force) {
        if (::isatty(STDIN_FILENO) == 0) {
            std::println(stderr, "tsfs: --purge asks for confirmation on a terminal; use --force in scripts");
            return kExitUsage;
        }
        const std::string where =
            daemon ? std::format("the daemon (pid {}) serving {}", daemon->pid, config.filesystem.mountpoint)
                   : std::format("this process (no daemon serves {})", config.filesystem.mountpoint);
        if (!confirm(std::format("Delete now every expired file of {} (preview: tsfs --purge --dry-run); "
                                 "purge done by {}?",
                                 config.filesystem.backing_directory, where))) {
            std::println("purge cancelled");
            return kExitOk;
        }
    }
    if (daemon) {
        const DaemonStatus before = *daemon;
        if (::kill(daemon->pid, SIGUSR2) != 0) {
            const int err = errno;
            std::println(stderr, "tsfs: cannot signal the daemon (pid {}): {}", daemon->pid, std::strerror(err));
            return err == EPERM ? kExitPrivileges : kExitRuntime;
        }
        std::println("purge requested from the daemon (pid {}), waiting for it to finish...", daemon->pid);
        auto last_report = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const auto now_status = read_daemon_status(config.metrics.status_file);
            if (!is_tsfs_process(before.pid)) {
                std::println(stderr, "tsfs: the daemon (pid {}) stopped during the purge", before.pid);
                return kExitRuntime;
            }
            if (!now_status) {
                continue;  // Being rewritten.
            }
            if (now_status->purges_completed > before.purges_completed) {
                std::println("purge: {} file(s) deleted ({}), {} failed, {} kept because open through the mount",
                             now_status->last_purge_deleted, format_size(now_status->last_purge_bytes),
                             now_status->last_purge_failed, now_status->last_purge_skipped_open);
                return now_status->last_purge_failed > 0 ? kExitRuntime : kExitOk;
            }
            if (std::chrono::steady_clock::now() - last_report > std::chrono::seconds(5)) {
                last_report = std::chrono::steady_clock::now();
                std::println("  ... {} file(s) deleted so far", now_status->deleted - before.deleted);
            }
        }
    }
    // Offline: same GC, unlimited rate, passes repeated while the scheduler saturates.
    log.set_level(config.logging.level < LogLevel::warning ? LogLevel::warning : config.logging.level);
    auto store = BackingStore::open(config.filesystem.backing_directory,
                                    {config.security.stay_on_filesystem, config.security.require_openat2, false});
    if (!store) {
        std::println(stderr, "tsfs: {}", store.error().message);
        return kExitState;
    }
    auto gc_snapshot = std::make_shared<GcSnapshot>(*make_gc_snapshot(*snapshot));
    gc_snapshot->config.max_deletions_per_second = 0;
    gc_snapshot->config.max_deletions_per_cycle = 0;
    SystemClock clock;
    PathLockTable locks;
    GarbageCollector gc({**store, log, clock, locks, nullptr, {}}, gc_snapshot);
    for (int pass = 0; pass < 1000; ++pass) {
        const std::uint64_t saturations = gc.stats().queue_saturations;
        gc.run_full_scan();
        gc.process_due_now();
        if (gc.stats().queue_saturations == saturations) {
            break;
        }
    }
    const GcStats s = gc.stats();
    std::println("purge: {} file(s) deleted ({}), {} failed", s.deleted, format_size(s.bytes_deleted), s.failed);
    return s.failed > 0 ? kExitRuntime : kExitOk;
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
    /// fusectl identifier of the mount (from /proc/self/mountinfo, set once mounted).
    std::optional<std::uint32_t> fuse_connection_;
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
    in.fuse_connection = fuse_connection_;
    if (fuse_connection_) {
        in.fuse_waiting = fuse_connection_waiting(*fuse_connection_);
    }
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
    std::uint64_t last_purge_state = 0;
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
        log_.maintenance();
        const auto now = std::chrono::steady_clock::now();
        // A purge that starts or ends is published at once: tsfs --purge waits for it.
        const GcStats gc = gc_->stats();
        const std::uint64_t purge_state = gc.purges_completed * 2 + gc.purging;
        if (now >= next_status || purge_state != last_purge_state) {
            last_purge_state = purge_state;
            write_status();
            next_status = now + config_.current()->config->metrics.interval;
        }
        cv.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
    }
}

int Daemon::run() {
    // Block the process signals first: a SIGTERM arriving before the handlers are
    // installed (during the preflight, or between the mount and the handlers) would
    // otherwise kill the daemon with the default action and leave a dead mount
    // behind (ENOTCONN). Blocked, it stays
    // pending and is handled as soon as the loop starts (clean unmount). Helper
    // threads inherit this mask: process signals go to the main thread only.
    sigset_t async_signals;
    sigemptyset(&async_signals);
    for (const int sig : {SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE}) {
        sigaddset(&async_signals, sig);
    }
    pthread_sigmask(SIG_BLOCK, &async_signals, nullptr);
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
        cfg.filesystem.read_only, [this] { return config_.current()->policy; }});
    log_.log(LogLevel::info, "fs", "filesystem_mount_started", "mounting",
             {{"mountpoint", cfg.filesystem.mountpoint}, {"backing_directory", cfg.filesystem.backing_directory}});
    auto session = FuseSession::mount({.mountpoint = cfg.filesystem.mountpoint,
                                       .allow_other = cfg.filesystem.allow_other,
                                       .read_only = cfg.filesystem.read_only,
                                       .fuse_debug = cli_.fuse_debug,
                                       .retention_xattrs = cfg.filesystem.retention_xattrs},
                                      *fs_);
    if (!session) {
        std::println(stderr, "tsfs: {}", session.error().message);
        return kExitState;
    }
    session_ = std::move(*session);
    // Read from /proc/self/mountinfo: a stat() of the mount point would be a request
    // to this very daemon, whose loop is not running yet.
    fuse_connection_ = fuse_connection_id(cfg.filesystem.mountpoint);
    if (!cli_.foreground && fuse_daemonize(0) != 0) {
        return kExitRuntime;
    }

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
                           },
                           [this] {
                               log_.log(LogLevel::info, "gc", "gc_purge_requested", "purge requested (SIGUSR2)");
                               gc_->request_purge();
                           }});
    if (!watcher) {
        log_.log(LogLevel::error, "config", "config_reload_failed", watcher.error().message);
        return kExitRuntime;
    }

    if (auto r = session_->install_signal_handlers(); !r) {
        log_.log(LogLevel::critical, "fs", "filesystem_operation_failed", r.error().message);
        return kExitRuntime;
    }
    install_control_signal_handlers((*watcher)->reload_fd(), (*watcher)->status_fd(), (*watcher)->purge_fd());

    live_session_.store(session_.get());
    gc_->start();
    (*watcher)->start();
    std::jthread housekeeper([this](std::stop_token st) { housekeeping(st); });
    log_.log(LogLevel::info, "fs", "filesystem_mounted", "TSFS is serving requests",
             {{"mountpoint", cfg.filesystem.mountpoint},
              {"inotify", (*watcher)->inotify_active()},
              {"fuse_control", fuse_connection_ ? std::format("{}/{}", kFuseConnectionsDir, *fuse_connection_)
                                                : std::string("unknown")}});

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
        case CliAction::stats: return action_stats(cli);
        case CliAction::purge: {
            Logger log;
            return action_purge(cli, log);
        }
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
