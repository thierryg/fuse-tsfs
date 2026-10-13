<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/performance/benchmark-report.md
Purpose : Measured benchmark results (baseline), environment and analysis
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Benchmark report - TSFS 0.1.0 baseline (2026-10-13)

Every number below was produced by the commands of section 2 on the machine of
section 1, on 2026-10-13 between 09:21 and 09:25 UTC. Nothing is extrapolated. The
raw outputs are the JSON files written by the `bench` target
(`build/release/bench-results/*.json`) and the JSON lines of
`scripts/tsfs-bench-fs.sh`; how the suite is built is described in
[docs/developer/performance.md](../developer/performance.md).

These are **synthetic benchmarks** (generated datasets, scripted workloads), not
real-world measurements. They establish a baseline; they are not tuned results.

## 1. Environment

| Item | Value |
|---|---|
| CPU | 13th Gen Intel Core i7-1360P (laptop part, 4 P-cores + 8 E-cores, 16 threads, up to 5.0 GHz); CPU frequency scaling **enabled** |
| Memory | 38.8 GiB |
| Kernel | Linux 6.8.0-138-generic (Ubuntu 22.04 host) |
| Toolchain | GCC 15.2 in the `tsfs-dev:26.04` container (Ubuntu 26.04), `-O2`, Release, hardening flags of `CMake/CompilerOptions.cmake` |
| libfuse | 3.18.2 |
| Google Benchmark | Ubuntu 26.04 package (prints "Library was built as DEBUG": the measured code is the TSFS Release build; only the harness library is not optimized) |
| Storage | NVMe SSD (Micron 2550, 512 GB), LVM, **ext4** (`/dev/mapper/vgubuntu-root`), bind-mounted into the container (`TSFS_BENCH_DIR=/src/build/bench-scratch`) |
| Cache state | **Warm**: every dataset is written just before it is measured, so dentries, inodes and data are in the page cache. Cold-cache runs (dropping caches as root) were not performed. |
| Machine load | A desktop session was active (browser, graphical client, endpoint security agent): load average 5 to 10 during the runs. Absolute numbers are therefore noisy (estimated a few percent to ~20 % between runs); the ratios between variants measured in the same run are more reliable than the absolute values. |

## 2. Commands

```sh
# Google Benchmark suites (policy, scheduler, GC, logging), 1k and 100k datasets:
scripts/tsfs-dev.sh env TSFS_BENCH_DIR=/src/build/bench-scratch \
    cmake --build build/release --target bench
# Scheduler with 1,000,000 entries (opt-in):
scripts/tsfs-dev.sh env TSFS_BENCH_LARGE=1 build/release/benchmarks/tsfs_bench_scheduler \
    --benchmark_filter=/1000000
# Filesystem workload: raw backing directory vs TSFS mount (GC off / on):
scripts/tsfs-dev.sh scripts/tsfs-bench-fs.sh -w /src/build/bench-scratch -n 10000 -s 256
```

The 1,000,000-file GC datasets (`TSFS_BENCH_LARGE=1` for `tsfs_bench_gc`) were not
run in this campaign (time and disk usage on a shared workstation).

## 3. Policy engine (CPU only)

| Benchmark | Rules | Time per call | Throughput |
|---|---|---|---|
| Basename glob `clip-*[0-9].mp4` | - | 31.6 ns | 31.7 M/s |
| Path glob `/tmp/record/**/*.mp4` | - | 55.3 ns | 18.1 M/s |
| `evaluate()` on a mixed path set | 8 (spec examples) | 126 ns | 7.96 M/s |
| | 18 | 184 ns | 5.43 M/s |
| | 108 | 747 ns | 1.34 M/s |
| | 1,008 | 4.62 us | 217 k/s |
| `compile()` (reload, policy side) | 8 | 2.72 us | - |
| | 108 | 110 us | - |
| | 1,008 | 1.60 ms | - |

Evaluation is a linear scan in precedence order: its cost grows linearly with the
number of rules that do not match. With the realistic rule counts of the
specification (under ~20), a decision costs well under a microsecond, negligible
next to the `statx` that precedes it during a scan.

## 4. Expiration scheduler: heap vs ordered

Throughput in operations per second (higher is better); "fill" and "drain" are the
whole operation on N entries.

