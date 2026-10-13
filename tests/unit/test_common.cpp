// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/unit/test_common.cpp
// Purpose : Unit tests: durations, sizes, managed paths, escaping, log event rendering
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>

#include <limits>

#include "common/escape.hpp"
#include "common/managed_path.hpp"
#include "common/units.hpp"
#include "filesystem/fuse_control.hpp"
#include "logging/log_field.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

using namespace std::chrono_literals;

// --- durations -------------------------------------------------------------------

TEST(Duration, ParsesEverySingleUnit) {
    EXPECT_EQ(*parse_duration("500ms"), 500ms);
    EXPECT_EQ(*parse_duration("90s"), 90s);
    EXPECT_EQ(*parse_duration("30m"), 30min);
    EXPECT_EQ(*parse_duration("12h"), 12h);
    EXPECT_EQ(*parse_duration("7d"), 168h);
    EXPECT_EQ(*parse_duration("2w"), 336h);
    EXPECT_EQ(*parse_duration("0s"), 0ms);
}

TEST(Duration, ParsesCombinedUnitsInDecreasingOrder) {
    EXPECT_EQ(*parse_duration("1d12h"), 36h);
    EXPECT_EQ(*parse_duration("1h30m15s"), 1h + 30min + 15s);
    EXPECT_EQ(*parse_duration("1s500ms"), 1500ms);
}

TEST(Duration, RejectsMalformedValues) {
    for (const char* bad :
         {"", "1", "d", "1x", "1 d", "-1d", "+1d", "1.5h", "1h1d", "1h1h", "h1", "1D", "1dd", "1d ", " 1d", "1msm"}) {
        EXPECT_FALSE(parse_duration(bad).has_value()) << bad;
    }
}

TEST(Duration, RejectsOverflow) {
    EXPECT_FALSE(parse_duration("99999999999999999999s").has_value());
    EXPECT_FALSE(parse_duration("9223372036854775807w").has_value());
    EXPECT_FALSE(parse_duration("15250284453w").has_value());  // > INT64_MAX milliseconds
    EXPECT_TRUE(parse_duration("15250284452w").has_value());   // Largest representable week count.
}

TEST(Duration, FormatsWithLargestExactUnits) {
    EXPECT_EQ(format_duration(0ms), "0s");
    EXPECT_EQ(format_duration(90s), "1m30s");
    EXPECT_EQ(format_duration(36h), "1d12h");
    EXPECT_EQ(format_duration(1500ms), "1s500ms");
    EXPECT_EQ(format_duration(168h), "1w");
}

TEST(Duration, FormatParseRoundTrip) {
    for (const auto value : {1ms, 999ms, 1000ms, 61000ms, 86400000ms, 90061001ms}) {
        EXPECT_EQ(*parse_duration(format_duration(value)), value);
    }
}

// --- sizes -----------------------------------------------------------------------

TEST(Size, ParsesBinaryAndDecimalUnits) {
    EXPECT_EQ(*parse_size("4096"), 4096U);
    EXPECT_EQ(*parse_size("512B"), 512U);
    EXPECT_EQ(*parse_size("1KiB"), 1024U);
    EXPECT_EQ(*parse_size("50MiB"), 50ULL << 20);
    EXPECT_EQ(*parse_size("2GiB"), 2ULL << 30);
    EXPECT_EQ(*parse_size("1TiB"), 1ULL << 40);
    EXPECT_EQ(*parse_size("1KB"), 1000U);
    EXPECT_EQ(*parse_size("1MB"), 1000000U);
    EXPECT_EQ(*parse_size("3GB"), 3000000000ULL);
}

TEST(Size, RejectsMalformedAndOverflowingValues) {
    for (const char* bad :
         {"", "MiB", "1mib", "1 MiB", "-1", "1.5MiB", "1PiB", "1Mi", "18446744073709551616", "17179869184GiB"}) {
        EXPECT_FALSE(parse_size(bad).has_value()) << bad;
    }
}

