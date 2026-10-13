<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : SECURITY.md
Purpose : Vulnerability reporting, supported versions and summary of the security controls
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Security

TSFS deletes files on its own, usually as root. Deletion is treated as a
security-sensitive operation. The full analysis is in
[docs/security/threat-model.md](docs/security/threat-model.md).

## Reporting a vulnerability

Email **Thierry Gayet <thierry.gayet@labworks.fr>**. Include the affected version
(`tsfs --version`), the kernel version, whether openat2(2) is available, the relevant
configuration, the steps to reproduce, and the impact. Do not open a public issue before
a fix is available.

## Supported versions

| Version | Supported |
|---|---|
| 0.1.x | yes |
| older | no |

## Threat model summary

| Asset | Threat | Control |
|---|---|---|
| Files outside the backing directory | `..`, absolute or relative symbolic links, magic links, a directory swapped for a link | All access is relative to one root descriptor. Resolution uses openat2 `RESOLVE_BENEATH \| RESOLVE_NO_SYMLINKS \| RESOLVE_NO_MAGICLINKS`, with a fallback walk using `O_NOFOLLOW` on every component. Managed paths are normalized (no `.`, `..` or NUL) before any system call. |
| Files on other mounts | GC or requests crossing a mount point below the root | `security.stay_on_filesystem` (default `true`): `RESOLVE_NO_XDEV` with openat2, or a device check in the fallback walk, for intermediate components. The final component must be on the root device: FUSE requests on an entry of another device get `EXDEV`, and the GC also requires the candidate to be on the root device. |
| Non-expired data | A file replaced, renamed, rewritten or opened between scheduling and deletion | `process_candidate()` holds the path locks of the file and all its ancestors. It re-resolves the path, then requires a regular file, the same (device, inode), expiry under the current policy and clock, and no handle open through the mount. Only then does it call `unlinkat()` relative to the validated parent descriptor. |
| Non-expired data | Bad rule, clock step, too many deletions at once | `retention.default: never`, a dry-run mode (`tsfs --dry-run`, `garbage_collector.dry_run`), and the `max_deletions_per_second` and `max_deletions_per_cycle` limits. A clock step backwards never deletes early. |
| Permissions of the mount | Users bypassing mode bits, device nodes | Mounted with `default_permissions`, so the kernel checks the caller. `mknod` of character or block devices is refused. Final names use `O_NOFOLLOW` or `AT_SYMLINK_NOFOLLOW`. New entries are given to their creator when the daemon runs as root, through a descriptor and only if the entry is still the one just created. Entries keep the mode requested by the client (the daemon runs with `umask(0)`). |
| Logs | Forged log lines through crafted file names, log flooding | Every field is escaped (text and JSON). Repetitive warnings are rate-limited. Rotation and a bounded async queue limit growth, and losses are counted. |
| Logs, crash reports, status file | Disclosure, symlink attacks | Modes `0640` (logs, status file), `0600` (crash reports), directories `0750`. Files are created with `O_NOFOLLOW`, crash reports with `O_EXCL`, the status file through an unpredictable `mkostemp()` name then `rename()`. These outputs are refused inside the backing directory or the mount point. |
| Deletion policy | A configuration file modifiable by non-root users | A root daemon refuses a configuration file that is not a regular file owned by root, or that is group- or world-writable (exit code 5). Command-line path overrides are validated like file values. |
| Host | Command injection | No shell, no `fork`/`exec`, no command built from paths. |
| Daemon resources | Notification floods, huge trees | Bounded event queue, scheduler and work queue. Time-sliced, resumable scans. Bounded retries. 4 MiB configuration limit. |

## Known limits

* **The daemon runs as root under systemd, without a private mount namespace.** The
  options `ProtectSystem=`, `ProtectHome=`, `PrivateTmp=` and similar would hide the FUSE
  mount from the host. The unit only reduces capabilities and uses options that create
  no mount namespace.
* **Direct writers bypass the protections.** Processes that write directly into the
  backing directory are not covered by the path locks, the open-file registry or the
  change notifications. A narrow race (between validation and `unlinkat`) remains for
  them. Keep the backing directory reachable by root only, so that users go through the
  mount.
* **Bind mounts of the same filesystem are only detected by openat2, and only for
  intermediate components.** The fallback resolver (kernels < 5.6) and the final-component
  check compare `st_dev` only. Set `security.require_openat2: true` on newer kernels and do
  not create bind mounts below the backing directory.
* **Expiry follows the wall clock.** A forward clock step makes files expire immediately.
* **The configuration file is trusted.** A root daemon checks its owner and mode, but not
  the directories leading to it. Whoever can write or replace it controls what is deleted.
* **`read_only: true` does not stop the GC.** Use `garbage_collector.enabled: false` or
  `dry_run: true` for that.

## Hardening checklist

- [ ] Keep the backing directory under a root-only (`0700`) parent. The default `/var/lib/tsfs/data` already is.
- [ ] Keep `security.stay_on_filesystem: true`. Set `security.require_openat2: true` on kernel 5.6 or later.
- [ ] Keep the configuration file `root:root`, mode `0644` or stricter (enforced for a root daemon), in a root-only directory.
- [ ] Review `tsfs --dry-run` before turning `garbage_collector.dry_run` off.
- [ ] Bound `max_deletions_per_second` and `max_deletions_per_cycle`.
- [ ] Enable `allow_other` and `diagnostics.native_core_dump` only when needed.
