# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Coverage.cmake
# Purpose : gcov instrumentation and the "coverage" target (lcov capture + genhtml report)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Workflow (dedicated tree, so the flags never reach a normal build):
#   cmake --preset coverage && cmake --build --preset coverage
#   cmake --build --preset coverage --target tests      # runs CTest, writes .gcda files
#   cmake --build --preset coverage --target coverage   # capture + HTML in build/coverage/coverage/html
# The coverage target resets nothing by itself: "coverage-reset" zeroes the counters.

if(NOT TSFS_ENABLE_COVERAGE)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "TSFS_ENABLE_COVERAGE requires GCC (gcov); current compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# gcov must match the compiler major version (gcov-15 for g++-15).
string(REGEX MATCH "^[0-9]+" _gcc_major "${CMAKE_CXX_COMPILER_VERSION}")
find_program(GCOV_EXE NAMES gcov-${_gcc_major} gcov REQUIRED)
find_program(LCOV_EXE NAMES lcov REQUIRED)
find_program(GENHTML_EXE NAMES genhtml REQUIRED)

target_compile_options(tsfs_options INTERFACE --coverage -fprofile-update=atomic -O0 -fno-inline)
target_link_options(tsfs_options INTERFACE --coverage)

set(TSFS_COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
# lcov 2.x is strict by default: report (not abort on) known harmless gcov quirks of
# C++ templates and inline functions, and never drop production sources from the report.
set(_lcov_common --gcov-tool ${GCOV_EXE} --rc branch_coverage=1
                 --ignore-errors mismatch,mismatch,negative,negative,unused,unused,inconsistent,inconsistent)

add_custom_target(coverage-reset
    COMMAND ${LCOV_EXE} ${_lcov_common} --zerocounters --directory ${CMAKE_BINARY_DIR}
    COMMENT "Resetting coverage counters" VERBATIM)

add_custom_target(coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TSFS_COVERAGE_DIR}
    COMMAND ${LCOV_EXE} ${_lcov_common} --capture --directory ${CMAKE_BINARY_DIR}
            --base-directory ${CMAKE_SOURCE_DIR} --no-external
            --output-file ${TSFS_COVERAGE_DIR}/raw.info
    # Keep production sources only: tests, benchmarks and generated files are scaffolding.
    COMMAND ${LCOV_EXE} ${_lcov_common} --extract ${TSFS_COVERAGE_DIR}/raw.info
            "${CMAKE_SOURCE_DIR}/src/*" --output-file ${TSFS_COVERAGE_DIR}/tsfs.info
    COMMAND ${GENHTML_EXE} --rc branch_coverage=1 --ignore-errors inconsistent,inconsistent,corrupt,corrupt
            --demangle-cpp --legend --title "TSFS ${PROJECT_VERSION}"
            --output-directory ${TSFS_COVERAGE_DIR}/html ${TSFS_COVERAGE_DIR}/tsfs.info
    COMMAND ${LCOV_EXE} ${_lcov_common} --summary ${TSFS_COVERAGE_DIR}/tsfs.info
    COMMAND ${CMAKE_COMMAND} -DINFO=${TSFS_COVERAGE_DIR}/tsfs.info -DOUT=${TSFS_COVERAGE_DIR}
            -P ${CMAKE_SOURCE_DIR}/CMake/CoverageSummary.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Capturing coverage into ${TSFS_COVERAGE_DIR}/html/index.html"
    VERBATIM)
