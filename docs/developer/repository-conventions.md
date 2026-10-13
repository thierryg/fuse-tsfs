<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/repository-conventions.md
Purpose : Phase 1 analysis of the reference repository and the conventions kept or rejected in TSFS
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Repository conventions (Phase 1: reference project analysis)

The specification ([SPECIFICATION.md](SPECIFICATION.md), section 1) requires TSFS to
follow the conventions of an existing reference repository, **Remote Shutdown**
(`remote-shutdown`, version 0.3.1 at the time of the analysis), without copying what does
not fit a C++ FUSE daemon. This document records that analysis: what the reference
project is, how it is organized, and which of its practices TSFS preserves, adapts or
deliberately does not adopt.

## 1. What the reference project is

Remote Shutdown is a LAN parental-control and machine-management system with a
master/agent architecture. It is **not** a C or C++ project. The languages actually used
are:

| Part | Language and stack |
|---|---|
| Master (`master/rsd_master/`) | **Python** 3.10+: FastAPI, uvicorn, SQLite, `cryptography` |
| Agent (`agent/`) | **Go**: static binary for Linux, Windows and macOS, `log/slog` |
| Web console (`master/rsd_master/static/`) | Framework-free JavaScript, HTML, CSS, 12 locales (JSON) |
| Tooling (`scripts/`, `docker/`, `deploy/`) | **Bash** (`rsd-*.sh`), one Python helper (`rsd-bump-version.py`), Ansible (YAML), Docker |
| Packaging (`packaging/`) | systemd units, Debian maintainer scripts (POSIX `sh`), RPM specs, WiX (MSI), launchd plist |

There is no CMake, no compiled C/C++ code and no FUSE component. The reusable material is
therefore the **repository discipline** (headers, versioning, changelog, scripts, exit
codes, documentation, systemd style), not the build system or the code structure.

## 2. Analysis of the reference repository

### 2.1 Structure

```
master/rsd_master/   Python master (config, db, pki, auth, hub, discovery, policy, web, server, cli, logs)
agent/               Go agent (one source tree, platform_<os>.go files, i18n/*.json)
packaging/           systemd units, default configs, Debian/RPM/WiX/launchd files
deploy/              Ansible IaC (deploy/ansible) and SSH MOTD (deploy/motd)
docker/              Dockerfiles, Compose files, rsd-start/status/stop/clean.sh, rsd-docker-common.sh
scripts/             rsd-build-*.sh, rsd-test-*.sh, rsd-gen-certs.sh, rsd-install-deps.sh, lib/rsd-common.sh
tests/               pytest suite (+ browser/ui_check.py); Go tests in agent/*_test.go
docs/                ARCHITECTURE.md, PROTOCOL.md
VERSION              single source of the version
README.md INSTALL.md CONTRIBUTING.md SECURITY.md CHANGELOG.md CODE_OF_CONDUCT.md
THIRD-PARTY-NOTICES.md LICENSE AGENTS.md (CLAUDE.md and GEMINI.md point to it)
.editorconfig .gitattributes .gitignore .shellcheckrc .pre-commit-config.yaml .gitleaks.toml
.github/workflows/ci.yml .github/dependabot.yml
```

### 2.2 Build and scripts

- A self-documenting **Makefile** is the entry point (`make help` lists the targets from
  their `## ` comments): `venv`, `check` (ruff, gofmt, bandit, ShellCheck, `go vet` for 3
  OSes, `go test -race`, pytest), `deps` (installs packaging tools), `build` (every
  package and `dist/SHA256SUMS`), `test-distro`, `test-deb`, `test-rpm`, `deploy-test`,
  `docker-start`, `audit`, `version`, `bump PART=...`, `clean`.
- Packages are built by scripts (`rsd-build-deb.sh`, `rsd-build-rpm.sh`,
  `rsd-build-msi.sh`, `rsd-build-pkg.sh`) and tested in Docker containers of several
  distributions.
