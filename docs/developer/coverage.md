<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/coverage.md
Purpose : Code coverage with gcov, lcov and genhtml: workflow, outputs, interpretation
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Code coverage

Coverage is measured with gcov (GCC instrumentation), captured with lcov and rendered
with genhtml, in a **dedicated build tree** so that the instrumentation never reaches a
normal build. The implementation is in `CMake/Coverage.cmake` and
`CMake/CoverageSummary.cmake`. The test strategy and the coverage goals are discussed in
[docs/testing/test-strategy.md](../testing/test-strategy.md).

## 1. Requirements

- GCC (coverage is refused at configure time with another compiler) and the gcov of the
  same major version (`gcov-15` for `g++-15`; CMake looks for `gcov-<major>` first, then
  `gcov`).
- lcov and genhtml **2.x**. The development image ships lcov 2.0
  (`cmake --build <tree> --target deps` with `-DTSFS_DEPS_PROFILES="build;test;coverage"`
  checks them).
- Coverage cannot be combined with sanitizers (configure-time error). The coverage tree
  is therefore **not sanitized**: although it is a Debug build, `TSFS_SANITIZE` defaults
  to `none` when `TSFS_ENABLE_COVERAGE=ON` (every other Debug tree defaults to ASan +
  LeakSanitizer + UBSan). Memory errors and leaks are caught by the `debug` tree, not
  here.

Everything is available in the development container; prefix the commands below with
`scripts/tsfs-dev.sh` on a host without the toolchain.

## 2. Workflow

### Preset form

```bash
cmake --preset coverage                               # build/coverage, Debug, TSFS_ENABLE_COVERAGE=ON
cmake --build --preset coverage                       # instrumented build
cmake --build --preset coverage --target coverage-reset   # optional: zero previous counters
cmake --build --preset coverage --target tests        # run CTest, writes the .gcda files
cmake --build --preset coverage --target coverage     # capture, filter, HTML, summary
```

