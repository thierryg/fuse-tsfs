// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/bench_gc.cpp
// Purpose : Garbage collector benchmarks on real on-disk datasets
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// Real files on the backing filesystem (TSFS_BENCH_DIR, else $TMPDIR or /tmp), no
// FUSE in the path: these numbers measure the GC alone. Results depend heavily on
// the storage device, the backing filesystem and the page cache (the dataset is
// written just before the measurement, so the dentry/inode caches are warm).
//
// Datasets: 1,000 and 100,000 files by default; 1,000,000 with TSFS_BENCH_LARGE=1.
// Layout: "flat" = one directory, "tree" = directories of 100 files.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <format>
#include <thread>

#include "bench_support.hpp"
#include "common/clock.hpp"
#include "fsutil/backing_store.hpp"
#include "fsutil/open_file_registry.hpp"
#include "fsutil/path_lock_table.hpp"
#include "logging/logger.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

enum Layout : int { flat = 0, tree = 1 };

/// Create @p n files; @p expired_percent of them are 2 h old (expired under the 1 h
/// rule), the others are fresh. With @p mixed_sizes, sizes cycle 0 / 1 KiB / 16 KiB / 64 KiB.
void make_dataset(const fs::path& root, std::size_t n, Layout layout, int expired_percent, bool mixed_sizes) {
    static const std::string block(64 * 1024, 'x');
    const auto old_time = std::chrono::system_clock::now() - 2h;
    const timespec old_ts{std::chrono::system_clock::to_time_t(old_time), 0};
    const timespec times[2] = {old_ts, old_ts};
    std::string current_dir;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string dir =
            layout == flat ? root.string() + "/flat" : std::format("{}/d{}", root.string(), i / 100);
        if (dir != current_dir) {
            fs::create_directories(dir);
            current_dir = dir;
        }
        const std::string file = std::format("{}/f{}.bin", dir, i);
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            throw std::runtime_error("cannot create " + file);
        }
        if (mixed_sizes) {
            static constexpr std::size_t kSizes[] = {0, 1024, 16 * 1024, 64 * 1024};
            if (const std::size_t size = kSizes[i % 4]; size > 0 && ::write(fd, block.data(), size) < 0) {
                ::close(fd);
                throw std::runtime_error("cannot write " + file);
            }
        }
        if (static_cast<int>(i % 100) < expired_percent) {
            ::futimens(fd, times);
        }
        ::close(fd);
    }
}

RetentionConfig one_hour_everywhere() {
    RetentionConfig c;
    c.default_retention = 1h;
    return c;
}

GcConfig unthrottled(std::uint32_t workers, std::size_t capacity) {
    GcConfig gc;
    gc.max_workers = workers;
    gc.max_deletions_per_second = 0;
    gc.max_pending_tasks = capacity;
    gc.scan_interval = 1h;  // One pass per measurement; the next one is far away.
    gc.max_scan_duration = 1s;
    return gc;
}

/// Everything a GarbageCollector needs, over a dataset directory.
struct Harness {
    explicit Harness(const fs::path& root) : store(std::move(*BackingStore::open(root.string(), {}))) {
        log.set_level(LogLevel::error);  // Measure the GC, not the per-file info lines.
    }
    std::unique_ptr<BackingStore> store;
    Logger log;
    SystemClock clock;
    PathLockTable locks;
    OpenFileRegistry open_files;

    [[nodiscard]] GarbageCollector::Dependencies deps() { return {*store, log, clock, locks, &open_files, {}}; }
};

void sizes(benchmark::internal::Benchmark* b, bool with_layout) {
    for (const std::int64_t n : {1000, 100000}) {
        if (with_layout) {
            b->Args({n, flat});
            b->Args({n, tree});
        } else {
            b->Args({n, tree});
        }
    }
    if (bench::large_enabled()) {
        b->Args({1000000, tree});
    }
}

