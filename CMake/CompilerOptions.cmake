# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/CompilerOptions.cmake
# Purpose : Warning policy, hardening flags and per-build-type defaults (interface target)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Every TSFS target links tsfs_options, so the flags never leak to third-party code
# and the coverage / sanitizer flags stay confined to their own build trees.

add_library(tsfs_options INTERFACE)

target_compile_options(tsfs_options INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
    -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    -Wmisleading-indentation -Wunused
    $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast>
    $<$<BOOL:${TSFS_WARNINGS_AS_ERRORS}>:-Werror>)

# Hardening for every build type (cheap, standard on Debian/Ubuntu).
target_compile_options(tsfs_options INTERFACE -fstack-protector-strong -fstack-clash-protection
                                              $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>)
target_link_options(tsfs_options INTERFACE -Wl,-z,relro -Wl,-z,now -Wl,--as-needed)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Debug: no optimization beyond -Og, assertions, frame pointers for usable stack traces.
# Release: -O2 (CMake default -O3 is not measurably faster here and grows the binary).
set(CMAKE_CXX_FLAGS_DEBUG "-Og -g3" CACHE STRING "Debug flags" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release flags" FORCE)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG" CACHE STRING "Release with symbols" FORCE)
target_compile_options(tsfs_options INTERFACE $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
# Release keeps the debug assertions of libstdc++ off; Debug turns them on.
target_compile_definitions(tsfs_options INTERFACE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS TSFS_DEBUG_BUILD=1>)

if(TSFS_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE} "--config-file=${CMAKE_SOURCE_DIR}/.clang-tidy")
endif()
