// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/garbage_collector.cpp
// Purpose : Garbage collector: discovery (scan + events), scheduling, validated deletion, retries
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "gc/garbage_collector.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>

#include "common/managed_path.hpp"
#include "common/threads.hpp"
#include "common/units.hpp"

namespace tsfs {
namespace {

using SteadyClock = std::chrono::steady_clock;
constexpr std::size_t kMaxEventsPerIteration = 4096;
constexpr std::size_t kMaxDryRunReported = 100000;
constexpr auto kMaxSleep = std::chrono::seconds(60);  // Bounds the effect of a wall-clock jump.

/// Transient errors are retried with backoff; the others are final.
bool is_transient(int error) noexcept {
    switch (error) {
        case EROFS:
        case ENAMETOOLONG:
        case EINVAL:
        case EISDIR:
        case ENOTDIR:
        case ELOOP:
        case EXDEV:
        case EFAULT: return false;
        default:
            // EBUSY, EIO, EAGAIN, EINTR, ENOMEM, ETXTBSY, EACCES, EPERM (permissions can be
            // fixed by the administrator), ENOSPC...: retried a bounded number of times.
            return true;
    }
}

std::int64_t unix_ms(TimePoint t) { return t.time_since_epoch().count(); }

Duration horizon(const GcConfig& config) { return config.scan_interval * 2; }

Duration backoff(const GcConfig& config, std::uint32_t attempt) {
    Duration delay = config.retry_initial_delay;
    for (std::uint32_t i = 1; i < attempt && delay < config.retry_max_delay; ++i) {
        delay *= 2;
    }
    return std::min(delay, config.retry_max_delay);
}

std::size_t work_capacity(const GcConfig& config) {
    const std::size_t wanted = std::max<std::size_t>(64, std::size_t{config.batch_size} * config.max_workers * 2);
    return std::min<std::size_t>(wanted, config.max_pending_tasks);
}

}  // namespace

const char* to_string(GcOutcome outcome) noexcept {
    switch (outcome) {
        case GcOutcome::deleted: return "deleted";
        case GcOutcome::would_delete: return "would_delete";
        case GcOutcome::vanished: return "vanished";
        case GcOutcome::not_expired: return "not_expired";
        case GcOutcome::kept: return "kept";
        case GcOutcome::replaced: return "replaced";
        case GcOutcome::busy_open: return "busy_open";
        case GcOutcome::unsafe: return "unsafe";
        case GcOutcome::retry: return "retry";
        case GcOutcome::failed: return "failed";
    }
    return "unknown";
}

GarbageCollector::GarbageCollector(Dependencies deps, std::shared_ptr<const GcSnapshot> snapshot)
    : deps_(std::move(deps)),
      scheduler_(make_scheduler(snapshot->config.scheduler, snapshot->config.max_pending_tasks)),
      work_capacity_(work_capacity(snapshot->config)),
      scanner_(deps_.store, snapshot->policy->needs_birth_time()),
      bucket_(snapshot->config.max_deletions_per_second) {
    snapshot_.store(std::move(snapshot));
}

GarbageCollector::~GarbageCollector() { stop(); }

void GarbageCollector::start() {
    if (running_.exchange(true)) {
        return;
    }
    const auto snap = snapshot_.load();
    deps_.log.log(LogLevel::info, "gc", "gc_started", "garbage collector started",
                  {{"enabled", snap->config.enabled},
                   {"dry_run", snap->config.dry_run},
                   {"workers", snap->config.max_workers},
                   {"scan_interval", format_duration(snap->config.scan_interval)},
                   {"scheduler", to_string(snap->config.scheduler)}});
    start_workers(snap->config.max_workers);
    coordinator_thread_ = std::jthread([this](std::stop_token st) { coordinator(st); });
}

void GarbageCollector::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    coordinator_thread_.request_stop();
    coordinator_cv_.notify_all();
    if (coordinator_thread_.joinable()) {
        coordinator_thread_.join();
    }
    stop_workers();
    std::size_t abandoned = 0;
    {
        std::scoped_lock lock(mutex_);
        abandoned = work_.size() + scheduler_->size();
        work_.clear();
    }
    const GcStats s = stats();
    deps_.log.log(LogLevel::info, "gc", "gc_completed", "garbage collector stopped",
                  {{"deleted", s.deleted},
                   {"bytes_deleted", s.bytes_deleted},
                   {"failed", s.failed},
                   {"pending_abandoned", abandoned}});
}

