<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/coding-style.md
Purpose : C++, CMake and shell conventions used in the TSFS sources
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Coding style

This document describes the conventions actually used in `src/`, `tests/`,
`benchmarks/`, `CMake/` and `scripts/`. When in doubt, match the surrounding code:
comment density, naming and idioms. Most of these rules come from the reference project
(see [repository-conventions.md](repository-conventions.md)).

## 1. Language

Identifiers, file names, comments, Doxygen blocks, log messages, error messages and every
Markdown file are written in **technical US English**.

## 2. File header

Every source file starts with the project header. Copy it from a sibling file and update
`File` and `Purpose` (one line). `Project : tsfs (version: VERSION)` is literal: the
version itself lives only in the `VERSION` file.

C++ (`//`), with `#` for CMake, shell, YAML and systemd files:

```cpp
// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/scheduler.hpp
// Purpose : Bounded expiration schedulers keyed by managed path (heap and ordered strategies)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
```

Shell scripts put the header right after the shebang. Markdown files use an HTML comment
whose separator lines are made of `=` only (`--` is not allowed inside an HTML/XML
comment). JSON files (`CMakePresets.json`) cannot carry a header.

A longer explanation (design notes, usage, limitations) follows the header as a plain
comment block, before the first `#include` or command.

## 3. C++ conventions

### 3.1 Language level and layout

- C++26 (`CMAKE_CXX_STANDARD 26`, no GNU extensions). C++26 features are used where they
  make the code clearer: `= delete("reason")`, pack indexing (`Args...[0]`), placeholder
  variables (`auto [_, _]`, `for (auto _ : state)`), `std::expected`, `std::print`,
  `#embed` (the example configuration is embedded in the binary).
- Headers use `#pragma once`. Sources are grouped by module under `src/<module>/`
  (`app`, `cli`, `common`, `config`, `diagnostics`, `filesystem`, `fsutil`, `gc`,
  `logging`, `metrics`, `policy`). Includes are written relative to `src/`
  (`#include "gc/scheduler.hpp"`).
- Every module is compiled into the static library `tsfs_core`; the executable, the tests
  and the benchmarks link the same objects.

### 3.2 Naming

| Entity | Convention | Examples |
|---|---|---|
| Types (classes, structs, enums, aliases) | `PascalCase` | `GarbageCollector`, `GcCandidate`, `UniqueFd`, `Result<T>` |
| Functions, methods, variables | `snake_case` | `make_scheduler()`, `pop_due()`, `run_full_scan()` |
| Private data members | `snake_case_` (trailing underscore) | `fd_`, `snapshot_`, `scheduler_` |
| Constants (`constexpr`) | `kPascalCase` | `kMaxEventsPerIteration`, `kVersion`, `kExitUsage` |
| Enumerators (`enum class`) | `snake_case` | `ScheduleResult::rejected_full`, `SchedulerKind::heap`, `LogLevel::warning` |
| Macros (build definitions only) | `TSFS_UPPER_CASE` | `TSFS_DEBUG_BUILD`, `TSFS_DEFAULT_CONFIG_PATH` |
| Files | `snake_case.hpp` / `.cpp` | `garbage_collector.cpp`, `rotating_file_sink.hpp` |

### 3.3 Namespaces

- All production code is in `namespace tsfs`; a few modules use a nested namespace
  (`tsfs::diagnostics`, `tsfs::managed_path`). Tests use `tsfs::test`, benchmarks
  `tsfs::bench` for their helpers.
- File-local helpers go in an unnamed namespace inside `tsfs` (no `static` free
  functions).
- No `using namespace` in headers. In sources, only literal namespaces
  (`using namespace std::chrono_literals;`) and short aliases
  (`namespace fs = std::filesystem;`).
- Closing braces are commented: `}  // namespace tsfs`.

### 3.4 Error handling: `Result<T>`

[`src/common/error.hpp`](../../src/common/error.hpp) defines the error model:

```cpp
struct Error {
    int code{0};          ///< Positive errno value (EINVAL for validation errors).
    std::string message;  ///< Human-readable description, US English.
};
template <typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] std::unexpected<Error> make_error(int code, std::string message);
[[nodiscard]] std::unexpected<Error> propagate(Error error);
```

Rules:

- Every fallible function returns `Result<T>` (or `Result<void>`). Errors are values.
- The code is a positive `errno` value (`ENOENT`, `EACCES`, ...); validation errors use
  `EINVAL`. The FUSE layer turns it into a negative errno.
- Create errors with `return make_error(EINVAL, "...");` and forward them with
  `return propagate(r.error());`. Messages say what failed and on which object (path,
  option, key), for example `"cannot create the crash directory '<dir>': <strerror>"`.
