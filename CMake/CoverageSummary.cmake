# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/CoverageSummary.cmake
# Purpose : Machine-readable coverage summary (JSON) and list of uncovered lines from an lcov .info
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Usage: cmake -DINFO=<tsfs.info> -DOUT=<dir> -P CMake/CoverageSummary.cmake
# Writes <dir>/summary.json (totals and per-file line/branch/function counts) and
# <dir>/uncovered.txt ("file: line line line..." for every file with unexecuted lines).

if(NOT INFO OR NOT OUT)
    message(FATAL_ERROR "usage: cmake -DINFO=<file.info> -DOUT=<dir> -P CoverageSummary.cmake")
endif()

file(STRINGS "${INFO}" _lines)
set(_json_files "")
set(_uncovered "")
set(_tot_lf 0)
set(_tot_lh 0)
set(_tot_brf 0)
set(_tot_brh 0)
set(_tot_fnf 0)
set(_tot_fnh 0)
foreach(_l IN LISTS _lines)
    if(_l MATCHES "^SF:(.*)$")
        set(_file "${CMAKE_MATCH_1}")
        set(_lf 0)
        set(_lh 0)
        set(_brf 0)
        set(_brh 0)
        set(_fnf 0)
        set(_fnh 0)
        set(_missed "")
    elseif(_l MATCHES "^DA:([0-9]+),0")
        string(APPEND _missed " ${CMAKE_MATCH_1}")
    elseif(_l MATCHES "^LF:([0-9]+)$")
        set(_lf ${CMAKE_MATCH_1})
    elseif(_l MATCHES "^LH:([0-9]+)$")
        set(_lh ${CMAKE_MATCH_1})
    elseif(_l MATCHES "^BRF:([0-9]+)$")
        set(_brf ${CMAKE_MATCH_1})
    elseif(_l MATCHES "^BRH:([0-9]+)$")
        set(_brh ${CMAKE_MATCH_1})
    elseif(_l MATCHES "^FNF:([0-9]+)$")
        set(_fnf ${CMAKE_MATCH_1})
    elseif(_l MATCHES "^FNH:([0-9]+)$")
        set(_fnh ${CMAKE_MATCH_1})
    elseif(_l STREQUAL "end_of_record")
        math(EXPR _tot_lf "${_tot_lf} + ${_lf}")
        math(EXPR _tot_lh "${_tot_lh} + ${_lh}")
        math(EXPR _tot_brf "${_tot_brf} + ${_brf}")
        math(EXPR _tot_brh "${_tot_brh} + ${_brh}")
        math(EXPR _tot_fnf "${_tot_fnf} + ${_fnf}")
        math(EXPR _tot_fnh "${_tot_fnh} + ${_fnh}")
        if(NOT _json_files STREQUAL "")
            string(APPEND _json_files ",\n")
        endif()
        string(APPEND _json_files "    {\"file\": \"${_file}\", \"lines\": [${_lh}, ${_lf}], "
                                  "\"branches\": [${_brh}, ${_brf}], \"functions\": [${_fnh}, ${_fnf}]}")
        if(NOT _missed STREQUAL "")
            string(APPEND _uncovered "${_file}:${_missed}\n")
        endif()
    endif()
endforeach()

macro(_pct out hit total)
    if(${total} EQUAL 0)
        set(${out} "100.0")
    else()
        math(EXPR _p10 "(${hit} * 1000) / ${total}")
        math(EXPR _int "${_p10} / 10")
        math(EXPR _dec "${_p10} % 10")
        set(${out} "${_int}.${_dec}")
    endif()
endmacro()
_pct(_lpct ${_tot_lh} ${_tot_lf})
_pct(_bpct ${_tot_brh} ${_tot_brf})
_pct(_fpct ${_tot_fnh} ${_tot_fnf})

file(WRITE "${OUT}/summary.json"
     "{\n  \"lines\": {\"hit\": ${_tot_lh}, \"total\": ${_tot_lf}, \"percent\": ${_lpct}},\n"
     "  \"branches\": {\"hit\": ${_tot_brh}, \"total\": ${_tot_brf}, \"percent\": ${_bpct}},\n"
     "  \"functions\": {\"hit\": ${_tot_fnh}, \"total\": ${_tot_fnf}, \"percent\": ${_fpct}},\n"
     "  \"files\": [\n${_json_files}\n  ]\n}\n")
file(WRITE "${OUT}/uncovered.txt" "${_uncovered}")
message(STATUS "coverage: lines ${_lpct}% (${_tot_lh}/${_tot_lf}), branches ${_bpct}% (${_tot_brh}/${_tot_brf}), "
               "functions ${_fpct}% (${_tot_fnh}/${_tot_fnf})")
message(STATUS "coverage: ${OUT}/summary.json, ${OUT}/uncovered.txt, ${OUT}/html/index.html")
