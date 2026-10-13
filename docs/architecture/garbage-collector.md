<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/garbage-collector.md
Purpose : Garbage collector design: discovery, bounded scheduling, validated deletion, retries, metrics
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Garbage collector

Sources: `src/gc/garbage_collector.{hpp,cpp}`, `src/gc/tree_scanner.{hpp,cpp}`,
`src/gc/scheduler.{hpp,cpp}`, `src/gc/token_bucket.hpp`,
`src/metrics/status_report.{hpp,cpp}`. Tests:
`tests/gc/test_garbage_collector.cpp`, `tests/gc/test_scheduler.cpp`.
Benchmarks: `benchmarks/bench_gc.cpp`, `benchmarks/bench_scheduler.cpp`.
Related: [policy engine](policy-engine.md), [filesystem](filesystem.md),
[metadata](metadata.md), [monitoring](../user/monitoring.md).

## 1. Design summary

Discovery is separated from deletion, and neither runs in a FUSE callback.

```
             +------------------ coordinator thread "tsfs-gc" ------------------+
 FUSE ------>| events_ (bounded deque) --drain_events()--+                       |
 notify      |                                           v                       |
             | TreeScanner --scan_slice(max_scan_duration)--> evaluate policy    |
             |                                           |  expiry <= now+horizon |
             |                                           v                        |
             |                          ExpirationScheduler (bounded, by path)    |
             |                                           | pop_due(now, room)     |
             +-------------------------------------------v------------------------+
                                           work_ (bounded deque, backpressure)
                                                         |  batch_size per take
                 +---------------------+-----------------+----------------------+
                 v                     v                                        v
          worker "tsfs-gc-w0"   worker "tsfs-gc-w1"   ...  (max_workers threads)
          TokenBucket.acquire -> lock path + ancestors -> re-resolve -> statx ->
          validate (identity, type, device, policy, expiry, open) -> unlinkat
```

* One **coordinator** thread runs time-sliced reconciliation passes, drains
  FUSE notifications, and moves due candidates to the work queue.
* `max_workers` **worker** threads validate and delete, throttled by a shared
  token bucket.
* Every structure in memory is bounded (section 9); the scheduler only holds
  files that expire within the **horizon**.

## 2. Configuration limits

All from `garbage_collector` (`GcConfig`, `src/config/config.hpp`; ranges from
`parse_gc`, `src/config/config_loader.cpp`). All are reloadable.

| Key | Default | Range | Role |
|---|---|---|---|
| `enabled` | `true` | | When false the coordinator sleeps until the next configuration change. |
| `dry_run` | `false` | | Validate everything, delete nothing (section 10). |
| `scan_interval` | `1m` | >= 1s | Time between the starts of two reconciliation passes; horizon = 2 x this. |
| `max_scan_duration` | `5s` | >= 10ms | Length of one scan slice. |
| `max_workers` | 2 | 1..64 | Worker threads. |
| `max_deletions_per_second` | 100 | 0..1000000 (0 = unlimited) | Token bucket rate: candidates examined (validated, then deleted or not) per second. |
| `max_deletions_per_cycle` | 0 (unlimited) | 0..10^9 | Candidates dispatched to the workers per scan cycle. |
| `max_pending_tasks` | 100000 | 16..10^7 | Scheduler capacity; also caps the work queue and the event queue. |
| `batch_size` | 64 | 1..10000, <= `max_pending_tasks` | Candidates a worker takes at once. |
| `retry_limit` | 5 | 0..100 | Retries of transient failures. |
| `retry_initial_delay` | `1s` | >= 10ms, <= `retry_max_delay` | Delay before the first retry (backoff `retry_initial_delay x 2^(attempt-1)`); also the recheck delay of open files. |
| `retry_max_delay` | `1m` | >= 10ms | Backoff cap. |
| `skip_open_files` | `true` | | Never delete a file open through the mount. |
| `scheduler` | `heap` | `heap`, `ordered` | Scheduler strategy. |

## 3. TreeScanner: time-sliced reconciliation

`TreeScanner` (`src/gc/tree_scanner.cpp`) walks the backing tree breadth-first
from `/`:

* Directories are opened with `BackingStore::open_directory` (no symlink, no
  mount crossing with `stay_on_filesystem`). `EXDEV`, `ELOOP`, `ENOTDIR` count
  as `boundaries`; `ENOENT` is ignored (directory removed meanwhile); other
  errors count as `errors` and are reported (rate-limited per errno).
* Entries: `DT_DIR` is queued; `DT_LNK` is counted and never followed;
  `DT_REG` and `DT_UNKNOWN` get a `statx(dirfd, name, AT_SYMLINK_NOFOLLOW)`
  (`stat_entry`); regular files are passed to the visitor, directories found by
  `statx` are queued, other types are counted as `others`.
