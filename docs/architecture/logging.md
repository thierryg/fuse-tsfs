<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/logging.md
Purpose : Logging subsystem: facade, async pipeline, rotation, compression, failure policy, formats
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Logging

Sources: `src/logging/*`, `src/common/escape.{hpp,cpp}`. Requirements:
[SPECIFICATION.md](../developer/SPECIFICATION.md) section 35. Tests:
`tests/logging/test_logging.cpp`; benchmarks: `benchmarks/bench_logging.cpp`.
User reference: [configuration](../user/configuration.md),
[monitoring](../user/monitoring.md). Crash reports are a separate subsystem:
[diagnostics](diagnostics.md).

## 1. Structure

```
 any thread                                          Logger (one per process, passed by reference)
 log(level, component, event, msg, fields)  ---->   level check (relaxed atomic) -> render_event()
                                                     pipeline_.load()  (atomic shared_ptr)
                                                          |
                         +--------------------------------+---------------------------+
                         | async.enabled                                              | sync
                         v                                                            v
              spdlog thread_pool (queue_size, 1 thread "tsfs-log")          spdlog::logger
              overflow: block | overrun_oldest | discard_new                        |
                         +--------------------------------+---------------------------+
                                                          v
                                  sinks: RotatingFileSink (logging.enabled)
                                         stderr_sink_mt   (console / --debug / errors when disabled)
                                                          |
                      rotation -> LogCompressor ("tsfs-logzip", bounded job queue)
                      sink/compressor events -> NoticeQueue -> Logger::maintenance() (housekeeping, 1 s)
                      last resort -> stderr_fallback() (write(2), rate-limited)
```

| Component | File | Role |
|---|---|---|
| `Logger` | `logger.{hpp,cpp}` | Facade: level filter, event rendering, pipeline construction and atomic swap, statistics. No global logger. |
| `LogField`, `render_event` | `log_field.{hpp,cpp}` | Typed fields (string, signed, unsigned, double, bool) and text/JSON rendering. |
| `RotatingFileSink` | `rotating_file_sink.{hpp,cpp}` | spdlog sink owning the active file and its rotation. |
| `LogCompressor` | `log_compressor.{hpp,cpp}` | Background gzip of rotated files. |
| `NoticeQueue`, `StderrFallback` | `notice_queue.{hpp,cpp}` | Out-of-band reports of the logging machinery; last-resort stderr. |
| `LogRateLimiter` | `rate_limiter.hpp` | Per-key rate limiting for repetitive failures (used by GC and FUSE). |

## 2. Levels and filtering

Levels: `trace`, `debug`, `info`, `warning`, `error`, `critical`. The minimum
level is an atomic in `Logger` (`set_level`, relaxed); `Logger::log` returns
after one comparison when the level is disabled, and callers guard expensive
debug/trace messages with `enabled(level)`. spdlog itself is set to `trace`;
filtering happens only in `Logger::log`.

Default level: `info`, or `debug` in Debug builds (`TSFS_DEFAULT_LOG_LEVEL`).
`--log-level` and `--debug` override the file for the process lifetime.
Successful operations are not logged per request; FUSE errors that are normal
client results are logged at `trace` only (see [filesystem](filesystem.md)).

## 3. Formats

Each event has a component (`gc`, `fs`, `config`, `logging`, `metrics`,
`diagnostics`), a stable event identifier, typed fields and an optional
message. `render_event` produces the variable part; the spdlog pattern adds the
timestamp (milliseconds and UTC offset), level, pid and tid.

Text (`logging.format: text`):

```
2026-10-13T10:15:30.123+02:00 INFO pid=1234 tid=1235 component=gc event=gc_file_deleted path="/a b.mp4" bytes=12 links=1 rule="video-files"
```

JSON Lines (`logging.format: json`), one object per line:

```
{"ts":"2026-10-13T10:15:30.123+02:00","level":"info","pid":1234,"tid":1235,"component":"gc","event":"gc_file_deleted","path":"/a b.mp4","bytes":12,"links":1,"rule":"video-files"}
```

* The text format prints the upper-case level name (`INFO`, `WARNING`, ...)
  through a custom `%U` pattern flag; JSON prints spdlog's lower-case name
  (`info`, `warning`, ...).
