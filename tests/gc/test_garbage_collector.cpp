// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/gc/test_garbage_collector.cpp
// Purpose : Garbage collector on real temporary trees: expiry, safety checks, retries, limits, threads
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

#include "gc/garbage_collector.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using test::kEpoch2026;

/// Waits (polling, bounded) until @p predicate holds. @return the final value.
template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

class GcTest : public ::testing::Test {
protected:
    void SetUp() override {
        fs::create_directories(root_);
        auto store = BackingStore::open(root_.string(), {});
        ASSERT_TRUE(store.has_value()) << store.error().message;
        store_ = std::move(*store);
        log_.set_level(LogLevel::critical);  // Keep the test output readable.
    }

    std::unique_ptr<GarbageCollector> make(const RetentionConfig& retention, GcConfig gc = fast_config()) {
        return std::make_unique<GarbageCollector>(
            GarbageCollector::Dependencies{*store_, log_, clock_, locks_, &open_files_,
                                           [this](std::string_view p) {
                                               // Called concurrently by the worker threads.
                                               std::scoped_lock lock(invalidated_mutex_);
                                               invalidated_.emplace_back(p);
                                           }},
            test::make_gc_snapshot(retention, gc));
    }

    static GcConfig fast_config() {
        GcConfig gc;
        gc.scan_interval = 1s;
        gc.max_deletions_per_second = 0;
        gc.retry_initial_delay = 10ms;
        gc.retry_max_delay = 40ms;
        return gc;
    }

    /// A one-rule configuration: everything under "/" expires after @p retention.
    static RetentionConfig everything(Duration retention = 1h) {
        RetentionConfig c;
        c.default_retention = retention;
        return c;
    }

    /// Create @p relative with an mtime @p age before the (manual) now.
    fs::path file(std::string_view relative, Duration age, std::string_view content = "data") {
        const fs::path p = root_ / relative;
        test::write_file(p, content);
        test::set_mtime(p, clock_.now() - age);
        return p;
    }

    test::TempDir dir_;
    fs::path root_{dir_ / "data"};
    fs::path outside_{dir_ / "outside"};
    ManualClock clock_{kEpoch2026};
    Logger log_;
    PathLockTable locks_;
    OpenFileRegistry open_files_;
    std::unique_ptr<BackingStore> store_;
    std::mutex invalidated_mutex_;
    std::vector<std::string> invalidated_;
};

/// tsfs --purge / SIGUSR2: every expired file at once, rate limit and cycle budget
/// ignored, exclusions and young files kept, and the scheduler bound does not leave
/// files behind (several passes).
TEST_F(GcTest, PurgeDeletesEveryExpiredFileAtOnce) {
    for (int i = 0; i < 300; ++i) {
        file(std::format("old/f{}", i), 2h);
    }
    const auto young = file("young", 10min);
    const auto kept = file("old/x.keep", 2h);
    RetentionConfig retention = everything(1h);
    auto keep = test::rule("keep", std::nullopt);
    keep.action = RuleAction::exclude;
    keep.glob = "*.keep";
    retention.rules.push_back(keep);
    GcConfig gc_config = fast_config();
    gc_config.scan_interval = 1h;            // No periodic pass during the test.
    gc_config.max_deletions_per_second = 1;  // Would take 5 minutes without the purge.
    gc_config.max_deletions_per_cycle = 10;
    gc_config.max_pending_tasks = 64;  // Fewer slots than expired files: several passes.
    auto gc = make(retention, gc_config);
    gc->start();
    ASSERT_TRUE(eventually([&] { return gc->stats().scans_completed >= 1; }));
    gc->request_purge();
    ASSERT_TRUE(eventually([&] { return gc->stats().purges_completed == 1; }, 30s));
    const GcStats s = gc->stats();
    EXPECT_EQ(s.purging, 0U);
    EXPECT_EQ(s.last_purge_failed, 0U);
    EXPECT_GE(s.last_purge_deleted + 2U, 300U);  // At most a token or two spent before the purge.
    EXPECT_EQ(s.deleted, 300U);
    EXPECT_TRUE(fs::exists(young));
    EXPECT_TRUE(fs::exists(kept));
    gc->stop();
}

