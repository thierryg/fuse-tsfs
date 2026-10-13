<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/concurrency.md
Purpose : Threading model, mutexes and lock order, atomics, signal handling, shutdown ordering
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Concurrency

Related: [overview](overview.md), [filesystem](filesystem.md),
[garbage collector](garbage-collector.md), [logging](logging.md),
[configuration reload](configuration-reload.md),
[test strategy](../testing/test-strategy.md) (TSan preset).

## 1. Threads

| Thread (name) | Created by | Count | Work |
|---|---|---|---|
| main | process | 1 | Startup, `fuse_loop_mt` (receives SIGINT/SIGTERM), shutdown. |
| libfuse workers | `fuse_loop_mt` (`FuseSession::loop`) | dynamic, 10 idle kept (`fuse_loop_cfg_set_idle_threads(10)`) | Execute `PassthroughFs` operations concurrently. |
| `tsfs-gc` | `GarbageCollector::start` | 1 | Coordinator: scan slices, event draining, dispatch; runs purges (`run_purge`). |
| `tsfs-gc-w<N>` | `GarbageCollector::start_workers` | `max_workers` (1..64) | Validate and delete candidates. |
| `tsfs-config` | `ConfigWatcher::start` | 1 | inotify/eventfd poll loop; runs reloads, SIGUSR1 status dumps and SIGUSR2 purge requests (`request_purge`, non-blocking). |
| `tsfs-housekeep` | `Daemon::run` (`std::jthread`) | 1 | Every second: `Logger::maintenance()`; every `metrics.interval`, and whenever a purge starts or ends: status file. |
| `tsfs-log` | spdlog `thread_pool` in `Logger::configure` | 1 per pipeline, only with `logging.async.enabled` | Formats and writes queued messages into the sinks. |
| `tsfs-logzip` | `LogCompressor` constructor | 1 per pipeline, only with `rotation.compress` | Compresses rotated files. |

Every TSFS helper thread starts with `prepare_helper_thread(name)`
(`src/common/threads.cpp`): block every asynchronous signal, set the thread name
(15 bytes max, visible in `top -H`, gdb, crash reports), install a 64 KiB
alternate signal stack (freed at thread exit) so that a stack overflow can still
be reported. All TSFS threads are `std::jthread`s joined by their owner;
blocking waits use `std::condition_variable_any` with a `std::stop_token`, so a
stop request wakes them immediately.

## 2. Mutexes

| Mutex | Guards | Held by |
|---|---|---|
| `ConfigManager::reload_mutex_` | observer list, reload sequence | watcher thread (reload), main (startup) |
| `Logger::configure_mutex_` | pipeline replacement | `configure()`, `shutdown()`; never by `log()` |
| `RotatingFileSink` base_sink mutex (spdlog `base_sink<std::mutex>`) | active fd, size, rotation state | the thread executing `sink_it_` (log thread, or the caller with synchronous logging) |
| `RotatingFileSink::rotation_lock_` (`shared_ptr<std::mutex>`) | renames in the log directory | sink during rotation; compressor while locating/renaming |
| `LogCompressor::mutex_` | job queue, `busy_` | sink (`enqueue`), compressor thread, `wait_idle` |
| `NoticeQueue::mutex_` | notice deque | sink, compressor (post); housekeeping (drain) |
| `StderrFallback::mutex_` | rate-limit window | any thread reporting a logging failure |
| `LogRateLimiter::mutex_` | per-key counters | GC, FUSE (`report_failure`) |
| `GarbageCollector::mutex_` | scheduler, work queue, event queue, `in_flight_`, `wake_`, flags (including `purge_requested_`), dry-run set | coordinator, workers, FUSE (`notify_changed`), reload (`apply`), `stats()` |
| `GarbageCollector::workers_mutex_` | worker thread vector | start/stop/apply, `stats()` |
| `TokenBucket::mutex_` | tokens and rate | workers, reload (`set_rate`) |
| `PathLockTable` stripes (1024) | namespace changes of a name | FUSE operations, GC workers |
| `OpenFileRegistry` shard mutexes (64) | per-identity open counts | FUSE `open`/`create`/`release`, GC validation |
| `PassthroughFs::DirHandle::mutex` | one directory stream | concurrent `readdir` on the same handle |
| `Daemon::status_mutex_` | `write_status()` (one status write at a time) | housekeeping thread (periodic and purge start/end), watcher thread (SIGUSR1), main (final status) |
| housekeeping local mutex | condition-variable wait only | housekeeping thread |

There is no recursive mutex and no reader/writer lock.

## 3. Lock order

Documented in the code:

* `src/gc/garbage_collector.hpp`: "Lock order: `mutex_` -> (nothing): path locks
  are only taken by workers while `mutex_` is NOT held."
* `src/logging/rotating_file_sink.hpp`: "sink mutex -> rotation lock (the
  compressor only takes the latter)."
* `src/fsutil/path_lock_table.hpp`: several stripes are always acquired in
  increasing index order, each at most once per holder.

