<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/testing/test-strategy.md
Purpose : Test strategy: pyramid, suites and labels, determinism, sanitizers, coverage, regressions
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# TSFS test strategy

This document explains **what** is tested, **at which level**, and **why the tests can be
trusted** (determinism, isolation, sanitizers). For the day-to-day commands, see
[developer testing](../developer/testing.md), [coverage](../developer/coverage.md) and
[debugging](../developer/debugging.md). For the security rationale behind many of the
tests, see the [threat model](../security/threat-model.md).

The GC deletes files, so the tests focus on two invariants. The test suites check them
at every level, from unit tests to stress tests:

1. **Never delete what must be kept.** This covers files that are not expired, excluded,
   replaced, renamed away, open, outside the tree, reached through a symbolic link, or
   on another mount.
2. **Always eventually delete what has expired**, despite saturation, failures, reloads
   and restarts.

## 1. Test pyramid

```
                 /\      stress (2)         churn + reloads, scheduler at scale
                /  \     filesystem (22)    real daemon, real FUSE mount, signals
               /    \    diagnostics (10)   crash reports in isolated subprocesses
              /------\   gc (28), reload (10), logging (20), security (14)
             /        \     component tests on real temporary trees, threads
            /----------\ unit (91): parsing, paths, escaping, configuration, policy,
                            scheduler, token bucket, locks, registry
```

The total is **199 test cases** in 11 GoogleTest executables, all registered in CTest
through `gtest_discover_tests` (`DISCOVERY_MODE PRE_TEST`):

```sh
scripts/tsfs-dev.sh ctest --test-dir build/debug -N | tail -3     # ... Total Tests: 199
```

Most component tests run on real directories and real system calls rather than mocks.
Only time (`ManualClock`) and, in a few cases, the open-file registry are driven
directly.

## 2. Suites, executables and labels

Every executable is declared in `tests/CMakeLists.txt` with `tsfs_add_test()`.

