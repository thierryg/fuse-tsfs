// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/gc/test_scheduler.cpp
// Purpose : Expiration schedulers (both strategies), token bucket, rate limiter, small utilities
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <gtest/gtest.h>

#include <random>
#include <thread>

#include "fsutil/open_file_registry.hpp"
#include "fsutil/path_lock_table.hpp"
#include "gc/scheduler.hpp"
#include "gc/token_bucket.hpp"
#include "logging/rate_limiter.hpp"
#include "metrics/sharded_counter.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

using namespace std::chrono_literals;
using test::kEpoch2026;

GcCandidate cand(std::string path, TimePoint due, ino_t ino = 1) { return {std::move(path), {1, ino}, due, 0, 0}; }

class SchedulerTest : public ::testing::TestWithParam<SchedulerKind> {
protected:
    std::unique_ptr<ExpirationScheduler> make(std::size_t capacity = 100) {
        return make_scheduler(GetParam(), capacity);
    }
};

TEST_P(SchedulerTest, PopsInDueOrderAndOnlyWhenDue) {
    auto s = make();
    s->schedule(cand("/c", kEpoch2026 + 3s));
    s->schedule(cand("/a", kEpoch2026 + 1s));
    s->schedule(cand("/b", kEpoch2026 + 2s));
    EXPECT_EQ(*s->next_due(), kEpoch2026 + 1s);
    std::vector<GcCandidate> out;
    EXPECT_EQ(s->pop_due(kEpoch2026, 10, out), 0U);
    EXPECT_EQ(s->pop_due(kEpoch2026 + 2s, 10, out), 2U);
    ASSERT_EQ(out.size(), 2U);
    EXPECT_EQ(out[0].path, "/a");
    EXPECT_EQ(out[1].path, "/b");
    EXPECT_EQ(s->size(), 1U);
}

TEST_P(SchedulerTest, ExactlyDueIsPopped) {
    auto s = make();
    s->schedule(cand("/a", kEpoch2026));
    std::vector<GcCandidate> out;
    EXPECT_EQ(s->pop_due(kEpoch2026, 10, out), 1U);
}

TEST_P(SchedulerTest, RescheduleReplacesTheEntry) {
    auto s = make();
    EXPECT_EQ(s->schedule(cand("/a", kEpoch2026 + 1s)), ScheduleResult::inserted);
    EXPECT_EQ(s->schedule(cand("/a", kEpoch2026 + 10s, 2)), ScheduleResult::updated);
    EXPECT_EQ(s->size(), 1U);
    std::vector<GcCandidate> out;
    EXPECT_EQ(s->pop_due(kEpoch2026 + 5s, 10, out), 0U);  // The old due time is gone.
    EXPECT_EQ(s->pop_due(kEpoch2026 + 10s, 10, out), 1U);
    EXPECT_EQ(out[0].identity.inode, 2U);
    EXPECT_EQ(s->find("/a"), nullptr);
}

TEST_P(SchedulerTest, CancelRemoves) {
    auto s = make();
    s->schedule(cand("/a", kEpoch2026));
    EXPECT_NE(s->find("/a"), nullptr);
    EXPECT_TRUE(s->cancel("/a"));
    EXPECT_FALSE(s->cancel("/a"));
    EXPECT_EQ(s->size(), 0U);
    EXPECT_FALSE(s->next_due().has_value());
}

TEST_P(SchedulerTest, CapacityIsEnforcedButUpdatesStillWork) {
    auto s = make(2);
    EXPECT_EQ(s->schedule(cand("/a", kEpoch2026)), ScheduleResult::inserted);
    EXPECT_EQ(s->schedule(cand("/b", kEpoch2026)), ScheduleResult::inserted);
    EXPECT_EQ(s->schedule(cand("/c", kEpoch2026)), ScheduleResult::rejected_full);
    EXPECT_EQ(s->schedule(cand("/a", kEpoch2026 + 1s)), ScheduleResult::updated);
    EXPECT_EQ(s->size(), 2U);
}

TEST_P(SchedulerTest, PopRespectsMax) {
    auto s = make();
    for (int i = 0; i < 10; ++i) {
        s->schedule(cand("/f" + std::to_string(i), kEpoch2026));
    }
    std::vector<GcCandidate> out;
    EXPECT_EQ(s->pop_due(kEpoch2026, 3, out), 3U);
    EXPECT_EQ(s->size(), 7U);
}

