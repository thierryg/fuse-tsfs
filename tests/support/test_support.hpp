// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/support/test_support.hpp
// Purpose : Test fixtures: isolated temporary directories, file helpers, snapshot builders
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <sys/stat.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "common/clock.hpp"
#include "config/config.hpp"
#include "gc/garbage_collector.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs::test {

/**
 * @brief A private directory removed (recursively) at destruction.
 *
 * Created under $TSFS_TEST_ARTIFACT_DIR when set (always kept for inspection when
 * TSFS_TEST_KEEP_ARTIFACTS=1, whatever the result), else under $TMPDIR or /tmp. Tests never touch
 * anything outside such a directory.
 */
class TempDir {
public:
    explicit TempDir(std::string_view prefix = "tsfs-test");
    ~TempDir();
    TempDir(const TempDir&) = delete("owns a directory");
    TempDir& operator=(const TempDir&) = delete("owns a directory");

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    /// @return path() / relative.
    [[nodiscard]] std::filesystem::path operator/(std::string_view relative) const { return path_ / relative; }

private:
    std::filesystem::path path_;
};

/// Create parent directories and write @p content to @p file.
void write_file(const std::filesystem::path& file, std::string_view content = "data");
/// Read a whole file ("" if missing).
std::string read_file(const std::filesystem::path& file);
/// Set the modification (and access) time of @p file, without following a link.
void set_mtime(const std::filesystem::path& file, TimePoint when);
/// Count the lines of a file.
std::size_t count_lines(const std::filesystem::path& file);

/// Build a GcSnapshot from a retention configuration and GC settings.
std::shared_ptr<const GcSnapshot> make_gc_snapshot(const RetentionConfig& retention, GcConfig gc = {},
                                                   std::uint64_t generation = 1);

/// A rule with only the fields set by the caller (designated-initializer friendly helper).
RuleConfig rule(std::string id, std::optional<Duration> retention = std::chrono::hours(1));

/// @return true when FUSE mounts are possible here (/dev/fuse usable and fusermount3 present).
bool fuse_available(std::string* reason = nullptr);

/// A fixed point in time used by deterministic tests (2026-01-01T00:00:00Z).
inline constexpr TimePoint kEpoch2026{std::chrono::milliseconds(1767225600000LL)};

}  // namespace tsfs::test