| Executable | Source | Labels (declared) | Tests | Covers |
|---|---|---|---|---|
| `tsfs_unit_tests` | `tests/unit/test_common.cpp` | unit | 21 | fusectl connection identifier parsed from `/proc/self/mountinfo` (escaped paths, stacked mounts, non-FUSE mounts) and the `waiting` reader. Duration and size parsing and formatting (units, overflow, round trips). `ManagedPath` normalization, traversal and NUL rejection, component-wise `is_within`. Log escaping (newlines, quotes, control bytes, invalid UTF-8, JSON). Log event rendering. |
| `tsfs_policy_tests` | `tests/policy/test_policy.cpp` | unit, policy | 27 | Glob engine (wildcards, classes, escapes, UTF-8, `*` versus `**`, invalid patterns). Policy precedence (exclusion always wins, then priority, scope specificity, recursion, pattern literals, declaration order). Extension and exact-file rules. `never`. Birth-time fallback. Saturating expiry. Future mtimes. |
| `tsfs_config_tests` | `tests/config/test_config.cpp` | unit, config | 24 | Safe defaults. The shipped example is valid. YAML errors with line and column. Unknown and duplicate keys. Invalid durations, sizes, paths, rules and GC settings. Logging settings. Dump and parse round trip. Configuration file trust (a root daemon requires a root-owned file not writable by group or others; an unprivileged daemon only needs a regular file). `-m`/`-b` overrides validated like file values. Missing file errno. Command-line parsing (including `--purge`, `--purge --force`, `--purge --dry-run` and `--dry-run --purge` as a preview, `--stats`), usage errors (`--force` alone, with `--stats` or with a preview), and `--help` documenting every option. |
| `tsfs_reload_tests` | `tests/config/test_reload.cpp` | config, reload, concurrency | 10 | New generation on success. A failed reload keeps the previous configuration. Consecutive reloads. Restart-only settings. Observer failure aborts publication. CLI overrides survive. Concurrent reloads and readers. inotify detection of atomic replacement (no-ops ignored). `SIGHUP`, `SIGUSR1` and `SIGUSR2` bridges. Re-watch after the directory is recreated. |
| `tsfs_scheduler_tests` | `tests/gc/test_scheduler.cpp` | unit, gc, concurrency | 22 | Both scheduler strategies (heap, ordered; 7 parameterized cases each): due order, exact due time, reschedule, cancel, capacity, `pop` max, randomized comparison with a reference model. Heap tombstone compaction. Token bucket (unlimited, rate, stop). Log rate limiter. Sharded counters. Open-file registry. No deadlock between ancestor locks and pair locks. |
| `tsfs_gc_tests` | `tests/gc/test_garbage_collector.cpp` | gc, concurrency, security | 30 | Deletion at, before and after expiry. Dry run (from the API and from the configuration). Modified, replaced, renamed and externally deleted files. Open files. Exclusion added by reload. Clock step backwards. Permission failures retried, then abandoned. Symbolic links never followed. Directory swapped for a link. Hard links. Queue saturation and recovery. Restart without state. Threaded pipeline. Notifications. Rate and per-cycle limits. Disabled GC. Fast shutdown. Worker count reload. Concurrent creation never loses fresh files. Purge: every expired file at once despite a rate of 1/s, a cycle budget and a scheduler smaller than the backlog (several passes), excluded and young files kept; purge with the GC disabled, and `dry_run` honored (nothing deleted, `would_delete` counted). |
| `tsfs_logging_tests` | `tests/logging/test_logging.cpp` | logging, concurrency | 20 | Directory and file creation. Line format. Levels and runtime filtering. JSON lines. Rotation and `max_files`. Rotation disabled. Logging disabled. Concurrent writers with rotation (block policy loses nothing). `discard_new` and `overrun_oldest` policies. Flush on shutdown. Reconfiguration and failed reconfiguration. Permission errors. Active file deleted or replaced externally. Verified compression and compression failure. Repair after an interrupted rotation. Disk full (`RLIMIT_FSIZE`) without recursion or crash. |
| `tsfs_diagnostics_tests` | `tests/diagnostics/test_diagnostics.cpp` | diagnostics | 10 | Symbol table of the running process (a TSFS function of the executable and a libc function resolved, unmapped address rejected). Crash reports for SIGSEGV, SIGABRT (the process still dies with SIGABRT), SIGFPE, stack overflow (alternate stack) and a crash in a helper thread. A normal exit is not disturbed. Old reports are pruned and other files are kept. Core-dump environment inspection. Native core dump (opt-in). |
| `tsfs_security_tests` | `tests/security/test_backing_store.cpp` | security | 14 | `BackingStore` with openat2 and with the forced fallback walk (6 parameterized cases each): ordinary paths, non-normalized paths, intermediate symbolic links (absolute, relative, nested, `/proc/self/root` magic link), a symbolic link opened as a directory, a final link seen as a link, and a swapped directory. Missing root. openat2 probe. |
| `tsfs_filesystem_tests` | `tests/filesystem/test_mount.cpp` | filesystem, security, concurrency | 26 | The real `tsfs` binary mounted with FUSE: create, read, write and append; truncate; rename, unlink and directories; `RENAME_NOREPLACE` and `RENAME_EXCHANGE`; large `readdir`; permissions, ownership and timestamps; symbolic and hard links; device nodes refused; `statfs`; large files; concurrent clients; GC through the mount; open file kept until release; `SIGHUP` reload without remount; invalid reload keeps serving; a file bind-mounted from another filesystem is unreachable (`EXDEV`, root only); the mode of a new entry ignores the daemon's umask; the status file names the FUSE connection (`fuse.connection_id` equal to the mount's device number); a second instance on a mounted mount point is refused (exit 7); `SIGTERM` during startup (10 iterations, as soon as the daemon announces the mount) ends in a clean unmount with exit 0; retention extended attributes (`user.tsfs.state`, `.rule`, `.expires` for an expiring, an excluded and a `never` file, `.created` as an ISO 8601 UTC value, `ENODATA` for a directory and an unknown name, `setxattr` refused, empty `listxattr`); `tsfs --stats` and `tsfs --purge --force` against the running daemon (20 long-expired files deleted at a rate limit of 1/s, the excluded file kept, `gc_purge_completed` logged); `tsfs-ls` on a terminal (through `script(1)`; skipped without it; `TSFS_REAL_LS=/usr/bin/ls` as the original) tags `ls -l` entries with the retention (`clip.mp4  created ...`, `, filter rec, deleted `, `hold.keep  UNFILTERED`), keeps the meaning of the system `ls` options run through the original (`-R -I '*.keep'` on the mount root hides `hold.keep` and tags `clip.mp4` in the `rec` subdirectory), and in a pipe its output is byte-identical to `/usr/bin/ls -la`; clean shutdown (exit 0, unmounted); read-only mount; shutdown with a file and a directory still open (their handles are freed and logged as `filesystem_handles_released`; regression test for a leak found by LeakSanitizer); every test ends with a check that the daemon exited with status 0 and printed no memory error, data race or undefined-behavior report, and that no leak in its LeakSanitizer report was allocated by TSFS code (`classify_leaks()`: the first frame after the allocator, outside the C++ standard library headers; leaks allocated inside libfuse when a session ends with queued requests are recorded as `third_party_leaks_at_exit`, not failures). |
| `tsfs_stress_tests` | `tests/stress/test_stress.cpp` | stress | 2 | 8 threads create, rename, rewrite and delete files while the GC runs and the configuration is reloaded every 50 ms. The invariant is that no young file is ever deleted and that all old files are deleted. Separately, 300,000 scheduler operations per strategy. |

