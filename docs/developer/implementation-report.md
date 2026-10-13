<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/implementation-report.md
Purpose : Final implementation report (specification section 33)
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Implementation report - TSFS 0.1.0 (2026-10-13)

This report answers section 33 of the [specification](SPECIFICATION.md). Every
result quoted here was produced on 2026-10-13 by the commands given; the final
quality gate run (`scripts/tsfs-qa.sh`, all steps) completed at 09:36 UTC.

## 1. Selected language and rationale

**C++26** (GCC 15.2 and Clang 21, libstdc++ 15), as required by the project owner,
using features of that version where they fit: pack indexing (FUSE trampolines),
`= delete("reason")`, `std::add_sat` (saturating expiry arithmetic),
`std::views::concat` (rule precedence scan), `#embed` (example configuration compiled
into the binary), plus C++23 `std::expected` / `std::print`. Rationale and the
alternatives considered: [language-selection.md](../architecture/language-selection.md).
The host (Ubuntu 22.04) has no C++26 toolchain: every build runs in the
`tsfs-dev:26.04` container through `scripts/tsfs-dev.sh`.

## 2. Final architecture

One process per TSFS filesystem: a high-level libfuse 3 passthrough over a backing
directory, a garbage collector (coordinator + workers), a configuration manager with
hot reload, a structured logger and a crash handler. Details:
[overview](../architecture/overview.md), [filesystem](../architecture/filesystem.md),
[policy engine](../architecture/policy-engine.md),
[garbage collector](../architecture/garbage-collector.md),
[configuration reload](../architecture/configuration-reload.md),
[concurrency](../architecture/concurrency.md), [metadata](../architecture/metadata.md),
[logging](../architecture/logging.md), [diagnostics](../architecture/diagnostics.md).

Code size: 10,225 lines in `src/` (C++ sources and headers), 4,559 lines of tests and
benchmarks.

## 3. Important design decisions

| Decision | Reason |
|---|---|
| Passthrough over a backing directory, no on-disk format | Data stays readable without TSFS; no recovery tool needed. |
| No persistent metadata index | The backing tree is the source of truth; a time-sliced reconciliation pass rebuilds the in-memory state at startup (157 ms per 100k files measured). |
| Bounded scheduler with a 2 x `scan_interval` horizon | Memory bounded regardless of the number of files; what does not fit is found by the next pass. |
| Deletion only after revalidation | Identity, type, device, open handles and the *current* policy are rechecked under the path locks immediately before `unlinkat`. |
| `openat2(RESOLVE_BENEATH | NO_SYMLINKS | NO_MAGICLINKS [| NO_XDEV])`, fallback walk | No path escape; mount boundaries enforced; final component checked separately. |
| Immutable snapshots swapped atomically on reload | Readers never lock; a failed reload keeps the previous configuration. |
| Crash-time symbolization from a preloaded symbol table | Function names in the report and on stderr without any allocation in the signal handler. |
| Debug builds sanitized by default (ASan, LSan, UBSan) | Leaks and memory errors fail the tests, including in the daemon spawned by the FUSE tests. |
| Heap scheduler by default | Measured 2.5-8x faster on schedule/cancel than the ordered set. |

## 4. Implementation status

### 4.1 Implemented and tested

- FUSE passthrough: create, read/write (buffer-based, splice capable), truncate,
  rename (incl. `RENAME_NOREPLACE`, `RENAME_EXCHANGE`), unlink, mkdir/rmdir, readdir
  of large directories, symlinks and hard links, chmod/chown/utimens, statfs,
  fsync, fallocate, read-only mounts, `allow_other`; device nodes refused.
- Ownership handover to the caller when running as root, on a verified descriptor
  (tested as root in the container: file, directory, symlink and FIFO).
- Mount boundaries (`EXDEV`) including a file bind-mounted from another filesystem
  (root-only test, run as root in the container).
- Policy engine: every rule type, exclusions, priorities, deterministic precedence,
  birth-time fallback, hidden files, `**` globs.
- Garbage collector: discovery, scheduling, workers, rate limit, retries, dry run,
  open-file protection, replaced-file detection, unsafe-entry refusal, queue
  saturation, reconciliation after restart, GC disable.
- Hot reload by inotify and `SIGHUP`, restart-only settings reported
  (`config_restart_required`), invalid reload keeps serving, CLI overrides kept.
- Configuration trust when running as root; `-m`/`-b` validated like the file.
- Logging: text and JSON, async pipeline, size rotation, retention count,
  compression, crash-recovery repair, overflow policies, disk full (`RLIMIT_FSIZE`),
  rate-limited errors, spec event identifiers.
