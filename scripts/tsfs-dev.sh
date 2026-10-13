#!/usr/bin/env bash
# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : scripts/tsfs-dev.sh
# Purpose : Run a command (or a shell) inside the C++26 development container
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
#
# TSFS is written in C++26 and needs GCC >= 15 or Clang >= 21 with libstdc++ >= 15,
# libfuse >= 3.14 and CMake >= 3.28. On an older host (e.g. Ubuntu 22.04/24.04) this
# script provides the toolchain through the tsfs-dev:26.04 image (docker/Dockerfile.dev),
# built on first use. The repository is bind-mounted at /src; the container runs as
# the host uid/gid, so build trees keep the host ownership. Nothing is installed on
# the host.
#
# The FUSE integration tests need /dev/fuse and CAP_SYS_ADMIN inside the container:
# they are granted by default (--no-fuse disables them; the FUSE tests then skip).
#
# Usage:
#   scripts/tsfs-dev.sh [--rebuild] [--no-fuse] [--no-aslr] [--] [COMMAND [ARG...]]
#   scripts/tsfs-dev.sh --help
#     --rebuild   rebuild the image even if it exists
#     --no-fuse   do not pass /dev/fuse nor CAP_SYS_ADMIN
#     --no-aslr   run COMMAND with ASLR disabled (setarch -R), required by the tsan
#                 preset on kernels with vm.mmap_rnd_bits > 28; implies an
#                 unconfined seccomp profile (Docker's blocks that personality flag)
#   Without COMMAND an interactive shell is started.
#
# Examples:
#   scripts/tsfs-dev.sh cmake --preset debug
#   scripts/tsfs-dev.sh cmake --build --preset debug --target tests
#   scripts/tsfs-dev.sh --no-aslr ctest --test-dir build/tsan
#
# Environment variables:
#   TSFS_DEV_IMAGE   image name (default tsfs-dev:26.04)
#   Forwarded to the container when set: TSFS_TEST_ENABLE_CORE_DUMPS,
#   TSFS_TEST_ARTIFACT_DIR, TSFS_TEST_KEEP_ARTIFACTS, TSFS_STRESS_SCALE,
#   TSFS_BENCH_LARGE, TSFS_BENCH_DIR (paths must be valid inside the container:
#   the repository is /src)
#
# Exit codes:
#   status of COMMAND, or
#   2  usage error
#   4  docker not available
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tsfs-common.sh
. "$SCRIPT_DIR/lib/tsfs-common.sh"

IMAGE="${TSFS_DEV_IMAGE:-tsfs-dev:26.04}"
REBUILD=0
FUSE=1
NO_ASLR=0
COMMAND=()

# usage: print the help text (the header comment block).
usage() {
  sed -n '12,/^SCRIPT_DIR=/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//'
}

# parse_args ARGS...: read the options; everything after them is the command.
parse_args() {
  while (($#)); do
    case "$1" in
      --rebuild) REBUILD=1; shift ;;
      --no-fuse) FUSE=0; shift ;;
      --no-aslr) NO_ASLR=1; shift ;;
      -h | --help) usage; exit "$E_OK" ;;
      --) shift; COMMAND=("$@"); return ;;
      -*) die "unknown option: $1 (see --help)" "$E_USAGE" ;;
      *) COMMAND=("$@"); return ;;
    esac
  done
}

# ensure_image: build the development image when missing (or with --rebuild).
ensure_image() {
  if ((REBUILD)) || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log_step "building $IMAGE"
    docker build -t "$IMAGE" --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" \
      -f "$TSFS_REPO_ROOT/docker/Dockerfile.dev" "$TSFS_REPO_ROOT/docker"
  fi
}

main() {
  parse_args "$@"
  require_cmd docker
  docker info >/dev/null 2>&1 || die "docker is not usable by $(id -un)" "$E_DEPS"
  ensure_image
  local -a args=(run --rm -i --init -v "$TSFS_REPO_ROOT:/src" -w /src
                 -e TSFS_TEST_ENABLE_CORE_DUMPS -e TSFS_TEST_ARTIFACT_DIR -e TSFS_TEST_KEEP_ARTIFACTS
                 -e TSFS_STRESS_SCALE -e TSFS_BENCH_LARGE -e TSFS_BENCH_DIR
                 --ulimit core=-1)
  [[ -t 0 ]] && args+=(-t)
  if ((FUSE)); then
    args+=(--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor=unconfined)
  fi
  if ((${#COMMAND[@]} == 0)); then
    COMMAND=(bash)
  fi
  if ((NO_ASLR)); then
    args+=(--security-opt seccomp=unconfined)
    COMMAND=(setarch -R "${COMMAND[@]}")
  fi
  exec docker "${args[@]}" "$IMAGE" "${COMMAND[@]}"
}

main "$@"