Count per suite with, for example:

```sh
scripts/tsfs-dev.sh ctest --test-dir build/debug -R '^GcTest\.' -N | tail -1
scripts/tsfs-dev.sh ctest --test-dir build/debug -L security -N | tail -1
```

### Labels

Labels select cross-cutting views, for example `-L security` for every test that guards
a security property, or `-L concurrency` for every threaded test. `tsfs_add_test()`
escapes the list separators of `LABELS` (and of `ENVIRONMENT`) before passing them to
`gtest_discover_tests()`, so every declared label is registered
(`ctest --print-labels`):

| Label | Tests |
|---|---|
| unit | 94 |
| policy | 27 |
| config | 34 |
| reload | 10 |
| gc | 52 |
| concurrency | 108 |
| logging | 20 |
| diagnostics | 10 |
| security | 70 |
| filesystem | 26 |
| stress | 2 |

Total: 206 tests (`ctest -N` in `build/debug`).

```sh
scripts/tsfs-dev.sh ctest --test-dir build/debug -L security --output-on-failure
scripts/tsfs-dev.sh ctest --test-dir build/debug -L concurrency -LE stress
```

## 3. Determinism and isolation

* **Injected clock.** The GC, the policy engine and the scheduler take a `Clock&`.
  `ManualClock` (`src/common/clock.hpp`) is atomic and can move forward or backward
  (`advance()`, `set()`). The GC tests run at `kEpoch2026` (2026-01-01T00:00:00Z) and set
  file mtimes relative to that clock (`test::set_mtime`, `utimensat` with
  `AT_SYMLINK_NOFOLLOW`). Expiry boundaries are therefore exact to the millisecond
  (`ExactlyAtExpiryIsDeleted`, `OneMillisecondBeforeExpiryIsKept`), and a clock step is
  reproducible (`ClockStepBackwardsDoesNotDeleteEarly`).
* **Synchronous GC entry points.** `run_full_scan()`, `process_due_now()` and
  `dry_run_report()` drive the GC without its threads, so most GC tests are
  single-threaded and deterministic. The threaded tests (`ThreadedPipelineDeletesManyFiles`,
  `ConcurrentCreationNeverLosesFreshFiles`, the mount and stress tests) assert
  **eventual** outcomes with bounded polling (`eventually(predicate, timeout)`). They never
  assert fixed sleeps or timings.
* **Private trees.** `test::TempDir` (`tests/support/test_support.cpp`) creates a
  `mkdtemp` directory under `$TSFS_TEST_ARTIFACT_DIR`. CTest sets that variable to
  `build/<tree>/test-artifacts` (CMake cache variable `TSFS_TEST_ARTIFACT_DIR`). Without
  it, the directory goes under `$TMPDIR` or `/tmp`. Tests never touch anything outside
  their directory. The destructor makes read-only subdirectories writable again and then
  removes the tree.
* **Keeping artifacts.** With `TSFS_TEST_KEEP_ARTIFACTS=1`, every `TempDir` is left in
  place, whether the test passed or failed, so you can inspect logs, status files and
  crash reports.
* **Seeded randomness.** The randomized scheduler test and the stress threads use
  `std::mt19937` with fixed seeds.