void GarbageCollector::start_workers(unsigned count) {
    std::scoped_lock lock(workers_mutex_);
    for (unsigned i = 0; i < count; ++i) {
        workers_.emplace_back([this, i](std::stop_token st) { worker(st, i); });
    }
}

void GarbageCollector::stop_workers() {
    std::vector<std::jthread> old;
    {
        std::scoped_lock lock(workers_mutex_);
        old.swap(workers_);
    }
    for (auto& w : old) {
        w.request_stop();
    }
    work_cv_.notify_all();
    old.clear();  // Joins: each worker finishes the file it is processing.
}

void GarbageCollector::apply(std::shared_ptr<const GcSnapshot> snapshot) {
    const GcConfig next = snapshot->config;
    const auto previous = snapshot_.exchange(std::move(snapshot));
    bucket_.set_rate(next.max_deletions_per_second);
    {
        std::scoped_lock lock(mutex_);
        if (!previous || previous->config.scheduler != next.scheduler ||
            previous->config.max_pending_tasks != next.max_pending_tasks) {
            // Entries are not migrated: the forced pass below rebuilds the queue.
            scheduler_ = make_scheduler(next.scheduler, next.max_pending_tasks);
        }
        work_capacity_ = work_capacity(next);
        snapshot_changed_ = true;
        rescan_requested_ = true;
        dry_run_reported_.clear();
        wake_ = true;
    }
    coordinator_cv_.notify_all();
    work_cv_.notify_all();
    if (running_.load() && previous && previous->config.max_workers != next.max_workers) {
        stop_workers();
        start_workers(next.max_workers);
    }
}

void GarbageCollector::request_purge() {
    {
        std::scoped_lock lock(mutex_);
        purge_requested_ = true;
        wake_ = true;
    }
    coordinator_cv_.notify_one();
}

void GarbageCollector::run_purge(const GcSnapshot& snap, std::stop_token stop) {
    purging_.store(true);
    const GcStats before = stats();
    deps_.log.log(LogLevel::info, "gc", "gc_purge_started", "purging every expired file now",
                  {{"dry_run", snap.config.dry_run}});
    constexpr int kMaxPasses = 1000;  // Bound: each pass removes up to max_pending_tasks files.
    int passes = 0;
    for (; passes < kMaxPasses && !stop.stop_requested(); ++passes) {
        const std::uint64_t saturations = counters_.queue_saturations.load(std::memory_order_relaxed);
        scanner_.reset();
        scanner_.set_want_birth(snap.policy->needs_birth_time());
        scan_started_ = SteadyClock::now();
        do {
            drain_events(snap);
            scan_slice(snap, SteadyClock::now() + snap.config.max_scan_duration);
            dispatch_due(snap, /*unlimited=*/true);
        } while (scanner_.in_progress() && !stop.stop_requested());
        // Drain: everything due now is dispatched and processed.
        for (;;) {
            dispatch_due(snap, /*unlimited=*/true);
            std::unique_lock lock(mutex_);
            const auto due = scheduler_->next_due();
            const bool due_now = due && *due <= deps_.clock.now();
            if ((!due_now && work_.empty() && in_flight_ == 0) || stop.stop_requested()) {
                break;
            }
            coordinator_cv_.wait_for(lock, stop, std::chrono::milliseconds(10), [this] { return wake_; });
            wake_ = false;
        }
        if (counters_.queue_saturations.load(std::memory_order_relaxed) == saturations) {
            ++passes;
            break;  // Nothing was left out of the scheduler: every expired file was seen.
        }
    }
    next_scan_ = SteadyClock::now() + snap.config.scan_interval;
    const GcStats after = stats();
    // Published before the completion counter: a reader that sees the new
    // purges_completed also sees these values (both in the same status document).
    counters_.last_purge_deleted.store(after.deleted - before.deleted, std::memory_order_relaxed);
    counters_.last_purge_bytes.store(after.bytes_deleted - before.bytes_deleted, std::memory_order_relaxed);
    counters_.last_purge_failed.store(after.failed - before.failed, std::memory_order_relaxed);
    counters_.last_purge_skipped_open.store(after.skipped_open - before.skipped_open, std::memory_order_relaxed);
    counters_.purges_completed.fetch_add(1, std::memory_order_release);
    purging_.store(false);
    deps_.log.log(LogLevel::info, "gc", "gc_purge_completed", "purge completed",
                  {{"deleted", after.deleted - before.deleted},
                   {"would_delete", after.would_delete - before.would_delete},
                   {"failed", after.failed - before.failed},
                   {"skipped_open", after.skipped_open - before.skipped_open},
                   {"passes", passes}});
}

