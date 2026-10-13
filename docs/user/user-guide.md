<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/user/user-guide.md
Purpose : User guide: installation, configuration, operation and troubleshooting
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# TSFS user guide

## Contents

1. [Introduction](#1-introduction)
2. [Supported platforms](#2-supported-platforms)
3. [System requirements](#3-system-requirements)
4. [Installation](#4-installation)
5. [Dependency installation](#5-dependency-installation)
6. [Building from source](#6-building-from-source)
7. [Release installation](#7-release-installation)
8. [Debug installation](#8-debug-installation)
9. [Configuration reference](#9-configuration-reference)
10. [Retention policy examples](#10-retention-policy-examples)
11. [Mounting the filesystem](#11-mounting-the-filesystem)
12. [Unmounting the filesystem](#12-unmounting-the-filesystem)
13. [Runtime configuration reload](#13-runtime-configuration-reload)
14. [Garbage collector behavior](#14-garbage-collector-behavior)
15. [Dry-run mode](#15-dry-run-mode)
16. [Logging](#16-logging)
17. [Monitoring](#17-monitoring)
18. [Performance tuning](#18-performance-tuning)
19. [Crash diagnostics](#19-crash-diagnostics)
20. [Core-dump analysis](#20-core-dump-analysis)
21. [Troubleshooting](#21-troubleshooting)
22. [Uninstallation](#22-uninstallation)
23. [Security recommendations](#23-security-recommendations)
24. [Known limitations](#24-known-limitations)

Related documents:

- [configuration.md](configuration.md): every configuration key, with types,
  defaults, ranges and reload behavior.
- [monitoring.md](monitoring.md): status file, counters and log events.
- The `tsfs(1)` and `tsfs-ls(1)` manual pages (`man tsfs`, `man tsfs-ls`).
- Architecture: [overview](../architecture/overview.md),
  [filesystem](../architecture/filesystem.md),
  [policy engine](../architecture/policy-engine.md),
  [garbage collector](../architecture/garbage-collector.md),
  [configuration reload](../architecture/configuration-reload.md),
  [logging](../architecture/logging.md),
  [diagnostics](../architecture/diagnostics.md).
- Security: [threat model](../security/threat-model.md).
- Developers: [building.md](../developer/building.md).

---

## 1. Introduction

TSFS (Time-aware Storage Filesystem) is a FUSE filesystem for Linux that deletes
files automatically once their retention period has elapsed.

TSFS exposes an existing directory, the **backing directory**, at a **mount
point**. Applications read and write files through the mount point as on any
filesystem; the data is stored unchanged in the backing directory (passthrough). A
built-in **garbage collector** applies the **retention rules** of a YAML
configuration file: each regular file gets a lifetime, computed from its
modification time (or, optionally, its creation time), and is deleted once that
lifetime has elapsed.

Typical uses:

- video recordings, captures or exports that must be purged after a day;
- caches that should not grow forever;
- application logs and core dumps that must be kept for a fixed period;
- any directory where "delete after N days" is easier to declare than to script
  with `cron` and `find -mtime`.

Key properties:

- **Declarative policy.** Rules select files by exact path, directory (optionally
  recursive), file-name glob, whole-path glob or extension. Exclusion rules protect
  files absolutely. The default for unmatched files is "keep forever".
- **Safe deletion.** Before each deletion, TSFS re-checks the file under a path
  lock: it must still exist, still be the same inode, still be a regular file on
  the same filesystem, not be open through the mount, and still be expired under
  the *current* policy. Paths are resolved without following symbolic links and
  never escape the backing directory.
- **Live reconfiguration.** Edit the file, send `SIGHUP`, or run
  `systemctl reload tsfs`: the new policy is validated, then applied atomically,
  without remounting. An invalid file is rejected and the running policy is kept.
- **Dry run.** `tsfs --dry-run` lists what would be deleted now, without mounting
  and without deleting anything; `garbage_collector.dry_run: true` makes the daemon
  log instead of delete.
- **Explicit purge and statistics.** `tsfs --purge` deletes every expired file at
  once, without the rate limits; `tsfs --stats` counts files, bytes and upcoming
  expiries per rule.
- **Retention visible to users.** Each file of the mount carries read-only
  attributes giving its state, expiry, deciding rule and creation time; the
  `tsfs-ls` wrapper, installed as the system `ls` by default, tags each entry of a
  listing with them.
- **Observability.** Structured logs with stable event names, a JSON status file
  with counters, optional crash reports and core dumps.

TSFS stores no state of its own: a file's expiry is derived from its timestamps
every time it is evaluated. Stopping, restarting or reinstalling TSFS never loses
retention information, and the data in the backing directory is never touched
except by the garbage collector deleting expired files.

## 2. Supported platforms

- **Operating system:** Linux only (FUSE 3, `statx`, `openat2`, `inotify`,
  `eventfd`).
- **Architectures:** any architecture supported by the toolchain. Crash reports
  decode the instruction pointer on x86_64 and aarch64 only; elsewhere they report
  0 for it (the backtrace is still written).
- **Distributions:** developed and tested on Ubuntu 26.04 (the `tsfs-dev:26.04`
  container image, `docker/Dockerfile.dev`). Any distribution that provides the
  versions listed in [System requirements](#3-system-requirements) can build and
  run TSFS. On older distributions (for example Ubuntu 22.04 or 24.04), build inside
  the development container with `scripts/tsfs-dev.sh` (see
  [Building from source](#6-building-from-source)).
- **Kernel:**
  - FUSE support (`/dev/fuse`), as on every mainstream distribution kernel.
  - Linux 5.6 or later for `openat2(2)`. On older kernels TSFS uses an equivalent
    component-by-component path resolution, unless `security.require_openat2: true`
    is set, in which case it refuses to start.
  - `retention.timestamp: birth` needs a backing filesystem that reports the
    creation time through `statx` (ext4, XFS, Btrfs and others); otherwise TSFS
    falls back to the modification time.
- **Backing filesystem:** any local filesystem with regular POSIX semantics. With
  `security.stay_on_filesystem: true` (the default), TSFS does not descend into
  other filesystems mounted below the backing directory.

## 3. System requirements

### Build

| Component            | Minimum version                                  | Ubuntu 26.04 package |
|----------------------|--------------------------------------------------|----------------------|
| C++26 compiler       | GCC 15, or Clang 21 with libstdc++ 15            | `g++` (or `clang`)   |
| CMake                | 3.28                                             | `cmake`              |
| Build tool           | Ninja (used by the presets) or Make              | `ninja-build`        |
| pkg-config           | any                                              | `pkg-config`         |
| libfuse              | 3.14                                             | `libfuse3-dev`       |
| yaml-cpp             | 0.7                                              | `libyaml-cpp-dev`    |
| spdlog               | 1.12                                             | `libspdlog-dev`      |
| fmt                  | the version spdlog uses                          | `libfmt-dev`         |
| zlib                 | any                                              | `zlib1g-dev`         |
| GoogleTest / GoogleMock | 1.12 (tests only; `-DTSFS_BUILD_TESTS=OFF` to skip) | `libgtest-dev`, `libgmock-dev` |
| Google Benchmark     | optional (benchmarks)                            | `libbenchmark-dev`   |
| Doxygen, Graphviz    | optional (`doc` target)                          | `doxygen`, `graphviz`|
| lcov                 | optional (`coverage` target)                     | `lcov`               |

### Runtime

- The shared libraries above (libfuse3, yaml-cpp, spdlog, fmt, zlib).
- The `fuse3` package (`fusermount3`), needed to mount as a non-root user.
- Root privileges for the system service (the provided systemd unit runs as
  root), or an unprivileged user with access to `/dev/fuse` and `fusermount3` (see
  [Mounting the filesystem](#11-mounting-the-filesystem)).
- Disk and memory: TSFS keeps in memory only the files that expire within the
  next two scan intervals, bounded by `garbage_collector.max_pending_tasks`
  (default 100000).

## 4. Installation

Installation is a build from source followed by `cmake --install`:

1. Install the dependencies ([chapter 5](#5-dependency-installation)), or use the
   development container.
2. Configure and build ([chapter 6](#6-building-from-source)).
3. Install ([chapter 7](#7-release-installation)), which places:

| Installed file (default prefix `/usr/local`)         | Notes |
|------------------------------------------------------|-------|
| `/usr/local/bin/tsfs`                                | daemon and command-line tool |
| `/usr/local/bin/tsfs-ls`                             | `ls` wrapper that tags the retention of each entry of a TSFS mount ([The TSFS ls](#the-tsfs-ls-replacing-the-system-ls)) |
| `/usr/bin/ls` (wrapper), `/usr/bin/ls.orig`          | root installation without `DESTDIR` only, unless built with `-DTSFS_REPLACE_SYSTEM_LS=OFF`: the system `ls` is renamed `ls.orig` and the wrapper installed as `/usr/bin/ls`; the `uninstall` target renames `ls.orig` back |
| `/usr/local/share/man/man1/tsfs.1`, `tsfs-ls.1`      | manual pages |
| `/usr/local/share/doc/tsfs/`                         | README, CHANGELOG, LICENSE, SECURITY, user and security documentation |
| `/usr/local/share/doc/tsfs/examples/config.example.yml` | commented example (refreshed at every install) |
| systemd unit directory`/tsfs.service`, `tsfs@.service` | from `pkg-config --variable=systemdsystemunitdir systemd`, else `<prefix>/lib/systemd/system`; installed **disabled** |
| `/etc/tsfs/config.yml`                               | created from the example **only if absent**, mode 0640; never overwritten |

The example configuration ships with `garbage_collector.dry_run: true`: a fresh
installation logs what it would delete and deletes nothing until you change it.

No data, log or state directory is created by the installation. The systemd unit
creates `/var/lib/tsfs` (`StateDirectory=`, mode 0700) and `/var/log/tsfs`
(`LogsDirectory=`, mode 0750) when the service starts. The backing directory and
the mount point must be created by the administrator.

## 5. Dependency installation

### Ubuntu 26.04 / Debian 13

```sh
sudo apt install build-essential g++ cmake ninja-build pkg-config \
    libfuse3-dev fuse3 libyaml-cpp-dev libspdlog-dev libfmt-dev zlib1g-dev \
    libgtest-dev libgmock-dev
# Optional: benchmarks, documentation, coverage, debugging
sudo apt install libbenchmark-dev doxygen graphviz lcov gdb
```

### Checking the dependencies

The `deps` target works even when a dependency is missing, and prints the
packages to install:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target deps
```

The feature sets checked are selected with `-DTSFS_DEPS_PROFILES="build;test"`
(available: `build`, `test`, `doc`, `coverage`, `bench`, `diagnostics`, `qa`).

### Older hosts: the development container

When the host compiler or libraries are too old, run every build command inside
the `tsfs-dev:26.04` Docker image (Ubuntu 26.04 with the complete toolchain):

```sh
scripts/tsfs-dev.sh cmake --preset release
scripts/tsfs-dev.sh cmake --build --preset release
scripts/tsfs-dev.sh                       # interactive shell in the container
```

The image is built on first use. The repository is mounted at `/src` inside the
container, which runs with your uid/gid, so the build tree keeps your ownership.
`/dev/fuse` and `CAP_SYS_ADMIN` are passed to the container so that the FUSE tests
can run (`--no-fuse` disables this). A binary built in the container links against
the container's libraries: install it on a host that provides the same library
versions (Ubuntu 26.04), or run it in the container.

## 6. Building from source

### With presets

`CMakePresets.json` defines these configure presets (build tree `build/<preset>`,
Ninja generator):

| Preset           | Build type     | Purpose |
|------------------|----------------|---------|
| `release`        | Release        | production (`-O2`, no debug information) |
| `relwithdebinfo` | RelWithDebInfo | production with debug symbols (`-O2 -g`), recommended when crash analysis matters |
| `debug`          | Debug          | development (`-Og -g3`, assertions, default log level `debug`, AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer by default) |
| `asan`           | Debug          | AddressSanitizer + LeakSanitizer + UndefinedBehaviorSanitizer, benchmarks off |
| `tsan`           | Debug          | ThreadSanitizer |
| `coverage`       | Debug          | gcov/lcov instrumentation (no sanitizer) |
| `clang`          | Debug          | Clang 21, warnings as errors (sanitizers as in `debug`) |
| `ci`             | Release        | warnings as errors |

```sh
cmake --preset release
cmake --build --preset release
```

### Without presets

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

Useful cache options: `-DTSFS_BUILD_TESTS=OFF`, `-DTSFS_BUILD_BENCHMARKS=OFF`,
`-DTSFS_DEFAULT_CONFIG_PATH=/etc/tsfs/config.yml` (the file used without
`--config`), `-DCMAKE_INSTALL_PREFIX=/usr`, `-DTSFS_SYSTEMD_UNIT_DIR=...`,
`-DTSFS_SANITIZE=none` (Debug build without sanitizers).

### Targets

| Target      | Command                                          | Effect |
|-------------|--------------------------------------------------|--------|
| `deps`      | `cmake --build build/release --target deps`      | check the dependencies, list missing packages |
| `build`     | `cmake --build build/release --target build`     | build the daemon, tests and benchmarks (same set as the default target) |
| `tests`     | `cmake --build build/release --target tests`     | build and run the test suite (JUnit report in `build/release/test-results.xml`) |
| `coverage`  | `cmake --build build/coverage --target coverage` | HTML coverage report (coverage preset) |
| `doc`       | `cmake --build build/release --target doc`       | API documentation with Doxygen |
| `bench`     | `cmake --build build/release --target bench`     | run the benchmarks (when Google Benchmark is installed) |
| `install`   | `sudo cmake --install build/release`             | install (see [chapter 7](#7-release-installation)) |
| `uninstall` | `sudo cmake --build build/release --target uninstall` | uninstall (see [chapter 22](#22-uninstallation)) |

The FUSE integration tests need `/dev/fuse`; they are skipped when it is not
available. The ThreadSanitizer preset inside the container needs ASLR disabled:
`scripts/tsfs-dev.sh --no-aslr ctest --test-dir build/tsan`.

Check the result:

```sh
build/release/src/tsfs --version
```

```text
tsfs 0.1.0
build: Release (GNU 15.2.0), C++ 202400L
libfuse: built with 3.18, running 3.18.2
```

More details for developers: [building.md](../developer/building.md).

## 7. Release installation

```sh
cmake --preset release                 # or relwithdebinfo, to keep symbols
cmake --build --preset release
cmake --build build/release --target tests     # optional
sudo cmake --install build/release
```

Then prepare the directories of the default configuration, review the
configuration, and start the service:

```sh
sudo mkdir -p /var/lib/tsfs/data /mnt/tsfs
sudoedit /etc/tsfs/config.yml          # mount point, backing directory, rules
sudo tsfs --check-config               # validate
sudo tsfs --dry-run                    # what would be deleted now
sudo systemctl daemon-reload
sudo systemctl enable --now tsfs.service
```

When the log shows the expected `dry run: would delete` lines, set
`garbage_collector.dry_run: false` and reload (`sudo systemctl reload tsfs`).

A staged installation (packaging) needs no privilege:
`DESTDIR=/tmp/stage cmake --install build/release`.

### The systemd unit

The installed `tsfs.service`:

- runs `tsfs --config /etc/tsfs/config.yml --foreground` as root, `Type=simple`;
- reloads with `ExecReload=/bin/kill -HUP $MAINPID` (`systemctl reload tsfs`);
- stops with `SIGTERM`, which unmounts cleanly (`TimeoutStopSec=30`);
- restarts on failure after 5 seconds;
- creates `/var/lib/tsfs` and `/var/log/tsfs`;
- sets `UMask=0027`, `LimitNOFILE=65536`, `NoNewPrivileges=yes` and a reduced
  capability bounding set (`CAP_SYS_ADMIN CAP_CHOWN CAP_FOWNER CAP_DAC_OVERRIDE
  CAP_DAC_READ_SEARCH CAP_SYS_RESOURCE`; no `CAP_MKNOD`, no `CAP_SETFCAP`).

`UMask=` does not affect the files created through the mount: the daemon calls
`umask(0)` at startup, because the kernel has already applied the umask of the
process that creates the entry. Every file the daemon creates for itself has an
explicit mode (log files 0640, crash reports 0600, status file 0640, the
directories it creates 0750).

When the daemon runs as root (as with this unit), the configuration file must be a
regular file owned by root and not writable by its group or by others (see
[chapter 9](#9-configuration-reference)); the installed `/etc/tsfs/config.yml`
(root, 0640) satisfies this.

Its sandboxing is deliberately limited: options such as `ProtectSystem=`,
`ProtectHome=`, `PrivateTmp=` or `PrivateDevices=` run the service in a private
mount namespace, and a FUSE mount created there would be invisible to the rest of
the system. Use a drop-in (`systemctl edit tsfs`) to adapt it, for example to pass
another configuration file.

### The TSFS ls replacing the system ls

`tsfs-ls` is a small standalone wrapper around the system `ls` (it does not need
the daemon or the TSFS library). It never lists anything itself: every listing is
produced by the original `ls`, so all the options of the system `ls` keep their
meaning. On a terminal, inside a TSFS mount, it appends the retention of each entry
to the listing (see
[Retention of a file](#retention-of-a-file-tsfs-ls-and-extended-attributes)).
Everywhere else it runs the original `ls` with the unchanged command line.

**By default, `sudo cmake --install` installs the wrapper as the system `ls`**, so
that a plain `ls -l` in a mount shows when files will be deleted:

- `tsfs-ls` is always installed as `<prefix>/bin/tsfs-ls`;
- then, only when the installation runs as root and without `DESTDIR` (never for
  staged or package installations), if `/usr/bin/ls` is the system `ls` (it does
  not contain the marker string `TSFS-LS-WRAPPER`), it is **renamed**
  `/usr/bin/ls.orig` (`rename(2)`, no copy). It may be a regular file (GNU
  coreutils) or a symbolic link (Ubuntu 26.04: `/usr/bin/ls ->
  ../lib/cargo/bin/coreutils/ls`, the uutils coreutils multi-call binary): a link
  is renamed as is. The wrapper is then installed as `/usr/bin/ls` (copied to a
  temporary name, mode 0755, then renamed: the replacement is atomic);
- installing again when `/usr/bin/ls` is already the wrapper and `ls.orig` exists
  only refreshes the wrapper. If a coreutils reinstallation or upgrade has put the
  system `ls` back over the wrapper, that `ls` is renamed over the stale
  `ls.orig`, then the wrapper is installed again;
- if `/usr/bin/ls` is the wrapper but `/usr/bin/ls.orig` is missing, the
  installation stops with an error asking to restore coreutils first: `sudo apt
  install --reinstall coreutils`, then install again.

The wrapper (`/usr/bin/ls`, or `/usr/local/bin/tsfs-ls`) runs the original `ls`
with `execv` and the **unchanged command line**, so that scripts and pipes get
exactly the output of the system `ls`, whenever:

1. no operand (default `.`) is inside a TSFS mount (filesystem type `fuse.tsfs` in
   `/proc/self/mountinfo`);
2. standard output is not a terminal (pipes, redirections, command substitution);
3. the environment contains `TSFS_LS=off`;
4. the options include `--help`, `--version`, `--zero`, `--dired` or `-D`.

Otherwise (an operand inside a TSFS mount, on a terminal, or with
`TSFS_LS=always`), the listing is still produced by the original `ls`, run with
the user's options followed by `-l --dired` (inserted before a `--`, so that a
later `-C`, `-x` or `-m` cannot cancel the long format) and the operands. The dired trailer of
`ls` (`//DIRED//`: byte offsets of every listed name; `//SUBDIRED//`: those of the
directory headers; supported by GNU coreutils and uutils coreutils) tells exactly
which entry is on which line, so sorting, `-a`/`-A`, `-R`, `-I`/`--hide`/`--ignore`,
`--time-style`, `-i`, `-s`, `-n`, `-h`, several operands and glob patterns keep
their meaning. Options that take their value as the next argument (`-I`, `-T`,
`-w`, `--block-size`, `--format`, `--hide`, `--ignore`, `--indicator-style`,
`--quoting-style`, `--sort`, `--tabsize`, `--time`, `--time-style`, `--width`) are
recognized, so their value is not taken for an operand.

- With a long format (`-l`, `-n`, `-g`, `-o`, `--format=long` or `verbose`), the
  lines of the original `ls` are kept (without the two-space dired indentation) and
  the tag is appended to each entry line.
- With any other format, each entry is printed on its own line followed by its tag
  (a column layout cannot carry it); the `total` lines are dropped, directory
  headers and blank lines are kept.
- If the output of the original `ls` has no dired trailer (an `ls` without
  `--dired` support), it is printed unchanged, without tags. Verified with GNU
  coreutils 8.32 and uutils coreutils 0.8.0, including `-C`, `-x`, `-m`, `-d`, `-o`.
- With `filesystem.retention_xattrs: false`, every entry is tagged
  `RETENTION UNKNOWN (filesystem.retention_xattrs is false)`.

Colors are not shown in TSFS listings (the original `ls` writes to a pipe), and
control characters in names are printed as `?`.

The original `ls` is looked up in this order: `$TSFS_REAL_LS`, then
`/usr/bin/ls.orig`, then the `PATH` (skipping the wrapper itself, recognized by its
marker string `TSFS-LS-WRAPPER-v2`). It always runs with `argv[0]` set to `ls`:
multi-call binaries such as uutils coreutils select the utility from it. When none
is found, the wrapper prints `ls: the original ls cannot be found
(/usr/bin/ls.orig missing; set TSFS_REAL_LS)` and exits with status 2.
`/usr/bin/ls.orig` can also be run directly (GNU `ls` and uutils alike).

To keep the system `ls`:

- at build time: `cmake --preset release -DTSFS_REPLACE_SYSTEM_LS=OFF` (`tsfs-ls`
  stays available under its own name; `-DTSFS_SYSTEM_LS=PATH` changes which `ls`
  is wrapped, default `/usr/bin/ls`);
- per shell or per user: `export TSFS_LS=off` (every call runs the original `ls`
  unchanged);
- afterwards: the `uninstall` target renames `ls.orig` back
  ([chapter 22](#22-uninstallation)).

Risks of the replacement, accepted by this default:

- `/usr/bin/ls` belongs to the `coreutils` package: `dpkg --verify coreutils` and
  `dpkg -S` based checks report it while the wrapper is in place, and a coreutils
  upgrade puts the system `ls` back over the wrapper (the tags disappear; the next
  TSFS installation renames it again).
- `ls` is used by every user and many scripts: a defect of the wrapper would affect
  them all. The pass-through rules limit this: scripts and pipes (no terminal) and
  every directory outside TSFS mounts get the original `ls` unchanged, and inside
  TSFS mounts the listing itself is still produced by the original `ls`.

The procedure (install, reinstall, uninstall, with uutils coreutils 0.8.0 where
`/usr/bin/ls` is a symbolic link) was verified as root in a disposable container:
the uninstallation restores the identical link, with the same target.

## 8. Debug installation

A Debug build is meant for development and for reproducing problems:

```sh
cmake --preset debug
cmake --build --preset debug
sudo cmake --install build/debug
```

Differences from a Release build:

- no optimization beyond `-Og`, full debug information (`-g3`), assertions enabled,
  no `_FORTIFY_SOURCE`;
- the default `logging.level` is `debug` instead of `info` (an explicit
  `logging.level` in the file, or `-l`, still wins);
- log lines carry a `src=<file>:<line>` field;
- AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer are enabled by
  default (`TSFS_SANITIZE` defaults to `address;undefined` for `CMAKE_BUILD_TYPE=Debug`
  without coverage). The daemon is slower and uses more memory; a memory error or
  undefined behavior aborts it with a report on stderr, and a memory leak is
  reported at exit and makes the process exit with a non-zero status (1). Configure
  with `-DTSFS_SANITIZE=none` for an uninstrumented Debug build (for example to run
  it under valgrind):

  ```sh
  cmake --preset debug -DTSFS_SANITIZE=none
  ```

For a production system that must remain diagnosable, prefer the
`relwithdebinfo` preset: optimized code with debug symbols, usable by `gdb` and
`addr2line` (see [chapter 20](#20-core-dump-analysis)).

To run a development build in the foreground without installing it:

```sh
sudo build/debug/src/tsfs -c /etc/tsfs/config.yml -d
```

`-d` keeps the process in the foreground, sets the log level to `debug` and copies
every log line to stderr (it only applies when mounting: combined with an action
such as `--check-config`, it is rejected with exit status 2). `--fuse-debug` additionally prints every FUSE request.

Sanitized builds (the default `debug` preset, `asan`, `tsan`, and `clang`, which is a
Debug build too) produce instrumented binaries for testing; do not install them on
production systems.

## 9. Configuration reference

The configuration file (default `/etc/tsfs/config.yml`) is YAML with these
sections:

| Section             | Purpose                                                | Reload |
|---------------------|--------------------------------------------------------|--------|
| `version`           | schema version, must be `1` (mandatory)                | n/a |
| `filesystem`        | mount point, backing directory, `allow_other`, `read_only`, `retention_xattrs` | restart |
| `retention`         | default lifetime, timestamp source, rules              | reload |
| `garbage_collector` | enable, dry run, scan interval, workers, throttling, retries | reload |
| `logging`           | file, level, format, async queue, rotation             | reload |
| `diagnostics`       | crash reports and core dumps                           | restart |
| `metrics`           | status file                                            | reload |
| `security`          | `stay_on_filesystem`, `require_openat2`                | restart |

Essentials:

- Only `version: 1` is mandatory; everything else has a default.
- Unknown and duplicate keys are rejected.
- Durations: `500ms`, `30s`, `30m`, `12h`, `1d`, `2w`, `1d12h` (largest unit first,
  no fraction). Sizes: `4096`, `512KiB`, `50MiB`, `1GB`.
- Paths in retention rules are relative to the TSFS root: `/tmp/record` means
  `<backing_directory>/tmp/record`.
- `tsfs --check-config` validates a file; `tsfs --show-config` prints the effective
  configuration with every default; `tsfs --print-example-config` prints the
  commented example.
- File trust: the file must be a regular file. When the daemon runs as root
  (effective uid 0), it must also be owned by root and not writable by its group or
  by others; otherwise TSFS refuses it (exit 5, "fix with chmod go-w"). A symbolic
  link is followed and its target is checked.
- `-m`/`--mountpoint` and `-b`/`--backing-dir` are normalized and validated like the
  values of the file; an invalid value exits with status 5 and a message starting
  with `command line:`.

The complete reference is [configuration.md](configuration.md).

## 10. Retention policy examples

Every example below is a complete configuration file that passes
`tsfs --check-config`. Sections not shown keep their defaults (mount point
`/mnt/tsfs`, backing directory `/var/lib/tsfs/data`, and so on). Paths in rules are
relative to the TSFS root.

### Temporary video recordings: one day

```yaml
version: 1
retention:
  default: "never"
  rules:
    # Every file below /tmp/record, at any depth, one day after its last modification.
    - id: temporary-recordings
      path: "/tmp/record"
      recursive: true
      retention: "1d"
```

`<mountpoint>/tmp/record/cam1/2026-10-13.mp4` is deleted one day after it was last
written. Files elsewhere are kept forever (`default: "never"`).

### All MP4 files: one day

```yaml
version: 1
retention:
  rules:
    # Every MP4 file anywhere in the tree (video.mp4, CLIP.MP4, a/b/c.Mp4): one day.
    - id: video-files
      extension: "mp4"
      retention: "1d"
```

`extension` is case-insensitive and needs at least one character before the dot:
`CLIP.MP4` and `.hidden.mp4` match, a file named just `.mp4` does not. Several
extensions can be listed: `extension: ["mp4", "mkv", "tar.gz"]`.

### A temporary cache: thirty minutes

```yaml
version: 1
retention:
  rules:
    # Cache entries: thirty minutes after the last write.
    - id: temporary-cache
      path: "/cache"
      recursive: true
      retention: "30m"
garbage_collector:
  # A 30-minute lifetime is enforced within seconds of expiry by the scheduler;
  # a shorter scan interval only matters for files written outside the mount.
  scan_interval: "1m"
```

Each write to a cache file restarts its 30 minutes (`timestamp: modification`).
To count from creation instead, add `timestamp: "birth"` to the rule.

### Application logs: seven days

```yaml
version: 1
retention:
  rules:
    # Application logs (current and rotated: app.log, app.log.1, app.log.2.gz...): seven days.
    - id: application-logs
      path: "/var/log/application"
      recursive: true
      retention: "7d"
```

An active log file that is written continuously never reaches its expiry, since
each write updates its modification time; rotated files stop changing and are
deleted seven days after their last write. Do not point TSFS's own
`logging.directory` inside the backing directory: the loader rejects it.

### Core dumps: two days

```yaml
version: 1
retention:
  rules:
    # "core.1234", "core.myapp.1234" anywhere in the tree: two days.
    - id: core-dumps-numbered
      glob: "core.*"
      retention: "2d"
    # "*.core" files (BSD-style names) anywhere: two days.
    - id: core-dumps-suffix
      glob: "*.core"
      retention: "2d"
```

A `glob` applies to the file name at any depth. To limit it to a directory, add
`path` (and `recursive`), or use a `path_glob` such as `/crash/**/core.*`.

### A global retention policy

```yaml
version: 1
retention:
  # Every file matched by no rule is deleted 30 days after its last modification.
  default: "30d"
  timestamp: "modification"
```

`retention.default` applies to every regular file that no rule matches. Rules
still take precedence over it; combine it with exclusions for what must never be
deleted.

### A directory-specific exception

```yaml
version: 1
retention:
  default: "7d"
  rules:
    # Directory-specific exception: nothing below /projects/keep is ever deleted,
    # whatever the default or any other rule (even with a higher priority) says.
    - id: keep-projects
      action: "exclude"
      path: "/projects/keep"
      recursive: true
```

Everything is deleted after seven days, except the files below `/projects/keep`.
An exclusion can use any selector: `file` for one file, `glob`, `extension` or
`path_glob`. To give a directory a *different* lifetime rather than an absolute
protection, use a more specific `delete` rule instead, for example
`path: "/projects/archive"`, `recursive: true`, `retention: "90d"`.

### A dry-run cleanup

```yaml
version: 1
retention:
  default: "14d"
  rules:
    - id: temporary-recordings
      path: "/tmp/record"
      recursive: true
      retention: "1d"
garbage_collector:
  # The daemon evaluates and logs "dry run: would delete" (gc_candidate_found)
  # instead of deleting. Set to false once the log looks right.
  dry_run: true
```

Before enabling a policy, list what it would delete right now, without mounting
and without deleting anything:

```sh
sudo tsfs --dry-run -c /etc/tsfs/config.yml
```

See [chapter 15](#15-dry-run-mode) for the output format.

### A runtime policy update

Starting from:

```yaml
version: 1
retention:
  rules:
    - id: temporary-recordings
      path: "/tmp/record"
      recursive: true
      retention: "1d"
```

shorten the lifetime and add a rule while the filesystem stays mounted:

```yaml
version: 1
retention:
  rules:
    # Lifetime shortened from 1d to 12h: applies to existing files at once.
    - id: temporary-recordings
      path: "/tmp/record"
      recursive: true
      retention: "12h"
    # New rule added at runtime.
    - id: video-files
      extension: "mp4"
      retention: "1d"
```

Procedure:

```sh
sudo cp /etc/tsfs/config.yml /etc/tsfs/config.yml.new
sudoedit /etc/tsfs/config.yml.new
sudo tsfs --check-config -c /etc/tsfs/config.yml.new     # must print "configuration valid"
sudo tsfs --dry-run -c /etc/tsfs/config.yml.new          # what the new policy deletes now
sudo mv /etc/tsfs/config.yml.new /etc/tsfs/config.yml    # applied automatically (inotify)
sudo systemctl reload tsfs                               # or explicitly: SIGHUP
```

Any one of the last two steps is enough: the daemon watches the file, and
`systemctl reload` (or `kill -HUP <pid>`) forces a reload. The log confirms it:

```text
... INFO ... component=config event=config_reload_started file="/etc/tsfs/config.yml" trigger="inotify" msg="reloading the configuration"
... INFO ... component=config event=config_reload_succeeded file="/etc/tsfs/config.yml" generation=2 rules=2 msg="configuration reloaded"
```

Files already older than 12 hours below `/tmp/record` are deleted right after the
reload. See [chapter 13](#13-runtime-configuration-reload).

### A complete production configuration

```yaml
version: 1

filesystem:
  mountpoint: "/srv/tsfs"
  backing_directory: "/var/lib/tsfs/data"
  allow_other: true
  read_only: false

retention:
  default: "90d"
  timestamp: "modification"
  rules:
    - id: temporary-recordings
      path: "/tmp/record"
      recursive: true
      retention: "1d"
    - id: video-files
      extension: ["mp4", "mkv"]
      retention: "1d"
    - id: temporary-cache
      path: "/cache"
      recursive: true
      retention: "30m"
    - id: application-logs
      path: "/var/log/application"
      recursive: true
      retention: "7d"
    - id: core-dumps
      glob: "core.*"
      retention: "2d"
    - id: keep-important-recordings
      action: "exclude"
      path: "/tmp/record/keep"
      recursive: true

garbage_collector:
  enabled: true
  dry_run: false
  scan_interval: "5m"
  max_scan_duration: "2s"
  max_workers: 4
  max_deletions_per_second: 500
  max_pending_tasks: 200000

logging:
  directory: "/var/log/tsfs"
  filename: "tsfs.log"
  level: "info"
  format: "json"
  rotation:
    max_file_size: "100MiB"
    max_files: 14
    compress: true

diagnostics:
  enabled: true
  text_stack_trace: true
  native_core_dump: false
  crash_directory: "/var/lib/tsfs/crash"

metrics:
  enabled: true
  status_file: "/run/tsfs/status.json"
  interval: "10s"
```

### Rule precedence and expiration semantics

#### Expiration

A file expires at **timestamp + retention**, where the timestamp is the file's
modification time (`timestamp: modification`, the default) or its creation time
(`timestamp: birth`, falling back to the modification time when the backing
filesystem does not report it). The file is eligible for deletion from that
instant on (a file evaluated exactly at its expiry is expired).

Examples with `retention: "1d"` and `timestamp: modification`:

| Event                                                   | Expiry |
|---------------------------------------------------------|--------|
| file written on Oct 13 at 10:00, never modified again   | Oct 14 at 10:00 |
| same file appended to on Oct 13 at 18:00                | Oct 14 at 18:00 (each write restarts the lifetime) |
| `touch -d "2 days ago" file`                            | already expired: deleted within seconds |
| modification time set in the future (Oct 20)           | Oct 21 |
| file only read                                          | unchanged (reading does not modify `st_mtime`) |
| file renamed or moved within the mount                  | unchanged timestamp, but the rules of the *new* path apply |

With `timestamp: birth`, writes do not postpone the expiry; only the creation time
counts.

A change of policy applies to existing files: after a reload, every file is
re-evaluated, and a file whose new expiry is already past is deleted at once.

#### Precedence

For each regular file, TSFS decides in three steps:

1. **Exclusions first.** If any enabled `exclude` rule matches, the file is never
   deleted, regardless of priorities.
2. **The best matching `delete` rule.** Rules are ordered by, in this order:
   `priority` (higher first); scope kind (`file` > `path_glob` > `path` > no scope);
   scope depth (deeper first); non-recursive before recursive; presence of a `glob`
   or `extension`; number of literal characters in the patterns; and finally
   declaration order. The first matching rule in that order decides the lifetime,
   even if it is `never`.
3. **The default.** If no rule matches, `retention.default` applies (`never` by
   default).

Worked example (`10-precedence`):

```yaml
version: 1
retention:
  default: "30d"
  rules:
    - id: all-mp4                 # pattern only (no scope)
      extension: "mp4"
      retention: "1d"
    - id: recordings              # recursive directory scope
      path: "/rec"
      recursive: true
      retention: "7d"
    - id: recordings-today        # non-recursive directory scope, deeper
      path: "/rec/today"
      retention: "2h"
    - id: urgent                  # explicit priority beats every specificity criterion
      glob: "urgent-*"
      priority: 10
      retention: "10m"
    - id: keep-legal              # exclusion: always wins
      action: "exclude"
      path_glob: "/rec/**/legal-*"
```

| File (age)                          | Matching rules                         | Decision | Why |
|-------------------------------------|----------------------------------------|----------|-----|
| `/other/clip.mp4` (3 h)             | all-mp4                                | 1d, kept for now | only rule that matches |
| `/other/notes.txt` (40 d)           | none                                   | default 30d: **deleted** | no rule matches |
| `/rec/clip.mp4` (3 d)               | all-mp4, recordings                    | 7d, kept | a `path` scope beats a pattern-only rule |
| `/rec/old.txt` (8 d)                | recordings                             | 7d: **deleted** | |
| `/rec/today/cam1.mp4` (3 h)         | all-mp4, recordings, recordings-today  | 2h: **deleted** | deeper scope (`/rec/today`) wins |
| `/rec/today/sub/cam2.mp4` (3 h)     | all-mp4, recordings                    | 7d, kept | `recordings-today` is not recursive |
| `/rec/today/urgent-1.bin` (20 min)  | urgent, recordings, recordings-today   | 10m: **deleted** | priority 10 beats every scope |
| `/rec/2026/legal-hold.mp4` (100 d)  | keep-legal, all-mp4, recordings        | **never deleted** | exclusion wins |
| `/rec/2026/legal-x/a.mp4` (100 d)   | all-mp4, recordings                    | 7d: **deleted** | `path_glob` matches the whole file path: the last component `a.mp4` does not match `legal-*` |

`tsfs --dry-run` on that tree prints exactly the files marked deleted:

```text
EXPIRED AT (UTC)                 SIZE  RULE                     PATH
2026-10-03T08:30:19Z            5  default                  /other/notes.txt
2026-07-12T08:30:19Z            5  recordings               /rec/2026/legal-x/a.mp4
2026-10-12T08:30:19Z            5  recordings               /rec/old.txt
2026-10-13T07:30:19Z            5  recordings-today         /rec/today/cam1.mp4
2026-10-13T08:20:19Z            5  urgent                   /rec/today/urgent-1.bin
dry run: 5 file(s), 25 byte(s) would be deleted now; 9 file(s) scanned in /var/lib/tsfs/data
```

To protect a whole directory with a `path_glob`, end the pattern with `/**`
(`/rec/**/legal-*/**`), or use `path` with `recursive: true`.

### Hidden files and globs

Hidden files (names starting with a dot) are **not** special:

- `glob: "*"` matches `.lock`; `glob: "*.log"` matches `.debug.log`;
  `glob: ".*.part"` matches only hidden `.part` files.
- `path_glob` components behave the same way: `/tmp/*` matches `/tmp/.lock`, and
  `**` crosses hidden directories such as `/.cache/`.
- `path` rules and `retention.default` apply to hidden files like any other file.
- `extension` needs at least one character before the extension: `.mp4` does not
  match `extension: mp4`, but `.hidden.mp4` does.

This is verified by the test suite (`tests/policy/test_policy.cpp`:
`EXPECT_TRUE(base("*", ".hidden"))`, `ExtensionRuleIsCaseInsensitiveAndNeedsAStem`).
To protect hidden files, add an exclusion such as `glob: ".*"` with
`action: "exclude"`.

Example (`11-hidden`):

```yaml
version: 1
retention:
  rules:
    - id: everything-in-tmp        # "*" also matches dot files such as ".lock"
      path: "/tmp"
      glob: "*"
      retention: "1h"
    - id: partial-downloads
      glob: ".*.part"              # only hidden partial files
      retention: "6h"
```

On a tree where every file is older than its retention, `/tmp/.lock`,
`/tmp/visible.txt` and `/dl/.movie.mkv.part` are listed by `tsfs --dry-run`;
`/dl/movie.mkv.part` (not hidden) and `/tmp/.cache/x.bin` (in a sub-directory of
the non-recursive `/tmp` rule) are not.

### Directory recursion

- The garbage collector walks the **whole** backing tree, including hidden
  directories. Recursion is about which rules *match*, not about what is scanned.
- `path: "/a"` (`recursive: false`, the default) matches files whose parent
  directory is exactly `/a`: `/a/x` matches, `/a/b/x` does not.
- `path: "/a"` with `recursive: true` matches files at any depth below `/a`:
  `/a/x`, `/a/b/x`, `/a/b/c/x`. `/ab/x` does not match (paths are compared
  component by component).
- `glob` and `extension` without a scope match at any depth.
- `path_glob`: `*` stays within one component; `**` (a whole component) matches
  zero or more directories: `/rec/**/a.mp4` matches `/rec/a.mp4` and
  `/rec/x/y/a.mp4`.
- Only regular files are deleted. Directories are never deleted, even when empty;
  symbolic links, devices, FIFOs and sockets are never deleted.
- Symbolic links are never followed by the scan, and a directory on another
  filesystem below the backing directory is skipped while
  `security.stay_on_filesystem` is `true`.

## 11. Mounting the filesystem

The daemon mounts the filesystem when started without an action option. Before
mounting, it checks that the configuration is valid, that the log file can be
opened, that the backing directory exists, that `/dev/fuse` is usable (exit 4 when
it does not exist, exit 3 when it is not accessible), that `filesystem.allow_other`
is permitted (root, or `user_allow_other` in `/etc/fuse.conf`; exit 3 otherwise) and
that the mount point exists and is a directory; any failure is reported on stderr
with a non-zero exit status.

### Creating a TSFS filesystem and its mount point, step by step

A TSFS filesystem is the association of **a backing directory** (where the files
are really stored, on an existing Linux filesystem) with **a mount point** (the
directory applications use), described by **one configuration file** and served by
**one `tsfs` daemon**. TSFS formats nothing and has no on-disk format of its own:
it presents the backing directory through FUSE and applies the retention rules.
The example below creates a filesystem named `recordings`, mounted on
`/mnt/recordings`.

**1. Choose and prepare the backing storage.** Any directory of a local filesystem
works (ext4, XFS, Btrfs...). A dedicated filesystem is recommended for retention
data: a full disk then only affects this data, and with
`security.stay_on_filesystem: true` (default) the GC and the mount never leave it.

```sh
# a) a directory of an existing filesystem
sudo mkdir -p /srv/tsfs/recordings/data

# b) or a dedicated filesystem (LVM example; adapt the volume group and the size)
sudo lvcreate -L 50G -n recordings vg0
sudo mkfs.ext4 -L tsfs-recordings /dev/vg0/recordings
sudo mkdir -p /srv/tsfs/recordings
echo 'LABEL=tsfs-recordings /srv/tsfs/recordings ext4 defaults,nodev,nosuid 0 2' | sudo tee -a /etc/fstab
sudo mount /srv/tsfs/recordings
sudo mkdir /srv/tsfs/recordings/data
```

Use a sub-directory (`data`) of the dedicated filesystem rather than its root: the
root of an ext4 filesystem holds `lost+found`, which would otherwise appear in the
mount. Rules:

- the backing directory and the mount point must not contain each other, and
  neither may be `/` (the configuration is rejected, exit 5);
- the log directory, the crash directory and the status file must be outside both;
- anything mounted below the backing directory is unreachable through TSFS
  (`EXDEV`) while `security.stay_on_filesystem` is `true`;
- rules with `timestamp: birth` need a filesystem that reports the birth time
  (ext4, XFS and Btrfs do; otherwise the modification time is used).

**2. Create the mount point**, an empty directory (whatever it contains is hidden
while TSFS is mounted on it):

```sh
sudo mkdir -p /mnt/recordings
```

**3. Write the configuration** `/etc/tsfs/recordings.yml`, owned by root and not
writable by others (a daemon running as root refuses it otherwise, exit 5):

```yaml
# /etc/tsfs/recordings.yml - TSFS filesystem "recordings" (systemd: tsfs@recordings.service)
version: 1

filesystem:
  mountpoint: "/mnt/recordings"                     # what applications use
  backing_directory: "/srv/tsfs/recordings/data"    # where the files are stored
  allow_other: true                                 # let every user reach the mount (permissions still apply)

retention:
  default: never                                    # nothing expires unless a rule says so
  rules:
    - id: cameras
      path: "/cameras"
      recursive: true
      retention: "1d"
    - id: keep
      action: exclude
      glob: "*.keep"

garbage_collector:
  dry_run: true                                     # first run: log what would be deleted only

logging:
  directory: "/var/log/tsfs/recordings"             # created by LogsDirectory=tsfs/%i

diagnostics:
  enabled: true
  crash_directory: "/var/lib/tsfs/recordings/crash" # under StateDirectory=tsfs/%i

metrics:
  status_file: "/run/tsfs/recordings.json"          # one status file per filesystem
```

```sh
sudo chown root:root /etc/tsfs/recordings.yml
sudo chmod 0640 /etc/tsfs/recordings.yml
```

**4. Validate it and preview the GC** (nothing is mounted, nothing is deleted):

```sh
sudo tsfs --check-config -c /etc/tsfs/recordings.yml
sudo tsfs --dry-run -c /etc/tsfs/recordings.yml
```

**5. Mount it.** With systemd, the template unit `tsfs@.service` runs one daemon
per configuration file `/etc/tsfs/NAME.yml` (here `NAME` = `recordings`) and creates
`/var/log/tsfs/recordings` and `/var/lib/tsfs/recordings`:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now tsfs@recordings.service   # mounted now and at every boot
```

Or by hand, without systemd (create the log directory first):

```sh
sudo mkdir -p /var/log/tsfs/recordings
sudo tsfs -c /etc/tsfs/recordings.yml                 # returns once mounted
```

**6. Check the mount:**

```sh
findmnt /mnt/recordings          # TARGET /mnt/recordings  SOURCE tsfs  FSTYPE fuse.tsfs
df -h /mnt/recordings            # capacity of the backing filesystem
echo test > /mnt/recordings/hello && ls -l /srv/tsfs/recordings/data/hello
sudo tsfs --status -c /etc/tsfs/recordings.yml
```

Applications must use the mount point, not the backing directory: only files
written through the mount notify the GC immediately and are protected while they
are open (files written directly in the backing directory are found by the next
reconciliation pass, see [Garbage collector behavior](#14-garbage-collector-behavior)).

**7. Enable deletion** once the dry-run log (`event=gc_candidate_found` with
`msg="dry run: would delete"`, or `tsfs --dry-run`) shows the expected files: set `garbage_collector.dry_run: false`
and reload, without unmounting:

```sh
sudoedit /etc/tsfs/recordings.yml
sudo systemctl reload tsfs@recordings.service
```

### Several TSFS filesystems (one per mount point)

Each filesystem is an independent daemon with its own configuration file:
`/etc/tsfs/recordings.yml` -> `tsfs@recordings.service`, `/etc/tsfs/cache.yml` ->
`tsfs@cache.service`, and so on. The non-template `tsfs.service` serves
`/etc/tsfs/config.yml` and is just one more filesystem. Two daemons must never
share any of these settings:

| Setting | Why it must be unique |
|---|---|
| `filesystem.mountpoint` | a second daemon on a mounted mount point is refused ("is already a mount point", exit 7) |
| `filesystem.backing_directory` | two GCs and two notification streams on the same files |
| `logging.directory` (or `logging.filename`) | two daemons rotating the same log file |
| `metrics.status_file` | the last writer wins |
| `diagnostics.crash_directory` | report pruning of one instance removes the reports of the other |

Two instances were verified side by side (2-second and 1-hour retentions on two
mount points: each GC only acts on its own files).

### Mounting at boot

Enable the service (`systemctl enable tsfs@NAME.service`): it starts after the
local filesystems (`After=local-fs.target`). When the backing directory or the
mount point lives on a filesystem that is mounted later (network storage, an
encrypted volume...), order the instance after it with a drop-in:

```sh
sudo systemctl edit tsfs@recordings.service
#   [Unit]
#   RequiresMountsFor=/srv/tsfs/recordings /mnt/recordings
```

`/etc/fstab` cannot mount TSFS directly: `mount -t fuse.tsfs` calls the binary
with the mount helper conventions (`tsfs SOURCE MOUNTPOINT -o OPTIONS`), which the
command line does not accept (exit 2, "unknown option '-o'"); the configuration
file is the single description of a TSFS filesystem. Use the systemd units instead.

### With systemd (recommended)

For the single filesystem described by `/etc/tsfs/config.yml` (paths of the
example configuration):

```sh
sudo mkdir -p /var/lib/tsfs/data /mnt/tsfs     # paths of the configuration
sudo systemctl enable --now tsfs.service
systemctl status tsfs
findmnt /mnt/tsfs
```

`findmnt` shows a `fuse.tsfs` filesystem with source `tsfs`.

### Manually

```sh
sudo tsfs -c /etc/tsfs/config.yml               # detaches into the background
sudo tsfs -c /etc/tsfs/config.yml -f            # stays in the foreground
sudo tsfs -c /etc/tsfs/config.yml -d            # foreground, debug log on stderr
sudo tsfs -m /srv/tsfs -b /data/tsfs            # override the paths of the file
```

Without `-f`, `tsfs` returns once the filesystem is mounted, and the daemon keeps
running in the background. Messages emitted before the log file is opened (such as
`filesystem_mount_started`) go to stderr.

### As a regular user

A non-root user can mount TSFS through `fusermount3` (package `fuse3`). Every
path must be accessible to that user, including the log directory and the status
file, whose defaults (`/var/log/tsfs`, `/run/tsfs/status.json`) are normally not:

```yaml
version: 1
filesystem:
  mountpoint: "/home/alice/tsfs"
  backing_directory: "/home/alice/.local/share/tsfs/data"
retention:
  rules:
    - id: downloads
      path: "/downloads"
      recursive: true
      retention: "7d"
logging:
  directory: "/home/alice/.local/state/tsfs"
metrics:
  status_file: "/home/alice/.local/state/tsfs/status.json"
```

```sh
mkdir -p ~/tsfs ~/.local/share/tsfs/data
tsfs -c ~/.config/tsfs/config.yml
```

`allow_other: true` additionally requires `user_allow_other` in `/etc/fuse.conf`;
without it, the daemon refuses to start with exit status 3.

### Mount options and permissions

TSFS always mounts with `default_permissions`: the kernel checks the ownership and
mode bits of each file against the caller, as on a local filesystem. With
`allow_other: false` (the default), only the user running the daemon (root for the
service) can access the mount. When the daemon runs as root, files and directories
created through the mount are owned by the calling user (and by the group of a
set-group-ID parent directory). The ownership is handed over on a descriptor of the
new entry, and only if it is still the entry the daemon has just created (expected
type, owned by the daemon, a directory with no entry yet); an entry replaced in
the meantime keeps its ownership. A file that already exists when `create` runs is
opened as is, without changing its ownership. The mode of a new entry is the mode
requested by the application after its own umask: the daemon's umask (for example
`UMask=` of the systemd unit) is not applied a second time. `read_only: true`
mounts read-only for applications; the garbage collector still deletes expired
files.

With `security.stay_on_filesystem: true` (the default), an entry located on another
filesystem than the backing directory (a mount point or a file bind-mounted below
it) is unreachable through the mount: the operation fails with `EXDEV` ("Invalid
cross-device link"). This applies to intermediate path components and to the final
component (`stat`, `access`, `open`, `create` of an existing file, `truncate`,
`chmod`, `chown`, `utimens`). A bind mount of the **same** filesystem has the same
device number: with `openat2` (Linux 5.6 or later) it is refused for intermediate
components, but the fallback walk of older kernels and the final-component check
compare device numbers only and do not detect it. Set
`security.require_openat2: true` where this matters.

## 12. Unmounting the filesystem

Any of these stops the daemon cleanly:

```sh
sudo systemctl stop tsfs                 # service: SIGTERM
sudo kill -TERM <pid>                    # or SIGINT (Ctrl-C in the foreground)
sudo umount /mnt/tsfs                    # root
fusermount3 -u ~/tsfs                    # the user who mounted it
```

On shutdown the daemon stops the configuration watcher, stops the garbage
collector (each worker finishes the file it is processing; pending candidates are
abandoned and found again at the next start), writes a final status file, unmounts,
and flushes the log:

```text
... INFO ... component=fs event=filesystem_unmounted loop_status=15 msg="shutting down"
... INFO ... component=gc event=gc_completed deleted=1 bytes_deleted=4 failed=0 pending_abandoned=0 msg="garbage collector stopped"
```

The exit status is 0 for a shutdown by `SIGTERM`, `SIGINT` or an external unmount.
A `SIGTERM` or `SIGINT` received while the daemon is still starting is held until
the mount is ready, then handled the same way (clean unmount, exit 0): it never
leaves a dead mount behind.

Unmounting does not touch the data: files stay in the backing directory. When
TSFS is started again, files that expired while it was stopped are deleted by the
first reconciliation pass.

If `umount` reports that the target is busy, a process still uses the mount
(`fuser -vm /mnt/tsfs` lists it).

## 13. Runtime configuration reload

A running daemon reloads its configuration file:

- automatically when the file content changes (inotify watch on the directory of
  the file, 250 ms debounce; a change that leaves the content identical is
  ignored, and replacing the file with `mv` is detected);
- on `SIGHUP`: `sudo kill -HUP <pid>`;
- with `sudo systemctl reload tsfs` (sends `SIGHUP`).

What happens:

1. The file is parsed, validated and compiled. Any error rejects the reload:

   ```text
   ... ERROR ... component=config event=config_reload_failed file="/etc/tsfs/config.yml" error="/etc/tsfs/config.yml:19:1: garbage_collector: duplicate key" generation=2 msg="reload rejected; the previous configuration stays active"
   ```

2. Settings that need a restart (`filesystem.*`, `diagnostics.*`, `security.*`)
   keep their running value, with one warning per changed setting:

   ```text
   ... WARNING ... component=config event=config_restart_required setting="filesystem.mountpoint" msg="this setting requires a restart; the running value is kept"
   ```

3. The new logging configuration is applied (a failure, such as an unwritable new
   log directory, rejects the whole reload), then the new retention policy and
   garbage collector settings.
4. `config_reload_succeeded` is logged with the new `generation`, and the garbage
   collector starts a reconciliation pass that re-evaluates every file under the
   new policy.

Command-line overrides (`-m`, `-b`, `-l`, `-d`, diagnostics options) stay in force
after every reload (`-m` and `-b` are restart-only settings anyway).

Recommendations:

- Edit a copy, validate it with `tsfs --check-config -c copy`, preview it with
  `tsfs --dry-run -c copy`, then move it into place. Editing the active file in
  place triggers a reload at each save; an invalid intermediate state is rejected
  safely, but a valid intermediate state would be applied.
- Shortening a retention or adding a rule can delete many files immediately after
  the reload. Preview with `--dry-run`, or reload with `garbage_collector.dry_run:
  true` first.
- Check `config.generation` and `config.reload_failures` in the status file
  ([monitoring.md](monitoring.md)).

Details: [configuration-reload.md](../architecture/configuration-reload.md).

## 14. Garbage collector behavior

The garbage collector separates **discovery** from **deletion**:

- **Reconciliation passes.** Every `scan_interval` (default 1 minute), a
  coordinator thread walks the backing tree, split into time slices of at most
  `max_scan_duration` (default 5 s). Each regular file is evaluated against the
  policy. A file that expires within the next two scan intervals is put into the
  **expiration scheduler**; files expiring later are found again by later passes,
  so memory does not grow with the number of files.
- **Change notifications.** Files created, written, truncated, renamed or linked
  through the mount, or whose times are changed, are evaluated immediately, so a
  short retention is honored even with a long scan interval. Files modified
  directly in the backing directory (bypassing the mount) are seen at the next pass.
- **Scheduling.** The coordinator sleeps until the earliest expiry, the next pass,
  a notification or a reload, and hands due files to `max_workers` worker threads,
  throttled to `max_deletions_per_second` (candidates examined, that is validated
  and then deleted or not, per second) and to `max_deletions_per_cycle`
  (candidates dispatched to the workers per scan cycle).
- **Validation before deletion.** Under a lock on the path and its ancestors, the
  worker re-resolves the path (without following symbolic links, without leaving
  the backing directory), re-reads the file's metadata and deletes only if the file
  is still the same inode, still a regular file on the backing filesystem, not open
  through the mount (`skip_open_files`), and still expired under the *current*
  policy. Otherwise it records why (`gc_file_skipped` with a `reason`).
- **Retries.** Transient failures (for example `EBUSY`, `EIO`, `EACCES`, `EPERM`)
  are retried up to `retry_limit` times with exponential backoff: retry *n* waits
  `retry_initial_delay * 2^(n-1)` (the first retry waits `retry_initial_delay`),
  capped at `retry_max_delay`. Exhausted retries and permanent
  failures are logged as `gc_delete_failed`; the next pass tries again.
- **Bounded memory.** The scheduler holds at most `max_pending_tasks` entries;
  overflow is counted (`queue_saturations`), logged (`gc_queue_saturated`) and
  recovered by the next pass.
- **Clock changes.** Expiries use the wall clock. The coordinator never sleeps more
  than one minute, so a clock step is taken into account within a minute. Setting
  the clock forward makes files expire earlier.

What is never deleted: directories (even empty), symbolic links, special files,
files matched by an exclusion, files matched by a rule or default with `never`,
files open through the mount (while `skip_open_files` is `true`), and anything
outside the backing directory.

Hard links: each path is evaluated with its own rules. Deleting one link of a file
with several links frees no space; such deletions count 0 bytes in
`bytes_deleted`.

Disable the garbage collector without unmounting by setting
`garbage_collector.enabled: false` and reloading.

### Purging expired files now: `tsfs --purge`

The rate limits protect the storage, but can leave a backlog of expired files (for
example after shortening a retention, or with `enabled: false`). `tsfs --purge`
deletes **now** every file that is already expired:

```sh
sudo tsfs --purge --dry-run -c /etc/tsfs/config.yml   # preview: the --dry-run listing
sudo tsfs --purge -c /etc/tsfs/config.yml             # asks for confirmation
sudo tsfs --purge --force -c /etc/tsfs/config.yml     # scripts: no question
```

- `max_deletions_per_second` and `max_deletions_per_cycle` are ignored; every
  other check of a regular deletion applies to each file (same inode, regular file
  inside the managed tree, not open through the mount, still expired under the
  current policy). Excluded files and files whose retention is `never` are never
  touched; files that expire later are kept.
- Complete reconciliation passes are repeated while the bounded scheduler
  saturates, so a backlog larger than `max_pending_tasks` is purged too.
- It works with `garbage_collector.enabled: false`.
- With `garbage_collector.dry_run: true`, a purge would delete nothing: `tsfs
  --purge` refuses with exit status 7 and a message pointing to the preview.

**Who deletes.** Pass the same configuration (`-c`) as the daemon:

- When a daemon serves the mount point, the purge is done by the daemon, the only
  process that knows which files are open through the mount. `tsfs --purge` finds
  it through `metrics.status_file` (its `pid` must be alive, the process must be
  named `tsfs`, and its `mountpoint` must be the configured one), sends it
  `SIGUSR2`, then follows the status file until the purge completes, printing
  `... N file(s) deleted so far` every 5 seconds and finally:

  ```text
  purge requested from the daemon (pid 4242), waiting for it to finish...
  purge: 300 file(s) deleted (600B), 0 failed, 0 kept because open through the mount
  ```

  The daemon logs `gc_purge_requested`, `gc_purge_started` and
  `gc_purge_completed` (`deleted`, `would_delete`, `failed`, `skipped_open`,
  `passes`), and rewrites the status file as soon as the purge starts and ends
  (`gc.purging`, `gc.purges_completed`, `gc.last_purge`).
- When no daemon serves the mount point (the mount point is not mounted), the CLI
  process purges the backing directory itself, with the same garbage collector and
  no rate limit, and prints `purge: N file(s) deleted (SIZE), F failed`.
- When the mount point is mounted but its daemon cannot be found
  (`metrics.enabled: false`, status file unreadable or stale, another `-c`), the
  purge is refused (exit 7): an offline purge could delete files that are open
  through the mount. Send `SIGUSR2` to the daemon instead.

**Confirmation.** On a terminal, `tsfs --purge` asks `Delete now every expired file
of <backing directory> ...? [y/N]`; any answer other than `y` or `yes` cancels
(`purge cancelled`, exit 0). `--force` skips the question and is required when
standard input is not a terminal (otherwise exit 2). `--force` with any other
action, or with `--purge --dry-run`, is a usage error (exit 2). `--purge --dry-run`
and `--dry-run --purge` are the same preview, exactly the listing of `tsfs
--dry-run` ([chapter 15](#15-dry-run-mode)).

**Exit status.** 0 done (or cancelled), 1 at least one deletion failed (or the
daemon stopped during the purge), 2 usage error, 3 the daemon cannot be signaled
(`EPERM`), 5 invalid configuration, 7 `dry_run` is `true`, mount without a
reachable daemon, or backing directory not accessible.

**Signal.** The daemon purges on `SIGUSR2` whoever sends it:

```sh
sudo kill -USR2 "$(systemctl show -p MainPID --value tsfs)"
sudo systemctl kill -s USR2 tsfs@recordings
```

Only users allowed to signal the daemon (root, or its own user) can request a
purge, and a purge never deletes a file that the regular collector would keep: it
only removes the waiting time.

Details: [garbage-collector.md](../architecture/garbage-collector.md).

## 15. Dry-run mode

TSFS has two dry-run mechanisms.

### One-shot report: `tsfs --dry-run`

```sh
sudo tsfs --dry-run -c /etc/tsfs/config.yml
sudo tsfs --dry-run -c /etc/tsfs/config.yml.new          # preview a new policy
sudo tsfs --dry-run -c /etc/tsfs/config.yml -b /backup/copy   # another tree
```

It loads the configuration, scans the backing directory once (directly, without
mounting; a running daemon is not needed and not disturbed), applies the same
checks as the real garbage collector, except the files open through a running
mount (unknown here), prints the files that are expired **now**, deletes nothing
and exits:

```text
EXPIRED AT (UTC)                 SIZE  RULE                     PATH
2026-10-12T08:30:28Z            5  video-files              /v/.hidden.mp4
2026-10-12T08:30:28Z            5  video-files              /v/CLIP.MP4
dry run: 2 file(s), 10 byte(s) would be deleted now; 8 file(s) scanned in /var/lib/tsfs/data
```

Columns: expiry time (UTC), size in bytes, deciding rule (`default` for
`retention.default`), managed path (relative to the TSFS root; unusual characters
are escaped and quoted). Files that expire later are not listed. Since no mount is
involved, files open through a running mount are listed as well. Only warnings and
errors are logged (to stderr). The report is independent of
`garbage_collector.dry_run` and `garbage_collector.enabled`.

The scan uses the same bounded scheduler as the daemon. When more files expire than
`garbage_collector.max_pending_tasks` can hold, the report is incomplete and its
summary says so:

```text
dry run: INCOMPLETE: 1200 expiring file(s) did not fit in garbage_collector.max_pending_tasks (100000) and are not listed; raise it to see them all
```

Raise `max_pending_tasks` in a copy of the file and run the report again with
`-c` to see every file.

### Daemon in dry-run mode: `garbage_collector.dry_run: true`

The mounted daemon runs normally but, instead of deleting, logs each file it would
delete once (per reload) and counts it in `gc.would_delete`:

```text
... INFO ... component=gc event=gc_candidate_found path="/tmp/record/a.mp4" bytes=4 rule="temporary-recordings" dry_run=true msg="dry run: would delete"
```

The installed example configuration enables it. Watch the log for a while, then set
`dry_run: false` and reload: no restart is needed.

## 16. Logging

TSFS writes its log to `logging.directory`/`logging.filename` (default
`/var/log/tsfs/tsfs.log`, mode 0640). Each line is an event with a stable
`event=` identifier:

```text
2026-10-13T08:30:41.291+00:00 INFO pid=12 tid=17 component=gc event=gc_file_deleted path="/tmp/record/a.mp4" bytes=4 links=1 rule="temporary-recordings"
```

- **Levels:** `trace`, `debug`, `info` (default in Release builds), `warning`,
  `error`, `critical`. Set `logging.level`, or override with `-l LEVEL`. `trace`
  logs every failed client request (including ordinary `ENOENT` lookups) and is
  meant for short investigations.
- **Formats:** `text` (above) or `json` (JSON Lines: `{"ts":...,"level":"info",
  "pid":...,"tid":...,"component":"gc","event":"gc_file_deleted",...}`).
- **Console:** `logging.console: true` (or `-d`) also writes to stderr, which
  systemd sends to the journal (`journalctl -u tsfs`). Under systemd, startup
  errors and messages emitted before the log file is opened are in the journal.
- **Asynchronous writing:** a dedicated thread writes the file. When its queue
  (`logging.async.queue_size`) is full, `overflow_policy` decides: `block` (default,
  nothing lost), `overrun_oldest` or `discard_new` (losses are counted, reported on
  stderr and by `log_queue_overflow`).
- **Rotation:** TSFS rotates its own file when it would exceed `max_file_size`
  (default 50 MiB), keeping `max_files` rotated files (`tsfs.log.1` to
  `tsfs.log.10`), optionally gzip-compressed (`compress: true`, `tsfs.log.N.gz`).
  Do not use logrotate on this file. If another program deletes or replaces the
  active file, TSFS recreates or reopens it within `check_interval` and logs
  `log_file_reopened`.
- **Disabled logging:** `logging.enabled: false` writes no file; errors still go to
  stderr.
- **Rate limiting:** a repeated warning (same error on many files) is written at
  most 10 times per minute, then summarized with `suppressed=<n>`.

Useful commands:

```sh
grep 'event=gc_file_deleted' /var/log/tsfs/tsfs.log | tail
grep -E ' (WARNING|ERROR|CRITICAL) ' /var/log/tsfs/tsfs.log | tail
zgrep 'event=config_reload' /var/log/tsfs/tsfs.log.*.gz
jq -c 'select(.event=="gc_delete_failed")' /var/log/tsfs/tsfs.log   # json format
```

The complete event list is in [monitoring.md](monitoring.md#5-log-events);
the design is in [logging.md](../architecture/logging.md).

## 17. Monitoring

- **Status file** (`metrics.status_file`, default `/run/tsfs/status.json`): JSON
  document rewritten every `metrics.interval` (default 10 s), on `SIGUSR1`, when a
  purge starts or ends, and at shutdown. It contains the version, pid, uptime,
  configuration generation and reload counts, garbage collector counters (deleted
  files and bytes, failures, retries, skips, queue saturations, scans, managed
  files, purges and the result of the last purge), per-operation FUSE call and
  error counts, the kernel FUSE connection (`fuse`: requests waiting for the
  daemon), and logging counters.
- **`tsfs --status`** prints the status file named by the configuration
  (`-c`); exit status 7 when there is none.
- **`SIGUSR1`** (`sudo kill -USR1 <pid>`) rewrites the status file at once and logs
  a `status_requested` summary.
- **Log events** to alert on: `config_reload_failed`, `gc_delete_failed`,
  `gc_queue_saturated`, `gc_file_skipped` with `reason="unsafe"`,
  `filesystem_operation_failed` at warning level or above, `log_queue_overflow`,
  `log_write_failed`, `log_rotation_failed`, `diagnostics_unavailable`,
  `config_restart_required`.

The status file is not removed at shutdown: check `pid` and `updated_unix_ms` to
detect a stopped daemon. Field definitions, counter semantics, alert suggestions and
a sample health check are in [monitoring.md](monitoring.md).

### Statistics per rule: `tsfs --stats`

`tsfs --stats` scans the backing directory once (directly, without the daemon;
read access to the whole tree is needed, so root for the service paths) and prints
one row per rule that decides at least one file, in configuration order (`default`
for `retention.default`), a `TOTAL` row, and a summary of the other entries:

```text
TSFS statistics of /var/lib/tsfs/data (2 active rule(s), default retention never)
RULE                         FILES       SIZE  EXPIRED     <24H  EXCLUDED    NEVER  NEXT EXPIRY (UTC)
rec                            305       610B      300        5         0        0  2026-10-14T11:51:22Z
keep                             5        10B        0        0         5        0  -
TOTAL                          310       620B      300        5         5        0  2026-10-14T11:51:22Z
directories: 3, symbolic links: 0, other entries: 0, unreadable: 0, mount boundaries: 0
```

| Column | Meaning |
|--------|---------|
| `FILES`, `SIZE` | regular files decided by the rule, and their total size |
| `EXPIRED` | files already expired now (what `tsfs --purge` would delete, subject to its checks) |
| `<24H` | files not expired yet that expire within the next 24 hours |
| `EXCLUDED` | files protected by an `exclude` rule |
| `NEVER` | files whose retention is `never` |
| `NEXT EXPIRY (UTC)` | earliest future expiry, `-` when none |

The last line counts the directories, symbolic links and other entries (never
deleted), the entries that could not be read, and the directories that were not
entered because they are mount points (with `security.stay_on_filesystem: true`)
or were swapped for a symbolic link during the scan. The
report ignores `garbage_collector.dry_run` and `enabled`. For the count seen by the
daemon itself, `gc.managed_files` in `tsfs --status` gives the regular files of
its last complete pass.

### Retention of a file: `tsfs-ls` and extended attributes

With `filesystem.retention_xattrs: true` (the default), every regular file of the
mount has four read-only virtual extended attributes, computed from the current
policy at each read:

| Attribute | Value |
|-----------|-------|
| `user.tsfs.state` | `expires` (expires in the future), `expired` (deletion pending), `excluded` or `never` |
| `user.tsfs.expires` | expiry in ISO 8601 UTC (`2026-10-14T11:51:22Z`); only when the file expires (absent for `excluded` and `never`) |
| `user.tsfs.rule` | identifier of the deciding rule, or `default` |
| `user.tsfs.created` | birth time of the backing file in ISO 8601 UTC (`statx` `btime`), or its modification time when the backing filesystem does not report it |

Read them by name, with `getfattr` (package `attr`) or `getxattr(2)`:

```sh
getfattr -n user.tsfs.state /mnt/recordings/clip.mp4
getfattr --only-values -n user.tsfs.expires /mnt/recordings/clip.mp4
getfattr --only-values -n user.tsfs.created /mnt/recordings/clip.mp4
```

`getfattr -d` and other tools that list attributes show nothing: `listxattr`
returns an empty list on purpose, because GNU `ls` (coreutils 9 and later) prints a
`+` after the mode of every file whose attribute list is not empty, as for an ACL,
which would mislead every `ls -l` of the mount. Directories, symbolic links,
special files and the root have no retention attribute (`ENODATA`). The attributes
cannot be set or removed (`setxattr` and `removexattr` are not supported), and the
real extended attributes of the backing files are not exposed through the mount.

The `ls` wrapper (installed as the system `ls` by default, see
[The TSFS ls](#the-tsfs-ls-replacing-the-system-ls)) tags each entry with them on a
terminal (here with uutils `ls` 0.8.0 as `ls.orig`):

```text
$ ls -l rec
total 20
-rw-r--r-- 1 dev dev    6 Oct 13 12:30 clip.mp4  created 2026-10-13 12:30, filter rec, deleted 2026-10-14 12:30 (in 1d 0h)
-rw-r--r-- 1 dev dev    2 Oct 13 12:30 hold.keep  UNFILTERED
-rw-r--r-- 1 dev dev    2 Oct 11 12:30 old.mp4  created 2026-10-13 12:30, filter rec, deletion pending (expired 2026-10-12 12:30)
-rw-r--r-- 1 dev dev    2 Oct 13 12:30 sp ace.mp4  created 2026-10-13 12:30, filter rec, deleted 2026-10-14 12:30 (in 1d 0h)
drwxr-xr-x 2 dev dev 4096 Oct 13 12:30 sub  UNFILTERED
$ ls -R rec
rec:
clip.mp4  created 2026-10-13 12:30, filter rec, deleted 2026-10-14 12:30 (in 1d 0h)
hold.keep  UNFILTERED
old.mp4  created 2026-10-13 12:30, filter rec, deletion pending (expired 2026-10-12 12:30)
sp ace.mp4  created 2026-10-13 12:30, filter rec, deleted 2026-10-14 12:30 (in 1d 0h)
sub  UNFILTERED

rec/sub:
deep.mp4  created 2026-10-13 12:30, filter rec, deleted 2026-10-14 12:30 (in 1d 0h)
```

The tags, with dates in local time:

| Tag | Meaning |
|-----|---------|
| `created <date>, filter <rule>, deleted <date> (in <remaining>)` | TSFS will delete the file at that date; the remaining time is in days and hours (hours and minutes, minutes, or seconds when shorter) |
| `created <date>, filter <rule>, deletion pending (expired <date>)` | already expired: the garbage collector will delete it soon |
| `UNFILTERED` | TSFS will never delete the entry: no expiring rule matches (retention `never` or default `never`), an exclude rule matches, or it is a directory, a symbolic link or a special file |

`created` is the value of `user.tsfs.created`. The deletion date is computed from
the timestamp of the rule (the modification time by default, the birth time with
`timestamp: birth`), so `created` can differ from the date the expiry is based on:
in the example, `old.mp4` was touched to an older modification time, so it shows
`created` today but is already expired.

Reading the attributes costs one FUSE request each; enabling them also makes the
kernel send one `getxattr` request before each write. On a write-intensive mount
of small files where nobody needs the retention tags, set
`filesystem.retention_xattrs: false` (restart required; see
[configuration.md](configuration.md)).

## 18. Performance tuning

TSFS adds a FUSE round trip to each file operation; the garbage collector runs in
separate threads. Start from the defaults and change one setting at a time while
watching the status file.

| Goal | Settings |
|------|----------|
| Lower background I/O on very large trees | raise `scan_interval` (for example `5m` or `15m`); lower `max_scan_duration`. Files written through the mount are still evaluated immediately. |
| Delete a large backlog faster | raise `max_deletions_per_second` (default 100/s: one million expired files take close to 3 hours) or set it to `0`. Do not assume that more `max_workers` helps: on a local SSD with expired files clustered in directories, 1 worker was the fastest (66 k files/s vs 45-50 k with 4-8, see [benchmark report](../performance/benchmark-report.md#53-end-to-end-with-the-real-threads-start-until-every-file-is-deleted)); more workers can help on high-latency storage or files spread over many directories. Measure on the target. |
| Protect the storage from deletion bursts | lower `max_deletions_per_second`; set `max_deletions_per_cycle` |
| Many files expiring within two scan intervals | raise `max_pending_tasks` if `gc_queue_saturated` appears (memory grows with it) |
| Fewer log writes | `logging.level: info` or higher; never leave `trace` or `debug` enabled in production |
| Logging must never slow requests | keep `async.enabled: true`; `overflow_policy: discard_new` or `overrun_oldest` trades completeness for latency |
| Less status-file churn | raise `metrics.interval` |

Indicators in the status file:

- `gc.last_scan_duration_ms` should stay well below `scan_interval` in
  milliseconds; otherwise passes run back to back.
- `gc.scheduled` close to `max_pending_tasks`, or a growing `queue_saturations`:
  the scheduler is too small or deletions too slow.
- `gc.pending_work` that stays high: deletions are throttled or slow; compare
  `delete_time_us` and `worker_busy_us` with the deletion count.
- `logging.queue_overruns` growing: the logging queue is too small for the level.

Other recommendations:

- Run a Release or RelWithDebInfo build; Debug and sanitizer builds are much
  slower.
- Keep `security.stay_on_filesystem: true` and avoid mounting other filesystems
  below the backing directory.
- The `scheduler` choice (`heap` or `ordered`) does not change results; keep the
  default unless benchmarks on your workload show a difference.
- Measure your storage with `scripts/tsfs-bench-fs.sh` (raw backing directory
  versus TSFS with the garbage collector off and on) and the `bench` target.

## 19. Crash diagnostics

Crash reports are disabled by default. Enable them in the file (restart required):

```yaml
diagnostics:
  enabled: true
  text_stack_trace: true
  crash_directory: "/var/lib/tsfs/crash"
  max_crash_reports: 10
```

or for one run with `--stack-trace`. When crash reports are armed, the daemon reads
the function symbols of the executable and of every loaded shared library at
startup (the `.symtab` table, or `.dynsym` for a stripped library), so that it can
name functions at crash time without any unsafe call. On a fatal signal
(`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`, `SIGSYS`, `SIGTRAP`), the
daemon:

1. prints on stderr (in the journal under systemd)
   `tsfs: fatal signal SIGSEGV, crash report: tsfs-crash-<unix time>-<pid>.txt event=diagnostic_report_started`,
   or `tsfs: fatal signal SIGSEGV, no crash report (cannot create it) event=diagnostic_report_failed`
   when the report file cannot be created;
2. prints `tsfs: backtrace:` followed by the symbolized backtrace, starting at the
   faulting frame;
3. writes the report (mode 0600) in `crash_directory`: version, signal and code,
   pid and tid, UTC time, fault address, instruction pointer, the backtrace as raw
   addresses, the symbolized backtrace, and the memory map (`/proc/self/maps`),
   then prints `tsfs: crash report written: <name> event=diagnostic_report_completed`;
4. terminates with the original signal (so a core dump is still produced when
   enabled, and systemd restarts the service with `Restart=on-failure`).

At startup, the log shows
`diagnostics_enabled directory="..." symbols=<count> msg="crash reports armed"`
(`symbols` is the number of function symbols loaded), or
`diagnostics_unavailable` if the handler cannot be installed (for example, the
directory cannot be created). Only the newest `max_crash_reports` reports are kept
(older `tsfs-crash-*.txt` files are removed at startup).

The symbolized section names the function and the offset in it for each frame,
from the faulting frame on (the frames of the signal handler itself are only in the
raw section):

```text
backtrace (symbols, from the faulting frame; file:line: scripts/tsfs-symbolize.sh):
  #00 0x5617c2a4b1f3 tsfs::PassthroughFs::write_buf(char const*, fuse_bufvec*, long, fuse_file_info*)+0x93 (/usr/local/bin/tsfs)
  ...
  #06 0x5617c2a5e2a0 tsfs::FuseSession::loop(unsigned int)+0x40 (/usr/local/bin/tsfs)
  #07 0x7f3a1c29ca94 ?? (/usr/lib/x86_64-linux-gnu/libc.so.6+0x9ca94)
```

A frame shown as `?? (module+0xOFFSET)` is an internal function of a stripped
library (libc, for example): the offset is relative to that library, for offline
symbolization with its debug symbols. To name the internal functions of TSFS, the
binary must not be stripped; `cmake --install` installs it unstripped (a
distribution package that strips it keeps only the exported names).

File and line numbers are resolved offline by `scripts/tsfs-symbolize.sh` (in the
source tree; it is not installed), which
maps each raw address to its module with the memory map of the report and runs
`addr2line -f -C -i -p` (inlined frames included). It needs the **same build** of
the binaries: a Release build gives function names only; build with the
`relwithdebinfo` or `debug` preset for files and lines.

```sh
scripts/tsfs-symbolize.sh /var/lib/tsfs/crash/tsfs-crash-1760344228-1234.txt
# Binary moved, or debug information in a separate file: substitute the module.
scripts/tsfs-symbolize.sh -m /usr/local/bin/tsfs=build/relwithdebinfo/src/tsfs REPORT
```

Each output line is `#NN 0xADDR module+0xOFFSET  function at file:line`.
Exit statuses: 0 success, 2 usage error, 4 `addr2line` missing, 5 not a crash
report.

When reporting a crash, include the report, the output of `tsfs --version`, the
configuration (`tsfs --show-config`), and the log lines preceding the crash.

Details: [diagnostics.md](../architecture/diagnostics.md).

## 20. Core-dump analysis

Native core dumps are disabled by default. Enable them (restart required):

```yaml
diagnostics:
  enabled: true
  native_core_dump: true
```

or for one run with `--core-dump`. TSFS then raises its soft `RLIMIT_CORE` to the
hard limit and marks itself dumpable. It inspects `/proc/sys/kernel/core_pattern`
and logs where cores will go:

```text
... INFO ... component=diagnostics event=diagnostics_core_dumps rlimit_core=... core_pattern="|/usr/lib/systemd/systemd-coredump ..." handler="systemd-coredump" msg="native core dumps enabled"
... INFO ... component=diagnostics event=diagnostics_core_dumps msg="cores are collected by systemd-coredump: coredumpctl list tsfs"
```

A warning `diagnostics_core_dumps ... msg="native core dumps unavailable"` means the hard limit is 0: raise it
(`LimitCORE=infinity` in a drop-in of the unit, or `ulimit -Hc unlimited` in the
shell that starts TSFS). The provided unit does not set `LimitCORE=`: systemd's
default (soft 0, hard unlimited) lets TSFS raise the soft limit only when
`native_core_dump` is enabled.

Where the core goes, by `core_pattern`:

- **systemd-coredump** (`|.../systemd-coredump`):

  ```sh
  coredumpctl list tsfs
  coredumpctl info tsfs
  coredumpctl gdb tsfs          # then: thread apply all bt full
  coredumpctl dump tsfs -o /tmp/tsfs.core
  ```

- **apport** (Ubuntu default without systemd-coredump): cores go to `/var/crash`,
  but apport ignores binaries that do not belong to a package by default. Use
  systemd-coredump, or a file pattern, for a TSFS built from source.
- **A file pattern** (for example `/var/crash/core.%e.%p`): the kernel writes the
  file directly. A relative pattern such as `core` writes into the working directory
  of the daemon, which is `/` and normally not writable; use an absolute pattern.
  Do not point it inside the backing directory: the core would be subject to
  retention.

Analyze a core with the exact binary that produced it, built with debug
information:

```sh
gdb /usr/local/bin/tsfs /var/crash/core.tsfs.1234
(gdb) info threads
(gdb) thread apply all bt
(gdb) frame 2
(gdb) info locals
```

TSFS threads are named (`tsfs-gc`, `tsfs-gc-w0`, `tsfs-config`, `tsfs-housekeep`,
`tsfs-log`, ...), which `info threads` shows. Core files can contain file names and
file data handled by the daemon: store and share them accordingly.

When crash reports are enabled too, compare the core with the report of the same
crash (same pid in the file name): the report's symbolized backtrace shows the
crashing thread, and `scripts/tsfs-symbolize.sh` adds file and line numbers to it
without a core (see [chapter 19](#19-crash-diagnostics)).

## 21. Troubleshooting

| Symptom | Cause and solution |
|---------|--------------------|
| Applications block on the mount | the daemon is slow or hung: `fuse.waiting` in the status file (or `cat /sys/fs/fuse/connections/$(stat -c %d MOUNTPOINT)/waiting`) shows the requests waiting for it. Restart the service; if the daemon cannot be stopped, write to the `abort` file of that directory (see [monitoring](monitoring.md#fuse)) and unmount with `umount -l` |
| `Transport endpoint is not connected` (`ENOTCONN`) | the daemon died or the connection was aborted while mounted: `sudo umount -l MOUNTPOINT`, then start the service again (and look for a crash report) |
| `tsfs: '<dir>' is already a mount point (see: findmnt '<dir>')` (exit 7) | another filesystem (often a second TSFS instance with the same `filesystem.mountpoint`) is mounted there; every instance needs its own mount point |
| `tsfs: cannot access the mount point '<dir>': Permission denied (already a FUSE mount of another user?)` (exit 3) | a FUSE filesystem of another user is mounted there; `findmnt <dir>` as root |
| `tsfs: /etc/tsfs/config.yml: cannot open configuration file: No such file or directory` (exit 5) | wrong path: pass `-c`, or reinstall to recreate the file from the example |
| `tsfs: <file>:<line>:<col>: <key>: ...` (exit 5) | invalid configuration; the message names the key. Run `tsfs --check-config -c <file>` |
| `unknown key (allowed: ...)` | a typo or a key in the wrong section; the allowed keys are listed |
| `invalid duration '1.5d'` | no fractions: `1d12h`; units `ms s m h d w`, largest first |
| `the log directory must be outside the backing directory and the mount point` | move `logging.directory` (same for `crash_directory` and `status_file`) |
| `cannot create the log directory ...` or `cannot open the log file ...` (exit 7) | run as the service account, fix permissions, or change `logging.directory` (a regular user cannot write `/var/log/tsfs`) |
| `cannot open the backing directory '...': No such file or directory` (exit 7) | create the backing directory |
| `the mount point '...' does not exist or is not a directory` (exit 7) | create the mount point. If `ls <mountpoint>` reports `Transport endpoint is not connected`, a previous daemon died without unmounting: `sudo umount -l <mountpoint>` (or `fusermount3 -u -z <mountpoint>`), then start again |
| `tsfs: <file>: running as root, the configuration must be owned by root (owner uid N)` or `... must not be writable by its group or by others (mode 0664; fix with chmod go-w)` (exit 5) | a root daemon only reads a configuration owned by root and not group/world-writable: `sudo chown root: <file>` and `sudo chmod go-w <file>` (for a symbolic link, the target is checked) |
| `tsfs: <file>: the configuration is not a regular file` (exit 5) | point `-c` to a regular file (a symbolic link to one is accepted) |
| `tsfs: command line: ...` (exit 5) | an invalid `-m`/`--mountpoint` or `-b`/`--backing-dir` value (not absolute, `/`, the two paths containing each other, or containing the log directory, crash directory or status file) |
| `tsfs: /dev/fuse does not exist (load the fuse module; in a container, pass the device)` (exit 4) | `sudo modprobe fuse`; in a container, pass `--device /dev/fuse` |
| `tsfs: cannot use /dev/fuse: Permission denied (run as root or grant access to the device)` (exit 3) | run as root, or give the user read/write access to `/dev/fuse` |
| `tsfs: filesystem.allow_other needs root or 'user_allow_other' in /etc/fuse.conf` (exit 3) | add `user_allow_other` to `/etc/fuse.conf`, run as root, or set `allow_other: false` |
| `--foreground, --debug, --fuse-debug and the diagnostics options only apply when mounting, not with --check-config` (exit 2) | drop the run-only option (`-f`, `-d`, `--fuse-debug`, `--stack-trace`, `--core-dump`, ...) when using an action |
| `Invalid cross-device link` (`EXDEV`) through the mount | the entry is on another filesystem than the backing directory (mount point or bind-mounted file) and `security.stay_on_filesystem` is `true`: expected; move the data into the backing filesystem |
| `cannot mount on '...' (does it exist? is /dev/fuse accessible? is user_allow_other set for allow_other?)` (exit 7) | load the `fuse` module, check `/dev/fuse` permissions, add `user_allow_other` to `/etc/fuse.conf` when a non-root daemon uses `allow_other`, or check that the mount point is not already mounted |
| exit 4 at startup | `security.require_openat2: true` on a kernel without `openat2` (before 5.6) |
| `tsfs: no status file at ...` (exit 7) | daemon not running, `metrics.enabled: false`, or a different `-c` than the daemon's |
| Nothing is deleted | check `garbage_collector.dry_run` (the installed example sets `true`) and `enabled`; run `tsfs --dry-run`; remember that rule paths are relative to the TSFS root, `default` is `never`, and an `exclude` rule always wins |
| A file is deleted later than expected | it was written again (modification time), it was open through the mount, deletions are throttled (`max_deletions_per_second`), or it was written outside the mount and waits for the next pass |
| A file was deleted unexpectedly | `grep 'path="/the/file"' tsfs.log` shows the `rule=`; check precedence with `tsfs --dry-run`; a rule without scope (`glob`, `extension`) applies to the whole tree |
| Changes to the file are not applied | look for `config_reload_failed` in the log; restart-only settings (`filesystem`, `diagnostics`, `security`) need a restart; a command-line override (`-m`, `-b`, `-l`) always wins |
| Permission denied through the mount | `default_permissions` applies normal ownership checks; with `allow_other: false` only the daemon's user can access the mount |
| Log file deleted by logrotate | TSFS recreates it (`log_file_reopened`); remove the logrotate rule |
| A large backlog of expired files is deleted slowly | expected with the rate limits (`max_deletions_per_second`, default 100/s); `tsfs --purge` deletes every expired file now |
| `tsfs: garbage_collector.dry_run is true: a purge would delete nothing...` (exit 7) | set `dry_run: false` and reload to delete; `tsfs --purge --dry-run` previews |
| `tsfs: <mountpoint> is mounted but its daemon cannot be found through metrics.status_file; refusing an offline purge ...` (exit 7) | pass the daemon's `-c`, enable `metrics`, run as a user who can read the status file, or send `SIGUSR2` to the daemon directly |
| `tsfs: --purge asks for confirmation on a terminal; use --force in scripts` (exit 2) | standard input is not a terminal: add `--force` |
| `tsfs: cannot signal the daemon (pid N): Operation not permitted` (exit 3) | run `tsfs --purge` as root or as the daemon's user |
| `getattr` or `getxattr` errors in the status file | normal: lookups of missing files count as `getattr` errors, and the `security.capability` lookup the kernel makes before each write counts as a `getxattr` error |
| `ls` shows no retention tags | output not a terminal (use `TSFS_LS=always`), `TSFS_LS=off`, `--help`, `--version`, `--zero`, `--dired` or `-D` given, no operand in a TSFS mount, an original `ls` without `--dired` support (its output is then shown unchanged), or `/usr/bin/ls` is the system `ls` again after a coreutils upgrade (`sudo cmake --install` re-applies the wrapper) |
| every entry is tagged `RETENTION UNKNOWN (filesystem.retention_xattrs is false)` | the daemon serves no retention attribute: set `filesystem.retention_xattrs: true` and restart it |
| every entry of a TSFS listing is tagged `UNFILTERED` | no expiring rule matches these files (`retention.default: never` and no rule) |
| `ls` shows no colors in a TSFS mount | expected: the original `ls` writes to a pipe there; `TSFS_LS=off ls --color=auto` or `ls.orig` show them, without tags |
| `ls: the original ls cannot be found (/usr/bin/ls.orig missing; set TSFS_REAL_LS)` | `sudo apt install --reinstall coreutils` restores the system `ls` (the next `sudo cmake --install` renames it `ls.orig` again); until then, `TSFS_REAL_LS=/path/to/ls` |
| `dpkg --verify coreutils` reports `/usr/bin/ls` | expected while the wrapper is in place; build with `-DTSFS_REPLACE_SYSTEM_LS=OFF` to avoid it |

For deeper investigation, run in the foreground with debug logging:

```sh
sudo systemctl stop tsfs
sudo tsfs -c /etc/tsfs/config.yml -d                 # add --fuse-debug for every request
```

Exit statuses: 0 success; 1 runtime error; 2 usage error; 3 insufficient
privileges (`/dev/fuse` not accessible, `allow_other` refused); 4 missing
dependency (`/dev/fuse` missing, `openat2` required but unavailable); 5 invalid
or untrusted configuration (file or command line); 7 unexpected system state
(missing directory, mount failure, missing status file, purge refused).

## 22. Uninstallation

1. Stop and disable the service:

   ```sh
   sudo systemctl disable --now tsfs.service
   sudo systemctl disable --now 'tsfs@*.service'   # every instance of the template
   ```

2. Remove the installed files, from the same build tree that was installed:

   ```sh
   sudo cmake --build build/release --target uninstall
   sudo systemctl daemon-reload
   ```

   The target removes exactly the files listed in `build/release/install_manifest.txt`
   (written by `cmake --install`). It removes only regular files and symbolic links,
   never directories (except the TSFS documentation directory when it ends up
   empty), and reports missing files without failing.

   It also puts the system `ls` back (never with `DESTDIR`):

   - `/usr/bin/ls` is the wrapper and `/usr/bin/ls.orig` exists: `ls.orig` is
     renamed back to `/usr/bin/ls` (`Restored the system ls`), identical to the
     original (a symbolic link, such as the uutils one, keeps its target);
   - `/usr/bin/ls` is the wrapper but `ls.orig` is missing: a warning asks to run
     `sudo apt install --reinstall coreutils`;
   - `/usr/bin/ls` is already the system `ls` (a coreutils upgrade replaced the
     wrapper) and `ls.orig` exists: the stale `ls.orig` is removed.

   Check with `dpkg --verify coreutils` (no output for `/usr/bin/ls`).

3. What is **kept**: `/etc/tsfs/config.yml`, the backing directory and all managed
   data, the logs (`/var/log/tsfs`), crash reports, `/var/lib/tsfs` and the status
   file. Remove them manually if they are no longer needed. **Removing the backing
   directory deletes your data.**

   ```sh
   sudo rm -r /etc/tsfs /var/log/tsfs /run/tsfs
   # only if the data is no longer needed:
   # sudo rm -r /var/lib/tsfs
   ```

If the build tree was deleted, the manifest is gone: configure and build the same
version with the same options (`CMAKE_INSTALL_PREFIX`, `TSFS_SYSTEMD_UNIT_DIR`),
run `sudo cmake --install` again to recreate the manifest (the existing
configuration is kept), then run the `uninstall` target.

## 23. Security recommendations

- **Start in dry-run mode.** Keep `garbage_collector.dry_run: true` (as installed)
  until `tsfs --dry-run` and the log show exactly the expected files. A rule
  without a scope (`glob`, `extension`) or a non-`never` default applies to the
  whole tree.
- **Protect what must survive with exclusions.** An `exclude` rule cannot be
  overridden by any priority.
- **Protect the configuration.** `/etc/tsfs/config.yml` decides what gets deleted:
  keep it owned by root, mode 0640 (as installed), in a directory writable only by
  root. Any user who can write it, or the directory that contains it, can make TSFS
  delete files. A daemon running as root refuses a file that is not owned by root or
  that is writable by its group or by others (exit 5).
- **Keep TSFS's own files out of the managed tree.** The loader rejects a log
  directory, crash directory or status file inside the backing directory or the
  mount point; do the same for core dumps.
- **Limit access to the mount.** Keep `allow_other: false` unless other users need
  the mount; with `allow_other: true`, the usual ownership and mode checks apply
  (`default_permissions`). Choose the permissions of the backing directory so that
  users cannot bypass the mount when that matters (the unit's `/var/lib/tsfs` has
  mode 0700).
- **Keep the path-safety settings.** `security.stay_on_filesystem: true` prevents
  scans and deletions from crossing into filesystems mounted below the backing
  directory. On kernels with `openat2`, set `security.require_openat2: true` to
  refuse the fallback resolver: only `openat2` detects a bind mount of the same
  filesystem below the backing directory (same device number).
- **Logs, status file, crash reports and cores** contain file paths, and core files
  can contain file data. The status file has mode 0640, crash reports 0600, the log
  0640 in a 0750 directory; keep them that way.
- **Run the service as shipped.** The unit sets `NoNewPrivileges=yes`, a reduced
  capability bounding set (without `CAP_MKNOD` and `CAP_SETFCAP`),
  `RestrictAddressFamilies=AF_UNIX`, `UMask=0027` (which does not alter the modes of
  files created through the mount: the daemon resets its umask to 0). It does not
  use mount-namespace sandboxing, which would hide the FUSE mount.
- **Purges.** `tsfs --purge` and `SIGUSR2` need the right to signal the daemon
  (root or the daemon's user) and only remove files that are already expired under
  the policy; protect the configuration as above, since it is what defines
  "expired". Prefer the daemon purge to an offline one.
- **Retention attributes disclose part of the policy.** Any user who can read a
  file through the mount (the usual kernel permission checks apply) can read its
  expiry, its creation time and the identifier of its rule. If
  rule names or expiries are sensitive, name rules neutrally or set
  `filesystem.retention_xattrs: false`.
- **The `ls` replacement** renames a file of the `coreutils` package
  (`/usr/bin/ls` becomes `/usr/bin/ls.orig`). Build with
  `-DTSFS_REPLACE_SYSTEM_LS=OFF` on systems where package integrity checks matter
  or where a non-standard `ls` is not acceptable.
- **Monitor** `config_reload_failed`, `gc_delete_failed` and
  `gc_file_skipped reason="unsafe"`: the last one indicates that something swapped
  a directory for a symbolic link or mounted a filesystem inside the tree.

The threat model, attack surfaces and mitigations are described in
[threat-model.md](../security/threat-model.md).

## 24. Known limitations

- **Passthrough only.** `filesystem.mode` accepts only `passthrough`.
- **Unsupported operations.** `ioctl` and `poll` are not supported (`ENOSYS`);
  `copy_file_range` falls back to read/write in the kernel. File locks are handled
  locally by the kernel.
- **Extended attributes.** Only the four read-only `user.tsfs.*` retention
  attributes are served, and they are not listed by `listxattr`; the real extended
  attributes of the backing files (including ACLs and security labels) are not
  exposed, and setting or removing attributes is not supported. With
  `retention_xattrs: false`, no attribute is served at all.
- **TSFS listings of the `ls` wrapper.** Colors are not shown in TSFS listings
  (the original `ls` writes to a pipe), and formats other than the long ones
  (columns, `-x`, `-m`, ...) become one entry per line followed by its tag; when an
  option overrides the long format so that the original `ls` prints no dired
  trailer, the listing is shown without tags. Outside TSFS mounts and when the
  output is not a terminal, `ls` is unchanged. A coreutils upgrade puts the system
  `ls` back over the wrapper until TSFS is installed again.
- **Purge.** Without a reachable daemon, `tsfs --purge` refuses to purge a mounted
  mount point. A purge runs at most 1000 complete passes, each limited to
  `max_pending_tasks` expired files; a larger backlog needs a second purge.
- **Only regular files expire.** Directories, including empty ones, symbolic links
  and special files are never deleted.
- **Open-file protection covers the mount only.** `skip_open_files` knows only the
  files opened through the TSFS mount, not those opened directly in the backing
  directory.
- **Changes outside the mount are seen late.** Files written directly in the
  backing directory are evaluated at the next reconciliation pass (up to
  `scan_interval`), not immediately.
- **Deletion timing is not exact.** Deletion happens shortly after expiry, subject
  to throttling, scheduler capacity and the scan cycle; under heavy backlog it can
  lag significantly.
- **Expiry follows the wall clock.** A clock step forward expires files early; a
  step backward postpones deletions.
- **`birth` depends on the backing filesystem.** Without creation-time support,
  the modification time is used.
- **Glob case sensitivity.** `glob` and `path_glob` are case-sensitive;
  `extension` is case-insensitive for ASCII letters only.
- **Restart-only settings.** `filesystem`, `diagnostics` and `security` changes
  need a restart.
- **Counters are not persistent.** They restart from 0 with the daemon, and the
  status file stays on disk after a shutdown.
- **No external log rotation.** TSFS rotates its own log; external rotation tools
  conflict with it.
- **Uninstallation needs the install manifest** of the build tree that was used to
  install.
- **Crash-report symbols.** The crash-time backtrace names functions but not files
  and lines; internal functions of stripped binaries and libraries appear as
  `?? (module+0xOFFSET)`, and libraries loaded with `dlopen` after startup are not
  covered. Use `scripts/tsfs-symbolize.sh` with the same build (RelWithDebInfo or
  Debug) for files and lines.
- **Same-filesystem bind mounts.** Without `openat2` (kernels before 5.6), and for
  the final path component, `stay_on_filesystem` compares device numbers only and
  does not detect a bind mount of the backing filesystem itself; set
  `security.require_openat2: true` where this matters.
- **One configuration file**, no include mechanism.
