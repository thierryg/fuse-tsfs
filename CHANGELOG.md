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
- systemd template unit `tsfs@.service`: one TSFS filesystem per configuration file
  `/etc/tsfs/NAME.yml`, with per-instance log and state directories.
- User guide: step-by-step creation of a TSFS filesystem and its mount point
  (backing storage, dedicated filesystem, configuration, validation, mounting,
  checks), several filesystems side by side, mounting at boot.

### Fixed
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