`scripts/tsfs-qa.sh coverage` runs the same sequence (configure, build,
`coverage-reset`, `tests`, `coverage`); see
[building.md](building.md#42-local-quality-gate-scriptstsfs-qash).

### Explicit form

```bash
cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Debug -DTSFS_ENABLE_COVERAGE=ON
cmake --build build/coverage -j"$(nproc)"
cmake --build build/coverage --target tests
cmake --build build/coverage --target coverage
```

Notes:

- The `coverage` target captures whatever counters exist; it **does not reset** them.
  Counters accumulate across test runs, which is useful to add a manual run but wrong for
  a clean measurement: run `coverage-reset` first (step 3 of the preset form). A rebuild
  of a source also invalidates its counters.
- The FUSE tests run the instrumented `tsfs` daemon; a process writes its `.gcda` data
  only when it exits normally. A daemon killed with `SIGKILL` or crashing contributes
  nothing. Without `/dev/fuse` (`scripts/tsfs-dev.sh --no-fuse`) those tests skip and the
  filesystem layer shows as largely uncovered.
- Instrumentation flags: `--coverage -fprofile-update=atomic -O0 -fno-inline`. Atomic
  counter updates keep the counts of the multithreaded code (GC workers, FUSE threads)
  exact; `-O0 -fno-inline` keeps line attribution faithful. The `coverage` preset also
  disables the benchmarks.
- A subset can be measured by running only some tests between `coverage-reset` and
  `coverage`, for example
  `ctest --test-dir build/coverage -L gc` instead of the `tests` target.

## 3. What the `coverage` target does

1. `lcov --capture --directory <tree> --base-directory <source> --no-external` into
   `coverage/raw.info` (`--no-external` drops system and third-party headers).
2. `lcov --extract raw.info "<source>/src/*"` into `coverage/tsfs.info`: only production
   sources remain. Tests, benchmarks, the test support library and generated files (such
   as `version.hpp`, generated in the build tree) are scaffolding and are excluded.
   Production code is never excluded to raise the percentage.
3. `genhtml --demangle-cpp --legend --rc branch_coverage=1` into `coverage/html/`.
4. `lcov --summary` on the console.
5. `CMake/CoverageSummary.cmake` writes `coverage/summary.json` and
   `coverage/uncovered.txt` and prints the totals.

Branch coverage is enabled everywhere (`--rc branch_coverage=1`).

## 4. Outputs

All outputs are in `<tree>/coverage/`, that is **`build/coverage/coverage/`** for the
preset (the tree is `build/coverage`, the output directory inside it is `coverage`):

| File | Content |
|---|---|
| `html/index.html` | Browsable report: per directory, per file, annotated sources |
| `summary.json` | Totals and per-file counts (machine-readable) |
| `uncovered.txt` | Every file with unexecuted lines and the list of those lines |
| `tsfs.info` | Filtered lcov tracefile (input of the reports; usable by other tools) |
| `raw.info` | Unfiltered capture |

`summary.json` layout (values are `[hit, total]` per file):

```json
{
  "lines": {"hit": H, "total": T, "percent": P},
  "branches": {"hit": H, "total": T, "percent": P},
  "functions": {"hit": H, "total": T, "percent": P},
  "files": [
    {"file": "/src/src/gc/scheduler.cpp", "lines": [H, T], "branches": [H, T], "functions": [H, T]}
  ]
}
```

Paths are absolute **container** paths (`/src/...`) when the report was produced through
`scripts/tsfs-dev.sh`. Open the HTML report from the host at
`build/coverage/coverage/html/index.html`.

## 5. Why lcov runs with `--ignore-errors`

lcov 2.x is strict: conditions that lcov 1.x silently accepted are now fatal errors. With
C++ templates, inline functions, lambdas and multithreaded code, gcov output routinely
contains harmless inconsistencies, so the capture passes:

```
--ignore-errors mismatch,mismatch,negative,negative,unused,unused,inconsistent,inconsistent
```

and genhtml passes `--ignore-errors inconsistent,inconsistent,corrupt,corrupt`.

| Category | Typical cause here |
|---|---|
| `mismatch` | Function and line records that disagree (end line of a template or inline function instantiated in several translation units) |
| `negative` | Negative counts reported by gcov for some lines or branches (known gcov quirk, more frequent with concurrent code) |
| `unused` | A filter pattern that matches nothing in a given capture (for example `--extract` on a partial run) |
| `inconsistent` | A function reported as executed while none of its lines is, or the reverse, again typical of inlined and templated code |
| `corrupt` (genhtml) | Records genhtml cannot reconcile in the merged tracefile |

Listing a category **twice** is the lcov 2.x way to downgrade the error *and* silence the
resulting warning. These options only make lcov tolerant of tool-level inconsistencies:
they do not drop production files from the report. If a lcov upgrade turns another
category into an error, add it here with its justification rather than removing files.

## 6. Reading the results

### The HTML report

- Line coverage: a line is red when it was compiled but never executed in this run.
- Branch coverage: `[+ -]` markers per condition; a `-` is an outcome never taken. Branch
  counts include branches generated by the compiler (exception paths, short-circuit
  evaluation, destructors), so 100 % branch coverage is rarely reachable or meaningful
  in C++.
- Function coverage: templates appear once per instantiation, with demangled names.

### `uncovered.txt`

One line per file that has unexecuted lines:

```
/src/src/gc/garbage_collector.cpp: 112 113 240 241 242
```

That is: file path, colon, then the line numbers whose execution count is zero, in
ascending order. Files whose lines are all executed are not listed. Use it to review the
untested code quickly (`grep garbage_collector uncovered.txt`), or to diff two runs.

For each listed range, decide whether it is:

1. **missing a test**: an error path or a decision of the retention or GC logic that
   should be exercised (write the test);
2. **hard to reach**: allocation failures, I/O errors on a healthy disk, races; covered,
   if at all, by fault injection or review;
3. **defensive or unreachable**: a candidate for simplification. Coverage alone does not
   prove that code is dead (other build configurations, platforms or inputs may reach it):
   confirm with review and static analysis before removing it.

### Limits

Coverage shows what was executed, not what was verified: an executed line without an
assertion on its effect is not tested. It does not prove the absence of defects, and an
unexecuted branch is never "tested". Use it to find gaps, together with the tests listed
in [docs/testing/test-strategy.md](../testing/test-strategy.md), the sanitizers and
static analysis.

## 7. Latest results

See docs/developer/implementation-report.md
([implementation-report.md](implementation-report.md)).