* Debug builds append `src="file.cpp:line"` (`std::source_location`).
* `msg` is emitted last and only when non-empty.

**Untrusted strings** (paths can contain anything) are always quoted and
escaped so that a crafted file name cannot forge a log line
(`src/common/escape.cpp`):

| Input | Text format | JSON format |
|---|---|---|
| `"`, `\` | `\"`, `\\` | `\"`, `\\` |
| newline, tab, CR | `\n`, `\t`, `\r` | `\n`, `\t`, `\r` (also `\b`, `\f`) |
| other control byte, DEL | `\xNN` | `\u00NN` |
| byte not part of valid UTF-8 (overlong, surrogate, > U+10FFFF, truncated) | `\xNN` | the six characters `\\xNN` (decodes to the text `\xNN`) |
| valid UTF-8 | copied as is | copied as is |

Non-finite doubles are written as `null` in JSON.

## 4. Asynchronous pipeline and overflow policies

With `logging.async.enabled: true` (default), `Logger::configure` creates an
spdlog `thread_pool` with `async.queue_size` slots (64..1048576, default 8192)
and one thread, initialized with `prepare_helper_thread("tsfs-log")`.

| `overflow_policy` | Behavior when the queue is full |
|---|---|
| `block` (default) | The producer waits for room; nothing is lost (`ConcurrentWritersWithRotationLoseNothingWithBlockPolicy`). A FUSE or GC thread can therefore be slowed by a saturated log device. |
| `overrun_oldest` | The oldest queued message is dropped. |
| `discard_new` | The new message is dropped. |

Losses are counted by spdlog (`overrun_counter + discard_counter`).
`Logger::maintenance()` (housekeeping thread, every second) compares the total
with the last reported value and, if it grew, writes one line through
`stderr_fallback()` and logs `log_queue_overflow` with the number dropped and
the policy (`DiscardNewPolicyCountsAndReportsLosses`). The cumulative count is
exported as `logging.queue_overruns` in the status file.

With `async.enabled: false` the caller formats and writes synchronously under
the sink mutex.

## 5. Size-based rotation (`RotatingFileSink`)

spdlog's own `rotating_file_sink` is not used: it names files `tsfs.1.log`,
cannot hand rotated files to a compressor and does not notice an externally
deleted active file. `RotatingFileSink` is the only rotation mechanism; no
external logrotate is required.

Layout with `filename: tsfs.log`, `max_files: N`:

```
tsfs.log        active file (mode 0640, O_APPEND|O_NOFOLLOW)
tsfs.log.1      most recent rotated file   (or tsfs.log.1.gz when compressed)
...
tsfs.log.N      oldest kept rotated file
```

At most `N` rotated files (1..1000, default 10) are kept; the active file is
not counted. The log directory is created with mode 0750, files with 0640.

**Algorithm** (`sink_it_`, called with the sink mutex held):

1. Format the message. Every `rotation.check_interval` (default 1 s) run the
   external-change check (section 8).
2. If rotation is enabled, a descriptor is open, the file is not empty, and
   `size + message > max_file_size`, and no failed rotation is waiting for its
   retry time: rotate **before** writing.
3. `write(2)` the message (no user-space buffer; `flush_` is a no-op); the
   tracked size grows by the bytes written.

**Rotation** (`rotate_locked`, under the sink mutex and then the rotation lock):

0. post `log_rotation_started` (debug level, before taking the rotation lock);
1. unlink `tsfs.log.N` and `tsfs.log.N.gz`;
2. for `i = N-1 .. 1`: rename `tsfs.log.i` -> `.i+1` and `tsfs.log.i.gz` ->
   `.i+1.gz` (missing files are skipped);
3. rename `tsfs.log` -> `tsfs.log.1`;
4. open a new `tsfs.log`;
5. outside the rotation lock: count the rotation, post `log_rotation_completed`,
   and, if compression is on, enqueue the rotated file's inode.

Size semantics: the size is tracked in memory (no `stat()` per message), so the
active file never exceeds `max_file_size` except when one message is larger than
the limit, or when another process appends to the file (the periodic check
re-reads the size). With the asynchronous pipeline there is one writer thread,
so queued messages are accounted exactly as they are written. Concurrent
rotations are impossible because rotation happens inside the sink mutex.

