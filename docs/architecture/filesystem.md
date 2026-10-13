<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/filesystem.md
Purpose : FUSE layer: passthrough design, operations, permissions, path resolution, locking
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Filesystem layer

Sources: `src/filesystem/passthrough_fs.{hpp,cpp}`,
`src/filesystem/fuse_session.{hpp,cpp}`, `src/fsutil/*`. Related:
[overview](overview.md), [garbage collector](garbage-collector.md),
[concurrency](concurrency.md), [threat model](../security/threat-model.md),
[user guide](../user/user-guide.md).

## 1. Design

### 1.1 Passthrough over a backing directory

TSFS exposes exactly one host directory, `filesystem.backing_directory`, at
`filesystem.mountpoint`. `filesystem.mode` accepts only `passthrough`
(`src/config/config_loader.cpp`, `parse_filesystem`). The loader refuses `/`
for either path and refuses a mount point and backing directory that contain
each other.

A **managed path** is the path libfuse hands to a callback, e.g. `/a/b`. It
always starts with `/`, has no empty, `.` or `..` component and no trailing
slash (`src/common/managed_path.hpp`). It designates `<backing>/a/b`; it is
never a host path. `BackingStore::open_parent` and `open_directory` refuse any
path that is not normalized (`EINVAL`).

### 1.2 High-level libfuse API

TSFS uses the **high-level, path-based** API (`#include <fuse.h>`,
`struct fuse_operations`, `fuse_new`, `fuse_mount`, `fuse_loop_mt`), compiled
with `FUSE_USE_VERSION=314`. Reasons visible in the code:

* operations receive managed paths, which is what the policy engine, the
  path-lock table and the GC notifications are keyed on;
* `fuse_invalidate_path(fuse, path)` drops kernel caches by path after a GC
  deletion (`FuseSession::invalidate`);
* libfuse's multi-threaded loop and inode bookkeeping are reused.

`FuseSession::mount` builds the option string
`default_permissions,fsname=tsfs,subtype=tsfs`, adds `allow_other` and `ro`
from the configuration and `-d` for `--fuse-debug`, then calls `fuse_new` and
`fuse_mount`. `FuseSession::loop` runs `fuse_loop_mt` with
`fuse_loop_cfg_set_idle_threads(10)` and `clone_fd = 0`. The destructor removes
libfuse's signal handlers, unmounts, then destroys the `fuse` object, so every
exit path leaves no stale mount. The `fsname`/`subtype` options make the mount
appear with filesystem type `fuse.tsfs` in `/proc/self/mountinfo`, which is how
the `tsfs-ls` wrapper (the system `ls` by default) recognizes TSFS mounts.

The operation table passed to `fuse_new` is `PassthroughFs::operations(
retention_xattrs)`: with `filesystem.retention_xattrs: false` it is a copy without
the `getxattr` and `listxattr` callbacks (see section 3.1).

Right after `fuse_mount`, the daemon reads the kernel connection identifier of the
mount (`fuse_connection_id`, `src/filesystem/fuse_control.cpp`): the device number
of the FUSE mount on the mount point, taken from `/proc/self/mountinfo` (the last,
topmost one), in the kernel encoding `major << 20 | minor` (FUSE mounts use an
anonymous device of major 0). It never calls `stat()` on its own mount: that would
be a FUSE request to a daemon whose loop is not running yet. The identifier names
`/sys/fs/fuse/connections/<id>/` (fusectl); `fuse_connection_waiting` reads its
`waiting` file at each status write (`null` when fusectl is not mounted or the file
is unreadable). It is published in the `fuse` section of the status file and in the
`fuse_control` field of `filesystem_mounted`.

`PassthroughFs::init` sets the libfuse configuration:

| Setting | Value | Effect |
|---|---|---|
| `use_ino` | 1 | `st_ino` of the backing file is reported (stable identities for tools). |
| `hard_remove` | 1 | `unlink` of an open file really unlinks it; no `.fuse_hidden*` files appear in the backing tree. |
| `nullpath_ok` | 1 | Operations on an open handle work after the name is removed (path may be `NULL`). |
| `entry_timeout`, `attr_timeout` | 1.0 s | Kernel caching of lookups and attributes. |
| `negative_timeout` | 0.0 | A file created directly in the backing tree is visible immediately. |

