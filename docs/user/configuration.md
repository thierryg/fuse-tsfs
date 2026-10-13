<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/user/configuration.md
Purpose : Complete reference of every configuration key (schema version 1)
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# TSFS configuration reference

This document lists every key of the TSFS configuration file (schema version 1),
with its type, default value, accepted values, and whether a running daemon applies
a change on reload. For task-oriented examples, see the
[user guide](user-guide.md). For the design of the policy engine and the reload
mechanism, see [policy-engine.md](../architecture/policy-engine.md) and
[configuration-reload.md](../architecture/configuration-reload.md).

Sources of truth: `src/config/config.hpp` (defaults), `src/config/config_loader.cpp`
(parsing and validation), `src/config/config_manager.cpp` (reload behavior),
`src/common/units.cpp` (units). The commented example `config/config.example.yml`
is compiled into the binary and printed by `tsfs --print-example-config`.

## Contents

- [1. General rules](#1-general-rules)
- [2. Value syntax](#2-value-syntax)
- [3. Reload behavior and command-line overrides](#3-reload-behavior-and-command-line-overrides)
- [4. `version`](#4-version)
- [5. `filesystem`](#5-filesystem)
- [6. `retention`](#6-retention)
- [7. `retention.rules[]`](#7-retentionrules)
- [8. `garbage_collector`](#8-garbage_collector)
- [9. `logging`](#9-logging)
- [10. `logging.async`](#10-loggingasync)
- [11. `logging.rotation`](#11-loggingrotation)
- [12. `diagnostics`](#12-diagnostics)
- [13. `metrics`](#13-metrics)
- [14. `security`](#14-security)
- [15. Cross-section constraints](#15-cross-section-constraints)
- [16. Effective defaults](#16-effective-defaults)

## 1. General rules

- The file is YAML. The default location is `/etc/tsfs/config.yml` (build option
  `TSFS_DEFAULT_CONFIG_PATH`); another file is selected with `-c`/`--config`.
- The file must not exceed 4 MiB.
- The file must be a regular file. A symbolic link is followed and its target is
  checked. When the daemon runs as root (effective uid 0), the file must also be
  owned by root and must not be writable by its group or by others; otherwise it is
  refused with exit status 5, for example:

  ```text
  tsfs: /etc/tsfs/config.yml: running as root, the configuration must not be writable by its group or by others (mode 0664; fix with chmod go-w)
  ```

  A file that anyone else can modify would let them choose what the root daemon
  deletes. An unprivileged daemon only requires a regular file. The check applies
  at startup, to every reload and to `--check-config`, `--show-config`,
  `--dry-run` and `--status`.
- `version` is the only mandatory key. Every other key and section is optional and
  falls back to the default listed here. A key written with an empty value
  (`key:` or `key: ~`) is treated as absent.
- Unknown keys are rejected, at every level. A typo never silently disables a
  setting. Duplicate keys are rejected as well.
- Every error message names the file, the line and column, and the key path, for
  example:

  ```text
  tsfs: /etc/tsfs/config.yml:6:18: rules[x].retention: invalid duration '1.5d': unknown unit '.' (use ms, s, m, h, d or w)
  ```

- Validate a file with `tsfs --check-config -c FILE` (exit status 0 when valid, 5
  when invalid). Print the effective configuration, defaults included, with
  `tsfs --show-config -c FILE`.

## 2. Value syntax

### Booleans

Exactly `true` or `false` (lower case). YAML 1.1 spellings such as `yes`, `on` or
`True` are rejected.

### Integers

Decimal integers without a sign prefix or a unit. Each integer key has an inclusive
range; a value outside the range is rejected with the range in the message.

### Durations

A duration is a sequence of `<number><unit>` groups with no space and no fraction.

| Unit | Meaning      |
|------|--------------|
| `w`  | week (7 days)|
| `d`  | day (24 h)   |
| `h`  | hour         |
| `m`  | minute       |
| `s`  | second       |
| `ms` | millisecond  |

Rules:

- Groups must appear from the largest to the smallest unit, each unit at most once:
  `1d12h` is valid, `12h1d` and `1h1h` are rejected.
- A number is mandatory before each unit and a unit is mandatory after each number:
  `90` and `h` are rejected.
- Fractions are not supported: write `1d12h`, not `1.5d`; `90m` or `1h30m`, not `1.5h`.
- Values that overflow a signed 64-bit count of milliseconds are rejected.
- Each duration key has its own minimum (see the tables below).
- `tsfs --show-config` prints durations in canonical form, largest units first:
  `90d` is shown as `12w6d`, `60s` as `1m`.

Examples: `500ms`, `30s`, `90s`, `30m`, `1h30m`, `12h`, `1d`, `7d`, `2w`, `1d12h`.

### Retention values

A retention value is either a duration of at least `1s`, or the word `never`
(the file is kept forever).

### Sizes

A size is a non-negative integer followed by an optional unit, with no space:

| Unit                   | Factor          |
|------------------------|-----------------|
| (none) or `B`          | 1 byte          |
| `KiB`, `MiB`, `GiB`, `TiB` | 1024, 1024^2, 1024^3, 1024^4 |
| `KB`, `MB`, `GB`, `TB` | 1000, 1000^2, 1000^3, 1000^4 |

Units are case-sensitive (`MiB`, not `mib`). Fractions are not supported. `--show-config`
prints sizes in the largest exact binary unit (`50MiB`), or in bytes (`50000000B`).

### Host paths

Keys that designate a location on the host (`filesystem.mountpoint`,
`filesystem.backing_directory`, `logging.directory`, `logging.file`,
`diagnostics.crash_directory`, `metrics.status_file`) must be absolute. Repeated
slashes are collapsed and a trailing slash is removed. `.` and `..` components are
rejected, not resolved. Environment variables and `~` are not expanded.

### Managed paths (retention rules)

Paths in retention rules (`file`, `path`, `path_glob`) are relative to the TSFS
root. They start with `/` and designate an entry *inside* the backing directory:
`/tmp/record` means `<backing_directory>/tmp/record`, seen by applications as
`<mountpoint>/tmp/record`. They never designate host paths. The same normalization
as for host paths applies (`.` and `..` rejected, `//` collapsed, trailing `/`
removed).

## 3. Reload behavior and command-line overrides

A running daemon re-reads the file on `SIGHUP` (`systemctl reload tsfs`) and when the
file content changes (inotify watch on the directory of the file, 250 ms debounce,
content hash comparison). A reload is atomic:

- The new file is parsed, validated and compiled completely before anything changes.
  If any step fails, the reload is rejected, `config_reload_failed` is logged at
  error level, and the previous configuration stays active.
- The new logging configuration is applied first. If it cannot be applied (for
  example a log directory that cannot be created), the whole reload is rejected.
- Settings marked **restart** in the tables below keep their running value. Each
  changed restart-only setting is reported by a warning
  (`event=config_restart_required setting=<key> msg="this setting requires a restart; the running value is kept"`),
  and the rest of the file is applied.
- Settings marked **reload** take effect immediately. After every successful reload
  the garbage collector starts a new reconciliation pass, so every existing file is
  re-evaluated against the new policy.

The restart-only settings are exactly: `filesystem.mountpoint`,
`filesystem.backing_directory`, `filesystem.allow_other`, `filesystem.read_only`,
`filesystem.retention_xattrs`, all keys of `diagnostics`, and all keys of `security` (`src/config/config_manager.cpp`,
`keep_static_settings`).

Command-line options override the file for the whole life of the process,
including after a reload:

| Option                | Overrides                                                 |
|-----------------------|-----------------------------------------------------------|
| `-m`, `--mountpoint`  | `filesystem.mountpoint`                                   |
| `-b`, `--backing-dir` | `filesystem.backing_directory`                            |
| `-l`, `--log-level`   | `logging.level`                                           |
| `-d`, `--debug`       | `logging.level` (`debug`, unless `-l` is given) and `logging.console` (`true`); also implies `--foreground` |
| `--stack-trace`       | `diagnostics.enabled: true`, `diagnostics.text_stack_trace: true` |
| `--no-stack-trace`    | `diagnostics.text_stack_trace: false`                     |
| `--core-dump`         | `diagnostics.enabled: true`, `diagnostics.native_core_dump: true` |
| `--no-core-dump`      | `diagnostics.native_core_dump: false`                     |

The paths given to `-m` and `-b` are normalized and validated like the values of
the file: they must be absolute and normalized, must not be `/`, must not contain
each other, and must not contain the log directory, the crash directory (when
diagnostics are enabled) or the status file (when metrics are enabled), as in
[section 15](#15-cross-section-constraints). An invalid value is refused with exit
status 5 and a message starting with `command line:`, for example
`tsfs: command line: the mount point and the backing directory must not contain each other`.
At startup, the daemon also checks that the mount point and the backing directory
exist.

`-f`, `-d`, `--fuse-debug` and the diagnostics options (`--stack-trace`,
`--no-stack-trace`, `--core-dump`, `--no-core-dump`) only apply when mounting:
combined with an action (`--check-config`, `--show-config`, `--dry-run`,
`--status`, `--stats`, `--purge`), they are rejected with exit status 2.

## 4. `version`

| Key       | Type    | Default | Allowed | Reload  |
|-----------|---------|---------|---------|---------|
| `version` | integer | none (mandatory) | `1` | n/a |

The schema version. A missing `version` is an error
(`missing configuration version (expected 'version: 1')`); any value other than 1 is
rejected (`unsupported configuration version`).

## 5. `filesystem`

| Key                 | Type      | Default              | Allowed                 | Reload  |
|---------------------|-----------|----------------------|-------------------------|---------|
| `mountpoint`        | host path | `/mnt/tsfs`          | absolute path, not `/`  | restart |
| `backing_directory` | host path | `/var/lib/tsfs/data` | absolute path, not `/`  | restart |
| `mode`              | string    | `passthrough`        | `passthrough`           | restart |
| `allow_other`       | boolean   | `false`              | `true`, `false`         | restart |
| `read_only`         | boolean   | `false`              | `true`, `false`         | restart |
| `retention_xattrs`  | boolean   | `true`               | `true`, `false`         | restart |

- `mountpoint`: directory where the filesystem is mounted. It must exist and be a
  directory when the daemon starts (TSFS does not create it).
- `backing_directory`: directory whose content is exposed at the mount point and
  where files are actually stored. It must exist when the daemon starts. The mount
  point and the backing directory must not contain each other.
- `mode`: exposure model. Only `passthrough` is implemented: every path under the
  mount point maps to the same path under the backing directory.
- `allow_other`: adds the FUSE `allow_other` mount option, letting users other than
  the one running the daemon access the mount. When the daemon does not run as root,
  `/etc/fuse.conf` must contain `user_allow_other`. Access is still checked by the
  kernel against file ownership and mode (`default_permissions` is always set).
- `read_only`: mounts the filesystem read-only (`ro`). The garbage collector still
  deletes expired files in the backing directory.
- `retention_xattrs`: serves four read-only virtual extended attributes on every
  regular file of the mount: `user.tsfs.state` (`expires`, `expired`, `excluded` or
  `never`), `user.tsfs.expires` (expiry in ISO 8601 UTC, only when the file
  expires), `user.tsfs.rule` (identifier of the deciding rule, or `default`) and
  `user.tsfs.created` (birth time of the backing file in ISO 8601 UTC, or its
  modification time when the backing filesystem does not report it).
  They are read by name (`getfattr -n user.tsfs.state FILE`, `getxattr(2)`) and
  are what the `tsfs-ls` wrapper (the system `ls` by default) displays; `listxattr` returns an empty list on purpose (see the
  user guide, "Retention of a file"). Cost: once the FUSE `getxattr` callback
  exists, the kernel sends one `getxattr` request (`security.capability`) before
  each write request; small-file workloads measured about 28 % fewer
  create-and-write operations per second (see
  [benchmark-report.md](../performance/benchmark-report.md)). `false` removes the
  `getxattr` and `listxattr` callbacks: no extra round trip, but no retention
  attributes, so `tsfs-ls` tags every entry `UNFILTERED`.

## 6. `retention`

| Key                    | Type             | Default        | Allowed                                             | Reload |
|------------------------|------------------|----------------|-----------------------------------------------------|--------|
| `default`              | retention        | `never`        | `never` or a duration >= `1s`                       | reload |
| `timestamp`            | string           | `modification` | `modification`, `last_modified`, `birth`, `creation` | reload |
| `evaluation_timestamp` | string           | (alias)        | same as `timestamp`                                 | reload |
| `rules`                | list of mappings | empty          | see [section 7](#7-retentionrules)                  | reload |

- `default`: lifetime of a regular file matched by no rule. The default `never`
  keeps unmatched files forever, which is the safe choice.
- `timestamp`: start of a file's lifetime.
  - `modification` (alias `last_modified`): `st_mtime`, the last content modification.
    Writing to a file, or setting its time with `touch`, restarts its lifetime.
  - `birth` (alias `creation`): the creation time reported by `statx()` (`btime`).
    When the backing filesystem does not report it, `st_mtime` is used instead.
- `evaluation_timestamp`: alias of `timestamp`. Writing both keys is an error.
- `rules`: ordered list of retention rules. Rule identifiers must be unique.

Expiration semantics: a file expires at `timestamp + retention`. It is eligible for
deletion when the current time is equal to or later than that instant. A timestamp
in the future postpones the expiry accordingly. A very large retention saturates at
the end of representable time instead of wrapping around.

## 7. `retention.rules[]`

Each rule is a mapping with the following keys:

| Key         | Type                       | Default   | Allowed                                                   |
|-------------|----------------------------|-----------|-----------------------------------------------------------|
| `id`        | string                     | mandatory | 1 to 64 characters among `A-Z a-z 0-9 . _ -`; unique     |
| `enabled`   | boolean                    | `true`    | `true`, `false`                                          |
| `action`    | string                     | `delete`  | `delete`, `exclude`                                      |
| `priority`  | integer                    | `0`       | -1000 to 1000                                            |
| `file`      | managed path               | none      | exact path of one file, not `/`                         |
| `path`      | managed path               | none      | directory scope                                          |
| `recursive` | boolean                    | `false`   | only together with `path`                                |
| `glob`      | string                     | none      | basename glob, no `/`, no `**`                           |
| `path_glob` | string                     | none      | whole-path glob, starts with `/`, `**` allowed           |
| `extension` | string or list of strings  | none      | extensions without wildcards, for example `mp4`, `tar.gz` |
| `retention` | retention                  | none      | `never` or a duration >= `1s`; mandatory for `delete`, forbidden for `exclude` |
| `timestamp` | string                     | `retention.timestamp` | as `retention.timestamp`; forbidden for `exclude` |

All rule keys are reloadable.

### Key details

- `id`: identifier used in log messages (`rule="..."`), in the dry-run report and for
  precedence ties.
- `enabled: false`: the rule is validated but ignored.
- `action`:
  - `delete` (the default): matching files are deleted once `retention` has elapsed.
    `retention: "never"` is allowed and keeps matching files forever (it shadows
    lower-precedence rules and the default).
  - `exclude`: matching files are never deleted, whatever any other rule or the
    default says. An exclude rule takes neither `retention` nor `timestamp`.
- `priority`: explicit precedence; higher wins. See below.
- `file`: matches exactly one managed path. Cannot be combined with `path`, `glob`,
  `path_glob` or `extension`.
- `path`: restricts the rule to the files *inside* a directory. With
  `recursive: false` (default), only files directly in that directory match. With
  `recursive: true`, files at any depth below it match. The directory itself is not
  a file and is never deleted.
- `glob`: matched against the file name (basename) only. Case-sensitive.
- `path_glob`: matched against the whole managed path, component by component.
  Cannot be combined with `path` (put the directory in the pattern). It must start
  with `/`, must not end with `/`, and must not contain empty, `.` or `..`
  components.
- `extension`: a single extension or a list. A leading dot is accepted and removed
  (`.mp4` and `mp4` are equivalent). Values are lower-cased; matching is
  case-insensitive (ASCII). Multi-dot extensions such as `tar.gz` are supported. The
  file name must have at least one character before the extension: `.mp4` alone does
  not match `extension: mp4`, `.hidden.mp4` does. An extension must not be empty,
  contain `/ * ? [ ] \`, or start or end with a dot after the optional leading dot is
  removed. An empty list is rejected.
- A rule needs at least one of `file`, `path`, `glob`, `path_glob`, `extension`.
  When several are given, a file must satisfy all of them (`path` + `glob`, `path` +
  `extension`, `path_glob` + `extension`, and so on).
- `timestamp`: overrides `retention.timestamp` for this rule.

### Glob syntax

| Token     | Matches                                                                 |
|-----------|-------------------------------------------------------------------------|
| `*`       | any sequence of characters inside one name, including an empty one; never `/` |
| `?`       | exactly one character (one complete UTF-8 sequence)                     |
| `[abc]`, `[a-z]` | one byte from the set or range                                  |
| `[!a-z]`, `[^a-z]` | one byte not in the set                                       |
| `\x`      | the character `x` literally (for example `\*`)                         |
| `**`      | in `path_glob` only, as a whole component: zero or more directories     |

A leading dot is not special: `*` matches `.hidden`, and `/tmp/*` matches
`/tmp/.lock`. Patterns are compiled when the configuration is loaded; an invalid
pattern (unterminated `[`, reversed range, trailing backslash, `/` in `glob`, `**`
inside a component) is a configuration error.

`path_glob` examples: `/rec/*.mp4` matches `/rec/a.mp4` but not `/rec/sub/a.mp4`;
`/rec/**/a.mp4` matches `/rec/a.mp4` and `/rec/x/y/a.mp4`; `/**/*.core` matches a
`.core` file at any depth.

### Precedence

For each regular file, TSFS evaluates:

1. Enabled `exclude` rules. If one matches, the file is never deleted. This holds
   even against a `delete` rule with a higher priority.
2. Enabled `delete` rules, in precedence order. The first one that matches decides
   the lifetime (which may be `never`).
3. Otherwise, `retention.default`.

Precedence among rules is decided by comparing, in this order (larger wins at the
first difference):

1. `priority`.
2. Scope kind: `file` > `path_glob` > `path` > no scope (`glob` or `extension` only).
3. Scope depth: number of components of the `file`, the `path`, or the literal
   directory prefix of the `path_glob` (`/var/log/*/x.log` has depth 2).
4. Non-recursive before recursive (for `path` rules; `file` rules count as
   non-recursive).
5. Presence of a name pattern (`glob` or `extension`).
6. Number of literal characters in the patterns (for an extension list, the shortest
   extension plus one for the dot).
7. Declaration order: the rule written first wins.

The [user guide](user-guide.md#rule-precedence-and-expiration-semantics) walks
through a complete example with its dry-run output.

## 8. `garbage_collector`

| Key                        | Type     | Default  | Allowed                       | Reload |
|----------------------------|----------|----------|-------------------------------|--------|
| `enabled`                  | boolean  | `true`   | `true`, `false`               | reload |
| `dry_run`                  | boolean  | `false`  | `true`, `false`               | reload |
| `scan_interval`            | duration | `1m`     | >= `1s`                       | reload |
| `max_scan_duration`        | duration | `5s`     | >= `10ms`                     | reload |
| `max_workers`              | integer  | `2`      | 1 to 64                       | reload |
| `max_deletions_per_second` | integer  | `100`    | 0 to 1000000 (0 = unlimited)  | reload |
| `max_deletions_per_cycle`  | integer  | `0`      | 0 to 1000000000 (0 = unlimited) | reload |
| `max_pending_tasks`        | integer  | `100000` | 16 to 10000000                | reload |
| `batch_size`               | integer  | `64`     | 1 to 10000, <= `max_pending_tasks` | reload |
| `retry_limit`              | integer  | `5`      | 0 to 100                      | reload |
| `retry_initial_delay`      | duration | `1s`     | >= `10ms`, <= `retry_max_delay` | reload |
| `retry_max_delay`          | duration | `1m`     | >= `10ms`                     | reload |
| `skip_open_files`          | boolean  | `true`   | `true`, `false`               | reload |
| `scheduler`                | string   | `heap`   | `heap`, `ordered`             | reload |

- `enabled`: when `false`, the garbage collector does nothing (no scan, no deletion)
  until it is re-enabled by a reload. The filesystem keeps working. An explicit
  purge (`tsfs --purge`, `SIGUSR2`) still runs: the workers are always started.
- `dry_run`: the garbage collector performs every check of a real deletion, then logs
  `event=gc_candidate_found ... dry_run=true msg="dry run: would delete"` once per
  file instead of deleting it, and increments the `would_delete` counter. The
  example configuration installed as `/etc/tsfs/config.yml` sets `dry_run: true`.
  It also applies to a purge: with `dry_run: true`, `tsfs --purge` refuses to run
  (exit status 7), and a purge requested with `SIGUSR2` deletes nothing.
- `scan_interval`: time between the starts of two reconciliation passes over the
  whole backing tree. Files whose expiry falls within twice this interval are
  scheduled individually and deleted at their expiry; later expiries are found again
  by later passes. Files written through the mount are also evaluated immediately
  through change notifications.
- `max_scan_duration`: a pass is split into time slices of at most this duration,
  so that event processing and due deletions are never blocked by a long scan.
- `max_workers`: number of deletion threads. A change restarts the workers.
- `max_deletions_per_second`: token-bucket throttle on the candidates examined per
  second: each candidate takes one token before it is validated, then deleted or not
  (skipped, kept). `0` disables it.
- `max_deletions_per_cycle`: maximum number of candidates dispatched to the workers
  per scan cycle. `0` means unlimited. Remaining candidates wait for the next
  cycle.

  Neither limit applies to an explicit purge (`tsfs --purge`, `SIGUSR2`), which
  deletes every expired file at once.
- `max_pending_tasks`: capacity of the expiration scheduler. When it is full, new
  candidates are rejected (counted in `queue_saturations`, logged as
  `gc_queue_saturated`) and found again by the next pass, so memory stays bounded.
  Changing it (or `scheduler`) rebuilds the scheduler, which a forced pass refills.
- `batch_size`: number of candidates a worker takes from the work queue at once.
- `retry_limit`: number of retries after a transient deletion failure (for example
  `EBUSY`, `EIO`, `EAGAIN`, `EACCES`, `EPERM`, `ENOSPC`). `0` disables retries. After
  the last retry the failure is counted and logged (`gc_delete_failed`); the next
  pass tries again. Permanent errors (`EROFS`, `ENAMETOOLONG`, `EINVAL`, `EISDIR`,
  `ENOTDIR`, `ELOOP`, `EXDEV`, `EFAULT`) are not retried.
- `retry_initial_delay`, `retry_max_delay`: exponential backoff of the retries:
  retry *n* waits `retry_initial_delay * 2^(n-1)`, capped at `retry_max_delay` (the
  first retry waits `retry_initial_delay`, the second twice that, and so on).
- `skip_open_files`: never delete a file that is currently open through the mount.
  The file is checked again after `retry_initial_delay`. Files opened directly in the
  backing directory (outside the mount) are not tracked.
- `scheduler`: data structure of the expiration scheduler. `heap` (binary min-heap
  with lazy invalidation) is the default; `ordered` (balanced tree with an index)
  cancels entries exactly. Both give the same results.

## 9. `logging`

| Key         | Type      | Default          | Allowed                                                    | Reload |
|-------------|-----------|------------------|------------------------------------------------------------|--------|
| `enabled`   | boolean   | `true`           | `true`, `false`                                            | reload |
| `directory` | host path | `/var/log/tsfs`  | absolute path                                              | reload |
| `filename`  | string    | `tsfs.log`       | plain file name, no `/`, not `.` or `..`, at most 200 bytes | reload |
| `file`      | host path | (none)           | absolute file path; alternative to `directory` + `filename` | reload |
| `level`     | string    | `info` (Release); `debug` in Debug builds | `trace`, `debug`, `info`, `warning`, `error`, `critical` | reload |
| `format`    | string    | `text`           | `text`, `json`                                             | reload |
| `console`   | boolean   | `false`          | `true`, `false`                                            | reload |
| `async`     | mapping   | see [section 10](#10-loggingasync) |                                         | reload |
| `rotation`  | mapping   | see [section 11](#11-loggingrotation) |                                      | reload |

- `enabled`: when `false`, no log file is written; errors are still written to
  stderr so that failures are never silent.
- `directory`, `filename`: location of the active log file. The directory is
  created (mode 0750) when missing; the file is created with mode 0640. Before
  mounting, the daemon checks that it can open the file and exits with status 7 if
  not. The directory must be outside the backing directory and the mount point.
- `file`: shortcut for `directory` + `filename` (`file: /var/log/tsfs/tsfs.log`).
  Writing `file` together with `directory` or `filename` is an error.
- `level`: minimum severity written. The built-in default depends on the build type:
  `info` for Release and RelWithDebInfo, `debug` for Debug builds. `-l` and `-d`
  override it.
- `format`: `text` produces one line per event:

  ```text
  2026-10-13T08:30:41.291+00:00 INFO pid=12 tid=17 component=gc event=gc_file_deleted path="/tmp/record/a.mp4" bytes=4 links=1 rule="temporary-recordings"
  ```

  `json` produces JSON Lines with the same fields:

  ```json
  {"ts":"2026-10-13T08:30:41.291+00:00","level":"info","pid":12,"tid":17,"component":"gc","event":"gc_file_deleted","path":"/tmp/record/a.mp4","bytes":4,"links":1,"rule":"temporary-recordings"}
  ```

- `console`: also write every message to stderr (captured by journald under
  systemd). `-d` forces it on.

## 10. `logging.async`

| Key               | Type    | Default | Allowed                                   | Reload |
|-------------------|---------|---------|-------------------------------------------|--------|
| `enabled`         | boolean | `true`  | `true`, `false`                           | reload |
| `queue_size`      | integer | `8192`  | 64 to 1048576 (messages)                  | reload |
| `overflow_policy` | string  | `block` | `block`, `overrun_oldest`, `discard_new`  | reload |

- `enabled`: messages are formatted by the caller and written by a dedicated
  logging thread. When `false`, the calling thread writes synchronously.
- `queue_size`: capacity of the queue between producers and the logging thread.
- `overflow_policy`: behavior when the queue is full.
  - `block`: the producer waits for room; no message is lost.
  - `overrun_oldest`: the oldest queued message is dropped.
  - `discard_new`: the new message is dropped.

  Dropped messages are counted (`logging.queue_overruns` in the status file),
  reported on stderr, and reported by a `log_queue_overflow` warning.

## 11. `logging.rotation`

| Key                 | Type     | Default | Allowed                       | Reload |
|---------------------|----------|---------|-------------------------------|--------|
| `enabled`           | boolean  | `true`  | `true`, `false`               | reload |
| `max_file_size`     | size     | `50MiB` | 4096 B to 1 TiB               | reload |
| `max_files`         | integer  | `10`    | 1 to 1000                     | reload |
| `check_interval`    | duration | `1s`    | >= `100ms`                    | reload |
| `compress`          | boolean  | `false` | `true`, `false`               | reload |
| `compression_level` | integer  | `6`     | 1 (fastest) to 9 (smallest)   | reload |

- `enabled`: size-based rotation. When `false`, the active file grows without limit.
- `max_file_size`: the active file is rotated before a write would make it exceed
  this size. Rotation renames `tsfs.log` to `tsfs.log.1`, shifting older files
  (`tsfs.log.1` to `tsfs.log.2`, ...).
- `max_files`: rotated files kept (`tsfs.log.1` to `tsfs.log.N`), the active file
  excluded. The oldest one is removed at rotation. When `max_files` is lowered,
  files above the new limit are removed when the log file is next opened (at
  startup or when a reload changes the logging configuration).
- `check_interval`: how often the sink checks whether the active file was deleted,
  replaced or truncated by another program (for example a misconfigured
  logrotate); TSFS then reopens or recreates it (`log_file_reopened`). It is also
  the delay before a failed rotation is retried.
- `compress`: gzip rotated files in a background thread (`tsfs.log.N.gz`). A crash
  during compression is repaired at the next start.
- `compression_level`: zlib compression level.

Do not rotate `tsfs.log` with an external tool such as logrotate: TSFS rotates its
own file.

## 12. `diagnostics`

| Key                 | Type      | Default               | Allowed          | Reload  |
|---------------------|-----------|-----------------------|------------------|---------|
| `enabled`           | boolean   | `false`               | `true`, `false`  | restart |
| `text_stack_trace`  | boolean   | `true`                | `true`, `false`  | restart |
| `native_core_dump`  | boolean   | `false`               | `true`, `false`  | restart |
| `crash_directory`   | host path | `/var/lib/tsfs/crash` | absolute path    | restart |
| `max_crash_reports` | integer   | `10`                  | 1 to 1000        | restart |

- `enabled`: master switch. Nothing below has an effect while it is `false`.
- `text_stack_trace`: on a fatal signal (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`,
  `SIGABRT`, `SIGSYS`, `SIGTRAP`), write `tsfs-crash-<unix time>-<pid>.txt` (mode
  0600) in `crash_directory`: signal, fault address, instruction pointer, raw
  backtrace, backtrace symbolized at crash time (`function+0xOFF (module)`, from the
  faulting frame; the function symbols of the loaded objects are read at startup),
  and `/proc/self/maps`. The symbolized backtrace is also printed on stderr. The
  process then terminates with the original signal. File and line numbers are
  resolved offline with `scripts/tsfs-symbolize.sh` and the same build.
- `native_core_dump`: raise the soft `RLIMIT_CORE` to the hard limit and mark the
  process dumpable, so that the kernel can write a core file. Where the core goes
  depends on `/proc/sys/kernel/core_pattern`; the daemon logs what it found.
- `crash_directory`: created (mode 0750) when missing. It must be outside the
  backing directory and the mount point when diagnostics are enabled.
- `max_crash_reports`: at startup, older text reports beyond this number are
  removed (only `tsfs-crash-*.txt` files; core files are never removed).

See [diagnostics.md](../architecture/diagnostics.md) and the user guide chapters
on crash diagnostics and core-dump analysis.

## 13. `metrics`

| Key           | Type      | Default                 | Allowed        | Reload |
|---------------|-----------|-------------------------|----------------|--------|
| `enabled`     | boolean   | `true`                  | `true`, `false`| reload |
| `status_file` | host path | `/run/tsfs/status.json` | absolute path  | reload |
| `interval`    | duration  | `10s`                   | >= `1s`        | reload |

- `enabled`: write the status file periodically, on `SIGUSR1`, when a purge starts
  or ends, and at shutdown. `tsfs --purge` needs it to find and follow the running
  daemon: with `enabled: false`, a purge of a mounted mount point is refused.
- `status_file`: JSON status document, written atomically (temporary file then
  rename): the temporary file is created with an unpredictable name and `O_EXCL`
  (`mkostemp`), set to mode 0640, then renamed over the status file. Its directory
  is created (mode 0750) when missing. It must be
  outside the backing directory and the mount point when metrics are enabled.
  `tsfs --status` prints it. The fields are documented in
  [monitoring.md](monitoring.md).
- `interval`: time between two writes. A new value takes effect after the next
  scheduled write.

## 14. `security`

| Key                  | Type    | Default | Allowed         | Reload  |
|----------------------|---------|---------|-----------------|---------|
| `stay_on_filesystem` | boolean | `true`  | `true`, `false` | restart |
| `require_openat2`    | boolean | `false` | `true`, `false` | restart |

- `stay_on_filesystem`: never cross a mount point located below the backing
  directory, neither when serving requests nor when scanning or deleting. A
  directory on another filesystem is counted as a boundary and skipped. Through the
  mount, an entry on another device than the backing directory fails with `EXDEV`,
  for intermediate path components and for the final component as well (`getattr`,
  `access`, `open`, `create` of an existing file, `truncate` by path, `chmod`,
  `chown`, `utimens`), so a file bind-mounted from another filesystem is
  unreachable. A bind mount of the **same** filesystem has the same device number:
  `openat2` (`RESOLVE_NO_XDEV`) refuses it as an intermediate component, but the
  fallback walk and the final-component check compare device numbers only and do not
  detect it. Combine with `require_openat2: true` where this matters.
- `require_openat2`: refuse to start when `openat2(2)` (Linux 5.6 and later) is
  unavailable (exit status 4). When `false`, an equivalent component-by-component
  resolution with `O_NOFOLLOW` is used on older kernels (it does not detect bind
  mounts of the same filesystem; see `stay_on_filesystem`). Recommended on kernels
  that provide `openat2`.

See the [threat model](../security/threat-model.md).

## 15. Cross-section constraints

The loader rejects the following combinations:

- `filesystem.mountpoint` or `filesystem.backing_directory` equal to `/`.
- A mount point inside the backing directory, or the reverse.
- `logging.directory` inside the backing directory or the mount point (logs would be
  subject to retention and would log their own writes).
- `diagnostics.crash_directory` inside the backing directory or the mount point,
  when `diagnostics.enabled` is `true`.
- `metrics.status_file` inside the backing directory or the mount point, when
  `metrics.enabled` is `true`.
- `garbage_collector.retry_initial_delay` greater than `retry_max_delay`.
- `garbage_collector.batch_size` greater than `max_pending_tasks`.
- `retention.timestamp` together with `retention.evaluation_timestamp`.
- `logging.file` together with `logging.directory` or `logging.filename`.

## 16. Effective defaults

`tsfs --show-config` with a file that contains only `version: 1` prints the
built-in defaults (Release build):

```yaml
version: 1
filesystem:
  mountpoint: "/mnt/tsfs"
  backing_directory: "/var/lib/tsfs/data"
  mode: passthrough
  allow_other: false
  read_only: false
  retention_xattrs: true
retention:
  default: "never"
  timestamp: modification
  rules:
    []
garbage_collector:
  enabled: true
  dry_run: false
  scan_interval: 1m
  max_scan_duration: 5s
  max_workers: 2
  max_deletions_per_second: 100
  max_deletions_per_cycle: 0
  max_pending_tasks: 100000
  batch_size: 64
  retry_limit: 5
  retry_initial_delay: 1s
  retry_max_delay: 1m
  skip_open_files: true
  scheduler: heap
logging:
  enabled: true
  directory: "/var/log/tsfs"
  filename: "tsfs.log"
  level: info
  format: text
  console: false
  async:
    enabled: true
    queue_size: 8192
    overflow_policy: block
  rotation:
    enabled: true
    max_file_size: 50MiB
    max_files: 10
    check_interval: 1s
    compress: false
    compression_level: 6
diagnostics:
  enabled: false
  text_stack_trace: true
  native_core_dump: false
  crash_directory: "/var/lib/tsfs/crash"
  max_crash_reports: 10
metrics:
  enabled: true
  status_file: "/run/tsfs/status.json"
  interval: 10s
security:
  stay_on_filesystem: true
  require_openat2: false
```

Note that the built-in default of `garbage_collector.dry_run` is `false`, while the
installed example configuration sets it to `true`.
