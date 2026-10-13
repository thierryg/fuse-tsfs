// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/garbage_collector.hpp
// Purpose : Garbage collector: discovery (scan + events), scheduling, validated deletion, retries
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "common/clock.hpp"
#include "config/config.hpp"
#include "fsutil/backing_store.hpp"
#include "fsutil/open_file_registry.hpp"
#include "fsutil/path_lock_table.hpp"
#include "gc/scheduler.hpp"
#include "gc/token_bucket.hpp"
#include "gc/tree_scanner.hpp"
#include "logging/logger.hpp"
#include "logging/rate_limiter.hpp"
#include "policy/policy_engine.hpp"

namespace tsfs {

/// Immutable inputs of the GC for one configuration generation.
struct GcSnapshot {
    GcConfig config;
    std::shared_ptr<const PolicyEngine> policy;
    std::uint64_t generation{0};
};

/// What happened to one candidate (state machine end states, see garbage-collector.md).
enum class GcOutcome {
    deleted,       ///< Unlinked.
    would_delete,  ///< Dry run: every check passed, nothing removed.
    vanished,      ///< Already gone (ENOENT): success, deletion is idempotent.
    not_expired,   ///< Modified or policy changed: rescheduled at the new expiry.
    kept,          ///< Now excluded or "never": dropped from the queue.
    replaced,      ///< Different inode under the same name: re-evaluated, not deleted.
    busy_open,     ///< Open through the mount: retried later (no attempt counted).
    unsafe,        ///< Not a regular file any more, other device, path escape: never deleted.
    retry,         ///< Transient failure: rescheduled with backoff.
    failed,        ///< Permanent failure or retries exhausted: dropped until the next scan.
};

/// Name of an outcome ("deleted", "busy_open"...).
const char* to_string(GcOutcome outcome) noexcept;

/// Cumulative GC counters (each one is a relaxed atomic; read with stats()).
struct GcStats {
    std::uint64_t candidates{0};  ///< Entries scheduled because they expire soon.
    std::uint64_t deleted{0};
    std::uint64_t bytes_deleted{0};
    std::uint64_t would_delete{0};
    std::uint64_t vanished{0};
    std::uint64_t skipped_modified{0};  ///< not_expired + kept.
    std::uint64_t skipped_replaced{0};
    std::uint64_t skipped_open{0};
    std::uint64_t skipped_unsafe{0};
    std::uint64_t failed{0};
    std::uint64_t retries{0};
    std::uint64_t queue_saturations{0};  ///< Candidates rejected by a full scheduler.
    std::uint64_t events_dropped{0};     ///< FUSE notifications dropped (event queue full).
    std::uint64_t scans_completed{0};
    std::uint64_t last_scan_files{0};  ///< Regular files seen by the last complete pass.
    std::uint64_t last_scan_duration_ms{0};
    std::int64_t last_scan_completed_unix_ms{0};
    std::uint64_t scan_time_ms{0};    ///< Cumulative time spent scanning.
    std::uint64_t delete_time_us{0};  ///< Cumulative time spent validating + deleting.
    std::uint64_t worker_busy_us{0};
    std::uint64_t scheduled{0};          ///< Gauge: entries in the scheduler.
    std::uint64_t pending_work{0};       ///< Gauge: entries handed to workers, not done.
    std::uint64_t workers{0};            ///< Gauge.
    std::uint64_t config_generation{0};  ///< Gauge.
};

/// One line of a dry-run report.
struct DryRunEntry {
    std::string path;
    std::string rule;
    TimePoint expiry{};
    std::uint64_t size{0};
};

/**
 * @brief The garbage collector service.
 *
 * Architecture (docs/architecture/garbage-collector.md):
 * - Discovery is separated from deletion. A coordinator thread runs time-sliced
 *   reconciliation passes over the backing tree (TreeScanner) and drains a bounded
 *   queue of change notifications from the FUSE layer. Files whose expiry falls
 *   within the horizon (2 x scan_interval) enter the bounded ExpirationScheduler;
 *   the rest is found again by later passes, so memory does not grow with the
 *   number of files.
 * - The coordinator sleeps on a condition variable until the earliest due time, the
 *   next pass, a notification or a configuration change (no polling), and moves
 *   due candidates into a bounded work queue (backpressure: what does not fit
 *   stays in the scheduler).
 * - max_workers worker threads validate and delete candidates, throttled by a
 *   token bucket. Validation re-resolves the path under the path locks, re-stats
 *   it, checks identity, type, device, open state and re-evaluates the *current*
 *   policy against fresh timestamps; the scheduler record alone never justifies a
 *   deletion. Dry run executes exactly the same validation.
 * - Failures are classified; transient ones are retried with exponential backoff
 *   (retry_initial_delay * 2^(attempt - 1): the first retry waits retry_initial_delay, capped at retry_max_delay, at
 * most retry_limit times); repeated messages are rate-limited.
 *
 * Thread-safety: notify_*(), apply(), stats() may be called from any thread.
 * Ownership: references in Dependencies must outlive the collector; stop() (or the
 * destructor) joins every thread before returning.
 */
class GarbageCollector {
public:
    /// External collaborators (dependency injection).
    struct Dependencies {
        const BackingStore& store;
        Logger& log;
        const Clock& clock;
        PathLockTable& locks;
        const OpenFileRegistry* open_files{nullptr};
        /// Called after each real deletion, outside every lock (FUSE cache invalidation).
        std::function<void(std::string_view path)> on_deleted;
    };

