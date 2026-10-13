// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/config/config_loader.cpp
// Purpose : YAML parsing, strict schema validation and serialization of the configuration
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "config/config_loader.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>

#include "common/managed_path.hpp"
#include "common/unique_fd.hpp"
#include "policy/glob.hpp"

namespace tsfs {

const char* to_string(TimestampSource value) noexcept {
    return value == TimestampSource::birth ? "birth" : "modification";
}
const char* to_string(RuleAction value) noexcept { return value == RuleAction::exclude ? "exclude" : "delete"; }
const char* to_string(LogLevel value) noexcept {
    switch (value) {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info";
        case LogLevel::warning: return "warning";
        case LogLevel::error: return "error";
        case LogLevel::critical: return "critical";
    }
    return "info";
}
const char* to_string(LogFormat value) noexcept { return value == LogFormat::json ? "json" : "text"; }
const char* to_string(LogOverflowPolicy value) noexcept {
    switch (value) {
        case LogOverflowPolicy::block: return "block";
        case LogOverflowPolicy::overrun_oldest: return "overrun_oldest";
        case LogOverflowPolicy::discard_new: return "discard_new";
    }
    return "block";
}
const char* to_string(SchedulerKind value) noexcept { return value == SchedulerKind::ordered ? "ordered" : "heap"; }

std::optional<LogLevel> parse_log_level(std::string_view text) noexcept {
    for (const LogLevel level :
         {LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warning, LogLevel::error, LogLevel::critical}) {
        if (text == to_string(level)) {
            return level;
        }
    }
    return std::nullopt;
}

namespace {

/// Internal validation failure; converted into a Result at the API boundary.
struct ConfigException {
    std::string message;
};

/// Walks the YAML tree with a key path for error messages ("garbage_collector.max_workers").
class Reader {
public:
    explicit Reader(std::string source) : source_(std::move(source)) {}

    [[noreturn]] void fail(const YAML::Node& node, const std::string& key, const std::string& reason) const {
        std::ostringstream out;
        out << source_;
        if (node.IsDefined() && node.Mark().line >= 0) {
            out << ':' << node.Mark().line + 1 << ':' << node.Mark().column + 1;
        }
        out << ": " << key << ": " << reason;
        throw ConfigException{out.str()};
    }

    /// Check that @p node is a mapping whose keys all belong to @p allowed and are unique.
    void expect_map(const YAML::Node& node, const std::string& key, std::initializer_list<const char*> allowed) const {
        if (!node.IsMap()) {
            fail(node, key, "expected a mapping");
        }
        std::set<std::string> seen;
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (!it->first.IsScalar()) {
                fail(it->first, key, "keys must be strings");
            }
            const std::string name = it->first.Scalar();
            if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return name == a; })) {
                std::string list;
                for (const char* a : allowed) {
                    list += list.empty() ? "" : ", ";
                    list += a;
                }
                fail(it->first, join(key, name), "unknown key (allowed: " + list + ")");
            }
            if (!seen.insert(name).second) {
                fail(it->first, join(key, name), "duplicate key");
            }
        }
    }

    std::string scalar(const YAML::Node& node, const std::string& key) const {
        if (!node.IsScalar()) {
            fail(node, key, "expected a scalar value");
        }
        return node.Scalar();
    }

    bool boolean(const YAML::Node& node, const std::string& key) const {
        const std::string v = scalar(node, key);
        if (v == "true") {
            return true;
        }
        if (v == "false") {
            return false;
        }
        fail(node, key, "expected true or false, got '" + v + "'");
    }

    template <typename T>
    T integer(const YAML::Node& node, const std::string& key, T min, T max) const {
        const std::string v = scalar(node, key);
        long long parsed = 0;
        const auto* begin = v.data();
        const auto* end = v.data() + v.size();
        const auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end || v.empty()) {
            fail(node, key, "expected an integer, got '" + v + "'");
        }
        // Every bound used by the schema fits in a long long, so the comparison is exact.
        if (parsed < static_cast<long long>(min) || parsed > static_cast<long long>(max)) {
            fail(node, key, "value " + v + " out of range [" + std::to_string(min) + ", " + std::to_string(max) + "]");
        }
        return static_cast<T>(parsed);
    }

    Duration duration(const YAML::Node& node, const std::string& key, Duration min) const {
        const std::string v = scalar(node, key);
        auto parsed = parse_duration(v);
        if (!parsed) {
            fail(node, key, parsed.error().message);
        }
        if (*parsed < min) {
            fail(node, key, "duration '" + v + "' is below the minimum " + format_duration(min));
        }
        return *parsed;
    }

    /// "never" or a duration of at least one second.
    std::optional<Duration> retention(const YAML::Node& node, const std::string& key) const {
        const std::string v = scalar(node, key);
        if (v == "never") {
            return std::nullopt;
        }
        return duration(node, key, std::chrono::seconds(1));
    }

    std::uint64_t size(const YAML::Node& node, const std::string& key, std::uint64_t min, std::uint64_t max) const {
        const std::string v = scalar(node, key);
        auto parsed = parse_size(v);
        if (!parsed) {
            fail(node, key, parsed.error().message);
        }
        if (*parsed < min || *parsed > max) {
            fail(node, key, "size '" + v + "' out of range [" + format_size(min) + ", " + format_size(max) + "]");
        }
        return *parsed;
    }

    /// An absolute host path (no "." or ".." component, normalized).
    std::string host_path(const YAML::Node& node, const std::string& key) const {
        const std::string v = scalar(node, key);
        auto normalized = managed_path::normalize(v);
        if (!normalized) {
            fail(node, key, normalized.error().message);
        }
        return *normalized;
    }

    /// A path relative to the TSFS root, written "/dir/sub".
    std::string tsfs_path(const YAML::Node& node, const std::string& key) const { return host_path(node, key); }

    template <typename Enum>
    Enum choice(const YAML::Node& node, const std::string& key,
                std::initializer_list<std::pair<const char*, Enum>> choices) const {
        const std::string v = scalar(node, key);
        std::string list;
        for (const auto& [name, value] : choices) {
            if (v == name) {
                return value;
            }
            list += list.empty() ? "" : ", ";
            list += name;
        }
        fail(node, key, "invalid value '" + v + "' (allowed: " + list + ")");
    }

    static std::string join(const std::string& parent, const std::string& child) {
        return parent.empty() ? child : parent + "." + child;
    }