Failure handling: if renaming the active file fails, or the new file cannot be
opened, logging continues in the current file (which may now be
`tsfs.log.1`), `log_rotation_failed` is posted, one line goes to stderr, and the
rotation is retried after `check_interval` (no rotation storm). Failed renames
or unlinks of older files are posted as warnings and do not stop the rotation.

`rotation.enabled: false` disables rotation (the file grows without limit).
The max file size accepts 4 KiB .. 1 TiB.

## 6. Compression (`LogCompressor`)

Enabled by `rotation.compress: true` (default `false`), gzip level
`compression_level` 1..9 (default 6), with zlib. Never applied to the active
file. Each pipeline with compression owns one thread (`tsfs-logzip`) and a job
queue of 8 entries; when it is full the rotated file stays uncompressed
(`log_compression_failed` notice).

Per job, identified by the rotated file's **inode** (it may be shifted from
`.1` to `.2` by a later rotation meanwhile):

1. under the rotation lock, find `tsfs.log.K` with that inode (search stops
   after three consecutive missing indices) and open it; if it is gone (rotated
   out of retention), the job ends;
2. compress into `.tsfs.log.compress-<inode>.tmp` (mode 0640), `gzclose`,
   `fsync`;
3. **verify**: decompress the temporary file and compare length and CRC-32 with
   the source;
4. under the rotation lock, find the file again, `rename` the temporary file to
   `tsfs.log.K.gz`, then unlink `tsfs.log.K`.

The uncompressed file is removed only after its verified replacement exists. On
any failure (read error, disk full during `gzwrite`/`gzclose`, `fsync`,
verification mismatch, rename) the temporary file is removed and the original
is kept (`CompressionFailureKeepsTheOriginal`). Shutdown waits for queued jobs
(`Logger::shutdown` -> `wait_idle`).

## 7. Reconfiguration

