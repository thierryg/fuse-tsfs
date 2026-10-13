<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/contributing.md
Purpose : Contribution workflow, commit conventions, versioning and the quality gate
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Contributing to TSFS

TSFS deletes files. Every change is reviewed with that in mind: a defect in the policy
engine, the garbage collector or the path handling can destroy data that was meant to be
kept. Contributions are welcome under these rules.

## 1. Before you start

- Read [building.md](building.md) and build the `debug` tree through
  `scripts/tsfs-dev.sh` (the host usually has no C++26 toolchain).
- Read [coding-style.md](coding-style.md), [testing.md](testing.md) and the relevant
  architecture documents in [docs/architecture/](../architecture/) (start with
  [overview.md](../architecture/overview.md)).
- Changes touching deletion, path resolution, symlinks, mount boundaries or privileges
  must also be checked against [docs/security/threat-model.md](../security/threat-model.md).
- The full requirements are in [SPECIFICATION.md](SPECIFICATION.md); the conventions
  inherited from the reference project are explained in
  [repository-conventions.md](repository-conventions.md).

## 2. Workflow

1. Start from an up-to-date main branch and create a topic branch
   (`fix/gc-retry-backoff`, `feat/scheduler-ordered-batch`, ...).
2. Keep one topic per change: a bug fix, a feature, a refactoring, not all three.
3. Write the test first when fixing a bug: it must fail before the fix and pass after it
   (regression test).
4. Update **every** document affected by the change in the same change: user guide and
   configuration reference for a new option or behavior, architecture documents for a
   design change, the manual page for a command-line change, `CHANGELOG.md` always. A
   change is not finished while a document is stale.
5. Run the quality gate (section 4) and report its real results in the pull request,
   failures and skips included.
6. Open the pull request; address the review; squash fix-up commits before merging.

Changes to the deletion path (GC validation and deletion, `BackingStore`, path
resolution), to the crash handler, to the systemd unit sandboxing, or to the install and
uninstall rules require a second review.

## 3. Commits

