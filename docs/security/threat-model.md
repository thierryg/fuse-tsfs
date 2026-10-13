<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/security/threat-model.md
Purpose : Threat model: assets, trust boundaries, attackers, mitigations, residual risks, security tests
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# TSFS threat model

This document covers section 21 of the specification
([SPECIFICATION.md](../developer/SPECIFICATION.md)). It describes what TSFS protects,
who it protects it from, the controls the code actually implements, and what it does
**not** guarantee. Code references point to the 0.1.x sources. A reference names a
function when line numbers are likely to move.

TSFS is a passthrough FUSE filesystem. A garbage collector (GC) deletes files whose
retention period has expired. Because the GC deletes files on its own, often as root,
**deletion is the most sensitive operation in the system**. Most of the controls below
exist to make sure the GC deletes only the regular file it evaluated, at the name it
evaluated, inside the backing directory, and only after its retention has expired.

Related documents: [architecture overview](../architecture/overview.md),
[filesystem](../architecture/filesystem.md),
[garbage collector](../architecture/garbage-collector.md),
[concurrency](../architecture/concurrency.md), [logging](../architecture/logging.md),
[diagnostics](../architecture/diagnostics.md), [user guide](../user/user-guide.md),
[testing](../developer/testing.md), [test strategy](../testing/test-strategy.md).

---

## 1. Assets

| Asset | Why it matters |
|---|---|
| Files outside the backing directory (host `/etc`, `/home`, other mounts) | The GC must never delete them, and FUSE requests must never read or write them. A path escape from the backing directory turns TSFS into a root-owned deletion primitive. |
| Files inside the backing directory that are not expired, excluded, or open | Deleting one of them is silent data loss. |
| Integrity of file ownership and permissions in the backing tree | The daemon usually runs as root and creates entries on behalf of users. |
| The configuration file | It defines the deletion policy and the paths the daemon writes to (logs, crash reports, status file). |
| Logs, crash reports, status file | Logs and reports contain managed paths and file names. Crash reports also contain memory maps and register dumps. |
| The retention policy itself | Rule identifiers and expiries reveal what is kept and for how long. Part of it is visible to mount users through the retention extended attributes (6.3). |
| The system `ls` | Renamed `/usr/bin/ls.orig` and wrapped by `tsfs-ls` (installed as `/usr/bin/ls`) at a default root installation; every user and many scripts run it (6.8). |
| Availability of the mount and of the host | Includes memory, file descriptors, disk space used by logs, and CPU. |

## 2. Trust boundaries

```
                trusted                              untrusted / partially trusted
 +--------------------------------+        +------------------------------------------+
 | administrator (root)           |        | users of the mount (local, unprivileged) |
 |  - config file, CLI, systemd   |        |  - file names, contents, timestamps,     |
 |  - backing_directory path      |        |    renames, symlink targets, hard links  |
 +---------------+----------------+        +---------------------+--------------------+
                 |                                               | FUSE requests (kernel
                 v                                               v  checks permissions)
 +-------------------------------------------------------------------------------------+
 | tsfs daemon (root under systemd)                                                     |
 |   PassthroughFs  --+                                                                  |
 |   GarbageCollector-+--> BackingStore (root fd, openat2 / O_NOFOLLOW walk) --> backing |
 |   Logger, CrashHandler, status writer --> log dir, crash dir, status file             |
 +-------------------------------------------------------------------------------------+
                 ^
                 | direct access to the backing directory, without going through the mount
 +---------------+--------------------------------------------------------------------+
 | any process that can write to the backing directory directly (host namespace)       |
 +-------------------------------------------------------------------------------------+
```

* **Trusted:** the administrator, the configuration file, the command line, the systemd
  unit, the kernel, libfuse, and the path of the backing directory. The backing directory
  root itself may be a symbolic link because it comes from the trusted configuration
  (`BackingStore::open()`, `src/fsutil/backing_store.cpp`).
* **Untrusted:** everything below the backing root. This covers entry names, entry
  types, symbolic link targets, timestamps, hard links, renames, and any mount created
  below the root. It applies to changes made through the mount and to changes made
  directly in the backing directory.