- Crash diagnostics: report with symbolized backtrace (report and stderr) for
  SIGSEGV, SIGABRT, SIGFPE, stack overflow, crash in a helper thread;
  `scripts/tsfs-symbolize.sh` for file:line; report pruning.
- Status file, `tsfs --status`, `SIGUSR1`; CLI actions and exit codes 0/1/2/3/4/5/7.
- Mount preflight: `/dev/fuse` access, `allow_other` permission, mount point that is
  already mounted (second instance) refused.
- Handles left open at shutdown freed (regression test verified to fail without the fix).
- Several filesystems side by side (two instances verified manually).
- Kernel FUSE connection in the status file (`fuse.connection_id`, `control_dir`,
  `waiting`), read from `/proc/self/mountinfo` after mounting (`FuseControl.*`,
  `MountTest.StatusFileNamesTheFuseConnection`).
- Read-only retention extended attributes `user.tsfs.state`, `user.tsfs.expires`,
  `user.tsfs.rule`, `user.tsfs.created` (empty `listxattr`), `filesystem.retention_xattrs`
  (`MountTest.RetentionExtendedAttributes`); cost measured by an A/B benchmark
  (about -28 % on small-file writes, see the benchmark report).
- `tsfs --purge` and `SIGUSR2`: unthrottled purge of every expired file by the
  daemon, offline purge without a mount, refusals (mounted without a daemon,
  `dry_run`), confirmation and `--force`, preview (`GcTest.Purge*`,
  `MountTest.PurgeCommandDeletesExpiredFilesThroughTheDaemon`,
  `CommandLine.PurgeStatsAndPreview`, `ReloadTest.SighupBridgeAlwaysReloads` for the
  `SIGUSR2` bridge).
- `tsfs --stats` (per-rule table; exercised by the purge mount test).
- `tsfs-ls`, a wrapper around the original `ls` (`ls.orig`): outside TSFS mounts, in
  pipes and with `TSFS_LS=off` it runs `ls.orig` with the unchanged command line;
  inside a TSFS mount on a terminal it runs `ls.orig` with `-l --dired` appended after the user's options (before a `--`) and
  uses the dired byte offsets to append a retention tag to each entry
  (`created ..., filter ..., deleted ... (in ...)`, `deletion pending`,
  `UNFILTERED`), so every option of the system `ls` keeps its meaning
  (`MountTest.TsfsLsShowsRetentionOnATerminalAndDelegatesOtherwise`: tag format,
  `-R -I` through `ls.orig`, byte-identical output in a pipe). At installation the
  system `ls` is renamed `/usr/bin/ls.orig` (a symbolic link, such as the uutils
  coreutils one of Ubuntu 26.04, is renamed as is) and the wrapper installed as
  `/usr/bin/ls`; uninstallation renames it back. Verified as root in a throwaway
  container with uutils coreutils 0.8.0 (install, reinstall, uninstall restoring the
  identical link).
- Signals blocked from the start of the daemon: a `SIGTERM` during startup unmounts
  cleanly (`MountTest.SigtermDuringStartupUnmountsCleanly`, verified to fail without
  the fix; found by TSan runs on 2 CPUs).
- CMake targets `deps`, `build`, `tests`, `coverage`, `doc`, `bench`, `install`,
  `uninstall`; staged install/reinstall/uninstall and a real root install and
  uninstall in a throwaway container (configuration preserved, 13 files removed).

### 4.2 Implemented but not tested (or only partially verified)

- **systemd units** `tsfs.service` and `tsfs@.service`: syntax checked with
  `systemd-analyze verify`; never started under a real systemd (the container has
  none). The ordering drop-in (`RequiresMountsFor=`) is documented, not exercised.
- **Native core dumps**: `DiagnosticsTest.NativeCoreDumpWhenTheEnvironmentAllowsIt`
  is opt-in (`TSFS_TEST_ENABLE_CORE_DUMPS=1`) and was skipped in the final run.
- **Fallback path resolution on kernels < 5.6**: tested by forcing the fallback on a
  6.8 kernel, not on an old kernel.
- **CI workflow** (`.github/workflows/ci.yml`): it only calls `scripts/tsfs-qa.sh`,
  which was executed locally. Its first GitHub runs failed in the `clang` (missing
  Clang sanitizer runtimes) and `tidy` (`@*.modmap` arguments) jobs; both causes are
  fixed (`libclang-rt-dev` in the image, module scanning disabled, `--fresh`
  configuration, `checkout@v5`/`upload-artifact@v5`, test logs uploaded on failure),
  but the workflow has not been re-run on GitHub since.
