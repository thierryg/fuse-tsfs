// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : src/gc/scheduler.cpp
// Purpose : Bounded expiration schedulers keyed by managed path (heap and ordered strategies)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include "gc/scheduler.hpp"

#include <algorithm>

namespace tsfs {

// --- HeapScheduler ---------------------------------------------------------------

void HeapScheduler::push_heap(const HeapEntry& entry) {
    heap_.push_back(entry);
    std::ranges::push_heap(heap_, later);
}

void HeapScheduler::release(std::uint32_t slot) {
    Slot& s = slots_[slot];
    s.live = false;
    ++s.generation;  // Every heap entry pointing at the old generation is now a tombstone.
    s.candidate = {};
    free_.push_back(slot);
}

ScheduleResult HeapScheduler::schedule(GcCandidate candidate) {
    if (const auto it = index_.find(candidate.path); it != index_.end()) {
        Slot& slot = slots_[it->second];
        ++slot.generation;
        slot.candidate = std::move(candidate);
        push_heap({slot.candidate.due, it->second, slot.generation});
        maybe_compact();
        return ScheduleResult::updated;
    }
    if (index_.size() >= capacity_) {
        return ScheduleResult::rejected_full;
    }
    std::uint32_t slot = 0;
    if (!free_.empty()) {
        slot = free_.back();
        free_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    Slot& s = slots_[slot];
    s.live = true;
    s.candidate = std::move(candidate);
    index_.emplace(s.candidate.path, slot);
    push_heap({s.candidate.due, slot, s.generation});
    maybe_compact();
    return ScheduleResult::inserted;
}

bool HeapScheduler::cancel(std::string_view path) {
    const auto it = index_.find(path);
    if (it == index_.end()) {
        return false;
    }
    const std::uint32_t slot = it->second;
    index_.erase(it);
    release(slot);
    maybe_compact();
    return true;
}

const GcCandidate* HeapScheduler::find(std::string_view path) const {
    const auto it = index_.find(path);
    return it == index_.end() ? nullptr : &slots_[it->second].candidate;
}

void HeapScheduler::drop_tombstones() {
    while (!heap_.empty()) {
        const HeapEntry& top = heap_.front();
        const Slot& slot = slots_[top.slot];
        if (slot.live && slot.generation == top.generation) {
            return;
        }
        std::ranges::pop_heap(heap_, later);
        heap_.pop_back();
    }
}

void HeapScheduler::maybe_compact() {
    if (heap_.size() <= 2 * index_.size() + 1024) {
        return;
    }
    std::erase_if(heap_, [this](const HeapEntry& e) {
        const Slot& slot = slots_[e.slot];
        return !slot.live || slot.generation != e.generation;
    });
    std::ranges::make_heap(heap_, later);
}

std::optional<TimePoint> HeapScheduler::next_due() {
    drop_tombstones();
    if (heap_.empty()) {
        return std::nullopt;
    }
    return heap_.front().due;
}

std::size_t HeapScheduler::pop_due(TimePoint now, std::size_t max, std::vector<GcCandidate>& out) {
    std::size_t popped = 0;
    while (popped < max) {
        drop_tombstones();
        if (heap_.empty() || heap_.front().due > now) {
            break;
        }
        const HeapEntry top = heap_.front();
        std::ranges::pop_heap(heap_, later);
        heap_.pop_back();
        Slot& slot = slots_[top.slot];
        index_.erase(slot.candidate.path);
        out.push_back(std::move(slot.candidate));
        release(top.slot);
        ++popped;
    }
    return popped;
}

void HeapScheduler::clear() {
    slots_.clear();
    free_.clear();
    index_.clear();
    heap_.clear();
}

// --- OrderedScheduler ------------------------------------------------------------

ScheduleResult OrderedScheduler::schedule(GcCandidate candidate) {
    ScheduleResult result = ScheduleResult::inserted;
    if (const auto it = index_.find(candidate.path); it != index_.end()) {
        by_due_.erase(it->second);
        index_.erase(it);
        result = ScheduleResult::updated;
    } else if (index_.size() >= capacity_) {
        return ScheduleResult::rejected_full;
    }
    const Key key{candidate.due, sequence_++};
    std::string path = candidate.path;
    by_due_.emplace(key, std::move(candidate));
    index_.emplace(std::move(path), key);
    return result;
}

bool OrderedScheduler::cancel(std::string_view path) {
    const auto it = index_.find(path);
    if (it == index_.end()) {
        return false;
    }
    by_due_.erase(it->second);
    index_.erase(it);
    return true;
}

const GcCandidate* OrderedScheduler::find(std::string_view path) const {
    const auto it = index_.find(path);
    return it == index_.end() ? nullptr : &by_due_.at(it->second);
}

std::optional<TimePoint> OrderedScheduler::next_due() {
    if (by_due_.empty()) {
        return std::nullopt;
    }
    return by_due_.begin()->first.first;
}

std::size_t OrderedScheduler::pop_due(TimePoint now, std::size_t max, std::vector<GcCandidate>& out) {
    std::size_t popped = 0;
    while (popped < max && !by_due_.empty() && by_due_.begin()->first.first <= now) {
        auto node = by_due_.extract(by_due_.begin());
        index_.erase(node.mapped().path);
        out.push_back(std::move(node.mapped()));
        ++popped;
    }
    return popped;
}

void OrderedScheduler::clear() {
    by_due_.clear();
    index_.clear();
}

std::unique_ptr<ExpirationScheduler> make_scheduler(SchedulerKind kind, std::size_t capacity) {
    if (kind == SchedulerKind::ordered) {
        return std::make_unique<OrderedScheduler>(capacity);
    }
    return std::make_unique<HeapScheduler>(capacity);
}

}  // namespace tsfs
