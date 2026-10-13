<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/developer/debugging.md
Purpose : Debugging the daemon: debug builds, logging, gdb, crash reports, core dumps, sanitizers
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->
# Debugging

Commands are shown as they run **inside** the development container (open a shell with
`scripts/tsfs-dev.sh`, which grants `/dev/fuse` by default). Debugging sessions are best
done in one interactive shell: the container is removed when it exits (`--rm`), together
with anything written outside `/src`.

See also: [building.md](building.md), [testing.md](testing.md),
[docs/architecture/](../architecture/) for the component design, and
[docs/user/user-guide.md](../user/user-guide.md) for the operator view (configuration,
logs, `--status`).

## 1. Debug build

```bash
cmake --preset debug && cmake --build --preset debug     # -> build/debug/src/tsfs
```

The Debug configuration (`CMake/CompilerOptions.cmake`) gives `-Og -g3`,
`-fno-omit-frame-pointer` (usable stack traces), `_GLIBCXX_ASSERTIONS` (bounds-checked
standard containers), a default `logging.level` of `debug` when the configuration file
sets none, and a `src=<file>:<line>` field on every log record (`TSFS_DEBUG_BUILD`).
Debug builds are also instrumented by default with **AddressSanitizer + LeakSanitizer +
UndefinedBehaviorSanitizer** (section 6); configure with `-DTSFS_SANITIZE=none` for an
uninstrumented Debug build (valgrind, timing). For profiling or for debugging an
optimization-dependent issue, use `relwithdebinfo` (`-O2 -g`).

## 2. A disposable test setup

Never debug against real data. A private work directory with its own configuration:

```bash
W=$(mktemp -d /src/build/dbg.XXXXXX); mkdir -p "$W/data" "$W/mnt" "$W/log"
cat > "$W/config.yml" <<EOF
version: 1
filesystem: {mountpoint: "$W/mnt", backing_directory: "$W/data"}
retention:
  default: never
  rules:
    - {id: tmp, path: /tmp, recursive: true, retention: 30s}
garbage_collector: {enabled: true, dry_run: false, scan_interval: 5s}
logging: {directory: "$W/log", level: debug}
diagnostics: {enabled: true, text_stack_trace: true, native_core_dump: true, crash_directory: "$W/crash"}
metrics: {status_file: "$W/status.json", interval: 1s}
EOF
build/debug/src/tsfs -c "$W/config.yml" --check-config      # exit 0 = valid, 5 = invalid
build/debug/src/tsfs -c "$W/config.yml" --show-config       # effective values, defaults included
build/debug/src/tsfs -c "$W/config.yml" --dry-run           # what the GC would delete now
```

`tsfs --print-example-config` prints the fully commented example
(`config/config.example.yml`).

## 3. Running the daemon in the foreground

| Option | Effect |
|---|---|
| `-f`, `--foreground` | Do not detach from the terminal |
| `-d`, `--debug` | Foreground, log level `debug`, and log to stderr as well as to the log file (run-only: rejected with `--check-config`, `--dry-run` and the other actions, exit 2) |
| `-l`, `--log-level LEVEL` | Override `logging.level`: `trace`, `debug`, `info`, `warning`, `error`, `critical` |
| `--fuse-debug` | Pass `-d` to libfuse: every FUSE request and reply is printed on stderr (implies `--foreground`) |
| `--stack-trace` / `--no-stack-trace` | Enable diagnostics with text crash reports / disable them |
| `--core-dump` / `--no-core-dump` | Enable diagnostics with native core dumps / leave the core-dump limits alone |

```bash
build/debug/src/tsfs -c "$W/config.yml" -d                       # most useful default
build/debug/src/tsfs -c "$W/config.yml" -d -l trace              # everything TSFS logs
build/debug/src/tsfs -c "$W/config.yml" -f --fuse-debug          # libfuse request trace
```

Command-line values override the file for the whole life of the process, including after
a reload. In another shell of the same container (`docker exec -it <container> bash`, or
run the daemon in the background with `&`):

```bash
tail -f "$W/log/tsfs.log"
build/debug/src/tsfs -c "$W/config.yml" --status        # reads metrics.status_file
kill -HUP <pid>                                          # reload the configuration
fusermount3 -u "$W/mnt"                                  # or SIGTERM: clean unmount and exit
```

`logging.format: json` produces JSON Lines (filter with `jq`), and `logging.async.enabled:
false` makes every record synchronous, which helps when correlating with a debugger.

## 4. gdb

