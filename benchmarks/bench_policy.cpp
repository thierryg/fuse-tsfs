// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/bench_policy.cpp
// Purpose : Policy engine micro-benchmarks: glob matching, rule evaluation, compilation
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// Synthetic, CPU-only: no filesystem access. The path set mixes matching and
// non-matching names so that the early exit of the precedence-ordered scan and the
// full scan (no rule matches, default applies) are both exercised.
#include <format>
#include <string>
#include <vector>

#include "bench_support.hpp"
#include "policy/glob.hpp"

namespace tsfs {
namespace {

using namespace std::chrono_literals;

std::vector<std::string> sample_paths() {
    std::vector<std::string> paths;
    for (int i = 0; i < 256; ++i) {
        switch (i % 8) {
            case 0: paths.push_back(std::format("/tmp/record/cam{}/clip-{}.mp4", i % 4, i)); break;
            case 1: paths.push_back(std::format("/var/cache/tsfs/obj-{}.bin", i)); break;
            case 2: paths.push_back(std::format("/var/log/application/app-{}.log", i)); break;
            case 3: paths.push_back(std::format("/crash/core.{}.core", i)); break;
            case 4: paths.push_back(std::format("/session-{}/data/part-{}.dat", i % 16, i)); break;
            case 5: paths.push_back(std::format("/home/user/documents/report-{}.odt", i)); break;
            case 6: paths.push_back(std::format("/tmp/record/.hidden-{}.mp4", i)); break;
            default: paths.push_back(std::format("/a/b/c/d/e/f/g/deep-{}.txt", i)); break;
        }
    }
    return paths;
}

/// The example rules of the specification, plus @p extra filler rules that never match.
RetentionConfig example_rules(int extra) {
    RetentionConfig c;
    c.default_retention = std::nullopt;
    auto r = bench::rule("mp4", 24h);
    r.extensions = {"mp4"};
    c.rules.push_back(r);
    r = bench::rule("record", 24h);
    r.path = "/tmp/record";
    r.recursive = true;
    c.rules.push_back(r);
    r = bench::rule("record-mp4", 24h);
    r.path_glob = "/tmp/record/**/*.mp4";
    c.rules.push_back(r);
    r = bench::rule("cache", 30min);
    r.path = "/var/cache/tsfs";
    r.recursive = true;
    c.rules.push_back(r);
    r = bench::rule("applog", 7 * 24h);
    r.path = "/var/log/application";
    c.rules.push_back(r);
    r = bench::rule("core", 48h);
    r.glob = "*.core";
    c.rules.push_back(r);
    r = bench::rule("session", 12h);
    r.path_glob = "/session-*/**";
    c.rules.push_back(r);
    r = bench::rule("keep", std::nullopt);
    r.action = RuleAction::exclude;
    r.glob = "*.keep";
    c.rules.push_back(r);
    for (int i = 0; i < extra; ++i) {
        r = bench::rule(std::format("filler-{}", i), 1h);
        r.path_glob = std::format("/nomatch-{}/**/*.x{}", i, i);
        c.rules.push_back(r);
    }
    return c;
}

void BM_GlobBasename(benchmark::State& state) {
    const auto glob = *Glob::compile("clip-*[0-9].mp4", Glob::Mode::basename);
    const std::vector<std::string> names{"clip-42.mp4", "clip-x.mp4", "other.txt", "clip-123456789.mp4"};
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(glob.matches(names[i++ & 3U]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GlobBasename);

void BM_GlobPathDoubleStar(benchmark::State& state) {
    const auto glob = *Glob::compile("/tmp/record/**/*.mp4", Glob::Mode::path);
    const auto paths = sample_paths();
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(glob.matches(paths[i++ & 255U]));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_GlobPathDoubleStar);

/// Arg: number of filler rules added to the 8 example rules.
void BM_PolicyEvaluate(benchmark::State& state) {
    const auto engine = *PolicyEngine::compile(example_rules(static_cast<int>(state.range(0))));
    const auto paths = sample_paths();
    const FileTimes times{TimePoint{std::chrono::milliseconds(1767225600000LL)}, std::nullopt};
    std::size_t i = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(engine->evaluate(paths[i++ & 255U], times));
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["rules"] = static_cast<double>(engine->active_rule_count());
}
BENCHMARK(BM_PolicyEvaluate)->Arg(0)->Arg(10)->Arg(100)->Arg(1000);

/// Configuration reload cost on the policy side: compiling a rule set. Arg: filler rules.
void BM_PolicyCompile(benchmark::State& state) {
    const auto config = example_rules(static_cast<int>(state.range(0)));
    for (auto _ : state) {
        auto engine = PolicyEngine::compile(config);
        benchmark::DoNotOptimize(engine);
    }
    state.counters["rules"] = static_cast<double>(config.rules.size());
}
BENCHMARK(BM_PolicyCompile)->Arg(0)->Arg(100)->Arg(1000)->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace tsfs