TEST_F(GcTest, PurgeWorksWithTheGcDisabledAndHonorsDryRun) {
    const auto old_file = file("old", 2h);
    GcConfig gc_config = fast_config();
    gc_config.enabled = false;
    gc_config.dry_run = true;
    auto gc = make(everything(1h), gc_config);
    gc->start();
    gc->request_purge();
    ASSERT_TRUE(eventually([&] { return gc->stats().purges_completed == 1; }));
    EXPECT_TRUE(fs::exists(old_file));        // dry_run: nothing deleted...
    EXPECT_EQ(gc->stats().would_delete, 1U);  // ...but the file was examined.
    gc->stop();
}

TEST_F(GcTest, EmptyStorage) {
    auto gc = make(everything());
    gc->run_full_scan();
    EXPECT_EQ(gc->process_due_now(), 0U);
    const GcStats s = gc->stats();
    EXPECT_EQ(s.scans_completed, 1U);
    EXPECT_EQ(s.last_scan_files, 0U);
}

TEST_F(GcTest, DeletesOneExpiredFileAndKeepsValidOnes) {
    const auto old_file = file("a/old.bin", 2h);
    const auto young = file("a/young.bin", 10min);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(old_file));
    EXPECT_TRUE(fs::exists(young));
    EXPECT_EQ(gc->stats().deleted, 1U);
    EXPECT_EQ(gc->stats().bytes_deleted, 4U);
    EXPECT_EQ(invalidated_, std::vector<std::string>{"/a/old.bin"});
}

TEST_F(GcTest, ExactlyAtExpiryIsDeleted) {
    const auto f = file("f", 1h);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, OneMillisecondBeforeExpiryIsKept) {
    const auto f = file("f", 1h - 1ms);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().scheduled, 1U);  // Within the horizon: scheduled for later.
    clock_.advance(1ms);
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, MixedExpiredAndValidFiles) {
    for (int i = 0; i < 50; ++i) {
        file("old/f" + std::to_string(i), 3h);
        file("new/f" + std::to_string(i), 1min);
    }
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_EQ(gc->stats().deleted, 50U);
    EXPECT_EQ(std::distance(fs::directory_iterator(root_ / "old"), fs::directory_iterator{}), 0);
    EXPECT_EQ(std::distance(fs::directory_iterator(root_ / "new"), fs::directory_iterator{}), 50);
}

TEST_F(GcTest, DryRunUsesSameChecksAndDeletesNothing) {
    const auto f = file("x/old.mp4", 2h);
    file("x/new.mp4", 1min);
    auto gc = make(everything(1h));
    const auto report = gc->dry_run_report();
    ASSERT_EQ(report.size(), 1U);
    EXPECT_EQ(report[0].path, "/x/old.mp4");
    EXPECT_EQ(report[0].rule, "default");
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().would_delete, 1U);
    EXPECT_EQ(gc->stats().deleted, 0U);
}

TEST_F(GcTest, DryRunModeFromConfiguration) {
    const auto f = file("old", 2h);
    GcConfig gc_config = fast_config();
    gc_config.dry_run = true;
    auto gc = make(everything(1h), gc_config);
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().would_delete, 1U);
}

TEST_F(GcTest, FileModifiedAfterSchedulingIsNotDeleted) {
    const auto f = file("f", 2h);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    test::set_mtime(f, clock_.now());  // Written again before the worker runs.
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().skipped_modified, 1U);
    EXPECT_EQ(gc->stats().scheduled, 0U);  // New expiry is beyond the horizon (2 x 1s).
}

