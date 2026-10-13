<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : README.md
Purpose : Project overview, quick start and documentation map
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# TSFS - Time-aware Storage Filesystem for Linux

TSFS is a FUSE (libfuse 3) filesystem that mounts a backing directory and deletes
files once their retention period has elapsed, according to declarative rules:

```yaml
retention:
  default: never                       # nothing expires unless a rule says so
  rules:
    - {id: recordings, path: /tmp/record, recursive: true, retention: 1d}
    - {id: videos, extension: mp4, retention: 1d}
    - {id: cache, path: /var/cache/tsfs, recursive: true, retention: 30m}
    - {id: cores, glob: "*.core", retention: 2d}
    - {id: keep, action: exclude, glob: "*.keep"}   # exclusions always win
```

- **Passthrough filesystem**: every operation goes to the backing directory through
  descriptor-relative calls (`openat2` with `RESOLVE_BENEATH`, no symlink or mount
  escape); files created through the mount notify the garbage collector.
- **Retention policy engine**: exact files, directories (optionally recursive),
  basename globs, whole-path globs with `**`, extensions, exclusions, priorities;
  deterministic precedence; modification or birth time.
- **Garbage collector**: time-sliced reconciliation scans, a bounded expiration
  scheduler, worker threads, rate limiting, retries with backoff, dry run. Before
  any deletion the file is re-validated (identity, type, device, open handles,
  current policy and timestamps).
- **Operations**: hot configuration reload (inotify, `SIGHUP`), status file and
  `tsfs --status`, structured logs (text or JSON) with size-based rotation and
  compression, crash reports with a symbolized backtrace, native core dumps.

Written in C++26 (GCC >= 15 or Clang >= 21) for Linux.

## Quick start

On a host without a C++26 toolchain (e.g. Ubuntu 22.04/24.04), every command runs
in the development container (`scripts/tsfs-dev.sh`, Ubuntu 26.04 image):

```sh
scripts/tsfs-dev.sh cmake --preset release
scripts/tsfs-dev.sh cmake --build build/release -j"$(nproc)"
scripts/tsfs-dev.sh cmake --build build/release --target tests

# Validate a configuration, preview what would be deleted, then mount.
build/release/src/tsfs --print-example-config > my.yml   # edit paths and rules
scripts/tsfs-dev.sh build/release/src/tsfs --check-config -c my.yml
scripts/tsfs-dev.sh build/release/src/tsfs --dry-run -c my.yml
```

Installation (the active configuration is created from the example only when
absent; it starts in dry-run mode):

```sh
cmake --preset release && cmake --build build/release
sudo cmake --install build/release
sudoedit /etc/tsfs/config.yml
sudo systemctl daemon-reload && sudo systemctl enable --now tsfs.service
```

Several filesystems, one per mount point: one configuration file each,
`/etc/tsfs/NAME.yml`, served by the template unit `tsfs@NAME.service` (see the
step-by-step guide in [Mounting the filesystem](docs/user/user-guide.md#11-mounting-the-filesystem)).

Build types: `release` (-O2, hardening), `debug` (-Og -g3, AddressSanitizer +
LeakSanitizer + UndefinedBehaviorSanitizer by default), `relwithdebinfo`, `asan`,
`tsan`, `coverage`, `clang`, `ci` presets. Targets: `deps`, `build`, `tests`,
`coverage`, `doc`, `bench`, `install`, `uninstall`.

## Documentation

| Topic | Document |
|---|---|
| Installation, configuration, mounting, reload, GC, dry run, logging, troubleshooting | [User guide](docs/user/user-guide.md) |
| Every configuration key | [Configuration reference](docs/user/configuration.md) |
| Status file, counters, log events | [Monitoring](docs/user/monitoring.md) |
| Command line | [tsfs(1)](docs/man/tsfs.1) |
| Architecture | [Overview](docs/architecture/overview.md), [filesystem](docs/architecture/filesystem.md), [policy engine](docs/architecture/policy-engine.md), [garbage collector](docs/architecture/garbage-collector.md), [reload](docs/architecture/configuration-reload.md), [concurrency](docs/architecture/concurrency.md), [metadata](docs/architecture/metadata.md), [logging](docs/architecture/logging.md), [diagnostics](docs/architecture/diagnostics.md), [language](docs/architecture/language-selection.md) |
| Security | [Threat model](docs/security/threat-model.md), [SECURITY.md](SECURITY.md) |
| Development | [Building](docs/developer/building.md), [coding style](docs/developer/coding-style.md), [testing](docs/developer/testing.md), [debugging](docs/developer/debugging.md), [coverage](docs/developer/coverage.md), [performance](docs/developer/performance.md), [contributing](CONTRIBUTING.md) |
| Quality | [Test strategy](docs/testing/test-strategy.md), [benchmark report](docs/performance/benchmark-report.md), [implementation report](docs/developer/implementation-report.md) |

The API documentation (Doxygen, with diagrams) is generated by
`cmake --build build/release --target doc` into `build/release/docs/html/`.

## License

0BSD - see [LICENSE](LICENSE). Copyright (c) 2026 Thierry Gayet.
