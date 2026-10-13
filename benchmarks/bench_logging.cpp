// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/bench_logging.cpp
// Purpose : Logging pipeline benchmarks: filtered calls, sync vs async file output
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
// The log file lives in a scratch directory (TSFS_BENCH_DIR, else $TMPDIR or /tmp).
// Rotation is enabled with a 16 MiB limit so that long runs also pay rotation costs.
#include "bench_support.hpp"
#include "logging/logger.hpp"

namespace tsfs {
namespace {

LoggingConfig file_config(const std::filesystem::path& dir, bool async) {
    LoggingConfig c;
    c.directory = dir.string();
    c.level = LogLevel::info;
    c.async.enabled = async;
    c.async.overflow_policy = LogOverflowPolicy::block;  // Every message is written.
    c.rotation.max_file_size = 16ULL << 20;
    c.rotation.max_files = 2;
    return c;
}

/// A call below the active level: the hot-path cost of disabled debug logging.
void BM_LogFilteredOut(benchmark::State& state) {
    Logger log;
    log.set_level(LogLevel::warning);
    for (auto _ : state) {
        log.log(LogLevel::debug, "bench", "bench_event", "filtered", {{"path", "/a/b"}, {"n", 42}});
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_LogFilteredOut);

/// Messages written to the rotating file. Arg 0: 0 = synchronous, 1 = asynchronous.
/// Threads: concurrent producers (FUSE and GC threads).
void BM_LogToFile(benchmark::State& state) {
    static std::unique_ptr<bench::ScratchDir> dir;
    static std::unique_ptr<Logger> log;
    if (state.thread_index() == 0) {
        dir = std::make_unique<bench::ScratchDir>("tsfs-bench-log");
        log = std::make_unique<Logger>();
        if (!log->configure(file_config(dir->path(), state.range(0) == 1))) {
            state.SkipWithError("cannot configure the logger");
        }
    }
    for (auto _ : state) {
        log->log(LogLevel::info, "gc", "gc_file_deleted", "expired file deleted",
                 {{"path", "/tmp/record/cam1/clip-000042.mp4"}, {"rule", "record"}, {"size", 1048576}});
    }
    state.SetItemsProcessed(state.iterations());
    if (state.thread_index() == 0) {
        log->shutdown();  // Includes draining the async queue: part of the real cost.
        log.reset();
        dir.reset();
        state.SetLabel(state.range(0) == 1 ? "async" : "sync");
    }
}
BENCHMARK(BM_LogToFile)->Arg(0)->Arg(1)->Threads(1)->Threads(4)->UseRealTime();

}  // namespace
}  // namespace tsfs