void GarbageCollector::notify_changed(std::string_view path) {
    {
        std::scoped_lock lock(mutex_);
        const auto snap = snapshot_.load();
        const std::size_t cap = std::min<std::size_t>(snap->config.max_pending_tasks, 65536);
        if (events_.size() >= cap) {
            counters_.events_dropped.fetch_add(1, std::memory_order_relaxed);
            return;  // The next reconciliation pass will see the file anyway.
        }
        events_.emplace_back(path);
        wake_ = true;
    }
    coordinator_cv_.notify_one();
}

void GarbageCollector::schedule(GcCandidate candidate) {
    ScheduleResult result{};
    {
        std::scoped_lock lock(mutex_);
        if (const GcCandidate* existing = scheduler_->find(candidate.path);
            existing != nullptr && existing->identity == candidate.identity &&
            (existing->attempt > 0 || existing->due == candidate.due)) {
            return;  // Same file already queued (keeps its retry backoff, avoids heap churn).
        }
        result = scheduler_->schedule(std::move(candidate));
        wake_ = true;
    }
    coordinator_cv_.notify_one();
    if (result == ScheduleResult::inserted) {
        counters_.candidates.fetch_add(1, std::memory_order_relaxed);
    } else if (result == ScheduleResult::rejected_full) {
        counters_.queue_saturations.fetch_add(1, std::memory_order_relaxed);
        if (const auto v = limiter_.check("gc_queue_saturated"); v.allowed) {
            deps_.log.log(
                LogLevel::warning, "gc", "gc_queue_saturated",
                "scheduler full (max_pending_tasks); candidates deferred to the next pass",
                {{"capacity", snapshot_.load()->config.max_pending_tasks}, {"suppressed", v.suppressed_before}});
        }
    }
}

void GarbageCollector::evaluate_and_schedule(std::string_view path, const EntryStat& stat, const GcSnapshot& snap,
                                             TimePoint now) {
    const Decision decision = snap.policy->evaluate(path, stat.times);
    if (decision.kind != Decision::Kind::expires || decision.expiry > saturating_add(now, horizon(snap.config))) {
        return;
    }
    if (decision.expired_at(now) && deps_.log.enabled(LogLevel::debug)) {
        deps_.log.log(LogLevel::debug, "gc", "gc_candidate_found", "expired file found",
                      {{"path", path},
                       {"rule", snap.policy->rule_id(decision.rule_index)},
                       {"expired_ms_ago", unix_ms(now) - unix_ms(decision.expiry)}});
    }
    schedule(GcCandidate{std::string(path), stat.identity, decision.expiry, 0, 0});
}