Complete partial order derived from the code (an arrow means "may be held while
acquiring"):

```
 ConfigManager::reload_mutex_
   -> Logger::configure_mutex_ (logger observer)
   -> GarbageCollector::mutex_, TokenBucket::mutex_, workers_mutex_ (GC observer; taken one at a time)
   -> logging path (reload logs its own events)

 PathLockTable stripes (ascending index)
   -> OpenFileRegistry shard mutex        (FUSE open/create add; GC is_open)
   -> GarbageCollector::mutex_            (FUSE create/mknod/truncate/utimens/rename/link call
                                           notify_changed while holding their stripe)
   -> logging path                        (rare debug messages under a stripe)

 spdlog sink mutex (RotatingFileSink)
   -> rotation_lock_ -> NoticeQueue::mutex_
   -> LogCompressor::mutex_ (enqueue after a rotation)
   -> StderrFallback::mutex_

 LogCompressor (no own lock held) -> rotation_lock_ -> NoticeQueue::mutex_
```

* The GC never holds `mutex_` while taking a path stripe, logging, or doing
  I/O (`statx` in `drain_events` runs on a batch copied out of the lock). Path
  stripes are taken by workers only, and `mutex_` is re-acquired only after the
  stripes are released (dry-run bookkeeping, `in_flight_`).
* `on_deleted` (kernel cache invalidation) is called after the stripes are
  released.
* The sink and the compressor never call the `Logger` (that would re-enter the
  sink mutex or recurse into a full queue); they post `LogNotice`s that the
  housekeeping thread logs later. Their last-resort channel,
  `stderr_fallback()`, uses `write(2)` on fd 2 under its own leaf mutex.
* `GarbageCollector::apply` takes `mutex_`, releases it, then (if the worker
  count changed) joins the workers without holding any GC lock; workers never
  wait for `reload_mutex_`.

`PathLockTable.AncestorLocksDoNotDeadlockWithPairLocks` exercises the ordering
of ancestor sets against rename pairs.

## 4. Atomics

| Atomic | Order | Use |
|---|---|---|
| `std::atomic<std::shared_ptr<const ConfigSnapshot>>` (`ConfigManager::current_`) | seq_cst (default) | Lock-free publication of configuration snapshots. |
| `std::atomic<std::shared_ptr<const GcSnapshot>>` (`GarbageCollector::snapshot_`) | seq_cst | GC configuration + policy per generation. |
| `std::atomic<std::shared_ptr<Pipeline>>` (`Logger::pipeline_`) | seq_cst | Logger pipeline swap; `log()` loads it without a lock. |
| `Logger::level_` | relaxed | Level filter: a disabled level costs one load and one comparison. |
| GC `Counters` (25 counters) | relaxed (`purges_completed`: release increment) | Statistics only; `stats()` is not a consistent cut across counters. `run_purge` stores the `last_purge_*` values before incrementing `purges_completed`. |
| `GarbageCollector::purging_` | seq_cst stores, relaxed loads | Set by the coordinator for the duration of a purge; workers skip the token bucket while it is set; published as the `purging` gauge. |
| `ShardedCounters<N>` (`src/metrics/sharded_counter.hpp`) | relaxed | FUSE call/error/byte counters (see below). |
| `OpenFileRegistry::total_`, logger/sink/compressor counters, config reload counters | relaxed or seq_cst | Gauges and counters. |
| `GarbageCollector::running_` | seq_cst | Idempotent `start`/`stop`. |
| `Daemon::live_session_` (`std::atomic<FuseSession*>`) | seq_cst | Lets GC workers invalidate kernel caches only while the session is live. |
| `g_reload_fd`, `g_status_fd`, `g_purge_fd` (`config_watcher.cpp`) | relaxed loads in handlers | eventfd numbers read by the SIGHUP/SIGUSR1/SIGUSR2 handlers. |
| `g_installed`, `g_handling` (`crash_handler.cpp`) | seq_cst | Crash-handler installation; first crashing thread wins. |
| `g_symbols` (`std::atomic<const SymbolTable*>`, `crash_handler.cpp`) | seq_cst | Symbol table loaded at installation, read (never modified) by the signal handler. |
| `ConfigWatcher::watch_` (`std::atomic<int>`) | relaxed | inotify watch descriptor: written by the watcher thread, read by `inotify_active()` on the main thread. |
| `ManualClock::now_` | seq_cst | Test clock. |

**ShardedCounters**: 32 shards, each `alignas(64)` and holding an array of `N`
`std::atomic<uint64_t>`. A thread increments the shard selected once by a
`thread_local` hash of its id (relaxed); readers sum all shards. This avoids one
contended cache line shared by every FUSE thread (spec section 20). Reads are
not a consistent snapshot, which is acceptable for statistics.

## 5. Signal handling

Rules (`src/app/application.cpp`, `src/common/threads.cpp`,
`src/config/config_watcher.cpp`, `src/diagnostics/crash_handler.cpp`):

1. The very first statement of `Daemon::run`, before the configuration is
   loaded, the preflight checks, the mount and any helper thread, blocks `SIGINT`,
   `SIGTERM`, `SIGHUP`, `SIGUSR1`, `SIGUSR2`, `SIGPIPE` in the main thread. A
   signal received during startup therefore stays pending instead of taking its
   default action (which would kill the daemon after the mount and leave a dead
   mount behind, `ENOTCONN`); it is delivered when the loop starts, once the
   handlers are installed (a `SIGTERM` then unmounts cleanly, exit 0). TSFS helper threads
   inherit that mask and additionally block every signal except the
   synchronous faults (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`,
   `SIGTRAP`, `SIGSYS`) with `block_async_signals()`, so the crash handler
   still runs on a faulting helper thread.
2. The main thread unblocks the asynchronous set only around `fuse_loop_mt`.
   The masks of libfuse's own worker threads are set by libfuse, not by TSFS;
   the TSFS handlers are async-signal-safe whichever thread receives them.
3. Handlers do the minimum:
   * SIGINT/SIGTERM: libfuse's handler ends the loop.
   * SIGHUP/SIGUSR1/SIGUSR2: `on_sighup`/`on_sigusr1`/`on_sigusr2` save
     `errno`, load an eventfd number from an atomic, `write(2)` 8 bytes, restore
     `errno`. Installed with `SA_RESTART` after libfuse's handlers, so TSFS owns
     SIGHUP. All real work (reload, status, purge request) happens on the
     watcher thread; the purge itself runs on the GC coordinator.
   * Fatal signals: `fatal_handler` runs on the alternate stack (`SA_ONSTACK`)
     and uses only async-signal-safe calls (see [diagnostics](diagnostics.md)).
4. At shutdown the asynchronous set is blocked again before teardown and the
   SIGHUP/SIGUSR1/SIGUSR2 dispositions are restored to `SIG_DFL`.

## 6. Shutdown ordering

Producers of work are stopped before their consumers, and the mount is removed
only when no thread can use it any more:

```
 fuse_loop_mt returns (libfuse joins its workers)
   1. block async signals; SIGHUP/SIGUSR1/SIGUSR2 -> SIG_DFL
   2. ConfigWatcher::stop()          no more reloads or status dumps
   3. housekeeping: request_stop + join
   4. live_session_ = nullptr        GC stops calling fuse_invalidate_path
   5. GarbageCollector::stop()       join coordinator, then workers
   6. write_status()                 final status file
   7. session_.reset()               remove libfuse handlers, unmount, destroy
   8. CrashHandler::uninstall()
   9. Logger::shutdown()             drain notices, flush, wait for the compressor,
                                     swap to the bootstrap stderr logger
```

**Logger pipeline destruction (TSan-validated).** A replaced or shut-down
`Logger::Pipeline` must not destroy its sinks while the spdlog worker thread can
still write into them. `Logger::Pipeline::~Pipeline()` resets the
`thread_pool` first (`src/logging/logger.cpp`): the async logger only holds a
weak reference to the pool, so the pool's destructor drains the queue and joins
the `tsfs-log` thread; queued messages keep the logger (hence the sinks) alive
until they are written. The file sink and the compressor are destroyed after
that join. This ordering removed a data race reported by ThreadSanitizer
(`tsan` preset) between the log thread and sink destruction during
reconfiguration and shutdown.

## 7. Fixed concurrency defects

* **Status writes.** `Daemon::write_status` runs on the housekeeping thread
  (periodic) and on the watcher thread (SIGUSR1). It is serialized by
  `Daemon::status_mutex_`, and `write_file_atomically` creates its temporary file
  with `mkostemp` (unpredictable name, `O_EXCL`), so concurrent writers can no
  longer interleave in a shared temporary file or make a `rename` fail.
* **inotify state.** `ConfigWatcher::watch_` is a `std::atomic<int>`: the main
  thread reads it through `inotify_active()` (for the `filesystem_mounted`
  event) while the watcher thread may update it after a re-watch.
* **GC reconfiguration.** `GarbageCollector::apply()` moves the new snapshot into
  `snapshot_` (`exchange(std::move(snapshot))`) after copying the GC section it
  needs, instead of copying the `shared_ptr`.
* **Logger pipeline destruction**: see section 6.
* **Signal during startup.** The asynchronous signals used to be blocked too
  late in the startup sequence: a `SIGTERM` or `SIGHUP` arriving before the
  handlers were installed killed the daemon with the default action and, after
  the mount, left a dead mount (`ENOTCONN`). A pending `SIGHUP` is now handled
  as a reload once the loop runs. They are now blocked at the very start of `Daemon::run` (section
  5). Found by ThreadSanitizer runs on 2 CPUs (simulating CI); regression test
  `MountTest.SigtermDuringStartupUnmountsCleanly`, verified to fail without the
  fix.
* **Purge flags.** `purge_requested_` is a plain `bool` guarded by
  `GarbageCollector::mutex_` (set by `request_purge()` on the watcher thread,
  consumed with `std::exchange` by the coordinator); `purging_` is an atomic
  because workers read it without the mutex for every candidate.
