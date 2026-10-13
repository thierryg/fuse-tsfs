<!--
=============================================================================
TSFS - Time-aware Storage Filesystem for Linux
=============================================================================
File    : docs/architecture/diagnostics.md
Purpose : Crash diagnostics: async-signal-safe text reports, symbolized backtrace, report pruning, native core dumps, limits
Author  : Thierry Gayet <thierry.gayet@labworks.fr>
Project : tsfs (version: VERSION)
Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
=============================================================================
-->

# Crash diagnostics

Sources: `src/diagnostics/crash_handler.{hpp,cpp}`,
`src/diagnostics/symbol_table.{hpp,cpp}`, `src/common/threads.cpp`,
`src/app/application.cpp`, `scripts/tsfs-symbolize.sh`. Tests: `tests/diagnostics/test_diagnostics.cpp`
(with the helper program `tests/diagnostics/crash_helper.cpp`). Related:
[logging](logging.md), [concurrency](concurrency.md),
[user guide](../user/user-guide.md), spec sections 11 and 35.8 in
[SPECIFICATION.md](../developer/SPECIFICATION.md).

Crash diagnostics are independent of the ordinary logger: the handler never
touches the `Logger`, spdlog, a mutex or the heap, and crash reports are never
removed by log rotation.

## 1. Configuration

| Key (`diagnostics`) | Default | CLI | Meaning |
|---|---|---|---|
| `enabled` | `false` | `--stack-trace` / `--core-dump` set it to true | Master switch. |
| `text_stack_trace` | `true` | `--stack-trace`, `--no-stack-trace` | Mode A: text report on a fatal signal. |
| `native_core_dump` | `false` | `--core-dump`, `--no-core-dump` | Mode B: allow kernel core dumps. |
| `crash_directory` | `/var/lib/tsfs/crash` | | Report directory; must be outside the backing directory and the mount point when enabled. |
| `max_crash_reports` | 10 | | 1..1000 reports kept. |

All are restart-only. The daemon installs diagnostics after daemonization and
after the logger is configured (startup step 10 in [overview](overview.md)).
Startup events (component `diagnostics`):

| Event | Level | Meaning |
|---|---|---|
| `diagnostics_enabled` | info | Crash reports armed; fields `directory` and `symbols` (number of function symbols loaded, `CrashHandler::symbol_count()`) |
| `diagnostics_unavailable` | warning | `CrashHandler::install` failed (for example, crash directory not creatable); not fatal |
| `diagnostics_core_dumps` | info / warning | Core-dump status (`rlimit_core`, `core_pattern`, `handler`; warning when unavailable), then one info event per operator note |

The `diagnostic_report_started`, `diagnostic_report_completed` and
`diagnostic_report_failed` identifiers are used only at crash time, on stderr
(section 2.2).

## 2. Mode A: text crash report

### 2.1 Installation (`CrashHandler::install`)

1. Create the directory (`mkdir -p`, mode 0750) and open it with
   `O_PATH | O_DIRECTORY`; the descriptor is kept for the handler.
2. Prune old reports so that at most `max_crash_reports - 1` remain (room for
   the next one).
3. Copy the version string into a fixed 64-byte buffer.
4. Call `backtrace()` once, outside any handler: glibc loads `libgcc_s` lazily
   (with `malloc` and `dlopen`) on first use, so the warm-up guarantees that the
   handler never allocates.
5. Load the symbol table once (`SymbolTable::load_current_process()`, section
   2.4): the handler only reads it.
6. Install an alternate signal stack for the calling (main) thread; helper
   threads get their own in `prepare_helper_thread`.
7. `sigaction` with `SA_SIGINFO | SA_ONSTACK` for `SIGSEGV`, `SIGBUS`,
   `SIGILL`, `SIGFPE`, `SIGABRT`, `SIGSYS`, `SIGTRAP`, saving the previous
   dispositions (restored by `uninstall()` at shutdown).

### 2.2 Handler (`fatal_handler`)