- **Dedicated backing filesystem steps** of the user guide (LVM, `mkfs.ext4`,
  fstab line for the backing storage): standard administration commands, not run
  (no block device in the container).

### 4.3 Partially implemented

- **`--dry-run` completeness**: bounded by `max_pending_tasks`; when it saturates the
  report says `INCOMPLETE` instead of listing every file.
- **FUSE performance**: functional baseline only (see section 7); no kernel FUSE
  passthrough, no writeback cache.
- **`tsfs-ls`**: colors are not shown in TSFS listings (`ls.orig` writes to a pipe),
  and non-long formats become one entry per line followed by its tag (a column
  layout cannot carry it).
- **Symbolization of stripped libraries**: internal libc/libfuse frames show
  `?? (module+offset)`.

### 4.4 Not implemented (by design or out of scope)

- Persistent metadata index (not needed: see section 3).
- Administrative IPC beyond the status file and signals (no concrete requirement;
  the only deletion command, the purge, is a signal: `SIGUSR2`).
- `/etc/fstab` / `mount -t fuse.tsfs` helper interface (systemd units instead).
- Extended attributes other than the four read-only retention attributes (no
  `setxattr`/`removexattr`, backing attributes not exposed), ioctl, poll; propagation of file locks to the backing
  filesystem; `copy_file_range` offload (the kernel falls back to read/write) (listed in [filesystem.md](../architecture/filesystem.md#4-unsupported-operations)).
- Cold-cache benchmarks and the 1,000,000-file GC datasets (opt-in, not run).

### 4.5 Blocked by the environment

- No C++26 toolchain on the host: all work happens in the container.
- ThreadSanitizer needs ASLR disabled (`vm.mmap_rnd_bits=32` on this kernel) and
  Docker's seccomp profile blocks it: solved with `scripts/tsfs-dev.sh --no-aslr`.
- No `perf` in the image: the worker-scaling finding (section 7) is not profiled.
- The machine is a busy desktop (load average 5-10): benchmark values are noisy.

## 5. Build results

Final gate run of 2026-10-13 (12:08-12:15 UTC) on a clean copy of the repository,
exactly as the CI does (`scripts/tsfs-qa.sh`, every step), plus ThreadSanitizer on a
container limited to 2 CPUs, repeated 3 times (the condition that exposed the
startup signal race seen in CI).

| Tree | Compiler / flags | Result |
|---|---|---|
| `ci` | GCC 15.2, Release, `-Werror` | builds without warnings, 206 tests, 0 failed |
| `debug` | GCC 15.2, Debug, ASan + LSan + UBSan | 206 tests: 204 passed, 2 skipped, 0 failed |
| `clang` | Clang 21, Debug (sanitized), `-Werror` | builds without warnings, 204 passed, 2 skipped |
| `tsan` | GCC 15.2, Debug, ThreadSanitizer | 204 passed, 2 skipped, no TSan report (also 3 x on 2 CPUs) |
| `coverage` | GCC 15.2, Debug, `--coverage` | 204 passed, 2 skipped |
| `release` | GCC 15.2, Release | builds, all tests and benchmarks run |

The two skipped tests: the opt-in native core dump test and the root-only bind
mount test (both executed separately: see 4.1 and 4.2). Static analysis:
clang-format clean, clang-tidy clean on `src/` and `benchmarks/` (each remaining
analyzer false positive suppressed at its line with a reason), shellcheck clean on
`scripts/`. Dependency check (`deps`, every profile): all present in the image.

## 6. Test results and coverage

206 test cases in 11 executables (labels: unit 94, policy 27, config 34, reload 10,
gc 52, concurrency 108, logging 20, diagnostics 10, security 70, filesystem 26,
stress 2). Strategy and per-suite content:
[test-strategy.md](../testing/test-strategy.md).

Coverage (gcov/lcov, `coverage` preset, production sources only):

| Metric | Covered |
|---|---|
| Lines | **80.0 %** (4,355 / 5,441) |
| Functions | **92.5 %** (538 / 581) |
| Branches | **51.2 %** (4,243 / 8,276) |

Least covered: `src/diagnostics/crash_handler.cpp` (the handler runs in processes
that crash, which never write their gcov counters), `src/app/application.cpp` (the
daemon runs in subprocesses; root-only and privilege-error paths are not reachable
as an unprivileged user). Report: `build/coverage/coverage/html/index.html`,
`summary.json`, `uncovered.txt`.

Defects found and fixed during validation (each with a regression test or a
sanitizer run that covers it): Logger pipeline teardown order (TSan), UBSan
intercepting the crash helper, build-speed-dependent stress assertion, CTest labels
and environment lost by `gtest_discover_tests`, ownership handover by name (race),
`create` taking ownership of a pre-existing file, final path component crossing a
mount, daemon umask applied twice, status file temporary name and concurrent
writers, `ConfigWatcher::watch_` data race, unvalidated `-m`/`-b`, `-d` accepted with
actions, `deps` false negatives, handles leaked at shutdown (LSan), stacked mount on
an already mounted mount point, `SIGTERM` during startup leaving a dead mount
(TSan runs on 2 CPUs), CI image without the Clang sanitizer runtimes, clang-tidy
failing on `@*.modmap` arguments of a configured-only tree.

## 7. Benchmark results (synthetic, warm cache)

Full report: [benchmark-report.md](../performance/benchmark-report.md). Highlights:

- Policy evaluation: 126 ns per file with the 8 example rules.
- Scheduler (heap) at 1,000,000 entries: 1.30 M inserts/s, 3.24 M reschedules/s.
- Discovery: 0.5-0.6 M files/s; single-threaded deletion with revalidation:
  59-74 k files/s (ext4 on NVMe).
- More GC workers were slower on clustered directories (66 k/s with 1 worker,
  45-50 k/s with 4-8): a measured finding, not yet profiled.
- FUSE overhead versus the raw backing directory: metadata operations at 6-17 % of
  native speed, streaming at 26-31 %.

## 8. Documentation

- User: [user-guide.md](../user/user-guide.md) (24 chapters, including the
  step-by-step creation of a filesystem and its mount point),
  [configuration.md](../user/configuration.md), [monitoring.md](../user/monitoring.md),
  man pages `docs/man/tsfs.1` and `docs/man/tsfs-ls.1`.
- Developer: [building](building.md), [coding style](coding-style.md),
  [testing](testing.md), [debugging](debugging.md), [coverage](coverage.md),
  [performance](performance.md), [contributing](contributing.md),
  [repository conventions](repository-conventions.md).
- Security: [threat-model.md](../security/threat-model.md), `SECURITY.md`.
- API (Doxygen with Graphviz diagrams): `cmake --build build/release --target doc`
  -> `build/release/docs/html/index.html`.

## 9. Installation

```sh
scripts/tsfs-dev.sh cmake --preset release
scripts/tsfs-dev.sh cmake --build build/release
sudo cmake --install build/release      # on a host with the runtime libraries
sudoedit /etc/tsfs/config.yml            # or /etc/tsfs/NAME.yml for tsfs@NAME
sudo systemctl daemon-reload && sudo systemctl enable --now tsfs.service
```

Uninstall: `sudo cmake --build build/release --target uninstall` (manifest based;
configuration, data, logs and state are kept). See the user guide, chapters 4, 7,
11 and 22.

## 10. Known limitations

Listed in the user guide (chapter 24) and the threat model (residual risks). The
main ones: FUSE overhead; files written directly into the backing directory are
only seen by the next reconciliation pass and are not protected while open there;
expiry follows the wall clock; same-filesystem bind mounts are only detected with
`openat2` for intermediate components; `read_only: true` does not stop the GC;
libfuse leaves memory behind when a session ends with queued requests (process
exit, not a TSFS leak).

## 11. Remaining work

1. Start both systemd units under a real systemd (boot, reload, stop, instance
   ordering with `RequiresMountsFor=`).
2. Profile the GC worker scaling (directory lock contention hypothesis) and the
   FUSE metadata path; evaluate `entry_timeout`/`attr_timeout`, `writeback_cache`
   and kernel FUSE passthrough, each against the baseline.
3. Run the 1,000,000-file GC datasets and cold-cache measurements on dedicated
   hardware.
4. Re-run the CI workflow on GitHub after the image and workflow fixes (never re-run
   since).
5. Raise branch coverage (51.2 %), mainly error paths of `passthrough_fs.cpp` and
   `application.cpp`; run the root-only tests automatically in a privileged CI job.
6. `tsfs-ls`: colors are not shown in TSFS listings; add them if users ask for it.
   The replacement of
   `/usr/bin/ls` is not packaged (a `dpkg-divert` in a Debian package would be the
   clean way).
7. Run the full quality gate again with the 206 tests and update sections 5 and 6.