TEST(Size, Formats) {
    EXPECT_EQ(format_size(50ULL << 20), "50MiB");
    EXPECT_EQ(format_size(1536), "1536B");
    EXPECT_EQ(format_size(0), "0B");
    EXPECT_EQ(format_size(1ULL << 40), "1TiB");
}

// --- managed paths ---------------------------------------------------------------

TEST(ManagedPath, NormalizesSlashes) {
    EXPECT_EQ(*managed_path::normalize("/"), "/");
    EXPECT_EQ(*managed_path::normalize("///"), "/");
    EXPECT_EQ(*managed_path::normalize("/tmp/record/"), "/tmp/record");
    EXPECT_EQ(*managed_path::normalize("//tmp//record"), "/tmp/record");
    EXPECT_EQ(*managed_path::normalize("/a b/c"), "/a b/c");
}

TEST(ManagedPath, RejectsRelativeTraversalAndNul) {
    EXPECT_FALSE(managed_path::normalize("").has_value());
    EXPECT_FALSE(managed_path::normalize("tmp").has_value());
    EXPECT_FALSE(managed_path::normalize("/tmp/../etc").has_value());
    EXPECT_FALSE(managed_path::normalize("/..").has_value());
    EXPECT_FALSE(managed_path::normalize("/./a").has_value());
    EXPECT_FALSE(managed_path::normalize(std::string_view("/a\0b", 4)).has_value());
}

TEST(ManagedPath, IsNormalized) {
    EXPECT_TRUE(managed_path::is_normalized("/"));
    EXPECT_TRUE(managed_path::is_normalized("/a/b"));
    EXPECT_FALSE(managed_path::is_normalized("/a/"));
    EXPECT_FALSE(managed_path::is_normalized("/a//b"));
    EXPECT_FALSE(managed_path::is_normalized("/a/../b"));
    EXPECT_FALSE(managed_path::is_normalized("a"));
    EXPECT_FALSE(managed_path::is_normalized(""));
}

TEST(ManagedPath, Components) {
    EXPECT_EQ(managed_path::basename("/a/b.mp4"), "b.mp4");
    EXPECT_EQ(managed_path::basename("/"), "");
    EXPECT_EQ(managed_path::parent("/a/b"), "/a");
    EXPECT_EQ(managed_path::parent("/a"), "/");
    EXPECT_EQ(managed_path::parent("/"), "/");
    EXPECT_EQ(managed_path::depth("/"), 0U);
    EXPECT_EQ(managed_path::depth("/a/b/c"), 3U);
    EXPECT_EQ(managed_path::join("/", "a"), "/a");
    EXPECT_EQ(managed_path::join("/a", "b"), "/a/b");
    EXPECT_EQ(managed_path::relative("/"), ".");
    EXPECT_EQ(managed_path::relative("/a/b"), "a/b");
}

TEST(ManagedPath, IsWithinIsComponentWise) {
    EXPECT_TRUE(managed_path::is_within("/a/b", "/a"));
    EXPECT_TRUE(managed_path::is_within("/a", "/a"));
    EXPECT_TRUE(managed_path::is_within("/anything", "/"));
    EXPECT_FALSE(managed_path::is_within("/ab", "/a"));
    EXPECT_FALSE(managed_path::is_within("/a", "/a/b"));
}

// --- escaping --------------------------------------------------------------------

std::string text(std::string_view s) {
    std::string out;
    append_text_quoted(out, s);
    return out;
}

std::string json(std::string_view s) {
    std::string out;
    append_json_quoted(out, s);
    return out;
}

TEST(Escape, TextQuotingNeutralizesNewlinesQuotesAndControls) {
    EXPECT_EQ(text("plain"), "\"plain\"");
    EXPECT_EQ(text("a b"), "\"a b\"");
    EXPECT_EQ(text("x\"y"), "\"x\\\"y\"");
    EXPECT_EQ(text("back\\slash"), "\"back\\\\slash\"");
    EXPECT_EQ(text("line\nforged"), "\"line\\nforged\"");
    EXPECT_EQ(text("\x01\x7f"), "\"\\x01\\x7f\"");
}

