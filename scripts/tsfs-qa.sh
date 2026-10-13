#!/usr/bin/env bash
# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : scripts/tsfs-qa.sh
# Purpose : Local quality gate: formatting, linting, builds, tests, sanitizers, coverage, docs
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
#
# Runs the checks that a change must pass, with the toolchain of the current
# environment. On a host without C++26, run it inside the development container:
#   scripts/tsfs-dev.sh --no-aslr scripts/tsfs-qa.sh [STEP...]
# (--no-aslr is needed by the tsan step only.)
#
# Steps (default: all, in this order):
#   deps      dependency check, every profile
#   format    clang-format --dry-run -Werror on src/, tests/, benchmarks/
#   shell     shellcheck on scripts/
#   ci        Release, warnings as errors (preset ci): build + tests
#   debug     Debug with ASan + LeakSanitizer + UBSan (preset debug): build + tests
#   clang     Clang, warnings as errors (preset clang): build + tests
#   tidy      clang-tidy on src/ and benchmarks/ (needs the clang tree)
#   tsan      ThreadSanitizer (preset tsan): build + tests
#   coverage  gcov/lcov (preset coverage): build, tests, HTML report
#   doc       Doxygen HTML (release tree)
#
# Usage:
#   scripts/tsfs-qa.sh [-j JOBS] [STEP...]
#
# Exit codes: 0 every step passed, 1 a step failed, 2 usage error, 4 missing tool.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tsfs-common.sh
. "$SCRIPT_DIR/lib/tsfs-common.sh"

ALL_STEPS=(deps format shell ci debug clang tidy tsan coverage doc)
JOBS="$(nproc)"

usage() {
  sed -n '12,/^SCRIPT_DIR=/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//'
}

# preset_build_test PRESET: configure, build and run the tests of one preset.
preset_build_test() {
  cmake --preset "$1" >/dev/null
  cmake --build "build/$1" -j"$JOBS"
  ctest --test-dir "build/$1" -j"$JOBS" --output-on-failure
}

cxx_sources() {
  find src tests benchmarks -name '*.cpp' -o -name '*.hpp' | sort
}

step_deps() {
  cmake --preset release >/dev/null
  cmake -DPROFILES="build;test;bench;doc;coverage;diagnostics;qa" -DCXX26_OK=1 -DCXX_COMPILER_ID=check \
        -DCXX_COMPILER_VERSION=check -P CMake/CheckDeps.cmake
  cmake --build build/release --target deps
}
step_format() {
  require_cmd clang-format
  cxx_sources | xargs clang-format --dry-run -Werror
}
step_shell() {
  require_cmd shellcheck
  shellcheck scripts/*.sh scripts/lib/*.sh
}
step_ci() { preset_build_test ci; }
step_debug() { preset_build_test debug; }
step_clang() { preset_build_test clang; }
step_tidy() {
  require_cmd run-clang-tidy
  [[ -f build/clang/compile_commands.json ]] || cmake --preset clang >/dev/null
  local out
  # Production code only (tests use gtest idioms that the checks do not target).
  out="$(run-clang-tidy -p build/clang -quiet -j"$JOBS" "^$PWD/(src|benchmarks)/" 2>/dev/null |
    grep -E '(warning|error):' | sort -u || true)"
  if [[ -n "$out" ]]; then
    printf '%s\n' "$out"
    return 1
  fi
}
step_tsan() { preset_build_test tsan; }
step_coverage() {
  cmake --preset coverage >/dev/null
  cmake --build build/coverage -j"$JOBS"
  cmake --build build/coverage --target coverage-reset
  cmake --build build/coverage --target tests
  cmake --build build/coverage --target coverage
}
step_doc() {
  cmake --preset release >/dev/null
  cmake --build build/release --target doc
}

main() {
  local one=""
  while getopts ':j:1:h' opt; do
    case "$opt" in
      j) JOBS="$OPTARG" ;;
      1) one="$OPTARG" ;;  # Internal: run a single step in this process.
      h) usage; exit "$E_OK" ;;
      :) die "option -$OPTARG needs a value" "$E_USAGE" ;;
      *) die "unknown option -$OPTARG (see -h)" "$E_USAGE" ;;
    esac
  done
  shift $((OPTIND - 1))
  if [[ -n "$one" ]]; then
    cd "$TSFS_REPO_ROOT"
    "step_$one"
    return
  fi
  local -a steps=("$@")
  ((${#steps[@]} > 0)) || steps=("${ALL_STEPS[@]}")
  local s
  local -a failed=()
  for s in "${steps[@]}"; do
    declare -F "step_$s" >/dev/null || die "unknown step '$s' (steps: ${ALL_STEPS[*]})" "$E_USAGE"
  done
  require_cmd cmake ctest
  cd "$TSFS_REPO_ROOT"
  for s in "${steps[@]}"; do
    log_step "qa: $s"
    # Each step runs in its own process with the strict mode of tsfs-common.sh: the
    # first failing command ends the step, the gate goes on with the next one.
    if "${BASH_SOURCE[0]}" -1 "$s" -j "$JOBS"; then
      log_ok "qa: $s passed"
    else
      log_error "qa: $s FAILED"
      failed+=("$s")
    fi
  done
  if ((${#failed[@]} > 0)); then
    die "failed step(s): ${failed[*]}" "$E_RUNTIME"
  fi
  log_ok "qa: every step passed (${steps[*]})"
}

main "$@"
