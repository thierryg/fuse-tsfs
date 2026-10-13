// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/policy/test_policy.cpp
// Purpose : Glob matcher and policy engine: every matching combination, precedence, expiry math
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>

#include "policy/glob.hpp"
#include "policy/policy_engine.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

using namespace std::chrono_literals;
using test::kEpoch2026;
using test::rule;

// --- glob ------------------------------------------------------------------------

bool base(std::string_view pattern, std::string_view name) {
    auto g = Glob::compile(pattern, Glob::Mode::basename);
    EXPECT_TRUE(g.has_value()) << pattern;
    return g && g->matches(name);
}

bool full(std::string_view pattern, std::string_view path) {
    auto g = Glob::compile(pattern, Glob::Mode::path);
    EXPECT_TRUE(g.has_value()) << pattern;
    return g && g->matches(path);
}

TEST(Glob, BasenameWildcards) {
    EXPECT_TRUE(base("*.mp4", "video.mp4"));
    EXPECT_TRUE(base("*.mp4", ".mp4"));  // '*' may match nothing.
    EXPECT_FALSE(base("*.mp4", "video.mp4.part"));
    EXPECT_FALSE(base("*.mp4", "video.MP4"));  // Case-sensitive.
    EXPECT_TRUE(base("rec-??.bin", "rec-01.bin"));
    EXPECT_FALSE(base("rec-??.bin", "rec-1.bin"));
    EXPECT_TRUE(base("*", ".hidden"));  // Hidden files are not special.
    EXPECT_TRUE(base("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(base("a*b*c", "aXXbYY"));
}

TEST(Glob, CharacterClasses) {
    EXPECT_TRUE(base("file[0-9].log", "file7.log"));
    EXPECT_FALSE(base("file[0-9].log", "fileA.log"));
    EXPECT_TRUE(base("file[!0-9].log", "fileA.log"));
    EXPECT_TRUE(base("file[^0-9].log", "fileA.log"));
    EXPECT_TRUE(base("[]]x", "]x"));
    EXPECT_TRUE(base("[a-]x", "-x"));
}

TEST(Glob, EscapesAndUtf8) {
    EXPECT_TRUE(base("\\*.txt", "*.txt"));
    EXPECT_FALSE(base("\\*.txt", "a.txt"));
    EXPECT_TRUE(base("caf?", "caf\xc3\xa9"));  // '?' consumes a whole UTF-8 character.
}

TEST(Glob, InvalidPatternsAreRejected) {
    EXPECT_FALSE(Glob::compile("", Glob::Mode::basename).has_value());
    EXPECT_FALSE(Glob::compile("[abc", Glob::Mode::basename).has_value());
    EXPECT_FALSE(Glob::compile("abc\\", Glob::Mode::basename).has_value());
    EXPECT_FALSE(Glob::compile("[z-a]", Glob::Mode::basename).has_value());
    EXPECT_FALSE(Glob::compile("a/b", Glob::Mode::basename).has_value());
    EXPECT_FALSE(Glob::compile("relative/*.mp4", Glob::Mode::path).has_value());
    EXPECT_FALSE(Glob::compile("/a//b", Glob::Mode::path).has_value());
    EXPECT_FALSE(Glob::compile("/a/../b", Glob::Mode::path).has_value());
    EXPECT_FALSE(Glob::compile("/a/b**", Glob::Mode::path).has_value());
    EXPECT_FALSE(Glob::compile("/a/", Glob::Mode::path).has_value());
}

TEST(Glob, PathGlobStarDoesNotCrossSlash) {
    EXPECT_TRUE(full("/rec/*.mp4", "/rec/a.mp4"));
    EXPECT_FALSE(full("/rec/*.mp4", "/rec/sub/a.mp4"));
    EXPECT_FALSE(full("/rec/*.mp4", "/other/a.mp4"));
    EXPECT_TRUE(full("/*/x", "/a/x"));
    EXPECT_FALSE(full("/*/x", "/a/b/x"));
}

TEST(Glob, PathGlobDoubleStar) {
    EXPECT_TRUE(full("/rec/**/a.mp4", "/rec/a.mp4"));
    EXPECT_TRUE(full("/rec/**/a.mp4", "/rec/x/a.mp4"));
    EXPECT_TRUE(full("/rec/**/a.mp4", "/rec/x/y/z/a.mp4"));
    EXPECT_FALSE(full("/rec/**/a.mp4", "/rec/x/b.mp4"));
    EXPECT_TRUE(full("/**/*.core", "/deep/down/x.core"));
    EXPECT_TRUE(full("/**", "/anything/at/all"));
    EXPECT_TRUE(full("/a/**/b/**/c", "/a/1/b/2/3/c"));
    EXPECT_FALSE(full("/a/**/b/**/c", "/a/1/2/3/c"));
}

TEST(Glob, LiteralPrefixAndCount) {
    auto g = Glob::compile("/var/log/*/x.log", Glob::Mode::path);
    ASSERT_TRUE(g.has_value());
    EXPECT_EQ(g->literal_prefix(), "/var/log");
    EXPECT_EQ(Glob::compile("/x.log", Glob::Mode::path)->literal_prefix(), "/");
    EXPECT_EQ(Glob::compile("*.mp4", Glob::Mode::basename)->literal_count(), 4U);
}

// --- policy engine -----------------------------------------------------------------

FileTimes at(TimePoint mtime, std::optional<TimePoint> birth = std::nullopt) { return {mtime, birth}; }

std::shared_ptr<const PolicyEngine> engine(RetentionConfig config) {
    auto e = PolicyEngine::compile(config);
    EXPECT_TRUE(e.has_value());
    return *e;
}

TEST(PolicyEngine, EmptyConfigurationNeverExpires) {
    const auto e = engine({});
    EXPECT_TRUE(e->never_expires());
    const Decision d = e->evaluate("/a.mp4", at(kEpoch2026));
    EXPECT_EQ(d.kind, Decision::Kind::keep_forever);
    EXPECT_EQ(d.rule_index, -1);
    EXPECT_EQ(e->rule_id(-1), "default");
}

TEST(PolicyEngine, GlobalDefault) {
    RetentionConfig c;
    c.default_retention = 7 * 24h;
    const auto e = engine(c);
    const Decision d = e->evaluate("/any/file", at(kEpoch2026));
    ASSERT_EQ(d.kind, Decision::Kind::expires);
    EXPECT_EQ(d.expiry, kEpoch2026 + 7 * 24h);
}

TEST(PolicyEngine, NotExpiredExactlyAtAndAfterExpiry) {
    RetentionConfig c;
    c.default_retention = 1h;
    const auto e = engine(c);
    const Decision d = e->evaluate("/f", at(kEpoch2026));
    EXPECT_FALSE(d.expired_at(kEpoch2026 + 1h - 1ms));
    EXPECT_TRUE(d.expired_at(kEpoch2026 + 1h));  // Exactly at expiry counts as expired.
    EXPECT_TRUE(d.expired_at(kEpoch2026 + 2h));
}

TEST(PolicyEngine, ExtensionRuleIsCaseInsensitiveAndNeedsAStem) {
    RetentionConfig c;
    auto r = rule("mp4", 24h);
    r.extensions = {"mp4"};
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/x/video.mp4"), 0);
    EXPECT_EQ(e->match("/x/VIDEO.MP4"), 0);
    EXPECT_EQ(e->match("/x/.mp4"), -1);
    EXPECT_EQ(e->match("/x/video.mp4.part"), -1);
}

TEST(PolicyEngine, MultiDotExtension) {
    RetentionConfig c;
    auto r = rule("archives", 24h);
    r.extensions = {"tar.gz", "zip"};
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/a.tar.gz"), 0);
    EXPECT_EQ(e->match("/a.zip"), 0);
    EXPECT_EQ(e->match("/a.gz"), -1);
}

TEST(PolicyEngine, ExactFile) {
    RetentionConfig c;
    auto r = rule("one", 1h);
    r.file = "/tmp/one.bin";
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/tmp/one.bin"), 0);
    EXPECT_EQ(e->match("/tmp/one.bin2"), -1);
    EXPECT_EQ(e->match("/tmp/sub/one.bin"), -1);
}

TEST(PolicyEngine, DirectoryNonRecursiveVersusRecursive) {
    RetentionConfig c;
    auto flat = rule("flat", 1h);
    flat.path = "/tmp/record";
    auto deep = rule("deep", 2h);
    deep.path = "/cache";
    deep.recursive = true;
    c.rules = {flat, deep};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/tmp/record/a"), 0);
    EXPECT_EQ(e->match("/tmp/record/sub/a"), -1);
    EXPECT_EQ(e->match("/tmp/recordings/a"), -1);  // Component-wise, not a string prefix.
    EXPECT_EQ(e->match("/cache/a"), 1);
    EXPECT_EQ(e->match("/cache/x/y/z"), 1);
    EXPECT_EQ(e->match("/cache"), -1);  // The directory itself is not a file of its scope.
}

TEST(PolicyEngine, DirectoryAndPatternCombination) {
    RetentionConfig c;
    auto r = rule("rec-mp4", 1h);
    r.path = "/tmp/record";
    r.recursive = true;
    r.glob = "*.mp4";
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/tmp/record/a.mp4"), 0);
    EXPECT_EQ(e->match("/tmp/record/x/a.mp4"), 0);
    EXPECT_EQ(e->match("/tmp/record/a.mkv"), -1);
    EXPECT_EQ(e->match("/other/a.mp4"), -1);
}

TEST(PolicyEngine, PathGlobRule) {
    RetentionConfig c;
    auto r = rule("sessions", 12h);
    r.path_glob = "/mnt/session-*/**";
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/mnt/session-1/a"), 0);
    EXPECT_EQ(e->match("/mnt/session-1/x/y"), 0);
    EXPECT_EQ(e->match("/mnt/other/a"), -1);
}

TEST(PolicyEngine, ExclusionAlwaysWinsEvenAgainstHigherPriority) {
    RetentionConfig c;
    auto del = rule("delete-all-mp4", 1h);
    del.extensions = {"mp4"};
    del.priority = 1000;
    auto keep = rule("keep", std::nullopt);
    keep.action = RuleAction::exclude;
    keep.path = "/keep";
    keep.recursive = true;
    keep.priority = -1000;
    c.rules = {del, keep};
    const auto e = engine(c);
    const Decision d = e->evaluate("/keep/a.mp4", at(kEpoch2026));
    EXPECT_EQ(d.kind, Decision::Kind::excluded);
    EXPECT_EQ(d.rule_index, 1);
    EXPECT_EQ(e->evaluate("/other/a.mp4", at(kEpoch2026)).rule_index, 0);
}

TEST(PolicyEngine, PriorityBeatsSpecificity) {
    RetentionConfig c;
    auto specific = rule("specific", 1h);
    specific.file = "/a/b.mp4";
    auto broad = rule("broad", 5h);
    broad.extensions = {"mp4"};
    broad.priority = 1;
    c.rules = {specific, broad};
    EXPECT_EQ(engine(c)->match("/a/b.mp4"), 1);
}

TEST(PolicyEngine, MoreSpecificScopeWins) {
    RetentionConfig c;
    auto ext = rule("ext", 1h);
    ext.extensions = {"mp4"};
    auto dir = rule("dir", 2h);
    dir.path = "/tmp/record";
    dir.recursive = true;
    auto deeper = rule("deeper", 3h);
    deeper.path = "/tmp/record/sub";
    deeper.recursive = true;
    auto file = rule("file", 4h);
    file.file = "/tmp/record/sub/x.mp4";
    c.rules = {ext, dir, deeper, file};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/elsewhere/x.mp4"), 0);       // Only the extension rule.
    EXPECT_EQ(e->match("/tmp/record/x.mp4"), 1);      // Directory beats a bare pattern.
    EXPECT_EQ(e->match("/tmp/record/sub/y.mp4"), 2);  // Deeper directory wins.
    EXPECT_EQ(e->match("/tmp/record/sub/x.mp4"), 3);  // Exact file wins.
}

TEST(PolicyEngine, NonRecursiveBeatsRecursiveAtSameDepth) {
    RetentionConfig c;
    auto rec = rule("recursive", 1h);
    rec.path = "/d";
    rec.recursive = true;
    auto flat = rule("flat", 2h);
    flat.path = "/d";
    c.rules = {rec, flat};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/d/a"), 1);
    EXPECT_EQ(e->match("/d/x/a"), 0);
}

TEST(PolicyEngine, MorePatternLiteralsWinThenDeclarationOrder) {
    RetentionConfig c;
    auto loose = rule("loose", 1h);
    loose.glob = "*.mp4";
    auto tight = rule("tight", 2h);
    tight.glob = "rec-*.mp4";
    auto twin = rule("twin", 3h);
    twin.glob = "rec-*.mp4";
    c.rules = {loose, tight, twin};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/rec-1.mp4"), 1);  // tight and twin tie: the first declared wins.
    EXPECT_EQ(e->match("/x.mp4"), 0);
}

TEST(PolicyEngine, DisabledRulesAreIgnored) {
    RetentionConfig c;
    auto r = rule("off", 1h);
    r.extensions = {"mp4"};
    r.enabled = false;
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_EQ(e->match("/a.mp4"), -1);
    EXPECT_EQ(e->active_rule_count(), 0U);
}

TEST(PolicyEngine, NeverRetentionRule) {
    RetentionConfig c;
    c.default_retention = 1h;
    auto r = rule("forever", std::nullopt);
    r.path = "/archive";
    r.recursive = true;
    c.rules = {r};
    EXPECT_EQ(engine(c)->evaluate("/archive/a", at(kEpoch2026)).kind, Decision::Kind::keep_forever);
}

TEST(PolicyEngine, BirthTimestampAndFallback) {
    RetentionConfig c;
    auto r = rule("born", 1h);
    r.extensions = {"bin"};
    r.timestamp = TimestampSource::birth;
    c.rules = {r};
    const auto e = engine(c);
    EXPECT_TRUE(e->needs_birth_time());
    const TimePoint birth = kEpoch2026;
    const TimePoint mtime = kEpoch2026 + 10h;
    Decision d = e->evaluate("/a.bin", at(mtime, birth));
    EXPECT_EQ(d.expiry, birth + 1h);
    EXPECT_FALSE(d.birth_fallback);
    d = e->evaluate("/a.bin", at(mtime));
    EXPECT_EQ(d.expiry, mtime + 1h);
    EXPECT_TRUE(d.birth_fallback);
}

TEST(PolicyEngine, ExpiryComputationSaturates) {
    EXPECT_EQ(saturating_add(TimePoint::max() - 1ms, 1h), TimePoint::max());
    EXPECT_EQ(saturating_add(kEpoch2026, 1h), kEpoch2026 + 1h);
    RetentionConfig c;
    c.default_retention = Duration::max();
    const Decision d = engine(c)->evaluate("/f", at(kEpoch2026));
    EXPECT_EQ(d.expiry, TimePoint::max());
    EXPECT_FALSE(d.expired_at(kEpoch2026 + 100 * 365 * 24h));
}

TEST(PolicyEngine, FutureModificationTimeExpiresLater) {
    RetentionConfig c;
    c.default_retention = 1h;
    const auto e = engine(c);
    const Decision d = e->evaluate("/f", at(kEpoch2026 + 24h));
    EXPECT_FALSE(d.expired_at(kEpoch2026 + 2h));
}

TEST(PolicyEngine, RuleIdLookup) {
    RetentionConfig c;
    c.rules = {rule("a"), rule("b")};
    const auto e = engine(c);
    EXPECT_EQ(e->rule_id(1), "b");
    EXPECT_EQ(e->rule_id(7), "default");
}

}  // namespace
}  // namespace tsfs
