<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/testing.md
Purpose : Running the test suite: targets, labels, single tests, environment, sanitizers
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Testing

This is the practical guide: how to run the tests, select them, control their
environment, use the sanitizers and add a new test. The test strategy (what is tested and
why, coverage goals, known gaps) is in
[docs/testing/test-strategy.md](../testing/test-strategy.md).

As in [building.md](building.md), commands are shown without the
`scripts/tsfs-dev.sh` prefix; add it on a host without a C++26 toolchain. The examples
use the `debug` tree; every other tree works the same way.

## 1. Test layout

The suite uses GoogleTest/GoogleMock. Each executable covers one area and is registered
in CTest with one test per GoogleTest case (`gtest_discover_tests`, discovery at test
time).

| Executable | Sources | Labels | Timeout |
|---|---|---|---|
| `tsfs_unit_tests` | `tests/unit/test_common.cpp` | `unit` | 120 s |
| `tsfs_policy_tests` | `tests/policy/test_policy.cpp` | `unit;policy` | 120 s |
| `tsfs_config_tests` | `tests/config/test_config.cpp` | `unit;config` | 120 s |
| `tsfs_reload_tests` | `tests/config/test_reload.cpp` | `config;reload;concurrency` | 120 s |
| `tsfs_scheduler_tests` | `tests/gc/test_scheduler.cpp` | `unit;gc;concurrency` | 120 s |
| `tsfs_gc_tests` | `tests/gc/test_garbage_collector.cpp` | `gc;concurrency;security` | 120 s |
| `tsfs_logging_tests` | `tests/logging/test_logging.cpp` | `logging;concurrency` | 120 s |
| `tsfs_diagnostics_tests` | `tests/diagnostics/test_diagnostics.cpp` (+ `tsfs_crash_helper`) | `diagnostics` | 120 s |
| `tsfs_security_tests` | `tests/security/test_backing_store.cpp` | `security` | 120 s |
| `tsfs_filesystem_tests` | `tests/filesystem/test_mount.cpp` (mounts the real `tsfs` binary) | `filesystem;security;concurrency` | 300 s |
| `tsfs_stress_tests` | `tests/stress/test_stress.cpp` | `stress` | 600 s |

Shared fixtures are in `tests/support/` (`tsfs::test::TempDir`, file helpers, snapshot
builders, `fuse_available()`). `tsfs_crash_helper` is a separate executable that crashes
on purpose so that the diagnostics tests never crash the test runner.

`tsfs_add_test` escapes the list separators of `LABELS` and `ENVIRONMENT` before passing
them to `gtest_discover_tests` (which flattens its `PROPERTIES`), so every declared label
and every environment variable is applied to every test case. CTest discovers 196 test
cases; per label:

| Label | Tests | Label | Tests |
|---|---|---|---|
| `unit` | 91 | `logging` | 20 |
| `concurrency` | 101 | `filesystem` | 21 |
| `security` | 63 | `reload` | 10 |
| `gc` | 50 | `diagnostics` | 10 |
| `config` | 33 | `stress` | 2 |
| `policy` | 27 | | |

Parameterized suites count once per instantiation: `tsfs_scheduler_tests` has 22 cases
(7 parameterized tests x 2 strategies + 8), `tsfs_security_tests` 14 (6 parameterized
tests x 2 resolution modes + 2).

## 2. Running the tests

### 2.1 Everything

```bash
cmake --build build/debug --target tests          # builds tsfs, the helper and the tests, then runs CTest
cmake --build --preset debug --target tests        # same through the build preset
```

The `tests` target runs `ctest --output-on-failure -j 4` and writes a JUnit report to
`<tree>/test-results.xml`. Skipped tests (no FUSE, core dumps not enabled, ...) are
reported as **Not Run (Skipped)** in the summary, never as passed.

Through CTest directly (after a build):

```bash
ctest --test-dir build/debug --output-on-failure -j"$(nproc)"
ctest --preset debug                                # test preset: output on failure, 4 jobs
```