| Operation | N | heap | ordered |
|---|---|---|---|
| Fill (schedule N new paths) | 1,000 | 4.34 M/s | 2.82 M/s |
| | 100,000 | 2.47 M/s (40.5 ms) | 1.02 M/s (98.4 ms) |
| | 1,000,000 | 1.30 M/s (768 ms) | 694 k/s (1.44 s) |
| Reschedule an existing path | 1,000 | 10.1 M/s | 2.71 M/s |
| | 100,000 | 3.81 M/s | 799 k/s |
| | 1,000,000 | 3.24 M/s | 409 k/s |
| Drain in due order (batches of 64) | 1,000 | 5.98 M/s | 6.47 M/s |
| | 100,000 | 2.09 M/s | 2.74 M/s |
| | 1,000,000 | 1.32 M/s | 1.67 M/s |
| Cancel | 1,000 | 8.04 M/s | 4.54 M/s |
| | 100,000 | 5.31 M/s | 1.49 M/s |
| | 1,000,000 | 3.03 M/s | 895 k/s |

The heap (default) is faster on every mutation (2.5x to 8x on reschedule and cancel
at 100k-1M entries); the ordered set drains about 25 % faster. Files are scheduled
and rescheduled far more often than they are drained, so **the default `heap`
choice is supported by the measurements**. Even at one million live entries the
heap sustains over 3 million reschedules per second: the scheduler is not a
bottleneck at the default `max_pending_tasks: 100000`.

## 5. Garbage collector (backing directory, no FUSE)

### 5.1 Discovery and scheduling (one reconciliation pass)

| Files | Layout | Expired | Pass duration | Files/s | Peak RSS |
|---|---|---|---|---|---|
| 1,000 | flat | 100 % | 1.02 ms | 978 k/s | 6.3 MiB |
| 1,000 | tree (100 per dir) | 10 % | 1.42 ms | 706 k/s | 6.3 MiB |
| 100,000 | flat | 100 % | 198 ms | 506 k/s | 36.7 MiB |
| 100,000 | flat | 10 % | 177 ms | 566 k/s | 36.7 MiB |
| 100,000 | tree | 100 % | 161 ms | 622 k/s | 39.2 MiB |
| 100,000 | tree | 10 % | 178 ms | 561 k/s | 39.2 MiB |

Peak RSS is the process-wide high-water mark of the benchmark process (dataset
bookkeeping included), not the GC alone. With every file expiring within the
horizon, the queue depth equals the number of files (the scheduler held 100,000
entries).

### 5.2 Deletion (validation + unlink), single thread, mixed sizes (0 to 64 KiB)

| Files | Layout | Time | Files/s | Bytes/s |
|---|---|---|---|---|
| 1,000 | flat | 14.2 ms | 70.7 k/s | 1.36 GiB/s |
| 1,000 | tree | 12.9 ms | 77.8 k/s | 1.50 GiB/s |
| 100,000 | flat | 1.69 s | 59.1 k/s | 1.14 GiB/s |
| 100,000 | tree | 1.36 s | 73.6 k/s | 1.42 GiB/s |

Every deletion re-opens the parent under the path locks, re-stats the file, checks
identity, type, device and open handles and re-evaluates the policy before
`unlinkat`: about 14 us per file on this machine, most of it in the kernel.

### 5.3 End to end with the real threads (start() until every file is deleted)

| Files | Workers | Time | Files/s | Worker utilization |
|---|---|---|---|---|
| 1,000 | 1 | 14.0 ms | 71.7 k/s | 0.83 |
| 1,000 | 2 | 17.6 ms | 56.9 k/s | 0.77 |
| 1,000 | 4 | 24.3 ms | 41.1 k/s | 0.71 |
| 1,000 | 8 | 17.2 ms | 58.2 k/s | 0.73 |
| 100,000 | 1 | 1.51 s | 66.3 k/s | 0.84 |
| 100,000 | 2 | 1.72 s | 58.1 k/s | 0.89 |
| 100,000 | 4 | 2.21 s | 45.2 k/s | 0.91 |
| 100,000 | 8 | 2.01 s | 49.7 k/s | 0.92 |

**Finding: more workers do not delete faster here; they are slower.** The workers
are busy (utilization 0.84 to 0.92) yet the throughput drops from 66 k/s (1 worker)
to 45-50 k/s (4-8 workers). The dataset is a tree of directories holding 100 files
each, deleted in expiry order, so concurrent workers mostly unlink entries of the
**same directory**, which the kernel serializes on that directory's inode lock;
the extra threads add contention (and the path-lock stripes and the dispatch
mutex) without adding parallelism. This was not profiled further in this campaign
(no `perf` in the image), so the attribution to the directory lock is the most
plausible explanation, not a measured fact.

Consequences:
- the default `max_workers: 2` costs about 12 % versus 1 worker on this workload;
  with the default `max_deletions_per_second: 100` the rate limit, not the worker
  count, bounds deletion in production;