TEST(Escape, Utf8IsKeptInvalidBytesAreHexEscaped) {
    EXPECT_EQ(text("vid\xc3\xa9o"), "\"vid\xc3\xa9o\"");
    EXPECT_EQ(text("bad\xff"), "\"bad\\xff\"");
    EXPECT_EQ(text("\xc3"), "\"\\xc3\"");                    // Truncated sequence.
    EXPECT_EQ(text("\xc0\xaf"), "\"\\xc0\\xaf\"");           // Overlong encoding of '/'.
    EXPECT_EQ(text("\xed\xa0\x80"), "\"\\xed\\xa0\\x80\"");  // UTF-16 surrogate.
}

TEST(Escape, JsonQuoting) {
    EXPECT_EQ(json("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(json("\n\t\x01"), "\"\\n\\t\\u0001\"");
    EXPECT_EQ(json("bad\xff"), "\"bad\\\\xff\"");
    EXPECT_EQ(json("caf\xc3\xa9"), "\"caf\xc3\xa9\"");
}

TEST(LogEvent, TextRendering) {
    const LogField fields[] = {{"path", "/a b"}, {"bytes", std::uint64_t{12}}, {"ok", true}, {"delta", -3}};
    EXPECT_EQ(render_event(LogFormat::text, "gc", "gc_file_deleted", "done", fields),
              "component=gc event=gc_file_deleted path=\"/a b\" bytes=12 ok=true delta=-3 msg=\"done\"");
}

TEST(LogEvent, JsonRenderingIsValidObjectContent) {
    const LogField fields[] = {{"path", "/x\"y"}, {"ratio", 0.5}};
    EXPECT_EQ(render_event(LogFormat::json, "gc", "e", "", fields),
              "\"component\":\"gc\",\"event\":\"e\",\"path\":\"/x\\\"y\",\"ratio\":0.500");
}

// --- fusectl connection of a mount ----------------------------------------------

TEST(FuseControl, ConnectionIdComesFromTheTopmostFuseMountOfThePath) {
    const std::string mountinfo =
        "22 1 252:1 / / rw,relatime shared:1 - ext4 /dev/mapper/root rw\n"
        "36 22 0:52 / /mnt/tsfs rw,nosuid,nodev shared:30 - fuse.tsfs tsfs rw,user_id=0,group_id=0\n"
        "37 22 0:60 / /mnt/with\\040space rw shared:31 master:2 - fuse.tsfs tsfs rw\n"
        "38 22 0:61 / /mnt/other rw - tmpfs tmpfs rw\n"
        "39 36 0:70 / /mnt/tsfs rw - fuse tsfs rw\n"  // Stacked: the last one is on top.
        "40 22 8:3 / /mnt/blk rw - fuseblk dev rw\n";
    EXPECT_EQ(fuse_connection_id(mountinfo, "/mnt/tsfs"), 70U);
    EXPECT_EQ(fuse_connection_id(mountinfo, "/mnt/with space"), 60U);
    EXPECT_FALSE(fuse_connection_id(mountinfo, "/mnt/other").has_value());  // Not FUSE.
    EXPECT_FALSE(fuse_connection_id(mountinfo, "/mnt/none").has_value());
    EXPECT_FALSE(fuse_connection_id(mountinfo, "/mnt/blk").has_value());  // fuseblk: not "fuse"/"fuse.*".
    EXPECT_FALSE(fuse_connection_id("garbage line\n\n", "/mnt/tsfs").has_value());
    // A non-zero major uses the kernel encoding (major << 20 | minor).
    EXPECT_EQ(fuse_connection_id("41 22 1:2 / /m rw - fuse.x x rw\n", "/m"), (1U << 20U) | 2U);
}

TEST(FuseControl, WaitingIsReadFromTheConnectionDirectory) {
    const test::TempDir dir("tsfs-fusectl");
    test::write_file(dir / "42/waiting", "3\n");
    EXPECT_EQ(fuse_connection_waiting(42, dir.path().string()), 3U);
    EXPECT_FALSE(fuse_connection_waiting(43, dir.path().string()).has_value());  // No such connection.
}

}  // namespace
}  // namespace tsfs