Test presets exist for `debug`, `release`, `asan`, `tsan`, `coverage` and `clang`.

### 2.2 By label or by name

```bash
ctest --test-dir build/debug --print-labels        # every label
ctest --test-dir build/debug -L unit               # one label
ctest --test-dir build/debug -L filesystem -L gc   # several -L: tests having all of them
ctest --test-dir build/debug -LE stress            # exclude a label
ctest --test-dir build/debug -R '^GcTest\.'        # name regex (GoogleTest Suite.Case)
ctest --test-dir build/debug -N -L gc              # list without running
```

### 2.3 A single test

Run the executable directly with a GoogleTest filter; this is also the most convenient
way to debug a test ([debugging.md](debugging.md)):

```bash
build/debug/tests/tsfs_gc_tests --gtest_list_tests
build/debug/tests/tsfs_gc_tests --gtest_filter='GcTest.DeletionRateIsBounded'
build/debug/tests/tsfs_policy_tests --gtest_filter='Glob.*:-Glob.EscapesAndUtf8'
ctest --test-dir build/debug -R '^GcTest\.DeletionRateIsBounded$' -V
```

Parameterized tests have names such as
`Strategies/SchedulerTest.RandomizedAgainstReferenceModel/0`.

### 2.4 Flaky or concurrency failures: repeat until failure

```bash
ctest --test-dir build/debug -R '^MountTest\.' --repeat until-fail:50 --output-on-failure
build/debug/tests/tsfs_reload_tests --gtest_repeat=200 --gtest_break_on_failure \
    --gtest_filter='ReloadTest.*'
build/debug/tests/tsfs_scheduler_tests --gtest_shuffle --gtest_repeat=20
```

Combine with the `tsan` tree for data races (section 5). The `debug` tree already runs
everything under ASan + LeakSanitizer + UBSan.

## 3. FUSE tests

`tsfs_filesystem_tests` (and the FUSE parts of the stress tests) mount the real `tsfs`
binary. `fuse_available()` skips them with a reason when:

- `/dev/fuse` cannot be opened;
- `fusermount3` is missing and the test does not run as root;
- `TSFS_TEST_DISABLE_FUSE` is set (any value).

