<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/performance.md
Purpose : Benchmark suite design, how to run it reproducibly, and how to profile
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Performance: benchmarks and profiling

This document describes how TSFS performance is measured: the benchmarks, their datasets,
how to run them, what must be recorded with every result, and how to profile. **Measured
results are published in
[docs/performance/benchmark-report.md](../performance/benchmark-report.md)**; this
document contains no numbers on purpose.

Two families of measurements are kept separate:

- **component benchmarks** (Google Benchmark, `benchmarks/bench_*.cpp`): the policy
  engine, the expiration scheduler, the garbage collector and the logging pipeline, in
  process, **without FUSE**;
- **filesystem benchmark** (`benchmarks/fs_workload.cpp` driven by
  `scripts/tsfs-bench-fs.sh`): real file operations on the raw backing directory and
  through a TSFS mount, with the GC disabled and enabled, to isolate the FUSE overhead and
  the cost of the GC on the I/O path.

The policy and scheduler benchmarks are synthetic (in memory). The GC and logging
benchmarks use real files on a real filesystem, but with generated datasets; none of them
replays a production workload.

## 1. Component benchmarks

All benchmark binaries link `tsfs_core`, so they measure the production code. Shared
helpers are in `benchmarks/bench_support.hpp` (snapshot builder, `peak_rss_kib()`,
`ScratchDir`, `large_enabled()`).

### 1.1 `tsfs_bench_policy` (`bench_policy.cpp`): CPU only

| Benchmark | Measures | Arguments |
|---|---|---|
| `BM_GlobBasename` | Matching a compiled basename glob (`clip-*[0-9].mp4`) | none |
| `BM_GlobPathDoubleStar` | Matching a path glob with `**` (`/tmp/record/**/*.mp4`) over 256 mixed paths | none |
| `BM_PolicyEvaluate` | Full rule evaluation of a path (precedence-ordered scan, early exit or default) with the 8 example rules of the specification | extra non-matching rules: 0, 10, 100, 1000 (counter `rules`) |
| `BM_PolicyCompile` | Compiling a rule set (the policy cost of a configuration reload) | extra rules: 0, 100, 1000 (microseconds) |

The 256-path sample mixes matching and non-matching names so that both the early exit and
the full scan (no rule matches, the default applies) are exercised.

### 1.2 `tsfs_bench_scheduler` (`bench_scheduler.cpp`): heap versus ordered

The GC time queue is a Strategy with two implementations, selectable at run time with
`garbage_collector.scheduler: heap|ordered`. Every scheduler benchmark runs both: argument
0 is the strategy (`0` = heap, `1` = ordered, shown as the benchmark label), argument 1
the number of live entries (1,000 and 100,000; 1,000,000 with `TSFS_BENCH_LARGE=1`). Due
times are uniformly distributed over 1,000 s.

| Benchmark | Measures |
|---|---|
| `BM_SchedulerFill` | Scheduling N candidates into an empty scheduler |
| `BM_SchedulerReschedule` | Steady state: rescheduling existing paths (files rewritten through the mount) |
| `BM_SchedulerDrain` | Popping every entry in due order, in batches of 64 (default GC batch size) |
| `BM_SchedulerCancel` | Cancelling every entry (files deleted or excluded through the mount) |

Compare the two strategies at equal N (same `Args` second value); throughput is reported
as items per second.

### 1.3 `tsfs_bench_gc` (`bench_gc.cpp`): the garbage collector on real files

Files are created in a scratch directory (section 3.3), with no FUSE in the path. Every
file is 2 h old (expired under the 1 h retention used by the benchmarks) or fresh,
according to the expired percentage. Layouts: **flat** (one directory) and **tree**
(directories of 100 files). The dataset is written just before the measurement, so the
dentry and inode caches are warm (section 4).

| Benchmark | Measures | Datasets | Extra output |
|---|---|---|---|
| `BM_GcDiscover` | One full reconciliation pass: scan, policy evaluation, scheduling (time to discover and schedule expired files) | 1k, 100k × flat, tree × 100 % and 10 % expired; 1M tree 10 % (large) | `files`, `queue_depth`, `peak_rss_MiB`; process CPU time and real time |
| `BM_GcDeleteSingleThread` | Validation and deletion of every expired file in the calling thread; mixed sizes 0 / 1 / 16 / 64 KiB; dataset recreated (untimed) before each of 3 iterations | 1k, 100k × flat, tree; 1M tree (large) | bytes processed; fails if not every file was deleted |
| `BM_GcEndToEnd` | `start()` until every expired file is gone, with the real threads: discovery, scheduling, dispatch, validation, deletion | 1k, 100k (tree) × 1, 2, 4, 8 workers; no 1M variant | `worker_utilization` (busy time / wall time × workers) |
| `BM_GcStartupReconciliation` | Startup / recovery: from `start()` to the end of the first pass over young files (rebuild of the in-memory state, nothing deleted) | 1k, 100k tree; 1M tree (large) | |
| `BM_GcReload` | Applying a new configuration generation to a running GC until the reconciliation pass it triggers completes | 1k, 100k tree; 1M tree (large) | |

