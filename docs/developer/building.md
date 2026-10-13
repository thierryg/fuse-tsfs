<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/building.md
Purpose : Prerequisites, development container, CMake presets, targets and options
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Building TSFS

This guide covers everything needed to build TSFS from a checkout: the toolchain, the
development container, the CMake presets, every build target, the build options, the
difference between Debug and Release builds, and installation and uninstallation from a
build tree.

Related documents: [testing.md](testing.md), [coverage.md](coverage.md),
[debugging.md](debugging.md), [performance.md](performance.md),
[coding-style.md](coding-style.md), [contributing.md](contributing.md), and the user-side
installation guide [docs/user/user-guide.md](../user/user-guide.md).

## 1. Prerequisites

TSFS uses C++26 language and library features (pack indexing, placeholder variables
`_`, `= delete("reason")`, saturation arithmetic, `std::views::concat`, `#embed`), so it
needs a recent toolchain. `CMake/Dependencies.cmake` checks these features with a test
program at configure time.

| Component | Minimum | Ubuntu 26.04 / Debian 13 package | Profile (`deps` target) |
|---|---|---|---|
| C++ compiler | GCC 15, or Clang 21 with libstdc++ 15 | `g++` (or `g++-16`, `clang`) | build |
| CMake | 3.28 | `cmake` | (configure) |
| Ninja | any | `ninja-build` (needed by the presets) | (presets) |
| pkg-config | any | `pkg-config` | build |
| libfuse | 3.14 | `libfuse3-dev`, `fuse3` (`fusermount3`) | build |
| yaml-cpp | 0.7 | `libyaml-cpp-dev` | build |
| spdlog / fmt | spdlog 1.12 | `libspdlog-dev`, `libfmt-dev` | build |
| zlib | any | `zlib1g-dev` | build |
| GoogleTest / GoogleMock | 1.12 | `libgtest-dev`, `libgmock-dev` | test |
| Google Benchmark | any | `libbenchmark-dev` | bench |
| Doxygen, Graphviz | any | `doxygen`, `graphviz` | doc |
| gcov, lcov, genhtml | gcov matching GCC; lcov 2.x | `gcc`, `lcov` | coverage |
| gdb, addr2line | any | `gdb`, `binutils` | diagnostics |
| clang-format, clang-tidy, cppcheck, shellcheck | any | same names | qa |

Nothing is ever downloaded by the build: a missing dependency is reported, never
fetched.

## 2. The development container (`scripts/tsfs-dev.sh`)

Older distributions (Ubuntu 22.04 and 24.04 in particular) ship no C++26 toolchain. The
supported way to build on such a host is the development image `tsfs-dev:26.04`, defined
by [`docker/Dockerfile.dev`](../../docker/Dockerfile.dev) (Ubuntu 26.04: GCC 15 and 16,
Clang 21, libfuse 3.18, lcov 2.0, Doxygen, gdb, cppcheck, shellcheck; every package comes
from the Ubuntu archive). Nothing is installed on the host.

```bash
scripts/tsfs-dev.sh [--rebuild] [--no-fuse] [--no-aslr] [--] [COMMAND [ARG...]]
scripts/tsfs-dev.sh --help
```