* **Skip, not pass.** Tests that cannot run in the current environment call
  `GTEST_SKIP()`. CTest's `SKIP_REGULAR_EXPRESSION "\[  SKIPPED \]"` reports them as
  *Not Run (Skipped)*, never as passed.

## 4. Crash tests in isolated subprocesses

A crash handler cannot be tested inside the test runner without killing it.
`tests/diagnostics/crash_helper.cpp` builds the separate program `tsfs_crash_helper`
(`-O0 -fno-omit-frame-pointer -rdynamic`, so the deliberate frames stay visible in the
backtrace). It installs the real `CrashHandler` and then triggers `segv`, `abort`, `fpe`,
`stack` (unbounded recursion), `thread-segv` or `exit`.
`tests/diagnostics/test_diagnostics.cpp` starts it with `posix_spawn` in a private
directory and checks three things: the wait status (for example, still killed by
SIGABRT), the content of the report, and pruning.

## 5. FUSE integration tests

`tests/filesystem/test_mount.cpp` starts the real `tsfs` binary
(`TSFS_BINARY` = `$<TARGET_FILE:tsfs>`) with `--foreground` on a temporary
configuration, backing directory and mount point. It waits for `fuse.tsfs` to appear in
`/proc/self/mountinfo` and stops the daemon with SIGTERM. If needed, it falls back to
`fusermount3 -u -z`.

`TearDown()` then checks that the daemon exited with status 0 and that its output
(`tsfs.out`) contains no `LeakSanitizer`, `AddressSanitizer`, `ThreadSanitizer` or
`runtime error:` report. In a sanitizer build, a leak or an error in the daemon therefore
fails the test that exercised the faulty path, even though the daemon runs in another
process.

`test::fuse_available()` decides whether these tests run. They are **skipped** when:

* `TSFS_TEST_DISABLE_FUSE` is set;
* `/dev/fuse` cannot be opened read-write;
* `fusermount3` is not installed in `/usr/bin` or `/bin` and the tests do not run as
  root.

`scripts/tsfs-dev.sh` passes `/dev/fuse` and `CAP_SYS_ADMIN` to the container by default.
`--no-fuse` withholds them, and the 26 mount tests are then skipped.
`MountTest.TsfsLsShowsRetentionOnATerminalAndDelegatesOtherwise` also needs
`script(1)` (util-linux, to give `tsfs-ls` a terminal) and `/usr/bin/ls`; it is
skipped without them. The mount tests depend on the `tsfs` and `tsfs_ls` targets
(`TSFS_BINARY`, `TSFS_LS_BINARY`).

## 6. Stress scale

`TSFS_STRESS_SCALE=<n>` (default 1, minimum 1) multiplies the stress workloads: 2,000 x n
expired files for `ChurnWithReloadsNeverDeletesYoungFiles` and 300,000 x n scheduler
operations for `SchedulerChurn`. The stress executable has a 600 s CTest timeout. Other
executables use 120 s, and the filesystem executable uses 300 s.

```sh
TSFS_STRESS_SCALE=10 scripts/tsfs-dev.sh ctest --test-dir build/debug -L stress --output-on-failure
```

## 7. Native core dump test

`DiagnosticsTest.NativeCoreDumpWhenTheEnvironmentAllowsIt` is opt-in because a core dump
depends on the host's `core_pattern` and limits. It is skipped unless
`TSFS_TEST_ENABLE_CORE_DUMPS=1` is set. Even with the variable set, it is skipped when the
hard `RLIMIT_CORE` is 0 or when `core_pattern` sends cores to a handler such as
systemd-coredump or apport. The test cannot verify those handlers itself; use
`coredumpctl` instead. `CoreDumps.InspectionReportsTheEnvironment` always runs and
records the environment as test properties.

```sh
TSFS_TEST_ENABLE_CORE_DUMPS=1 scripts/tsfs-dev.sh \
    ctest --test-dir build/debug -R NativeCoreDump --output-on-failure
```

## 8. Sanitizers

`CMake/Sanitizers.cmake` accepts `TSFS_SANITIZE=address`, `undefined`,
`address;undefined`, `thread` or `none`. **Debug builds use `address;undefined` by
default** (AddressSanitizer, which includes LeakSanitizer on Linux, plus
UndefinedBehaviorSanitizer) when coverage is off; every other build type, and the
coverage tree, defaults to `none`. `-DTSFS_SANITIZE=none` opts a Debug tree out (for
example, to run under valgrind). Combining TSan with ASan, or any sanitizer with
coverage, is rejected. Code is built with `-fno-sanitize-recover=all`, so any report
fails the test.

