# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Dependencies.cmake
# Purpose : Discovery of the build dependencies (pkg-config / CMake packages), never downloads
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Sets TSFS_DEPS_OK (all production dependencies found), TSFS_MISSING_DEPS (list),
# TSFS_CXX26_OK, TSFS_HAVE_GTEST, TSFS_HAVE_BENCHMARK and imported targets:
#   PkgConfig::FUSE3, yaml-cpp target, spdlog::spdlog, ZLIB::ZLIB, Threads::Threads,
#   GTest::gtest / GTest::gmock, benchmark::benchmark.
# Nothing is downloaded: a missing dependency is reported, never fetched.

set(TSFS_DEPS_OK TRUE)
set(TSFS_MISSING_DEPS "")

macro(tsfs_missing name)
    set(TSFS_DEPS_OK FALSE)
    list(APPEND TSFS_MISSING_DEPS ${name})
endmacro()

# --- C++26 compiler -------------------------------------------------------------
# The code uses C++26 language and library features (pack indexing, placeholder
# variables, deleted-function reasons, saturation arithmetic, views::concat, #embed).
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++26")
check_cxx_source_compiles([=[
#include <expected>
#include <numeric>
#include <print>
#include <ranges>
#include <vector>
template <typename... T> using first_t = T...[0];
struct S { S(const S&) = delete("reason"); };
int main() {
    std::vector<int> a{1}, b{2};
    auto [_, _] = std::pair{1, 2};
    first_t<int, long> x = std::add_sat(1, 2);
    for (int v : std::views::concat(a, b)) { x += v; }
    std::expected<int, int> e = x;
    return e.value() > 0 ? 0 : 1;
}
]=] TSFS_CXX26_OK)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT TSFS_CXX26_OK)
    tsfs_missing("C++26 compiler (GCC >= 15 or Clang >= 21 with libstdc++ >= 15)")
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig)
if(NOT PkgConfig_FOUND)
    tsfs_missing("pkg-config")
else()
    pkg_check_modules(FUSE3 IMPORTED_TARGET fuse3>=3.14)
    if(NOT FUSE3_FOUND)
        tsfs_missing("libfuse3-dev (>= 3.14)")
    endif()
endif()

find_package(yaml-cpp 0.7 QUIET)
if(TARGET yaml-cpp::yaml-cpp)
    set(TSFS_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(TSFS_YAML_TARGET yaml-cpp)
else()
    tsfs_missing("libyaml-cpp-dev (>= 0.7)")
endif()

find_package(spdlog 1.12 QUIET)
if(NOT spdlog_FOUND)
    tsfs_missing("libspdlog-dev (>= 1.12)")
endif()

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    tsfs_missing("zlib1g-dev")
endif()

# --- optional: tests and benchmarks ----------------------------------------------
set(TSFS_HAVE_GTEST FALSE)
if(TSFS_BUILD_TESTS)
    find_package(GTest 1.12 QUIET)
    if(GTest_FOUND)
        set(TSFS_HAVE_GTEST TRUE)
    else()
        tsfs_missing("libgtest-dev and libgmock-dev (or configure with -DTSFS_BUILD_TESTS=OFF)")
    endif()
endif()

set(TSFS_HAVE_BENCHMARK FALSE)
if(TSFS_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        set(TSFS_HAVE_BENCHMARK TRUE)
    else()
        message(STATUS "TSFS: Google Benchmark not found, benchmarks disabled (libbenchmark-dev)")
    endif()
endif()
