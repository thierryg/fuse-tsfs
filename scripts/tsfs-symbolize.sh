#!/usr/bin/env bash
# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : scripts/tsfs-symbolize.sh
# Purpose : Symbolize the raw backtrace of a TSFS crash report (addr2line)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
#
# A crash report (diagnostics.crash_directory/tsfs-crash-<time>-<pid>.txt) holds the
# raw return addresses of the crashing thread and a copy of /proc/self/maps. This
# script maps every address to its module and file offset, then asks addr2line for
# the function, file and line (inlined frames included).
#
# Usage:
#   scripts/tsfs-symbolize.sh [-m MODULE=FILE]... REPORT
#     -m  use FILE (e.g. an unstripped binary or a .debug file of the same build)
#         for the module whose path in the report is MODULE; repeatable
#
# Requirements: addr2line (binutils). Symbols come from the binaries themselves, so
# the same build must still be available (a Release build has function names only;
# RelWithDebInfo or Debug give files and lines).
#
# Exit codes: 0 success, 2 usage error, 4 missing dependency, 5 not a crash report.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/tsfs-common.sh
. "$SCRIPT_DIR/lib/tsfs-common.sh"

declare -A SUBSTITUTE=()

usage() {
  sed -n '12,/^SCRIPT_DIR=/p' "${BASH_SOURCE[0]}" | sed -e '$d' -e 's/^# \{0,1\}//'
}

parse_args() {
  while getopts ':m:h' opt; do
    case "$opt" in
      m) [[ "$OPTARG" == *=* ]] || die "-m takes MODULE=FILE" "$E_USAGE"
         SUBSTITUTE["${OPTARG%%=*}"]="${OPTARG#*=}" ;;
      h) usage; exit "$E_OK" ;;
      :) die "option -$OPTARG needs a value" "$E_USAGE" ;;
      *) die "unknown option -$OPTARG (see -h)" "$E_USAGE" ;;
    esac
  done
  shift $((OPTIND - 1))
  (($# == 1)) || die "exactly one crash report expected (see -h)" "$E_USAGE"
  REPORT="$1"
  require_file "$REPORT"
}

# symbolize_frame INDEX ADDRESS: print "#NN 0xADDR module+0xOFF  function at file:line".
symbolize_frame() {
  local index="$1" address="$2" line start end offset module rel file
  # A return address points after the call: look up the call instruction itself.
  local lookup=$((address - (index > 0 ? 1 : 0)))
  while read -r line; do
    start=$((16#${line%%-*}))
    end=${line#*-}
    end=$((16#${end%% *}))
    if ((lookup >= start && lookup < end)); then
      offset=$(awk '{print $3}' <<<"$line")
      module=$(awk '{print $6}' <<<"$line")
      rel=$((lookup - start + 16#$offset))
      file="${SUBSTITUTE[$module]:-$module}"
      if [[ -n "$module" && -r "$file" ]]; then
        printf '#%02d 0x%x %s+0x%x  %s\n' "$index" "$address" "$module" "$rel" \
          "$(addr2line -e "$file" -f -C -i -p "$(printf '0x%x' "$rel")" 2>/dev/null | paste -sd ';' -)"
      else
        printf '#%02d 0x%x %s+0x%x  (module not readable here)\n' "$index" "$address" "${module:-?}" "$rel"
      fi
      return
    fi
  done <<<"$MAPS"
  printf '#%02d 0x%x  (not in any mapping of the report)\n' "$index" "$address"
}

main() {
  parse_args "$@"
  require_cmd addr2line awk
  grep -q '^backtrace (raw addresses):' "$REPORT" ||
    die "$REPORT is not a TSFS crash report (no raw backtrace section)" "$E_CONFIG"
  MAPS="$(sed -n '/^memory map (\/proc\/self\/maps):/,/^end of report/p' "$REPORT" | grep -E '^[0-9a-f]+-[0-9a-f]+ ')"
  [[ -n "$MAPS" ]] || die "$REPORT has no memory map section" "$E_CONFIG"
  grep -E '^(signal|pid|time|version):' "$REPORT" || true
  local index address
  while read -r index address; do
    symbolize_frame "$((10#${index#\#}))" "$((address))"
  done < <(sed -n '/^backtrace (raw addresses):/,/^$/p' "$REPORT" | grep -E '^ +#[0-9]+ 0x[0-9a-f]+$')
}

main "$@"