- Mark value-returning functions `[[nodiscard]]` when ignoring the result is a bug.
- Command-line errors carry the process exit code (`kExitUsage`, `kExitConfig`, ...,
  declared in `src/cli/command_line.hpp`) and are printed as `tsfs: <message>` on stderr.

### 3.5 Exceptions

- **No exception may cross a libfuse callback.** libfuse is a C library: unwinding
  through it is undefined behavior. Every FUSE operation goes through the `Trampoline`
  template of `src/filesystem/passthrough_fs.cpp`, a `noexcept` function that converts
  `std::bad_alloc` to `-ENOMEM` and any other exception to `-EIO`, counts the operation
  and reports the failure.
- Exceptions are tolerated only at well-defined boundaries where third-party or standard
  code throws: YAML parsing (`config_loader.cpp` catches `YAML::Exception` and returns an
  `Error`), the logger (a log write must never fail its caller), and `main()`, which
  catches `std::exception` as a last resort (`tsfs: fatal error: ...`, exit code 1).
- Background threads (GC workers, watchers, logger) must not let an exception escape their
  thread function.
- Fatal-signal handlers (`src/diagnostics/crash_handler.cpp`) use async-signal-safe
  calls only: no allocation, no mutex, no C++ object, no logger.

### 3.6 Ownership and resources

- RAII for every resource: `UniqueFd` (`src/common/unique_fd.hpp`) for descriptors, lock
  guards for mutexes, `FuseSession` for the mount, scratch directories in tests and
  benchmarks.
- `std::unique_ptr` for exclusive ownership and factories
  (`Result<std::unique_ptr<BackingStore>> BackingStore::open(...)`,
  `std::unique_ptr<ExpirationScheduler> make_scheduler(...)`).
- `std::shared_ptr<const T>` for immutable snapshots shared across threads
  (`Config`, `PolicyEngine`, `GcSnapshot`); a reload publishes a new snapshot
  (`std::atomic<std::shared_ptr<const GcSnapshot>>`) instead of mutating the current one.
- Non-owning dependencies are passed by reference (dependency injection, for example
  `GarbageCollector::Dependencies` holds references to the store, logger, clock, lock
  table); nullable non-owning pointers are documented as such.
- No raw `new`/`delete`, no owning raw pointer, no global mutable state except where the
  operating system imposes it (signal handlers are process-wide).
- Copy and move semantics are explicit. Deleted operations state why, with C++26
  `= delete("reason")`:

  ```cpp
  UniqueFd(const UniqueFd&) = delete("a descriptor has a single owner: move it or dup() it explicitly");
  ```

### 3.7 Concurrency

