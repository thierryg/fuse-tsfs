// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/config/test_config.cpp
// Purpose : Configuration parsing/validation, serialization round trip, command-line parsing
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <vector>

#include "cli/command_line.hpp"
#include "config/config_loader.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

using namespace std::chrono_literals;

Result<Config> parse(std::string_view yaml) { return parse_config(yaml, "test.yml"); }

void expect_error(std::string_view yaml, std::string_view fragment) {
    const auto r = parse(yaml);
    ASSERT_FALSE(r.has_value()) << "accepted:\n" << yaml;
    EXPECT_NE(r.error().message.find(fragment), std::string::npos) << r.error().message;
    EXPECT_EQ(r.error().code, EINVAL);
}

TEST(ConfigLoader, MinimalDocumentUsesSafeDefaults) {
    const auto r = parse("version: 1\n");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->retention.default_retention.has_value());  // "never".
    EXPECT_TRUE(r->retention.rules.empty());
    EXPECT_EQ(r->filesystem.mountpoint, "/mnt/tsfs");
    EXPECT_EQ(r->logging.directory, "/var/log/tsfs");
    EXPECT_EQ(r->logging.filename, "tsfs.log");
    EXPECT_EQ(r->logging.rotation.max_file_size, 50ULL << 20);
    EXPECT_EQ(r->gc.scheduler, SchedulerKind::heap);
}

TEST(ConfigLoader, ShippedExampleIsValid) {
    const auto r = parse(example_configuration());
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->retention.rules.size(), 8U);
    EXPECT_TRUE(r->gc.dry_run);
}

TEST(ConfigLoader, FullRuleSet) {
    const auto r = parse(R"(
version: 1
retention:
  default: "7d"
  evaluation_timestamp: last_modified
  rules:
    - {id: rec, path: /tmp/record/, recursive: true, retention: 1d}
    - {id: mp4, glob: "*.mp4", retention: 1d}
    - {id: ext, extension: [".MP4", mkv], retention: 2h, priority: 5}
    - {id: one, file: /a/b, retention: never}
    - {id: sess, path_glob: "/s/session-*/**", retention: 12h, timestamp: birth}
    - {id: keep, action: exclude, path: /tmp/record/keep}
)");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    ASSERT_EQ(r->retention.rules.size(), 6U);
    EXPECT_EQ(*r->retention.default_retention, 168h);
    EXPECT_EQ(*r->retention.rules[0].path, "/tmp/record");  // Normalized.
    EXPECT_EQ(r->retention.rules[2].extensions, (std::vector<std::string>{"mp4", "mkv"}));
    EXPECT_FALSE(r->retention.rules[3].retention.has_value());
    EXPECT_EQ(r->retention.rules[4].timestamp, TimestampSource::birth);
    EXPECT_EQ(r->retention.rules[5].action, RuleAction::exclude);
}

TEST(ConfigLoader, RejectsInvalidYamlWithLocation) {
    expect_error("version: 1\nretention: [\n", "test.yml:");
    expect_error("version: 1\nretention: {default: \"1d\"\n", "invalid YAML");
}

TEST(ConfigLoader, RejectsMissingOrUnsupportedVersion) {
    expect_error("", "empty configuration");
    expect_error("filesystem: {}\n", "missing configuration version");
    expect_error("version: 2\n", "unsupported configuration version 2");
    expect_error("version: one\n", "expected an integer");
}

TEST(ConfigLoader, RejectsUnknownAndDuplicateKeys) {
    expect_error("version: 1\nretension: {}\n", "unknown key");
    expect_error("version: 1\ngarbage_collector: {max_worker: 2}\n", "garbage_collector.max_worker: unknown key");
    expect_error("version: 1\nversion: 1\n", "duplicate key");
}

TEST(ConfigLoader, ErrorsCarryLineAndColumn) {
    const auto r = parse("version: 1\ngarbage_collector:\n  scan_interval: \"10x\"\n");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("test.yml:3:18: garbage_collector.scan_interval"), std::string::npos)
        << r.error().message;
}