* **Boundary that TSFS cannot fully enforce:** processes that write directly into the
  backing directory bypass the path locks, the open-file registry and the change
  notifications (see [Residual risks](#10-residual-risks)). The deployment has to make
  the backing directory unreachable for unprivileged users (see the hardening checklist).

## 3. Attackers

| Attacker | Capabilities assumed |
|---|---|
| A1 Local unprivileged user of the mount | Creates, writes, renames and deletes entries through the mount within kernel permission checks. Chooses arbitrary names (any byte except `/` and NUL), symbolic link targets and timestamps on files they own (`touch -d`). |
| A2 Local user with direct write access to the backing directory | Everything A1 can do, plus changes outside the daemon's knowledge. Can swap a directory for a symbolic link, rename files over others, or create entries while the GC validates a candidate. |
| A3 User racing the GC | A1 or A2 timing renames, symlink swaps or replacements between the moment the GC evaluates a candidate and the moment it deletes it. |
| A4 Malicious or careless configuration author | Anyone who can write the configuration file. The file is **trusted**, so TSFS limits the damage of mistakes but cannot defend against a hostile author (see 6.4). |
| A5 Log and resource flooder | Generates errors, notifications or huge directory trees to fill the disk, exhaust memory or hide events in the logs. |
| A6 Disk-full / faulty storage condition | ENOSPC, EIO or a read-only backing filesystem. Not malicious, but must not cause unsafe behavior. |
| A7 Someone who controls the clock | An administrator, NTP or a VM snapshot that steps the wall clock. |

Out of scope: root on the host, kernel or libfuse vulnerabilities, physical access, and
network attackers. TSFS opens no network socket. The systemd unit restricts it to
`AF_UNIX`.

## 4. Attack surfaces

1. **FUSE operations** (`src/filesystem/passthrough_fs.cpp`). Each request carries a path
   chosen by the caller, plus modes, flags, sizes, timestamps and link targets.
2. **The backing tree as seen by the GC** (`src/gc/tree_scanner.cpp`,
   `src/gc/garbage_collector.cpp`). Names, types, timestamps, hard links, symbolic links
   and mount points, all of which may change at any time.
3. **Configuration loading and reload** (`src/config/config_loader.cpp`,
   `src/config/config_watcher.cpp`). YAML parsing, path values, and inotify plus `SIGHUP`
   triggers.
4. **Output files** (logs, rotated or compressed logs, crash reports, status file) and
   their directories.
5. **Signals** (`SIGHUP` reload, `SIGUSR1` status, `SIGUSR2` purge, `SIGTERM`/`SIGINT`
   shutdown). Only the process owner or root can send them.
7. **`tsfs-ls`** (`src/tools/tsfs_ls.cpp`), installed as `/usr/bin/ls` by default: its
   command line, environment (`TSFS_LS`, `TSFS_REAL_LS`, `PATH`), the output of the
   original `ls` it parses, and the extended attributes it reads.
6. **Crash diagnostics**: optional native core dumps (`diagnostics.native_core_dump`).

## 5. Security properties claimed

The following properties are enforced by the code. Anything not listed here is not
claimed.

* **P1:** Every access to the managed tree is relative to one root descriptor, and an
  intermediate symbolic link is never followed.
* **P2:** A managed path containing `.` or `..` components, empty components, or NUL is
  rejected before any system call.
* **P3:** When `security.stay_on_filesystem` is `true` (the default), no mount point below
  the backing root is crossed by an intermediate component when openat2(2) is used. With
  the fallback walk, only a mount of a **different device** is detected. The final
  component is checked by comparing its `st_dev` with the root device, in the GC and in
  the FUSE handlers that reach an existing entry (EXDEV); a bind mount of the **same**
  filesystem on the final component is not detected by that check (see 7.2).
* **P4:** The GC only deletes a candidate after re-validating it under the path locks.
  The candidate must still be a regular file on the root device, have the same
  (device, inode) identity it had when it was scheduled, be expired under the current
  policy and clock, and not be open through the mount.
* **P5:** The GC never deletes directories, symbolic links, FIFOs, sockets or device
  nodes.
* **P6:** No shell is invoked and no command line is built from untrusted data.

## 6. Mitigations

### 6.1 Path resolution (`src/fsutil/backing_store.cpp`)

* The root is opened once with `open(backing_directory, O_PATH | O_DIRECTORY | O_CLOEXEC)`
  (`BackingStore::open()`). Every later access goes through `BackingStore::open_parent()`
  or `BackingStore::open_directory()`. No component builds a host path string to access
  managed files.
* `open_parent()` and `open_directory()` first call `managed_path::is_normalized()`
  (`src/common/managed_path.cpp`). That check rejects relative paths, `.` and `..`
  components, empty components, a trailing `/`, and embedded NUL bytes (EINVAL). The
  containment check therefore does not rely on string prefixes. The kernel enforces it.
* **openat2 path** (`BackingStore::resolve()`, lines 81 to 83). The code uses
  `how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS`, adds
  `RESOLVE_NO_XDEV` when `stay_on_filesystem` is set, and passes the open flags
  `O_NOFOLLOW | O_CLOEXEC`. `RESOLVE_NO_SYMLINKS` refuses symbolic links in **every**
  component, the final one included. Callers resolve only the parent directory, then
  handle the final name with `*at()` calls (see below). EAGAIN and EINTR (a concurrent
  rename raced with the lookup) are retried in a loop.
* **Fallback walk** (`BackingStore::resolve_fallback()`, line 98). It is used on kernels
  without openat2 (< 5.6) and in tests through `Options::force_fallback`. The walk opens
  one component at a time with `openat(..., O_PATH | O_DIRECTORY | O_NOFOLLOW)` and
  `O_NOFOLLOW` on the last component. When `stay_on_filesystem` is set, it also compares
  each component's `st_dev` with the root device (line 121) and fails with EXDEV.
* `security.require_openat2: true` refuses to start when openat2 is missing (ENOSYS, exit
  code for missing dependencies). `kernel_supports_openat2()` probes the syscall once.
* The final component is always handled with `AT_SYMLINK_NOFOLLOW` or `O_NOFOLLOW`. This
  applies to `stat_entry()` (`src/fsutil/entry_stat.cpp`, `statx(..., AT_SYMLINK_NOFOLLOW)`)
  and to `openat`, `fchmodat`, `fchownat`, `utimensat` and `truncate` in
  `PassthroughFs`. `unlinkat` and `renameat2` never follow the final component.
* The resolvers only check the **intermediate** components against mount boundaries. With
  `stay_on_filesystem`, the final component is checked by its caller:
  `PassthroughFs::check_device()` and `check_name_device()` return EXDEV when the entry
  lives on another device than the backing root. They guard `getattr`, `access`, `open`,
  `create` (when an existing file is opened), path-based `truncate`, `chmod`, `chown` and
  `utimens`; the GC performs the same device check before deleting (6.2). See the comment
  of `BackingStore` in `src/fsutil/backing_store.hpp`.

### 6.2 Validation of deletion candidates (`GarbageCollector::process_candidate()`)

The scanner and the notification path only **schedule** candidates. The scanner never
follows `DT_LNK` entries and counts them as symlinks (`TreeScanner::run()`). Each
scheduled candidate stores the (device, inode) identity seen at evaluation time. The
deletion itself happens in `process_candidate()` (`src/gc/garbage_collector.cpp`) and
runs entirely while the following guard is held:

1. `deps_.locks.lock_with_ancestors(candidate.path)` locks the stripe of the candidate and
   of **every ancestor directory** (`src/fsutil/path_lock_table.hpp`). FUSE operations
   lock the stripes of the paths they modify (`open`, `create`, `mknod`, `mkdir`,
   `unlink`, `rmdir`, `symlink`, `rename`, `link`, `truncate`, `utimens`). As a result, neither the
   file nor a parent directory can be renamed or replaced *through the mount* between
   validation and unlink. Stripes are taken in increasing index order with duplicates
   removed, which prevents deadlocks.
2. `store.open_parent(path)` resolves the parent again, safely (6.1). A symbolic link or
   mount point that appeared in the meantime produces ELOOP, EXDEV or ENOTDIR. That
   outcome is classified as `unsafe`, the GC does not delete, and a rate-limited warning
   `gc_file_skipped reason=unsafe` is logged.
3. `stat_entry(parent_fd, name)` with `AT_SYMLINK_NOFOLLOW`. A missing entry is classified
   as `vanished` and is not an error.
4. The entry must be a regular file. With `stay_on_filesystem`, its device must also equal
   the root device. Otherwise the outcome is `unsafe`.
5. The (device, inode) identity must equal the identity recorded at scheduling time. If
   it differs, the outcome is `replaced` and the GC deletes nothing. The new inode is
   rescheduled under its own identity and is evaluated from scratch. The GC **never
   deletes on a stale record**.
6. The policy is evaluated again with the **current** policy snapshot and the current
   clock, so an exclusion added by a reload is honored. A file that is no longer
   expired, for example rewritten or touched, is classified as `not_expired` or `kept`.
7. When `garbage_collector.skip_open_files` is `true` (the default), a file open through
   the mount is skipped (`OpenFileRegistry::is_open(identity)`, outcome `busy_open`) and
   retried after `retry_initial_delay`. `PassthroughFs::open` and `create` register the
   handle while they hold the same path lock. Either the GC sees the handle or the open
   sees ENOENT.
8. In dry-run mode the outcome is `would_delete` and nothing is removed.
9. Otherwise `unlinkat(parent_fd, name, 0)` is called **relative to the validated parent
   descriptor**. The `0` flag means unlinkat never removes a directory.

Failures are classified by `is_transient()`. EACCES, EPERM, EBUSY, EIO and ENOSPC are
retried with exponential backoff (`retry_initial_delay` x 2^(attempt-1), so the first
retry waits the initial delay, capped at `retry_max_delay`) up to `retry_limit`. EROFS, EISDIR, ELOOP, EXDEV and
EINVAL are final. Retries are bounded.

Hard links: unlinking a candidate removes only that name. Data that is still reachable
through another link is not counted in `bytes_deleted`.

### 6.3 FUSE operations (`src/filesystem/passthrough_fs.cpp`, `src/filesystem/fuse_session.cpp`)

* The mount always uses `default_permissions` (`FuseSession::mount()`). The **kernel**
  checks mode bits, ownership, the sticky bit, `fs.protected_hardlinks` and `chown`
  rights against the attributes TSFS reports, using the caller's credentials. The daemon
  does not reimplement those checks. Its `access()` handler only reports existence,
  EROFS and EXDEV (final component on another device).
* `mknod` refuses character and block devices with EPERM. Those nodes would bypass the
  permission model of the mount.
* Every final-name operation uses `O_NOFOLLOW` or `AT_SYMLINK_NOFOLLOW`. `chmod` and
  `chown` on a symbolic link never reach the link target.
* A symbolic link created through the mount stores the caller's target verbatim. The
  daemon never follows it. A client that follows the link resolves it in its own mount
  namespace with its own credentials, so the link does not give the client any extra
  access.
* When the daemon runs as root, new entries are handed over to the calling user (unless
  the caller is root). `give_to_caller(dirfd, name, type)` (`mknod`, `mkdir`, `symlink`)
  re-opens the new entry with `O_PATH | O_NOFOLLOW` and changes the ownership **on that
  descriptor** (`fchownat(fd, "", ..., AT_EMPTY_PATH)`), never by name. It does so only
  if the descriptor still designates the entry created here: expected file type, owned
  by the daemon's effective uid, and, for a directory, a link count of 2 (empty).
  Otherwise the ownership is left untouched and a warning is logged. `create()` opens with
  `O_EXCL` first and calls `give_fd_to_caller()` on the new descriptor only if it actually
  created the file; a file that appeared meanwhile (written directly into the backing
  directory) is opened as is when the caller did not ask for `O_EXCL`, and its ownership
  is not changed. The owner is the caller's uid, and the group is either the caller's gid
  or the parent's gid when the parent is set-group-ID. If the change fails, the entry
  stays owned by root, which is the safe direction. All four creating operations hold the
  path lock. Validated as root in the development container: files, directories,
  symbolic links and FIFOs end up owned by the creating user.
* `hard_remove = 1`: an unlinked open file is really unlinked, and no `.fuse_hidden*`
  files are left in the backing tree.
* `read_only: true` mounts with `ro` and returns EROFS from every mutating handler. **The
  GC still deletes expired files on a read-only mount.** The read-only setting protects
  the backing tree from users, not from the retention policy. Use
  `garbage_collector.enabled: false` or `dry_run: true` to stop deletions.
* Exceptions never unwind into libfuse. The trampoline converts `std::bad_alloc` to
  ENOMEM and any other exception to EIO.
* **Retention extended attributes** (`getxattr`, `filesystem.retention_xattrs`, default
  `true`). Read-only: `setxattr` and `removexattr` are not implemented, and the real
  extended attributes of the backing files (ACLs, security labels, `user.*`) are never
  exposed through the mount. A name outside `user.tsfs.state`, `user.tsfs.expires`,
  `user.tsfs.rule` and `user.tsfs.created` returns ENODATA without touching the disk;
  the four names resolve
  the path with the same `BackingStore` rules as every other operation (no symlink
  following, device check) and only answer for regular files. **Information
  disclosure:** with `default_permissions`, the kernel applies its usual check to a
  `user.*` attribute (`xattr_permission()`: search permission on the path and read
  permission on the file), so in practice any user who can reach and read a file
  through the mount, which for a mode 0644 file means every user of the mount, can read
  its expiry, its state, the identifier of the deciding rule and its creation time
  (`user.tsfs.created`: birth time of the backing file, else its modification time).
  This reveals part of
  the policy (for example that `/hr/` files are deleted
  after 30 days, or the name of an exclusion rule). Name rules neutrally, or set
  `retention_xattrs: false` when this matters. `listxattr` returns an empty list.

### 6.4 Configuration (`src/config/`)

* Strict YAML schema: unknown and duplicate keys are rejected, and values carry ranges.
  For example, `max_pending_tasks` must be between 16 and 10,000,000. Errors include the
  line and column. The file is limited to 4 MiB (`load_config_file()`).
* `retention.default` is `never` by default. The shipped example sets
  `garbage_collector.dry_run: true` (see `systemd/tsfs.service.in`).
* The mount point and the backing directory must not be `/` and must not contain each
  other (`parse_filesystem()`). The log directory, crash directory and status file must
  be outside both (`cross_checks()`). The `-m`/`--mountpoint` and `-b`/`--backing-dir`
  overrides go through the same normalization and checks (`validate_path_overrides()`,
  called by `ConfigManager::build()`); errors exit with code 5 and a message starting
  with `command line:`. These are **component-wise string checks** on the
  configured host paths (`managed_path::is_within`). They catch mistakes. They do not
  resolve symbolic links and are not a security boundary.
* A failed reload keeps the running configuration (`ConfigManager`). Restart-only
  settings (`filesystem.*`, `security.*`, `diagnostics.*`) keep their running values
  until the daemon restarts.
* **File trust** (`check_config_file_trust()`, called by `load_config_file()` at startup,
  on every reload and for `--check-config`). The file is opened (symbolic links are
  followed, so the **target** is checked) and examined with `fstat()` on the open
  descriptor. It must be a regular file. When the daemon's effective uid is 0, it must
  also be owned by root and not be writable by its group or by others; otherwise loading
  fails with EACCES and the process exits with code 5 (message ending in
  `fix with chmod go-w` for the mode case). An unprivileged daemon only requires a
  regular file. A failed check during a reload keeps the running configuration.
* **Limit:** the directory containing the file, and the directories along a symbolic link
  to it, are not checked. Anyone who can write the file, or replace it, controls the
  deletion policy for the whole backing tree. Because `logging.*` is reloadable, that
  person can also choose the directories where a root process creates, rotates and
  removes `<filename>.N` files. Keep the file `root:root 0644` or stricter, in a
  directory only root can write.

### 6.5 Logs, crash reports, status file, temporary files

| Output | Creation | Mode |
|---|---|---|
| Log directory | `make_directories(..., directory_mode)` | `0750` |
| Active log file | `open(..., O_WRONLY \| O_APPEND \| O_CREAT \| O_NOFOLLOW \| O_CLOEXEC)`, must be a regular file | `0640` |
| Compression temporary file | `openat(dirfd, ".<file>.compress-<inode>.tmp", ... O_NOFOLLOW)`, verified, then `renameat` | `0640` |
| Crash directory | `mkdir` per component | `0750` |
| Crash report | `openat(dir_fd, "tsfs-crash-<sec>-<pid>.txt", O_CREAT \| O_EXCL \| O_NOFOLLOW)` | `0600` |
| Status file directory | `mkdir` (last component only) | `0750` |
| Status file | `mkostemp("<path>.tmp.XXXXXX")` (unpredictable name, `O_EXCL`), `fchmod(0640)`, write, then `rename()` | `0640` |

The daemon calls `umask(0)` at startup (`Daemon::run()`, `src/app/application.cpp`), so
the modes above are the final modes: every file and directory the daemon creates for
itself has an explicit mode (logs `file_mode`/`directory_mode`, default `0640`/`0750`;
crash reports `0600`; status `0640`; directories `0750`). The systemd unit also sets
`LogsDirectoryMode=0750` and `StateDirectoryMode=0700`. The default status directory is
`/run/tsfs`; the unit does not create it, the daemon does (`0750`).

Status writes are serialized by a mutex in the daemon (the housekeeping thread and the
`SIGUSR1` handler both write the file). Because the temporary name is unpredictable and
created with `O_EXCL`, a file or link planted in the status directory is never opened
or overwritten.

Log injection: every field value is quoted and escaped (`src/common/escape.hpp`). Newline,
CR, tab, quote, backslash, other control bytes and invalid UTF-8 are escaped, so a
crafted file name cannot forge a log line in either the text or the JSON format.

Crash reports contain `/proc/self/maps` (the address-space layout), the fault and
instruction addresses, and a raw and symbolized backtrace (function names of the
loaded objects). Native core dumps, when enabled, contain process memory, which can include
file data and paths. `enable_core_dumps()` sets `PR_SET_DUMPABLE` to 1. Enable them only
when needed, and protect the place `core_pattern` writes to.

### 6.6 No shell, no command construction

The daemon contains no `system()`, `popen()`, `fork()`, `exec*()` or `posix_spawn()`
(checked with `grep -rn` on `src/`). Paths are passed to system calls only. The only
external program involved is `fusermount3`, which **libfuse** executes for unprivileged
mounts and unmounts with a fixed argument vector. The configuration reload
(`ExecReload=/bin/kill -HUP $MAINPID`) is part of the trusted unit.

### 6.7 Bounded resource consumption

| Resource | Bound |
|---|---|
| Change notifications from FUSE | `min(max_pending_tasks, 65536)` queued paths. Extra notifications are counted in `events_dropped`, and the next reconciliation pass finds the files anyway. |
| Scheduler | `max_pending_tasks` candidates. A full scheduler is counted in `queue_saturations`, logged with a rate limit, and recovered by later passes. |
| Work queue | `min(max(64, batch_size x max_workers x 2), max_pending_tasks)` candidates. |
| Deletion rate | Token bucket `max_deletions_per_second` (default 100, 0 means unlimited) and `max_deletions_per_cycle`. |
| Scan time per slice | `max_scan_duration`. The scan is resumable, so notifications keep being served. |
| Coordinator sleep | At most 60 s (`kMaxSleep`), which bounds how late a wall-clock step is noticed. |
| Repetitive warnings | `LogRateLimiter`: 10 per key per minute, with the count of suppressed messages. Ordinary client errors (ENOENT, EACCES and similar) are logged at `trace` level only. |
| Log disk usage | Rotation: roughly `max_file_size` x (`max_files` + 1) when rotation is enabled. Disk-full errors are counted (`lost_writes`) and never cause recursive logging. |
| Async log queue | `queue_size` entries. The overflow policy is `block` (default), `discard_new` or `overrun_oldest`. Losses are counted and reported. |
| Configuration size | 4 MiB. |
| Retries | `retry_limit` attempts with exponential backoff capped at `retry_max_delay`. |

Not bounded: the list of directories that the scanner still has to visit
(`TreeScanner::pending_`) grows with the number of directories in the backing tree (see
R8).

### 6.8 The `tsfs-ls` replacement of the system `ls`

By default (`TSFS_REPLACE_SYSTEM_LS=ON`), `cmake --install` run as root without
`DESTDIR` renames the system `/usr/bin/ls` to `/usr/bin/ls.orig` (`rename(2)`, no copy:
a regular file such as GNU `ls`, or a symbolic link such as the uutils coreutils
multi-call binary of Ubuntu 26.04, is kept exactly as it is) and installs `tsfs-ls` as
`/usr/bin/ls`; the `uninstall` target renames `ls.orig` back. The project chose this
replacement deliberately, over safer alternatives (a separate command, a shell alias);
its consequences:

* **Package integrity.** `/usr/bin/ls` belongs to `coreutils`: `dpkg --verify coreutils`
  and checks based on `dpkg -S` report it while the wrapper is in place (the file dpkg
  installed now lives under another name), and file-integrity monitors (AIDE,
  Tripwire, IMA) flag it. A coreutils upgrade or reinstallation puts the system `ls`
  back over the wrapper; the next TSFS installation renames it to `ls.orig` again,
  replacing the stale one.
* **Blast radius.** Every user, every login shell and many scripts run `ls`. A crash or
  a wrong listing in the wrapper would affect all of them. Mitigations: the original
  `ls` runs with the identical command line (`execv`) whenever standard output is not a
  terminal (all pipes and redirections, hence nearly all scripts, get exactly the
  system `ls` output), no operand is inside a `fuse.tsfs` mount, `TSFS_LS=off`, or the
  options include `--help`, `--version`, `--zero`, `--dired` or `-D`. Even inside a TSFS
  mount, the listing itself is produced by the original `ls` (run with `-l --dired` after the user's options
  prepended); the wrapper only splits its output with the dired byte offsets and
  appends the tags. Output without a dired trailer is printed unchanged.
* **Which `ls` runs.** The original is `$TSFS_REAL_LS`, then `/usr/bin/ls.orig`, then
  the first `ls` of `PATH` that is not the wrapper (marker `TSFS-LS-WRAPPER-v2`), always
  with `argv[0]` = `ls`. `TSFS_REAL_LS` and `PATH` come from the caller's own
  environment, so they only let a user run a program of their choice with their own
  rights, which they can do anyway. `tsfs-ls` is not setuid and gains no privilege.
  Running `/usr/bin/ls.orig` directly works for GNU `ls` and for uutils (the multi-call
  binary is reached through a link named `ls`).
* **What it reads.** `/proc/self/mountinfo`, the captured output of the original `ls`,
  and `lgetxattr()` of the `user.tsfs.state`, `rule`, `expires` and `created`
  attributes of each listed entry (no symlink following). Control characters in the
  printed lines become `?`, so a crafted file name cannot send escape sequences to the
  terminal; colors are not shown in TSFS listings.
* **Installation safety.** Only as root and never with `DESTDIR`. The wrapper is copied
  to `/usr/bin/ls.tsfs-new` (mode 0755) then renamed over `/usr/bin/ls`, so `/usr/bin/ls`
  is never partial; it is absent only briefly during a first installation, between
  the rename of the system `ls` and the rename of the wrapper. If `/usr/bin/ls` is
  already the wrapper but `ls.orig` is missing, the installation fails
  (`FATAL_ERROR`, asking for `apt install --reinstall coreutils`) rather than leave no
  original. Verified as root in a disposable container with uutils coreutils 0.8.0:
  install, reinstall, uninstall restoring the identical symbolic link (same target).
* **Opting out.** `-DTSFS_REPLACE_SYSTEM_LS=OFF` at build time (`tsfs-ls` stays
  available under its own name), `TSFS_LS=off` per shell.

## 7. Mount namespaces, mount boundaries and bind mounts

### 7.1 Which namespace

* The daemon resolves `filesystem.backing_directory` and `filesystem.mountpoint` in **its
  own mount namespace**, once, at startup. The backing root descriptor pins the directory
  that was opened. Mounting something over the backing path afterwards does not change
  what TSFS operates on, and moving or renaming the backing directory does not either.
* The FUSE mount is created in the daemon's mount namespace and is visible to processes
  in namespaces that share it (or that receive it through mount propagation). This is why
  the systemd unit avoids every option that creates a private mount namespace (see 9).
  Under such options the mount would be invisible to the host.
* In a container, such as the development container started by `scripts/tsfs-dev.sh`,
  both paths are container paths. A host process sees the backing directory under the
  host path of the bind mount and sees no FUSE mount unless propagation is configured.

### 7.2 Mount points below the backing root

| Situation | openat2 resolver | Fallback walk (kernel < 5.6) |
|---|---|---|
| Mount of another filesystem below the root, `stay_on_filesystem: true` | Refused (`RESOLVE_NO_XDEV`, EXDEV). The scanner counts it as a boundary. FUSE requests get EXDEV. | Refused (`st_dev` differs, EXDEV). |
| **Bind mount of a directory from the same filesystem** below the root, `stay_on_filesystem: true` | Refused (`RESOLVE_NO_XDEV` detects any mount crossing, whatever the device). | **Not detected**: `st_dev` is identical, so the walk crosses it. The GC may then delete expired regular files that live outside the backing tree but are reachable through the bind mount. |
| Any mount, `stay_on_filesystem: false` | Crossed. | Crossed. |
| Regular file bind-mounted over a managed file, from **another** filesystem | FUSE requests on that name get EXDEV (`check_device()`; test `MountTest.FileBindMountedFromAnotherFilesystemIsNotReachable`). The GC's device check or `unlinkat` (EBUSY on a mount point) prevents the deletion. | Same. |
| Regular file bind-mounted over a managed file, from the **same** filesystem | Not detected by the `st_dev` comparison: reachable through the mount. `unlinkat` fails with EBUSY on the mount point, so the GC cannot delete it. | Same. |

Recommendations: keep `stay_on_filesystem: true`. Set `security.require_openat2: true` on
kernels 5.6 and later so that the fallback is never used (the fallback walk and the
final-component check compare `st_dev` only). Do not create bind mounts below the backing
directory.

## 8. Privilege model

| Mode | Who runs the daemon | Consequences |
|---|---|---|
| System service (`systemd/tsfs.service.in`) | root, with a reduced capability bounding set | Mounts without `fusermount3`. Every backing operation runs as root, after the kernel has checked the caller's permissions (`default_permissions`). New entries are given to the caller (`give_to_caller()`). The GC can delete any expired regular file in the tree. |
| User mount | an unprivileged user | Mounts through the setuid `fusermount3`. Backing operations run with that user's rights, so files created by other users (with `allow_other`) are owned by the mounting user on the backing filesystem. The GC can only delete what that user may delete. |

* `allow_other` (default `false`) lets other users access the mount. A non-root daemon
  needs `user_allow_other` in `/etc/fuse.conf`; the mount preflight
  (`check_mount_privileges()`) refuses to start without it (exit code 3), and also exits
  with 3 when `/dev/fuse` is not accessible (4 when it does not exist). Without
  `allow_other`, only the mounting user (and, for a root mount, root) can access the
  mount.
* The default backing directory `/var/lib/tsfs/data` lives under
  `StateDirectory=tsfs` with mode `0700`. As a result, unprivileged users can reach the
  data **only through the mount**, where the path locks, the open-file registry and the
  notifications apply. Keep this property when you choose another backing directory.
* No IPC endpoint exposes administrative commands. `tsfs --status` reads the status
  file. Reload, status and purge requests are signals, which only the process owner and
  root can send.
* **Purge** (`tsfs --purge`, `SIGUSR2`). Only users allowed to signal the daemon (root, or
  its own uid) can request a purge; `tsfs --purge` exits with code 3 when `kill()` fails
  with EPERM. A purge does not widen what may be deleted: it ignores
  `max_deletions_per_second` and `max_deletions_per_cycle`, but every candidate goes
  through the full validation of 6.2 (P4, P5), excluded and `never` files are never
  candidates, and `garbage_collector.dry_run: true` still prevents every deletion. What
  it removes is the blast-radius limit of the rate settings (see R3): after a bad rule
  or a clock step, a purge deletes everything that looks expired at once. The command
  finds the daemon through the status file (`pid` alive, `/proc/<pid>/comm` equal to
  `tsfs`, same mount point), so a forged or stale status file can at most make it signal
  another process named `tsfs` that the caller may signal anyway. When the mount point
  is mounted but no daemon is found, the offline purge is refused, because the CLI
  process cannot see the files open through the mount. An offline purge (no mount) runs
  with the caller's rights over the backing directory.

## 9. systemd unit hardening and its deliberate limits

`systemd/tsfs.service.in` keeps the options that do **not** create a mount namespace:

* `CapabilityBoundingSet=CAP_SYS_ADMIN CAP_CHOWN CAP_FOWNER CAP_DAC_OVERRIDE CAP_DAC_READ_SEARCH CAP_SYS_RESOURCE`
  (no `CAP_MKNOD`: device nodes are refused anyway; no `CAP_SETFCAP`: extended
  attributes cannot be set through the mount)
* `NoNewPrivileges=yes`, `LockPersonality=yes`, `RestrictRealtime=yes`,
  `RestrictNamespaces=yes`, `RestrictAddressFamilies=AF_UNIX`,
  `SystemCallArchitectures=native`
* `UMask=0027`, `StateDirectoryMode=0700`, `LogsDirectoryMode=0750`, `LimitNOFILE=65536`
* No `LimitCORE=`. systemd's default (soft limit 0, hard limit infinity) lets the daemon
  raise the soft limit itself only when `diagnostics.native_core_dump` is enabled.

The unit deliberately omits `ProtectSystem=`, `ProtectHome=`, `PrivateTmp=`,
`PrivateDevices=` and similar options. Each of them runs the service in a private mount
namespace, where the FUSE mount would be invisible to the rest of the system. The
service therefore runs as **root with full filesystem visibility**. Its confinement
comes from the code-level controls above and from the capability bounding set. The
filesystem view is not restricted.

`UMask=0027` no longer alters entries created through the mount: the kernel has already
applied the client's umask to the requested mode, and the daemon runs with `umask(0)`,
so a file created with mode `0644` by a client is stored as `0644` (regression test
`MountTest.NewEntryModeIgnoresTheDaemonUmask`).

## 10. Residual risks

| # | Risk | Status |
|---|---|---|
| R1 | **Direct writers to the backing directory bypass the path locks.** Between the GC's `stat_entry()` and its `unlinkat()`, a process working directly in the backing tree can rename another file over the candidate's name. The GC then removes the replacement, for example a freshly saved file written with the "write a temporary file, then rename" pattern. The identity check narrows this window to a few system calls but cannot close it, because Linux has no "unlink only if inode matches" call. | Accepted. Mitigation: no direct writers (backing directory `0700`). Writers must go through the mount. |
| R2 | **Direct writers do not produce notifications.** Their changes are seen by the next reconciliation pass (`scan_interval`). They are not registered in the open-file registry, so a file open directly in the backing tree can be unlinked (its data remains readable through the open descriptor until it is closed). | Accepted, documented. |
| R3 | **Wall-clock dependence.** Expiry uses the system clock (`SystemClock`). A forward step, from an administrator, NTP or a VM restore, makes files look expired immediately, and deletions are irreversible. A backward step only delays deletions (test `ClockStepBackwardsDoesNotDeleteEarly`). Users can set the mtime of files they own to the past and so expire their **own** files early. That is not an escalation. | Accepted. Use `dry_run`, `max_deletions_per_second` and `max_deletions_per_cycle` to limit the blast radius. |
| R4 | **Ownership handover (root daemon), residual window.** The handover now works on a descriptor and checks that the entry is still the one created here (6.3), and every creating operation holds the path lock, so mount users cannot race it. A direct writer (A2) can still swap the name between the creation and the re-open, but only an entry of the same type, owned by the daemon's uid (root) and, for a directory, empty, would be handed over. | Narrowed. Mitigation: no direct writers. |
| R5 | **Same-filesystem bind mounts** (7.2): not detected by the fallback resolver for intermediate components, nor by the `st_dev` check of the final component (openat2 or not). | Mitigated by `require_openat2: true` on kernels 5.6 and later for intermediate components; do not create bind mounts below the backing directory. |
| R6 | **Configuration file integrity.** A root daemon refuses a file that is not owned by root or is group- or world-writable (6.4); the directories leading to it are not checked. | Deployment responsibility for the directories. |
| R7 | **Core dumps and crash reports** expose memory contents and layout (6.5). | Off by default (`diagnostics.enabled: false`). |
| R8 | **Memory growth on huge trees.** The scanner's pending-directory list is not bounded by configuration. A user who creates millions of directories increases the daemon's memory use during a pass. | Accepted, bounded by the tree size. |
| R9 | **Policy is path-based.** A user who may rename a file can move it under a rule with a shorter retention. The kernel's permission and sticky-bit checks decide who may do that. TSFS adds no restriction of its own. | By design. |
| R10 | **No exclusion of in-progress writes by direct writers or through `mmap` alone.** `skip_open_files` covers handles opened through the mount (an `mmap` keeps the handle open). | Accepted. |
| R11 | **Policy disclosure through the retention attributes.** Any user who can reach and read a file through the mount can read its expiry, rule identifier and creation time (6.3). | Accepted by default; `filesystem.retention_xattrs: false` removes it. |
| R12 | **System `ls` wrapped by `tsfs-ls`** (6.8): a package-owned file is renamed (`/usr/bin/ls.orig`), and a defect of the wrapper affects every user. | Accepted by the default installation (explicit choice); mitigated by running the original `ls` unchanged outside TSFS mounts and in pipes, and by letting it produce every listing; `-DTSFS_REPLACE_SYSTEM_LS=OFF` avoids it. |
| R13 | **Purge removes the rate limits** that bound the damage of a bad rule or a clock step. | Restricted to users who can signal the daemon; preview with `tsfs --purge --dry-run`; confirmation on a terminal. |

## 11. Security testing procedures

Run everything inside the development container (`scripts/tsfs-dev.sh`), which provides
the C++26 toolchain, `/dev/fuse` and `CAP_SYS_ADMIN`.

| Test file | What it attacks |
|---|---|
| `tests/security/test_backing_store.cpp` (`tsfs_security_tests`, 14 tests) | Each case runs with openat2 and with the forced fallback walk: absolute, relative and nested directory symlinks, the `/proc/self/root` magic link, a final symlink seen as a link, non-normalized paths (`..`, `.`, `//`, trailing `/`, relative, empty), and a directory swapped for a symlink after a successful resolution. |
| `tests/gc/test_garbage_collector.cpp` (`tsfs_gc_tests`, 30 tests) | `SymlinksAreNeverFollowedNorDeleted`, `DirectorySwappedForSymlinkIsRefused`, `ReplacedFileIsNeverDeletedOnTheStaleRecord`, `RenamedFileIsNotFollowedToItsNewName`, `FileModifiedAfterSchedulingIsNotDeleted`, `OpenFileIsSkippedAndRetriedLater`, `ExclusionAddedByReloadPreventsDeletion`, `ClockStepBackwardsDoesNotDeleteEarly`, `HardLinkedFileOnlyLosesTheExpiredName`, `PermissionFailureIsRetriedThenGivenUp`, `ConcurrentCreationNeverLosesFreshFiles`, rate and budget limits, `PurgeDeletesEveryExpiredFileAtOnce` (exclusions and young files kept), `PurgeWorksWithTheGcDisabledAndHonorsDryRun`. |
| `tests/filesystem/test_mount.cpp` (`tsfs_filesystem_tests`, 26 tests) | The real daemon on a real FUSE mount: `DeviceNodesAreRefused`, `PermissionsOwnershipAndTimestamps`, `SymlinksAndHardLinks`, `OpenFileIsNotDeletedUntilReleased`, `FileBindMountedFromAnotherFilesystemIsNotReachable` (EXDEV on the final component; root only), `NewEntryModeIgnoresTheDaemonUmask`, `RetentionExtendedAttributes` (read-only, unknown names give ENODATA, empty list), `PurgeCommandDeletesExpiredFilesThroughTheDaemon` (excluded file kept), `TsfsLsShowsRetentionOnATerminalAndDelegatesOtherwise` (piped output identical to the system `ls`), `SigtermDuringStartupUnmountsCleanly` (no dead mount), `ReadOnlyMountTest.WritesAreRefused`, invalid reload, and clean shutdown. Every test also requires the daemon to exit with status 0 without a sanitizer report. Skipped when FUSE is unavailable. |
| `tests/config/test_config.cpp` | `ConfigTrust.RootRequiresARootOwnedFileNotWritableByOthers`, `ConfigTrust.UnprivilegedDaemonOnlyNeedsARegularFile`, `ConfigOverrides.CommandLinePathsAreValidatedLikeTheFile`. |
| `tests/stress/test_stress.cpp` | Invariant under churn and reloads: a file younger than its retention is never deleted. |
| `tests/unit/test_common.cpp` | `ManagedPath.*` (normalization, traversal, NUL, component-wise `is_within`) and `Escape.*` (log injection). |

Commands:

```sh
# Build the Debug tree (ASan + LeakSanitizer + UBSan by default), then run the
# security label (70 tests: tsfs_security_tests, tsfs_gc_tests, tsfs_filesystem_tests).
scripts/tsfs-dev.sh cmake --preset debug
scripts/tsfs-dev.sh cmake --build --preset debug
scripts/tsfs-dev.sh ctest --test-dir build/debug -L security --output-on-failure

# Path and escaping unit tests (label unit).
scripts/tsfs-dev.sh ctest --test-dir build/debug -R '^(ManagedPath|Escape)\.' --output-on-failure

# Data races (TSan). Needs ASLR disabled on kernels with vm.mmap_rnd_bits > 28.
scripts/tsfs-dev.sh cmake --preset tsan
scripts/tsfs-dev.sh cmake --build --preset tsan
scripts/tsfs-dev.sh --no-aslr ctest --preset tsan
```

The suite has 206 tests. In the last recorded full run (2026-10-13, before the purge,
`tsfs-ls`, retention-attribute and startup-signal tests were added), 197 of the 199
tests passed in the `debug` tree (ASan + LeakSanitizer +
UBSan); 2 were skipped: `DiagnosticsTest.NativeCoreDumpWhenTheEnvironmentAllowsIt`
(unless `TSFS_TEST_ENABLE_CORE_DUMPS=1` is set) and the root-only
`MountTest.FileBindMountedFromAnotherFilesystemIsNotReachable`, run separately as root. The whole gate, TSan
included, runs with `scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh`.

Manual checks before a release:

1. Run `ctest -R BackingStore` and check that the
   `[ INFO ] openat2 available: yes` line appears on the target kernel.
2. On a test host, bind-mount a directory below the backing root and confirm that
   `tsfs --dry-run` reports it as a boundary and lists none of its files (openat2
   kernels).
3. Check the modes of the log directory, crash directory, status file and backing
   directory after a first start under systemd.
4. Run `systemd-analyze security tsfs.service` and review the result against section 9.
5. Confirm the configuration file is `root:root` and not group- or world-writable (a
   root daemon refuses it otherwise), and that its directory is writable by root only.

Gaps (not covered by automated tests): directory bind mounts below the backing root
(only the final-component case is covered, and only when the tests run as root); the R1
and R4 races; ownership handover as root (validated manually in the container;
`PermissionFailureIsRetriedThenGivenUp` skips as root).

## 12. Hardening checklist

- [ ] Keep the backing directory reachable only by root (`0700` parent), so that users go through the mount.
- [ ] Keep `security.stay_on_filesystem: true`. Set `security.require_openat2: true` on kernel 5.6 or later.
- [ ] Keep the configuration file `root:root`, mode `0644` or stricter (enforced for a root daemon), in a directory only root can write.
- [ ] Start with `garbage_collector.dry_run: true` and review `tsfs --dry-run` before enabling deletions.
- [ ] Set `max_deletions_per_second` and `max_deletions_per_cycle` to values that bound the damage of a bad rule or a clock step.
- [ ] Use a reliable time source. Avoid manual clock steps forward on production hosts.
- [ ] Enable `allow_other` only when other users need the mount.
- [ ] Leave `diagnostics.native_core_dump` off unless you are debugging.
- [ ] Do not create bind mounts or other mounts below the backing directory.
- [ ] Decide whether mount users may see expiries, rule identifiers and creation times; otherwise set `filesystem.retention_xattrs: false`.
- [ ] Decide whether the system `ls` may be replaced; otherwise build with `-DTSFS_REPLACE_SYSTEM_LS=OFF`.
- [ ] Restrict who can signal the daemon (`SIGUSR2` purges); preview purges with `tsfs --purge --dry-run`.