* **Time slicing**: `run(deadline, ...)` checks `steady_clock` every 64 loop
  iterations (entries and directory openings alike) and returns `false` at the
  deadline. The open `DIR*` stream and the queue of pending directories are
  kept, so the next call resumes exactly where it stopped, even inside a huge
  directory. Memory is the queue of directories still to list, not the files.
* The coordinator runs one slice of `max_scan_duration`, then serves events and
  due entries, then runs the next slice immediately while a pass is in
  progress. A new pass starts when `now >= next_scan_` (the previous pass start
  plus `scan_interval`) or when a rescan is requested (startup, reload).
* At the end of a pass: `scans_completed`, `last_scan_files`,
  `last_scan_duration_ms`, `last_scan_completed_unix_ms` are updated and a
  `gc_reconciliation_completed` event is logged with the totals (files,
  directories, symlinks, errors, boundaries, scheduled).

## 4. Horizon and scheduling

`evaluate_and_schedule` evaluates the policy for each regular file seen by a
scan or a notification. A file enters the scheduler only if
`decision.kind == expires` and `expiry <= now + 2 x scan_interval` (the
**horizon**). Files that expire later are not kept in memory: a later pass
finds them again before they are due, because two passes always run within the
horizon. Memory therefore depends on the number of files expiring soon, not on
the number of files.

`schedule()` keeps one entry per managed path. A re-discovered file whose entry
has the same identity and either the same due time or a pending retry
(`attempt > 0`) is left untouched (no heap churn, retry backoff preserved);
otherwise the entry is replaced.

## 5. Bounded scheduler

`ExpirationScheduler` (`src/gc/scheduler.hpp`) is a Strategy interface
(`schedule`, `cancel`, `find`, `next_due`, `pop_due`, `size`, `capacity`,
`clear`), keyed by managed path, with a fixed capacity
(`max_pending_tasks`). A new path offered to a full scheduler is rejected
(`rejected_full`): `queue_saturations` is incremented, a rate-limited
`gc_queue_saturated` warning is logged, and the file is found again by a later
pass (`QueueSaturationIsBoundedAndRecoveredByLaterPasses`). The factory
`make_scheduler(kind, capacity)` selects the strategy. Access is serialized by
the GC mutex; the classes themselves are not thread-safe.

| | `HeapScheduler` (default, `heap`) | `OrderedScheduler` (`ordered`) |
|---|---|---|
| Structure | slots + `unordered_map<path, slot>` + binary min-heap of `(due, slot, generation)` | `std::map<(due, sequence), GcCandidate>` + `unordered_map<path, key>` |
| Update / cancel | bump the slot generation; older heap triples become tombstones (lazy invalidation) | erase and reinsert, exact |
| `schedule`, `cancel` | O(log n) amortized | O(log n) |
| `next_due` | O(1) amortized (tombstones dropped at the top) | O(1) |
| `pop_due` (k entries) | O(k log n) | O(k log n) |
| Memory | heap size bounded: compacted in O(n) when it exceeds `2 x live + 1024` | one tree node per entry |
| Trade-off | contiguous storage, lower constant factor | no tombstones, higher allocation cost |

The two are compared in `benchmarks/bench_scheduler.cpp` (1000 and 100000 live
entries, 10^6 with large benchmarks enabled). A timing wheel was not
implemented. Changing `scheduler` or `max_pending_tasks` by reload builds a new
empty scheduler; entries are not migrated, and the forced pass that every
reload triggers rebuilds the queue.

## 6. Validation before unlink

`process_candidate` (worker step) never deletes on the strength of the
scheduler record. Under `PathLockTable::lock_with_ancestors(candidate.path)`
(candidate and every ancestor directory), it performs, in order:

| # | Check | On failure |
|---|---|---|
| 1 | Re-resolve the parent with `BackingStore::open_parent` (openat2 beneath the root, no symlinks, no mount crossing) | errno classified (section 8) |
| 2 | `statx(parent, name, AT_SYMLINK_NOFOLLOW)` | errno classified; `ENOENT` -> `vanished` |
| 3 | Still a **regular file** | `unsafe` |
| 4 | With `stay_on_filesystem`: entry `st_dev` equals the backing root's device | `unsafe` |
| 5 | Re-evaluate the **current** policy snapshot with the fresh timestamps | |
| 6 | **Identity** `(dev, ino)` equals the one recorded at scheduling | `replaced` (if still expiring) or `kept`; the candidate takes the new identity |
| 7 | Decision is still `expires` (not excluded, not `never`) | `kept` |
| 8 | `expiry <= now` with the fresh timestamps | `not_expired`, rescheduled at the new expiry |
| 9 | With `skip_open_files`: identity not in `OpenFileRegistry` | `busy_open` |
| 10 | Dry run (configuration or one-shot) | `would_delete` |
| 11 | `unlinkat(parent_fd, name, 0)` | errno classified; success -> `deleted` |

