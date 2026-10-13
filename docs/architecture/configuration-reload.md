<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/configuration-reload.md
Purpose : Configuration snapshots and hot reload: triggers, validation, publication, failure handling
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Configuration and hot reload

Sources: `src/config/config.hpp`, `src/config/config_loader.{hpp,cpp}`,
`src/config/config_manager.{hpp,cpp}`, `src/config/config_watcher.{hpp,cpp}`,
`src/app/application.cpp`. Tests: `tests/config/test_reload.cpp`,
`tests/config/test_config.cpp`, `tests/filesystem/test_mount.cpp`
(`ReloadBySighupWithoutRemounting`, `InvalidReloadKeepsServing`). User
reference: [configuration](../user/configuration.md),
[user guide](../user/user-guide.md).

## 1. Snapshots

```
ConfigSnapshot {                                  published through
  shared_ptr<const Config>        config;        std::atomic<std::shared_ptr<const ConfigSnapshot>>
  shared_ptr<const PolicyEngine>  policy;        (ConfigManager::current_)
  uint64_t                        generation;    1 at startup, +1 per successful reload
}
```

* A snapshot is never modified after publication. `ConfigManager::current()`
  is a lock-free atomic load; a reader that keeps the returned `shared_ptr`
  keeps a consistent configuration and policy for as long as it needs them.
* The GC receives its own immutable `GcSnapshot` (GC section, policy,
  generation) through `GarbageCollector::apply`, stored in another
  `std::atomic<std::shared_ptr<const GcSnapshot>>`. Workers load it once per
  candidate, so a candidate is validated against one consistent policy.
* The logger keeps its own immutable `Pipeline` (see [logging](logging.md)).

## 2. Triggers

`ConfigWatcher` (`src/config/config_watcher.cpp`) owns one thread
(`tsfs-config`) that `poll()`s four descriptors: a stop eventfd, a reload
eventfd, a status eventfd and an inotify descriptor. Callbacks run on this
thread, one at a time.

| Trigger | Mechanism | Behavior |
|---|---|---|
| `SIGHUP` | `on_sighup` writes 1 to the reload eventfd (`write(2)` is async-signal-safe; errno is saved and restored) | Always reloads, even if the file did not change (trigger `"sighup"`); then records the file hash. |
| File change | inotify watch on the **directory** of the configuration file with `IN_CLOSE_WRITE`, `IN_MOVED_TO`, `IN_MOVED_FROM`, `IN_CREATE`, `IN_DELETE`, `IN_ATTRIB`, `IN_DELETE_SELF`, `IN_MOVE_SELF` | Debounced: 250 ms without new events, then the file content is hashed; reload (trigger `"inotify"`) only if the hash differs from the last loaded content and the file is readable. |
| `SIGUSR1` | `on_sigusr1` writes to the status eventfd | No reload: the watcher writes the status file and logs `status_requested` with a few counters. |

Watching the directory rather than the file survives atomic replacement
(editor `rename` over the file, Kubernetes-style `..data` symlink swap), which
changes the inode. Unrelated files in the same directory and no-op saves cause
no reload because of the content hash. If the directory itself disappears
(`IN_IGNORED`, `IN_DELETE_SELF`, `IN_MOVE_SELF`), the watch is re-established
every 2 s and a debounce is armed when it succeeds
(`WatchIsReestablishedWhenTheDirectoryIsRecreated`). If inotify is unavailable
the watcher still runs and SIGHUP still works; the `filesystem_mounted` event
reports `inotify=false`.

The inotify path updates the recorded hash even when the reload fails, so an
invalid file is not re-parsed in a loop; the next edit (or SIGHUP) retries.

## 3. Reload protocol (`ConfigManager::reload`)

All reloads are serialized by `reload_mutex_`.

