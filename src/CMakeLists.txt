# =============================================================================
# TSFS - Time-aware Storage Filesystem for Linux
# -----------------------------------------------------------------------------
# File    : src/CMakeLists.txt
# Purpose : tsfs_core library (every module) and the tsfs executable
# Author  : Thierry Gayet <thierry.gayet@labworks.fr>
# Project : tsfs (version: VERSION)
# Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
# =============================================================================
configure_file(version.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/generated/tsfs/version.hpp @ONLY)

# Every module in one static library: the executable, the tests and the benchmarks
# link the same objects (the tests exercise exactly the production code).
add_library(tsfs_core STATIC
    app/application.cpp
    cli/command_line.cpp
    common/escape.cpp
    common/managed_path.cpp
    common/threads.cpp
    common/units.cpp
    config/config_loader.cpp
    config/config_manager.cpp
    config/config_watcher.cpp
    diagnostics/crash_handler.cpp
    diagnostics/symbol_table.cpp
    filesystem/fuse_session.cpp
    filesystem/passthrough_fs.cpp
    fsutil/backing_store.cpp
    fsutil/entry_stat.cpp
    gc/garbage_collector.cpp
    gc/scheduler.cpp
    gc/tree_scanner.cpp
    logging/log_compressor.cpp
    logging/log_field.cpp
    logging/logger.cpp
    logging/notice_queue.cpp
    logging/rotating_file_sink.cpp
    metrics/status_report.cpp
    policy/glob.cpp
    policy/policy_engine.cpp
)
target_include_directories(tsfs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(tsfs_core PUBLIC
    FUSE_USE_VERSION=314
    TSFS_DEFAULT_CONFIG_PATH="${TSFS_DEFAULT_CONFIG_PATH}"
    TSFS_DEFAULT_LOG_LEVEL=$<IF:$<CONFIG:Debug>,"debug","info">)
# #embed (C++26) resolves "config/config.example.yml" from the source root.
set_source_files_properties(cli/command_line.cpp PROPERTIES
    COMPILE_OPTIONS "--embed-dir=${PROJECT_SOURCE_DIR}"
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/config/config.example.yml")
target_link_libraries(tsfs_core
    PUBLIC tsfs_options PkgConfig::FUSE3 ${TSFS_YAML_TARGET} spdlog::spdlog ZLIB::ZLIB Threads::Threads)

add_executable(tsfs main.cpp)
target_link_libraries(tsfs PRIVATE tsfs_core)