void GarbageCollector::drain_events(const GcSnapshot& snap) {
    std::deque<std::string> batch;
    {
        std::scoped_lock lock(mutex_);
        const std::size_t n = std::min(events_.size(), kMaxEventsPerIteration);
        batch.insert(batch.end(), std::make_move_iterator(events_.begin()),
                     std::make_move_iterator(events_.begin() + static_cast<std::ptrdiff_t>(n)));
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    const TimePoint now = deps_.clock.now();
    for (const std::string& path : batch) {
        auto parent = deps_.store.open_parent(path);
        if (!parent) {
            continue;
        }
        auto st = stat_entry(parent->directory.get(), parent->name.c_str(), snap.policy->needs_birth_time());
        if (!st) {
            if (st.error().code == ENOENT) {
                std::scoped_lock lock(mutex_);
                scheduler_->cancel(path);
            }
            continue;
        }
        if (st->is_regular()) {
            evaluate_and_schedule(path, *st, snap, now);
        }
    }
}

std::size_t GarbageCollector::dispatch_due(const GcSnapshot& snap, bool unlimited) {
    std::vector<GcCandidate> due;
    {
        std::scoped_lock lock(mutex_);
        std::size_t room = work_capacity_ > work_.size() ? work_capacity_ - work_.size() : 0;
        if (!unlimited && snap.config.max_deletions_per_cycle != 0) {
            const std::uint64_t left = snap.config.max_deletions_per_cycle > dispatched_in_cycle_
                                           ? snap.config.max_deletions_per_cycle - dispatched_in_cycle_
                                           : 0;
            room = std::min<std::size_t>(room, left);
        }
        if (room == 0) {
            return 0;
        }
        scheduler_->pop_due(deps_.clock.now(), room, due);
        dispatched_in_cycle_ += due.size();
        for (auto& c : due) {
            work_.push_back(std::move(c));
        }
    }
    if (!due.empty()) {
        work_cv_.notify_all();
    }
    return due.size();
}

void GarbageCollector::scan_slice(const GcSnapshot& snap, SteadyClock::time_point deadline) {
    const auto t0 = SteadyClock::now();
    const TimePoint now = deps_.clock.now();
    const bool finished = scanner_.run(
        deadline, [&](std::string_view path, const EntryStat& st) { evaluate_and_schedule(path, st, snap, now); },
        [&](std::string_view path, int error) {
            if (const auto v = limiter_.check(std::format("scan:{}", error)); v.allowed) {
                deps_.log.log(LogLevel::warning, "gc", "filesystem_operation_failed",
                              "cannot read an entry during a scan",
                              {{"path", path},
                               {"errno", error},
                               {"error", std::strerror(error)},
                               {"suppressed", v.suppressed_before}});
            }
        });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - t0);
    counters_.scan_time_ms.fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
    if (!finished) {
        return;
    }
    const auto& totals = scanner_.totals();
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - scan_started_);
    counters_.scans_completed.fetch_add(1, std::memory_order_relaxed);
    counters_.last_scan_files.store(totals.files, std::memory_order_relaxed);
    counters_.last_scan_duration_ms.store(static_cast<std::uint64_t>(duration.count()), std::memory_order_relaxed);
    counters_.last_scan_completed_unix_ms.store(unix_ms(deps_.clock.now()), std::memory_order_relaxed);
    next_scan_ = scan_started_ + snap.config.scan_interval;
    std::size_t scheduled = 0;
    {
        std::scoped_lock lock(mutex_);
        scheduled = scheduler_->size();
    }
    deps_.log.log(LogLevel::info, "gc", "gc_reconciliation_completed", "reconciliation pass completed",
                  {{"files", totals.files},
                   {"directories", totals.directories},
                   {"symlinks", totals.symlinks},
                   {"errors", totals.errors},
                   {"boundaries", totals.boundaries},
                   {"scheduled", scheduled},
                   {"duration_ms", duration.count()}});
}