### 1.3 Callback trampolines

`PassthroughFs::operations()` builds a static `fuse_operations` table whose
entries are `Trampoline<&PassthroughFs::method, FsOp::op>::call`. Each
trampoline:

1. fetches the instance from `fuse_get_context()->private_data`;
2. calls the member function inside `try`; `std::bad_alloc` becomes `-ENOMEM`,
   any other exception `-EIO` (the function is `noexcept`: nothing unwinds into
   libfuse);
3. counts the call (and the error) in `ShardedCounters`;
4. on failure, extracts the path when the first parameter is `const char*`
   (C++26 pack indexing `Args...[0]`) and calls `report_failure`.

`report_failure` classifies errnos: ordinary client results (`ENOENT`,
`EEXIST`, `EACCES`, `ENOSPC`, `EROFS`, ...) are logged at `trace` only;
`ELOOP`/`EXDEV` (refused path resolution) at `debug`; anything else is an
"unexpected error" warning, rate-limited per `(operation, errno)` key by a
`LogRateLimiter` (10 per minute per key).

## 2. Path resolution: `BackingStore`

`BackingStore` (`src/fsutil/backing_store.{hpp,cpp}`) is the only way TSFS
touches the backing tree. At startup it opens the backing directory once with
`O_PATH | O_DIRECTORY` (the configured path itself may be a symlink: it comes
from the trusted configuration) and records its `st_dev`. Every later access is
relative to that descriptor.

```
 managed path "/a/b/c"
     |
     +-- open_parent(): resolve "a/b" (relative to the root fd) -> O_PATH dir fd, name "c"
     |        openat2(root, "a/b", O_PATH|O_DIRECTORY|O_NOFOLLOW,
     |                RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS
     |                [| RESOLVE_NO_XDEV if stay_on_filesystem])
     |        retried on EAGAIN / EINTR (concurrent rename during lookup)
     |
     +-- caller: fstatat / openat / unlinkat / renameat2 ... (dirfd, "c",
                 AT_SYMLINK_NOFOLLOW or O_NOFOLLOW)
```

* **openat2 path** (Linux >= 5.6): the kernel guarantees that resolution stays
  beneath the root, follows no symlink (including in intermediate components)
  and no magic link, and, with `security.stay_on_filesystem` (default `true`),
  crosses no mount point (`EXDEV`).
* **Fallback walk** (`resolve_fallback`): used when `openat2` is unavailable
  (`kernel_supports_openat2()` probes `openat2(AT_FDCWD, "/")` and treats only
  `ENOSYS` as absence). Each component is opened with `openat(current, name,
  O_PATH | O_DIRECTORY | O_NOFOLLOW)`; with `stay_on_filesystem`, `fstat` of
  every step must report the root's `st_dev`, else `EXDEV`. This detects a
  mount of another filesystem but **not** a bind mount of the same filesystem
  (same `st_dev`); set `security.require_openat2` where this matters. `..`
  cannot occur because managed paths are normalized.
* `security.require_openat2: true` makes `BackingStore::open` fail with
  `ENOSYS`, which the daemon maps to exit code 4.
* `open_directory(path)` resolves the full path with `O_RDONLY | O_DIRECTORY`
  under the same rules, so the final component is checked too (used by
  `opendir` and by the GC scanner).

Consequences: an external process that swaps a directory for a symlink makes
the operation fail with `ELOOP` or `ENOTDIR` instead of escaping; absolute
symlinks and `..` cannot leave the root.

### 2.1 Mount boundaries and bind mounts

With `stay_on_filesystem: true`:

| Situation | FUSE layer | GC |
|---|---|---|
| Mount point (or directory bind mount) as an **intermediate** component | refused, `EXDEV` (or `ELOOP`/`ENOTDIR` logged at debug) | refused (scanner counts it in `boundaries`) |
| Mount point **directory** as the final component of `opendir` | refused, `EXDEV` (`open_directory` resolves it) | not listed (`boundaries`) |
| **File** bind-mounted inside the tree from **another filesystem**, as the final component | refused, `EXDEV`: `getattr`, `access`, `open`, `create` (existing file), path-based `truncate`, `chmod`, `chown` and `utimens` compare the entry's `st_dev` with `root_device()` (`MountTest.FileBindMountedFromAnotherFilesystemIsNotReachable`) | never deleted: `process_candidate` compares the entry's device with `root_device()` and returns `unsafe` |
| **File** bind-mounted from the **same filesystem**, as the final component | *not* detected (same `st_dev`) | not detected by the device check; `unlinkat` fails with `EBUSY` on a mount point |