/// Discovery + scheduling: one full reconciliation pass. Arg 2 = expired percentage.
void BM_GcDiscover(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto layout = static_cast<Layout>(state.range(1));
    const int expired = static_cast<int>(state.range(2));
    bench::ScratchDir dir("tsfs-bench-gc");
    make_dataset(dir.path(), n, layout, expired, false);
    Harness h(dir.path());
    GcStats last{};
    for (auto _ : state) {
        GarbageCollector gc(h.deps(), bench::make_snapshot(one_hour_everywhere(), unthrottled(1, n)));
        gc.run_full_scan();
        last = gc.stats();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.counters["files"] = static_cast<double>(last.last_scan_files);
    state.counters["queue_depth"] = static_cast<double>(last.scheduled);
    state.counters["peak_rss_MiB"] = static_cast<double>(bench::peak_rss_kib()) / 1024.0;
    state.SetLabel(std::format("{} expired={}%", layout == flat ? "flat" : "tree", expired));
}
BENCHMARK(BM_GcDiscover)
    ->Apply([](benchmark::internal::Benchmark* b) {
        for (const std::int64_t n : {1000, 100000}) {
            for (const std::int64_t layout : {flat, tree}) {
                b->Args({n, layout, 100});
                b->Args({n, layout, 10});
            }
        }
        if (bench::large_enabled()) {
            b->Args({1000000, tree, 10});
        }
    })
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime()
    ->UseRealTime();

/// Validation + deletion in the calling thread (process_due_now), mixed file sizes.
/// The dataset is recreated (untimed) before every iteration.
void BM_GcDeleteSingleThread(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto layout = static_cast<Layout>(state.range(1));
    bench::ScratchDir dir("tsfs-bench-gc");
    std::uint64_t bytes = 0;
    for (auto _ : state) {
        state.PauseTiming();
        fs::remove_all(dir.path() / "flat");
        for (const auto& e : fs::directory_iterator(dir.path())) {
            fs::remove_all(e.path());
        }
        make_dataset(dir.path(), n, layout, 100, true);
        Harness h(dir.path());
        GarbageCollector gc(h.deps(), bench::make_snapshot(one_hour_everywhere(), unthrottled(1, n)));
        gc.run_full_scan();
        state.ResumeTiming();
        gc.process_due_now();
        state.PauseTiming();
        const GcStats s = gc.stats();
        if (s.deleted != n) {
            state.SkipWithError(std::format("deleted {} of {}", s.deleted, n));
        }
        bytes += s.bytes_deleted;
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.SetBytesProcessed(static_cast<std::int64_t>(bytes));
    state.SetLabel(layout == flat ? "flat" : "tree");
}
BENCHMARK(BM_GcDeleteSingleThread)
    ->Apply([](benchmark::internal::Benchmark* b) { sizes(b, true); })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->UseRealTime();

/// End to end with the real threads: start() until every expired file is gone
/// (discovery, scheduling, dispatch, validation, deletion). Arg 1 = worker count.
void BM_GcEndToEnd(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    const auto workers = static_cast<std::uint32_t>(state.range(1));
    bench::ScratchDir dir("tsfs-bench-gc");
    double busy_ratio = 0;
    for (auto _ : state) {
        state.PauseTiming();
        for (const auto& e : fs::directory_iterator(dir.path())) {
            fs::remove_all(e.path());
        }
        make_dataset(dir.path(), n, tree, 100, false);
        Harness h(dir.path());
        GarbageCollector gc(h.deps(), bench::make_snapshot(one_hour_everywhere(), unthrottled(workers, 100000)));
        const auto t0 = std::chrono::steady_clock::now();
        state.ResumeTiming();
        gc.start();
        while (gc.stats().deleted < n) {
            std::this_thread::sleep_for(1ms);
        }
        state.PauseTiming();
        const auto wall_us =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        const GcStats s = gc.stats();
        busy_ratio = wall_us > 0 ? static_cast<double>(s.worker_busy_us) / (static_cast<double>(wall_us) * workers) : 0;
        gc.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.counters["worker_utilization"] = busy_ratio;
    state.SetLabel(std::format("workers={}", workers));
}
BENCHMARK(BM_GcEndToEnd)
    ->Apply([](benchmark::internal::Benchmark* b) {
        for (const std::int64_t n : {1000, 100000}) {
            for (const std::int64_t w : {1, 2, 4, 8}) {
                b->Args({n, w});
            }
        }
    })
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3)
    ->UseRealTime();

/// Startup / recovery: time from start() to the end of the first reconciliation pass
/// over N young files (nothing to delete: the pass rebuilds the in-memory state).
void BM_GcStartupReconciliation(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    bench::ScratchDir dir("tsfs-bench-gc");
    make_dataset(dir.path(), n, tree, 0, false);
    Harness h(dir.path());
    for (auto _ : state) {
        GarbageCollector gc(h.deps(), bench::make_snapshot(one_hour_everywhere(), unthrottled(2, 100000)));
        gc.start();
        while (gc.stats().scans_completed == 0) {
            std::this_thread::sleep_for(1ms);
        }
        state.PauseTiming();
        gc.stop();
        state.ResumeTiming();
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_GcStartupReconciliation)
    ->Apply([](benchmark::internal::Benchmark* b) { sizes(b, false); })
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

/// Configuration reload: apply() a new generation to a running GC, until the
/// reconciliation pass it triggers has completed over N files.
void BM_GcReload(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(0));
    bench::ScratchDir dir("tsfs-bench-gc");
    make_dataset(dir.path(), n, tree, 0, false);
    Harness h(dir.path());
    GarbageCollector gc(h.deps(), bench::make_snapshot(one_hour_everywhere(), unthrottled(2, 100000)));
    gc.start();
    while (gc.stats().scans_completed == 0) {
        std::this_thread::sleep_for(1ms);
    }
    std::uint64_t generation = 2;
    for (auto _ : state) {
        const auto before = gc.stats().scans_completed;
        GcConfig next = unthrottled(1 + static_cast<std::uint32_t>(generation % 2), 100000);
        gc.apply(bench::make_snapshot(one_hour_everywhere(), next, generation++));
        while (gc.stats().scans_completed == before) {
            std::this_thread::sleep_for(100us);
        }
    }
    gc.stop();
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
}
BENCHMARK(BM_GcReload)
    ->Apply([](benchmark::internal::Benchmark* b) { sizes(b, false); })
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

}  // namespace
}  // namespace tsfs