TEST(ConfigLoader, RejectsInvalidDurationsAndSizes) {
    expect_error("version: 1\nretention: {default: 7days}\n", "unknown unit");
    expect_error("version: 1\nretention: {default: -1d}\n", "expected a number");
    expect_error("version: 1\nretention: {default: 0s}\n", "below the minimum");
    expect_error("version: 1\nretention: {default: 99999999999999999999d}\n", "overflows");
    expect_error("version: 1\nlogging: {rotation: {max_file_size: 50XB}}\n", "unknown unit");
    expect_error("version: 1\nlogging: {rotation: {max_file_size: 1KiB}}\n", "out of range");
}

TEST(ConfigLoader, RejectsInvalidPaths) {
    expect_error("version: 1\nfilesystem: {mountpoint: relative/dir}\n", "must start with '/'");
    expect_error("version: 1\nfilesystem: {backing_directory: /var/../etc}\n", "'.' or '..'");
    expect_error("version: 1\nfilesystem: {mountpoint: /}\n", "may be '/'");
    expect_error("version: 1\nfilesystem: {mountpoint: /srv/a, backing_directory: /srv/a/data}\n",
                 "contain each other");
    expect_error("version: 1\nlogging: {filename: a/b}\n", "invalid file name");
    expect_error("version: 1\nfilesystem: {backing_directory: /data}\nlogging: {directory: /data/logs}\n",
                 "outside the backing directory");
}

TEST(ConfigLoader, RejectsInvalidRules) {
    expect_error("version: 1\nretention: {rules: [{retention: 1d, glob: '*'}]}\n", "missing rule identifier");
    expect_error("version: 1\nretention: {rules: [{id: 'bad id', glob: '*', retention: 1d}]}\n", "invalid identifier");
    expect_error("version: 1\nretention: {rules: [{id: a, retention: 1d}]}\n", "needs at least one of");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: '*.mp4'}]}\n", "missing retention");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: '[x', retention: 1d}]}\n", "unterminated");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: 'a/b', retention: 1d}]}\n", "cannot contain '/'");
    expect_error("version: 1\nretention: {rules: [{id: a, path_glob: 'rel/*', retention: 1d}]}\n",
                 "must start with '/'");
    expect_error("version: 1\nretention: {rules: [{id: a, file: /x, glob: '*', retention: 1d}]}\n",
                 "cannot be combined");
    expect_error("version: 1\nretention: {rules: [{id: a, path: /x, path_glob: '/x/*', retention: 1d}]}\n",
                 "cannot be combined");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: '*', recursive: true, retention: 1d}]}\n",
                 "only applies to a 'path' rule");
    expect_error("version: 1\nretention: {rules: [{id: a, action: exclude, glob: '*', retention: 1d}]}\n",
                 "neither 'retention' nor 'timestamp'");
    expect_error(
        "version: 1\nretention: {rules: [{id: a, glob: '*', retention: 1d}, {id: a, glob: 'b', retention: 1d}]}\n",
        "duplicate rule identifier");
    expect_error("version: 1\nretention: {rules: [{id: a, extension: '*.mp4', retention: 1d}]}\n", "invalid extension");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: '*', retention: 1d, priority: 5000}]}\n",
                 "out of range");
    expect_error("version: 1\nretention: {rules: [{id: a, glob: '*', retention: 1d, action: purge}]}\n", "allowed:");
}

TEST(ConfigLoader, RejectsInvalidGarbageCollectorSettings) {
    expect_error("version: 1\ngarbage_collector: {max_workers: 0}\n", "out of range");
    expect_error("version: 1\ngarbage_collector: {max_workers: -2}\n", "out of range");
    expect_error("version: 1\ngarbage_collector: {max_pending_tasks: 1}\n", "out of range");
    expect_error("version: 1\ngarbage_collector: {retry_initial_delay: 2m, retry_max_delay: 1m}\n",
                 "must not exceed retry_max_delay");
    expect_error("version: 1\ngarbage_collector: {batch_size: 500, max_pending_tasks: 100}\n",
                 "must not exceed max_pending_tasks");
    expect_error("version: 1\ngarbage_collector: {enabled: yes}\n", "expected true or false");
    expect_error("version: 1\ngarbage_collector: {scheduler: wheel}\n", "allowed: heap, ordered");
}