- Every Bash script sources `scripts/lib/rsd-common.sh`: strict mode
  (`set -Eeuo pipefail`, `inherit_errexit`), `ERR` trap reporting command, file and line,
  `EXIT` trap running LIFO cleanups (`on_exit`), colored `log_info/ok/warn/error/step/debug`
  on stderr honoring `NO_COLOR` and `RSD_COLOR`, `die MSG CODE`, `require_root`,
  `require_cmd`, `require_file`, `confirm`, `is_true`, `rsd_version`. The header comment
  block doubles as `--help`; `main "$@"` ends each script. `.shellcheckrc` sets
  `source-path=SCRIPTDIR` and `external-sources=true`.
- Quality tooling: `.editorconfig`, pre-commit hooks (whitespace, YAML/JSON/TOML checks,
  private-key detection, ruff, bandit, ShellCheck, gitleaks), GitHub Actions CI,
  Dependabot.

### 2.3 Logging

- Master: standard `logging` with a JSON formatter, **one JSON object per line on
  stderr**, collected by journald (`journalctl -u rsd-master -o cat | jq ...`);
  `log_json = false` switches to a human-readable format for development. Structured
  fields are passed through `extra=`.
- Agent: `log/slog` JSON handler on stderr plus an optional size-rotated file (5 MiB, one
  backup).
- Scripts: the `log_*` helpers above, on stderr, `HH:MM:SS LEVEL script: message`.

### 2.4 Configuration

- Master: INI file `/etc/rsd-master/master.ini` (or `--config`, env `RSD_CONFIG`),
  overridden by environment variables `RSD__SECTION__KEY`, over built-in defaults
  (a dataclass). INI was chosen because it is parsed by the standard library. Static
  settings require a restart; runtime parameters live in the database.
- Agent: precedence command-line flags > environment (`RSD_*`) > Windows registry > JSON
  file > defaults.
- Packages mark the configuration as a conffile (`packaging/debian/master/conffiles`): an
  administrator's file is never overwritten by an upgrade.

### 2.5 Command line

- Master: `argparse` with sub-commands (`serve` (default), `info`, `reset-password`,
  `check-password`, `create-token`, `verify-audit`, `status [--json]`, `backup`,
  `version`), each documented with a `help=` string.
- Agent: Go `flag` sets per sub-command (`run`, `enroll ...`).

### 2.6 Signals and process lifecycle

- Both services run in the foreground under a supervisor (systemd `Type=simple`,
  Windows service, launchd); they never daemonize themselves.
- Master: uvicorn handles `SIGINT`/`SIGTERM` for a graceful stop. When the server
  certificate must be reloaded (uvicorn cannot reload a TLS context), the master stops and
  exits with code **75**, and systemd's `Restart=on-failure` restarts it.
- Agent: `signal.NotifyContext(SIGINT, SIGTERM)` cancels a context propagated to every
  goroutine.
- systemd units: `Restart=on-failure`, `RestartSec=5`, `StateDirectory=`, strict
  sandboxing (`ProtectSystem=strict`, `ProtectHome=`, `PrivateTmp=`, `PrivateDevices=`,
  `NoNewPrivileges=`, `CapabilityBoundingSet=` empty, `SystemCallFilter=@system-service`,
  resource accounting, `MemoryMax=`), and a header comment like any source file.

### 2.7 Installation

Native packages (`.deb`, `.rpm`, `.msi`, `.pkg`) with maintainer scripts that create the
service account and enable the unit; removal through the package manager (which knows
every installed file); Ansible for a hardened deployment of the master; Docker images for
local tests.

### 2.8 Error handling

- Python: exceptions inside the application; the CLI converts outcomes to exit codes;
  background tasks catch and log exceptions to keep serving (`noqa: BLE001` with a
  justification).
- Go: `error` values, exit codes as named constants.
- Scripts: strict mode, `die MSG CODE`, the ERR trap.

### 2.9 Exit codes

One table shared by the scripts, the Python CLI and the Go agent:

| Code | Meaning |
|---|---|
| 0 | Success |
| 1 | Runtime failure |
| 2 | Usage error (unknown option) |
| 3 | Insufficient privileges |
| 4 | Missing prerequisite |
| 5 | Invalid configuration or input |
| 6 | Network or remote service failure |
| 7 | Unexpected system state |
| 8 | Completed with warnings |
| 75 | Master only: restart requested (new certificate) |
| 130 | Interrupted |