gdb is installed in the image (`deps` profile `diagnostics`).

### 4.1 Starting the daemon under gdb

The simplest and most reliable way, because the container has no `CAP_SYS_PTRACE` and the
host's Yama `ptrace_scope` (1 on Ubuntu) forbids attaching to a process that is not a
descendant of the debugger:

```bash
gdb -q --args build/debug/src/tsfs -c "$W/config.yml" -f
(gdb) handle SIGHUP nostop noprint pass     # let reloads through
(gdb) break tsfs::GarbageCollector::apply
(gdb) run
(gdb) thread apply all bt                   # after Ctrl-C or a stop
```

Always use `-f` (or `-d`) under gdb: without it the daemon forks and detaches. The FUSE
mount stays active while the process is stopped at a breakpoint: any access to the mount
point from another shell blocks until you continue.

### 4.2 Attaching to a running daemon

`gdb -p <pid>` works when gdb is the parent (not practical) or when ptrace is allowed: as
root on the host for a host process, or in a container started manually with
`--cap-add SYS_PTRACE`. Otherwise it fails with "Operation not permitted".

### 4.3 Debugging a test

```bash
gdb -q --args build/debug/tests/tsfs_gc_tests --gtest_filter='GcTest.DeletionRateIsBounded' \
    --gtest_break_on_failure
```

`--gtest_break_on_failure` turns the first failed assertion into a trap, so gdb stops at
the failing line.

## 5. Crash diagnostics

The diagnostics subsystem (`src/diagnostics/crash_handler.*`, configuration section
`diagnostics:`) has two independent modes; the user-facing description is in
[docs/user/user-guide.md](../user/user-guide.md).

### 5.1 Mode A: text crash report

With `diagnostics.enabled: true` and `text_stack_trace: true` (or `--stack-trace`), a
fatal signal (`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE`, `SIGABRT`, `SIGSYS`, `SIGTRAP`)
writes `tsfs-crash-<unix-time>-<pid>.txt` (mode 0600) in `diagnostics.crash_directory`
(default `/var/lib/tsfs/crash`). The report contains: version, signal and code, pid and
tid, time, fault address, instruction pointer, the raw frame addresses, a **symbolized
backtrace** and a copy of `/proc/self/maps`. At most `max_crash_reports` (default 10) are
kept; core files are never pruned.

On stderr (the journal under systemd), the handler prints:

```text
tsfs: fatal signal SIGSEGV, crash report: tsfs-crash-1767225600-1234.txt event=diagnostic_report_started
tsfs: backtrace:
  #00 0x... <faulting function>+0x... (/usr/bin/tsfs)
  ...
  #06 0x... tsfs::FuseSession::loop(unsigned int)+0x40 (/usr/bin/tsfs)
  ...
tsfs: crash report written: tsfs-crash-1767225600-1234.txt event=diagnostic_report_completed
```

or `tsfs: fatal signal <NAME>, no crash report (cannot create it)
event=diagnostic_report_failed` followed by the backtrace when the file cannot be
created.

**How the names are resolved.** `CrashHandler::install` calls
`SymbolTable::load_current_process()` (`src/diagnostics/symbol_table.{hpp,cpp}`): for
every ELF object loaded at that time (`dl_iterate_phdr`: the executable and its shared
libraries), it reads the function symbols of `.symtab` (which includes internal-linkage
functions) or, for a stripped file, of `.dynsym`, demangles and sorts them, and records
the `PT_LOAD` segments. In the signal handler, `lookup()` and `locate()` are binary
searches in that memory (no allocation, no lock, no system call): async-signal-safe. The
startup event `diagnostics_enabled` carries `symbols=<count>`
(`CrashHandler::symbol_count()`); a count of 0 means that no symbol could be loaded
(the handler then falls back to `backtrace_symbols_fd()`).

The report section is headed `backtrace (symbols, from the faulting frame; file:line:
scripts/tsfs-symbolize.sh):` and starts at the faulting frame (the handler frames are
skipped). Each line reads `  #NN 0xADDR function+0xOFF (module)`. A frame inside an
internal function of a stripped library (typically libc) has no symbol and is printed as
`?? (module+0xOFFSET)`, the offset being what `addr2line` expects for that module.
Objects loaded later with `dlopen` are not covered.

Function names of internal TSFS functions require an **unstripped** binary: the build
and `cmake --install` do not strip; if you strip a package, keep the debug symbols for
offline symbolization.