- Document the thread-safety of every class in its Doxygen block ("Not thread-safe: the
  GarbageCollector serializes access with its own mutex", "every method is const and
  safe to call concurrently").
- Prefer immutable snapshots and message passing over shared mutable state.
- New concurrent code must pass the `tsan` preset ([testing.md](testing.md#5-sanitizers)).
- New code must also pass the `debug` tree, which runs ASan + LeakSanitizer + UBSan by
  default: no leak, including at daemon shutdown.

### 3.8 Includes

Order, separated by blank lines (`IncludeBlocks: Regroup` in `.clang-format`):

1. the header of the current source file (`#include "gc/garbage_collector.hpp"`);
2. C and system headers, and third-party library headers (`<fcntl.h>`, `<sys/stat.h>`,
   `<fuse.h>`, `<gtest/gtest.h>`, `<benchmark/benchmark.h>`, `<spdlog/...>`);
3. C++ standard library headers (`<algorithm>`, `<format>`);
4. TSFS headers (`"common/managed_path.hpp"`).

Each block is sorted alphabetically. Include what you use.

### 3.9 Documentation comments (Doxygen)

- Classes and non-trivial functions get a `/** @brief ... */` block describing purpose,
  invariants, thread-safety, error codes and limitations.
- Short items use `///` before the declaration; data members use trailing `///<`.
- Use `@return`, `@p name` for parameters, `@ref` for cross-references.
- Comments explain *why* (constraints, rejected alternatives), not what the next line
  does.
- `cmake --build <tree> --target doc` must not add Doxygen warnings
  (`<tree>/docs/doxygen-warnings.log`).

Example from `src/gc/scheduler.hpp`:

```cpp
/**
 * @brief Strategy interface of the GC time queue (Strategy pattern).
 *
 * Entries are keyed by managed path: scheduling a path that is already present
 * replaces its entry (new identity, due time, attempt). ...
 *
 * Not thread-safe: the GarbageCollector serializes access with its own mutex.
 */
class ExpirationScheduler {
public:
    /// Remove @p path. @return true if it was present.
    virtual bool cancel(std::string_view path) = 0;
```

### 3.10 Logging

Log through `tsfs::Logger` with a component, a stable event name and structured fields:

```cpp
log.log(LogLevel::info, "gc", "gc_file_deleted", "expired file deleted",
        {{"path", path}, {"rule", rule_id}, {"size", size}});
```

Event names are `snake_case` and stable (tests and operators match them). Paths are
untrusted input: pass them as fields, never concatenated into the message, so that the
text and JSON renderers escape them (`src/common/escape.hpp`: a crafted file name cannot
forge a log line). Never log file contents.

## 4. Tests and benchmarks

- GoogleTest/GoogleMock; one executable per area, registered with `tsfs_add_test`
  ([testing.md](testing.md#6-adding-a-test)).
- Tests use isolated temporary directories (`tsfs::test::TempDir`), an injectable clock
  for time-dependent behavior, and never sleep for long durations.
- Missing capabilities (no `/dev/fuse`, no core dumps) end in `GTEST_SKIP()` with a
  reason; they are never reported as passed.
- Benchmarks use Google Benchmark and `benchmarks/bench_support.hpp`
  ([performance.md](performance.md)).

## 5. CMake conventions

- Header block in every `.cmake` file and `CMakeLists.txt`, followed by a usage comment.
- Project-specific options and cache variables are prefixed `TSFS_`.
- Compiler flags are attached to the `tsfs_options` interface target, never to global
  `CMAKE_CXX_FLAGS` (except the per-build-type defaults in `CompilerOptions.cmake`).
- Custom targets use `VERBATIM`, a `COMMENT`, and `USES_TERMINAL` when they print a report.
- Dependencies are discovered (pkg-config, `find_package`), never downloaded.

## 6. Shell scripts

All scripts live in `scripts/`, are named `tsfs-<name>.sh`, and source the shared library
[`scripts/lib/tsfs-common.sh`](../../scripts/lib/tsfs-common.sh):

```bash
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tsfs-common.sh
. "$SCRIPT_DIR/lib/tsfs-common.sh"
```

The library provides:

- **strict mode**: `set -Eeuo pipefail`, `inherit_errexit`, an `ERR` trap that reports the
  failing command, file and line and exits with its status, an `INT`/`TERM` trap
  (exit 130), and an `EXIT` trap running the cleanups registered with `on_exit COMMAND`
  in LIFO order;
- **logging** on stderr: `log_info`, `log_ok`, `log_warn`, `log_error`, `log_step`
  (section title), `log_debug` (only with `TSFS_DEBUG=1`, which also enables `set -x`);
  format `HH:MM:SS LEVEL script: message`; colors are disabled when stderr is not a
  terminal, when `NO_COLOR` is set, or with `TSFS_COLOR=never` (`always`/`auto` also
  accepted);
- **errors**: `die MESSAGE [CODE]` (default `E_RUNTIME`);
- **checks**: `require_cmd CMD...` and `require_file PATH...` (exit `E_DEPS`);
- **utilities**: `is_true VALUE`, `tsfs_version`, `TSFS_REPO_ROOT`.

Exit codes, shared by every script and by the `tsfs` binary:

| Code | Name | Meaning |
|---|---|---|
| 0 | `E_OK` | Success |
| 1 | `E_RUNTIME` | Unexpected runtime failure |
| 2 | `E_USAGE` | Invalid arguments (an unknown option is always 2) |
| 3 | `E_PRIV` | Insufficient privileges |
| 4 | `E_DEPS` | Missing prerequisite (command, package, file, device) |
| 5 | `E_CONFIG` | Invalid configuration or input data |
| 7 | `E_STATE` | Unexpected system state |
| 8 | `E_WARN` | Completed with warnings |
| 130 | | Interrupted (SIGINT/SIGTERM) |

Code 6 (network) of the reference project is not used by TSFS and is not defined.

Script structure:

1. shebang `#!/usr/bin/env bash` and the header;
2. a comment block with **Usage**, options, environment variables and **Exit codes**;
   `-h`/`--help` prints that block (`usage()` extracts it with `sed`);
3. sourcing of the library, then global defaults in `UPPER_CASE`;
4. small documented functions (`# name ARGS: what it does`), `snake_case`, `local`
   variables;
5. `main "$@"` as the last line.

Scripts must be idempotent where it makes sense, must clean up after themselves (temporary
directories and mounts are registered with `on_exit`), and must be clean under
ShellCheck, following the sourced library:

```bash
scripts/tsfs-dev.sh shellcheck scripts/*.sh scripts/lib/*.sh
scripts/tsfs-dev.sh scripts/tsfs-qa.sh shell          # same, as a quality-gate step
```

The root [`.shellcheckrc`](../../.shellcheckrc) sets `external-sources=true` and
`source-path=SCRIPTDIR`, so ShellCheck follows the
`. "$SCRIPT_DIR/lib/tsfs-common.sh"` line without `-x -P SCRIPTDIR` (no SC1091).

## 7. Warning policy

`CMake/CompilerOptions.cmake` enables, for every TSFS target:

```
-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
-Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2
-Wimplicit-fallthrough -Wmisleading-indentation -Wunused
GCC only: -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
```

`TSFS_WARNINGS_AS_ERRORS=ON` adds `-Werror`; the `ci` (GCC, Release) and `clang` (Clang
21, Debug) presets enable it, and both currently build cleanly and pass every test. New
code must build without warnings in both. A warning that cannot be fixed is silenced as
locally as possible with a comment explaining why. Example: Clang 21 reports the C++26
`#embed` of `src/cli/command_line.cpp` as a C23 extension under `-Wpedantic`; the warning
is disabled with `#pragma clang diagnostic push/ignored/pop` around that single
declaration. A `// NOLINT(check-name)` suppression must name the check.

## 8. Formatting and static analysis

The code base is **clang-format clean and clang-tidy clean**; both are enforced by the
local quality gate `scripts/tsfs-qa.sh` (steps `format` and `tidy`, see
[contributing.md](contributing.md#4-quality-gate)).

- **clang-format**: the repository root provides [`.clang-format`](../../.clang-format)
  (clang-format 21: Google base style, `Standard: Latest`, 4-space indentation, 120
  columns, left-aligned pointers, include regrouping as described in section 3.8).
  `SpaceBeforeParens: Custom` with `AfterPlacementOperator: false` keeps C++26
  `= delete("reason")` without a space (otherwise clang-format 21 formats it like a
  placement `delete (...)`). Format changed files before submitting:

  ```bash
  scripts/tsfs-dev.sh clang-format -i <files>
  scripts/tsfs-dev.sh scripts/tsfs-qa.sh format     # src/, tests/, benchmarks/
  ```

- **clang-tidy**: `TSFS_ENABLE_CLANG_TIDY=ON` runs clang-tidy during compilation with
  `--config-file=<source>/.clang-tidy`. The root [`.clang-tidy`](../../.clang-tidy)
  (clang-tidy 21) focuses on defects rather than style: `bugprone-*`,
  `clang-analyzer-*`, `concurrency-*`, `performance-*` and a selection of `cert-*` and
  `misc-*` checks; every disabled check is listed in the file with its reason. Findings
  are warnings (`WarningsAsErrors` is empty), so they become errors only together with
  `TSFS_WARNINGS_AS_ERRORS=ON`; the `tidy` step of `scripts/tsfs-qa.sh` fails on any
  warning. [`benchmarks/.clang-tidy`](../../benchmarks/.clang-tidy) inherits the root file
  and disables `clang-analyzer-deadcode.DeadStores`, which reports the Google Benchmark
  idiom `for (auto _ : state)` on every benchmark. A suppression is placed at the line,
  names the check and gives the reason, for example
  `// NOLINT(clang-analyzer-security.ArrayBound): n <= size - 1 (readlinkat bound)` in
  `src/filesystem/passthrough_fs.cpp`; the remaining clang-analyzer false positives are
  handled that way.

  ```bash
  scripts/tsfs-dev.sh scripts/tsfs-qa.sh tidy        # run-clang-tidy with build/clang
  # or during compilation:
  scripts/tsfs-dev.sh cmake -S . -B build/tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_COMPILER=clang++ -DTSFS_ENABLE_CLANG_TIDY=ON
  scripts/tsfs-dev.sh cmake --build build/tidy
  ```
- **cppcheck** and **shellcheck** are part of the `qa` profile of the `deps` target and of
  the development image; shellcheck is the `shell` step of the quality gate.
- **`.editorconfig`**: UTF-8, LF line endings, final newline, trailing whitespace trimmed
  (except in Markdown), 4-space indentation by default, 2 spaces for JSON, YAML and shell
  scripts.
- **`.gitignore`**: build trees, core files, crash reports, test artifacts and other
  generated files are never committed.

## 9. Other files

- YAML (configuration example): 2-space indentation, comments on the same line explain
  each key and its unit.
- systemd unit (`systemd/tsfs.service.in`): header, then a comment explaining every
  non-obvious directive (for example why `ProtectSystem=` and private namespaces are not
  used: a FUSE mount created in a private mount namespace would be invisible).
- Markdown: header comment, US English, relative links between documents.