`Logger::configure` (under `configure_mutex_`, never held by `log()`) builds a
complete new pipeline (compressor, file sink with leftover repair, optional
stderr sink, optional thread pool, formatter, error handler) and publishes it
with an atomic exchange only when everything succeeded. A failure (directory
not creatable, file not openable, pool allocation) returns the error and leaves
the previous pipeline active (`FailedReconfigurationKeepsThePreviousLogger`); in
a reload this aborts the whole reload (see
[configuration-reload.md](configuration-reload.md)). After the swap, the old
pipeline's counters are folded into the cumulative statistics, it is flushed,
and it is destroyed by the last thread still using it. Its destructor joins the
spdlog pool first, which drains the queued messages into the old sinks before
the sinks are destroyed (see [concurrency](concurrency.md#6-shutdown-ordering)),
so a directory or filename change does not lose queued messages
(`ReconfigureToNewDirectoryAndFilename`).

`logging.enabled: false` builds a pipeline with only a stderr sink restricted
to `error` and above, so failures are never silent
(`LoggingDisabledCreatesNoFile`). `logging.console: true` (or `--debug`) adds a
stderr sink at every level.

Before the first `configure()`, and after `shutdown()`, a synchronous stderr
**bootstrap** logger (text format) is active; it serves startup errors and the
one-shot actions.

## 8. External deletion and replacement

Every `check_interval`, on the next message written, the sink compares the
active file name with its open descriptor (`fstatat` of the name versus the
recorded inode):

* same inode: the tracked size is refreshed from the file (external truncation
  or appends are accounted);
* name missing (deleted externally) or different inode (replaced, e.g. by a
  misconfigured logrotate): a new active file is opened and
  `log_file_reopened` is posted (`ExternallyDeletedActiveFileIsRecreated`,
  `ExternallyReplacedActiveFileIsReopened`);
* reopening fails: `log_write_failed` is posted and writes continue on the old
  descriptor; if no descriptor is open, each check retries.

The check runs only when messages are written; an idle logger notices the
change with its next message.

## 9. Disk full and write failures

`write_all` never throws and never blocks on failure. When `write(2)` fails
(`ENOSPC`, `EIO`, ...):

* the message is counted as lost (`lost_writes` in the status file);
* the first failure of a streak posts one `log_write_failed` notice; further
  failures of the streak post nothing;
* every failure is reported through `stderr_fallback()`, which writes at most 5
  lines per 60 s and then one "N similar logging error message(s) suppressed"
  line when the window rolls over;
* the next message retries the write; the first success posts "log writes
  recovered after N lost message(s)".

Because notices are logged later by the housekeeping thread through the normal
path (and dropped and counted if the 256-entry notice queue is full), a full
disk cannot cause recursive logging or an unbounded loop
(`DiskFullIsCountedWithoutRecursionOrCrash`). Exceptions thrown inside spdlog
are routed to its error handler, which uses the same fallback.

## 10. Crash-time leftovers repair

`RotatingFileSink::create` calls `repair_leftovers` before opening the active
file (at startup and at each reconfiguration):

* temporary compression files `.tsfs.log.compress-*.tmp` are deleted (they are
  incomplete by definition);
* when both `tsfs.log.K` and `tsfs.log.K.gz` exist (crash between the rename of
  the verified archive and the unlink of the original), the `.gz` is kept if it
  is a complete, valid gzip stream (`gzip_stream_is_valid`, which also refuses
  non-gzip data read "transparently" by zlib), otherwise the `.gz` is removed
  and the original kept; a `log_repair_completed` warning notice is posted;
* rotated files with an index above `max_files` (after the limit was lowered)
  are removed;
* names that do not follow the scheme exactly (`tsfs.log.old`, leading zeros)
  are never touched (`RestartRepairsAnInterruptedRotation`).

## 11. Rate limiting of repetitive messages

`LogRateLimiter` lets at most 10 messages per key per minute through (defaults)
and returns the number suppressed since the previous allowed message, which the
caller logs as a `suppressed` field. Keys are short strings such as
`gc_delete_failed:13` or `fs:open:5`; the table is cleared at a window change
once it holds 1024 keys. Used for GC deletion failures, scan errors, unsafe
candidates, queue saturation, and unexpected FUSE errors.

## 12. Event identifiers

Emitted by the code (component in parentheses):

* configuration: `config_loaded`, `config_reload_started`,
  `config_reload_succeeded`, `config_reload_failed`,
  `config_restart_required` (config);
* filesystem: `filesystem_mount_started`, `filesystem_mounted`,
  `filesystem_unmounted`, `filesystem_operation_failed` (fs, gc, metrics);
* GC: `gc_started`, `gc_completed`, `gc_candidate_found`, `gc_file_deleted`,
  `gc_file_skipped`, `gc_delete_failed`, `gc_retry_scheduled`,
  `gc_queue_saturated`, `gc_reconciliation_started`,
  `gc_reconciliation_completed` (gc);
* logging: `log_rotation_started` (debug), `log_rotation_completed`,
  `log_rotation_failed`, `log_repair_completed`,
  `log_compression_started`, `log_compression_completed`,
  `log_compression_failed`, `log_queue_overflow`, `log_write_failed`,
  `log_file_reopened` (logging);
* diagnostics: `diagnostics_enabled`, `diagnostics_core_dumps`,
  `diagnostics_unavailable` (diagnostics);
* metrics: `status_requested` (metrics).

The crash handler cannot use the logger (it runs in a signal handler): it
writes `diagnostic_report_started`, `diagnostic_report_completed` and
`diagnostic_report_failed` on stderr (the journal under systemd), as
`event=<identifier>` at the end of its lines (see
[diagnostics](diagnostics.md#22-handler-fatal_handler)). The identifiers follow
spec section 35.7.

## 13. Bounded resources

| Resource | Bound |
|---|---|
| Async queue | `async.queue_size` messages |
| Log threads | 1 (`tsfs-log`) + 1 (`tsfs-logzip`, if compression) per pipeline |
| Compression queue | 8 jobs |
| Notice queue | 256 notices, overflow counted (`notices_dropped`) |
| Files on disk | active file + `max_files` rotated files (+ at most one temporary compression file per running job) |
| stderr fallback | 5 lines per 60 s + one summary line |