TEST_P(SchedulerTest, RandomizedAgainstReferenceModel) {
    auto s = make(1000);
    std::map<std::string, TimePoint> model;
    std::mt19937 rng(42);
    for (int step = 0; step < 20000; ++step) {
        const std::string path = "/f" + std::to_string(rng() % 300);
        const auto op = rng() % 10;
        if (op < 6) {
            const TimePoint due = kEpoch2026 + std::chrono::milliseconds(rng() % 100000);
            s->schedule(cand(path, due));
            model[path] = due;
        } else if (op < 8) {
            EXPECT_EQ(s->cancel(path), model.erase(path) == 1);
        } else {
            const TimePoint now = kEpoch2026 + std::chrono::milliseconds(rng() % 100000);
            std::vector<GcCandidate> out;
            s->pop_due(now, 1000000, out);
            std::size_t expected = 0;
            for (auto it = model.begin(); it != model.end();) {
                if (it->second <= now) {
                    ++expected;
                    it = model.erase(it);
                } else {
                    ++it;
                }
            }
            ASSERT_EQ(out.size(), expected);
            for (std::size_t i = 1; i < out.size(); ++i) {
                ASSERT_LE(out[i - 1].due, out[i].due);
            }
        }
        ASSERT_EQ(s->size(), model.size());
    }
}

INSTANTIATE_TEST_SUITE_P(Strategies, SchedulerTest, ::testing::Values(SchedulerKind::heap, SchedulerKind::ordered),
                         [](const auto& param_info) { return std::string(to_string(param_info.param)); });

TEST(HeapScheduler, TombstonesAreCompacted) {
    HeapScheduler s(10);
    for (int i = 0; i < 100000; ++i) {
        s.schedule(cand("/same", kEpoch2026 + std::chrono::milliseconds(i)));
    }
    EXPECT_EQ(s.size(), 1U);
    EXPECT_LE(s.heap_size(), 2 * s.size() + 1024 + 1);
}

// --- token bucket ------------------------------------------------------------------

TEST(TokenBucket, UnlimitedNeverWaits) {
    TokenBucket bucket(0);
    std::stop_source stop;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(bucket.acquire(stop.get_token()));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
}

TEST(TokenBucket, EnforcesTheRate) {
    TokenBucket bucket(200);  // 200/s: 60 tokens need >= ~250 ms after the 20-token burst.
    std::stop_source stop;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        ASSERT_TRUE(bucket.acquire(stop.get_token()));
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, 180ms);
    EXPECT_LT(elapsed, 2s);
}

TEST(TokenBucket, StopInterruptsAWait) {
    TokenBucket bucket(1);
    std::stop_source stop;
    ASSERT_TRUE(bucket.acquire(stop.get_token()));  // Burst token.
    std::jthread stopper([&] {
        std::this_thread::sleep_for(50ms);
        stop.request_stop();
    });
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(bucket.acquire(stop.get_token()));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 900ms);
}

// --- misc utilities -----------------------------------------------------------------

TEST(LogRateLimiter, AllowsBurstThenCountsSuppressed) {
    LogRateLimiter limiter(2, 1min);
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(limiter.check("k", t0).allowed);
    EXPECT_TRUE(limiter.check("k", t0).allowed);
    EXPECT_FALSE(limiter.check("k", t0).allowed);
    EXPECT_FALSE(limiter.check("k", t0).allowed);
    EXPECT_TRUE(limiter.check("other", t0).allowed);
    EXPECT_EQ(limiter.pending_suppressed(), 2U);
    const auto next = limiter.check("k", t0 + 2min);
    EXPECT_TRUE(next.allowed);
    EXPECT_EQ(next.suppressed_before, 2U);
}

TEST(ShardedCounters, SumsAcrossThreads) {
    ShardedCounters<2> counters;
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 10000; ++i) {
                    counters.add(0);
                    counters.add(1, 2);
                }
            });
        }
    }
    EXPECT_EQ(counters.sum(0), 80000U);
    EXPECT_EQ(counters.sum(1), 160000U);
}

TEST(OpenFileRegistry, CountsHandles) {
    OpenFileRegistry registry;
    const FileIdentity a{1, 10};
    registry.add(a);
    registry.add(a);
    EXPECT_TRUE(registry.is_open(a));
    registry.remove(a);
    EXPECT_TRUE(registry.is_open(a));
    registry.remove(a);
    EXPECT_FALSE(registry.is_open(a));
    registry.remove(a);  // Unbalanced release is harmless.
    EXPECT_EQ(registry.handles(), 0U);
}

TEST(PathLockTable, AncestorLocksDoNotDeadlockWithPairLocks) {
    PathLockTable table;
    std::atomic<int> done{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 2000; ++i) {
                    if (t % 2 == 0) {
                        auto g = table.lock_with_ancestors("/a/b/c/d" + std::to_string(i % 7));
                    } else {
                        auto g = table.lock("/a/b", "/x/y" + std::to_string(i % 5));
                    }
                }
                ++done;
            });
        }
    }
    EXPECT_EQ(done.load(), 4);
}

}  // namespace
}  // namespace tsfs
