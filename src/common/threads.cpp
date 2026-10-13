// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/common/threads.cpp
// Purpose : Thread setup helpers: signal masking for helper threads, thread names
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "common/threads.hpp"

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>

namespace tsfs {

void block_async_signals() noexcept {
    sigset_t set;
    sigfillset(&set);
    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS}) {
        sigdelset(&set, sig);
    }
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void set_thread_name(std::string_view name) noexcept {
    std::array<char, 16> buffer{};
    const std::size_t n = std::min(name.size(), buffer.size() - 1);
    std::copy_n(name.begin(), n, buffer.begin());
    pthread_setname_np(pthread_self(), buffer.data());
}

namespace {

/// Per-thread alternate stack, released when the thread exits.
struct AlternateStack {
    void* memory{nullptr};
    std::size_t size{0};
    ~AlternateStack() {
        if (memory != nullptr) {
            stack_t disable{};
            disable.ss_flags = SS_DISABLE;
            sigaltstack(&disable, nullptr);
            munmap(memory, size);
        }
    }
};

}  // namespace

void install_alternate_signal_stack() noexcept {
    thread_local AlternateStack stack;
    if (stack.memory != nullptr) {
        return;
    }
    const std::size_t size = 64 * 1024;
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (memory == MAP_FAILED) {
        return;  // Diagnostics degrade (no report on stack overflow), the thread still runs.
    }
    stack_t ss{};
    ss.ss_sp = memory;
    ss.ss_size = size;
    if (sigaltstack(&ss, nullptr) != 0) {
        munmap(memory, size);
        return;
    }
    stack.memory = memory;
    stack.size = size;
}

void prepare_helper_thread(std::string_view name) noexcept {
    block_async_signals();
    set_thread_name(name);
    install_alternate_signal_stack();
}

}  // namespace tsfs
