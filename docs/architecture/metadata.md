<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/metadata.md
Purpose : Metadata held by TSFS, file identity, restart recovery, and why there is no persistent index
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Metadata

Related: [garbage collector](garbage-collector.md), [policy engine](policy-engine.md),
[threat model](../security/threat-model.md),
[SPECIFICATION.md](../developer/SPECIFICATION.md) section 8.

## 1. Decision: no persistent metadata

TSFS keeps **no persistent metadata index**: no database, no journal, no
on-disk cache of expiry times. The source tree contains no SQLite or journal
code, and nothing written by the daemon is read back by it:

| File written | Read back by the daemon? |
|---|---|
| Log files (`logging.directory`) | No (the sink only stats the active file to detect external changes). |
| Status file (`metrics.status_file`) | No; only `tsfs --status` prints it. |
| Crash reports (`diagnostics.crash_directory`) | No; only listed for pruning at startup. |

The authoritative metadata is the backing filesystem itself: file type, inode
identity, `st_mtime`, statx birth time, size and link count, read with `statx`
when needed.

## 2. In-memory state

| State | Owner | Content | Bound |
|---|---|---|---|
| Configuration snapshot | `ConfigManager` | `Config`, compiled `PolicyEngine`, generation | one live generation (older ones until their last reader drops them) |
| Expiration scheduler | `GarbageCollector` | `GcCandidate{path, identity, due, attempt, last_error}` for files expiring within `2 x scan_interval` | `max_pending_tasks` |
| Work queue | `GarbageCollector` | candidates handed to workers | `min(max(64, 2 x batch_size x max_workers), max_pending_tasks)` |
| Event queue | `GarbageCollector` | managed paths notified by FUSE | `min(max_pending_tasks, 65536)` |
| Scanner position | `TreeScanner` | pending directories, open directory stream | directory count |
| Dry-run de-duplication | `GarbageCollector` | paths already reported as `would_delete` | 100000 |
| Open-file registry | `OpenFileRegistry` | `(dev, ino) -> open handle count` | open handles |
| FUSE handles | libfuse `fh` | backing descriptor, identity, modified flag | open handles |
| Counters | GC, filesystem, logger | statistics | fixed |

All of it is lost at exit by design and rebuilt at the next start.

## 3. File identity

`FileIdentity` (`src/fsutil/backing_store.hpp`) is the pair `(device, inode)`:

* stable across renames, different after a replacement (new inode under the
  same name, e.g. an atomic `rename` of a new version over the old one);
* recorded when a candidate is scheduled (from `statx`:
  `makedev(stx_dev_major, stx_dev_minor)`, `stx_ino`) and when a file is opened
  through the mount (from `fstat`);
* compared before every deletion: a different identity under the candidate's
  name yields `replaced` or `kept`, never a deletion
  (`GcTest.ReplacedFileIsNeverDeletedOnTheStaleRecord`);
* used as the key of the open-file registry.

Inode numbers can be reused after a file is deleted. A reuse between scheduling
and validation is harmless because the policy is re-evaluated with the new
file's own timestamps before any unlink: the identity check is a fast
"same file" test, not the sole safety condition.

## 4. Recovery after restart

There is nothing to recover from disk. At start the GC runs a full
reconciliation pass immediately (`rescan_requested_ = true`,
`next_scan_ = now`), time-sliced like every other pass:

1. every regular file of the backing tree is `statx`ed and evaluated with the
   current policy;
2. files already expired, or expiring within the horizon, are scheduled;
3. expired files are validated and deleted by the workers as the pass
   progresses.

`GcTest.RestartRecoversWithoutPersistentState` covers it. A crash or `SIGKILL`
loses only in-memory scheduling state; the worst consequence is a delay of one
pass, never an incorrect deletion. Files that expired while the daemon was
stopped are deleted after the restart, as soon as the pass reaches them.

## 5. Why no persistent index

Arguments from the requirements (spec section 8) applied to this design:

| Concern | With a persistent index | Current design |
|---|---|---|
| Safety | The index could disagree with the disk (crash, external change, restore from backup) and must never authorize a deletion alone. | The disk is the only source of truth; every deletion re-checks it. |
| Corruption | Needs detection, repair, and a fallback procedure. | Nothing to corrupt. |
| Startup cost | Index load, then still a verification pass. | One reconciliation pass (time-sliced, does not block FUSE). |
| Write amplification | Every create/write/rename through the mount updates the index. | FUSE only appends a path to a bounded in-memory queue. |
| External changes | Not seen by the index anyway (no inotify on the backing tree). | Seen by the next pass. |
| Memory | Proportional to the number of files if kept in memory. | Proportional to the files expiring within `2 x scan_interval`, capped by `max_pending_tasks`. |
| Complexity | Schema, transactions, migrations, dependency. | None. |

The cost of this choice is the periodic scan: CPU and I/O proportional to the
size of the tree once per `scan_interval`, bounded per slice by
`max_scan_duration`. `benchmarks/bench_gc.cpp` measures discovery
(`BM_GcDiscover`) and startup reconciliation (`BM_GcStartupReconciliation`) on
real on-disk datasets; operators tune
`scan_interval` for very large trees (see [user guide](../user/user-guide.md)).

## 6. Limits

* Lifetimes can only be computed from timestamps the filesystem stores
  (`st_mtime`, birth time); "first seen by TSFS" cannot survive a restart and
  is not offered.
* A file whose `st_mtime` is changed by an external process (for example
  `touch -d` directly in the backing directory) is evaluated with the new value
  at the next pass or validation.