private:
    std::string source_;
};

template <typename Fn>
void with(const YAML::Node& map, const char* name, Fn&& fn) {
    const YAML::Node child = map[name];
    if (child.IsDefined() && !child.IsNull()) {
        fn(child);
    }
}

TimestampSource parse_timestamp(const Reader& r, const YAML::Node& node, const std::string& key) {
    return r.choice<TimestampSource>(node, key,
                                     {{"modification", TimestampSource::modification},
                                      {"last_modified", TimestampSource::modification},
                                      {"birth", TimestampSource::birth},
                                      {"creation", TimestampSource::birth}});
}

void parse_filesystem(const Reader& r, const YAML::Node& node, FilesystemConfig& fs) {
    const std::string k = "filesystem";
    r.expect_map(node, k, {"mountpoint", "backing_directory", "mode", "allow_other", "read_only", "retention_xattrs"});
    with(node, "mountpoint", [&](const YAML::Node& n) { fs.mountpoint = r.host_path(n, k + ".mountpoint"); });
    with(node, "backing_directory",
         [&](const YAML::Node& n) { fs.backing_directory = r.host_path(n, k + ".backing_directory"); });
    with(node, "mode", [&](const YAML::Node& n) {
        fs.mode = r.choice<FilesystemMode>(n, k + ".mode", {{"passthrough", FilesystemMode::passthrough}});
    });
    with(node, "allow_other", [&](const YAML::Node& n) { fs.allow_other = r.boolean(n, k + ".allow_other"); });
    with(node, "read_only", [&](const YAML::Node& n) { fs.read_only = r.boolean(n, k + ".read_only"); });
    with(node, "retention_xattrs",
         [&](const YAML::Node& n) { fs.retention_xattrs = r.boolean(n, k + ".retention_xattrs"); });
    if (fs.mountpoint == "/" || fs.backing_directory == "/") {
        r.fail(node, k, "neither the mount point nor the backing directory may be '/'");
    }
    if (managed_path::is_within(fs.mountpoint, fs.backing_directory) ||
        managed_path::is_within(fs.backing_directory, fs.mountpoint)) {
        r.fail(node, k, "the mount point and the backing directory must not contain each other");
    }
}

bool valid_rule_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
               c == '.';
    });
}

std::string normalize_extension(const Reader& r, const YAML::Node& node, const std::string& key) {
    std::string ext = r.scalar(node, key);
    if (!ext.empty() && ext.front() == '.') {
        ext.erase(0, 1);
    }
    if (ext.empty() || ext.find_first_of("/*?[]\\") != std::string::npos || ext.front() == '.' || ext.back() == '.') {
        r.fail(node, key, "invalid extension '" + r.scalar(node, key) + "' (expected e.g. \"mp4\" or \"tar.gz\")");
    }
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return ext;
}

