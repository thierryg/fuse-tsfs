<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/language-selection.md
Purpose : Language decision (C++26), toolchain, and the C++26 features actually used
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Language selection

## 1. Decision

TSFS is written in **C++26** against **libfuse 3** (high-level API,
`FUSE_USE_VERSION=314`, `src/CMakeLists.txt`).

[SPECIFICATION.md](../developer/SPECIFICATION.md) section 3 proposed C++20/23
as the default candidate and Rust as the alternative. The project owner then
required C++26 explicitly, including features that only exist in that
standard. The build enforces it: `CMakeLists.txt` sets
`CMAKE_CXX_STANDARD 26` with `CMAKE_CXX_STANDARD_REQUIRED ON`, and
`CMake/Dependencies.cmake:27-49` compiles a probe program using pack indexing,
deleted-function reasons, placeholder variables, `std::add_sat`,
`std::views::concat`, `std::expected` and `<print>`; configuration fails with
"C++26 compiler (GCC >= 15 or Clang >= 21 with libstdc++ >= 15)" otherwise.

The arguments that made C++ (any recent standard) the right family for this
project still hold:

* libfuse3 is a C library; C++ calls it directly with no binding layer, and the
  callbacks are plain function pointers (`PassthroughFs::operations()`).
* Every Linux interface TSFS depends on (`openat2`, `statx`, `renameat2`,
  `fallocate`, `eventfd`, `inotify`, `sigaltstack`, `backtrace`) is a direct
  system call or glibc function.
* RAII (`UniqueFd`, `std::jthread`, lock guards) gives deterministic release of
  descriptors, threads and mounts, which matters for an unmount on every exit
  path.
* Mature dependencies are available as distribution packages: yaml-cpp,
  spdlog/fmt, zlib, GoogleTest, Google Benchmark (`docker/Dockerfile.dev`).

## 2. Toolchain

| Item | Value | Source |
|---|---|---|
| Compilers | GCC 15 or 16, Clang 21 (with libstdc++ >= 15) | `CMake/Dependencies.cmake`, `docker/Dockerfile.dev`, `CMakePresets.json` (`clang` preset) |
| libfuse | >= 3.14 required (`pkg_check_modules(FUSE3 fuse3>=3.14)`); 3.18 in the development image | `CMake/Dependencies.cmake:56`, `docker/Dockerfile.dev` header |
| CMake | >= 3.28 | `CMakeLists.txt:11` |
| Development image | Ubuntu 26.04 (`FROM ubuntu:26.04`), all packages from the Ubuntu archive | `docker/Dockerfile.dev` |
| Presets | `release`, `relwithdebinfo`, `debug`, `asan`, `tsan`, `coverage`, `clang`, `ci` | `CMakePresets.json` |

The reference host runs Ubuntu 22.04, whose compilers cannot build C++26. Builds
and tests therefore run inside the development container through
`scripts/tsfs-dev.sh`, which builds the `tsfs-dev:26.04` image on first use,
bind-mounts the repository at `/src`, runs as the host uid/gid (build trees keep
the host ownership), and passes `/dev/fuse` and `CAP_SYS_ADMIN` so that the FUSE
integration tests can mount (`--no-fuse` disables this; `--no-aslr` runs the
command under `setarch -R`, needed by the `tsan` preset on kernels with large
`vm.mmap_rnd_bits`). Nothing is installed on the host.

```
scripts/tsfs-dev.sh cmake --preset debug
scripts/tsfs-dev.sh cmake --build --preset debug
scripts/tsfs-dev.sh --no-aslr ctest --test-dir build/tsan
```

The debug configuration defines `_GLIBCXX_ASSERTIONS` and `TSFS_DEBUG_BUILD=1`
(`CMake/CompilerOptions.cmake:36`); the latter adds a `src=file:line` field to
every log event. The default log level is `debug` in Debug builds and `info`
otherwise (`TSFS_DEFAULT_LOG_LEVEL`, `src/CMakeLists.txt:45`).

## 3. C++26 features actually used

Only features found in `src/` are listed.

| Feature (paper) | Where | Purpose |
|---|---|---|
| Pack indexing `Args...[0]`, `args...[0]` (P2662) | `Trampoline<Method, Op>::call`, `src/filesystem/passthrough_fs.cpp` | The generic FUSE trampoline extracts the path argument (when the first parameter is a `const char*`) to report a failed operation, without one hand-written wrapper per callback. |
| `= delete("reason")` (P2573) | `src/common/unique_fd.hpp`, `src/fsutil/backing_store.hpp`, `src/fsutil/path_lock_table.hpp`, `src/filesystem/fuse_session.hpp`, `src/gc/garbage_collector.hpp`, `src/config/config_watcher.hpp`, `src/logging/logger.hpp`, `src/logging/log_compressor.hpp` | Copy operations of owning types are deleted with a diagnostic that tells the caller what to do instead ("move it or dup() it explicitly"). |
| `std::add_sat` (P0543, `<numeric>`) | `saturating_add`, `src/policy/policy_engine.cpp` | Expiry = timestamp + retention clamps to the maximum time point instead of wrapping into the past (which would delete the file). |
| `std::views::concat` (P2542) | `PolicyEngine::match`, `src/policy/policy_engine.cpp` | It iterates exclusions then expire rules as one range. |
| `#embed` (P1967) | `kExampleConfig`, `src/cli/command_line.cpp`; `--embed-dir` set in `src/CMakeLists.txt:46-49` | `config/config.example.yml` is compiled into the binary, so `tsfs --print-example-config` always matches the parser of the same build. Clang 21 still flags `#embed` as a C23 extension under `-Wpedantic`; the warning is silenced for that declaration only. |
| `_` as a binding name (P2169 placeholder) | `PolicyEngine::compile`, `LogRateLimiter::check` / `pending_suppressed`, `CrashHandler::prune_reports` | Unused structured-binding members. The code never declares two `_` in the same scope, so these uses would also compile as ordinary identifiers in C++23; they document intent rather than depend on C++26 semantics. |

Earlier-standard features used pervasively, for completeness: `std::expected`
(C++23, the `Result<T>` alias in `src/common/error.hpp`), `std::print` /
`std::println` (C++23, `src/main.cpp`, `src/app/application.cpp`),
`std::atomic<std::shared_ptr<T>>` (C++20, configuration/GC/logger snapshots),
`std::jthread` and `std::stop_token` with `std::condition_variable_any`
(C++20, every helper thread), `std::format` and chrono formatting (C++20),
`std::source_location` (C++20, debug-build log locations), `std::span`
(C++20), ranges algorithms (C++20).

## 4. Alternatives considered

| Language | Assessment |
|---|---|
| C | Same libfuse access, but no RAII, no `std::expected`, manual ownership of every descriptor and allocation on every error path; the policy engine, scheduler and configuration validation would need hand-written containers. Rejected for maintainability. |
| Rust | Memory and thread safety by construction and good FUSE crates exist (`fuser`, which reimplements the protocol rather than wrapping libfuse 3). Rejected because the project owner required C++26, and because libfuse 3 (including its multi-threaded loop and `fuse_invalidate_path`) is used directly from C++ without a binding layer. |
| Go | Garbage-collected runtime with stop-the-world pauses and goroutine scheduling in the FUSE request path; FUSE libraries reimplement the protocol. Rejected for latency predictability and because signal handling (`sigaltstack`, async-signal-safe crash reports) is constrained by the runtime. |

These assessments are qualitative; no Rust or Go prototype was built ("do not
implement the same project twice", spec section 3).
