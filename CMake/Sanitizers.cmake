# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Sanitizers.cmake
# Purpose : Opt-in AddressSanitizer / UndefinedBehaviorSanitizer / ThreadSanitizer builds
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# TSFS_SANITIZE accepts "address", "undefined", "address;undefined", "thread" or "none".
# Default (CMakeLists.txt): "address;undefined" for Debug trees without coverage,
# "none" otherwise. AddressSanitizer includes LeakSanitizer on Linux: every process
# built with it reports the memory still allocated and unreachable at exit, and exits
# with a non-zero status (1), so a leak fails the test that produced it.
# ThreadSanitizer cannot be combined with AddressSanitizer (both replace the memory
# allocator and shadow memory); the combination is rejected at configure time.
# Release builds never enable them implicitly, so release packages never carry them.

if(TSFS_SANITIZE AND NOT TSFS_SANITIZE STREQUAL "none")
    set(_allowed address undefined thread)
    foreach(_s IN LISTS TSFS_SANITIZE)
        if(NOT _s IN_LIST _allowed)
            message(FATAL_ERROR "TSFS_SANITIZE: unknown sanitizer '${_s}' (allowed: ${_allowed})")
        endif()
    endforeach()
    if("thread" IN_LIST TSFS_SANITIZE AND "address" IN_LIST TSFS_SANITIZE)
        message(FATAL_ERROR "TSFS_SANITIZE: 'thread' and 'address' cannot be combined")
    endif()
    if(TSFS_ENABLE_COVERAGE)
        message(FATAL_ERROR "TSFS_SANITIZE and TSFS_ENABLE_COVERAGE must use separate build trees")
    endif()
    list(JOIN TSFS_SANITIZE "," _list)
    target_compile_options(tsfs_options INTERFACE -fsanitize=${_list} -fno-omit-frame-pointer
                                                  -fno-sanitize-recover=all)
    target_link_options(tsfs_options INTERFACE -fsanitize=${_list})
    message(STATUS "TSFS: sanitizers enabled: ${_list}")
endif()