Deletion is unthrottled (`max_deletions_per_second: 0`) and the logger is set to `error`,
so the numbers measure the GC itself, not the per-file log lines or the rate limiter.
Throughput (files per second) is the `items_per_second` field.

### 1.4 `tsfs_bench_logging` (`bench_logging.cpp`)

| Benchmark | Measures | Arguments |
|---|---|---|
| `BM_LogFilteredOut` | Cost of a log call below the active level (disabled debug logging on hot paths) | none |
| `BM_LogToFile` | A structured `gc_file_deleted` record written to the rotating file (16 MiB rotation, 2 files kept), including the final drain of the asynchronous queue | `0` = synchronous, `1` = asynchronous; 1 and 4 producer threads; real time |

### 1.5 What the suite does not measure directly

Lock contention and allocation overhead have no dedicated benchmark; they appear in the
`BM_GcEndToEnd` worker utilization and in the multi-threaded `BM_LogToFile` runs, and are
best investigated with a profiler (section 6) before any optimization is attempted.

## 2. Filesystem benchmark

`benchmarks/fs_workload.cpp` (executable `tsfs_fs_workload`, no Google Benchmark) runs a
fixed sequence of operations in a fresh sub-directory of `--dir` and prints one JSON object
per operation:

```json
{"label":"gc-on","op":"create_small","count":10000,"seconds":1.234,"ops_per_s":8103.7}
```

| Operation | What is timed |
|---|---|
| `create_small` | create + 4 KiB write + close, N files |
| `stat` | metadata lookup of the N files |
| `readdir` | directory enumeration |
| `rename` | renaming the N files |
| `read_small` | open + read + close of the N files |
| `seq_write` | one large file, MiB/s, `fsync` included |
| `seq_read` | reading that file back, MiB/s |
| `delete_small` | unlinking the N files |
| `concurrent_create` | small-file creation from T threads |

`scripts/tsfs-bench-fs.sh` runs it three times on the same storage:

| Label | Setup |
|---|---|
| `raw` | Directly in the backing directory: baseline without FUSE |
| `gc-off` | Through a TSFS mount, `garbage_collector.enabled: false` |
| `gc-on` | Through a TSFS mount, GC enabled with a 1-day retention matching every file and `scan_interval: 1s`: scans and change notifications run, nothing is deleted under the workload |

```bash
scripts/tsfs-bench-fs.sh [-B BUILD_DIR] [-w WORK_DIR] [-n FILES] [-t THREADS] [-s LARGE_MIB]
#   -B  build tree with src/tsfs and benchmarks/tsfs_fs_workload (default build/release)
#   -w  parent of the work directory: selects the storage under test (default $TMPDIR or /tmp)
#   -n  small files per operation (default 10000)
#   -t  threads of the concurrent phase (default 4)
#   -s  size of the sequential file in MiB (default 256)
```

It needs `/dev/fuse` and `fusermount3`, works in a private work directory removed at the
end, always unmounts on exit, and prints an environment header (section 5) before the
JSON lines. Exit codes: 0, 1 (runtime), 2 (usage), 4 (missing dependency).