TEST_F(GcTest, ReplacedFileIsNeverDeletedOnTheStaleRecord) {
    const auto f = file("f", 2h, "original");
    auto gc = make(everything(1h));
    gc->run_full_scan();
    // Atomic replacement: a new inode under the same name, with an old timestamp.
    const fs::path tmp = root_ / "f.tmp";
    test::write_file(tmp, "replacement");
    test::set_mtime(tmp, clock_.now() - 2h);
    fs::rename(tmp, f);
    gc->process_due_now();
    const GcStats s = gc->stats();
    EXPECT_EQ(s.skipped_replaced, 1U);
    // Re-evaluated: the replacement is itself expired, so it is rescheduled with its own
    // identity and deleted by a later, fully validated attempt.
    EXPECT_EQ(s.deleted, 1U);
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, RenamedFileIsNotFollowedToItsNewName) {
    const auto f = file("tmp/record/a.mp4", 2h);
    RetentionConfig c;
    auto r = test::rule("rec", 1h);
    r.path = "/tmp/record";
    c.rules = {r};
    auto gc = make(c);
    gc->run_full_scan();
    fs::create_directories(root_ / "keep");
    fs::rename(f, root_ / "keep/a.mp4");
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(root_ / "keep/a.mp4"));
    EXPECT_EQ(gc->stats().vanished, 1U);
}

TEST_F(GcTest, FileDeletedExternallyIsIdempotent) {
    const auto f = file("f", 2h);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    fs::remove(f);
    gc->process_due_now();
    EXPECT_EQ(gc->stats().vanished, 1U);
    EXPECT_EQ(gc->stats().failed, 0U);
}

TEST_F(GcTest, OpenFileIsSkippedAndRetriedLater) {
    const auto f = file("f", 2h);
    struct stat st{};
    ASSERT_EQ(::stat(f.c_str(), &st), 0);
    open_files_.add({st.st_dev, st.st_ino});
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().skipped_open, 1U);
    open_files_.remove({st.st_dev, st.st_ino});
    clock_.advance(1s);
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, ExclusionAddedByReloadPreventsDeletion) {
    const auto f = file("vip/a.bin", 2h);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    RetentionConfig next = everything(1h);
    auto keep = test::rule("vip", std::nullopt);
    keep.action = RuleAction::exclude;
    keep.path = "/vip";
    next.rules = {keep};
    gc->apply(test::make_gc_snapshot(next, fast_config(), 2));
    gc->process_due_now();  // The stale schedule entry is re-evaluated with the new policy.
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().config_generation, 2U);
}

TEST_F(GcTest, ClockStepBackwardsDoesNotDeleteEarly) {
    const auto f = file("f", 59min);
    clock_.advance(2min);  // Clock jumps forward: the file looks expired...
    auto gc = make(everything(1h));
    gc->run_full_scan();
    clock_.advance(-10min);  // ...then the clock is corrected before deletion.
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().deleted, 0U);
    clock_.advance(9min);  // Real expiry reached: now it goes.
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, PermissionFailureIsRetriedThenGivenUp) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses directory permissions";
    }
    const auto f = file("locked/f", 2h);
    fs::permissions(root_ / "locked", fs::perms::owner_read | fs::perms::owner_exec);
    GcConfig gc_config = fast_config();
    gc_config.retry_limit = 3;
    auto gc = make(everything(1h), gc_config);
    gc->run_full_scan();
    for (int i = 0; i < 10; ++i) {
        gc->process_due_now();
        clock_.advance(1s);
    }
    const GcStats s = gc->stats();
    EXPECT_EQ(s.retries, 3U);
    EXPECT_EQ(s.failed, 1U);
    EXPECT_EQ(s.scheduled, 0U);  // Bounded: no infinite retry.
    fs::permissions(root_ / "locked", fs::perms::owner_all);
    EXPECT_TRUE(fs::exists(f));
}

