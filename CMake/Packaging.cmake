# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : CMake/Packaging.cmake
# Purpose : Install rules and the manifest-based "uninstall" target
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
# Installed (GNUInstallDirs, prefix CMAKE_INSTALL_PREFIX, default /usr/local):
#   bin/tsfs                                   the daemon and its CLI (0755)
#   share/man/man1/tsfs.1                      manual page
#   share/doc/tsfs/                            README, CHANGELOG, LICENSE, user and security docs
#   share/doc/tsfs/examples/config.example.yml the commented example (always refreshed)
#   <TSFS_SYSTEMD_UNIT_DIR>/tsfs.service       systemd unit (not enabled): /etc/tsfs/config.yml
#   <TSFS_SYSTEMD_UNIT_DIR>/tsfs@.service      template unit: one filesystem per /etc/tsfs/NAME.yml
#   <dir of TSFS_DEFAULT_CONFIG_PATH>/config.yml   ONLY if absent: copy of the example
# The active configuration is created by install(CODE), outside the install
# manifest, so that "uninstall" never removes it. An existing configuration is never
# overwritten. No data, log or state directory is created: systemd creates
# /var/lib/tsfs and /var/log/tsfs (StateDirectory=, LogsDirectory=).
#
# Root is needed only for the install step into system directories:
#   cmake --build build/release && sudo cmake --install build/release
# Staged installation (packaging, tests) needs no privilege:
#   DESTDIR=/tmp/stage cmake --install build/release

set(TSFS_SYSTEMD_UNIT_DIR "" CACHE PATH
    "Where tsfs.service is installed (default: systemd's systemdsystemunitdir, else lib/systemd/system)")
if(NOT TSFS_SYSTEMD_UNIT_DIR)
    pkg_get_variable(_systemd_dir systemd systemdsystemunitdir)
    if(_systemd_dir)
        set(TSFS_SYSTEMD_UNIT_DIR "${_systemd_dir}")
    else()
        set(TSFS_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
    endif()
endif()

set(TSFS_CONFIG_PATH "${TSFS_DEFAULT_CONFIG_PATH}")
get_filename_component(TSFS_CONFIG_DIR "${TSFS_CONFIG_PATH}" DIRECTORY)
set(TSFS_INSTALL_FULL_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
set(TSFS_INSTALL_FULL_DOCDIR "${CMAKE_INSTALL_FULL_DOCDIR}")
configure_file(${PROJECT_SOURCE_DIR}/systemd/tsfs.service.in ${CMAKE_BINARY_DIR}/systemd/tsfs.service @ONLY)
configure_file(${PROJECT_SOURCE_DIR}/systemd/tsfs@.service.in ${CMAKE_BINARY_DIR}/systemd/tsfs@.service @ONLY)

install(TARGETS tsfs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
install(FILES ${PROJECT_SOURCE_DIR}/docs/man/tsfs.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 COMPONENT runtime)
install(FILES ${CMAKE_BINARY_DIR}/systemd/tsfs.service ${CMAKE_BINARY_DIR}/systemd/tsfs@.service
        DESTINATION ${TSFS_SYSTEMD_UNIT_DIR} COMPONENT runtime)
install(FILES ${PROJECT_SOURCE_DIR}/config/config.example.yml
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples COMPONENT runtime)
install(FILES ${PROJECT_SOURCE_DIR}/README.md ${PROJECT_SOURCE_DIR}/CHANGELOG.md ${PROJECT_SOURCE_DIR}/LICENSE
              ${PROJECT_SOURCE_DIR}/SECURITY.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/user ${PROJECT_SOURCE_DIR}/docs/security
        DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime FILES_MATCHING PATTERN "*.md")

# The active configuration: created from the example when (and only when) absent.
# Mode 0640: it names directories and policies, and may be read by an admin group.
install(CODE "
    set(_dest \"\$ENV{DESTDIR}${TSFS_CONFIG_PATH}\")
    if(EXISTS \"\${_dest}\")
        message(STATUS \"Keeping the existing configuration: \${_dest}\")
    else()
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${TSFS_CONFIG_DIR}\")
        file(COPY_FILE \"${PROJECT_SOURCE_DIR}/config/config.example.yml\" \"\${_dest}\" ONLY_IF_DIFFERENT)
        file(CHMOD \"\${_dest}\" FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ)
        message(STATUS \"Created the configuration from the example: \${_dest} (garbage_collector.dry_run: true)\")
    endif()
    message(STATUS \"Next: review \${_dest}, then 'systemctl daemon-reload && systemctl enable --now tsfs.service'\")
" COMPONENT runtime)

# uninstall: removes exactly the files listed in install_manifest.txt (written by the
# last "cmake --install" of this tree). See CMake/Uninstall.cmake for the safety rules.
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
            -DCONFIG_PATH=${TSFS_CONFIG_PATH} -P ${PROJECT_SOURCE_DIR}/CMake/Uninstall.cmake
    COMMENT "Uninstalling TSFS (files of ${CMAKE_BINARY_DIR}/install_manifest.txt)"
    USES_TERMINAL
    VERBATIM)
