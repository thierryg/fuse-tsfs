# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : benchmarks/CMakeLists.txt
# Purpose : Benchmark executables and the "bench" target
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Benchmarks are meaningful in Release (or RelWithDebInfo for profiling) builds only;
# the "bench" target prints a warning otherwise. Results: build/<tree>/bench-results/.
#   cmake --build build/release --target bench          # 1k and 100k-file datasets
#   TSFS_BENCH_LARGE=1 cmake --build build/release --target bench   # adds 1M files
# The FUSE-level workload needs a mount: scripts/tsfs-bench-fs.sh.

function(tsfs_add_benchmark name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE tsfs_core benchmark::benchmark benchmark::benchmark_main)
    set(TSFS_BENCH_TARGETS ${TSFS_BENCH_TARGETS} ${name} PARENT_SCOPE)
endfunction()

tsfs_add_benchmark(tsfs_bench_policy bench_policy.cpp)
tsfs_add_benchmark(tsfs_bench_scheduler bench_scheduler.cpp)
tsfs_add_benchmark(tsfs_bench_gc bench_gc.cpp)
tsfs_add_benchmark(tsfs_bench_logging bench_logging.cpp)

# Standalone workload generator (no Google Benchmark: it targets an arbitrary directory).
add_executable(tsfs_fs_workload fs_workload.cpp)
target_link_libraries(tsfs_fs_workload PRIVATE tsfs_options Threads::Threads)
list(APPEND TSFS_BENCH_TARGETS tsfs_fs_workload)

set(TSFS_BENCH_RESULTS_DIR "${CMAKE_BINARY_DIR}/bench-results")
set(_bench_commands "")
foreach(_b IN ITEMS tsfs_bench_policy tsfs_bench_scheduler tsfs_bench_gc tsfs_bench_logging)
    list(APPEND _bench_commands
        COMMAND $<TARGET_FILE:${_b}> --benchmark_out=${TSFS_BENCH_RESULTS_DIR}/${_b}.json
                --benchmark_out_format=json --benchmark_counters_tabular=true)
endforeach()

if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
    set(_bench_warning COMMAND ${CMAKE_COMMAND} -E echo
        "WARNING: ${CMAKE_BUILD_TYPE} build: benchmark numbers are not representative (use a Release tree)")
endif()

add_custom_target(bench
    ${_bench_warning}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TSFS_BENCH_RESULTS_DIR}
    ${_bench_commands}
    DEPENDS ${TSFS_BENCH_TARGETS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running the TSFS benchmarks (JSON results in ${TSFS_BENCH_RESULTS_DIR})"
    VERBATIM)

set(TSFS_BUILD_TARGETS ${TSFS_BUILD_TARGETS} ${TSFS_BENCH_TARGETS} PARENT_SCOPE)