TEST_F(GcTest, SymlinksAreNeverFollowedNorDeleted) {
    fs::create_directories(outside_);
    const auto target = outside_ / "precious.bin";
    test::write_file(target);
    test::set_mtime(target, clock_.now() - 10h);
    fs::create_directory_symlink(outside_, root_ / "dirlink");
    fs::create_symlink(target, root_ / "filelink");
    const timespec old_times[2] = {{0, 0}, {0, 0}};
    ::utimensat(AT_FDCWD, (root_ / "filelink").c_str(), old_times, AT_SYMLINK_NOFOLLOW);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(target));
    EXPECT_TRUE(fs::is_symlink(root_ / "filelink"));
    EXPECT_TRUE(fs::is_symlink(root_ / "dirlink"));
    EXPECT_EQ(gc->stats().deleted, 0U);
}

TEST_F(GcTest, DirectorySwappedForSymlinkIsRefused) {
    const auto f = file("d/victim", 2h);
    fs::create_directories(outside_);
    test::write_file(outside_ / "victim");
    test::set_mtime(outside_ / "victim", clock_.now() - 2h);
    auto gc = make(everything(1h));
    gc->run_full_scan();
    // An attacker with write access to the backing tree swaps "d" for a link.
    fs::rename(root_ / "d", root_ / "d.orig");
    fs::create_directory_symlink(outside_, root_ / "d");
    gc->process_due_now();
    EXPECT_TRUE(fs::exists(outside_ / "victim"));
    EXPECT_EQ(gc->stats().skipped_unsafe, 1U);
    (void)f;
}

TEST_F(GcTest, HardLinkedFileOnlyLosesTheExpiredName) {
    const auto a = file("a/name1", 2h);
    fs::create_directories(root_ / "b");
    fs::create_hard_link(a, root_ / "b/name2");
    RetentionConfig c;
    auto r = test::rule("a-only", 1h);
    r.path = "/a";
    c.rules = {r};
    auto gc = make(c);
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(a));
    EXPECT_TRUE(fs::exists(root_ / "b/name2"));
    EXPECT_EQ(gc->stats().bytes_deleted, 0U);  // The data is still referenced.
}

TEST_F(GcTest, QueueSaturationIsBoundedAndRecoveredByLaterPasses) {
    for (int i = 0; i < 100; ++i) {
        file("f" + std::to_string(i), 2h);
    }
    GcConfig gc_config = fast_config();
    gc_config.max_pending_tasks = 16;
    gc_config.batch_size = 8;
    auto gc = make(everything(1h), gc_config);
    for (int pass = 0; pass < 10 && gc->stats().deleted < 100; ++pass) {
        gc->run_full_scan();
        EXPECT_LE(gc->stats().scheduled, 16U);
        gc->process_due_now();
    }
    EXPECT_EQ(gc->stats().deleted, 100U);
    EXPECT_GT(gc->stats().queue_saturations, 0U);
}

TEST_F(GcTest, RestartRecoversWithoutPersistentState) {
    const auto f = file("f", 2h);
    {
        auto gc = make(everything(1h));
        gc->run_full_scan();  // Scheduled, then "crash" before deletion.
    }
    auto gc = make(everything(1h));
    gc->run_full_scan();
    gc->process_due_now();
    EXPECT_FALSE(fs::exists(f));
}

TEST_F(GcTest, ThreadedPipelineDeletesManyFiles) {
    for (int d = 0; d < 20; ++d) {
        for (int i = 0; i < 100; ++i) {
            file("d" + std::to_string(d) + "/f" + std::to_string(i), 2h);
        }
    }
    file("keep/young", 1min);
    GcConfig gc_config = fast_config();
    gc_config.max_workers = 4;
    gc_config.max_scan_duration = 20ms;  // Several slices.
    auto gc = make(everything(1h), gc_config);
    gc->start();
    EXPECT_TRUE(eventually([&] { return gc->stats().deleted == 2000; }));
    gc->stop();
    EXPECT_TRUE(fs::exists(root_ / "keep/young"));
    EXPECT_EQ(gc->stats().workers, 0U);
}