TEST(ConfigLoader, LoggingSettings) {
    const auto r = parse(R"(
version: 1
logging:
  file: /var/log/x/app.log
  level: warning
  format: json
  async: {queue_size: 1024, overflow_policy: discard_new}
  rotation: {max_file_size: 1MB, max_files: 3, compress: true, compression_level: 9}
)");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->logging.directory, "/var/log/x");
    EXPECT_EQ(r->logging.filename, "app.log");
    EXPECT_EQ(r->logging.level, LogLevel::warning);
    EXPECT_EQ(r->logging.format, LogFormat::json);
    EXPECT_EQ(r->logging.async.overflow_policy, LogOverflowPolicy::discard_new);
    EXPECT_EQ(r->logging.rotation.max_file_size, 1000000U);
    expect_error("version: 1\nlogging: {file: /a/b, directory: /c}\n", "not both");
    expect_error("version: 1\nlogging: {async: {overflow_policy: drop}}\n", "allowed:");
    expect_error("version: 1\nlogging: {rotation: {max_files: 0}}\n", "out of range");
    expect_error("version: 1\nlogging: {level: verbose}\n", "allowed:");
}

TEST(ConfigLoader, DumpRoundTripsToAnEquivalentConfiguration) {
    const auto original = parse(example_configuration());
    ASSERT_TRUE(original.has_value());
    const std::string dumped = dump_config(*original);
    const auto again = parse(dumped);
    ASSERT_TRUE(again.has_value()) << again.error().message << "\n" << dumped;
    EXPECT_EQ(dump_config(*again), dumped);
}

TEST(ConfigTrust, RootRequiresARootOwnedFileNotWritableByOthers) {
    struct stat st{};
    st.st_mode = S_IFREG | 0640;
    st.st_uid = 0;
    EXPECT_TRUE(check_config_file_trust(st, 0, "c.yml").has_value());
    st.st_uid = 1000;
    auto r = check_config_file_trust(st, 0, "c.yml");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, EACCES);
    EXPECT_NE(r.error().message.find("owned by root"), std::string::npos);
    st.st_uid = 0;
    st.st_mode = S_IFREG | 0664;
    r = check_config_file_trust(st, 0, "c.yml");
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("0664"), std::string::npos);
    st.st_mode = S_IFREG | 0646;
    EXPECT_FALSE(check_config_file_trust(st, 0, "c.yml").has_value());
}

TEST(ConfigTrust, UnprivilegedDaemonOnlyNeedsARegularFile) {
    struct stat st{};
    st.st_mode = S_IFREG | 0666;
    st.st_uid = 4242;
    EXPECT_TRUE(check_config_file_trust(st, 1000, "c.yml").has_value());
    st.st_mode = S_IFIFO | 0600;
    const auto r = check_config_file_trust(st, 1000, "c.yml");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, EINVAL);
}

TEST(ConfigOverrides, CommandLinePathsAreValidatedLikeTheFile) {
    Config c;
    c.filesystem.mountpoint = "/mnt/tsfs";
    c.filesystem.backing_directory = "/srv/data";
    c.logging.directory = "/var/log/tsfs";
    EXPECT_TRUE(validate_path_overrides(c).has_value());
    c.filesystem.mountpoint = "/srv/data/inside";
    EXPECT_FALSE(validate_path_overrides(c).has_value());
    c.filesystem.mountpoint = "/mnt/x/../y";
    EXPECT_FALSE(validate_path_overrides(c).has_value());
    c.filesystem.mountpoint = "/";
    EXPECT_FALSE(validate_path_overrides(c).has_value());
    c.filesystem.mountpoint = "/mnt/tsfs";
    c.logging.directory = "/srv/data/logs";
    const auto r = validate_path_overrides(c);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, EINVAL);
}

TEST(ConfigLoader, MissingFileReportsErrno) {
    const auto r = load_config_file("/nonexistent/tsfs/config.yml");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ENOENT);
}

// --- command line ------------------------------------------------------------------

Result<CliOptions> cli(std::vector<std::string> args) {
    args.insert(args.begin(), "tsfs");
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    return parse_command_line(static_cast<int>(args.size()), argv.data());
}

TEST(CommandLine, DefaultsToRunWithDefaultConfig) {
    const auto r = cli({});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->action, CliAction::run);
    EXPECT_EQ(r->config_path, kDefaultConfigPath);
    EXPECT_FALSE(r->foreground);
}