The handler only uses async-signal-safe calls and runs on an alternate signal stack, so
stack overflows of the main threads are reported. Afterwards the default action is
restored and the signal re-raised: the process dies as it would have, with a core dump if
Mode B allows it. Limitations: `SIGKILL` and OOM kills cannot be caught; heap or stack
corruption can prevent `backtrace()` from working (the header lines are written first);
threads created by libfuse have no alternate stack, so a stack overflow there produces no
report (the core dump still does).

### 5.2 File and line numbers (`scripts/tsfs-symbolize.sh`)

The crash-time backtrace gives function names only. For files, lines and inlined frames,
run `scripts/tsfs-symbolize.sh` on the report with the **same build** (same tree, same
commit, not stripped; `relwithdebinfo` or `debug` for line numbers, a Release build gives
function names only):

```bash
scripts/tsfs-symbolize.sh /var/lib/tsfs/crash/tsfs-crash-1767225600-1234.txt
scripts/tsfs-symbolize.sh -m /usr/bin/tsfs=build/relwithdebinfo/src/tsfs REPORT
```

The script reads the raw addresses and the copied memory map of the report, maps every
address to its module and file offset (a return address is looked up at `address - 1`,
the call instruction), then runs `addr2line -f -C -i -p` against the module. It prints
`#NN 0xADDR module+0xOFF  function at file:line` per raw frame (the raw list includes the
signal-handler frames before the faulting one). `-m MODULE=FILE` (repeatable) replaces
the module whose path appears in the report by another file, for example an unstripped
copy or a `.debug` file of the same build. It needs `addr2line` (binutils). Exit codes: 0
success, 2 usage error, 4 missing dependency, 5 not a crash report.

The manual equivalent, for one raw address: the start address of the first mapping of the
`tsfs` binary (file offset `00000000`) in the copied memory map is the load base;
subtract it from the raw address:

```bash
grep -m1 '/tsfs$' tsfs-crash-1767225600-1234.txt        # e.g. 5581a6e00000-... r--p 00000000 ... /usr/local/bin/tsfs
printf '0x%x\n' $((0x5581a6e4a2f1 - 0x5581a6e00000))     # -> 0x4a2f1
addr2line -e build/debug/src/tsfs -f -C -i -p 0x4a2f1
```

Frames in shared libraries (libfuse, libstdc++, libc) are symbolized the same way
against the library file listed in the map, provided its debug symbols are installed. A
Release binary gives function names only; build `relwithdebinfo` with the same options to
symbolize a Release crash with line numbers.

### 5.3 Mode B: native core dumps

With `native_core_dump: true` (or `--core-dump`), the daemon raises its soft
`RLIMIT_CORE` up to the hard limit at startup and logs what the system will do with a core
(`event=diagnostics_core_dumps`: `core_pattern`, handler, notes). The kernel then writes the core according to
`/proc/sys/kernel/core_pattern`, a **host-global** setting:

| `core_pattern` | Where the core goes |
|---|---|
| `core` or `core.%p` (relative) | The working directory of the process (`/` for a detached daemon) |
| `/some/dir/core.%e.%p` (absolute) | That path, resolved in the process's mount namespace: inside the container for a process run there |
| `|/usr/lib/systemd/systemd-coredump ...` | systemd-coredump on the host: `coredumpctl list tsfs`, `coredumpctl gdb <pid>` |
| `|/usr/share/apport/apport ...` | apport on the host (`/var/crash`, usually only for packaged binaries) |

`scripts/tsfs-dev.sh` starts containers with `--ulimit core=-1`. Under systemd, the unit
sets no `LimitCORE=` (soft 0, hard unlimited by default), so the daemon raises the limit
itself only when Mode B is enabled. Changing `core_pattern` requires root on the host
(`sudo sysctl kernel.core_pattern=...`) and affects every process; TSFS never changes it.

Analyze a core inside the same container session (cores written under the container's
`/tmp` disappear with it; copy them under `/src` first if needed):

```bash
cat /proc/sys/kernel/core_pattern
gdb -q build/debug/src/tsfs /tmp/core.tsfs.1234
(gdb) bt full
(gdb) thread apply all bt
(gdb) info sharedlibrary
```