The final component of FUSE operations is handled with `*at()` calls relative to
the parent descriptor. With `stay_on_filesystem`, `PassthroughFs::check_device()`
(on a `struct stat` already obtained) and `check_name_device()` (`fstatat` with
`AT_SYMLINK_NOFOLLOW`) return `-EXDEV` for an entry on another device than the
backing root; they guard the operations listed above. The GC adds its own device
check before every unlink (see [garbage-collector.md](garbage-collector.md#6-validation-before-unlink)).
See the comment of `BackingStore` in `src/fsutil/backing_store.hpp`.

## 3. Implemented operations

All operations return `0` (or a byte count/offset) or a negative errno. "RO"
means the operation returns `-EROFS` when `filesystem.read_only` is set (the
mount is also `ro`, so the kernel normally refuses first). "Lock" lists the
`PathLockTable` stripes taken. "Notify" is the GC notification
(`notify_changed`).

| Operation | Semantics | RO | Lock | Notify |
|---|---|---|---|---|
| `getattr` | `fstat(fh)` when a handle is given, else `fstatat(parent, name, AT_SYMLINK_NOFOLLOW)` (`AT_EMPTY_PATH` for the root), then the device check (`EXDEV`). | | | |
| `readlink` | `readlinkat`, NUL-terminated, truncated to `size - 1`. The target is returned verbatim. | | | |
| `mknod` | Regular files via `openat(O_CREAT|O_EXCL|O_NOFOLLOW)`; FIFOs/sockets via `mknodat`; **character and block devices refused with `EPERM`**; ownership given to the caller (see 5). | yes | path | yes |
| `mkdir` | `mkdirat`, ownership given to the caller (see 5). | yes | path | |
| `unlink` | `unlinkat(parent, name, 0)`. | yes | path | |
| `rmdir` | `unlinkat(..., AT_REMOVEDIR)`. | yes | path | |
| `symlink` | `symlinkat(target, parent, name)`; the target string is stored as given and never followed by TSFS; ownership given to the caller (see 5). | yes | path | |
| `rename` | `renameat2(src_parent, src, dst_parent, dst, flags)`; `RENAME_NOREPLACE` and `RENAME_EXCHANGE` are passed through. | yes | from + to | `to`; also `from` with `RENAME_EXCHANGE` |
| `link` | `linkat(..., 0)` (hard link, no symlink following). | yes | from + to | `to` |
| `chmod` | `fchmod(fh)` or, after the device check, `fchmodat(..., AT_SYMLINK_NOFOLLOW)` (fails on symlinks rather than changing the target). | yes | | |
| `chown` | `fchown(fh)` or, after the device check, `fchownat(..., AT_SYMLINK_NOFOLLOW)`. | yes | | |
| `truncate` | With a handle: `ftruncate`, marks the handle modified. Without: `openat(O_WRONLY|O_NOFOLLOW)`, device check on the descriptor, `ftruncate`. | yes | path (path form) | path form: yes; handle form: on release |
| `open` | `openat(parent, name, flags & ~O_CREAT | O_NOFOLLOW | O_CLOEXEC)`; `fstat` gives the `(dev, ino)` identity (device check: `EXDEV`), registered in `OpenFileRegistry`. `O_TRUNC` marks the handle modified. Write access on a read-only mount: `EROFS`. | write modes | path | on release if modified |
| `create` | `openat(flags | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode)` first: if this call created the file, ownership is given to the caller on that descriptor (see 5). If the file appeared meanwhile and the caller did not ask for `O_EXCL`, it is opened as is (ownership untouched, device check). Identity registered, handle marked modified. | yes | path | yes |
| `read_buf` | Returns a `fuse_bufvec` pointing at the backing descriptor (`FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK`); libfuse reads or splices directly from the file. | | | |
| `write_buf` | `fuse_buf_copy` into the backing descriptor at `offset` (`FUSE_BUF_SPLICE_NONBLOCK`); marks the handle modified. | yes | | on release |
| `statfs` | `fstatvfs` of the backing root descriptor. | | | |
| `flush` | `close(dup(fd))`: reports deferred write errors (NFS, quota) to the caller of `close(2)` without closing the handle. | | | |
| `release` | Unregisters the identity, closes the descriptor, then notifies the GC if the handle was modified (new mtime moves the expiry). | | | if modified |
| `fsync` | `fdatasync` or `fsync` of the backing descriptor. | | | |
| `opendir` | `BackingStore::open_directory` + `fdopendir`. | | | |
| `readdir` | Streams entries with `readdir`; offsets are `telldir` values; `seekdir` on offset change; `d_ino` and `d_type` are passed to the filler. Serialized by a per-handle mutex. | | | |
| `releasedir` | `closedir`. | | | |
| `fsyncdir` | `fdatasync`/`fsync` of the directory stream descriptor. | | | |
| `access` | Existence check (`fstatat`), device check (`EXDEV`) and `EROFS` for `W_OK` on a read-only mount. Mode bits are checked by the kernel (`default_permissions`). | | | |
| `utimens` | `futimens(fh)` (marks modified) or, after the device check, `utimensat(..., AT_SYMLINK_NOFOLLOW)`. | yes | path (path form) | path form: yes; handle form: on release |
| `fallocate` | `fallocate(fd, mode, offset, length)` on an open handle only (`EOPNOTSUPP` without one); marks modified. | yes | | on release |
| `lseek` | `lseek` on the backing descriptor (`SEEK_DATA`/`SEEK_HOLE` pass through); `EBADF` without a handle. | | | |
| `getxattr` | Read-only retention attributes (section 3.1); `ENODATA` for any other name, without disk access. | | | |
| `listxattr` | Always an empty list (section 3.1). | | | |

The 30 operations above are the counters reported in the status file
(`FsOp`, `src/filesystem/passthrough_fs.hpp`), plus `bytes_read`,
`bytes_written` and `open_handles`.

### 3.1 Retention extended attributes

`getxattr` serves four virtual, read-only attributes, computed at each request
from the policy of the current configuration snapshot:

| Name | Value |
|---|---|
| `user.tsfs.state` | `expires`, `expired` (expiry reached, deletion pending), `excluded` or `never` |
| `user.tsfs.expires` | expiry, ISO 8601 UTC to the second (`%FT%TZ`); only for `expires` and `expired` |
| `user.tsfs.rule` | identifier of the deciding rule, or `default` |
| `user.tsfs.created` | birth time of the backing file (statx `btime`), ISO 8601 UTC to the second, or `st_mtime` when the backing filesystem does not report it |

* A name outside `user.tsfs.` (for example the `security.capability` lookup the
  kernel makes before each write, or ACL probes), or an unknown `user.tsfs.*`
  name, returns `ENODATA` immediately: no path resolution, no disk access.
* For the four names, the path is resolved with `open_parent` and the entry read
  with `stat_entry` (no symlink following; birth time always requested, for
  `user.tsfs.created`).
  Only regular files of the managed tree have attributes: the root, directories,
  symbolic links, special files, files on another device (with
  `stay_on_filesystem`) and the absence of a policy give `ENODATA`; resolution
  errors (`ENOENT`, `EXDEV`...) are returned as is. The decision is the one the
  GC would make (`PolicyEngine::evaluate`), with the expiry compared to the wall
  clock.
* Values follow the FUSE convention: size 0 returns the length, a short buffer
  `ERANGE`; no terminating NUL.
* `listxattr` returns 0 (an empty list) on purpose: GNU `ls` (coreutils 9 and
  later) prints a `+` after the mode of any file whose `listxattr` is not empty,
  which would mark every file of the mount as if it had an ACL. The attributes
  are read by name (`getfattr -n`, `tsfs-ls` with `lgetxattr`).
* `setxattr` and `removexattr` stay unset, and the real attributes of the
  backing files are never exposed.

Cost: once a `getxattr` callback exists, the kernel sends one `getxattr`
(`security.capability`, the file-capability check made before a write) request
before each write request, counted as a `getxattr` error. With
`filesystem.retention_xattrs: false` both callbacks are removed: libfuse answers
`ENOSYS` once and the kernel stops asking. Measurements:
[benchmark report](../performance/benchmark-report.md#8-retention-extended-attributes-cost).

## 4. Unsupported operations

Callbacks not set in the operation table, so libfuse answers `ENOSYS` to the
kernel (`PassthroughFs::operations()`):

* extended attributes: `setxattr`, `removexattr` (and `getxattr`, `listxattr`
  when `filesystem.retention_xattrs` is `false`);
* `ioctl`, `poll`, `bmap`;
* `lock` and `flock`: POSIX and BSD locks are handled locally by the kernel
  (they are not propagated to the backing filesystem, so they do not
  coordinate with processes using the backing directory directly);
* `copy_file_range`: the kernel falls back to read/write;
* `destroy` is not set (shutdown is driven by `Daemon`).

Device nodes cannot be created through the mount (`mknod` returns `EPERM`).
TSFS does not claim full POSIX compatibility; the behavior covered by the
integration tests is listed in `tests/filesystem/test_mount.cpp` (see
[test strategy](../testing/test-strategy.md)).

## 5. Permissions and ownership

* The mount always carries `default_permissions`: the kernel checks the
  caller's credentials against the mode, uid and gid that `getattr` reports
  (the backing file's own values). TSFS performs no second permission check;
  `access` only answers existence, the device check and read-only state.
* `allow_other` (`filesystem.allow_other`) lets users other than the mounting
  user access the mount; a non-root daemon needs `user_allow_other` in
  `/etc/fuse.conf`: the mount preflight (`check_mount_privileges`,
  `src/app/application.cpp`) refuses to start otherwise, with exit code 3. The
  same preflight exits with 3 when `/dev/fuse` is not readable and writable, and
  with 4 when it does not exist.
* All backing-tree operations run with the daemon's own credentials. When the
  daemon runs as root (`geteuid() == 0` at construction), entries created by
  `mknod`, `mkdir`, `symlink` and `create` are given to the caller. Root callers
  are skipped. The change is made on a descriptor, never by name:
  * `give_to_caller(dirfd, name, type)` (`mknod`, `mkdir`, `symlink`) re-opens
    the new entry with `O_PATH | O_NOFOLLOW` and calls `give_fd_to_caller`;
  * `give_fd_to_caller(fd, dirfd, type)` (directly for `create`, on the
    descriptor it opened with `O_EXCL`) checks with `fstat` that the descriptor is
    still the entry created here: expected file type, owned by the daemon's
    effective uid and, for a directory, a link count of 2. Otherwise the
    ownership is left untouched and a warning is logged
    (`filesystem_operation_failed`, "new entry replaced before its ownership was
    set"). Then `fchownat(fd, "", ctx->uid, gid, AT_EMPTY_PATH |
    AT_SYMLINK_NOFOLLOW)`, where `gid` is the caller's gid, or the parent's gid
    when the parent directory is set-group-ID (BSD semantics).

  Every creating operation holds the path lock. A failed chown is logged at
  debug level and the entry stays owned by root (the safe direction). Validated
  as root in the development container: files, directories, symbolic links and
  FIFOs end up owned by the creating user. A non-root daemon creates every entry
  as itself.
* Modes: the kernel has already applied the client's umask to the mode passed
  to `create`, `mknod` and `mkdir`, and the daemon runs with `umask(0)` (set at
  the start of `Daemon::run`), so the stored mode is exactly the requested one;
  the systemd unit's `UMask=` does not alter it
  (`MountTest.NewEntryModeIgnoresTheDaemonUmask`).
* Explicit `chown`/`chmod` requests are passed to the backing filesystem, which
  enforces its own rules for the daemon's credentials.

## 6. Open-file registry

`OpenFileRegistry` (`src/fsutil/open_file_registry.hpp`) counts open handles per
`FileIdentity` (`st_dev`, `st_ino`): `open`/`create` call `add`, `release`
calls `remove`. It is split into 64 cache-line-aligned shards, each a mutex and
an `unordered_map<FileIdentity, uint32_t>`, plus a relaxed total used for the
`open_handles` gauge.

The GC calls `is_open(identity)` during validation when
`garbage_collector.skip_open_files` is `true` (default): a file open *through
the mount* is not deleted; it is rescheduled `retry_initial_delay` later
(`busy_open`). Files opened directly in the backing directory by other
processes are not visible to the registry.

Handle ownership: the `FileHandle`/`DirHandle` objects behind `fuse_file_info::fh` are
owned by `PassthroughFs` from `open`/`create`/`opendir` (`adopt()`) until
`release`/`releasedir` (`take_file()`/`take_dir()`); the live set is kept under
`handles_mutex_` (taken by open and release only, never by read or write). libfuse
does not call `release()` for handles still open when the session ends (files held
open by applications at unmount, or a RELEASE still queued at SIGTERM), so
`~PassthroughFs()` frees the remaining ones and logs `filesystem_handles_released`
(`files`, `directories`). The daemon destroys the filesystem right after unmounting,
while the logger still runs.

## 7. Path lock table

`PathLockTable` (`src/fsutil/path_lock_table.hpp`) is an array of 1024 mutexes
indexed by `std::hash<std::string_view>(path) % 1024`.

* FUSE operations lock the stripe(s) of the name(s) they create, remove, open
  or modify: one stripe for `mknod`, `mkdir`, `unlink`, `rmdir`, `symlink`,
  `open`, `create`, path-based `truncate` and `utimens`; two (`from`, `to`) for
  `rename` and `link`. `chmod`, `chown` and handle-based operations take no
  stripe.
* The GC locks the candidate **and every ancestor up to `/`**
  (`lock_with_ancestors`) while it re-resolves, re-validates and unlinks. A
  rename of a parent directory through the mount therefore waits, and cannot
  move the file under a different policy between validation and unlink.
* `Guard` sorts and deduplicates the stripe indices before locking, and unlocks
  in reverse order. Every holder of several stripes acquires them in increasing
  index order, which excludes deadlock between FUSE pairs and GC ancestor sets.
  Hash collisions only add contention.
* `open` takes the stripe so that "open versus GC unlink" is ordered: either
  the GC sees the registered handle, or the open sees `ENOENT`.

Changes made by other processes directly in the backing directory take no
stripe; see [threat model](../security/threat-model.md) for the residual
window.

## 8. Interaction with the garbage collector

```
 FUSE thread                                  GC
 -----------                                  --
 create/mknod/truncate/utimens/rename/link
 release(modified handle)  --notify_changed(path)-->  bounded event deque (non-blocking)
 open/create  --OpenFileRegistry::add-->              is_open(identity) during validation
 release      --OpenFileRegistry::remove-->
 namespace ops --PathLockTable stripe(s)--            lock_with_ancestors(candidate)
                                     <--on_deleted(path)--  after a real unlink
 FuseSession::invalidate(path) -> fuse_invalidate_path()
```

* `notify_changed` only appends to a bounded deque under the GC mutex (cap
  `min(max_pending_tasks, 65536)`); on overflow the event is dropped and
  counted (`events_dropped`), and the next reconciliation pass sees the file.
  The FUSE thread never performs policy evaluation or I/O for the GC.
* The GC never deletes on the strength of a notification: the event only causes
  a fresh `statx` and policy evaluation, and deletion repeats the complete
  validation.
* After each real deletion the GC calls `on_deleted(path)` outside every lock.
  The daemon forwards it to `FuseSession::invalidate`, which calls
  `fuse_invalidate_path` (an `ENOENT` result, meaning nothing was cached, is
  ignored). The pointer to the session is an `std::atomic<FuseSession*>`, set
  when the loop starts and cleared before the GC stops.
* Not notified: `unlink`/`rmdir` through the mount (a scheduled entry for the
  removed name ends as `vanished`), `mkdir`, `chmod`/`chown` (they do not
  change `st_mtime`), and the contents of a renamed directory (only the
  directory's own new path is notified, and it is not a regular file; files
  inside are re-evaluated by the next pass).
