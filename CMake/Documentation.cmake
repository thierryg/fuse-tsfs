# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Documentation.cmake
# Purpose : "doc" target: Doxygen HTML (API, diagrams, Markdown documentation)
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# cmake --build <tree> --target doc  ->  <tree>/docs/html/index.html
# Inputs: src/ (API), README.md (main page) and docs/**/*.md. Graphviz (dot) adds
# class, collaboration, include and call graphs; without it the HTML is still
# generated, without diagrams (a warning says so). Doxygen warnings are written to
# <tree>/docs/doxygen-warnings.log and their count is printed: the target fails
# only when Doxygen itself fails, never silently hides warnings.
# Reproducible: no timestamps in the pages (TIMESTAMP = NO), sorted inputs.

find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)

set(TSFS_DOC_DIR "${CMAKE_BINARY_DIR}/docs")

if(NOT DOXYGEN_FOUND)
    add_custom_target(doc
        COMMAND ${CMAKE_COMMAND} -E echo "doc: Doxygen is not installed (package: doxygen; optional: graphviz)"
        COMMAND ${CMAKE_COMMAND} -E false
        VERBATIM)
    return()
endif()

if(TARGET Doxygen::dot)
    set(TSFS_DOXYGEN_HAVE_DOT YES)
    get_target_property(TSFS_DOXYGEN_DOT_PATH Doxygen::dot IMPORTED_LOCATION)
    get_filename_component(TSFS_DOXYGEN_DOT_PATH "${TSFS_DOXYGEN_DOT_PATH}" DIRECTORY)
else()
    set(TSFS_DOXYGEN_HAVE_DOT NO)
    set(TSFS_DOXYGEN_DOT_PATH "")
    message(STATUS "TSFS: Graphviz (dot) not found, the documentation will have no diagrams")
endif()

configure_file(${PROJECT_SOURCE_DIR}/docs/Doxyfile.in ${TSFS_DOC_DIR}/Doxyfile @ONLY)

add_custom_target(doc
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${TSFS_DOC_DIR}/html
    COMMAND ${DOXYGEN_EXECUTABLE} ${TSFS_DOC_DIR}/Doxyfile
    COMMAND ${CMAKE_COMMAND} -DLOG=${TSFS_DOC_DIR}/doxygen-warnings.log
            -DINDEX=${TSFS_DOC_DIR}/html/index.html -P ${PROJECT_SOURCE_DIR}/CMake/DoxygenReport.cmake
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating the Doxygen HTML documentation in ${TSFS_DOC_DIR}/html"
    VERBATIM)