void GarbageCollector::coordinator(std::stop_token stop) {
    prepare_helper_thread("tsfs-gc");
    std::shared_ptr<const GcSnapshot> snap = snapshot_.load();
    next_scan_ = SteadyClock::now();
    while (!stop.stop_requested()) {
        bool changed = false;
        {
            std::scoped_lock lock(mutex_);
            changed = std::exchange(snapshot_changed_, false);
        }
        if (changed) {
            snap = snapshot_.load();
            scanner_.reset();
            scanner_.set_want_birth(snap->policy->needs_birth_time());
        }
        bool purge = false;
        {
            std::scoped_lock lock(mutex_);
            purge = std::exchange(purge_requested_, false);
        }
        if (purge) {
            run_purge(*snap, stop);
            continue;
        }
        if (!snap->config.enabled) {
            std::unique_lock lock(mutex_);
            coordinator_cv_.wait(lock, stop, [this] { return snapshot_changed_ || purge_requested_; });
            continue;
        }
        drain_events(*snap);
        dispatch_due(*snap);

        auto now = SteadyClock::now();
        bool want_scan = false;
        {
            std::scoped_lock lock(mutex_);
            want_scan = rescan_requested_ || now >= next_scan_ || scanner_.in_progress();
            if (want_scan && !scanner_.in_progress()) {
                rescan_requested_ = false;
                dispatched_in_cycle_ = 0;  // A new cycle starts with the pass.
            }
        }
        if (want_scan) {
            if (!scanner_.in_progress()) {
                scan_started_ = now;
                deps_.log.log(LogLevel::debug, "gc", "gc_reconciliation_started", "reconciliation pass started");
            }
            scan_slice(*snap, now + snap->config.max_scan_duration);
            dispatch_due(*snap);
            if (scanner_.in_progress()) {
                continue;  // Next slice right away, after serving events and due entries.
            }
        }

        // Sleep until: next pass, earliest due entry, a notification, a worker freeing
        // room, a configuration change, or stop. Never longer than kMaxSleep so that a
        // wall-clock step is noticed within a minute.
        std::unique_lock lock(mutex_);
        now = SteadyClock::now();
        auto deadline = std::min(next_scan_, now + kMaxSleep);
        const bool queue_full = work_.size() >= work_capacity_;
        const bool budget_spent =
            snap->config.max_deletions_per_cycle != 0 && dispatched_in_cycle_ >= snap->config.max_deletions_per_cycle;
        if (!queue_full && !budget_spent) {
            if (const auto due = scheduler_->next_due()) {
                const auto wall_now = deps_.clock.now();
                const auto delta = *due > wall_now
                                       ? std::chrono::duration_cast<std::chrono::milliseconds>(*due - wall_now)
                                       : std::chrono::milliseconds(0);
                deadline = std::min(deadline, now + std::min<SteadyClock::duration>(delta, kMaxSleep));
            }
        }
        if (deadline > now) {
            coordinator_cv_.wait_until(lock, stop, deadline, [this] { return wake_; });
        }
        wake_ = false;
    }
}

void GarbageCollector::worker(std::stop_token stop, unsigned index) {
    prepare_helper_thread(std::format("tsfs-gc-w{}", index));
    std::vector<GcCandidate> batch;
    while (!stop.stop_requested()) {
        batch.clear();
        {
            std::unique_lock lock(mutex_);
            if (!work_cv_.wait(lock, stop, [this] { return !work_.empty(); })) {
                return;
            }
            const std::size_t n = std::min<std::size_t>(work_.size(), snapshot_.load()->config.batch_size);
            for (std::size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(work_.front()));
                work_.pop_front();
            }
            in_flight_ += n;
            wake_ = true;  // Room was freed: the coordinator may dispatch more.
        }
        coordinator_cv_.notify_one();
        const auto t0 = SteadyClock::now();
        for (GcCandidate& candidate : batch) {
            if (stop.stop_requested() || (!purging_.load(std::memory_order_relaxed) && !bucket_.acquire(stop))) {
                break;  // Shutdown: the rest is found again by the next start's first pass.
            }
            const auto snap = snapshot_.load();
            const GcOutcome outcome = process_candidate(candidate);
            handle_outcome(candidate, outcome, *snap, candidate.last_error);
        }
        counters_.worker_busy_us.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count()),
            std::memory_order_relaxed);
        std::scoped_lock lock(mutex_);
        in_flight_ -= batch.size();
    }
}