- raising `max_workers` is only worth testing when expired files are spread over
  many directories or the backing storage has high latency (network filesystems),
  where waiting on I/O dominates; this is documented in the user guide
  (performance tuning).

### 5.4 Startup, recovery and reload

| Benchmark | 1,000 files | 100,000 files |
|---|---|---|
| Startup: `start()` until the first complete pass (nothing to delete) | 3.80 ms | 157 ms (635 k files/s) |
| Reload: `apply()` of a new generation until its pass completes | 1.03 ms | 133 ms (753 k files/s) |

Recovery after a restart is this first pass (no persistent index: see
[metadata](../architecture/metadata.md)); at ~0.6 M files/s, 1 M files take on the
order of 1.6 s of scanning (extrapolation, not measured).

## 6. Logging

| Benchmark | Threads | Time per message | Messages/s |
|---|---|---|---|
| Call below the active level (filtered) | 1 | 7.35 ns | 136 M/s |
| Synchronous file sink | 1 | 1.41 us | 710 k/s |
| Synchronous file sink | 4 | 15.6 us (wall) | 63.9 k/s |
| Asynchronous queue (block policy) | 1 | 1.53 us | 652 k/s |
| Asynchronous queue (block policy) | 4 | 6.99 us (wall) | 143 k/s |

Disabled levels cost one atomic load; under contention the asynchronous pipeline
(default) delivers 2.2x the throughput of synchronous writes.

## 7. Filesystem: raw backing directory vs TSFS mount

10,000 small (4 KiB) files, one 256 MiB file in 1 MiB requests, 4 threads for the
concurrent phase. `gc-on` runs the GC with a retention that matches every file but
does not expire during the run (scans every second and change notifications, no
deletion).

| Operation | raw ext4 | TSFS, GC off | TSFS, GC on | Mount / raw (GC off) |
|---|---|---|---|---|
| create + 4 KiB write + close | 47.6 k/s | 8.18 k/s | 5.60 k/s | 0.17 |
| stat (metadata lookup) | 930 k/s | 112 k/s | 54.2 k/s | 0.12 |
| readdir (10,000 entries) | 6.16 M/s | 1.77 M/s | 2.18 M/s | 0.29 |
| open + 4 KiB read + close | 333 k/s | 20.7 k/s | 18.2 k/s | 0.06 |
| rename | 71.9 k/s | 11.2 k/s | 10.6 k/s | 0.16 |
| unlink | 134 k/s | 13.7 k/s | 11.2 k/s | 0.10 |
| sequential write (fsync included) | 625 MiB/s | 192 MiB/s | 203 MiB/s | 0.31 |
| sequential read (warm cache) | 5,294 MiB/s | 1,401 MiB/s | 1,566 MiB/s | 0.26 |
| concurrent create (4 threads) | 152 k/s | 33.1 k/s | 21.2 k/s | 0.22 |

Observations:
- The FUSE round trip dominates: metadata operations run at 6 % to 17 % of the
  native speed, data streaming at 26 % to 31 %. This is the usual cost of a
  high-level libfuse passthrough without kernel-side passthrough; it is the baseline
  against which future optimizations must be measured.
- With the GC enabled, create, stat and concurrent create lose a further 30 % to
  50 % on this run, while the streaming numbers are within noise (gc-on was even
  faster on the sequential tests). The GC scans the whole tree every second here
  (`scan_interval: 1s`, deliberately aggressive) and every create sends a
  notification; with the default 60 s interval the scan share would be much
  smaller. This run alone does not separate the two causes.
- Candidate optimizations, **not implemented and not measured**: longer
  `entry_timeout`/`attr_timeout` (fewer lookups), `writeback_cache`, kernel FUSE
  passthrough for open files (Linux >= 6.9, libfuse >= 3.17), batching
  notifications. Each must be justified by a measurement first.

## 8. Summary

| Question | Answer from the measurements |
|---|---|
| Is policy evaluation a bottleneck? | No: 0.13-0.75 us per file for 8-108 rules. |
| Which scheduler? | heap (2.5-8x faster mutations; drains ~25 % slower). |
| Scheduler at 1 M entries? | 1.3 M inserts/s, 3.2 M reschedules/s (heap). |
| Discovery speed | 0.5-0.6 M files/s per pass (warm cache, ext4, NVMe). |
| Deletion speed | ~60-74 k files/s single-threaded, revalidation included. |
| Do more workers help? | Not on a local SSD with clustered directories: 1 worker was fastest. |
| FUSE overhead | 3x-17x depending on the operation (see section 7). |

Re-run the commands of section 2 on the target hardware before relying on any of
these values; compare variants within one run.