RuleConfig parse_rule(const Reader& r, const YAML::Node& node, const std::string& key) {
    r.expect_map(node, key,
                 {"id", "enabled", "action", "priority", "file", "path", "recursive", "glob", "path_glob", "extension",
                  "retention", "timestamp"});
    RuleConfig rule;
    rule.line = node.Mark().line + 1;
    const YAML::Node id = node["id"];
    if (!id.IsDefined() || id.IsNull()) {
        r.fail(node, key + ".id", "missing rule identifier");
    }
    rule.id = r.scalar(id, key + ".id");
    if (!valid_rule_id(rule.id)) {
        r.fail(id, key + ".id", "invalid identifier '" + rule.id + "' (1 to 64 characters among A-Z a-z 0-9 . _ -)");
    }
    const std::string rk = "rules[" + rule.id + "]";
    with(node, "enabled", [&](const YAML::Node& n) { rule.enabled = r.boolean(n, rk + ".enabled"); });
    with(node, "action", [&](const YAML::Node& n) {
        rule.action =
            r.choice<RuleAction>(n, rk + ".action", {{"delete", RuleAction::expire}, {"exclude", RuleAction::exclude}});
    });
    with(node, "priority",
         [&](const YAML::Node& n) { rule.priority = r.integer<int>(n, rk + ".priority", -1000, 1000); });
    with(node, "file", [&](const YAML::Node& n) { rule.file = r.tsfs_path(n, rk + ".file"); });
    with(node, "path", [&](const YAML::Node& n) { rule.path = r.tsfs_path(n, rk + ".path"); });
    with(node, "recursive", [&](const YAML::Node& n) { rule.recursive = r.boolean(n, rk + ".recursive"); });
    with(node, "glob", [&](const YAML::Node& n) {
        rule.glob = r.scalar(n, rk + ".glob");
        auto compiled = Glob::compile(*rule.glob, Glob::Mode::basename);
        if (!compiled) {
            r.fail(n, rk + ".glob", compiled.error().message);
        }
    });
    with(node, "path_glob", [&](const YAML::Node& n) {
        rule.path_glob = r.scalar(n, rk + ".path_glob");
        auto compiled = Glob::compile(*rule.path_glob, Glob::Mode::path);
        if (!compiled) {
            r.fail(n, rk + ".path_glob", compiled.error().message);
        }
    });
    with(node, "extension", [&](const YAML::Node& n) {
        if (n.IsSequence()) {
            if (n.size() == 0) {
                r.fail(n, rk + ".extension", "empty extension list");
            }
            for (std::size_t i = 0; i < n.size(); ++i) {
                rule.extensions.push_back(normalize_extension(r, n[i], rk + ".extension[" + std::to_string(i) + "]"));
            }
        } else {
            rule.extensions.push_back(normalize_extension(r, n, rk + ".extension"));
        }
    });
    with(node, "retention", [&](const YAML::Node& n) { rule.retention = r.retention(n, rk + ".retention"); });
    with(node, "timestamp", [&](const YAML::Node& n) { rule.timestamp = parse_timestamp(r, n, rk + ".timestamp"); });

    const bool has_scope_or_pattern = rule.file || rule.path || rule.glob || rule.path_glob || !rule.extensions.empty();
    if (!has_scope_or_pattern) {
        r.fail(node, rk, "a rule needs at least one of: file, path, glob, path_glob, extension");
    }
    if (rule.file && (rule.path || rule.glob || rule.path_glob || !rule.extensions.empty())) {
        r.fail(node, rk, "'file' cannot be combined with path, glob, path_glob or extension");
    }
    if (rule.path_glob && rule.path) {
        r.fail(node, rk, "'path_glob' cannot be combined with 'path' (put the directory in the pattern)");
    }
    if (node["recursive"].IsDefined() && !rule.path) {
        r.fail(node["recursive"], rk + ".recursive", "'recursive' only applies to a 'path' rule");
    }
    if (rule.file && *rule.file == "/") {
        r.fail(node["file"], rk + ".file", "'/' is a directory, not a file");
    }
    const bool has_retention_key = node["retention"].IsDefined();
    if (rule.action == RuleAction::exclude) {
        if (has_retention_key || rule.timestamp) {
            r.fail(node, rk, "an exclude rule takes neither 'retention' nor 'timestamp'");
        }
    } else if (!has_retention_key) {
        r.fail(node, rk + ".retention", "missing retention (a duration such as \"1d\", or \"never\")");
    }
    return rule;
}

void parse_retention(const Reader& r, const YAML::Node& node, RetentionConfig& ret) {
    const std::string k = "retention";
    r.expect_map(node, k, {"default", "timestamp", "evaluation_timestamp", "rules"});
    with(node, "default", [&](const YAML::Node& n) { ret.default_retention = r.retention(n, k + ".default"); });
    if (node["timestamp"].IsDefined() && node["evaluation_timestamp"].IsDefined()) {
        r.fail(node, k, "use either 'timestamp' or its alias 'evaluation_timestamp', not both");
    }
    with(node, "timestamp", [&](const YAML::Node& n) { ret.timestamp = parse_timestamp(r, n, k + ".timestamp"); });
    with(node, "evaluation_timestamp",
         [&](const YAML::Node& n) { ret.timestamp = parse_timestamp(r, n, k + ".evaluation_timestamp"); });
    with(node, "rules", [&](const YAML::Node& n) {
        if (!n.IsSequence()) {
            r.fail(n, k + ".rules", "expected a list of rules");
        }
        std::unordered_set<std::string> ids;
        for (std::size_t i = 0; i < n.size(); ++i) {
            RuleConfig rule = parse_rule(r, n[i], k + ".rules[" + std::to_string(i) + "]");
            if (!ids.insert(rule.id).second) {
                r.fail(n[i]["id"], k + ".rules[" + std::to_string(i) + "].id",
                       "duplicate rule identifier '" + rule.id + "'");
            }
            ret.rules.push_back(std::move(rule));
        }
    });
}