GcOutcome GarbageCollector::process_candidate(GcCandidate& candidate, bool force_dry_run) {
    const auto snap = snapshot_.load();
    const bool dry_run = force_dry_run || snap->config.dry_run;
    const PolicyEngine& policy = *snap->policy;
    const auto t0 = SteadyClock::now();
    GcOutcome outcome = GcOutcome::failed;
    std::uint64_t bytes = 0;
    std::uint64_t links = 0;
    int rule = -1;
    candidate.last_error = 0;
    const auto fail_with = [&](int error) {
        candidate.last_error = error;
        if (error == ENOENT) {
            return GcOutcome::vanished;
        }
        if (error == ELOOP || error == EXDEV || error == ENOTDIR) {
            return GcOutcome::unsafe;  // The path now crosses a symlink or a mount point.
        }
        return is_transient(error) ? GcOutcome::retry : GcOutcome::failed;
    };
    {
        // Ancestors are locked too: a rename of a parent directory through the mount
        // cannot move the file under another policy between validation and unlink.
        const auto guard = deps_.locks.lock_with_ancestors(candidate.path);
        auto parent = deps_.store.open_parent(candidate.path);
        if (!parent) {
            outcome = fail_with(parent.error().code);
        } else if (auto st = stat_entry(parent->directory.get(), parent->name.c_str(), policy.needs_birth_time());
                   !st) {
            outcome = fail_with(st.error().code);
        } else if (!st->is_regular() ||
                   (deps_.store.options().stay_on_filesystem && st->identity.device != deps_.store.root_device())) {
            outcome = GcOutcome::unsafe;
        } else {
            const Decision decision = policy.evaluate(candidate.path, st->times);
            rule = decision.rule_index;
            bytes = st->size;
            links = st->links;
            if (st->identity != candidate.identity) {
                // Not the file that was evaluated: never delete on a stale record.
                candidate.identity = st->identity;
                candidate.due = decision.expiry;
                outcome = decision.kind == Decision::Kind::expires ? GcOutcome::replaced : GcOutcome::kept;
            } else if (decision.kind != Decision::Kind::expires) {
                outcome = GcOutcome::kept;
            } else if (!decision.expired_at(deps_.clock.now())) {
                candidate.due = decision.expiry;
                outcome = GcOutcome::not_expired;
            } else if (snap->config.skip_open_files && deps_.open_files != nullptr &&
                       deps_.open_files->is_open(st->identity)) {
                outcome = GcOutcome::busy_open;
            } else if (dry_run) {
                outcome = GcOutcome::would_delete;
            } else if (::unlinkat(parent->directory.get(), parent->name.c_str(), 0) == 0) {
                outcome = GcOutcome::deleted;
            } else {
                outcome = fail_with(errno);
            }
        }
    }
    counters_.delete_time_us.fetch_add(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - t0).count()),
        std::memory_order_relaxed);

    const std::string_view rule_id = policy.rule_id(rule);
    switch (outcome) {
        case GcOutcome::deleted:
            counters_.deleted.fetch_add(1, std::memory_order_relaxed);
            counters_.bytes_deleted.fetch_add(links > 1 ? 0 : bytes, std::memory_order_relaxed);
            deps_.log.log(LogLevel::info, "gc", "gc_file_deleted", "",
                          {{"path", candidate.path}, {"bytes", bytes}, {"links", links}, {"rule", rule_id}});
            if (deps_.on_deleted) {
                deps_.on_deleted(candidate.path);  // Outside the path locks.
            }
            break;
        case GcOutcome::would_delete: {
            counters_.would_delete.fetch_add(1, std::memory_order_relaxed);
            bool first = true;
            {
                std::scoped_lock lock(mutex_);
                if (dry_run_sink_ != nullptr) {
                    dry_run_sink_->push_back({candidate.path, std::string(rule_id), candidate.due, bytes});
                }
                if (dry_run_reported_.size() >= kMaxDryRunReported) {
                    dry_run_reported_.clear();
                }
                first = dry_run_reported_.insert(candidate.path).second;
            }
            if (first) {
                deps_.log.log(LogLevel::info, "gc", "gc_candidate_found", "dry run: would delete",
                              {{"path", candidate.path}, {"bytes", bytes}, {"rule", rule_id}, {"dry_run", true}});
            }
            break;
        }
        case GcOutcome::vanished:
            counters_.vanished.fetch_add(1, std::memory_order_relaxed);
            deps_.log.log(LogLevel::debug, "gc", "gc_file_skipped", "",
                          {{"path", candidate.path}, {"reason", "vanished"}});
            break;
        case GcOutcome::not_expired:
        case GcOutcome::kept:
            counters_.skipped_modified.fetch_add(1, std::memory_order_relaxed);
            deps_.log.log(LogLevel::debug, "gc", "gc_file_skipped", "",
                          {{"path", candidate.path}, {"reason", to_string(outcome)}, {"rule", rule_id}});
            break;
        case GcOutcome::replaced:
            counters_.skipped_replaced.fetch_add(1, std::memory_order_relaxed);
            deps_.log.log(LogLevel::debug, "gc", "gc_file_skipped", "",
                          {{"path", candidate.path}, {"reason", "replaced"}});
            break;
        case GcOutcome::busy_open:
            counters_.skipped_open.fetch_add(1, std::memory_order_relaxed);
            deps_.log.log(LogLevel::debug, "gc", "gc_file_skipped", "", {{"path", candidate.path}, {"reason", "open"}});
            break;
        case GcOutcome::unsafe:
            counters_.skipped_unsafe.fetch_add(1, std::memory_order_relaxed);
            if (const auto v = limiter_.check("gc_unsafe"); v.allowed) {
                deps_.log.log(LogLevel::warning, "gc", "gc_file_skipped",
                              "not deleted: no longer a regular file inside the managed tree",
                              {{"path", candidate.path},
                               {"reason", "unsafe"},
                               {"errno", candidate.last_error},
                               {"suppressed", v.suppressed_before}});
            }
            break;
        case GcOutcome::retry:
        case GcOutcome::failed: break;  // Reported by handle_outcome(), which knows whether retries are left.
    }
    return outcome;
}