```
 fatal signal on thread T (alternate stack)
   g_handling.exchange(1) != 0 ?  -> another thread is reporting: pause() forever
   openat(dir_fd, "tsfs-crash-<unix-seconds>-<pid>.txt", O_CREAT|O_EXCL|O_NOFOLLOW, 0600)
   backtrace() (up to 64 frames); find the faulting frame (= instruction pointer)
   stderr: "tsfs: fatal signal SIGSEGV, crash report: <name> event=diagnostic_report_started"
       or "tsfs: fatal signal SIGSEGV, no crash report (cannot create it) event=diagnostic_report_failed"
   stderr: "tsfs: backtrace:" + symbolized frames (same format as the report)
   write_report(); fsync(); close()
   stderr: "tsfs: crash report written: <name> event=diagnostic_report_completed"
   sigaction(sig, SIG_DFL)
   kernel fault (si_code > 0, not SIGABRT): return -> the instruction faults again -> default action
   otherwise (raise/kill/abort): raise(sig) -> default action
```

The logger cannot be used in a signal handler: the crash-time events go to
stderr (the journal under systemd) with the same event identifiers as the log.
The symbolized backtrace is printed on stderr even when no report file can be
created. The process therefore terminates exactly as it would have without the handler,
including a core dump when Mode B allows it; execution never resumes after a
fault.

### 2.3 Report content

Written with `put()`/`put_unsigned()`/`put_hex()` (integer formatting into stack
buffers, `write(2)` loops):

```
TSFS crash report
version: <version>
signal: <number> (<name>)
code: <si_code>
pid: <pid>
tid: <gettid>
time: <YYYY-MM-DDTHH:MM:SSZ> (unix <seconds>)
fault_address: 0x...
instruction_pointer: 0x...          (x86-64 REG_RIP, AArch64 pc, else 0)

backtrace (raw addresses):
  #00 0x...
  ...                                (up to 64 frames)

backtrace (symbols, from the faulting frame; file:line: scripts/tsfs-symbolize.sh):
  #00 0x... function+0xOFF (module)  (faulting frame first; handler frames skipped)
  #01 0x... ?? (module+0xOFFSET)     (internal function of a stripped library)
  ...

memory map (/proc/self/maps):
<copied with open/read/write>

end of report
```

The UTC date is computed with pure integer arithmetic (civil-from-days
algorithm). The header lines are written before the backtrace, so they survive
even if stack unwinding fails. The raw section lists every captured frame,
handler frames included; the symbolized section starts at the faulting frame
(the frame whose address equals the instruction pointer) and renumbers from
`#00`. A frame that no symbol covers but that lies in a loaded segment is shown
as `?? (module+0xOFFSET)`, the module-relative address expected by `addr2line`;
an address outside every loaded object is shown as `??`. Example from the
daemon: `#06 0x... tsfs::FuseSession::loop(unsigned int)+0x40 (/usr/bin/tsfs)`.

The memory map allows offline symbolization of the raw addresses with file and
line: `scripts/tsfs-symbolize.sh [-m MODULE=FILE]... REPORT` maps every raw
address to its module and file offset with the report's memory map and runs
`addr2line -f -C -i -p` (inlined frames included). It needs the same build
(RelWithDebInfo or Debug for file:line; a Release build gives function names
only); `-m` substitutes an unstripped binary or a `.debug` file for a module.
See [debugging.md](../developer/debugging.md).

### 2.4 Symbol table (`SymbolTable`)

`SymbolTable::load_current_process()` runs once, in `CrashHandler::install`, in
a normal context. It enumerates every loaded ELF object with `dl_iterate_phdr`
(the executable is read through `/proc/self/exe`; the vDSO is skipped), reads
each file with `pread`, takes the function symbols (`STT_FUNC`,
`STT_GNU_IFUNC`) of `.symtab` (which also holds internal-linkage functions) or,
for a stripped file, of `.dynsym`, relocates them by the load bias, demangles
them (`abi::__cxa_demangle`) and sorts them by address. The `PT_LOAD` segments
of every object are recorded too. Unreadable or unsupported objects are
skipped. Objects loaded later with `dlopen` are not covered.