void parse_gc(const Reader& r, const YAML::Node& node, GcConfig& gc) {
    const std::string k = "garbage_collector";
    r.expect_map(node, k,
                 {"enabled", "dry_run", "scan_interval", "max_scan_duration", "max_workers", "max_deletions_per_second",
                  "max_deletions_per_cycle", "max_pending_tasks", "batch_size", "retry_limit", "retry_initial_delay",
                  "retry_max_delay", "skip_open_files", "scheduler"});
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    with(node, "enabled", [&](const YAML::Node& n) { gc.enabled = r.boolean(n, k + ".enabled"); });
    with(node, "dry_run", [&](const YAML::Node& n) { gc.dry_run = r.boolean(n, k + ".dry_run"); });
    with(node, "scan_interval",
         [&](const YAML::Node& n) { gc.scan_interval = r.duration(n, k + ".scan_interval", seconds(1)); });
    with(node, "max_scan_duration", [&](const YAML::Node& n) {
        gc.max_scan_duration = r.duration(n, k + ".max_scan_duration", milliseconds(10));
    });
    with(node, "max_workers",
         [&](const YAML::Node& n) { gc.max_workers = r.integer<std::uint32_t>(n, k + ".max_workers", 1, 64); });
    with(node, "max_deletions_per_second", [&](const YAML::Node& n) {
        gc.max_deletions_per_second = r.integer<std::uint32_t>(n, k + ".max_deletions_per_second", 0, 1000000);
    });
    with(node, "max_deletions_per_cycle", [&](const YAML::Node& n) {
        gc.max_deletions_per_cycle = r.integer<std::uint64_t>(n, k + ".max_deletions_per_cycle", 0, 1000000000ULL);
    });
    with(node, "max_pending_tasks", [&](const YAML::Node& n) {
        gc.max_pending_tasks = r.integer<std::uint64_t>(n, k + ".max_pending_tasks", 16, 10000000ULL);
    });
    with(node, "batch_size",
         [&](const YAML::Node& n) { gc.batch_size = r.integer<std::uint32_t>(n, k + ".batch_size", 1, 10000); });
    with(node, "retry_limit",
         [&](const YAML::Node& n) { gc.retry_limit = r.integer<std::uint32_t>(n, k + ".retry_limit", 0, 100); });
    with(node, "retry_initial_delay", [&](const YAML::Node& n) {
        gc.retry_initial_delay = r.duration(n, k + ".retry_initial_delay", milliseconds(10));
    });
    with(node, "retry_max_delay",
         [&](const YAML::Node& n) { gc.retry_max_delay = r.duration(n, k + ".retry_max_delay", milliseconds(10)); });
    with(node, "skip_open_files",
         [&](const YAML::Node& n) { gc.skip_open_files = r.boolean(n, k + ".skip_open_files"); });
    with(node, "scheduler", [&](const YAML::Node& n) {
        gc.scheduler = r.choice<SchedulerKind>(n, k + ".scheduler",
                                               {{"heap", SchedulerKind::heap}, {"ordered", SchedulerKind::ordered}});
    });
    if (gc.retry_initial_delay > gc.retry_max_delay) {
        r.fail(node, k, "retry_initial_delay must not exceed retry_max_delay");
    }
    if (gc.batch_size > gc.max_pending_tasks) {
        r.fail(node, k, "batch_size must not exceed max_pending_tasks");
    }
}

