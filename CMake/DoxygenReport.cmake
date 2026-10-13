# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/DoxygenReport.cmake
# Purpose : Summarize the Doxygen warning log after the "doc" target
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Usage: cmake -DLOG=<doxygen-warnings.log> -DINDEX=<html/index.html> -P DoxygenReport.cmake
# Fails when no HTML was produced; prints the warnings (at most 50) and their count.

if(NOT EXISTS "${INDEX}")
    message(FATAL_ERROR "doc: Doxygen produced no ${INDEX}")
endif()
set(_count 0)
if(EXISTS "${LOG}")
    file(STRINGS "${LOG}" _warnings REGEX "warning:")
    list(LENGTH _warnings _count)
    if(_count GREATER 0)
        list(SUBLIST _warnings 0 50 _shown)
        foreach(_w IN LISTS _shown)
            message(STATUS "  ${_w}")
        endforeach()
    endif()
endif()
message(STATUS "doc: ${INDEX} (${_count} Doxygen warning(s), full list: ${LOG})")
