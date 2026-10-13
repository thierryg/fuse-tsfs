#!/usr/bin/env bash
# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : scripts/tsfs-bench-fs.sh
# Purpose : Filesystem benchmark: raw backing directory vs TSFS mount (GC off / on)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
#
# Runs tsfs_fs_workload three times on the same storage:
#   raw      directly in the backing directory (baseline, no FUSE)
#   gc-off   through a TSFS mount, garbage_collector.enabled: false
#   gc-on    through a TSFS mount, GC enabled with a retention that matches every
#            file but never expires during the run (scan_interval 1s): the scans
#            and the change notifications run, no file is deleted under the workload
# and prints the JSON lines of each run, plus an environment header (CPU, memory,
# kernel, compiler, libfuse, backing filesystem). Everything happens in a private
# work directory (removed at the end); the mount is always unmounted on exit.
#
# Usage:
#   scripts/tsfs-bench-fs.sh [-B BUILD_DIR] [-w WORK_DIR] [-n FILES] [-t THREADS] [-s LARGE_MIB]
#     -B  build tree with src/tsfs and benchmarks/tsfs_fs_workload (default build/release)
#     -w  parent of the work directory: selects the storage under test (default $TMPDIR or /tmp)
#     -n  small files per operation (default 10000)
#     -t  threads of the concurrent phase (default 4)
#     -s  size of the sequential file in MiB (default 256)
#
# Requirements: /dev/fuse and fusermount3 (inside the dev container:
#   scripts/tsfs-dev.sh scripts/tsfs-bench-fs.sh).
#
# Exit codes: 0 success, 1 runtime error, 2 usage error, 4 missing dependency.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tsfs-common.sh
. "$SCRIPT_DIR/lib/tsfs-common.sh"

BUILD_DIR="$TSFS_REPO_ROOT/build/release"
WORK_PARENT="${TMPDIR:-/tmp}"
FILES=10000
THREADS=4
LARGE_MIB=256

usage() {
  sed -n '12,/^SCRIPT_DIR=/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//'
}

parse_args() {
  while getopts ':B:w:n:t:s:h' opt; do
    case "$opt" in
      B) BUILD_DIR="$OPTARG" ;;
      w) WORK_PARENT="$OPTARG" ;;
      n) FILES="$OPTARG" ;;
      t) THREADS="$OPTARG" ;;
      s) LARGE_MIB="$OPTARG" ;;
      h) usage; exit "$E_OK" ;;
      :) die "option -$OPTARG needs a value" "$E_USAGE" ;;
      *) die "unknown option -$OPTARG (see -h)" "$E_USAGE" ;;
    esac
  done
  [[ "$FILES" =~ ^[1-9][0-9]*$ && "$THREADS" =~ ^[1-9][0-9]*$ && "$LARGE_MIB" =~ ^[1-9][0-9]*$ ]] ||
    die "-n, -t and -s take positive integers" "$E_USAGE"
}

# print_environment: what the numbers depend on (recorded with every run).
print_environment() {
  local fs_type
  # findmnt names ext4 correctly (stat -f reports "ext2/ext3" for the whole family).
  fs_type="$(findmnt -n -o FSTYPE --target "$WORK" 2>/dev/null || stat -f -c %T "$WORK")"
  printf '# date: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '# cpu: %s (%s threads)\n' "$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)" "$(nproc)"
  printf '# memory: %s\n' "$(awk '/MemTotal/ {printf "%.1f GiB", $2 / 1048576}' /proc/meminfo)"
  printf '# kernel: %s\n' "$(uname -r)"
  printf '# tsfs: %s\n' "$("$TSFS" --version | head -1)"
  printf '# compiler/build: %s\n' "$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p; s/^CMAKE_BUILD_TYPE:[^=]*=/build type /p' \
    "$BUILD_DIR/CMakeCache.txt" | paste -sd ' ')"
  printf '# libfuse: %s\n' "$(pkg-config --modversion fuse3 2>/dev/null || echo unknown)"
  printf '# backing filesystem: %s (%s)\n' "$fs_type" "$(df --output=source "$WORK" | tail -1)"
  printf '# parameters: files=%s threads=%s large_mib=%s\n' "$FILES" "$THREADS" "$LARGE_MIB"
}

# write_config FILE GC_ENABLED
write_config() {
  cat >"$1" <<EOF
version: 1
filesystem: {mountpoint: "$WORK/mnt", backing_directory: "$WORK/data"}
retention:
  default: never
  rules:
    - {id: all, path: /, recursive: true, retention: 1d}
garbage_collector: {enabled: $2, dry_run: false, scan_interval: 1s, max_deletions_per_second: 0}
logging: {directory: "$WORK/log", level: warning}
metrics: {status_file: "$WORK/status.json", interval: 1s}
EOF
}

# unmount_if_mounted: lazy-free unmount of the work mount (EXIT handler).
unmount_if_mounted() {
  if mountpoint -q "$WORK/mnt" 2>/dev/null; then
    fusermount3 -u "$WORK/mnt" || log_warn "cannot unmount $WORK/mnt"
  fi
  if [[ -n "${TSFS_PID:-}" ]]; then
    wait "$TSFS_PID" 2>/dev/null || true
    TSFS_PID=""
  fi
}

# run_mounted LABEL GC_ENABLED
run_mounted() {
  write_config "$WORK/config.yml" "$2"
  "$TSFS" -c "$WORK/config.yml" -f &
  TSFS_PID=$!
  local _
  for _ in $(seq 50); do
    mountpoint -q "$WORK/mnt" && break
    sleep 0.1
  done
  mountpoint -q "$WORK/mnt" || die "the TSFS mount did not appear (see $WORK/log)" "$E_RUNTIME"
  "$WORKLOAD" --dir "$WORK/mnt" --files "$FILES" --threads "$THREADS" --large-mib "$LARGE_MIB" --label "$1"
  unmount_if_mounted
}

main() {
  parse_args "$@"
  require_cmd fusermount3
  require_cmd mountpoint
  TSFS="$BUILD_DIR/src/tsfs"
  WORKLOAD="$BUILD_DIR/benchmarks/tsfs_fs_workload"
  [[ -x "$TSFS" && -x "$WORKLOAD" ]] ||
    die "build $BUILD_DIR first (cmake --build $BUILD_DIR): src/tsfs and benchmarks/tsfs_fs_workload" "$E_DEPS"
  [[ -c /dev/fuse ]] || die "/dev/fuse is not available" "$E_DEPS"

  WORK="$(mktemp -d "$WORK_PARENT/tsfs-bench-fs.XXXXXX")"
  on_exit "rm -rf -- '$WORK'"
  on_exit unmount_if_mounted
  mkdir -p "$WORK/data" "$WORK/mnt" "$WORK/log"

  print_environment
  "$WORKLOAD" --dir "$WORK/data" --files "$FILES" --threads "$THREADS" --large-mib "$LARGE_MIB" --label raw
  run_mounted gc-off false
  run_mounted gc-on true
}

main "$@"