void parse_logging(const Reader& r, const YAML::Node& node, LoggingConfig& log) {
    const std::string k = "logging";
    r.expect_map(node, k,
                 {"enabled", "directory", "filename", "file", "level", "format", "console", "async", "rotation"});
    with(node, "enabled", [&](const YAML::Node& n) { log.enabled = r.boolean(n, k + ".enabled"); });
    if (node["file"].IsDefined() && (node["directory"].IsDefined() || node["filename"].IsDefined())) {
        r.fail(node["file"], k + ".file", "use either 'file' or 'directory' + 'filename', not both");
    }
    with(node, "file", [&](const YAML::Node& n) {
        const std::string path = r.host_path(n, k + ".file");
        if (path == "/") {
            r.fail(n, k + ".file", "not a file path");
        }
        log.directory = std::string(managed_path::parent(path));
        log.filename = std::string(managed_path::basename(path));
    });
    with(node, "directory", [&](const YAML::Node& n) { log.directory = r.host_path(n, k + ".directory"); });
    with(node, "filename", [&](const YAML::Node& n) {
        log.filename = r.scalar(n, k + ".filename");
        if (log.filename.empty() || log.filename == "." || log.filename == ".." ||
            log.filename.find('/') != std::string::npos || log.filename.find('\0') != std::string::npos ||
            log.filename.size() > 200) {
            r.fail(n, k + ".filename", "invalid file name '" + log.filename + "' (a plain name, no '/')");
        }
    });
    with(node, "level", [&](const YAML::Node& n) {
        log.level = r.choice<LogLevel>(n, k + ".level",
                                       {{"trace", LogLevel::trace},
                                        {"debug", LogLevel::debug},
                                        {"info", LogLevel::info},
                                        {"warning", LogLevel::warning},
                                        {"error", LogLevel::error},
                                        {"critical", LogLevel::critical}});
    });
    with(node, "format", [&](const YAML::Node& n) {
        log.format = r.choice<LogFormat>(n, k + ".format", {{"text", LogFormat::text}, {"json", LogFormat::json}});
    });
    with(node, "console", [&](const YAML::Node& n) { log.console = r.boolean(n, k + ".console"); });
    with(node, "async", [&](const YAML::Node& n) {
        const std::string ak = k + ".async";
        r.expect_map(n, ak, {"enabled", "queue_size", "overflow_policy"});
        with(n, "enabled", [&](const YAML::Node& c) { log.async.enabled = r.boolean(c, ak + ".enabled"); });
        with(n, "queue_size", [&](const YAML::Node& c) {
            log.async.queue_size = r.integer<std::uint32_t>(c, ak + ".queue_size", 64, 1048576);
        });
        with(n, "overflow_policy", [&](const YAML::Node& c) {
            log.async.overflow_policy =
                r.choice<LogOverflowPolicy>(c, ak + ".overflow_policy",
                                            {{"block", LogOverflowPolicy::block},
                                             {"overrun_oldest", LogOverflowPolicy::overrun_oldest},
                                             {"discard_new", LogOverflowPolicy::discard_new}});
        });
    });
    with(node, "rotation", [&](const YAML::Node& n) {
        const std::string rk = k + ".rotation";
        r.expect_map(n, rk,
                     {"enabled", "max_file_size", "max_files", "check_interval", "compress", "compression_level"});
        with(n, "enabled", [&](const YAML::Node& c) { log.rotation.enabled = r.boolean(c, rk + ".enabled"); });
        with(n, "max_file_size", [&](const YAML::Node& c) {
            log.rotation.max_file_size = r.size(c, rk + ".max_file_size", 4096, 1ULL << 40);
        });
        with(n, "max_files", [&](const YAML::Node& c) {
            log.rotation.max_files = r.integer<std::uint32_t>(c, rk + ".max_files", 1, 1000);
        });
        with(n, "check_interval", [&](const YAML::Node& c) {
            log.rotation.check_interval = r.duration(c, rk + ".check_interval", std::chrono::milliseconds(100));
        });
        with(n, "compress", [&](const YAML::Node& c) { log.rotation.compress = r.boolean(c, rk + ".compress"); });
        with(n, "compression_level", [&](const YAML::Node& c) {
            log.rotation.compression_level = r.integer<int>(c, rk + ".compression_level", 1, 9);
        });
    });
}

void parse_diagnostics(const Reader& r, const YAML::Node& node, DiagnosticsConfig& diag) {
    const std::string k = "diagnostics";
    r.expect_map(node, k, {"enabled", "text_stack_trace", "native_core_dump", "crash_directory", "max_crash_reports"});
    with(node, "enabled", [&](const YAML::Node& n) { diag.enabled = r.boolean(n, k + ".enabled"); });
    with(node, "text_stack_trace",
         [&](const YAML::Node& n) { diag.text_stack_trace = r.boolean(n, k + ".text_stack_trace"); });
    with(node, "native_core_dump",
         [&](const YAML::Node& n) { diag.native_core_dump = r.boolean(n, k + ".native_core_dump"); });
    with(node, "crash_directory",
         [&](const YAML::Node& n) { diag.crash_directory = r.host_path(n, k + ".crash_directory"); });
    with(node, "max_crash_reports", [&](const YAML::Node& n) {
        diag.max_crash_reports = r.integer<std::uint32_t>(n, k + ".max_crash_reports", 1, 1000);
    });
}

