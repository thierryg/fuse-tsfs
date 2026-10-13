<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : CONTRIBUTING.md
Purpose : Entry point for contributors: points to the developer documentation
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Contributing to TSFS

The contribution guide is
**[docs/developer/contributing.md](docs/developer/contributing.md)**: workflow, commit
conventions (Conventional Commits), `VERSION` and `CHANGELOG.md` rules, and the quality
gate to run before submitting.

Quick start (the host needs only Docker; the C++26 toolchain comes from the development
image):

```bash
scripts/tsfs-dev.sh cmake --preset debug                         # Debug: ASan + LeakSanitizer + UBSan
scripts/tsfs-dev.sh cmake --build --preset debug --target tests

# Before submitting: the local quality gate (format, shellcheck, builds, tests,
# sanitizers, clang-tidy, coverage, documentation)
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh
```

Developer documentation:

| Document | Content |
|---|---|
| [docs/developer/building.md](docs/developer/building.md) | Prerequisites, container, presets, targets, quality gate, options, install and uninstall |
| [docs/developer/coding-style.md](docs/developer/coding-style.md) | C++, CMake and shell conventions, warnings, clang-format, clang-tidy |
| [docs/developer/testing.md](docs/developer/testing.md) | Running and adding tests, sanitizers |
| [docs/developer/debugging.md](docs/developer/debugging.md) | Debug builds, gdb, crash reports, `tsfs-symbolize.sh`, core dumps, sanitizers, common failures |
| [docs/developer/coverage.md](docs/developer/coverage.md) | gcov / lcov workflow and interpretation |
| [docs/developer/performance.md](docs/developer/performance.md) | Benchmarks and profiling |
| [docs/developer/repository-conventions.md](docs/developer/repository-conventions.md) | Conventions inherited from the reference project |

Security vulnerabilities: see [SECURITY.md](SECURITY.md), not a public issue.