### 2.10 Documentation and project memory

- Every file (code, scripts, units, YAML, Markdown) starts with a header: project banner,
  `File`, `Purpose`, `Author`, `Project : remote-shutdown (version: VERSION)`,
  `Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD`. XML/HTML comments use
  `=` separators. JSON cannot carry one.
- US English everywhere; every module, class and function documented (Google-style
  docstrings, Go doc comments, JSDoc).
- `CHANGELOG.md` in Keep a Changelog 1.1.0 + SemVer 2.0.0, with an `[Unreleased]`
  section; `make bump` moves it under a dated heading.
- `AGENTS.md` is the single project memory for AI agents; `CLAUDE.md` and `GEMINI.md`
  only point to it.
- "Documentation is always regenerated": a change is not done while a document is stale.
- Conventional Commits; a second review for security-sensitive areas.
- License: 0BSD.

## 3. Practices preserved in TSFS

| Practice | Reference | TSFS |
|---|---|---|
| File header | Banner, File, Purpose, Author, Project (version: VERSION), 0BSD SPDX | Same format with the TSFS banner, in every C++, CMake, shell, YAML, systemd and Markdown file ([coding-style.md](coding-style.md#2-file-header)) |
| Single version source | `VERSION`, read by Python, Go `-ldflags`, packaging | `VERSION`, read by `CMakeLists.txt` into `PROJECT_VERSION`, then `version.hpp` (`tsfs --version`) and `tsfs_version` in scripts |
| Changelog | Keep a Changelog 1.1.0, SemVer, `[Unreleased]` | Same ([contributing.md](contributing.md#5-versioning-and-changelog)) |
| Shared shell library | `scripts/lib/rsd-common.sh` | `scripts/lib/tsfs-common.sh`: same strict mode, traps, `on_exit`, log helpers, `NO_COLOR` / `TSFS_COLOR`, `TSFS_DEBUG`, `die`, `require_cmd`, `require_file`, `is_true`, version helper |
| Script naming and structure | `rsd-<name>.sh`, header as `--help`, `main "$@"` | `tsfs-<name>.sh`, same structure |
| Exit codes | 0-8 table shared by scripts and binaries | Same numbering for the scripts **and** the `tsfs` binary (`kExitOk` ... `kExitState` in `src/cli/command_line.hpp`), minus code 6 |
| Foreground service under systemd | `Type=simple`, `Restart=on-failure`, `RestartSec=5`, `StateDirectory=` | Same in `systemd/tsfs.service.in`; `ExecReload=` sends `SIGHUP`; `LogsDirectory=` added |
| Commented systemd unit | Header and comments | Same; the comments explain the sandboxing choices |
| Structured logging | JSON lines, fields separate from the message | spdlog-based logger with `text` or `json` (JSON Lines) format, event names and structured fields, optional console output for journald |
| Configuration never overwritten | Debian conffiles | `install` creates the configuration only when absent; `uninstall` never removes it |
| Status command | `rsd-master status [--json]` | `tsfs --status` (reads the status file written by the daemon) |
| Development in containers | Distribution tests in Docker | The whole toolchain in `tsfs-dev:26.04` (`scripts/tsfs-dev.sh`), built from distribution packages only |
| No silent downloads | Distribution packages; explicit `make deps` | The build never downloads anything; `deps` reports, never installs |
| Language and documentation | US English, everything documented, docs updated with every change | Same; Doxygen comments for C++ ([coding-style.md](coding-style.md#39-documentation-comments-doxygen)) |
| Commits | Conventional Commits, second review for sensitive areas | Same ([contributing.md](contributing.md#3-commits)) |
| License | 0BSD | 0BSD |

## 4. Practices adapted or deliberately not adopted

| Reference practice | Decision in TSFS | Reason |
|---|---|---|
| Python master and Go agent | **C++26** with libfuse 3 | The filesystem is in the I/O path of every operation: native code, the libfuse high-level API and no garbage-collected runtime ([docs/architecture/language-selection.md](../architecture/language-selection.md)) |
| Makefile as the entry point | **CMake** (`CMakeLists.txt`, `CMakePresets.json`) with the targets required by the specification: `deps`, `build`, `tests`, `coverage`, `doc`, `bench`, `install`, `uninstall` ([building.md](building.md)) | Native C++ build, dependency discovery, compile commands for tooling, presets for every build mode |
| `make deps` installs tools (Go from go.dev, packaging tools) | `deps` only **reports** and prints the `apt install` line | The specification forbids modifying the system or downloading silently |
| Native packages (`.deb`, `.rpm`, ...) and package-manager removal | `cmake --install` and a manifest-based `uninstall` target; no package recipes yet (`packaging/` is empty) | Required targets of the specification; the manifest limitation is documented ([building.md](building.md#8-uninstallation)) |
| INI configuration with `RSD__SECTION__KEY` environment overrides | **YAML** (`/etc/tsfs/config.yml`) with command-line overrides only; no environment overrides | The specification mandates YAML; retention rules are nested lists that INI cannot express; environment overrides of destructive settings would be invisible to `--show-config` reviews |
| Static configuration (restart required) | **Live reload** (`SIGHUP` and file watching) with immutable snapshots; an invalid file is rejected and the previous configuration stays active | Required by the specification; remounting would interrupt clients |
| Exit code 6 (network) | Not defined | TSFS has no network function |
| Exit code 75 (restart requested) | Not used | TSFS reloads its configuration in place |
| `require_root`, `confirm` and the Docker/MSI helpers of the shell library | Not carried over | No TSFS script needs them yet; the library only contains what is used |
| Strict systemd sandboxing (`ProtectSystem=strict`, `PrivateTmp=`, `PrivateDevices=`, empty capability set) | Reduced: only directives that do not create a private mount namespace, and a bounded capability set | A FUSE mount created in a private mount namespace is invisible to the rest of the system; the daemon needs `CAP_SYS_ADMIN` (and file-ownership capabilities) to mount and to preserve ownership |
| Unprivileged service account | The unit runs as root | Same reason: mounting and preserving the ownership of files created through the mount; the risk analysis is in [docs/security/threat-model.md](../security/threat-model.md) |
| Internationalization (12 locales) | Not adopted | A system daemon and its logs are in US English only |
| Python/Go tooling (ruff, bandit, gofmt, `go vet`, pytest, pip-audit, govulncheck) | Replaced by the C++ equivalents: compiler warnings with `-Werror` gates, clang-tidy, clang-format, cppcheck, GoogleTest, sanitizers, gcov/lcov | Different language |
| `.shellcheckrc` (`source-path=SCRIPTDIR`) | Not present; run `shellcheck -x -P SCRIPTDIR` | Not yet added to the repository |
| `.editorconfig`, `.gitattributes`, `.pre-commit-config.yaml`, `.gitleaks.toml`, GitHub Actions, Dependabot | Not present at the time of writing | Candidates for later; the quality gate is run manually ([contributing.md](contributing.md#4-quality-gate)) |
| `make bump` (`rsd-bump-version.py`) | Manual release steps | No bump script yet |
| `AGENTS.md` / `CLAUDE.md` / `GEMINI.md` project memory | Not present | The specification and the developer documentation play this role |
| `INSTALL.md`, `docs/ARCHITECTURE.md`, `docs/PROTOCOL.md` at fixed places | Documentation tree `docs/{architecture,developer,user,security,testing,performance}/` | Layout required by the specification |

## 5. Summary

TSFS keeps the reference project's **discipline**: identical file headers, a single
`VERSION`, Keep a Changelog, Conventional Commits, a shared strict-mode shell library with
the same logging helpers and exit-code table, commented foreground systemd units, US
English and documentation updated with every change, and 0BSD. It does not keep its
**technology** (Python, Go, Makefile, native packages, INI configuration) nor the parts
that conflict with a FUSE daemon (private mount namespaces, unprivileged account, restart
instead of reload).