void parse_metrics(const Reader& r, const YAML::Node& node, MetricsConfig& metrics) {
    const std::string k = "metrics";
    r.expect_map(node, k, {"enabled", "status_file", "interval"});
    with(node, "enabled", [&](const YAML::Node& n) { metrics.enabled = r.boolean(n, k + ".enabled"); });
    with(node, "status_file", [&](const YAML::Node& n) { metrics.status_file = r.host_path(n, k + ".status_file"); });
    with(node, "interval",
         [&](const YAML::Node& n) { metrics.interval = r.duration(n, k + ".interval", std::chrono::seconds(1)); });
}

void parse_security(const Reader& r, const YAML::Node& node, SecurityConfig& sec) {
    const std::string k = "security";
    r.expect_map(node, k, {"stay_on_filesystem", "require_openat2"});
    with(node, "stay_on_filesystem",
         [&](const YAML::Node& n) { sec.stay_on_filesystem = r.boolean(n, k + ".stay_on_filesystem"); });
    with(node, "require_openat2",
         [&](const YAML::Node& n) { sec.require_openat2 = r.boolean(n, k + ".require_openat2"); });
}

void cross_checks(const Reader& r, const YAML::Node& root, const Config& c) {
    const auto inside_backing = [&](const std::string& p) {
        return managed_path::is_within(p, c.filesystem.backing_directory) ||
               managed_path::is_within(p, c.filesystem.mountpoint);
    };
    if (inside_backing(c.logging.directory)) {
        r.fail(root["logging"], "logging.directory",
               "the log directory must be outside the backing directory and the mount point (logs would be subject "
               "to retention and recursive logging)");
    }
    if (c.diagnostics.enabled && inside_backing(c.diagnostics.crash_directory)) {
        r.fail(root["diagnostics"], "diagnostics.crash_directory",
               "the crash directory must be outside the backing directory and the mount point");
    }
    if (c.metrics.enabled && inside_backing(c.metrics.status_file)) {
        r.fail(root["metrics"], "metrics.status_file",
               "the status file must be outside the backing directory and the mount point");
    }
}

}  // namespace

Result<Config> parse_config(std::string_view yaml_text, const std::string& source_name) {
    try {
        const YAML::Node root = YAML::Load(std::string(yaml_text));
        Reader r(source_name);
        if (!root.IsDefined() || root.IsNull()) {
            return make_error(EINVAL, source_name + ": empty configuration (at least 'version: 1' is required)");
        }
        r.expect_map(root, "",
                     {"version", "filesystem", "retention", "garbage_collector", "logging", "diagnostics", "metrics",
                      "security"});
        Config config;
        config.logging.level = parse_log_level(TSFS_DEFAULT_LOG_LEVEL).value_or(LogLevel::info);
        const YAML::Node version = root["version"];
        if (!version.IsDefined() || version.IsNull()) {
            r.fail(root, "version", "missing configuration version (expected 'version: 1')");
        }
        config.version = r.integer<int>(version, "version", 0, 1000);
        if (config.version != kConfigVersion) {
            r.fail(version, "version",
                   "unsupported configuration version " + std::to_string(config.version) +
                       " (this build supports version " + std::to_string(kConfigVersion) + ")");
        }
        with(root, "filesystem", [&](const YAML::Node& n) { parse_filesystem(r, n, config.filesystem); });
        with(root, "retention", [&](const YAML::Node& n) { parse_retention(r, n, config.retention); });
        with(root, "garbage_collector", [&](const YAML::Node& n) { parse_gc(r, n, config.gc); });
        with(root, "logging", [&](const YAML::Node& n) { parse_logging(r, n, config.logging); });
        with(root, "diagnostics", [&](const YAML::Node& n) { parse_diagnostics(r, n, config.diagnostics); });
        with(root, "metrics", [&](const YAML::Node& n) { parse_metrics(r, n, config.metrics); });
        with(root, "security", [&](const YAML::Node& n) { parse_security(r, n, config.security); });
        cross_checks(r, root, config);
        config.source_path = source_name;
        return config;
    } catch (const ConfigException& e) {
        return make_error(EINVAL, e.message);
    } catch (const YAML::ParserException& e) {
        return make_error(EINVAL, source_name + ":" + std::to_string(e.mark.line + 1) + ":" +
                                      std::to_string(e.mark.column + 1) + ": invalid YAML: " + e.msg);
    } catch (const YAML::Exception& e) {
        return make_error(EINVAL, source_name + ": invalid YAML: " + e.what());
    } catch (const std::exception& e) {
        return make_error(EINVAL, source_name + ": " + e.what());
    }
}

