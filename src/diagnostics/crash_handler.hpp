// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/diagnostics/crash_handler.hpp
// Purpose : Fatal-signal text reports (async-signal-safe) and native core-dump enabling
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/resource.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/error.hpp"

namespace tsfs::diagnostics {

/// Settings of the text crash report (Mode A).
struct CrashReportOptions {
    std::string directory;          ///< Where reports go (created 0750 if missing).
    std::uint32_t max_reports{10};  ///< Older reports are pruned at install time.
    std::string version;            ///< Copied into every report.
};

/**
 * @brief Mode A: write a text report when the process receives a fatal signal.
 *
 * Handled signals: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP.
 * The handler runs on the alternate signal stack (stack overflows are reported)
 * and only uses async-signal-safe operations: openat/write/read/close/fsync on
 * descriptors prepared at install time, integer formatting into stack buffers,
 * backtrace() (its library is preloaded at install so the handler never
 * allocates) and backtrace_symbols_fd(). No mutex, no malloc, no C++ object, no
 * logger. The report "tsfs-crash-<unix-time>-<pid>.txt" (mode 0600) contains the
 * signal, code, pid, tid, time, fault address, instruction pointer, raw frames,
 * the symbolized backtrace ("function+offset (module)", from the faulting frame)
 * and a copy of /proc/self/maps for offline file:line symbolization
 * (scripts/tsfs-symbolize.sh). The summary line and the symbolized backtrace also
 * go to stderr (the journal under systemd).
 *
 * Symbols: install() loads the function symbols of every loaded ELF object into a
 * SymbolTable (demangled, sorted); the handler only performs binary searches in
 * that memory. Binaries must keep their .symtab (not stripped) for internal
 * functions; a stripped binary falls back to its exported (.dynsym) symbols.
 *
 * After the report the default action is restored and the signal re-raised (or
 * the faulting instruction re-executed), so the process terminates exactly as it
 * would have, including a core dump when Mode B allows it. Execution is never
 * resumed after a fault. A second thread crashing concurrently waits for the
 * first report to finish.
 *
 * Limitations: SIGKILL and OOM kills cannot be caught; a heap/stack corruption can
 * prevent backtrace() from working (the header lines are written first); threads
 * created by libfuse have no alternate stack, so a stack overflow there yields
 * no report (the core dump still does).
 *
 * Process-wide state (signal handlers are global): install once from main().
 */
class CrashHandler {
public:
    /// Install the handlers. @return EACCES/ENOENT when the directory is unusable.
    static Result<void> install(const CrashReportOptions& options);
    /// Restore the previous dispositions (normal shutdown, tests).
    static void uninstall() noexcept;
    /// @return true when installed.
    static bool installed() noexcept;
    /// @return the number of function symbols available for crash-time backtraces
    /// (0 before install(), or when no symbol table could be read).
    static std::size_t symbol_count() noexcept;
    /// Remove the oldest reports so that at most @p keep remain. @return files removed.
    static std::size_t prune_reports(const std::string& directory, std::uint32_t keep);
};

/// Mode B: what the system will do with a core dump of this process.
struct CoreDumpStatus {
    bool enabled{false};  ///< RLIMIT_CORE soft limit > 0 and process dumpable.
    rlim_t soft_limit{0};
    rlim_t hard_limit{0};
    bool dumpable{false};            ///< prctl(PR_GET_DUMPABLE).
    std::string core_pattern;        ///< /proc/sys/kernel/core_pattern ("" if unreadable).
    std::string handler;             ///< "systemd-coredump", "apport", "pipe", "file".
    std::vector<std::string> notes;  ///< Human-readable warnings (hard limit 0, relative pattern...).
};

/// @return the current core-dump configuration (no change).
CoreDumpStatus inspect_core_dumps();

/**
 * @brief Mode B: allow native core dumps (soft RLIMIT_CORE raised to the hard
 * limit, process marked dumpable). Kernel settings (core_pattern, suid_dumpable)
 * are reported, never modified.
 * @return the resulting status; enabled == false with notes when impossible.
 */
CoreDumpStatus enable_core_dumps();

}  // namespace tsfs::diagnostics