The mounts use the default `filesystem.retention_xattrs: true`, so every write
request costs one extra `getxattr` round trip (`security.capability`). To measure
that cost, compare runs on the same storage with the setting `true` and `false`
(the script has no option for it: add `retention_xattrs: false` to the `filesystem`
section written by `write_config()` in a local copy), and read the `getxattr` and `write`
counters of the status file: they are equal under a write workload. Reference A/B
result (3 runs each, medians): small-file create + write -28 %, large sequential
writes and concurrent creates unchanged within noise
([benchmark report](../performance/benchmark-report.md#8-retention-extended-attributes-cost)).

## 3. Running the benchmarks

Benchmarks are meaningful only in an optimized build. Use the `release` tree (or
`relwithdebinfo` for profiling). The `bench` target prints a warning in any other build
type; the `asan`, `tsan` and `coverage` presets do not build the benchmarks at all. The
`debug` tree does build them, but instrumented with ASan + LeakSanitizer + UBSan (the
Debug default): its numbers are meaningless.

### 3.1 The whole suite

```bash
scripts/tsfs-dev.sh cmake --preset release
scripts/tsfs-dev.sh cmake --build --preset release
scripts/tsfs-dev.sh cmake --build build/release --target bench
```

The `bench` target runs the four Google Benchmark binaries with
`--benchmark_out=<tree>/bench-results/<binary>.json --benchmark_out_format=json
--benchmark_counters_tabular=true`; the console shows the tabular output and the JSON
files keep everything, including the context block (section 5).

Datasets: 1,000 and 100,000 files by default. The 1,000,000-file variants are opt-in:

```bash
TSFS_BENCH_LARGE=1 scripts/tsfs-dev.sh cmake --build build/release --target bench
```

(`scripts/tsfs-dev.sh` forwards `TSFS_BENCH_LARGE`; it adds only the 1,000,000-file datasets to the default ones.) The large runs need several GiB of
free space and millions of free inodes on the scratch filesystem, and take a long time:
the GC deletion benchmark recreates its dataset before each of its 3 iterations.

### 3.2 A single benchmark

Run a binary directly with Google Benchmark's options:

```bash
scripts/tsfs-dev.sh build/release/benchmarks/tsfs_bench_scheduler --benchmark_list_tests
scripts/tsfs-dev.sh build/release/benchmarks/tsfs_bench_scheduler --benchmark_filter='BM_SchedulerDrain'
scripts/tsfs-dev.sh build/release/benchmarks/tsfs_bench_gc \
    --benchmark_filter='BM_GcDiscover/100000/1/10/' --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true
scripts/tsfs-dev.sh build/release/benchmarks/tsfs_bench_policy \
    --benchmark_out=build/release/bench-results/policy-manual.json --benchmark_out_format=json
```

Benchmark names include their arguments (`BM_GcDiscover/<files>/<layout>/<expired%>`,
layout `0` = flat, `1` = tree; `BM_SchedulerFill/<0 heap|1 ordered>/<entries>`), followed by
suffixes such as `/process_time/real_time` or `/iterations:3`. The filter is a regular
expression searched in the name: end a prefix with `/` to avoid matching longer arguments
(`BM_GcDiscover/100000/1/10` also matches `.../1/100`).
`--benchmark_repetitions=N` gives mean, median and standard deviation; use it before
comparing two runs.

### 3.3 Choosing the storage (`TSFS_BENCH_DIR`)

The GC and logging benchmarks create their files under `$TSFS_BENCH_DIR`, else `$TMPDIR`,
else `/tmp`. **Inside the container, `/tmp` is on the container's overlay filesystem**,
which is rarely the storage you want to characterize. Point the scratch directory at the
filesystem under test, under the bind-mounted repository (or another volume you mount
yourself):

```bash
mkdir -p build/bench-scratch
TSFS_BENCH_DIR=/src/build/bench-scratch scripts/tsfs-dev.sh \
    cmake --build build/release --target bench
```

`scripts/tsfs-dev.sh` forwards `TSFS_BENCH_DIR` when it is set on the host; the value must
be a container path (`/src/...`). The same applies to the filesystem benchmark: select the storage with
`-w`:

```bash
scripts/tsfs-dev.sh scripts/tsfs-bench-fs.sh -B build/release -w /src/build/bench-scratch
scripts/tsfs-dev.sh scripts/tsfs-bench-fs.sh -n 100000 -t 8 -s 1024 -w /src/build/bench-scratch \
    | tee build/release/bench-results/fs-$(date +%Y%m%d).txt
```

## 4. Warm and cold cache

All benchmarks run with a **warm** page, dentry and inode cache: the GC datasets are
written immediately before they are measured, and the filesystem workload reads files it
has just written. These are the conditions of a GC pass on a recently written tree; they
are not those of the first scan after a reboot.

For a cold-cache measurement, drop the caches between dataset creation and measurement.
This requires root **on the host** (the container has no permission to do it), affects the
whole machine, and is **not done by any TSFS script**:

```bash
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches     # as root: sync; echo 3 > /proc/sys/vm/drop_caches
```

The Google Benchmark binaries create and measure in the same process, so a cold-cache GC
measurement needs a manual sequence: create a dataset (for example with
`tsfs_fs_workload` or a script), drop the caches, then run `tsfs --dry-run` or start the
daemon and time the first scan from its logs. Always state in the report which condition
a number was measured in.

## 5. Recording the environment

A number without its environment is not reproducible. Record with every result:

| Item | Source |
|---|---|
| CPU model, thread count, frequency scaling | Google Benchmark JSON `context` (`num_cpus`, `mhz_per_cpu`, `cpu_scaling_enabled`, caches); `tsfs-bench-fs.sh` header (`/proc/cpuinfo`) |
| Memory | `tsfs-bench-fs.sh` header (`MemTotal`); `free -g` |
| Kernel | `uname -r` (header of `tsfs-bench-fs.sh`) |
| Compiler and build type | `build/release/src/tsfs --version`; `CMakeCache.txt` (header of `tsfs-bench-fs.sh`) |
| libfuse | `tsfs --version` ("built with", "running"); header of `tsfs-bench-fs.sh` |
| Storage device and backing filesystem | `stat -f -c %T <dir>`, `df <dir>`, `lsblk`; header of `tsfs-bench-fs.sh` |
| Dataset and parameters | benchmark name and arguments; `parameters:` line of `tsfs-bench-fs.sh` |
| Cache condition | warm (default) or cold (section 4) |
| Load | Google Benchmark `load_avg`; keep the machine otherwise idle |
| Container | whether the run was inside `tsfs-dev:26.04` (it normally is) |

For the component benchmarks, the JSON `context` block does not include the kernel,
compiler, libfuse version or the scratch filesystem: write them down separately.

**Google Benchmark built as DEBUG.** The Ubuntu `libbenchmark` package used by the image
reports itself as a debug build: the console prints `***WARNING*** Library was built as
DEBUG. Timings may be affected.` and the JSON context contains
`"library_build_type": "debug"`. This refers to the distribution's build of the benchmark
library, not to TSFS (which is built in Release). It may add a small, roughly constant
overhead to the framework's own bookkeeping; it does not change the instrumented TSFS
code. It matters most for nanosecond-scale benchmarks (`BM_GlobBasename`,
`BM_LogFilteredOut`), so prefer relative comparisons there and mention the warning in the
report. Likewise, `CPU scaling is enabled` warns that frequency scaling can add noise:
use the `performance` governor on the host if you can.

## 6. Profiling with perf

Profile a `relwithdebinfo` build (optimized, with symbols):

```bash
scripts/tsfs-dev.sh cmake --preset relwithdebinfo
scripts/tsfs-dev.sh cmake --build --preset relwithdebinfo
```

`perf` is **not installed in the development image**, and TSFS binaries built in the image
generally do not run on an older host (newer glibc and libstdc++). Profiling therefore
happens in a container where perf is available. A possible approach (as a sketch; adapt it
to your host):

1. On the host, allow unprivileged profiling for the session, as root:
   `sudo sysctl kernel.perf_event_paranoid=1` (Ubuntu's default is more restrictive).
2. Start a root shell in the image and install perf:

   ```bash
   docker run --rm -it --user root --cap-add PERFMON --cap-add SYS_ADMIN --device /dev/fuse \
       --security-opt apparmor=unconfined -v "$PWD:/src" -w /src tsfs-dev:26.04 bash
   apt-get update && apt-get install -y linux-tools-generic
   ```

   Ubuntu's `perf` wrapper looks for tools matching the **running (host) kernel**; if it
   refuses, call the installed binary directly (`ls /usr/lib/linux-tools/*/perf`).
3. Record and report. `relwithdebinfo` does not keep frame pointers, so use DWARF call
   graphs:

   ```bash
   perf record --call-graph dwarf -o /src/build/perf.data -- \
       build/relwithdebinfo/benchmarks/tsfs_bench_gc --benchmark_filter='BM_GcDiscover/100000/1/10/'
   perf report -i /src/build/perf.data --no-children
   ```

   For the daemon, start it in the foreground and attach:
   `perf record --call-graph dwarf -p <pid> -- sleep 30`, while
   `tsfs_fs_workload` or `tsfs-bench-fs.sh` drives the mount.

Files written as root inside such a container belong to root on the host; keep them under
`build/` and remove them with `sudo` if needed.

Prefer profiling evidence over speculative micro-optimization: establish a baseline with
the benchmarks above, profile, change one thing, and re-measure under the same recorded
conditions.

## 7. Publishing results

Results, with their environment, conditions and interpretation, go in
[docs/performance/benchmark-report.md](../performance/benchmark-report.md). Never publish
a number that was not measured, and never compare numbers taken under different recorded
conditions without saying so.
