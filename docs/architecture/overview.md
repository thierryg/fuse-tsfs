<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/overview.md
Purpose : Architecture overview: components, process lifecycle, data flow, ownership, errors
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# TSFS architecture overview

TSFS is a single-process libfuse3 daemon that exposes a *backing directory* at a
*mount point* (passthrough) and deletes regular files once the retention rule
that applies to them has elapsed. This document is the entry point of the
architecture set; every statement refers to the code under `src/`.

| Document | Topic |
|---|---|
| [language-selection.md](language-selection.md) | Why C++26, toolchain, C++26 features actually used |
| [filesystem.md](filesystem.md) | FUSE layer, operations, path resolution, locks |
| [policy-engine.md](policy-engine.md) | Rule grammar, matching, precedence, expiry arithmetic |
| [garbage-collector.md](garbage-collector.md) | Scanner, scheduler, workers, validation, retries |
| [configuration-reload.md](configuration-reload.md) | Snapshots, reload triggers, reloadable settings |
| [concurrency.md](concurrency.md) | Threads, mutexes, lock order, signals, shutdown |
| [metadata.md](metadata.md) | What state exists, and why there is no persistent index |
| [logging.md](logging.md) | Logger facade, rotation, compression, failure policy |
| [diagnostics.md](diagnostics.md) | Crash reports and native core dumps |

User-facing material: [user guide](../user/user-guide.md),
[configuration reference](../user/configuration.md),
[monitoring](../user/monitoring.md). Security analysis:
[threat model](../security/threat-model.md). Tests:
[test strategy](../testing/test-strategy.md). Requirements:
[SPECIFICATION.md](../developer/SPECIFICATION.md).

## 1. Components

```
                        +--------------------------- tsfs process ----------------------------+
 kernel FUSE  <-------> | filesystem/  FuseSession + PassthroughFs (libfuse worker threads)    |
 (/dev/fuse)            |      |  on_changed(path)            ^ invalidate(path)              |
                        |      v                              |                               |
                        | gc/  GarbageCollector: coordinator + workers                         |
                        |      | uses: policy/ PolicyEngine (immutable, shared)                |
                        |      | uses: fsutil/ BackingStore, PathLockTable, OpenFileRegistry   |
                        |      v                                                               |
 backing directory <--- | fsutil/ BackingStore (root fd, openat2 / fallback walk)              |
                        |                                                                      |
 config.yml ----------> | config/ ConfigWatcher -> ConfigManager -> observers (logger, GC)     |
 SIGHUP/USR1/USR2 ----> |         (eventfd bridges; USR2 -> GarbageCollector::request_purge)   |
                        | logging/ Logger (spdlog pipeline, RotatingFileSink, LogCompressor)   |
                        | metrics/ status JSON writer        diagnostics/ CrashHandler         |
                        | app/ Daemon (owner, startup/shutdown order)   cli/ argument parsing  |
                        +----------------------------------------------------------------------+
```

