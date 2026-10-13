#!/usr/bin/env bash
# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : scripts/lib/tsfs-common.sh
# Purpose : Shared bash library: colors, logging, error handling, exit codes, checks
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
#
# Sourced (never executed) by every TSFS shell script:
#
#   SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
#   # shellcheck source=lib/tsfs-common.sh
#   . "$SCRIPT_DIR/lib/tsfs-common.sh"
#
# It provides:
#   * strict mode (set -Eeuo pipefail) and an ERR trap that reports the failing
#     command, file and line, then exits with the command status;
#   * colorized log helpers (log_info, log_ok, log_warn, log_error, log_step,
#     log_debug) on stderr, disabled when stderr is not a terminal, when NO_COLOR
#     is set (https://no-color.org) or with TSFS_COLOR=never;
#   * die MESSAGE [CODE]: log an error and exit with a documented code;
#   * requirement checks: require_cmd, require_file;
#   * cleanup registration (on_exit) run by the EXIT trap in LIFO order.
#
# Standard exit codes (shared by every script and by the tsfs binary):
#   0  E_OK        success
#   1  E_RUNTIME   unexpected runtime failure (a command failed)
#   2  E_USAGE     invalid arguments / usage error
#   3  E_PRIV      insufficient privileges
#   4  E_DEPS      missing prerequisite (command, package, file)
#   5  E_CONFIG    invalid configuration or input data
#   7  E_STATE     unexpected system state (check failed, resource busy)
#   8  E_WARN      completed with warnings
#   130            interrupted (SIGINT)
#
# Environment variables:
#   NO_COLOR / TSFS_COLOR=always|never|auto   color control
#   TSFS_DEBUG=1                              enable log_debug output and xtrace

[[ -n "${_TSFS_COMMON_SH:-}" ]] && return 0
readonly _TSFS_COMMON_SH=1

set -Eeuo pipefail
shopt -s inherit_errexit 2>/dev/null || true

# shellcheck disable=SC2034
readonly E_OK=0 E_RUNTIME=1 E_USAGE=2 E_PRIV=3 E_DEPS=4 E_CONFIG=5 E_STATE=7 E_WARN=8

SCRIPT_NAME="$(basename -- "${0}")"
# Root of the repository (scripts/lib/../..), valid when run from a checkout.
TSFS_REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

# setup_colors: define the color variables according to the terminal and the
# NO_COLOR / TSFS_COLOR conventions. Called once at source time.
setup_colors() {
  local mode="${TSFS_COLOR:-auto}"
  if [[ "$mode" == "never" || -n "${NO_COLOR:-}" ]] || { [[ "$mode" == "auto" ]] && [[ ! -t 2 ]]; }; then
    C_RESET="" C_BOLD="" C_DIM="" C_RED="" C_GREEN="" C_YELLOW="" C_BLUE="" C_CYAN=""
  else
    C_RESET=$'\e[0m' C_BOLD=$'\e[1m' C_DIM=$'\e[2m' C_RED=$'\e[31m' C_GREEN=$'\e[32m'
    C_YELLOW=$'\e[33m' C_BLUE=$'\e[34m' C_CYAN=$'\e[36m'
  fi
  export C_RESET C_BOLD C_DIM C_RED C_GREEN C_YELLOW C_BLUE C_CYAN
}
setup_colors

# _log LEVEL COLOR MESSAGE...: internal formatter "HH:MM:SS LEVEL script: message".
_log() {
  local level="$1" color="$2"
  shift 2
  printf '%s%s%s %s%-5s%s %s%s:%s %s\n' "$C_DIM" "$(date +%H:%M:%S)" "$C_RESET" \
    "$color$C_BOLD" "$level" "$C_RESET" "$C_DIM" "$SCRIPT_NAME" "$C_RESET" "$*" >&2
}
log_info()  { _log INFO "$C_BLUE" "$@"; }
log_ok()    { _log OK "$C_GREEN" "$@"; }
log_warn()  { _log WARN "$C_YELLOW" "$@"; }
log_error() { _log ERROR "$C_RED" "$@"; }
log_debug() { [[ "${TSFS_DEBUG:-0}" == 1 ]] && _log DEBUG "$C_DIM" "$@"; return 0; }
# log_step MESSAGE: highlighted section title.
log_step()  { printf '\n%s==>%s %s%s%s\n' "$C_CYAN$C_BOLD" "$C_RESET" "$C_BOLD" "$*" "$C_RESET" >&2; }

# die MESSAGE [CODE]: log an error and exit (default code E_RUNTIME).
die() {
  local msg="$1" code="${2:-$E_RUNTIME}"
  log_error "$msg"
  exit "$code"
}

_TSFS_CLEANUPS=()
# on_exit COMMAND: register a cleanup command run at exit (last registered first).
on_exit() { _TSFS_CLEANUPS+=("$1"); }

# _tsfs_exit_trap: run the registered cleanups; the exit status given to "exit" is kept.
_tsfs_exit_trap() {
  local i
  trap - ERR
  set +e
  for ((i = ${#_TSFS_CLEANUPS[@]} - 1; i >= 0; i--)); do
    eval "${_TSFS_CLEANUPS[i]}"
  done
  return 0
}

# _tsfs_err_trap: report the failing command with its location, then exit with its status.
_tsfs_err_trap() {
  local status=$? cmd="${BASH_COMMAND}" line="${BASH_LINENO[0]}" src="${BASH_SOURCE[1]:-$SCRIPT_NAME}"
  log_error "command failed (status ${status}) at $(basename -- "$src"):${line}: ${cmd}"
  exit "$status"
}

trap _tsfs_exit_trap EXIT
trap _tsfs_err_trap ERR
trap 'log_warn "interrupted"; exit 130' INT TERM
[[ "${TSFS_DEBUG:-0}" == 1 ]] && set -x

# require_cmd CMD...: exit with E_DEPS if one of the commands is not in PATH.
require_cmd() {
  local c missing=()
  for c in "$@"; do command -v "$c" >/dev/null 2>&1 || missing+=("$c"); done
  ((${#missing[@]} == 0)) || die "missing required command(s): ${missing[*]}" "$E_DEPS"
}

# require_file PATH...: exit with E_DEPS if a file does not exist or is unreadable.
require_file() {
  local f
  for f in "$@"; do [[ -r "$f" ]] || die "required file not found or unreadable: $f" "$E_DEPS"; done
}

# is_true VALUE: success for 1/yes/true/on (case-insensitive).
is_true() { [[ "${1,,}" =~ ^(1|y|yes|true|on)$ ]]; }

# tsfs_version: print the software version (single source VERSION at the repository root).
tsfs_version() {
  local f="$TSFS_REPO_ROOT/VERSION"
  if [[ -r "$f" ]]; then tr -d '[:space:]' <"$f"; echo; else echo "unknown"; fi
}