In the handler, `lookup()` (function containing an address) and `locate()`
(object and module-relative offset) are binary searches in that immutable
memory: no allocation, no lock, no system call. A return address points after
the call instruction, so `address - 1` is looked up for every frame except the
faulting one.

Function names require unstripped binaries: `cmake --install` does not strip
`tsfs`, so internal TSFS functions are named. System libraries are usually
stripped, so their internal functions appear as `?? (module+0xOFFSET)`.

### 2.5 Async-signal-safety

Calls used in the handler: `openat`, `write`, `read`, `open`, `close`, `fsync`,
`clock_gettime`, `getpid`, `syscall(SYS_gettid)`, `sigaction`, `raise`,
`pause`, `backtrace` (pre-loaded), `SymbolTable::lookup`/`locate` (binary
searches in preloaded memory). `backtrace_symbols_fd` (writes directly to the
descriptor, no allocation) is used only as a fallback when no symbol table
could be loaded. State read by the handler is written before installation
(`g_dir_fd`, `g_version`, the symbol table published through the atomic
`g_symbols`) or is atomic (`g_handling`). No mutex,
no `malloc`, no stdio, no C++ object construction, no logger. `errno` is saved
at entry and restored before returning.

### 2.6 Report pruning

`CrashHandler::prune_reports(directory, keep)` lists regular files named
`tsfs-crash-*.txt`, sorts them by modification time (newest first) and unlinks
all but `keep`. Any other file in the directory, including core files, is never
touched (`OldReportsArePrunedButNotOtherFiles`). Pruning runs only at
installation (process start), never in the handler.

## 3. Mode B: native core dumps (`enable_core_dumps`)

* Raise the soft `RLIMIT_CORE` to the hard limit (when the hard limit is
  non-zero) and set `PR_SET_DUMPABLE` to 1.
* Inspect, never modify, the kernel settings (`inspect_core_dumps`): read
  `/proc/sys/kernel/core_pattern` and classify the handler as
  `systemd-coredump`, `apport`, another pipe (`|...`) or a plain file pattern.
* Produce notes for the operator, logged at startup: "cores are collected by
  systemd-coredump: coredumpctl list tsfs"; apport ignores non-packaged
  binaries by default; a relative file pattern writes into the working
  directory, which is `/` for a daemon (usually not writable); a hard
  `RLIMIT_CORE` of 0 must be raised (systemd `LimitCORE=infinity`, `ulimit -Hc`).
* `enabled` is reported true when the soft limit is non-zero and the process is
  dumpable; the startup event (`diagnostics_core_dumps`, info or warning)
  carries `rlimit_core`, `core_pattern` and `handler`; each note is logged as a
  separate `diagnostics_core_dumps` info event.

Mode A and Mode B combine: the text report is written first, then the default
action produces the core dump.

## 4. Limitations

* `SIGKILL` and kernel OOM kills cannot be caught: no report.
* Heap or stack corruption can prevent `backtrace()` from completing; the header lines are written first
  for that reason. A report is best effort, never guaranteed.
* Threads created by libfuse have no alternate signal stack: a stack overflow on
  a FUSE worker thread produces no text report (a core dump, if enabled, is
  still produced). TSFS helper threads and the main thread have one.
* If two threads crash at the same time, only the first one is reported; the
  second waits in `pause()` until the process is terminated.
* The crash-time backtrace gives function names, not files and lines; run
  `scripts/tsfs-symbolize.sh` on the report for file:line. Internal functions of
  stripped libraries (libc, libfuse) and of a stripped `tsfs` binary appear as
  `?? (module+0xOFFSET)`, to be resolved offline with the matching debug
  symbols. Objects loaded after startup (`dlopen`) are not symbolized.
* One report per second per PID can be created (the name has a one-second
  resolution and is opened with `O_EXCL`); this only matters for a crash loop
  that reuses the same PID.
* Crash reports, core files and the crash directory are outside the log
  rotation scheme; operators manage core files through the system's core
  handler.