void GarbageCollector::report_failure(std::string_view event, std::string_view path, int error,
                                      std::string_view message) {
    if (const auto v = limiter_.check(std::format("{}:{}", event, error)); v.allowed) {
        deps_.log.log(
            LogLevel::warning, "gc", event, message,
            {{"path", path}, {"errno", error}, {"error", std::strerror(error)}, {"suppressed", v.suppressed_before}});
    }
}

void GarbageCollector::handle_outcome(GcCandidate& candidate, GcOutcome outcome, const GcSnapshot& snap, int error) {
    const TimePoint now = deps_.clock.now();
    switch (outcome) {
        case GcOutcome::not_expired:
        case GcOutcome::replaced:
            candidate.attempt = 0;
            if (candidate.due <= saturating_add(now, horizon(snap.config))) {
                schedule(std::move(candidate));
            }
            break;
        case GcOutcome::busy_open:
            candidate.due = saturating_add(now, snap.config.retry_initial_delay);
            schedule(std::move(candidate));
            break;
        case GcOutcome::retry:
            if (candidate.attempt >= snap.config.retry_limit) {
                counters_.failed.fetch_add(1, std::memory_order_relaxed);
                report_failure("gc_delete_failed", candidate.path, error,
                               "deletion failed; retries exhausted (the next pass will try again)");
                break;
            }
            ++candidate.attempt;
            counters_.retries.fetch_add(1, std::memory_order_relaxed);
            candidate.due = saturating_add(now, backoff(snap.config, candidate.attempt));
            deps_.log.log(LogLevel::debug, "gc", "gc_retry_scheduled", "",
                          {{"path", candidate.path},
                           {"attempt", candidate.attempt},
                           {"errno", error},
                           {"delay", format_duration(backoff(snap.config, candidate.attempt))}});
            schedule(std::move(candidate));
            break;
        case GcOutcome::failed:
            counters_.failed.fetch_add(1, std::memory_order_relaxed);
            report_failure("gc_delete_failed", candidate.path, error, "deletion failed permanently");
            break;
        default: break;
    }
}

