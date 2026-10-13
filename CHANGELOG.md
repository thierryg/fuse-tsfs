<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : CHANGELOG.md
Purpose : Release history (Keep a Changelog 1.1.0, Semantic Versioning 2.0.0)
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html).

The version has a single source, `VERSION`; CMake and the binary read it from there.

## [Unreleased]

### Added
- `tsfs --purge`: deletes now every file that is already expired, ignoring
  `max_deletions_per_second` and `max_deletions_per_cycle`, with the usual per-file
  validation (excluded and `never` files are never touched; backlogs larger than
  `max_pending_tasks` are purged by repeated passes; works with the GC disabled).
  Done by the running daemon, found through `metrics.status_file` and signaled with
  `SIGUSR2`; offline by the command itself when the mount point is not mounted;
  refused (exit 7) when it is mounted without a reachable daemon or when
  `garbage_collector.dry_run` is true. Asks for confirmation on a terminal
  (`--force` for scripts); `--purge --dry-run` previews. New log events
  `gc_purge_requested`, `gc_purge_started`, `gc_purge_completed`; new status fields
  `gc.purges_completed`, `gc.purging`, `gc.last_purge`.
- `SIGUSR2`: the daemon purges every expired file (same as `tsfs --purge`).
- `tsfs --stats`: one scan of the backing directory, per-rule table of files, size,
  expired, expiring within 24 hours, excluded, never and next expiry, plus a total.
- Read-only virtual extended attributes on the regular files of the mount:
  `user.tsfs.state` (`expires`, `expired`, `excluded`, `never`), `user.tsfs.expires`
  (ISO 8601 UTC), `user.tsfs.rule` and `user.tsfs.created` (birth time of the backing
  file in ISO 8601 UTC, else its modification time), read by name (`getfattr -n`); `listxattr`
  stays empty so that `ls` shows no `+`. New setting `filesystem.retention_xattrs`
  (default `true`, restart required) to remove the callbacks, whose cost is one
  `getxattr` request per write (about -28 % on small-file writes, measured). The
  status file counts `getxattr` and `listxattr` operations.
- `tsfs-ls` and its manual page `tsfs-ls(1)`: a wrapper around the system `ls` that
  tags the retention of each entry inside TSFS mounts. Every listing is produced by
  the original `ls`, so all its options keep their meaning. Outside TSFS mounts, when
  standard output is not a terminal, with `TSFS_LS=off` or with `--help`,
  `--version`, `--zero`, `--dired` or `-D`, the original `ls` runs with the unchanged
  command line. Inside a TSFS mount on a terminal (or with `TSFS_LS=always`), it runs
  with `-l --dired` appended after the user's options (before a `--`) and its dired trailer locates every entry, tagged
  `created <date>, filter <rule>, deleted <date> (in <remaining>)`,
  `created <date>, filter <rule>, deletion pending (expired <date>)` or `UNFILTERED`;
  long formats keep the lines of `ls`, other formats list one entry per line. No
  colors in TSFS listings. The original `ls` is `$TSFS_REAL_LS`, `/usr/bin/ls.orig`,
  then the `PATH`, always run as `argv[0]` = `ls` (multi-call binaries such as uutils
  coreutils). By default, a root installation (without `DESTDIR`) renames the system
  `/usr/bin/ls` to `/usr/bin/ls.orig` (a regular file or a symbolic link, kept as is)
  and installs the wrapper as `/usr/bin/ls`; a reinstallation refreshes the wrapper,
  or renames a system `ls` put back by a coreutils upgrade over the stale `ls.orig`;
  the `uninstall` target renames `ls.orig` back. CMake options
  `TSFS_REPLACE_SYSTEM_LS` (default `ON`) and `TSFS_SYSTEM_LS` (default
  `/usr/bin/ls`).
- Status file: `fuse` section with the kernel FUSE connection of the mount
  (`connection_id`, `control_dir` = `/sys/fs/fuse/connections/<id>`, and `waiting`,
  the requests not answered yet by the daemon); the `filesystem_mounted` event logs
  the control directory.
- systemd template unit `tsfs@.service`: one TSFS filesystem per configuration file
  `/etc/tsfs/NAME.yml`, with per-instance log and state directories.
- User guide: step-by-step creation of a TSFS filesystem and its mount point
  (backing storage, dedicated filesystem, configuration, validation, mounting,
  checks), several filesystems side by side, mounting at boot.

### Fixed
- CI: the `clang` job failed to link (Clang sanitizer runtimes missing from the
  development image: `libclang-rt-dev` added) and the `tidy` job failed on a
  configured-only tree (`@*.modmap` arguments in `compile_commands.json`: C++ module
  scanning disabled, the project uses no modules). `scripts/tsfs-qa.sh` now configures
  with `--fresh` so that a stale local cache cannot hide such differences. The
  workflow uses `actions/checkout@v5` and `actions/upload-artifact@v5`, and uploads the
  test logs of a failed job.
- A `SIGTERM` or `SIGHUP` received while the daemon was starting (before its signal
  handlers were installed) killed it with the default action and could leave a dead
  mount (`ENOTCONN`); the signals are now blocked from the very start and handled
  once the filesystem serves requests (a `SIGTERM` unmounts cleanly, exit 0). Found by
  ThreadSanitizer runs on 2 CPUs.
- A daemon whose mount point is already mounted (for example a second instance with
  the same `filesystem.mountpoint`) now stops with an explicit message (exit 7)
  instead of a misleading libfuse error, or of a mount stacked over the first one
  when running as root.
- File and directory handles still open when the daemon stops are freed (libfuse
  does not call release for them); found by LeakSanitizer.

## [0.1.0] - 2026-10-13

### Added
- FUSE passthrough filesystem (libfuse 3.14+) over a backing directory, with
  descriptor-relative path resolution (`openat2` `RESOLVE_BENEATH`,
  `RESOLVE_NO_SYMLINKS`, `RESOLVE_NO_MAGICLINKS`, `RESOLVE_NO_XDEV`; fallback walk
  with `O_NOFOLLOW` on kernels older than 5.6) and mount-boundary checks.
- Retention policy engine: file, directory (recursive or not), basename glob,
  path glob with `**`, extension and exclude rules; priorities; deterministic
  precedence; modification or birth time.
- Garbage collector: time-sliced reconciliation scans, bounded heap or ordered
  scheduler, worker pool, token-bucket rate limit, retries with exponential
  backoff, revalidation before every deletion, dry run.
- Hot configuration reload (inotify, `SIGHUP`) with atomic snapshot replacement;
  restart-only settings are reported, not applied.
- Structured logging (text or JSON Lines) with asynchronous pipeline, size-based
  rotation, retention count, optional compression and crash-recovery repair.
- Crash diagnostics: text crash reports with a symbolized backtrace (function
  names resolved at crash time from preloaded ELF symbol tables, also printed on
  stderr), `scripts/tsfs-symbolize.sh` for file:line, native core dumps.
- Status file and `tsfs --status`, `SIGUSR1`; CLI actions `--check-config`,
  `--show-config`, `--dry-run`, `--print-example-config`.
- CMake build with presets (release, relwithdebinfo, debug, asan, tsan, coverage,
  clang, ci) and targets `deps`, `build`, `tests`, `coverage`, `doc`, `bench`,
  `install`, `uninstall`; Debug builds use ASan + LeakSanitizer + UBSan by default.
- Test suite (GoogleTest, CTest labels), Google Benchmark suite and FUSE workload
  benchmark (`scripts/tsfs-bench-fs.sh`), gcov/lcov coverage, Doxygen documentation,
  systemd unit, man page, development container (`scripts/tsfs-dev.sh`).