Because the unlink is relative to the freshly resolved parent descriptor and
the name, and the stripes of the path and of its ancestors are held, a rename
of the file or of a parent directory through the mount cannot redirect the
unlink to another file. Changes made directly in the backing tree by other
processes are re-checked by steps 1-9 but are not excluded by the locks (see
[threat model](../security/threat-model.md)).

Hard links: only the expired name is removed; `bytes_deleted` is not increased
when `st_nlink > 1` (`HardLinkedFileOnlyLosesTheExpiredName`). Directories are
never removed. After a real deletion, `on_deleted(path)` invalidates the kernel
cache (outside all locks).

## 7. Work queue, backpressure and throttling

* `dispatch_due` moves due entries from the scheduler into `work_` only while
  there is room: capacity `min(max(64, batch_size x max_workers x 2),
  max_pending_tasks)`, further limited by the remaining per-cycle budget when
  `max_deletions_per_cycle != 0`. What does not fit stays in the scheduler.
  The cycle counter (`dispatched_in_cycle_`) resets when a new pass starts.
* Workers take up to `batch_size` candidates per wake-up, set `wake_` so that
  the coordinator refills the queue, and process them one at a time.
* **Token bucket** (`src/gc/token_bucket.hpp`): continuous refill at
  `max_deletions_per_second`, burst `max(1, rate / 10)` (at most 100 ms of
  deletions back to back); `acquire()` waits on a condition variable and returns
  `false` immediately on stop. A token is taken **before each candidate is
  processed**, whatever its outcome, so the setting bounds validations per
  second (an upper bound on deletions). Rate 0 disables throttling.
* **Event queue**: `notify_changed` appends to `events_` up to
  `min(max_pending_tasks, 65536)` entries; beyond that, events are dropped and
  counted (`events_dropped`). The coordinator drains at most 4096 events per
  iteration, without holding the GC mutex during `statx`.

### Coordinator sleep

The coordinator never polls. After a slice it waits on `coordinator_cv_` until
the earliest of: the next pass, the earliest due entry (only when the work
queue has room and the cycle budget is not spent), a notification, a worker
freeing room, a configuration change, or stop. The wait is capped at 60 s
(`kMaxSleep`) because due times are wall-clock values while the wait uses
`steady_clock`: a wall-clock step is noticed within a minute.

## 8. Outcomes and retries

`GcOutcome` (`src/gc/garbage_collector.hpp`) is the end state of one
processing attempt; `handle_outcome` decides what happens next.

```
                      +--> deleted        (counted; on_deleted)            [end]
                      +--> would_delete   (dry run; logged once per path)  [end]
                      +--> vanished       (ENOENT: idempotent success)     [end]
                      +--> kept           (now excluded / never)           [end]
 candidate --process--+--> unsafe         (not regular, other device,      [end]
                      |                    ELOOP/EXDEV/ENOTDIR)
                      +--> not_expired ---+  reschedule at new expiry if
                      +--> replaced ------+  within the horizon (attempt=0), else drop
                      +--> busy_open -------> reschedule now + retry_initial_delay
                      |                      (no attempt counted)
                      +--> retry ---------> attempt < retry_limit ?
                      |                       yes: attempt++, reschedule at now + backoff
                      |                       no : failed (counted, warning)       [end]
                      +--> failed         (permanent errno; counted, warning) [end]
```

Error classification (`fail_with`, `is_transient`):

| errno | Outcome |
|---|---|
| `ENOENT` | `vanished` |
| `ELOOP`, `EXDEV`, `ENOTDIR` | `unsafe` (the path now crosses a symlink or a mount point) |
| `EROFS`, `ENAMETOOLONG`, `EINVAL`, `EISDIR`, `EFAULT` | `failed` (permanent) |
| anything else (`EBUSY`, `EIO`, `EAGAIN`, `EINTR`, `ENOMEM`, `ETXTBSY`, `EACCES`, `EPERM`, `ENOSPC`...) | `retry` |

Backoff: attempt `n` waits `min(retry_initial_delay x 2^(n-1), retry_max_delay)`
(1s, 2s, 4s... with the defaults). After `retry_limit` retries the candidate is
dropped as `failed`; it is not retried forever, but the next reconciliation
pass rediscovers the file and starts a fresh series
(`PermissionFailureIsRetriedThenGivenUp`). Failure messages are rate-limited
per `(event, errno)` (10 per minute per key, with the suppressed count reported
in the next allowed message).

## 9. Bounded resources

| Structure | Bound |
|---|---|
| Scheduler | `max_pending_tasks` live entries (heap: plus at most `live + 1024` tombstones) |
| Work queue | `min(max(64, 2 x batch_size x max_workers), max_pending_tasks)` |
| Event queue | `min(max_pending_tasks, 65536)` |
| Scanner | pending-directory queue + one open directory stream |
| Dry-run log de-duplication set | cleared when it reaches 100000 paths |
| Threads | 1 coordinator + `max_workers` |

