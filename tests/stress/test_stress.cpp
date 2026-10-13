// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/stress/test_stress.cpp
// Purpose : Stress tests: GC under churn, reloads during GC, logging during GC (TSFS_STRESS_SCALE)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <thread>

#include "gc/garbage_collector.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

int scale() {
    const char* s = std::getenv("TSFS_STRESS_SCALE");
    return s != nullptr ? std::max(1, std::atoi(s)) : 1;
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return predicate();
}

class StressTest : public ::testing::Test {
protected:
    void SetUp() override {
        fs::create_directories(root_);
        store_ = std::move(*BackingStore::open(root_.string(), {}));
    }
    test::TempDir dir_{"tsfs-stress"};
    fs::path root_{dir_ / "data"};
    std::unique_ptr<BackingStore> store_;
    SystemClock clock_;
    PathLockTable locks_;
    OpenFileRegistry open_files_;
};

/// Files are created, renamed, rewritten and deleted by 8 threads while the GC runs and
/// the configuration is reloaded repeatedly. Invariant: a file younger than its retention
/// is never deleted by the GC (every "young" file present at the end must exist).
TEST_F(StressTest, ChurnWithReloadsNeverDeletesYoungFiles) {
    Logger log;
    LoggingConfig lc;
    lc.directory = (dir_ / "logs").string();
    lc.level = LogLevel::info;  // gc_file_deleted lines: logging under sustained GC activity.
    lc.rotation.max_file_size = 256 * 1024;
    ASSERT_TRUE(log.configure(lc).has_value());

    const int files = 2000 * scale();
    for (int i = 0; i < files; ++i) {
        const fs::path p = root_ / ("old/d" + std::to_string(i % 50)) / ("f" + std::to_string(i));
        test::write_file(p);
        test::set_mtime(p, clock_.now() - 2h);
    }
    RetentionConfig retention;
    retention.default_retention = 1h;
    GcConfig gc_config;
    gc_config.scan_interval = 1s;
    gc_config.max_scan_duration = 5ms;
    gc_config.max_workers = 4;
    gc_config.max_deletions_per_second = 0;
    gc_config.max_pending_tasks = 64;  // Saturation on purpose, even in slow (sanitizer) builds.
    GarbageCollector gc({*store_, log, clock_, locks_, &open_files_, {}}, test::make_gc_snapshot(retention, gc_config));
    gc.start();

    std::atomic<bool> stop{false};
    std::atomic<int> young_created{0};
    {
        std::vector<std::jthread> churn;
        for (int t = 0; t < 8; ++t) {
            churn.emplace_back([&, t] {
                std::mt19937 rng(static_cast<unsigned>(t));
                int i = 0;
                while (!stop.load()) {
                    const fs::path p = root_ / ("young/t" + std::to_string(t)) / ("f" + std::to_string(i % 200));
                    std::error_code ec;
                    switch (rng() % 3) {
                        case 0:
                            test::write_file(p);
                            gc.notify_changed(p.string().substr(root_.string().size()));
                            break;
                        case 1:
                            fs::rename(p, p.string() + ".r", ec);
                            fs::rename(p.string() + ".r", p, ec);
                            break;
                        default: fs::remove(p, ec); break;
                    }
                    ++i;
                    young_created.fetch_add(1);
                }
            });
        }
        std::jthread reloader([&] {
            for (std::uint64_t g = 2; !stop.load(); ++g) {
                GcConfig next = gc_config;
                next.max_workers = 2 + static_cast<std::uint32_t>(g % 3);
                gc.apply(test::make_gc_snapshot(retention, next, g));
                std::this_thread::sleep_for(50ms);
            }
        });
        EXPECT_TRUE(eventually([&] { return gc.stats().deleted == static_cast<std::uint64_t>(files); }, 60s))
            << "deleted " << gc.stats().deleted << " of " << files;
        stop.store(true);
    }
    gc.stop();
    log.shutdown();
    EXPECT_GT(young_created.load(), 0);
    EXPECT_GT(gc.stats().queue_saturations, 0U);
    // Every remaining young file is intact; none of them was deleted by the GC (deleted ==
    // exactly the number of old files).
    EXPECT_EQ(gc.stats().deleted, static_cast<std::uint64_t>(files));
    EXPECT_GT(test::count_lines(dir_ / "logs/tsfs.log"), 0U);
}

TEST_F(StressTest, SchedulerChurn) {
    for (const SchedulerKind kind : {SchedulerKind::heap, SchedulerKind::ordered}) {
        auto s = make_scheduler(kind, 100000);
        std::mt19937 rng(1);
        const TimePoint base = test::kEpoch2026;
        for (int i = 0; i < 300000 * scale(); ++i) {
            s->schedule({"/f" + std::to_string(rng() % 50000),
                         {1, 1},
                         base + std::chrono::milliseconds(rng() % 1000000),
                         0,
                         0});
        }
        std::vector<GcCandidate> out;
        s->pop_due(TimePoint::max(), SIZE_MAX, out);
        EXPECT_EQ(s->size(), 0U) << to_string(kind);
        for (std::size_t i = 1; i < out.size(); ++i) {
            ASSERT_LE(out[i - 1].due, out[i].due);
        }
    }
}

}  // namespace
}  // namespace tsfs