    GarbageCollector(Dependencies deps, std::shared_ptr<const GcSnapshot> snapshot);
    ~GarbageCollector();
    GarbageCollector(const GarbageCollector&) = delete("owns threads");
    GarbageCollector& operator=(const GarbageCollector&) = delete("owns threads");

    /// Start the coordinator and the workers. Idempotent.
    void start();
    /// Stop accepting work, let workers finish their current file, join every thread.
    void stop();

    /// Publish a new configuration generation: limits, policy, worker count.
    /// Triggers a new reconciliation pass so every file is re-evaluated.
    void apply(std::shared_ptr<const GcSnapshot> snapshot);

    /// A file was created/written/renamed-to/truncated through the mount (non-blocking).
    void notify_changed(std::string_view path);

    [[nodiscard]] GcStats stats() const;
    [[nodiscard]] std::shared_ptr<const GcSnapshot> snapshot() const { return snapshot_.load(); }

    // --- synchronous building blocks (one-shot dry run, tests) -----------------
    /// Run complete reconciliation passes until one finishes; schedules candidates.
    void run_full_scan();
    /// Validate and process every candidate due now, in the calling thread.
    std::size_t process_due_now();
    /// Validate and process one candidate (worker step), returning its outcome.
    GcOutcome process_candidate(GcCandidate& candidate, bool force_dry_run = false);
    /// One-shot dry run: full pass + validation of every expired file, nothing deleted.
    std::vector<DryRunEntry> dry_run_report();

private:
    void coordinator(std::stop_token stop);
    void worker(std::stop_token stop, unsigned index);
    void start_workers(unsigned count);
    void stop_workers();
    void evaluate_and_schedule(std::string_view path, const EntryStat& stat, const GcSnapshot& snap, TimePoint now);
    void drain_events(const GcSnapshot& snap);
    std::size_t dispatch_due(const GcSnapshot& snap);
    void handle_outcome(GcCandidate& candidate, GcOutcome outcome, const GcSnapshot& snap, int error);
    void schedule(GcCandidate candidate);
    void report_failure(std::string_view event, std::string_view path, int error, std::string_view message);
    void scan_slice(const GcSnapshot& snap, std::chrono::steady_clock::time_point deadline);

    Dependencies deps_;
    std::atomic<std::shared_ptr<const GcSnapshot>> snapshot_;

    // Scheduler and work queue: guarded by mutex_. Lock order: mutex_ -> (nothing):
    // path locks are only taken by workers while mutex_ is NOT held.
    mutable std::mutex mutex_;
    std::condition_variable_any coordinator_cv_;
    std::condition_variable_any work_cv_;
    std::unique_ptr<ExpirationScheduler> scheduler_;
    std::deque<GcCandidate> work_;
    std::size_t work_capacity_{0};
    std::size_t in_flight_{0};
    std::deque<std::string> events_;
    bool rescan_requested_{true};
    bool snapshot_changed_{false};
    std::uint64_t dispatched_in_cycle_{0};
    std::unordered_set<std::string> dry_run_reported_;  ///< Avoids repeating dry-run lines every pass.
    std::vector<DryRunEntry>* dry_run_sink_{nullptr};   ///< Set by dry_run_report().

    TreeScanner scanner_;  ///< Driven by the coordinator only.
    std::chrono::steady_clock::time_point next_scan_{};
    std::chrono::steady_clock::time_point scan_started_{};
    TokenBucket bucket_;
    LogRateLimiter limiter_;

    std::jthread coordinator_thread_;
    mutable std::mutex workers_mutex_;  ///< Guards workers_ (start/stop/apply may race).
    std::vector<std::jthread> workers_;
    bool wake_{false};  ///< Coordinator wake-up flag (guarded by mutex_).
    std::atomic<bool> running_{false};

    // Counters (relaxed atomics: written by a handful of threads at deletion rate).
    struct Counters {
        std::atomic<std::uint64_t> candidates{0}, deleted{0}, bytes_deleted{0}, would_delete{0}, vanished{0},
            skipped_modified{0}, skipped_replaced{0}, skipped_open{0}, skipped_unsafe{0}, failed{0}, retries{0},
            queue_saturations{0}, events_dropped{0}, scans_completed{0}, last_scan_files{0}, last_scan_duration_ms{0},
            scan_time_ms{0}, delete_time_us{0}, worker_busy_us{0};
        std::atomic<std::int64_t> last_scan_completed_unix_ms{0};
    };
    mutable Counters counters_;
};

}  // namespace tsfs
