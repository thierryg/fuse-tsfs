# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : tests/CMakeLists.txt
# Purpose : Test executables, CTest registration with labels, and the "tests" target
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Labels (ctest -L <label>): unit, policy, config, reload, gc, concurrency, logging,
# diagnostics, security, filesystem, stress. Skipped tests (GTEST_SKIP: no FUSE, root,
# core dumps not enabled...) are reported as "Not Run (Skipped)", never as passed.
include(GoogleTest)

add_library(tsfs_test_support STATIC support/test_support.cpp)
target_include_directories(tsfs_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tsfs_test_support PUBLIC tsfs_core GTest::gtest GTest::gtest_main)

set(TSFS_TEST_ARTIFACT_DIR "${CMAKE_BINARY_DIR}/test-artifacts" CACHE PATH
    "Where tests create their temporary trees (kept with TSFS_TEST_KEEP_ARTIFACTS=1)")

# Sanitizer runtime options for every test process (and the tsfs daemons and crash
# helpers they start, which inherit the environment). LeakSanitizer is explicit:
# a leak makes the process exit with a non-zero status and fails the test.
set(TSFS_SANITIZER_TEST_ENV "")
if("address" IN_LIST TSFS_SANITIZE)
    list(APPEND TSFS_SANITIZER_TEST_ENV
         "ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:check_initialization_order=1:detect_stack_use_after_return=1"
         "LSAN_OPTIONS=print_suppressions=0:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/sanitizers/lsan.supp")
endif()
if("undefined" IN_LIST TSFS_SANITIZE)
    list(APPEND TSFS_SANITIZER_TEST_ENV "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endif()

# tsfs_add_test(<name> LABELS <labels...> SOURCES <files...> [TIMEOUT <s>])
function(tsfs_add_test name)
    cmake_parse_arguments(T "" "TIMEOUT" "LABELS;SOURCES" ${ARGN})
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT 120)
    endif()
    add_executable(${name} ${T_SOURCES})
    target_link_libraries(${name} PRIVATE tsfs_test_support)
    target_compile_definitions(${name} PRIVATE
        TSFS_BINARY="$<TARGET_FILE:tsfs>"
        TSFS_CRASH_HELPER="$<TARGET_FILE:tsfs_crash_helper>"
        TSFS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    # gtest_discover_tests flattens PROPERTIES: list values (several labels, several
    # environment variables) must have their separators escaped, or only the first
    # element is kept and the next ones are read as property names.
    set(_env "TSFS_TEST_ARTIFACT_DIR=${TSFS_TEST_ARTIFACT_DIR}" ${TSFS_SANITIZER_TEST_ENV})
    string(REPLACE ";" "\\;" _env "${_env}")
    string(REPLACE ";" "\\;" _labels "${T_LABELS}")
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "${_labels}"
            TIMEOUT ${T_TIMEOUT}
            SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
            ENVIRONMENT "${_env}")
    set(TSFS_TEST_TARGETS ${TSFS_TEST_TARGETS} ${name} PARENT_SCOPE)
endfunction()

add_executable(tsfs_crash_helper diagnostics/crash_helper.cpp)
target_link_libraries(tsfs_crash_helper PRIVATE tsfs_core)
# Frame pointers and no inlining keep the deliberate crash frames visible in the report.
target_compile_options(tsfs_crash_helper PRIVATE -fno-omit-frame-pointer -O0)
target_link_options(tsfs_crash_helper PRIVATE -rdynamic)

tsfs_add_test(tsfs_unit_tests LABELS unit SOURCES unit/test_common.cpp)
tsfs_add_test(tsfs_policy_tests LABELS "unit;policy" SOURCES policy/test_policy.cpp)
tsfs_add_test(tsfs_config_tests LABELS "unit;config" SOURCES config/test_config.cpp)
tsfs_add_test(tsfs_reload_tests LABELS "config;reload;concurrency" SOURCES config/test_reload.cpp)
tsfs_add_test(tsfs_scheduler_tests LABELS "unit;gc;concurrency" SOURCES gc/test_scheduler.cpp)
tsfs_add_test(tsfs_gc_tests LABELS "gc;concurrency;security" SOURCES gc/test_garbage_collector.cpp)
tsfs_add_test(tsfs_logging_tests LABELS "logging;concurrency" SOURCES logging/test_logging.cpp)
tsfs_add_test(tsfs_diagnostics_tests LABELS diagnostics SOURCES diagnostics/test_diagnostics.cpp)
add_dependencies(tsfs_diagnostics_tests tsfs_crash_helper)
tsfs_add_test(tsfs_security_tests LABELS "security" SOURCES security/test_backing_store.cpp)
tsfs_add_test(tsfs_filesystem_tests LABELS "filesystem;security;concurrency" SOURCES filesystem/test_mount.cpp
              TIMEOUT 300)
add_dependencies(tsfs_filesystem_tests tsfs)
tsfs_add_test(tsfs_stress_tests LABELS stress SOURCES stress/test_stress.cpp TIMEOUT 600)

# "tests": build every test binary (and the daemon), then run CTest.
add_custom_target(tests
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TSFS_TEST_ARTIFACT_DIR}
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
            --output-junit ${CMAKE_BINARY_DIR}/test-results.xml -j 4
    DEPENDS tsfs tsfs_crash_helper ${TSFS_TEST_TARGETS}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
    COMMENT "Running the TSFS test suite (results: ${CMAKE_BINARY_DIR}/test-results.xml)")

set(TSFS_BUILD_TARGETS ${TSFS_BUILD_TARGETS} tsfs_crash_helper ${TSFS_TEST_TARGETS} PARENT_SCOPE)
