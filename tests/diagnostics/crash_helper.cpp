// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/diagnostics/crash_helper.cpp
// Purpose : Isolated test executable that crashes on purpose (never the test runner itself)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// Usage: tsfs_crash_helper <segv|abort|fpe|stack|thread-segv|exit> <report-dir> [core]
// Installs the TSFS crash handler (and core dumps with "core"), then triggers the
// requested failure. "exit" terminates normally (the handler must not interfere).
#include <signal.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <print>
#include <string_view>
#include <thread>

#include "common/threads.hpp"
#include "diagnostics/crash_handler.hpp"

namespace {

// Excluded from UBSan: with -fno-sanitize-recover the null check would exit(1)
// before the store faults, and the crash handler under test would never run.
[[gnu::noinline, gnu::no_sanitize("undefined")]] void deliberate_null_write() {
    volatile int* p = nullptr;
    *p = 42;  // NOLINT: the purpose of this program.
}

// The unbounded recursion is the point of the "stack" mode: silence the warning
// for this function only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
[[gnu::noinline]] int recurse(int depth) {
    volatile char frame[1024];
    frame[0] = static_cast<char>(depth);
    return recurse(depth + 1) + frame[0];  // Unbounded recursion: stack overflow.
}
#pragma GCC diagnostic pop

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println(stderr, "usage: tsfs_crash_helper <mode> <report-dir> [core]");
        return 2;
    }
    const std::string_view mode = argv[1];
    if (argc > 3 && std::string_view(argv[3]) == "core") {
        const auto status = tsfs::diagnostics::enable_core_dumps();
        std::println("core_enabled={} soft={} handler={}", status.enabled, status.soft_limit, status.handler);
    }
    if (auto r = tsfs::diagnostics::CrashHandler::install({argv[2], 10, "test"}); !r) {
        std::println(stderr, "install failed: {}", r.error().message);
        return 3;
    }
    std::fflush(stdout);
    if (mode == "segv") {
        deliberate_null_write();
    } else if (mode == "abort") {
        std::abort();
    } else if (mode == "fpe") {
        std::raise(SIGFPE);
    } else if (mode == "stack") {
        return recurse(0);
    } else if (mode == "thread-segv") {
        std::thread t([] {
            tsfs::install_alternate_signal_stack();
            deliberate_null_write();
        });
        t.join();
    } else if (mode == "exit") {
        tsfs::diagnostics::CrashHandler::uninstall();
        return 0;
    }
    return 4;
}
