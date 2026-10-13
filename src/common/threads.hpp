// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/threads.hpp
// Purpose : Thread setup helpers: signal masking for helper threads, thread names
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <string_view>

namespace tsfs {

/**
 * @brief Block every asynchronous signal in the calling thread.
 *
 * Process signals (SIGINT, SIGTERM, SIGHUP, SIGUSR1...) must be delivered to the
 * main thread, which runs the FUSE loop and the signal handlers. Every helper
 * thread (GC, logging, reload watcher) calls this first. Synchronous fault signals
 * (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT) stay unblocked so that the crash
 * handler still runs on the faulting thread.
 */
void block_async_signals() noexcept;

/// Name the calling thread (visible in top -H, gdb, crash reports). Truncated to 15 bytes.
void set_thread_name(std::string_view name) noexcept;

/**
 * @brief Give the calling thread an alternate signal stack (64 KiB, freed at thread exit).
 *
 * Without it a stack overflow cannot be reported: the crash handler would need the
 * exhausted stack to run. Idempotent per thread.
 */
void install_alternate_signal_stack() noexcept;

/// Standard setup of a TSFS helper thread: block_async_signals(), name, alternate stack.
void prepare_helper_thread(std::string_view name) noexcept;

}  // namespace tsfs
