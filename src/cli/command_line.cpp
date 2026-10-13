// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/cli/command_line.cpp
// Purpose : Command-line parsing, usage text and exit codes of the tsfs executable
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "cli/command_line.hpp"

#include <fuse.h>
#include <getopt.h>

#include <format>
#include <optional>

#include "tsfs/version.hpp"

namespace tsfs {
namespace {

// C++26 #embed: the reference example is compiled into the binary, so
// "tsfs --print-example-config" always matches the version that parses it.
// Clang 21 still reports #embed as a C23 extension in C++ under -Wpedantic (it is
// standard C++26, P1967): silenced for this declaration only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
constexpr unsigned char kExampleConfig[] = {
#embed "config/config.example.yml"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

enum LongOnly : int {
    kCheckConfig = 1000,
    kShowConfig,
    kDryRun,
    kStatus,
    kPrintExample,
    kFuseDebug,
    kStackTrace,
    kNoStackTrace,
    kCoreDump,
    kNoCoreDump,
    kPurge,
    kForce,
    kStats,
};

const char* action_name(CliAction action) {
    switch (action) {
        case CliAction::run: return "run";
        case CliAction::check_config: return "--check-config";
        case CliAction::show_config: return "--show-config";
        case CliAction::dry_run: return "--dry-run";
        case CliAction::status: return "--status";
        case CliAction::purge: return "--purge";
        case CliAction::stats: return "--stats";
        case CliAction::print_example_config: return "--print-example-config";
        case CliAction::version: return "--version";
        case CliAction::help: return "--help";
    }
    return "?";
}

}  // namespace

std::string_view example_configuration() noexcept {
    return {reinterpret_cast<const char*>(kExampleConfig), sizeof(kExampleConfig)};
}

std::string usage_text() {
    return std::format(R"(Usage: tsfs [OPTIONS]

TSFS mounts a directory through FUSE and deletes files once their retention
period has elapsed, according to the rules of the configuration file.

Actions (at most one; default: mount and run):
      --check-config         validate the configuration and exit (0 = valid, 5 = invalid)
      --show-config          print the effective configuration (defaults included) and exit
      --dry-run              scan the backing directory once, list the files that would be
                             deleted now (same checks as the real GC, except files open
                             through a running mount, unknown here), delete nothing, exit
      --status               print the status file of the running daemon (metrics.status_file)
      --stats                scan the backing directory once and print, per rule, the number
                             of files and bytes, expired, expiring within 24 h, excluded, never
      --purge                delete now every file that is already expired (rate limits
                             ignored, same checks as the GC; excluded and "never" files are
                             kept); done by the running daemon (SIGUSR2) or, when none runs,
                             by this process. Asks for confirmation; with --dry-run: only list
      --force                with --purge: do not ask for confirmation (scripts)
      --print-example-config print the commented example configuration
  -V, --version              print version information
  -h, --help                 print this help

Configuration:
  -c, --config PATH          configuration file (default: {})
  -m, --mountpoint PATH      override filesystem.mountpoint
  -b, --backing-dir PATH     override filesystem.backing_directory
  -l, --log-level LEVEL      override logging.level (trace, debug, info, warning, error, critical)

Running (default action only):
  -f, --foreground           do not detach from the terminal
  -d, --debug                foreground, log level debug, log also to stderr
      --fuse-debug           print every FUSE request (libfuse -d, implies --foreground)
      --stack-trace          enable diagnostics with text crash reports
      --no-stack-trace       disable text crash reports
      --core-dump            enable diagnostics with native core dumps
      --no-core-dump         do not change the core-dump limits

Command-line values override the file for the whole life of the process,
including after a reload (SIGHUP or file change).

Signals of the daemon: SIGHUP reload, SIGUSR1 write the status file, SIGUSR2 purge
expired files, SIGTERM/SIGINT unmount and exit.

Exit codes: 0 success, 1 runtime error, 2 usage error, 3 privileges,
            4 missing dependency, 5 invalid configuration, 7 unexpected system state.
)",
                       kDefaultConfigPath);
}

std::string version_text() {
    return std::format("tsfs {}\nbuild: {} ({}), C++ {}L\nlibfuse: built with {}.{}, running {}\n", kVersion,
                       kBuildType, kCompiler, __cplusplus, FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION, fuse_pkgversion());
}

Result<CliOptions> parse_command_line(int argc, char** argv) {
    static constexpr option kLong[] = {
        {"config", required_argument, nullptr, 'c'},
        {"mountpoint", required_argument, nullptr, 'm'},
        {"backing-dir", required_argument, nullptr, 'b'},
        {"log-level", required_argument, nullptr, 'l'},
        {"foreground", no_argument, nullptr, 'f'},
        {"debug", no_argument, nullptr, 'd'},
        {"version", no_argument, nullptr, 'V'},
        {"help", no_argument, nullptr, 'h'},
        {"check-config", no_argument, nullptr, kCheckConfig},
        {"show-config", no_argument, nullptr, kShowConfig},
        {"dry-run", no_argument, nullptr, kDryRun},
        {"status", no_argument, nullptr, kStatus},
        {"print-example-config", no_argument, nullptr, kPrintExample},
        {"fuse-debug", no_argument, nullptr, kFuseDebug},
        {"stack-trace", no_argument, nullptr, kStackTrace},
        {"no-stack-trace", no_argument, nullptr, kNoStackTrace},
        {"core-dump", no_argument, nullptr, kCoreDump},
        {"no-core-dump", no_argument, nullptr, kNoCoreDump},
        {"purge", no_argument, nullptr, kPurge},
        {"force", no_argument, nullptr, kForce},
        {"stats", no_argument, nullptr, kStats},
        {nullptr, 0, nullptr, 0},
    };
    CliOptions options;
    std::optional<CliAction> action;
    bool run_only_option = false;
    const auto usage = [](std::string message) {
        return make_error(kExitUsage, std::move(message) + " (see tsfs --help)");
    };
    CliAction attempted = CliAction::run;
    const auto set_action = [&](CliAction next) -> bool {
        attempted = next;
        if (action && *action != next) {
            return false;
        }
        action = next;
        return true;
    };
    ::optind = 0;  // 0 (not 1) fully re-initializes glibc's getopt between calls.
    ::opterr = 0;  // Errors are reported by the caller, not printed by getopt.
    int c = 0;
    while ((c = ::getopt_long(argc, argv, ":c:m:b:l:fdVh", kLong, nullptr)) != -1) {
        bool ok = true;
        switch (c) {
            case 'c': options.config_path = ::optarg; break;
            case 'm': options.overrides.mountpoint = ::optarg; break;
            case 'b': options.overrides.backing_directory = ::optarg; break;
            case 'l': {
                const auto level = parse_log_level(::optarg);
                if (!level) {
                    return usage(std::format("invalid log level '{}'", ::optarg));
                }
                options.overrides.log_level = level;
                break;
            }
            case 'f':
                options.foreground = true;
                run_only_option = true;
                break;
            case 'd':
                options.debug = true;
                options.foreground = true;
                run_only_option = true;
                options.overrides.log_level = options.overrides.log_level.value_or(LogLevel::debug);
                options.overrides.console = true;
                break;
            case 'V': ok = set_action(CliAction::version); break;
            case 'h': ok = set_action(CliAction::help); break;
            case kCheckConfig: ok = set_action(CliAction::check_config); break;
            case kShowConfig: ok = set_action(CliAction::show_config); break;
            case kDryRun:
                if (action == CliAction::purge) {
                    options.purge_preview = true;  // --purge --dry-run: preview.
                } else {
                    ok = set_action(CliAction::dry_run);
                }
                break;
            case kPurge:
                if (action == CliAction::dry_run) {
                    action = CliAction::purge;  // --dry-run --purge: preview.
                    options.purge_preview = true;
                } else {
                    ok = set_action(CliAction::purge);
                }
                break;
            case kForce: options.force = true; break;
            case kStats: ok = set_action(CliAction::stats); break;
            case kStatus: ok = set_action(CliAction::status); break;
            case kPrintExample: ok = set_action(CliAction::print_example_config); break;
            case kFuseDebug:
                options.fuse_debug = true;
                options.foreground = true;
                run_only_option = true;
                break;
            case kStackTrace:
                options.overrides.diagnostics_enabled = true;
                options.overrides.text_stack_trace = true;
                run_only_option = true;
                break;
            case kNoStackTrace:
                options.overrides.text_stack_trace = false;
                run_only_option = true;
                break;
            case kCoreDump:
                options.overrides.diagnostics_enabled = true;
                options.overrides.native_core_dump = true;
                run_only_option = true;
                break;
            case kNoCoreDump:
                options.overrides.native_core_dump = false;
                run_only_option = true;
                break;
            case ':': return usage(std::format("option '{}' needs an argument", argv[::optind - 1]));
            default: return usage(std::format("unknown option '{}'", argv[::optind - 1]));
        }
        if (!ok) {
            return usage(std::format("{} cannot be combined with {}", action_name(*action), action_name(attempted)));
        }
    }
    if (::optind < argc) {
        return usage(std::format("unexpected argument '{}'", argv[::optind]));
    }
    options.action = action.value_or(CliAction::run);
    if (options.force && (options.action != CliAction::purge || options.purge_preview)) {
        return usage("--force only applies to --purge (without --dry-run)");
    }
    if (options.action != CliAction::run && run_only_option) {
        return usage(
            std::format("--foreground, --debug, --fuse-debug and the diagnostics options only apply when "
                        "mounting, not with {}",
                        action_name(options.action)));
    }
    return options;
}

}  // namespace tsfs