TEST_F(GcTest, NotificationSchedulesWithoutWaitingForAScan) {
    GcConfig gc_config = fast_config();
    gc_config.scan_interval = 1h;  // Only the first pass runs during the test.
    auto gc = make(everything(1h), gc_config);
    gc->start();
    EXPECT_TRUE(eventually([&] { return gc->stats().scans_completed == 1; }));
    const auto f = file("late/arrival", 2h);
    gc->notify_changed("/late/arrival");
    EXPECT_TRUE(eventually([&] { return !fs::exists(f); }));
    gc->stop();
}

TEST_F(GcTest, DeletionRateIsBounded) {
    for (int i = 0; i < 60; ++i) {
        file("f" + std::to_string(i), 2h);
    }
    GcConfig gc_config = fast_config();
    gc_config.max_deletions_per_second = 100;
    gc_config.max_workers = 4;
    auto gc = make(everything(1h), gc_config);
    const auto t0 = std::chrono::steady_clock::now();
    gc->start();
    EXPECT_TRUE(eventually([&] { return gc->stats().deleted == 60; }));
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    gc->stop();
    EXPECT_GE(elapsed, 450ms);  // 60 deletions at 100/s with a 10-token burst: >= 0.5 s.
}

TEST_F(GcTest, PerCycleBudgetLimitsDeletions) {
    for (int i = 0; i < 30; ++i) {
        file("f" + std::to_string(i), 2h);
    }
    GcConfig gc_config = fast_config();
    gc_config.scan_interval = 1h;
    gc_config.max_deletions_per_cycle = 10;
    auto gc = make(everything(1h), gc_config);
    gc->start();
    EXPECT_TRUE(eventually([&] { return gc->stats().deleted == 10; }));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(gc->stats().deleted, 10U);
    gc->stop();
}

TEST_F(GcTest, DisabledCollectorDoesNothing) {
    const auto f = file("f", 2h);
    GcConfig gc_config = fast_config();
    gc_config.enabled = false;
    auto gc = make(everything(1h), gc_config);
    gc->start();
    std::this_thread::sleep_for(200ms);
    gc->stop();
    EXPECT_TRUE(fs::exists(f));
    EXPECT_EQ(gc->stats().scans_completed, 0U);
}

TEST_F(GcTest, ShutdownWithPendingTasksIsFast) {
    for (int i = 0; i < 500; ++i) {
        file("f" + std::to_string(i), 2h);
    }
    GcConfig gc_config = fast_config();
    gc_config.max_deletions_per_second = 20;
    auto gc = make(everything(1h), gc_config);
    gc->start();
    EXPECT_TRUE(eventually([&] { return gc->stats().deleted >= 1; }));
    const auto t0 = std::chrono::steady_clock::now();
    gc->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
    EXPECT_LT(gc->stats().deleted, 500U);
}

TEST_F(GcTest, WorkerCountFollowsReload) {
    auto gc = make(everything(1h));
    gc->start();
    EXPECT_EQ(gc->stats().workers, 2U);
    GcConfig more = fast_config();
    more.max_workers = 5;
    gc->apply(test::make_gc_snapshot(everything(1h), more, 2));
    EXPECT_EQ(gc->stats().workers, 5U);
    gc->stop();
}

TEST_F(GcTest, ConcurrentCreationNeverLosesFreshFiles) {
    for (int i = 0; i < 300; ++i) {
        file("old/f" + std::to_string(i), 2h);
    }
    GcConfig gc_config = fast_config();
    gc_config.max_workers = 4;
    auto gc = make(everything(1h), gc_config);
    gc->start();
    std::vector<fs::path> fresh;
    for (int i = 0; i < 300; ++i) {
        fresh.push_back(file("new/f" + std::to_string(i), 0ms));
        gc->notify_changed("/new/f" + std::to_string(i));
    }
    EXPECT_TRUE(eventually([&] { return gc->stats().deleted == 300; }));
    gc->stop();
    for (const auto& p : fresh) {
        ASSERT_TRUE(fs::exists(p)) << p;
    }
}

}  // namespace
}  // namespace tsfs