The deliberate-crash tests (`tsfs_diagnostics_tests`, helper `tsfs_crash_helper`) show
both modes end to end; see [testing.md](testing.md#4-environment-variables) for
`TSFS_TEST_ENABLE_CORE_DUMPS`.

## 6. Sanitizers for debugging

| Problem | Tree | Command |
|---|---|---|
| Memory errors, leaks, undefined behavior | `debug` (default), `asan`, `clang` | `cmake --preset debug && cmake --build --preset debug`, then run `build/debug/src/tsfs -d ...` or the tests |
| Data races, lock-order inversions | `tsan` | `scripts/tsfs-dev.sh --no-aslr ...` around every execution (see [testing.md](testing.md#5-sanitizers)) |

Every Debug tree is built with ASan + LeakSanitizer + UBSan unless configured with
`-DTSFS_SANITIZE=none`. Sanitizer reports go to stderr; run the daemon with `-f` or `-d`
to see them. Builds use `-fno-sanitize-recover=all`, so the first error terminates the
process. A **leak** is reported by LeakSanitizer when the process exits, and the exit
status becomes non-zero (1 with ASan): a daemon that leaked on a code path exits 1 after a
clean unmount, which the FUSE tests detect (their teardown requires exit status 0 and no
sanitizer report, see [testing.md](testing.md#3-fuse-tests)). Under CTest the runtime
options come from `tests/CMakeLists.txt` (`ASAN_OPTIONS=detect_leaks=1:...`,
`LSAN_OPTIONS` with the suppression file `tests/sanitizers/lsan.supp`, empty, which must
never suppress a TSFS symbol, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`); set
them yourself when running a binary by hand. Under gdb, stop on the report with
`break __sanitizer::Die` (or set `ASAN_OPTIONS=abort_on_error=1`). Sanitized binaries are
much slower and use more memory: never benchmark them.

## 7. Common failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `tsfs: /dev/fuse does not exist (...)`, exit 4 | No FUSE device (container started with `--no-fuse`, or kernel module not loaded) | Use `scripts/tsfs-dev.sh` without `--no-fuse`; on the host `modprobe fuse` |
| `tsfs: cannot use /dev/fuse: Permission denied (...)`, exit 3 | The device exists but the user cannot open it | Run as root or grant access to the device |
| `tsfs: filesystem.allow_other needs root or 'user_allow_other' in /etc/fuse.conf`, exit 3 | `filesystem.allow_other: true` for a non-root user without `user_allow_other` | Add `user_allow_other` to `/etc/fuse.conf` (already done in the image), or set `allow_other: false` |
| `Transport endpoint is not connected` on the mount point; mount fails with "mount point ... is busy" or "does not exist" | Stale mount: the daemon died (crash, `SIGKILL`, killed test) without unmounting | `fusermount3 -u <mountpoint>` (add `-z` for a lazy unmount if it is still busy); check with `mount \| grep fuse` |
| `tsfs: the mount point '...' does not exist or is not a directory` | Wrong `filesystem.mountpoint`, or stale mount (see above) | Create the directory or unmount the stale mount |
| Operations hang on the mount | The daemon is stopped in gdb, or deadlocked | Continue in gdb; otherwise take `thread apply all bt`, then `fusermount3 -u` |
| Exit code 5 at startup | Invalid configuration, invalid `-m`/`-b` value (`command line: ...`), or, as root, a configuration file not owned by root or writable by group/others (`fix with chmod go-w`) | `tsfs -c <file> --check-config` prints the key and the reason |
| Exit code 2 | Usage error (unknown option, incompatible actions, run-only option with another action) | `tsfs --help` |
| Logging directory error at startup | `logging.directory` not writable by the current user | Use a writable directory (as in section 2) or run as the service account |
| FUSE tests skipped | `/dev/fuse` unavailable or `TSFS_TEST_DISABLE_FUSE` set | See [testing.md](testing.md#3-fuse-tests) |
| TSan binary aborts immediately | ASLR with `vm.mmap_rnd_bits` > 28 | `scripts/tsfs-dev.sh --no-aslr ...` |
| `addr2line` prints `??:0` | Binary differs from the crashed one, or is stripped / Release | Use the exact binary of the crash, or a `relwithdebinfo` build of the same commit (`scripts/tsfs-symbolize.sh -m`) |
| Crash backtrace shows `?? (module+0x...)` | Internal function of a stripped library (libc), or a stripped `tsfs` | Normal for system libraries; symbolize offline with their debug symbols; do not strip `tsfs` |
| `ERROR: LeakSanitizer: detected memory leaks` at exit, exit status 1 | A real leak (Debug builds run LeakSanitizer) | Fix it; never add a TSFS symbol to `tests/sanitizers/lsan.supp` |

Exit codes of `tsfs`: 0 success, 1 runtime error, 2 usage error, 3 privileges, 4 missing
dependency, 5 invalid configuration, 7 unexpected system state.
