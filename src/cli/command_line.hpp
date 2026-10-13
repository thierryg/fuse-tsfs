// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/cli/command_line.hpp
// Purpose : Command-line parsing, usage text and exit codes of the tsfs executable
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <string>
#include <string_view>

#include "common/error.hpp"
#include "config/config_manager.hpp"

namespace tsfs {

/// Exit codes shared with the scripts (scripts/lib/tsfs-common.sh).
enum ExitCode : int {
    kExitOk = 0,
    kExitRuntime = 1,
    kExitUsage = 2,
    kExitPrivileges = 3,
    kExitDependencies = 4,
    kExitConfig = 5,
    kExitState = 7,
};

/// What the invocation asks for (mutually exclusive).
enum class CliAction {
    run,
    check_config,
    show_config,
    dry_run,
    status,
    purge,
    stats,
    print_example_config,
    version,
    help
};

/// Parsed command line.
struct CliOptions {
    CliAction action{CliAction::run};
    std::string config_path{kDefaultConfigPath};
    bool foreground{false};
    bool debug{false};          ///< --debug: foreground, level debug, console logging.
    bool fuse_debug{false};     ///< --fuse-debug: libfuse request trace (foreground).
    bool force{false};          ///< --force: --purge without confirmation.
    bool purge_preview{false};  ///< --purge --dry-run: list what a purge would delete.
    ConfigOverrides overrides;
};

/**
 * @brief Parse @p argv. Never exits, never prints.
 * @return the options, or an Error whose code is kExitUsage and whose message
 *         explains the problem (unknown option, missing argument, two actions,
 *         an option that has no meaning for the chosen action...).
 */
Result<CliOptions> parse_command_line(int argc, char** argv);

/// The --help text (kept in sync with the parser by the CLI tests).
std::string usage_text();
/// The --version text: version, compiler, C++ standard, libfuse, build type.
std::string version_text();
/// config/config.example.yml, embedded at build time with #embed.
std::string_view example_configuration() noexcept;

}  // namespace tsfs