Result<void> validate_path_overrides(Config& config) {
    FilesystemConfig& fs = config.filesystem;
    for (std::string* p : {&fs.mountpoint, &fs.backing_directory}) {
        auto normalized = managed_path::normalize(*p);
        if (!normalized) {
            return make_error(EINVAL, std::format("command line: '{}': {}", *p, normalized.error().message));
        }
        *p = std::move(*normalized);
    }
    if (fs.mountpoint == "/" || fs.backing_directory == "/") {
        return make_error(EINVAL, "command line: neither the mount point nor the backing directory may be '/'");
    }
    if (managed_path::is_within(fs.mountpoint, fs.backing_directory) ||
        managed_path::is_within(fs.backing_directory, fs.mountpoint)) {
        return make_error(EINVAL,
                          "command line: the mount point and the backing directory must not contain each other");
    }
    const auto inside = [&](const std::string& p) {
        return managed_path::is_within(p, fs.backing_directory) || managed_path::is_within(p, fs.mountpoint);
    };
    if (inside(config.logging.directory) ||
        (config.diagnostics.enabled && inside(config.diagnostics.crash_directory)) ||
        (config.metrics.enabled && inside(config.metrics.status_file))) {
        return make_error(EINVAL,
                          "command line: the log directory, the crash directory and the status file must be outside "
                          "the backing directory and the mount point");
    }
    return {};
}

Result<void> check_config_file_trust(const struct stat& st, uid_t euid, const std::string& path) {
    if (!S_ISREG(st.st_mode)) {
        return make_error(EINVAL, path + ": the configuration is not a regular file");
    }
    if (euid == 0 && st.st_uid != 0) {
        return make_error(
            EACCES, std::format("{}: running as root, the configuration must be owned by root (owner uid {})", path,
                                st.st_uid));
    }
    if (euid == 0 && (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        return make_error(EACCES,
                          std::format("{}: running as root, the configuration must not be writable by its group "
                                      "or by others (mode {:04o}; fix with chmod go-w)",
                                      path, st.st_mode & 07777));
    }
    return {};
}

Result<Config> load_config_file(const std::string& path) {
    const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        const int err = errno;
        return make_error(err, path + ": cannot open configuration file: " + std::strerror(err));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0) {
        const int err = errno;
        return make_error(err, path + ": cannot stat configuration file: " + std::strerror(err));
    }
    if (auto trusted = check_config_file_trust(st, ::geteuid(), path); !trusted) {
        return propagate(trusted.error());
    }
    constexpr std::size_t kMaxConfigSize = 4U << 20U;
    if (static_cast<std::uint64_t>(st.st_size) > kMaxConfigSize) {
        return make_error(EFBIG, path + ": configuration file larger than 4 MiB");
    }
    std::string content;
    char buffer[64 * 1024];
    for (;;) {
        const ssize_t n = ::read(fd.get(), buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            const int err = errno;
            return make_error(err, path + ": cannot read configuration file: " + std::strerror(err));
        }
        if (n == 0) {
            break;
        }
        content.append(buffer, static_cast<std::size_t>(n));
        if (content.size() > kMaxConfigSize) {  // Grew while being read.
            return make_error(EFBIG, path + ": configuration file larger than 4 MiB");
        }
    }
    return parse_config(content, path);
}