1. Log `config_reload_started` (file, trigger).
2. `build()`: read the file (at most 4 MiB; the file trust check of
   `load_config_file()` applies, see [configuration](../user/configuration.md)), parse and validate the **complete**
   document (`parse_config`: unknown/duplicate keys, types, ranges, durations,
   sizes, paths, patterns, cross-checks such as "log directory outside the
   backing directory and the mount point"), apply the command-line overrides
   (`-m`/`-b` are then normalized and validated like file values by
   `validate_path_overrides()`), compile the policy. Nothing is changed yet.
3. Copy the new `Config` and restore every restart-only setting to its running
   value (`keep_static_settings`); each restored setting is logged as a
   warning `config_restart_required` with `setting=<key>` ("this setting
   requires a restart; the running value is kept").
4. Build the next snapshot with `generation = previous + 1`.
5. Call the observers in registration order with `(next, previous)`. The daemon
   registers two (`src/app/application.cpp`):
   * **logger**: if `next.logging != previous.logging`, `Logger::configure`
     builds a complete new pipeline; this is the only fallible step (log
     directory or file cannot be opened);
   * **GC**: `GarbageCollector::apply(make_gc_snapshot(next))`, never fails.
   If an observer returns an error, the reload is aborted and nothing is
   published.
6. Publish the snapshot atomically (`current_.store`), increment the success
   counter, log `config_reload_succeeded` (generation, active rules).

Any failure in steps 2 or 5 increments the failure counter, logs
`config_reload_failed` with the error ("reload rejected; the previous
configuration stays active") and leaves the previous snapshot active
(`FailedReloadKeepsThePreviousConfiguration`,
`ObserverFailureAbortsPublication`). The success and failure counters and the
current generation are exported in the status file (`"config"`).

```
 watcher thread                ConfigManager (reload_mutex_)          readers
 --------------                -----------------------------          -------
 SIGHUP / inotify --reload()--> build(): parse+validate+overrides       current() -> old snapshot
                                keep restart-only values
                                observer 1: Logger::configure  --fail--> abort, failures++, old stays
                                observer 2: GC apply
                                current_.store(next)  -------------->  current() -> new snapshot
```

## 4. Reloadable versus restart-only settings

Restart-only settings are exactly those listed in
`ConfigManager::keep_static_settings` (`src/config/config_manager.cpp`):

| Section | Restart-only keys |
|---|---|
| `filesystem` | `mountpoint`, `backing_directory`, `allow_other`, `read_only` (`mode` has a single allowed value) |
| `diagnostics` | `enabled`, `text_stack_trace`, `native_core_dump`, `crash_directory`, `max_crash_reports` |
| `security` | `stay_on_filesystem`, `require_openat2` |

Everything else takes effect after a successful reload:

| Section | Effect of a reload |
|---|---|
| `retention` | New compiled `PolicyEngine`. The GC clears its dry-run de-duplication set and starts a new reconciliation pass immediately; every worker validation already uses the new snapshot (`ExclusionAddedByReloadPreventsDeletion`). |
| `garbage_collector` | Token bucket rate updated at once; work-queue capacity recomputed; a change of `scheduler` or `max_pending_tasks` replaces the scheduler (rebuilt by the forced pass); a change of `max_workers` stops and restarts the workers (`WorkerCountFollowsReload`); `enabled`, `dry_run`, intervals, limits and retry settings apply from the next coordinator iteration or candidate. |
| `logging` | Any change rebuilds the pipeline (sink, format, level, async queue, rotation, compression) and swaps it atomically; see [logging](logging.md#7-reconfiguration). |
| `metrics` | Read from the current snapshot at each status write: `enabled`, `status_file`, `interval` apply from the next write. |

## 5. Command-line overrides

`ConfigOverrides` (`src/config/config_manager.hpp`) holds the values given on
the command line: `--mountpoint`, `--backing-dir`, `--log-level`, the console
flag set by `--debug`, and the diagnostics switches (`--stack-trace`,
`--no-stack-trace`, `--core-dump`, `--no-core-dump`). `ConfigManager` stores
them and `build()` applies them after parsing, at startup and on **every**
reload (path overrides are validated like file values; an error is reported as
`command line: ...` and exits with code 5 at startup), so they persist for the whole life of the process
(`CommandLineOverridesSurviveReloads`). `--debug` additionally forces a stderr
sink at every logger reconfiguration (`force_console`). One-shot actions
(`--check-config`, `--show-config`, `--dry-run`, `--status`) use the same
`build()` and therefore see the same effective configuration; `--debug`,
`--foreground`, `--fuse-debug` and the diagnostics switches are run-only and are
rejected with an action (exit code 2).

## 6. Concurrency notes

* `reload()` runs on the watcher thread; it holds `reload_mutex_` while calling
  the observers, which take the logger's `configure_mutex_` and the GC's
  mutexes. Observers must not call `reload()`.
* Readers never take `reload_mutex_`; FUSE threads do not read the
  configuration at all (the filesystem layer only depends on startup values).
* Reloads concurrent with readers are tested in
  `ConcurrentReloadsAndReaders`.

## 7. Assumptions and limits

* Only the logger observer can fail. If a later observer could fail, the logger
  would already run with the new settings while the old snapshot stayed
  published; the current observer list does not allow this case.
* The file is re-read completely on every reload; there is no partial update.
* A reload never remounts the filesystem; restart-only changes need a restart
  of the service.
