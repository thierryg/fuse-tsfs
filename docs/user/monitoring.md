<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/user/monitoring.md
Purpose : Status file, status commands, counters and log events to monitor
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Monitoring TSFS

TSFS exposes its state through two channels:

- a **status file**: a JSON document rewritten periodically, readable by any
  monitoring agent and printed by `tsfs --status`;
- the **log**: one structured event per line (text or JSON Lines), with a stable
  event identifier that alerting rules can match.

TSFS opens no network port and no IPC endpoint. Access control is the mode of the
status file and of the log directory.

Related documents: [configuration.md](configuration.md) (the `metrics` and
`logging` sections), [logging.md](../architecture/logging.md),
[garbage-collector.md](../architecture/garbage-collector.md).

## Contents

- [1. Status file](#1-status-file)
- [2. Reading the status](#2-reading-the-status)
- [3. Status fields](#3-status-fields)
- [4. Counters and gauges](#4-counters-and-gauges)
- [5. Log events](#5-log-events)
- [6. Events worth alerting on](#6-events-worth-alerting-on)
- [7. Suggested checks](#7-suggested-checks)

## 1. Status file

Configured in the `metrics` section:

```yaml
metrics:
  enabled: true
  status_file: "/run/tsfs/status.json"
  interval: "10s"
```

The daemon writes the file:

- every `metrics.interval` (default 10 s);
- immediately on `SIGUSR1`;
- once more during a clean shutdown, with the final counters.

Writes are atomic: the content goes to a temporary file
`<status_file>.tmp.XXXXXX` with an unpredictable name, created exclusively
(`mkostemp`, so an existing file or a link planted in the directory is never
opened), set to mode 0640 and then renamed over the status file, so a reader never
sees a partial document. Its directory is created with mode 0750 when missing (only
the last component; the parent must exist). The periodic write and a `SIGUSR1`
request are serialized: two writes never run at the same time. When the file
cannot be written, a `filesystem_operation_failed` warning (component `metrics`) is
logged, at most once every 10 minutes.

The file is not removed when the daemon stops: after a shutdown, it holds the last
values. Use `pid` and `updated_unix_ms` to tell a live daemon from a stale file (see
[section 7](#7-suggested-checks)).

## 2. Reading the status

### `tsfs --status`

```sh
sudo tsfs --status                      # default configuration /etc/tsfs/config.yml
sudo tsfs --status -c /etc/tsfs/other.yml
```

`--status` loads the configuration given with `-c` (it must be valid) to find
`metrics.status_file`, then prints that file. Pass the same configuration file as the
daemon. Exit status:

| Status | Meaning                                                                  |
|--------|--------------------------------------------------------------------------|
| 0      | the file was printed                                                     |
| 5      | the configuration is invalid                                             |
| 7      | no status file (`tsfs: no status file at ... (is the daemon running with metrics.enabled?)`) |

Because the file has mode 0640 and is owned by the account running the daemon
(root under the provided systemd unit), reading it requires that account or its
group.

### `SIGUSR1`

```sh
sudo kill -USR1 "$(systemctl show -p MainPID --value tsfs)"
```

On `SIGUSR1` the daemon rewrites the status file immediately and logs a summary:

```text
... INFO pid=12 tid=19 component=metrics event=status_requested deleted=1 scheduled=0 failed=0 managed_files=2 msg="status requested (SIGUSR1)"
```

Use it before reading the file when the values must be current to the second.

### Other tools

The status file is plain JSON:

```sh
jq '.gc | {deleted, failed, queue_saturations}' /run/tsfs/status.json
```

## 3. Status fields

Example, taken from a daemon that has just deleted one expired file:

```json
{
  "version": "0.1.0",
  "pid": 12,
  "uptime_s": 5,
  "updated_unix_ms": 1791880244291,
  "mountpoint": "/srv/tsfs",
  "backing_directory": "/var/lib/tsfs/data",
  "config": {"generation": 1, "reloads": 0, "reload_failures": 0},
  "gc": {"candidates": 1, "deleted": 1, "bytes_deleted": 4, "would_delete": 0, "vanished": 0, "skipped_modified": 0, "skipped_replaced": 0, "skipped_open": 0, "skipped_unsafe": 0, "failed": 0, "retries": 0, "queue_saturations": 0, "events_dropped": 0, "scans_completed": 2, "managed_files": 2, "last_scan_duration_ms": 0, "last_reconciliation_unix_ms": 1791880244290, "scan_time_ms": 0, "delete_time_us": 62, "worker_busy_us": 86, "scheduled": 0, "pending_work": 0, "workers": 2},
  "filesystem": {"operations": {"getattr": {"calls": 24, "errors": 6}, "readlink": {"calls": 0, "errors": 0}, "...": {}}, "bytes_read": 0, "bytes_written": 10, "open_handles": 0},
  "logging": {"queue_overruns": 0, "lost_writes": 0, "rotations": 0, "reconfigurations": 1, "notices_dropped": 0}
}
```

(The `"...": {}` entry stands for the other operations; the real file lists all of
them.)

### Top level

| Field               | Type    | Meaning                                                        |
|---------------------|---------|----------------------------------------------------------------|
| `version`           | string  | TSFS version of the running daemon                             |
| `pid`               | integer | process identifier of the daemon                               |
| `uptime_s`          | integer | seconds since the daemon started                               |
| `updated_unix_ms`   | integer | time of this write, milliseconds since the Unix epoch          |
| `mountpoint`        | string  | mount point in use                                             |
| `backing_directory` | string  | backing directory in use                                       |

### `config`

| Field             | Type    | Meaning                                                              |
|-------------------|---------|----------------------------------------------------------------------|
| `generation`      | integer | generation of the active configuration: 1 at startup, +1 per successful reload |
| `reloads`         | counter | successful reloads                                                   |
| `reload_failures` | counter | rejected reloads (the previous configuration stayed active)          |

### `gc`

| Field                         | Type    | Meaning |
|-------------------------------|---------|---------|
| `candidates`                  | counter | files inserted into the expiration scheduler because they expire within the scheduling horizon (2 x `scan_interval`) |
| `deleted`                     | counter | files deleted |
| `bytes_deleted`               | counter | bytes freed by deletions; a file with several hard links counts 0 bytes, since its data is still referenced |
| `would_delete`                | counter | dry-run validations that would have deleted a file; a file that stays in place is counted again at later passes |
| `vanished`                    | counter | candidates that no longer existed when the worker validated them |
| `skipped_modified`            | counter | candidates not deleted because they were no longer expired (modified since) or no longer subject to expiry (policy changed, excluded) |
| `skipped_replaced`            | counter | candidates not deleted because the path now names another file (different inode) |
| `skipped_open`                | counter | candidates not deleted because the file was open through the mount (`skip_open_files`); checked again later |
| `skipped_unsafe`              | counter | candidates not deleted because the path is no longer a regular file inside the managed tree (symbolic link or mount point in the path, other device, other file type) |
| `failed`                      | counter | deletions that failed permanently, or whose retries were exhausted |
| `retries`                     | counter | retries scheduled after transient failures |
| `queue_saturations`           | counter | candidates rejected because the scheduler was full (`max_pending_tasks`) |
| `events_dropped`              | counter | change notifications from the filesystem dropped because their queue was full; the next pass finds the files anyway |
| `scans_completed`             | counter | complete reconciliation passes |
| `managed_files`               | gauge   | regular files seen by the last complete pass |
| `last_scan_duration_ms`       | gauge   | wall-clock duration of the last complete pass, all slices included |
| `last_reconciliation_unix_ms` | gauge   | end time of the last complete pass, milliseconds since the Unix epoch (0 before the first one) |
| `scan_time_ms`                | counter | cumulative time spent scanning |
| `delete_time_us`              | counter | cumulative time spent validating and deleting candidates, in microseconds |
| `worker_busy_us`              | counter | cumulative time the workers spent processing batches, in microseconds |
| `scheduled`                   | gauge   | entries currently in the expiration scheduler |
| `pending_work`                | gauge   | entries handed to the workers and not yet processed |
| `workers`                     | gauge   | running worker threads |

### `filesystem`

| Field                       | Type    | Meaning |
|-----------------------------|---------|---------|
| `operations.<op>.calls`     | counter | FUSE requests of this type served |
| `operations.<op>.errors`    | counter | requests of this type that returned an error to the caller |
| `bytes_read`                | counter | bytes returned by `read` |
| `bytes_written`             | counter | bytes accepted by `write` |
| `open_handles`              | gauge   | files currently open through the mount |

`<op>` is one of: `getattr`, `readlink`, `mknod`, `mkdir`, `unlink`, `rmdir`,
`symlink`, `rename`, `link`, `chmod`, `chown`, `truncate`, `open`, `read`, `write`,
`statfs`, `flush`, `release`, `fsync`, `opendir`, `readdir`, `releasedir`,
`fsyncdir`, `access`, `create`, `utimens`, `fallocate`, `lseek`.

Errors include ordinary results such as `ENOENT` on `getattr` (a shell or an
editor probing for a file that does not exist), so a non-zero error count is normal.
Watch the trend, and the `filesystem_operation_failed` warnings for unexpected
errors.

### `logging`

| Field              | Type    | Meaning |
|--------------------|---------|---------|
| `queue_overruns`   | counter | messages dropped by a full asynchronous queue (`overrun_oldest` or `discard_new`) |
| `lost_writes`      | counter | messages the log file sink could not write |
| `rotations`        | counter | log rotations performed |
| `reconfigurations` | counter | times the logging pipeline was configured (1 at startup, +1 per reload that changed the `logging` section) |
| `notices_dropped`  | counter | internal notices of the log sink (rotation, compression, write errors) dropped because their queue was full |

## 4. Counters and gauges

- **Counters** only increase. They start at 0 when the daemon starts and are not
  persisted: a restart resets them. Compute rates from the difference between two
  readings, and treat a decrease (or a new `pid`) as a restart.
- **Gauges** are instantaneous values: `managed_files`, `last_scan_duration_ms`,
  `last_reconciliation_unix_ms`, `scheduled`, `pending_work`, `workers`,
  `open_handles`, and `config.generation`.

## 5. Log events

Every log line carries `component=` and `event=` fields. Events are stable
identifiers; messages (`msg=`) are for humans and may change. In the text format
the event is unquoted, so `grep 'event=gc_file_deleted'` works; in JSON use
`jq 'select(.event=="gc_file_deleted")'`.

| Event | Component | Level | Meaning |
|-------|-----------|-------|---------|
| `config_loaded` | config | info | configuration loaded at startup (`generation`, `rules`, `version`) |
| `config_reload_started` | config | info | a reload began (`trigger` = `sighup` or `inotify`) |
| `config_reload_succeeded` | config | info | new configuration active (`generation`, `rules`) |
| `config_reload_failed` | config | error | reload rejected (`error`), previous configuration still active |
| `config_restart_required` | config | warning | a restart-only setting changed (`setting`); the running value is kept |
| `filesystem_mount_started` | fs | info | mounting (written to stderr, before the log file is configured) |
| `filesystem_mounted` | fs | info | mount ready; also logged by the FUSE `init` callback (`openat2`, `read_only`) |
| `filesystem_unmounted` | fs | info | shutdown started (`loop_status`) |
| `filesystem_handles_released` | fs | info | at shutdown, file and directory handles still open (no RELEASE received) were freed (`files`, `directories`) |
| `filesystem_operation_failed` | fs, gc, metrics | trace to critical | an operation failed: trace for ordinary client errors, debug for refused path resolutions, warning for unexpected errors and scan errors, critical when signal handlers cannot be installed |
| `gc_started` | gc | info | garbage collector started (`enabled`, `dry_run`, `workers`, `scan_interval`, `scheduler`) |
| `gc_completed` | gc | info | garbage collector stopped (`deleted`, `bytes_deleted`, `failed`, `pending_abandoned`) |
| `gc_reconciliation_started` | gc | debug | a pass began |
| `gc_reconciliation_completed` | gc | info | a pass completed (`files`, `directories`, `symlinks`, `errors`, `boundaries`, `scheduled`, `duration_ms`) |
| `gc_candidate_found` | gc | debug / info | debug: expired file found; info with `dry_run=true`: dry run, would delete |
| `gc_file_deleted` | gc | info | file deleted (`path`, `bytes`, `links`, `rule`) |
| `gc_file_skipped` | gc | debug / warning | not deleted (`reason` = `vanished`, `not_expired`, `kept`, `replaced`, `open`); warning for `reason=unsafe` |
| `gc_retry_scheduled` | gc | debug | transient failure, retry scheduled (`attempt`, `errno`, `delay`) |
| `gc_delete_failed` | gc | warning | deletion failed permanently or retries exhausted (`errno`, `error`) |
| `gc_queue_saturated` | gc | warning | scheduler full (`capacity`) |
| `log_rotation_started` | logging | debug | a rotation of the active file began |
| `log_rotation_completed` | logging | info | rotation done |
| `log_repair_completed` | logging | warning | an interrupted compression was repaired when the log file was opened (startup or reconfiguration) |
| `log_rotation_failed` | logging | warning / error | a rotated file could not be renamed or removed, or the active file could not be rotated (logging continues in the current file) |
| `log_compression_started` | logging | debug | compression of a rotated file began |
| `log_compression_completed` | logging | debug | compression done |
| `log_compression_failed` | logging | warning | compression failed, or its queue was full (the file stays uncompressed) |
| `log_file_reopened` | logging | warning | the active log file was deleted or replaced by another program and was recreated or reopened |
| `log_queue_overflow` | logging | warning | messages dropped by the asynchronous queue (`dropped`, `policy`) |
| `log_write_failed` | logging | warning / error / critical | the log file cannot be written (error), writes recovered (warning), or the logging pipeline could not be set up at startup (critical) |
| `diagnostics_enabled` | diagnostics | info | crash reports armed (`directory`, `symbols` = number of function symbols loaded for the crash-time backtrace) |
| `diagnostics_unavailable` | diagnostics | warning | crash reports could not be armed (for example, crash directory not creatable) |
| `diagnostics_core_dumps` | diagnostics | info / warning | core-dump setup result (`rlimit_core`, `core_pattern`, `handler`; warning when core dumps are unavailable), then one info line per note |
| `status_requested` | metrics | info | `SIGUSR1` received (`deleted`, `scheduled`, `failed`, `managed_files`) |

On a fatal signal the crash handler cannot use the logger: it writes on stderr
(the journal under systemd) `tsfs: fatal signal <SIG>, crash report: <name>
event=diagnostic_report_started` (or `..., no crash report (cannot create it)
event=diagnostic_report_failed`), the symbolized backtrace after `tsfs: backtrace:`,
then `tsfs: crash report written: <name> event=diagnostic_report_completed`.

Repetitive warnings (the same error on many files) are rate-limited: at most 10
messages per key and per minute are written, and the next written message carries
`suppressed=<n>`, the number of similar messages skipped.

## 6. Events worth alerting on

| Condition | Match | Suggested action |
|-----------|-------|------------------|
| Configuration change rejected | `event=config_reload_failed` | fix the file (`tsfs --check-config`), reload again; until then the old policy is applied |
| Restart-only setting changed | `event=config_restart_required` | restart the service to apply it |
| Deletions failing | `event=gc_delete_failed`, or `gc.failed` increasing | check `errno`: permissions, read-only backing filesystem, I/O errors |
| Scheduler saturated | `event=gc_queue_saturated`, or `gc.queue_saturations` increasing | raise `max_pending_tasks`, raise `max_workers` or `max_deletions_per_second`, or lower `scan_interval` |
| Unsafe paths | `event=gc_file_skipped` with `reason="unsafe"` | something replaced a directory with a symbolic link or mounted a filesystem inside the backing tree |
| Unexpected filesystem errors | `event=filesystem_operation_failed` at `WARNING` or above | check the backing filesystem (`errno`, `op`, `path`) |
| Log loss | `event=log_queue_overflow`, `event=log_write_failed`, `logging.lost_writes` or `logging.queue_overruns` increasing | free space in the log directory, raise `logging.async.queue_size`, or use `overflow_policy: block` |
| Log rotation problems | `event=log_rotation_failed`, `event=log_compression_failed` | check the permissions and free space of the log directory |
| Crash reports not armed | `event=diagnostics_unavailable` | check `diagnostics.crash_directory` |
| Daemon crashed | `event=diagnostic_report_started` or `diagnostic_report_failed` in the journal, a new `tsfs-crash-*.txt` in `diagnostics.crash_directory`, or a restart reported by systemd | see the user guide, "Crash diagnostics" |
| Status file stale | `updated_unix_ms` older than 3 x `metrics.interval`, or `pid` not running | the daemon is stopped or hung |
| No progress | `gc.last_reconciliation_unix_ms` older than a few `scan_interval` while `gc` is enabled | scans are slow or blocked; compare `last_scan_duration_ms` with `scan_interval` |

## 7. Suggested checks

A minimal liveness check for a monitoring agent (adapt the path and thresholds):

```sh
#!/bin/sh
# Exit 0 when the TSFS daemon is alive and its status file is fresh, 2 otherwise.
status=/run/tsfs/status.json
max_age_ms=30000                      # 3 x metrics.interval (10s)
pid=$(jq -r .pid "$status") || exit 2
updated=$(jq -r .updated_unix_ms "$status") || exit 2
now=$(($(date +%s%3N)))
kill -0 "$pid" 2>/dev/null || { echo "tsfs: pid $pid not running"; exit 2; }
[ $((now - updated)) -le "$max_age_ms" ] || { echo "tsfs: status file stale"; exit 2; }
mountpoint -q "$(jq -r .mountpoint "$status")" || { echo "tsfs: not mounted"; exit 2; }
echo "tsfs: ok, deleted=$(jq .gc.deleted "$status") failed=$(jq .gc.failed "$status")"
```

Under systemd, `systemctl is-active tsfs` and `journalctl -u tsfs` complement the
status file (stderr goes to the journal; with `logging.console: true` every log
line goes there too).
