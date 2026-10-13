// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/bench_scheduler.cpp
// Purpose : Expiration scheduler benchmarks: heap vs ordered strategy
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// Synthetic, in memory. Arg 0 selects the strategy (0 = heap, 1 = ordered), arg 1
// the number of live entries. Due times are uniformly distributed over 1000 s.
#include <format>
#include <random>
#include <string>
#include <vector>

#include "bench_support.hpp"
#include "gc/scheduler.hpp"

namespace tsfs {
namespace {

const TimePoint kBase{std::chrono::milliseconds(1767225600000LL)};

SchedulerKind kind_of(const benchmark::State& state) {
    return state.range(0) == 0 ? SchedulerKind::heap : SchedulerKind::ordered;
}

std::vector<std::string> make_paths(std::size_t n) {
    std::vector<std::string> paths;
    paths.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        paths.push_back(std::format("/data/d{}/file-{}.bin", i % 1000, i));
    }
    return paths;
}

GcCandidate candidate(const std::string& path, std::mt19937& rng) {
    return {path, {1, 1}, kBase + std::chrono::milliseconds(rng() % 1000000), 0, 0};
}

void args(benchmark::internal::Benchmark* b) {
    for (int kind : {0, 1}) {
        b->Args({kind, 1000});
        b->Args({kind, 100000});
        if (bench::large_enabled()) {
            b->Args({kind, 1000000});
        }
    }
}

/// Fill an empty scheduler with N entries (time to schedule expiration candidates).
void BM_SchedulerFill(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(1));
    const auto paths = make_paths(n);
    for (auto _ : state) {
        state.PauseTiming();
        auto s = make_scheduler(kind_of(state), n);
        std::mt19937 rng(1);
        state.ResumeTiming();
        for (const auto& p : paths) {
            benchmark::DoNotOptimize(s->schedule(candidate(p, rng)));
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.SetLabel(kind_of(state) == SchedulerKind::heap ? "heap" : "ordered");
}
BENCHMARK(BM_SchedulerFill)->Apply(args)->Unit(benchmark::kMillisecond);

/// Steady state: reschedule existing paths (files rewritten through the mount).
void BM_SchedulerReschedule(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(1));
    const auto paths = make_paths(n);
    auto s = make_scheduler(kind_of(state), n);
    std::mt19937 rng(1);
    for (const auto& p : paths) {
        s->schedule(candidate(p, rng));
    }
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(s->schedule(candidate(paths[i++ % n], rng)));
    }
    state.SetItemsProcessed(state.iterations());
    state.SetLabel(kind_of(state) == SchedulerKind::heap ? "heap" : "ordered");
}
BENCHMARK(BM_SchedulerReschedule)->Apply(args);

/// Drain every entry in due order, in batches of 64 (the default GC batch size).
void BM_SchedulerDrain(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(1));
    const auto paths = make_paths(n);
    std::vector<GcCandidate> out;
    for (auto _ : state) {
        state.PauseTiming();
        auto s = make_scheduler(kind_of(state), n);
        std::mt19937 rng(1);
        for (const auto& p : paths) {
            s->schedule(candidate(p, rng));
        }
        state.ResumeTiming();
        std::size_t drained = 0;
        while (drained < n) {
            out.clear();
            drained += s->pop_due(kBase + std::chrono::hours(1), 64, out);
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.SetLabel(kind_of(state) == SchedulerKind::heap ? "heap" : "ordered");
}
BENCHMARK(BM_SchedulerDrain)->Apply(args)->Unit(benchmark::kMillisecond);

/// Cancellation (files deleted or excluded through the mount).
void BM_SchedulerCancel(benchmark::State& state) {
    const auto n = static_cast<std::size_t>(state.range(1));
    const auto paths = make_paths(n);
    for (auto _ : state) {
        state.PauseTiming();
        auto s = make_scheduler(kind_of(state), n);
        std::mt19937 rng(1);
        for (const auto& p : paths) {
            s->schedule(candidate(p, rng));
        }
        state.ResumeTiming();
        for (const auto& p : paths) {
            benchmark::DoNotOptimize(s->cancel(p));
        }
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(n));
    state.SetLabel(kind_of(state) == SchedulerKind::heap ? "heap" : "ordered");
}
BENCHMARK(BM_SchedulerCancel)->Apply(args)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace tsfs
