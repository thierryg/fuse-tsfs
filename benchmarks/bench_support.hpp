// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : benchmarks/bench_support.hpp
// Purpose : Shared helpers of the benchmarks: datasets, snapshots, process metrics
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "gc/garbage_collector.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs::bench {

/// @return true when TSFS_BENCH_LARGE=1: adds the 1,000,000-file datasets (1k and 100k always run).
inline bool large_enabled() {
    const char* v = std::getenv("TSFS_BENCH_LARGE");
    return v != nullptr && std::string_view(v) == "1";
}

/// Build a GC snapshot (throws on an invalid policy: benchmarks use fixed, valid rules).
inline std::shared_ptr<const GcSnapshot> make_snapshot(const RetentionConfig& retention, const GcConfig& gc,
                                                       std::uint64_t generation = 1) {
    auto policy = PolicyEngine::compile(retention);
    if (!policy) {
        throw std::runtime_error(policy.error().message);
    }
    auto snapshot = std::make_shared<GcSnapshot>();
    snapshot->config = gc;
    snapshot->policy = *policy;
    snapshot->generation = generation;
    return snapshot;
}

/// A rule with an id and a retention, the other fields left to the caller.
inline RuleConfig rule(std::string id, std::optional<Duration> retention) {
    RuleConfig r;
    r.id = std::move(id);
    r.retention = retention;
    return r;
}

/// Peak resident set size of the process in KiB (VmHWM), 0 if unavailable.
inline std::uint64_t peak_rss_kib() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmHWM:")) {
            return std::strtoull(line.c_str() + 6, nullptr, 10);
        }
    }
    return 0;
}

/// Current resident set size of the process in KiB (VmRSS), 0 if unavailable.
inline std::uint64_t rss_kib() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmRSS:")) {
            return std::strtoull(line.c_str() + 6, nullptr, 10);
        }
    }
    return 0;
}

/**
 * @brief Private dataset directory, removed at destruction.
 *
 * Created under $TSFS_BENCH_DIR (to benchmark a specific device or filesystem),
 * else $TMPDIR or /tmp. Never touches anything outside that directory.
 */
class ScratchDir {
public:
    explicit ScratchDir(std::string_view prefix) {
        const char* base = std::getenv("TSFS_BENCH_DIR");
        if (base == nullptr) {
            base = std::getenv("TMPDIR");
        }
        std::string templ = std::string(base != nullptr ? base : "/tmp") + "/" + std::string(prefix) + "-XXXXXX";
        if (::mkdtemp(templ.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed for " + templ);
        }
        path_ = templ;
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete("owns a directory");
    ScratchDir& operator=(const ScratchDir&) = delete("owns a directory");

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace tsfs::bench