## 10. Dry run

* `garbage_collector.dry_run: true` runs the complete pipeline, including
  validation steps 1-9 under the path locks, and stops before `unlinkat`
  (`would_delete`). Each path is logged once (`gc_candidate_found`,
  `dry_run=true`) until the de-duplication set is reset by a reload or by
  reaching 100000 entries. Because nothing is deleted, an expired file is
  re-evaluated at every pass.
* `tsfs --dry-run` (`action_dry_run`, `src/app/application.cpp`) builds a
  standalone `GarbageCollector` (no mount, no threads, no open-file registry),
  runs one full pass synchronously (`run_full_scan`), then processes every due
  candidate with `force_dry_run` and prints a sorted table (expiry, size, rule,
  path) and a summary. Limits: candidates beyond `max_pending_tasks` are
  rejected by the scheduler (a `gc_queue_saturated` warning goes to stderr) and
  are missing from the report, and the summary then adds `dry run: INCOMPLETE: N
  expiring file(s) did not fit in garbage_collector.max_pending_tasks (X) and are
  not listed; raise it to see them all`. Files open through a running mount are
  not detected because the registry belongs to the daemon process (the `--help`
  text says "same checks as the real GC, except files open through a running
  mount, unknown here").

## 11. Counters (`GcStats`)

Relaxed atomics, written at deletion rate by a handful of threads and read by
`stats()`. Published in the status file under `"gc"` (field name in
parentheses when it differs).

| Counter | Meaning |
|---|---|
| `candidates` | New scheduler entries (inserted, not updated) |
| `deleted`, `bytes_deleted` | Real deletions; bytes only for files with one link |
| `would_delete` | Dry-run deletions |
| `vanished` | Candidates already gone |
| `skipped_modified` | `not_expired` + `kept` |
| `skipped_replaced`, `skipped_open`, `skipped_unsafe` | Corresponding outcomes |
| `failed`, `retries` | Permanent failures / exhausted retries; retries scheduled |
| `queue_saturations`, `events_dropped` | Rejected by a full scheduler; notifications dropped |
| `scans_completed`, `last_scan_files` (`managed_files`), `last_scan_duration_ms`, `last_scan_completed_unix_ms` (`last_reconciliation_unix_ms`) | Reconciliation |
| `scan_time_ms`, `delete_time_us`, `worker_busy_us` | Cumulative time scanning, validating+deleting, and in worker batches |
| `scheduled`, `pending_work`, `workers`, `config_generation` | Gauges (scheduler size, work queue + in flight, threads, snapshot generation) |

## 12. Metrics status file

`Daemon::write_status` (`src/app/application.cpp`) assembles `StatusInputs`
(version, pid, start time, mount point, backing directory, configuration
generation and reload counters, `GcStats`, `FsStats`, `LoggerStats`),
`render_status_json` renders one JSON document, and `write_file_atomically`
writes it to `metrics.status_file` (default `/run/tsfs/status.json`): the
directory is created (0750) if missing, the content goes to a temporary file
created by `mkostemp("<file>.tmp.XXXXXX")` (unpredictable name, `O_EXCL`), set to
mode 0640 with `fchmod`, then renamed over the target, so readers never see a
partial document. It is written by the housekeeping thread every
`metrics.interval` (default 10s), on SIGUSR1 by the watcher thread, and once at
shutdown, only when `metrics.enabled`; `Daemon::status_mutex_` serializes the
writers. `tsfs --status` prints the
file. No IPC endpoint is opened. Schema: [monitoring](../user/monitoring.md).

## 13. Startup and stale-metadata recovery

The GC has no persistent state. At start, `rescan_requested_` is true and
`next_scan_` is "now", so a full pass begins immediately and schedules every
file that is expired or expires within the horizon; expired files are deleted
as soon as they are validated (`RestartRecoversWithoutPersistentState`).
Because every deletion re-validates against the file system, a stale scheduler
entry (file modified, replaced, renamed, excluded by a reload, deleted by
someone else) can only lead to `not_expired`, `replaced`, `kept`, `unsafe` or
`vanished`, never to a wrong deletion. Each reload forces a new pass with the
new policy. See [metadata.md](metadata.md).

## 14. Shutdown

`stop()` requests stop on the coordinator and joins it, then stops the workers
(stop request + `work_cv_` broadcast + join). A worker finishes the candidate it
is processing; a worker waiting in `TokenBucket::acquire` returns at once. The
remaining work queue is cleared and, with the scheduler size, reported as
`pending_abandoned` in `gc_completed`; it is rediscovered by the first pass of
the next start (`ShutdownWithPendingTasksIsFast`).