`tests/CMakeLists.txt` exports the sanitizer runtime options to every test process (and,
by inheritance, to the daemons and crash helpers it starts):

| Variable | Value | Effect |
|---|---|---|
| `ASAN_OPTIONS` | `detect_leaks=1:strict_string_checks=1:check_initialization_order=1:detect_stack_use_after_return=1` | A leak makes the process exit non-zero (status 1 with ASan) |
| `LSAN_OPTIONS` | `print_suppressions=0:suppressions=tests/sanitizers/lsan.supp` | Suppressions for third-party code only (the file is empty today; rule: never suppress a TSFS symbol) |
| `UBSAN_OPTIONS` | `print_stacktrace=1:halt_on_error=1` | The first undefined behavior stops the process |

| Preset | Build tree | Purpose | Run |
|---|---|---|---|
| `debug` | `build/debug` | Debug, AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer (default) | `scripts/tsfs-dev.sh ctest --preset debug` |
| `asan` | `build/asan` | Same sanitizers, benchmarks off | `scripts/tsfs-dev.sh ctest --preset asan` |
| `tsan` | `build/tsan` | ThreadSanitizer (2 parallel jobs) | `scripts/tsfs-dev.sh --no-aslr ctest --preset tsan` |
| `clang` | `build/clang` | Clang 21, Debug (so ASan + UBSan as well), warnings as errors | `scripts/tsfs-dev.sh ctest --preset clang` |

TSan needs ASLR disabled on kernels with `vm.mmap_rnd_bits > 28`. `--no-aslr` runs the
command under `setarch -R` with an unconfined seccomp profile.

The local quality gate `scripts/tsfs-qa.sh` runs these trees in sequence (steps `ci`,
`debug`, `clang`, `tidy`, `tsan`, `coverage`, among others); see
[contributing](../developer/contributing.md).

**Status on 2026-10-13:** the suite now has 206 tests. In the last recorded full run of
the `debug` tree (ASan + LeakSanitizer + UBSan), before the purge, statistics, `tsfs-ls`
and startup-signal tests were added, 197 of the 199 tests passed and 2 were skipped: the opt-in native core dump test (section 7) and
the root-only bind-mount test (run separately as root). The `clang` and `ci` presets build with `-Werror` and pass the suite.

## 9. Regression tests for defects found

Each defect found by the tests or the sanitizers is fixed together with a test that
would have caught it. Examples fixed on 2026-10-13:

| Defect | Found by | Fix and regression guard |
|---|---|---|
| UBSan intercepted the deliberate null write in the crash helper (`-fno-sanitize-recover` exits before the store faults), so the crash handler under test never ran in the `asan` tree. | ASan+UBSan run, `DiagnosticsTest.SegfaultWritesACompleteReport` | `deliberate_null_write()` in `tests/diagnostics/crash_helper.cpp` is `[[gnu::noinline, gnu::no_sanitize("undefined")]]`. The diagnostics tests now pass in the sanitizer tree. |
| The stress test relied on the build being slow enough to saturate the GC queue, so `queue_saturations > 0` failed on fast builds or passed only by chance. | Result depending on build speed (sanitizer vs optimized trees) | `tests/stress/test_stress.cpp` sets `max_pending_tasks = 64` (the minimum accepted range is 16), so saturation happens on purpose at any build speed. The test asserts `queue_saturations > 0`. |
| A leak in the daemon was invisible: mount tests only checked the mount, not how the daemon ended. | LeakSanitizer now on by default in Debug | `MountTest::TearDown()` requires exit status 0, no memory error/race/UB report and no leak allocated by TSFS code in the daemon output (`classify_leaks()`). |
| File and directory handles still open when the daemon stopped (a RELEASE still queued at SIGTERM, or files held open by applications) were never freed: libfuse does not call release() for them. Found intermittently by LeakSanitizer in `MountTest.RenameNoReplaceAndExchange`. | ASan/LSan, debug tree under parallel load | `PassthroughFs` tracks its live handles (`adopt()`/`take_file()`/`take_dir()`) and frees the remaining ones in its destructor, logged as `filesystem_handles_released`; the daemon destroys the filesystem before shutting the logger down. Regression test `MountTest.ShutdownWithOpenFilesFreesTheirHandles` (verified to fail without the fix). |
| A `SIGTERM` (or `SIGHUP`) received during startup, before the signal handlers were installed, killed the daemon with the default action; after the mount, this left a dead mount (`ENOTCONN`). | ThreadSanitizer runs on 2 CPUs (simulating CI), where the startup is slow enough for the test harness's `SIGTERM` to arrive early | `Daemon::run()` blocks the asynchronous signals as its very first step; a pending signal is handled once the loop runs (clean unmount, exit 0). Regression test `MountTest.SigtermDuringStartupUnmountsCleanly` (verified to fail without the fix). |
| Teardown order of `Logger::Pipeline`: sinks could be destroyed while the async thread pool was still draining into them. | TSan | `src/logging/logger.cpp`: `~Pipeline()` resets `pool` first. That joins the worker after it drains its queue, and only then are the sinks released. Under TSan this path is exercised by `LoggingTest.ShutdownFlushesQueuedMessages`, `ReconfigureToNewDirectoryAndFilename`, `ConcurrentWritersWithRotationLoseNothingWithBlockPolicy` and the stress test, which logs while the configuration is reloaded. |

