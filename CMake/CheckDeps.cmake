# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/CheckDeps.cmake
# Purpose : "deps" target: report every dependency per feature set, fail on missing required ones
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Usage (normally through the target): cmake -DPROFILES="build;test" -P CMake/CheckDeps.cmake
# Profiles:
#   build        C++26 compiler, pkg-config, libfuse3, yaml-cpp, spdlog, fmt, zlib, fusermount3
#   test         GoogleTest/GoogleMock
#   bench        Google Benchmark
#   doc          Doxygen, Graphviz (dot)
#   coverage     gcov, lcov, genhtml
#   diagnostics  gdb, addr2line
#   qa           clang-format, clang-tidy, cppcheck, shellcheck
# Every profile is reported; only the requested ones make the target fail. Nothing is
# installed: the report prints the Ubuntu/Debian package names to install.

if(NOT DEFINED PROFILES)
    set(PROFILES build test)
endif()
set(_all_profiles build test bench doc coverage diagnostics qa)
set(_failed FALSE)
set(_packages "")

find_package(PkgConfig QUIET)

# _report(profile name found package [detail])
function(_report profile name found package)
    set(_detail "${ARGN}")
    if(profile IN_LIST PROFILES)
        set(_kind "required")
    else()
        set(_kind "optional")
    endif()
    if(found)
        message(STATUS "  [ok]      ${profile}: ${name} ${_detail}")
    else()
        if(_kind STREQUAL "required")
            message(STATUS "  [MISSING] ${profile}: ${name} (package: ${package})")
            set(_failed TRUE PARENT_SCOPE)
            set(_packages "${_packages} ${package}" PARENT_SCOPE)
        else()
            message(STATUS "  [absent]  ${profile}: ${name} (optional for the requested profiles; package: ${package})")
        endif()
    endif()
endfunction()

function(_check_pc profile module package)
    if(PkgConfig_FOUND)
        pkg_check_modules(_PC QUIET ${module})
    endif()
    if(_PC_FOUND)
        _report(${profile} "${module}" TRUE ${package} "${_PC_VERSION}")
    else()
        _report(${profile} "${module}" FALSE ${package})
    endif()
    set(_failed ${_failed} PARENT_SCOPE)
    set(_packages "${_packages}" PARENT_SCOPE)
endfunction()

# In script mode (cmake -P) CMAKE_SYSTEM_PREFIX_PATH is empty: the standard include
# directories must be given explicitly, or installed headers are reported missing.
set(_include_dirs /usr/local/include /usr/include /usr/include/x86_64-linux-gnu /usr/include/aarch64-linux-gnu)

function(_check_header profile header package)
    find_path(_H_${header} NAMES ${header} PATHS ${_include_dirs} NO_CACHE)
    if(_H_${header})
        _report(${profile} "<${header}>" TRUE ${package} "")
    else()
        _report(${profile} "<${header}>" FALSE ${package})
    endif()
    set(_failed ${_failed} PARENT_SCOPE)
    set(_packages "${_packages}" PARENT_SCOPE)
endfunction()

function(_check_program profile program package)
    find_program(_P_${program} NAMES ${program} NO_CACHE)
    if(_P_${program})
        execute_process(COMMAND ${_P_${program}} --version OUTPUT_VARIABLE _v ERROR_VARIABLE _v
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "[^\n]*" _v "${_v}")
        _report(${profile} "${program}" TRUE ${package} "(${_v})")
    else()
        _report(${profile} "${program}" FALSE ${package})
    endif()
    set(_failed ${_failed} PARENT_SCOPE)
    set(_packages "${_packages}" PARENT_SCOPE)
endfunction()

message(STATUS "TSFS dependency check (required profiles: ${PROFILES}; known: ${_all_profiles})")

# build
if(CXX26_OK)
    _report(build "C++26 compiler" TRUE g++ "${CXX_COMPILER_ID} ${CXX_COMPILER_VERSION}")
else()
    _report(build "C++26 compiler (have ${CXX_COMPILER_ID} ${CXX_COMPILER_VERSION})" FALSE "g++-15 or clang-21")
endif()
_check_program(build pkg-config pkg-config)
_check_pc(build "fuse3>=3.14" libfuse3-dev)
_check_pc(build "yaml-cpp>=0.7" libyaml-cpp-dev)
_check_pc(build "spdlog>=1.12" libspdlog-dev)
_check_pc(build fmt libfmt-dev)
_check_pc(build zlib zlib1g-dev)
_check_program(build fusermount3 fuse3)
# test
_check_header(test gtest/gtest.h libgtest-dev)
_check_header(test gmock/gmock.h libgmock-dev)
# bench
_check_header(bench benchmark/benchmark.h libbenchmark-dev)
# doc
_check_program(doc doxygen doxygen)
_check_program(doc dot graphviz)
# coverage
_check_program(coverage gcov gcc)
_check_program(coverage lcov lcov)
_check_program(coverage genhtml lcov)
# diagnostics
_check_program(diagnostics gdb gdb)
_check_program(diagnostics addr2line binutils)
# qa
_check_program(qa clang-format clang-format)
_check_program(qa clang-tidy clang-tidy)
_check_program(qa cppcheck cppcheck)
_check_program(qa shellcheck shellcheck)

if(_failed)
    message(STATUS "")
    message(STATUS "Install the missing packages (Ubuntu 26.04 / Debian 13):")
    message(STATUS "  sudo apt install${_packages}")
    message(STATUS "Or build inside the development container: scripts/tsfs-dev.sh")
    message(FATAL_ERROR "TSFS: required dependencies are missing")
endif()
message(STATUS "TSFS: every required dependency is present")
