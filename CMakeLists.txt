# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMakeLists.txt
# Purpose : Canonical build entry point (targets: build, tests, coverage, doc, deps, install, uninstall)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# CMake 3.28 is the oldest release whose CXX_STANDARD 26 handling and presets we test.
cmake_minimum_required(VERSION 3.28)

# The version has a single source: the VERSION file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TSFS_VERSION LIMIT_COUNT 1)
string(STRIP "${TSFS_VERSION}" TSFS_VERSION)

project(tsfs
    VERSION ${TSFS_VERSION}
    DESCRIPTION "Time-aware Storage Filesystem for Linux (FUSE, retention policies, garbage collector)"
    HOMEPAGE_URL "https://labworks.fr/tsfs"
    LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")

# --- options -------------------------------------------------------------------
option(TSFS_BUILD_TESTS "Build the test suite" ON)
option(TSFS_BUILD_BENCHMARKS "Build the benchmarks (needs Google Benchmark)" ON)
option(TSFS_ENABLE_COVERAGE "Instrument for gcov/lcov (use a dedicated build tree)" OFF)
option(TSFS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TSFS_ENABLE_CLANG_TIDY "Run clang-tidy while compiling" OFF)
# Debug builds are instrumented with AddressSanitizer (LeakSanitizer included: memory
# leaks are reported at exit and fail the test) and UndefinedBehaviorSanitizer by
# default. Release builds never are. "none" opts out (e.g. to run under valgrind);
# coverage trees default to none (both instrument the code).
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT TSFS_ENABLE_COVERAGE)
    set(_tsfs_default_sanitize "address;undefined")
else()
    set(_tsfs_default_sanitize "none")
endif()
set(TSFS_SANITIZE "${_tsfs_default_sanitize}" CACHE STRING
    "Sanitizers: 'address;undefined' (Debug default), 'address', 'undefined', 'thread' or 'none'")
set(TSFS_DEFAULT_CONFIG_PATH "/etc/tsfs/config.yml" CACHE STRING "Configuration file used without --config")
set(TSFS_DEPS_PROFILES "build;test" CACHE STRING
    "Feature sets checked by the deps target: build, test, doc, coverage, bench, diagnostics, qa")

include(GNUInstallDirs)
include(Dependencies)

# The deps target works even when a required dependency is missing, so that a fresh
# machine can be diagnosed with: cmake -S . -B build && cmake --build build --target deps
add_custom_target(deps
    COMMAND ${CMAKE_COMMAND}
            "-DPROFILES=${TSFS_DEPS_PROFILES}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}"
            "-DCXX_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
            "-DCXX26_OK=${TSFS_CXX26_OK}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckDeps.cmake"
    COMMENT "Checking the TSFS dependencies (profiles: ${TSFS_DEPS_PROFILES})"
    USES_TERMINAL VERBATIM)

if(NOT TSFS_DEPS_OK)
    message(WARNING "TSFS: required dependencies are missing (${TSFS_MISSING_DEPS}); only the 'deps' "
                    "target is available. Run: cmake --build ${CMAKE_BINARY_DIR} --target deps")
    return()
endif()

include(CompilerOptions)
include(Sanitizers)
include(Coverage)

add_subdirectory(src)

set(TSFS_BUILD_TARGETS tsfs)
if(TSFS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(TSFS_BUILD_BENCHMARKS AND TSFS_HAVE_BENCHMARK)
    add_subdirectory(benchmarks)
endif()

# "build": every artifact of this tree (daemon, tests, benchmarks). Plain
# "cmake --build <dir>" builds the same set through the default "all" target.
add_custom_target(build DEPENDS ${TSFS_BUILD_TARGETS})

include(Documentation)
include(Packaging)

message(STATUS "TSFS ${PROJECT_VERSION}: ${CMAKE_BUILD_TYPE}, ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, "
               "C++${CMAKE_CXX_STANDARD}, libfuse ${FUSE3_VERSION}, sanitizers '${TSFS_SANITIZE}', "
               "coverage ${TSFS_ENABLE_COVERAGE}")