[Conventional Commits](https://www.conventionalcommits.org/), as in the reference
project, in US English and imperative mood:

```
<type>(<optional scope>): <summary in imperative mood, no final period>

<body: what and why, wrapped at 72 columns>

<footer: references, BREAKING CHANGE: ...>
```

| Type | Use |
|---|---|
| `feat` | New user-visible behavior (option, command, configuration key) |
| `fix` | Bug fix |
| `perf` | Performance change backed by a measurement ([performance.md](performance.md)) |
| `refactor` | Code change without behavior change |
| `test` | Tests only |
| `docs` | Documentation only |
| `build` | CMake, presets, Dockerfile, packaging |
| `ci` | Continuous integration |
| `security` | Hardening or a security fix |

Scopes follow the source modules: `gc`, `policy`, `config`, `filesystem`, `fsutil`,
`logging`, `diagnostics`, `metrics`, `cli`, `app`, plus `cmake`, `scripts`, `docker`,
`systemd`, `docs`.

Example:

```
fix(gc): re-validate the file identity before unlink

A file replaced between scheduling and deletion (same path, new inode) was
deleted with the retention of the old file. The worker now compares the
identity captured at scheduling time with an fstatat() of the parent fd.
```

Never commit build trees (`build/`), core files, crash reports, test artifacts,
benchmark scratch data or local configuration files.

## 4. Quality gate

Run the local quality gate before submitting. `scripts/tsfs-qa.sh` runs every check, in
the development container (`--no-aslr` is needed by the `tsan` step):

```bash
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh             # every step
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh format tidy # selected steps
scripts/tsfs-dev.sh scripts/tsfs-qa.sh -j 8 debug            # -j: build/test parallelism
```

| Step | Check |
|---|---|
| `deps` | Every dependency profile is reported and present |
| `format` | `clang-format --dry-run -Werror` on `src/`, `tests/`, `benchmarks/` (rules in `.clang-format`) |
| `shell` | `shellcheck` on `scripts/*.sh` and `scripts/lib/*.sh` (`.shellcheckrc`) |
| `ci` | Release with `-Werror` (GCC): build and the whole test suite |
| `debug` | Debug with ASan + LeakSanitizer + UBSan: build and tests |
| `clang` | Clang 21 with `-Werror` (Debug, hence also sanitized): build and tests |
| `tidy` | clang-tidy on `src/` and `benchmarks/` (`.clang-tidy`): no warning allowed |
| `tsan` | ThreadSanitizer: build and tests |
| `coverage` | Coverage build, tests and HTML report: no unexplained drop, new code covered |
| `doc` | Doxygen succeeds, no new warning |

Each step runs in its own process; a failing step does not stop the following ones and
the script ends with the list of failed steps (exit 1). The optional CI workflow
(`.github/workflows/ci.yml`) runs one step per matrix job in the same development image,
with no logic of its own ([building.md](building.md#42-local-quality-gate-scriptstsfs-qash)).

The steps are equivalent to these manual commands, from the fastest to the slowest:

```bash
# format
scripts/tsfs-dev.sh bash -c 'clang-format --dry-run -Werror $(find src tests benchmarks -name "*.[ch]pp")'
# shell
scripts/tsfs-dev.sh shellcheck scripts/*.sh scripts/lib/*.sh
# deps
scripts/tsfs-dev.sh cmake --preset release \
    -DTSFS_DEPS_PROFILES="build;test;doc;coverage;bench;diagnostics;qa"
scripts/tsfs-dev.sh cmake --build build/release --target deps
# ci
scripts/tsfs-dev.sh cmake --preset ci
scripts/tsfs-dev.sh cmake --build --preset ci
scripts/tsfs-dev.sh cmake --build build/ci --target tests
# debug (ASan + LeakSanitizer + UBSan by default)
scripts/tsfs-dev.sh cmake --preset debug
scripts/tsfs-dev.sh cmake --build --preset debug --target tests
# clang
scripts/tsfs-dev.sh cmake --preset clang
scripts/tsfs-dev.sh cmake --build --preset clang --target tests
# tidy (see coding-style.md)
scripts/tsfs-dev.sh run-clang-tidy -p build/clang -quiet '^.*/(src|benchmarks)/.*'
# tsan (ASLR disabled, see testing.md)
scripts/tsfs-dev.sh cmake --preset tsan
scripts/tsfs-dev.sh cmake --build --preset tsan
scripts/tsfs-dev.sh --no-aslr cmake --build build/tsan --target tests
# coverage
scripts/tsfs-dev.sh cmake --preset coverage
scripts/tsfs-dev.sh cmake --build --preset coverage
scripts/tsfs-dev.sh cmake --build --preset coverage --target coverage-reset
scripts/tsfs-dev.sh cmake --build --preset coverage --target tests
scripts/tsfs-dev.sh cmake --build --preset coverage --target coverage
# doc
scripts/tsfs-dev.sh cmake --build build/release --target doc
```

Notes:

- `-DTSFS_DEPS_PROFILES=...` with `--preset release` sets the cache variable on an existing tree;
  reconfigure without `-DTSFS_DEPS_PROFILES=...` (or set it back to `build;test`) if you
  want the default report again.
- All tests must pass, including the sanitizer checks: a LeakSanitizer report (also from
  the daemon started by the FUSE tests) fails the test. Never suppress a TSFS symbol in
  `tests/sanitizers/lsan.supp`.
- Skipped tests are acceptable only for a documented environmental
  reason (no `/dev/fuse`, core dumps not enabled); say so in the pull request.
- Run the FUSE tests (default in `scripts/tsfs-dev.sh`), not only the unit tests.
- For performance-sensitive changes (GC, scheduler, policy evaluation, FUSE operations),
  also run the relevant benchmarks before and after the change in the `release` tree and
  attach both results with their environment ([performance.md](performance.md)).
- For packaging changes, run a staged installation and uninstallation
  (`DESTDIR=... cmake --install build/release`, then the `uninstall` target with the same
  `DESTDIR`), see [building.md](building.md#7-installation).

## 5. Versioning and changelog

- **Single source of the version: `VERSION`** at the repository root (Semantic Versioning
  2.0.0). CMake reads it into `PROJECT_VERSION`; the binary gets it through the generated
  `version.hpp` (`tsfs --version`), the scripts through `tsfs_version`
  (`scripts/lib/tsfs-common.sh`). File headers say `Project : tsfs (version: VERSION)`
  literally. Never write the version anywhere else.
- **`CHANGELOG.md`** follows [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/):
  every user-visible change adds an entry under `## [Unreleased]`, in the matching section
  (`Added`, `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`). Write entries for
  administrators: what changed, the option or behavior concerned, and any action they need
  to take.
- **Release**: update `VERSION`, move the `[Unreleased]` entries under
  `## [X.Y.Z] - YYYY-MM-DD`, leave an empty `[Unreleased]` section, run the full quality
  gate, then tag `vX.Y.Z`. TSFS has no bump script (the reference project's
  `make bump` is not carried over); the steps are manual.

## 6. Reporting bugs and vulnerabilities

- Bugs: include `tsfs --version`, the configuration (`tsfs --show-config`, with private
  paths redacted), the relevant log lines, and for crashes the crash report or a core
  backtrace ([debugging.md](debugging.md#5-crash-diagnostics)).
- Security vulnerabilities: do not open a public issue; follow `SECURITY.md` at the
  repository root.

## 7. License

TSFS is distributed under the BSD Zero Clause License (0BSD, see `LICENSE`). By
contributing, you agree that your contribution is distributed under the same license.
Every new file carries the standard header with the SPDX identifier.