`scripts/tsfs-dev.sh` grants `/dev/fuse`, `CAP_SYS_ADMIN` and an unconfined AppArmor
profile by default, so the FUSE tests run inside the container; `--no-fuse` removes them,
and the tests then skip. A test that fails while mounting can leave a stale mount point:
see [debugging.md](debugging.md#7-common-failure-modes).

Every `MountTest` ends (`TearDown`) by stopping the daemon and checking that it **exited
with status 0** and that its output contains no memory error (`ERROR: AddressSanitizer`),
data race (`ThreadSanitizer`) or undefined behavior (`runtime error:`), and no leak
**allocated by TSFS code**. The daemon is started with `LSAN_OPTIONS=...:exitcode=0` and
`classify_leaks()` attributes each LeakSanitizer block to the first frame after the
allocator that is not in the C++ standard library headers: a TSFS frame (`tsfs::` or a
path under `src/`) fails the test; a leak allocated inside libfuse (request memory left
when a session ends with queued RELEASE/RELEASEDIR requests, a libfuse behavior) is only
recorded as the `third_party_leaks_at_exit` property. A module-wide `leak:libfuse3.so`
suppression is deliberately not used: it would also hide TSFS allocations made from
libfuse threads. In a sanitized tree, a leak or a memory error in the daemon therefore
fails the test that exercised the faulty path, even though the daemon runs in a separate
process. A few tests need root
(`MountTest.FileBindMountedFromAnotherFilesystemIsNotReachable` creates a bind mount) and
skip otherwise.

## 4. Environment variables

| Variable | Read by | Effect |
|---|---|---|
| `TSFS_TEST_ARTIFACT_DIR` | `tests/support/test_support.cpp` | Parent of every temporary test directory (else `$TMPDIR`, else `/tmp`) |
| `TSFS_TEST_KEEP_ARTIFACTS=1` | `TempDir` destructor | Keep every temporary directory, always (whether the test passed or failed), for inspection |
| `TSFS_STRESS_SCALE=N` | `tests/stress/test_stress.cpp` | Multiply the stress workloads by N (minimum and default 1) |
| `TSFS_TEST_ENABLE_CORE_DUMPS=1` | `tests/diagnostics/test_diagnostics.cpp` | Enable the native core-dump test (skipped otherwise) |
| `TSFS_TEST_DISABLE_FUSE` | `fuse_available()` | Skip every FUSE test |

Notes:

- **`TSFS_TEST_ARTIFACT_DIR` under CTest.** The CMake cache variable of the same name
  (default `<tree>/test-artifacts`) is set as the CTest `ENVIRONMENT` property of each
  test, which overrides the variable inherited from the shell. To relocate the artifacts
  of CTest runs, reconfigure:
  `cmake -S . -B build/debug -DTSFS_TEST_ARTIFACT_DIR=/src/build/debug/my-artifacts`.
  The environment variable takes effect when a test executable is run directly.
- **Forwarding into the container.** `scripts/tsfs-dev.sh` forwards
  `TSFS_TEST_ENABLE_CORE_DUMPS`, `TSFS_TEST_ARTIFACT_DIR`, `TSFS_TEST_KEEP_ARTIFACTS`,
  `TSFS_STRESS_SCALE`, `TSFS_BENCH_LARGE` and `TSFS_BENCH_DIR` when they are set on the
  host:

  ```bash
  TSFS_TEST_KEEP_ARTIFACTS=1 scripts/tsfs-dev.sh build/debug/tests/tsfs_gc_tests
  TSFS_STRESS_SCALE=10 scripts/tsfs-dev.sh ctest --test-dir build/release -L stress
  ```

  Other variables (for example `ASAN_OPTIONS`) still need
  `scripts/tsfs-dev.sh env VAR=value ...`.

  Paths must be container paths (`/src/...`); anything outside `/src` disappears with the
  container.
- **Core dumps.** The container is started with `--ulimit core=-1`, but where a core goes
  is decided by the host kernel's `kernel.core_pattern` (shared by every container). The
  native core-dump test only verifies the file when the pattern writes a plain file
  relative to the working directory; with a pipe handler (`systemd-coredump`, `apport`)
  or a restrictive hard limit it skips with the reason. The tests never change the kernel
  configuration. `CoreDumps.InspectionReportsTheEnvironment` always records the effective
  `core_pattern`, `RLIMIT_CORE` and handler in its output and in the JUnit properties.

  ```bash
  TSFS_TEST_ENABLE_CORE_DUMPS=1 scripts/tsfs-dev.sh ctest --test-dir build/debug -L diagnostics -V
  ```

## 5. Sanitizers

ThreadSanitizer and coverage each need their own tree: TSan is incompatible with the ASan
runtime, and no sanitizer can be combined with coverage, which the configuration
enforces.

### AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer (`debug`, `asan`, `clang`)

Every Debug tree is instrumented by default (`TSFS_SANITIZE` defaults to
`address;undefined` for `CMAKE_BUILD_TYPE=Debug` without coverage), so the day-to-day
`debug` tree, the `clang` tree and the `asan` tree (the same without benchmarks) all run
the suite under ASan + LeakSanitizer + UBSan:

```bash
cmake --preset debug
cmake --build --preset debug --target tests
```

Built with `-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=all`: the first error aborts the test with a report on stderr
(visible with `--output-on-failure`). `tests/CMakeLists.txt` sets the runtime options in
the CTest `ENVIRONMENT` of every test; the daemons and crash helpers started by the tests
inherit them:

| Variable | Value | Effect |
|---|---|---|
| `ASAN_OPTIONS` | `detect_leaks=1:strict_string_checks=1:check_initialization_order=1:detect_stack_use_after_return=1` | LeakSanitizer explicitly on, stricter checks |
| `LSAN_OPTIONS` | `print_suppressions=0:suppressions=<source>/tests/sanitizers/lsan.supp` | Suppression file |
| `UBSAN_OPTIONS` | `print_stacktrace=1:halt_on_error=1` | Stack trace, stop at the first report |

A leak makes the process exit with a non-zero status (1 with ASan) after the
LeakSanitizer report, so the test fails. For the daemon itself, see the `MountTest`
teardown check in section 3.

`tests/sanitizers/lsan.supp` is empty. Rule: **never suppress a TSFS symbol**, a leak in
`src/` is a defect to fix; an entry is only acceptable for third-party code, with the
library and the reason (and a reference when possible).

These options only apply under CTest. When running a test executable directly, export
them yourself if needed (for example
`scripts/tsfs-dev.sh env ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 build/debug/tests/tsfs_gc_tests`).
To build Debug without sanitizers (valgrind, measurements), configure with
`-DTSFS_SANITIZE=none`.

### ThreadSanitizer (`tsan`)

```bash
scripts/tsfs-dev.sh cmake --preset tsan
scripts/tsfs-dev.sh cmake --build --preset tsan
scripts/tsfs-dev.sh --no-aslr cmake --build build/tsan --target tests
scripts/tsfs-dev.sh --no-aslr ctest --preset tsan                  # 2 parallel jobs
```

Why `--no-aslr`: on recent kernels `vm.mmap_rnd_bits` is 32, which places mappings
outside the address ranges ThreadSanitizer expects, and TSan-instrumented binaries then
abort at startup ("unexpected memory mapping"). Running under `setarch -R` (ASLR
disabled) avoids it, but Docker's default seccomp profile blocks
`personality(ADDR_NO_RANDOMIZE)`, so `--no-aslr` also starts the container with
`seccomp=unconfined`. Alternative on the host, as root and system-wide:
`sysctl vm.mmap_rnd_bits=28`. The FUSE tests mount the TSan-instrumented daemon too, so
they also exercise the FUSE threads.

## 6. Adding a test

1. Put the test in the directory of its area (`tests/<area>/test_<topic>.cpp`) with the
   standard file header, in `namespace tsfs` (with an unnamed namespace for local
   helpers). Use `tsfs::test::TempDir` for any file, `ManualClock`
   (`src/common/clock.hpp`) instead of sleeping, and `GTEST_SKIP() << "reason"` when a
   capability is missing.
2. Add the source to an existing executable in `tests/CMakeLists.txt`, or register a new
   one:

   ```cmake
   # tsfs_add_test(<name> LABELS <labels...> SOURCES <files...> [TIMEOUT <s>])
   tsfs_add_test(tsfs_metrics_tests LABELS metrics SOURCES metrics/test_status_report.cpp)
   ```

   `tsfs_add_test` links `tsfs_test_support` (hence `tsfs_core`, `GTest::gtest`,
   `GTest::gtest_main`), defines `TSFS_BINARY` (path of the `tsfs` executable) and
   `TSFS_CRASH_HELPER`, registers every test case with all its labels, a timeout
   (default 120 s), the skip pattern, `TSFS_TEST_ARTIFACT_DIR` and the sanitizer options
   of section 5, and adds the executable to the `tests` and `build` targets. A test that
   runs the daemon must also declare `add_dependencies(<name> tsfs)`.
3. A test that deliberately crashes must do so in a subprocess (`tsfs_crash_helper` or a
   forked child), never in the test runner.
4. Every bug fix comes with a regression test when practical.
5. Run the new test in `debug` (sanitized), then in `tsan` if it touches concurrency.

## 7. Before submitting

The full gate is `scripts/tsfs-qa.sh` ([contributing.md](contributing.md#4-quality-gate),
[building.md](building.md#42-local-quality-gate-scriptstsfs-qash)). At minimum:

```bash
cmake --build --preset debug --target tests        # ASan + LeakSanitizer + UBSan
scripts/tsfs-dev.sh --no-aslr cmake --build build/tsan --target tests
# or, everything:
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh
```
