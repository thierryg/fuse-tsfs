# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Uninstall.cmake
# Purpose : Remove the files recorded in install_manifest.txt, and nothing else
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Usage (normally through the target):
#   cmake -DMANIFEST=<build>/install_manifest.txt -DCONFIG_PATH=/etc/tsfs/config.yml -P Uninstall.cmake
# Honors $DESTDIR like "cmake --install".
#
# Safety rules:
# - only paths listed in the manifest are touched, and only regular files or
#   symbolic links (a directory, device or anything else listed there is refused);
# - the active configuration (CONFIG_PATH), the managed data, the logs, the crash
#   reports and the state directory are never in the manifest, so they are kept;
# - directories are not removed (they may be shared or hold user data), except the
#   TSFS documentation directory when it ends up empty;
# - a missing file is reported and skipped (idempotent).
# Limitation: the manifest lives in the build tree; once that tree is deleted, run
# "cmake --install" again from a fresh build with the same options to recreate it,
# then uninstall (see docs/user/user-guide.md, "Uninstallation").

if(NOT MANIFEST)
    message(FATAL_ERROR "usage: cmake -DMANIFEST=<install_manifest.txt> [-DCONFIG_PATH=<path>] -P Uninstall.cmake")
endif()
if(NOT EXISTS "${MANIFEST}")
    message(FATAL_ERROR "uninstall: ${MANIFEST} not found: nothing was installed from this build tree "
                        "(or the manifest was removed). Install once from this tree to recreate it.")
endif()

file(STRINGS "${MANIFEST}" _files)
set(_removed 0)
set(_missing 0)
set(_doc_dirs "")
foreach(_f IN LISTS _files)
    if(_f STREQUAL "")
        continue()
    endif()
    set(_path "$ENV{DESTDIR}${_f}")
    if(CONFIG_PATH AND _f STREQUAL CONFIG_PATH)
        message(STATUS "Keeping the configuration ${_path}")
        continue()
    endif()
    if(IS_SYMLINK "${_path}" OR (EXISTS "${_path}" AND NOT IS_DIRECTORY "${_path}"))
        file(REMOVE "${_path}")
        if(EXISTS "${_path}" OR IS_SYMLINK "${_path}")
            message(FATAL_ERROR "uninstall: cannot remove ${_path} (permissions? use the same privileges as for the install)")
        endif()
        message(STATUS "Removed ${_path}")
        math(EXPR _removed "${_removed} + 1")
        if(_f MATCHES "^(.*/doc/tsfs)/")
            list(APPEND _doc_dirs "$ENV{DESTDIR}${CMAKE_MATCH_1}")
        endif()
    elseif(IS_DIRECTORY "${_path}")
        message(WARNING "uninstall: refusing to remove the directory ${_path} listed in the manifest")
    else()
        message(STATUS "Already absent: ${_path}")
        math(EXPR _missing "${_missing} + 1")
    endif()
endforeach()

# The system ls: rename <ls>.orig back to <ls> if the wrapper is in place (never with
# DESTDIR).
if(SYSTEM_LS AND "$ENV{DESTDIR}" STREQUAL "")
    set(_orig "${SYSTEM_LS}.orig")
    set(_ours "")
    if(EXISTS "${SYSTEM_LS}" AND NOT IS_SYMLINK "${SYSTEM_LS}")
        file(STRINGS "${SYSTEM_LS}" _ours REGEX "TSFS-LS-WRAPPER" LIMIT_COUNT 1)
    endif()
    if(_ours AND (EXISTS "${_orig}" OR IS_SYMLINK "${_orig}"))
        file(RENAME "${_orig}" "${SYSTEM_LS}")
        message(STATUS "Restored the system ls: ${_orig} -> ${SYSTEM_LS}")
    elseif(_ours)
        message(WARNING "uninstall: ${SYSTEM_LS} is still the TSFS wrapper and ${_orig} is missing: "
                        "restore it with 'apt install --reinstall coreutils'")
    elseif(EXISTS "${_orig}" OR IS_SYMLINK "${_orig}")
        # coreutils reinstalled the system ls over the wrapper meanwhile: ls.orig is stale.
        file(REMOVE "${_orig}")
        message(STATUS "${SYSTEM_LS} is already the system ls; removed the stale ${_orig}")
    endif()
endif()

# Remove the (now empty) documentation tree of TSFS, deepest directories first.
list(REMOVE_DUPLICATES _doc_dirs)
foreach(_doc IN LISTS _doc_dirs)
    if(IS_DIRECTORY "${_doc}")
        file(GLOB_RECURSE _left LIST_DIRECTORIES false "${_doc}/*")
        if(_left STREQUAL "")
            file(REMOVE_RECURSE "${_doc}")
            message(STATUS "Removed the empty directory ${_doc}")
        endif()
    endif()
endforeach()

message(STATUS "uninstall: ${_removed} file(s) removed, ${_missing} already absent")
if(CONFIG_PATH)
    message(STATUS "Kept: $ENV{DESTDIR}${CONFIG_PATH} and every data, log and state directory "
                   "(remove them manually if they are no longer needed)")
endif()
message(STATUS "If the service was enabled: systemctl disable --now tsfs.service && systemctl daemon-reload")