| Directory | Main types | Responsibility |
|---|---|---|
| `src/app` | `Daemon`, `run_application` (`application.cpp`) | Selects the action; owns every component of the mounted service and the startup/shutdown order. |
| `src/cli` | `parse_command_line`, `CliOptions`, `ExitCode` | getopt_long parsing, action exclusivity, CLI overrides, usage/version text, the example configuration embedded with `#embed`. |
| `src/common` | `Result`/`Error`, `UniqueFd`, `Clock`, `managed_path`, units, escape, threads | Shared vocabulary: error values, RAII descriptor, injectable wall clock, managed-path normalization, duration/size parsing, injection-safe string escaping, helper-thread setup. |
| `src/config` | `Config`, `parse_config`, `ConfigManager`, `ConfigWatcher` | Strict YAML schema (yaml-cpp), immutable snapshots, validated hot reload, inotify/SIGHUP/SIGUSR1 triggers, SIGUSR2 purge bridge. |
| `src/policy` | `PolicyEngine`, `Glob`, `Decision` | Compiled retention rules; deterministic precedence; saturating expiry computation. |
| `src/gc` | `GarbageCollector`, `TreeScanner`, `ExpirationScheduler` (`HeapScheduler`, `OrderedScheduler`), `TokenBucket` | Discovery (time-sliced scans + FUSE notifications), bounded scheduling, validated deletion, retries, unthrottled purge. |
| `src/filesystem` | `PassthroughFs`, `FuseSession`, `fuse_connection_id`, `fuse_connection_waiting` | High-level libfuse operation table (including the read-only retention extended attributes), mount lifecycle, kernel cache invalidation, kernel FUSE connection (fusectl) of the mount. |
| `src/fsutil` | `BackingStore`, `stat_entry`, `PathLockTable`, `OpenFileRegistry` | Race-resistant access to the backing tree; per-path stripes; open-inode registry. |
| `src/logging` | `Logger`, `RotatingFileSink`, `LogCompressor`, `NoticeQueue`, `LogRateLimiter` | Structured text/JSON logging, size-based rotation, background gzip, failure reporting without recursion. |
| `src/metrics` | `ShardedCounters`, `render_status_json`, `write_file_atomically` | Contention-free FUSE counters; status file written atomically. |
| `src/tools` | `tsfs_ls.cpp` (executable `tsfs-ls`) | Standalone `ls` wrapper (no TSFS library). Every listing is produced by the original `ls` (`ls.orig`): outside TSFS mounts, in pipes and with `TSFS_LS=off` it is run with `execv` and the unchanged command line; inside a TSFS mount on a terminal it is run with `-l --dired` appended after the user's options (before a `--`), and its dired trailer locates each entry, which is tagged with the retention read from the `user.tsfs.*` extended attributes. Installed as the system `ls` by default (`TSFS_REPLACE_SYSTEM_LS`: the system `ls` is renamed `<ls>.orig`). |
| `src/diagnostics` | `CrashHandler`, `SymbolTable`, `enable_core_dumps` | Async-signal-safe text crash reports with a symbolized backtrace (function symbols of every loaded ELF object, loaded at installation and searched in the signal handler); native core-dump enabling. |

## 2. Process lifecycle

### 2.1 Entry point

`src/main.cpp` parses the command line (`parse_command_line`, never prints) and
calls `run_application` (`src/app/application.cpp`). A usage error returns
exit code 2; options that only apply when mounting (`--foreground`, `--debug`,
`--fuse-debug`, the diagnostics options) combined with an action are also usage errors
(exit 2). An exception escaping everything returns 1. One-shot actions
(`--help`, `--version`, `--print-example-config`, `--check-config`,
`--show-config`, `--status`, `--dry-run`, `--stats`, `--purge`) return without
mounting. The default action constructs a `Logger` and a `Daemon` and calls
`Daemon::run()`.

