// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/app/application.hpp
// Purpose : Entry point logic: one-shot actions and the mounted daemon lifecycle
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include "cli/command_line.hpp"

namespace tsfs {

/**
 * @brief Execute the action selected on the command line.
 *
 * One-shot actions (--check-config, --show-config, --dry-run, --status,
 * --print-example-config, --version, --help) print to stdout/stderr and return.
 * The default action mounts the filesystem and runs until unmounted or terminated
 * (SIGINT/SIGTERM); see docs/architecture/overview.md for the startup and shutdown
 * sequences.
 *
 * @return a process exit code (ExitCode).
 */
int run_application(const CliOptions& options);

}  // namespace tsfs