void GarbageCollector::run_full_scan() {
    const auto snap = snapshot_.load();
    scanner_.reset();
    scanner_.set_want_birth(snap->policy->needs_birth_time());
    scan_started_ = SteadyClock::now();
    do {
        scan_slice(*snap, SteadyClock::now() + std::chrono::hours(24));
    } while (scanner_.in_progress());
}

std::size_t GarbageCollector::process_due_now() {
    std::size_t processed = 0;
    for (;;) {
        std::vector<GcCandidate> due;
        {
            std::scoped_lock lock(mutex_);
            scheduler_->pop_due(deps_.clock.now(), 1024, due);
        }
        if (due.empty()) {
            return processed;
        }
        for (GcCandidate& candidate : due) {
            const auto snap = snapshot_.load();
            const GcOutcome outcome = process_candidate(candidate);
            // Retries are rescheduled in the future, so this loop always terminates.
            handle_outcome(candidate, outcome, *snap, candidate.last_error);
            ++processed;
        }
    }
}

std::vector<DryRunEntry> GarbageCollector::dry_run_report() {
    std::vector<DryRunEntry> report;
    {
        std::scoped_lock lock(mutex_);
        dry_run_sink_ = &report;
    }
    run_full_scan();
    for (;;) {
        std::vector<GcCandidate> due;
        {
            std::scoped_lock lock(mutex_);
            scheduler_->pop_due(deps_.clock.now(), 1024, due);
        }
        if (due.empty()) {
            break;
        }
        for (GcCandidate& candidate : due) {
            process_candidate(candidate, /*force_dry_run=*/true);
        }
    }
    {
        std::scoped_lock lock(mutex_);
        dry_run_sink_ = nullptr;
    }
    std::ranges::sort(report, {}, &DryRunEntry::path);
    return report;
}

GcStats GarbageCollector::stats() const {
    GcStats s;
    const auto& c = counters_;
    constexpr auto r = std::memory_order_relaxed;
    s.candidates = c.candidates.load(r);
    s.deleted = c.deleted.load(r);
    s.bytes_deleted = c.bytes_deleted.load(r);
    s.would_delete = c.would_delete.load(r);
    s.vanished = c.vanished.load(r);
    s.skipped_modified = c.skipped_modified.load(r);
    s.skipped_replaced = c.skipped_replaced.load(r);
    s.skipped_open = c.skipped_open.load(r);
    s.skipped_unsafe = c.skipped_unsafe.load(r);
    s.failed = c.failed.load(r);
    s.retries = c.retries.load(r);
    s.queue_saturations = c.queue_saturations.load(r);
    // Acquire, paired with the release increment in run_purge(): last_purge_* read
    // below belong to (at least) this purge.
    s.purges_completed = c.purges_completed.load(std::memory_order_acquire);
    s.last_purge_deleted = c.last_purge_deleted.load(r);
    s.last_purge_bytes = c.last_purge_bytes.load(r);
    s.last_purge_failed = c.last_purge_failed.load(r);
    s.last_purge_skipped_open = c.last_purge_skipped_open.load(r);
    s.purging = purging_.load(r) ? 1 : 0;
    s.events_dropped = c.events_dropped.load(r);
    s.scans_completed = c.scans_completed.load(r);
    s.last_scan_files = c.last_scan_files.load(r);
    s.last_scan_duration_ms = c.last_scan_duration_ms.load(r);
    s.last_scan_completed_unix_ms = c.last_scan_completed_unix_ms.load(r);
    s.scan_time_ms = c.scan_time_ms.load(r);
    s.delete_time_us = c.delete_time_us.load(r);
    s.worker_busy_us = c.worker_busy_us.load(r);
    const auto snap = snapshot_.load();
    s.config_generation = snap->generation;
    {
        std::scoped_lock lock(mutex_);
        s.scheduled = scheduler_->size();
        s.pending_work = work_.size() + in_flight_;
    }
    {
        std::scoped_lock lock(workers_mutex_);
        s.workers = workers_.size();
    }
    return s;
}

}  // namespace tsfs