| Action | Function | Work |
|---|---|---|
| `--dry-run` | `action_dry_run` | Standalone `GarbageCollector` (no threads, no open-file registry), one full pass, every due candidate validated with `force_dry_run`; prints the table. |
| `--stats` | `action_stats` | One `TreeScanner` pass over the backing directory, `PolicyEngine::evaluate` per regular file, per-rule counters (files, bytes, expired, within 24 h, excluded, never, next expiry). |
| `--purge` | `action_purge` | `--dry-run` with `--purge --dry-run`; otherwise finds the daemon through the status file and sends it `SIGUSR2`, then polls the status file; without a daemon and without a mount, purges offline with a standalone `GarbageCollector` without rate limits. See [garbage-collector.md](garbage-collector.md#15-purge-tsfs---purge-sigusr2). |

Exit codes (`src/cli/command_line.hpp`): 0 ok, 1 runtime, 2 usage, 3
privileges (mount preflight: `/dev/fuse` not accessible, or `allow_other` without
root and without `user_allow_other`), 4 missing dependency (openat2 required but
absent, `/dev/fuse` missing), 5 invalid configuration (including an untrusted
configuration file and invalid `-m`/`-b` overrides), 7 unexpected system state.

### 2.2 Startup sequence (`Daemon::run`, `src/app/application.cpp`)

| Step | Code | Failure |
|---|---|---|
| 0a. Block SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE in the main thread, first of all: a signal received during startup stays pending (instead of killing the daemon, possibly after the mount, leaving a dead mount) and is handled when the loop starts; helper threads inherit the mask. | `pthread_sigmask` | |
| 0. `umask(0)`: the kernel already applied the client's umask to entries created through the mount; every file the daemon creates for itself has an explicit mode. | `::umask` | |
| 1. Load the configuration (file trust check: a root daemon requires a root-owned file not writable by group or others), validate it, apply and validate the CLI overrides, compile the policy (generation 1). | `ConfigManager::load_initial` | exit 5 |
| 2. Preflight: log directory/file creatable (if `logging.enabled`). | `RotatingFileSink::check_writable` | exit 7 |
| 3. Open the backing directory (root descriptor, openat2 probe). | `BackingStore::open` | exit 4 (ENOSYS) or 7 |
| 4. Mount preflight: `/dev/fuse` readable and writable; `allow_other` needs root or `user_allow_other` in `/etc/fuse.conf`. | `check_mount_privileges` | exit 3 (exit 4 when `/dev/fuse` does not exist) |
| 4b. Check that the mount point exists, is a directory and is not already a mount point. | `check_mount_point` (`statx`, `STATX_ATTR_MOUNT_ROOT`) | exit 7 (exit 3 when access is denied) |
| 5. Construct `GarbageCollector` and `PassthroughFs` (no thread started). | | |
| 6. `fuse_new` (operation table with or without the xattr callbacks, `filesystem.retention_xattrs`) + `fuse_mount`; then read the FUSE connection identifier of the new mount from `/proc/self/mountinfo` (never a `stat()` of the mount, which would be a request to this daemon whose loop is not running yet). | `FuseSession::mount`, `fuse_connection_id` | exit 7 |
| 7. `fuse_daemonize(0)` unless `--foreground`. Done before any thread exists because it forks. | | exit 1 |
| 8. No helper thread exists yet; the signal mask set at step 0a is inherited by every thread created from now on. | | |
| 9. Build the real logging pipeline (replaces the stderr bootstrap logger). | `Logger::configure` | exit 7 |
| 10. Optional diagnostics: crash handler (loads the symbol table), core dumps (failures are logged, not fatal). Events `diagnostics_enabled`, `diagnostics_core_dumps`, `diagnostics_unavailable`. | `CrashHandler::install`, `enable_core_dumps` | warning |
| 11. Register reload observers: logger first (fallible), then GC. | `ConfigManager::add_observer` | |
| 12. Create the configuration watcher (eventfds + inotify). | `ConfigWatcher::create` | exit 1 |
| 13. Install libfuse's signal handlers, then TSFS's SIGHUP/SIGUSR1/SIGUSR2 handlers. | `FuseSession::install_signal_handlers`, `install_control_signal_handlers` | exit 1 |
| 14. Publish the session for cache invalidation, start GC, watcher, housekeeping thread; log `filesystem_mounted` (with `fuse_control`). | `gc_->start()`, `watcher->start()`, `std::jthread` | |
| 15. Unblock the async signals in the main thread and run `fuse_loop_mt`. | `FuseSession::loop` | |

Every check that can fail on configuration or environment (steps 1-6) runs
while stderr is still the terminal, so the operator sees the error before the
process detaches.

### 2.3 Signals

| Signal | Handler | Effect |
|---|---|---|
| SIGINT, SIGTERM | libfuse (`fuse_set_signal_handlers`) | Ends `fuse_loop_mt`; shutdown follows. |
| SIGHUP | `on_sighup` (`src/config/config_watcher.cpp`), installed after libfuse's | `write(2)` to the reload eventfd; the watcher thread reloads. |
| SIGUSR1 | `on_sigusr1` | `write(2)` to the status eventfd; the watcher thread writes the status file and logs `status_requested`. |
| SIGUSR2 | `on_sigusr2` | `write(2)` to the purge eventfd; the watcher thread logs `gc_purge_requested` and calls `GarbageCollector::request_purge()`; the coordinator runs the purge. |
| SIGPIPE | libfuse | Ignored by libfuse's handler set. |
| SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP | `fatal_handler` when diagnostics are enabled | Text crash report, then default action. |

Details and masking rules: [concurrency.md](concurrency.md#5-signal-handling).

### 2.4 Reload

A reload is triggered by SIGHUP (always) or by an inotify event on the
configuration directory (only if the file content changed). The watcher thread
calls `ConfigManager::reload`, which validates the complete file, keeps
restart-only settings, runs the observers (logger, then GC) and publishes a new
immutable snapshot. A failure keeps the previous snapshot. See
[configuration-reload.md](configuration-reload.md).

### 2.5 Shutdown sequence (`src/app/application.cpp`)

When `fuse_loop_mt` returns (signal, `fusermount3 -u`, or error), the main
thread re-blocks the async signals and tears the service down in this order:

1. Restore default SIGHUP/SIGUSR1/SIGUSR2 dispositions (`install_control_signal_handlers(-1, -1)`, the purge descriptor defaulting to -1).
2. Stop and join the configuration watcher (no more reloads).
3. Stop and join the housekeeping thread.
4. Clear `live_session_` so the GC stops invalidating kernel caches.
5. `GarbageCollector::stop()`: join the coordinator, then the workers (each
   finishes the file it is processing); pending candidates are abandoned and
   counted in the `gc_completed` event.
6. Write a final status file.
7. `session_.reset()`: remove libfuse signal handlers, `fuse_unmount`, `fuse_destroy`.
8. Uninstall the crash handler.
9. `Logger::shutdown()`: drain notices, flush, wait for the compressor, swap back to the stderr bootstrap logger.

A loop status of 0, `SIGINT`, `SIGTERM` (or their negatives, as reported by
libfuse >= 3.16) is a clean exit (code 0); anything else returns 1.

## 3. Data flow: FUSE to GC notifications

```
 client write()/close()                                GC coordinator thread
        |                                                       |
 PassthroughFs::write_buf  -> handle->modified = true           |
 PassthroughFs::release    -> changed(path)                     |
        |  on_changed (std::function, set by Daemon)            |
        v                                                       |
 GarbageCollector::notify_changed(path)                         |
   lock mutex_; if events_.size() >= min(max_pending_tasks, 65536)
       -> events_dropped++ (the next scan finds the file)       |
   else events_.push_back(path); wake_ = true; notify_one ----->+ drain_events(): up to 4096 paths
                                                                |   open_parent + statx (no lock held)
                                                                |   ENOENT -> scheduler.cancel(path)
                                                                |   regular file -> evaluate policy
                                                                |   expiry <= now + 2*scan_interval
                                                                |      -> scheduler.schedule(...)
                                                                v
                                                        dispatch_due() -> work queue -> workers
                                                                v
                                                 worker: validate + unlinkat
                                                                v
                                         on_deleted(path) -> FuseSession::invalidate(path)
                                                          (fuse_invalidate_path)
```

Operations that call `changed()` (`src/filesystem/passthrough_fs.cpp`):
`mknod`, `create`, path-based `truncate`, path-based `utimens`, `rename`
(destination; also the source with `RENAME_EXCHANGE`), `link` (new name), and
`release` of a handle that was written, truncated, `fallocate`d, `futimens`ed,
opened with `O_TRUNC` or created. `notify_changed` never blocks on I/O: it only
takes the GC mutex to append to a bounded deque.

## 4. Ownership and lifetime rules

* **Single owner.** `Daemon` (`src/app/application.cpp`) owns the
  `ConfigManager`, `SystemClock`, `BackingStore`, `PathLockTable`,
  `OpenFileRegistry`, `GarbageCollector`, `PassthroughFs` and `FuseSession`. The
  `Logger` is created by `run_application` and outlives the `Daemon`. There is
  no global logger and no singleton service; the only process-wide state is
  signal-related (eventfd numbers for the SIGHUP/SIGUSR1/SIGUSR2 handlers, crash-handler
  state) and the rate-limited `stderr_fallback()`.
* **Dependency injection by reference.** `GarbageCollector::Dependencies` and
  `PassthroughFs::Dependencies` hold references; the owner guarantees that the
  referenced objects outlive the users. Member declaration order in `Daemon`
  makes the destructor release the session first, then the filesystem, then the
  GC, then the shared utilities.
* **Immutable shared state.** `ConfigSnapshot` (config + compiled policy +
  generation), `GcSnapshot` and the logger `Pipeline` are published through
  `std::atomic<std::shared_ptr<...>>`. A reader keeps a consistent snapshot as
  long as it holds the `shared_ptr`; old snapshots die with their last reader.
* **RAII everywhere.** Descriptors (`UniqueFd`), directory streams
  (`TreeScanner::DirCloser`, `DirHandle`), path stripes (`PathLockTable::Guard`),
  the mount (`FuseSession` destructor: remove handlers, unmount, destroy) and
  threads (`std::jthread`, joined on destruction) are released by destructors.
* **FUSE handles.** `fuse_file_info::fh` carries a heap `FileHandle` (descriptor,
  identity, `modified` flag) or `DirHandle`, created in `open`/`create`/`opendir`
  and destroyed in `release`/`releasedir`.
* **Cross-component callbacks.** The GC never references the FUSE session
  directly: it calls `on_deleted`, which loads `live_session_` (an
  `std::atomic<FuseSession*>`) that is non-null only between startup step 14 and
  shutdown step 4.

## 5. Error propagation

* Every fallible function returns `Result<T>` = `std::expected<T, Error>`
  (`src/common/error.hpp`). `Error` carries a positive errno-style `code` and a
  US-English `message`. `make_error(code, msg)` and `propagate(error)` build the
  unexpected value. Validation errors use `EINVAL`.
* Errors are values, not exceptions, so nothing unwinds through libfuse's C
  callbacks. Exceptions thrown by the standard library or dependencies are
  contained at boundaries:
  * FUSE: every operation goes through `Trampoline<Method, Op>::call`
    (`src/filesystem/passthrough_fs.cpp`), which is `noexcept`, maps
    `std::bad_alloc` to `-ENOMEM` and anything else to `-EIO`, counts the call,
    and reports unexpected errnos.
  * Configuration: the YAML reader throws an internal `ConfigException` that
    `parse_config` converts to `EINVAL` with `<file>:<line>:<column>: <key>: <reason>`.
  * Logging: `Logger::log` catches exceptions and reports through the
    rate-limited `stderr_fallback()`; spdlog's error handler does the same.
  * `main()` catches `std::exception` as a last resort (exit 1).
* FUSE operations return `0` or a negative errno; `Error::code` values coming
  from `BackingStore` are negated (`neg(parent.error().code)`).
* The GC classifies errno values into outcomes (vanished, unsafe, retry,
  failed): see [garbage-collector.md](garbage-collector.md#8-outcomes-and-retries).

## 6. Extension points

| Extension point | Mechanism | Where |
|---|---|---|
| Scheduler data structure | Strategy interface `ExpirationScheduler` + factory `make_scheduler(SchedulerKind, capacity)` | `src/gc/scheduler.hpp` |
| Time source | `Clock` interface (`SystemClock`, `ManualClock` for tests) | `src/common/clock.hpp` |
| Reaction to configuration changes | `ConfigManager::Observer` list, called in registration order; an error aborts publication | `src/config/config_manager.hpp` |
| FUSE-to-GC coupling | `PassthroughFs::Dependencies::on_changed`, `GarbageCollector::Dependencies::on_deleted` (`std::function`) | `src/filesystem/passthrough_fs.hpp`, `src/gc/garbage_collector.hpp` |
| Exposure model | `FilesystemMode` enum; only `passthrough` exists and is the only value the loader accepts | `src/config/config.hpp` |
| New log fields | `LogField::Value` variant (string, signed, unsigned, double, bool) | `src/logging/log_field.hpp` |
| New status fields | `StatusInputs` + `render_status_json` | `src/metrics/status_report.hpp` |

## 7. Unresolved requirements and explicit assumptions

Requirements from [SPECIFICATION.md](../developer/SPECIFICATION.md) that are
not, or only partially, implemented:

* **Managed-storage mode** (spec section 4, mode B): not implemented;
  `filesystem.mode` only accepts `passthrough`.
* **Timing wheel / hierarchical scheduler** (spec section 7): not implemented.
  Two strategies exist (binary heap with lazy invalidation, ordered map) and are
  compared in `benchmarks/bench_scheduler.cpp`.
* **Persistent metadata** (spec section 8): deliberately absent; state is
  rebuilt by reconciliation. See [metadata.md](metadata.md).
* **"First observation" lifetime** (spec section 6): not supported; only
  `st_mtime` and statx birth time.
* **Extended attributes** (other than the four read-only `user.tsfs.*`
  retention attributes), **ioctl, poll, flock/POSIX lock callbacks,
  copy_file_range**: not implemented (see [filesystem.md](filesystem.md#4-unsupported-operations)).

Explicit assumptions of the design:

* Changes made directly in the backing directory by other processes are not
  notified (no inotify on the backing tree); they are picked up by the next
  reconciliation pass, at most `scan_interval` later. Races with such processes
  are reduced by re-validation but not eliminated (residual TOCTOU, see
  [threat model](../security/threat-model.md)).
* Directories are never deleted by the GC; empty directories remain.
* The wall clock is used for expiry; sleeps use `steady_clock` and are capped at
  60 s so that a wall-clock step is noticed within a minute.
* The configuration file is trusted (the backing directory itself may be a
  symlink; nothing below it is followed). A root daemon only checks that the file is
  owned by root and not writable by group or others, not the directories leading to it.
* The FUSE session runs with `default_permissions`: access control is the
  kernel's evaluation of the mode bits and ownership reported by `getattr`.