## 10. Coverage

`CMake/Coverage.cmake` (GCC and gcov only) adds `--coverage -fprofile-update=atomic -O0
-fno-inline` in a dedicated tree, so the flags never reach a normal build. The report
includes **production sources only** (`src/*`). Tests, benchmarks and generated files
are excluded.

```sh
scripts/tsfs-dev.sh cmake --preset coverage
scripts/tsfs-dev.sh cmake --build --preset coverage
scripts/tsfs-dev.sh cmake --build --preset coverage --target tests      # runs CTest, writes .gcda
scripts/tsfs-dev.sh cmake --build --preset coverage --target coverage   # lcov + genhtml
# Report: build/coverage/coverage/html/index.html; "coverage-reset" zeroes the counters.
```

See [coverage](../developer/coverage.md) for interpreting the report.

## 11. Adding a test

1. Put the file in the directory of its area (`tests/<area>/test_<topic>.cpp`). Use
   `tests/support/test_support.hpp` for `TempDir`, `write_file`, `set_mtime`,
   `make_gc_snapshot`, `rule()`, `fuse_available()` and `kEpoch2026`.
2. Add cases to an existing executable, or register a new one in `tests/CMakeLists.txt`:

   ```cmake
   # tsfs_add_test(<name> LABELS <labels...> SOURCES <files...> [TIMEOUT <s>])
   tsfs_add_test(tsfs_example_tests LABELS "gc;security" SOURCES gc/test_example.cpp TIMEOUT 120)
   ```

   `tsfs_add_test()` links `tsfs_test_support` (`tsfs_core` + GoogleTest), defines
   `TSFS_BINARY`, `TSFS_CRASH_HELPER`, `TSFS_LS_BINARY` and `TSFS_SOURCE_DIR`, discovers the cases, applies the labels, the
   timeout (default 120 s) and the skip expression, and exports `TSFS_TEST_ARTIFACT_DIR`.
   It also exports the sanitizer options of section 8. Add
   `add_dependencies(<name> tsfs)` when the test starts the daemon.
3. Rules for a good TSFS test:
   * Drive time with `ManualClock`. Never sleep to wait for an expiry in component tests.
   * Wait for asynchronous outcomes with a bounded `eventually()` and never with a fixed
     sleep.
   * Create everything inside a `TempDir`. Never touch a fixed host path.
   * Use `GTEST_SKIP()` with a reason when the environment lacks something (FUSE, root,
     core dumps).
   * For every new deletion path or validation, add a "must not delete" case next to the
     "deletes" case.
4. Build and run the `tests` target, which builds every test binary and the daemon, runs
   CTest with `-j 4` and writes `test-results.xml` (JUnit):

   ```sh
   scripts/tsfs-dev.sh cmake --build build/debug --target tests
   ```

5. Before merging, run the change under the `debug` (sanitizers by default) and `tsan`
   presets as well, or the whole gate: `scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh`.