| Option | Effect |
|---|---|
| (none) | Run `COMMAND` in the container; without a command, start an interactive `bash` |
| `--rebuild` | Rebuild the image even if it exists (it is built automatically on first use) |
| `--no-fuse` | Do not pass `/dev/fuse` nor `CAP_SYS_ADMIN`: the FUSE tests then skip |
| `--no-aslr` | Run `COMMAND` under `setarch -R` (ASLR disabled) with an unconfined seccomp profile; required by the `tsan` preset (see [testing.md](testing.md#5-sanitizers)) |

How the container is started (see the script for the exact `docker run` line):

- the repository is bind-mounted at `/src`, which is also the working directory;
- the container runs as a `dev` account whose uid/gid match the host user, so build
  trees keep the host ownership;
- by default it gets `--device /dev/fuse --cap-add SYS_ADMIN --security-opt
  apparmor=unconfined` (FUSE mounts) and `--ulimit core=-1` (core dumps);
- `/etc/fuse.conf` in the image contains `user_allow_other`;
- the variables `TSFS_TEST_ENABLE_CORE_DUMPS`, `TSFS_TEST_ARTIFACT_DIR`,
  `TSFS_TEST_KEEP_ARTIFACTS`, `TSFS_STRESS_SCALE`, `TSFS_BENCH_LARGE` and `TSFS_BENCH_DIR`
  are forwarded from the host environment when set (paths must be valid inside the
  container, that is under `/src`), for example
  `TSFS_STRESS_SCALE=4 scripts/tsfs-dev.sh ctest --test-dir build/debug -L stress`. Any
  other variable must be passed explicitly (`scripts/tsfs-dev.sh env VAR=value ...`);
- `TSFS_DEV_IMAGE` selects another image name (default `tsfs-dev:26.04`).

Exit codes of the script: the status of `COMMAND`, or 2 (usage error) and 4 (Docker not
available).

> **Build trees and the container.** CMake records absolute paths. A tree configured in
> the container refers to `/src/...` (for example `build/release/CMakeCache.txt` contains
> `/src/build/release`), so the trees under `build/` are only usable through
> `scripts/tsfs-dev.sh`. Do not run `cmake --build build/<preset>` directly on the host:
> it fails (wrong paths, missing compiler). On a host with a C++26 toolchain, use separate
> trees.

Every command in this guide is shown without the `scripts/tsfs-dev.sh` prefix. On a host
without C++26, prefix it, for example:

```bash
scripts/tsfs-dev.sh cmake --preset release
scripts/tsfs-dev.sh cmake --build --preset release
```

## 3. Presets (`CMakePresets.json`)

All configure presets inherit a hidden `base` preset: generator **Ninja**,
`binaryDir = build/<preset>`, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`.

```bash
cmake --list-presets          # configure presets
cmake --build --list-presets  # build presets
ctest --list-presets          # test presets
```

| Preset | Tree | Cache variables | Use |
|---|---|---|---|
| `release` | `build/release` | `CMAKE_BUILD_TYPE=Release` | Production build, benchmarks, documentation |
| `relwithdebinfo` | `build/relwithdebinfo` | `CMAKE_BUILD_TYPE=RelWithDebInfo` | Optimized with symbols: profiling, separate symbol packages |
| `debug` | `build/debug` | `CMAKE_BUILD_TYPE=Debug` (sanitizers `address;undefined` by default) | Day-to-day development and debugging, with ASan + LeakSanitizer + UBSan |
| `asan` | `build/asan` | Debug, `TSFS_SANITIZE=address;undefined`, `TSFS_BUILD_BENCHMARKS=OFF` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | `build/tsan` | Debug, `TSFS_SANITIZE=thread`, `TSFS_BUILD_BENCHMARKS=OFF` | ThreadSanitizer (run with `--no-aslr`) |
| `coverage` | `build/coverage` | Debug, `TSFS_ENABLE_COVERAGE=ON`, `TSFS_BUILD_BENCHMARKS=OFF` | gcov/lcov coverage ([coverage.md](coverage.md)) |
| `clang` | `build/clang` | Debug, `CMAKE_CXX_COMPILER=clang++`, `TSFS_WARNINGS_AS_ERRORS=ON` | Second compiler (Clang 21), warnings as errors; Debug, so ASan + LeakSanitizer + UBSan by default |
| `ci` | `build/ci` | Release, `TSFS_WARNINGS_AS_ERRORS=ON` | CI gate: Release with `-Werror` |

The `asan` preset is equivalent to `debug` without the benchmarks (both use
`address;undefined`); it is kept as an explicit name. The `clang` and `ci` presets build
with `-Werror` and pass the whole test suite. Clang 21 still reports the C++26 `#embed`
of `src/cli/command_line.cpp` as a C23 extension under `-Wpedantic`: the warning is
silenced locally with a `#pragma clang diagnostic` around that declaration only.

Build presets exist for all eight configure presets. Test presets exist for `debug`,
`release`, `asan`, `tsan` (2 parallel jobs), `coverage` and `clang` (4 parallel jobs,
output on failure); there is no test preset for `relwithdebinfo` nor `ci` (use
`ctest --test-dir build/<preset>` or the `tests` target there).

Typical preset workflow:

```bash
cmake --preset debug                          # configure build/debug
cmake --build --preset debug                  # build every artifact of the tree
cmake --build --preset debug --target tests   # build and run the test suite
ctest --preset debug -L gc                    # or run a subset through CTest
```

## 4. Targets

| Target | Defined in | Purpose | Output |
|---|---|---|---|
| `deps` | `CMakeLists.txt`, `CMake/CheckDeps.cmake` | Report every dependency per profile; fail when one required by `TSFS_DEPS_PROFILES` is missing | Console report with the `apt install` line |
| `build` | `CMakeLists.txt` | Every artifact of the tree: daemon, test binaries, crash helper, benchmarks | `src/tsfs`, `tests/*`, `benchmarks/*` |
| (default `all`) | CMake | Same set as `build` (`cmake --build <tree>`) | |
| `tests` | `tests/CMakeLists.txt` | Build the daemon and every test binary, then run CTest (`-j 4`, output on failure, JUnit) | `<tree>/test-results.xml`, `<tree>/test-artifacts/` |
| `coverage` | `CMake/Coverage.cmake` (coverage trees only) | lcov capture, filter to `src/`, genhtml, summary | `<tree>/coverage/{html/,summary.json,uncovered.txt,tsfs.info}` |
| `coverage-reset` | `CMake/Coverage.cmake` (coverage trees only) | Zero the gcov counters | |
| `doc` | `CMake/Documentation.cmake` | Doxygen HTML of `src/`, `README.md` and `docs/**/*.md` | `<tree>/docs/html/index.html`, `<tree>/docs/doxygen-warnings.log` |
| `bench` | `benchmarks/CMakeLists.txt` (when Google Benchmark is found) | Run the four Google Benchmark binaries | `<tree>/bench-results/*.json` |
| `install` | CMake (rules in `CMake/Packaging.cmake`) | Standard install step (prefer `cmake --install`) | Files under `CMAKE_INSTALL_PREFIX` |
| `uninstall` | `CMake/Packaging.cmake`, `CMake/Uninstall.cmake` | Remove exactly the files listed in `<tree>/install_manifest.txt` | |

Each target can be listed from a configured tree:

```bash
cmake --build build/release --target help | grep -E "deps|build|tests|bench|doc|install|uninstall|coverage"
```

### 4.1 Preset form and explicit (specification) form

Both forms are supported. The presets use Ninja; the explicit form below uses the default
generator of the platform (Unix Makefiles) unless `-G Ninja` is added.

> **Do not mix generators in one tree.** If `build/release` was configured by
> `cmake --preset release` (Ninja), a later `cmake -S . -B build/release ...` reuses the
> cached generator and works; but if the tree was first configured with Makefiles,
> `cmake --preset release` fails with a generator mismatch. Delete the tree (or use
> another directory) when switching.

**Dependency check**

```bash
# preset form
cmake --preset release
cmake --build build/release --target deps

# explicit form
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target deps
```

The `deps` target is available even when a required dependency is missing: in that case
configuration prints a warning, only `deps` is generated, and the target prints the
packages to install. Select the checked profiles with `TSFS_DEPS_PROFILES` (section 5),
for example `-DTSFS_DEPS_PROFILES="build;test;doc;coverage;bench;diagnostics;qa"`. All
profiles are always reported; only the requested ones make the target fail (`[MISSING]`
versus `[absent]` in the report).

`CMake/CheckDeps.cmake` runs in script mode (`cmake -P`), where
`CMAKE_SYSTEM_PREFIX_PATH` is empty: its header checks (`<gtest/gtest.h>`,
`<gmock/gmock.h>`, `<benchmark/benchmark.h>`) therefore search the standard include
directories explicitly (`/usr/local/include`, `/usr/include` and the multiarch
directories).

**Release build**

```bash
cmake --preset release && cmake --build --preset release

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target build -j"$(nproc)"
```

Release builds are never instrumented by sanitizers.

**Debug build**

```bash
cmake --preset debug && cmake --build --preset debug

cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target build -j"$(nproc)"

# Debug without sanitizers (for example to run under valgrind or to measure)
cmake -S . -B build/debug-nosan -DCMAKE_BUILD_TYPE=Debug -DTSFS_SANITIZE=none
```

A Debug build is instrumented with **AddressSanitizer + LeakSanitizer +
UndefinedBehaviorSanitizer by default** (`TSFS_SANITIZE` defaults to `address;undefined`
when `CMAKE_BUILD_TYPE=Debug` and coverage is off; see section 5). Any memory error,
undefined behavior or leak stops the process with a report on stderr and a non-zero exit
status (1 for a leak), so it fails the test that triggered it. `-DTSFS_SANITIZE=none`
opts out. `TSFS_SANITIZE` is a cache variable: changing the build type of an existing
tree does not change it, set it explicitly (or use a new tree).

**Tests**

```bash
cmake --build --preset debug --target tests
cmake --build build/debug --target tests
```

See [testing.md](testing.md) for labels, single tests and sanitizers.

**Coverage**

```bash
cmake --preset coverage
cmake --build --preset coverage
cmake --build --preset coverage --target tests
cmake --build --preset coverage --target coverage

cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Debug -DTSFS_ENABLE_COVERAGE=ON
cmake --build build/coverage -j"$(nproc)"
cmake --build build/coverage --target tests
cmake --build build/coverage --target coverage
```

See [coverage.md](coverage.md).

**Documentation**

```bash
cmake --build build/release --target doc     # -> build/release/docs/html/index.html
```

The target fails only when Doxygen fails or produces no `index.html`; warnings are
counted, the first 50 are printed, and the full list is written to
`<tree>/docs/doxygen-warnings.log`. Without Graphviz the HTML is generated without
diagrams. Without Doxygen the target exists but fails with an explicit message.

**Benchmarks**

```bash
cmake --build build/release --target bench
TSFS_BENCH_LARGE=1 cmake --build build/release --target bench     # adds the 1M-file datasets
```

See [performance.md](performance.md).

**Other presets**

```bash
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
cmake --preset asan && cmake --build --preset asan --target tests
cmake --preset tsan && cmake --build --preset tsan
scripts/tsfs-dev.sh --no-aslr cmake --build build/tsan --target tests   # from the host
cmake --preset clang && cmake --build --preset clang
cmake --preset ci && cmake --build --preset ci
```

### 4.2 Local quality gate (`scripts/tsfs-qa.sh`)

`scripts/tsfs-qa.sh` runs every check a change must pass, with the toolchain of the
current environment. On a host without C++26, run it in the development container
(`--no-aslr` is needed by the `tsan` step only):

```bash
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh            # every step
scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh format tidy  # selected steps
scripts/tsfs-qa.sh -h                                         # usage
```

| Step | Check |
|---|---|
| `deps` | Dependency check, every profile |
| `format` | `clang-format --dry-run -Werror` on `src/`, `tests/`, `benchmarks/` |
| `shell` | `shellcheck` on `scripts/` |
| `ci` | Preset `ci` (Release, `-Werror`): build and tests |
| `debug` | Preset `debug` (ASan + LeakSanitizer + UBSan): build and tests |
| `clang` | Preset `clang` (Clang 21, `-Werror`): build and tests |
| `tidy` | `run-clang-tidy` on `src/` and `benchmarks/` with the `build/clang` compilation database; any warning fails |
| `tsan` | Preset `tsan`: build and tests |
| `coverage` | Preset `coverage`: build, tests, HTML report |
| `doc` | Doxygen HTML (release tree) |

Default: every step, in this order. Option `-j N` sets the build and test parallelism
(default `nproc`). Each step runs in its own process: a failing step does not stop the
following ones, and the script ends with the list of failed steps. Exit codes: 0 every
step passed, 1 a step failed, 2 usage error, 4 missing tool.

The optional GitHub Actions workflow [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
runs exactly the same thing: one matrix job per step, each building the development image
and calling `scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh <step>`. It uploads the
coverage report and the Doxygen HTML as artifacts. On a runner without `/dev/fuse`, the
FUSE tests are reported as skipped.

## 5. Build options

| Option | Default | Effect |
|---|---|---|
| `TSFS_BUILD_TESTS` | `ON` | Build the test suite; requires GoogleTest/GoogleMock (configuration reports them as missing otherwise; use `OFF` for a daemon-only build) |
| `TSFS_BUILD_BENCHMARKS` | `ON` | Build the benchmarks and the `bench` target when Google Benchmark is found (silently disabled otherwise, with a status message); `OFF` in the `asan`, `tsan` and `coverage` presets |
| `TSFS_ENABLE_COVERAGE` | `OFF` | gcov instrumentation (`--coverage -fprofile-update=atomic -O0 -fno-inline`) and the `coverage` / `coverage-reset` targets; GCC only; use a dedicated tree |
| `TSFS_WARNINGS_AS_ERRORS` | `OFF` | Add `-Werror` (`ON` in the `clang` and `ci` presets) |
| `TSFS_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy during compilation with `--config-file=<source>/.clang-tidy` (see [coding-style.md](coding-style.md#8-formatting-and-static-analysis)) |
| `TSFS_SANITIZE` | `address;undefined` for `Debug` without coverage, `none` otherwise | `address`, `undefined`, `address;undefined`, `thread` or `none`; `thread` cannot be combined with `address`, and sanitizers cannot be combined with coverage (configure-time error). Flags: `-fsanitize=<list> -fno-omit-frame-pointer -fno-sanitize-recover=all` (`CMake/Sanitizers.cmake`) |
| `TSFS_DEFAULT_CONFIG_PATH` | `/etc/tsfs/config.yml` | Configuration file used without `--config`; also where `install` creates the active configuration |
| `TSFS_DEPS_PROFILES` | `build;test` | Profiles checked by `deps`: `build`, `test`, `doc`, `coverage`, `bench`, `diagnostics`, `qa` |
| `TSFS_SYSTEMD_UNIT_DIR` | empty | Destination of `tsfs.service`; empty means systemd's `systemdsystemunitdir` from pkg-config, else `<prefix>/lib/systemd/system` |
| `TSFS_TEST_ARTIFACT_DIR` | `<tree>/test-artifacts` | Where CTest-run tests create their temporary trees (see [testing.md](testing.md)) |

Standard CMake variables also apply: `CMAKE_BUILD_TYPE` (`Debug`, `Release`,
`RelWithDebInfo`; `Release` when unset with a single-configuration generator),
`CMAKE_CXX_COMPILER`, `CMAKE_INSTALL_PREFIX` (default `/usr/local`).

Example: a Clang build with clang-tidy and warnings as errors:

```bash
cmake -S . -B build/tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DTSFS_ENABLE_CLANG_TIDY=ON -DTSFS_WARNINGS_AS_ERRORS=ON
cmake --build build/tidy
```

## 6. Debug versus Release

All flags come from the `tsfs_options` interface library (`CMake/CompilerOptions.cmake`),
linked by every TSFS target only, so they never leak into third-party code.

| Aspect | Debug | Release | RelWithDebInfo |
|---|---|---|---|
| Optimization / symbols | `-Og -g3` | `-O2 -DNDEBUG` | `-O2 -g -DNDEBUG` |
| Frame pointers | `-fno-omit-frame-pointer` | compiler default | compiler default |
| libstdc++ assertions | `_GLIBCXX_ASSERTIONS` | off | off |
| `TSFS_DEBUG_BUILD=1` | yes: every log record gets a `src=<file>:<line>` field | no | no |
| Default `logging.level` (when the file sets none) | `debug` | `info` | `info` |
| Sanitizers (default `TSFS_SANITIZE`) | ASan + LeakSanitizer + UBSan | none | none |
| `_FORTIFY_SOURCE=3` | no (needs optimization) | yes | yes |

Common to every build type: the warning set (see [coding-style.md](coding-style.md#7-warning-policy)),
`-fstack-protector-strong -fstack-clash-protection`, position-independent code, and the
link flags `-Wl,-z,relro -Wl,-z,now -Wl,--as-needed`. Release uses `-O2` rather than
CMake's default `-O3`, which is not measurably faster here and grows the binary.

Release and RelWithDebInfo builds are never instrumented. Debug builds get ASan +
LeakSanitizer + UBSan unless `TSFS_SANITIZE=none` or coverage is enabled; ThreadSanitizer
and coverage only exist in their dedicated trees (`tsan`, `coverage`).

## 7. Installation

Install rules (`CMake/Packaging.cmake`), relative to `CMAKE_INSTALL_PREFIX`:

| Path | Content |
|---|---|
| `bin/tsfs` | Daemon and command-line interface |
| `share/man/man1/tsfs.1` | Manual page |
| `share/doc/tsfs/` | `README.md`, `CHANGELOG.md`, `LICENSE`, `SECURITY.md`, `docs/user/`, `docs/security/` |
| `share/doc/tsfs/examples/config.example.yml` | Commented example configuration |
| `<TSFS_SYSTEMD_UNIT_DIR>/tsfs.service` | systemd unit, installed **disabled** |
| `<dir of TSFS_DEFAULT_CONFIG_PATH>/config.yml` | Created from the example **only if absent** (mode 0640); never overwritten |

The active configuration is created by an `install(CODE)` step, outside the install
manifest, so `uninstall` never removes it. No data, log or state directory is created:
systemd creates `/var/lib/tsfs` and `/var/log/tsfs` (`StateDirectory=`, `LogsDirectory=`).

Root is only needed for the installation step into system directories; build as a normal
user:

```bash
cmake --build build/release
sudo cmake --install build/release
```

`cmake --install` does not strip the binary: the crash handler needs the `.symtab`
symbols to name internal functions in the crash-time backtrace
([debugging.md](debugging.md#5-crash-diagnostics)). Strip only if you keep a matching
unstripped copy or `.debug` file for `scripts/tsfs-symbolize.sh -m`.

### Staged installation (`DESTDIR`)

Packaging and tests use a staging root and need no privilege:

```bash
DESTDIR="$PWD/build/stage" cmake --install build/release
find build/stage -type f
```

Inside the development container, the staging directory must be under `/src` (the
bind-mounted repository) to remain visible on the host after the container exits. The
configuration file is created under `$DESTDIR` as well, and `uninstall` honors
`$DESTDIR` in the same way.

## 8. Uninstallation

```bash
sudo cmake --build build/release --target uninstall
DESTDIR="$PWD/build/stage" cmake --build build/release --target uninstall   # staged
```

`uninstall` (`CMake/Uninstall.cmake`) removes exactly the files listed in
`<tree>/install_manifest.txt`, written by the last `cmake --install` of that tree:

- only regular files and symbolic links listed in the manifest are removed; a directory
  listed there is refused;
- the active configuration, the managed data, the logs, the crash reports and the state
  directory are never in the manifest and are kept;
- directories are not removed, except the TSFS documentation directory when it ends up
  empty;
- a missing file is reported and skipped (the target is idempotent).

Use the same privileges as for the installation. If the service was enabled, run
`systemctl disable --now tsfs.service && systemctl daemon-reload` as well.

**Limitation: the manifest lives in the build tree.** Once the tree is deleted (or the
manifest removed), `uninstall` fails with "install_manifest.txt not found". To recover,
configure a fresh tree **with the same options** (`CMAKE_INSTALL_PREFIX`,
`TSFS_DEFAULT_CONFIG_PATH`, `TSFS_SYSTEMD_UNIT_DIR`), run `cmake --install` again to
recreate the manifest (an existing configuration is kept), then run `uninstall`. See
[docs/user/user-guide.md](../user/user-guide.md) ("Uninstallation").

## 9. Troubleshooting the build

| Symptom | Cause and fix |
|---|---|
| `only the 'deps' target is available` | A required dependency is missing: run `cmake --build <tree> --target deps` and install the listed packages, or use `scripts/tsfs-dev.sh` |
| `C++26 compiler ... MISSING` | GCC < 15 or Clang < 21 / libstdc++ < 15: use the container |
| `Does not match the generator used previously` | The tree was configured with another generator: remove it or use another directory |
| Paths under `/src/...` in errors on the host | The tree was configured in the container: run the command through `scripts/tsfs-dev.sh` |
| `TSFS_SANITIZE and TSFS_ENABLE_COVERAGE must use separate build trees` | Use the dedicated presets |
| `TSFS_ENABLE_COVERAGE requires GCC` | Coverage uses gcov: do not combine with `clang++` |
| `ERROR: LeakSanitizer: detected memory leaks` or `ERROR: AddressSanitizer` when running a Debug binary | A real defect (Debug builds are sanitized by default): fix it; see [debugging.md](debugging.md#6-sanitizers-for-debugging). For an uninstrumented Debug build, configure with `-DTSFS_SANITIZE=none` |
| `bench` target unknown | Google Benchmark not found, or `TSFS_BUILD_BENCHMARKS=OFF` (as in the `asan`, `tsan` and `coverage` presets) |