std::string dump_config(const Config& c) {
    YAML::Emitter out;
    const auto retention = [](const std::optional<Duration>& d) {
        return d ? format_duration(*d) : std::string("never");
    };
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << c.version;

    out << YAML::Key << "filesystem" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "mountpoint" << YAML::Value << YAML::DoubleQuoted << c.filesystem.mountpoint;
    out << YAML::Key << "backing_directory" << YAML::Value << YAML::DoubleQuoted << c.filesystem.backing_directory;
    out << YAML::Key << "mode" << YAML::Value << "passthrough";
    out << YAML::Key << "allow_other" << YAML::Value << c.filesystem.allow_other;
    out << YAML::Key << "read_only" << YAML::Value << c.filesystem.read_only;
    out << YAML::Key << "retention_xattrs" << YAML::Value << c.filesystem.retention_xattrs;
    out << YAML::EndMap;

    out << YAML::Key << "retention" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "default" << YAML::Value << YAML::DoubleQuoted << retention(c.retention.default_retention);
    out << YAML::Key << "timestamp" << YAML::Value << to_string(c.retention.timestamp);
    out << YAML::Key << "rules" << YAML::Value << YAML::BeginSeq;
    for (const auto& rule : c.retention.rules) {
        out << YAML::BeginMap;
        out << YAML::Key << "id" << YAML::Value << YAML::DoubleQuoted << rule.id;
        out << YAML::Key << "enabled" << YAML::Value << rule.enabled;
        out << YAML::Key << "action" << YAML::Value << to_string(rule.action);
        out << YAML::Key << "priority" << YAML::Value << rule.priority;
        if (rule.file) {
            out << YAML::Key << "file" << YAML::Value << YAML::DoubleQuoted << *rule.file;
        }
        if (rule.path) {
            out << YAML::Key << "path" << YAML::Value << YAML::DoubleQuoted << *rule.path;
            out << YAML::Key << "recursive" << YAML::Value << rule.recursive;
        }
        if (rule.glob) {
            out << YAML::Key << "glob" << YAML::Value << YAML::DoubleQuoted << *rule.glob;
        }
        if (rule.path_glob) {
            out << YAML::Key << "path_glob" << YAML::Value << YAML::DoubleQuoted << *rule.path_glob;
        }
        if (!rule.extensions.empty()) {
            out << YAML::Key << "extension" << YAML::Value << YAML::Flow << rule.extensions;
        }
        if (rule.action == RuleAction::expire) {
            out << YAML::Key << "retention" << YAML::Value << YAML::DoubleQuoted << retention(rule.retention);
            if (rule.timestamp) {
                out << YAML::Key << "timestamp" << YAML::Value << to_string(*rule.timestamp);
            }
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    const auto& g = c.gc;
    out << YAML::Key << "garbage_collector" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << g.enabled;
    out << YAML::Key << "dry_run" << YAML::Value << g.dry_run;
    out << YAML::Key << "scan_interval" << YAML::Value << format_duration(g.scan_interval);
    out << YAML::Key << "max_scan_duration" << YAML::Value << format_duration(g.max_scan_duration);
    out << YAML::Key << "max_workers" << YAML::Value << g.max_workers;
    out << YAML::Key << "max_deletions_per_second" << YAML::Value << g.max_deletions_per_second;
    out << YAML::Key << "max_deletions_per_cycle" << YAML::Value << g.max_deletions_per_cycle;
    out << YAML::Key << "max_pending_tasks" << YAML::Value << g.max_pending_tasks;
    out << YAML::Key << "batch_size" << YAML::Value << g.batch_size;
    out << YAML::Key << "retry_limit" << YAML::Value << g.retry_limit;
    out << YAML::Key << "retry_initial_delay" << YAML::Value << format_duration(g.retry_initial_delay);
    out << YAML::Key << "retry_max_delay" << YAML::Value << format_duration(g.retry_max_delay);
    out << YAML::Key << "skip_open_files" << YAML::Value << g.skip_open_files;
    out << YAML::Key << "scheduler" << YAML::Value << to_string(g.scheduler);
    out << YAML::EndMap;

    const auto& l = c.logging;
    out << YAML::Key << "logging" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << l.enabled;
    out << YAML::Key << "directory" << YAML::Value << YAML::DoubleQuoted << l.directory;
    out << YAML::Key << "filename" << YAML::Value << YAML::DoubleQuoted << l.filename;
    out << YAML::Key << "level" << YAML::Value << to_string(l.level);
    out << YAML::Key << "format" << YAML::Value << to_string(l.format);
    out << YAML::Key << "console" << YAML::Value << l.console;
    out << YAML::Key << "async" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << l.async.enabled;
    out << YAML::Key << "queue_size" << YAML::Value << l.async.queue_size;
    out << YAML::Key << "overflow_policy" << YAML::Value << to_string(l.async.overflow_policy);
    out << YAML::EndMap;
    out << YAML::Key << "rotation" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << l.rotation.enabled;
    out << YAML::Key << "max_file_size" << YAML::Value << format_size(l.rotation.max_file_size);
    out << YAML::Key << "max_files" << YAML::Value << l.rotation.max_files;
    out << YAML::Key << "check_interval" << YAML::Value << format_duration(l.rotation.check_interval);
    out << YAML::Key << "compress" << YAML::Value << l.rotation.compress;
    out << YAML::Key << "compression_level" << YAML::Value << l.rotation.compression_level;
    out << YAML::EndMap;
    out << YAML::EndMap;

    const auto& d = c.diagnostics;
    out << YAML::Key << "diagnostics" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << d.enabled;
    out << YAML::Key << "text_stack_trace" << YAML::Value << d.text_stack_trace;
    out << YAML::Key << "native_core_dump" << YAML::Value << d.native_core_dump;
    out << YAML::Key << "crash_directory" << YAML::Value << YAML::DoubleQuoted << d.crash_directory;
    out << YAML::Key << "max_crash_reports" << YAML::Value << d.max_crash_reports;
    out << YAML::EndMap;

    out << YAML::Key << "metrics" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "enabled" << YAML::Value << c.metrics.enabled;
    out << YAML::Key << "status_file" << YAML::Value << YAML::DoubleQuoted << c.metrics.status_file;
    out << YAML::Key << "interval" << YAML::Value << format_duration(c.metrics.interval);
    out << YAML::EndMap;

    out << YAML::Key << "security" << YAML::Value << YAML::BeginMap;
    out << YAML::Key << "stay_on_filesystem" << YAML::Value << c.security.stay_on_filesystem;
    out << YAML::Key << "require_openat2" << YAML::Value << c.security.require_openat2;
    out << YAML::EndMap;

    out << YAML::EndMap;
    return std::string(out.c_str()) + "\n";
}

}  // namespace tsfs