TEST(CommandLine, ActionsAndOverrides) {
    auto r = cli({"--config", "/x.yml", "--check-config", "-m", "/m", "-b", "/b", "-l", "warning"});
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->action, CliAction::check_config);
    EXPECT_EQ(r->config_path, "/x.yml");
    EXPECT_EQ(*r->overrides.mountpoint, "/m");
    EXPECT_EQ(*r->overrides.backing_directory, "/b");
    EXPECT_EQ(*r->overrides.log_level, LogLevel::warning);
    EXPECT_EQ(cli({"--dry-run"})->action, CliAction::dry_run);
    EXPECT_EQ(cli({"--show-config"})->action, CliAction::show_config);
    EXPECT_EQ(cli({"--status"})->action, CliAction::status);
    EXPECT_EQ(cli({"-V"})->action, CliAction::version);
    EXPECT_EQ(cli({"--help"})->action, CliAction::help);
}

TEST(CommandLine, DebugImpliesForegroundDebugLevelAndConsole) {
    const auto r = cli({"-d"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->foreground);
    EXPECT_EQ(*r->overrides.log_level, LogLevel::debug);
    EXPECT_TRUE(*r->overrides.console);
    EXPECT_EQ(*cli({"-l", "trace", "-d"})->overrides.log_level, LogLevel::trace);  // Explicit level kept.
}

TEST(CommandLine, DiagnosticsOptions) {
    const auto r = cli({"--stack-trace", "--core-dump"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(*r->overrides.diagnostics_enabled);
    EXPECT_TRUE(*r->overrides.text_stack_trace);
    EXPECT_TRUE(*r->overrides.native_core_dump);
}

TEST(CommandLine, PurgeStatsAndPreview) {
    auto r = cli({"--purge"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->action, CliAction::purge);
    EXPECT_FALSE(r->force);
    EXPECT_FALSE(r->purge_preview);
    r = cli({"--purge", "--force"});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->force);
    for (const auto& args : std::vector<std::vector<std::string>>{{"--purge", "--dry-run"}, {"--dry-run", "--purge"}}) {
        r = cli(args);
        ASSERT_TRUE(r.has_value()) << args.front();
        EXPECT_EQ(r->action, CliAction::purge);
        EXPECT_TRUE(r->purge_preview);
    }
    r = cli({"--stats", "-c", "/etc/x.yml"});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->action, CliAction::stats);
}

TEST(CommandLine, UsageErrors) {
    for (const auto& args : std::vector<std::vector<std::string>>{{"--bogus"},
                                                                  {"-c"},
                                                                  {"--log-level", "loud"},
                                                                  {"--check-config", "--dry-run"},
                                                                  {"extra"},
                                                                  {"--dry-run", "--foreground"},
                                                                  {"--status", "--core-dump"},
                                                                  {"--debug", "--check-config"},
                                                                  {"--force"},
                                                                  {"--stats", "--force"},
                                                                  {"--purge", "--dry-run", "--force"},
                                                                  {"--purge", "--stats"},
                                                                  {"--version", "--help"}}) {
        const auto r = cli(args);
        ASSERT_FALSE(r.has_value()) << args.front();
        EXPECT_EQ(r.error().code, kExitUsage);
        EXPECT_NE(r.error().message.find("tsfs --help"), std::string::npos);
    }
}

TEST(CommandLine, HelpDocumentsEveryLongOption) {
    const std::string help = usage_text();
    for (const char* option :
         {"--config", "--mountpoint", "--backing-dir", "--log-level", "--foreground", "--debug", "--fuse-debug",
          "--check-config", "--show-config", "--dry-run", "--status", "--print-example-config", "--version", "--help",
          "--stack-trace", "--no-stack-trace", "--core-dump", "--no-core-dump"}) {
        EXPECT_NE(help.find(option), std::string::npos) << option;
        EXPECT_TRUE(cli({option}).has_value() || std::string(option) == "--config" ||
                    std::string(option) == "--mountpoint" || std::string(option) == "--backing-dir" ||
                    std::string(option) == "--log-level")
            << option;
    }
}

}  // namespace
}  // namespace tsfs
