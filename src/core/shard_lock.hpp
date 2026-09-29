#ifndef FARLANDS_SHARD_LOCK_HPP
#define FARLANDS_SHARD_LOCK_HPP

// Shard locking for ChunkMap: the RAII lock types, the timed acquisition
// helpers they are built on, and the per-shard telemetry the perf report reads.
//
// They live here rather than in chunk_map.hpp so that the map's header stays a
// description of the map. ChunkMap re-exports both lock types as member
// aliases, so `ChunkMap::ShardLock` / `ChunkMap::ExclusiveShardLock` still name
// them and no call site had to change when this was split out.

#include "core/lock_order_checker.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace VoxelEngine {

class ChunkMap; // friend of both lock types below

// How many shards the map is split across. ChunkMap::kNumShards aliases this.
// lock_order_checker.hpp tracks held shards in a std::bitset<64>, so the count
// can never exceed 64 — asserted here, where the number is defined.
inline constexpr size_t kShardCount = 64;
static_assert(kShardCount <= 64, "lock_order_checker's held-shard bitset is 64 wide");

// Per-shard lock telemetry, read by the perf report to attribute the main
// thread's spike frames. Every counter is relaxed: each field is independently
// consistent, but two fields are never consistent with each other, which is
// fine — these are diagnostics, not state.
struct ShardLockStats {
    std::atomic<uint64_t> shared_contended{0};
    std::atomic<uint64_t> shared_wait_total_ns{0};
    std::atomic<uint64_t> shared_wait_max_ns{0};
    std::atomic<uint64_t> excl_contended{0};
    std::atomic<uint64_t> excl_wait_total_ns{0};
    std::atomic<uint64_t> excl_wait_max_ns{0};
    std::atomic<uint64_t> excl_hold_total_ns{0};
    std::atomic<uint64_t> excl_hold_max_ns{0};

    void record_wait(bool exclusive, uint64_t ns) {
        if (exclusive) {
            excl_contended.fetch_add(1, std::memory_order_relaxed);
            excl_wait_total_ns.fetch_add(ns, std::memory_order_relaxed);
            uint64_t m = excl_wait_max_ns.load(std::memory_order_relaxed);
            while (ns > m && !excl_wait_max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
        } else {
            shared_contended.fetch_add(1, std::memory_order_relaxed);
            shared_wait_total_ns.fetch_add(ns, std::memory_order_relaxed);
            uint64_t m = shared_wait_max_ns.load(std::memory_order_relaxed);
            while (ns > m && !shared_wait_max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
        }
    }

    void record_hold(uint64_t ns) {
        excl_hold_total_ns.fetch_add(ns, std::memory_order_relaxed);
        uint64_t m = excl_hold_max_ns.load(std::memory_order_relaxed);
        while (ns > m && !excl_hold_max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    }

    // Aggregated across all shards, and drained: the counters reset so each
    // perf-report interval measures itself, matching reset_all() on the timers.
    void drain_into(uint64_t out[8]) {
        auto pull = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
        out[0] += pull(shared_contended);
        out[1] += pull(shared_wait_total_ns);
        out[2] = std::max(out[2], pull(shared_wait_max_ns));
        out[3] += pull(excl_contended);
        out[4] += pull(excl_wait_total_ns);
        out[5] = std::max(out[5], pull(excl_wait_max_ns));
        out[6] += pull(excl_hold_total_ns);
        out[7] = std::max(out[7], pull(excl_hold_max_ns));
    }
};

namespace shard_lock_detail {
// Contended-acquisition telemetry. The uncontended path is a single try_lock:
// no clock read, no counter write. A contended one pays one clock pair and a
// couple of relaxed fetch_adds — which is exactly the event being measured.
inline std::shared_lock<std::shared_mutex> lock_shared_timed(std::shared_mutex& m, ShardLockStats& st) {
    if (m.try_lock_shared()) return std::shared_lock<std::shared_mutex>(m, std::adopt_lock);
    const auto t0 = std::chrono::steady_clock::now();
    m.lock_shared();
    st.record_wait(false, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    return std::shared_lock<std::shared_mutex>(m, std::adopt_lock);
}

inline std::unique_lock<std::shared_mutex> lock_excl_timed(std::shared_mutex& m, ShardLockStats& st) {
    if (m.try_lock()) return std::unique_lock<std::shared_mutex>(m, std::adopt_lock);
    const auto t0 = std::chrono::steady_clock::now();
    m.lock();
    st.record_wait(true, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    return std::unique_lock<std::shared_mutex>(m, std::adopt_lock);
}

// Single-shard write accessors (insert/erase/...) use lock_excl_timed directly:
// wait telemetry only, no hold clock. Those writes run millions of times per
// session, and any per-call clock read or atomic write inside the critical
// section lands on the same cache line as the mutex — measurable overhead that
// amplifies the very convoys being diagnosed.
} // namespace shard_lock_detail

// RAII lock that holds shared_locks on one or more shards in ascending
// shard-index order (deadlock-safe).
class [[nodiscard]] ShardLock {
    friend class ChunkMap;
    std::vector<std::shared_lock<std::shared_mutex>> locks_;
#ifdef DEBUG_ENABLED
    std::vector<size_t> shard_indices_;
#endif
    ShardLock() = default;
public:
    ShardLock(ShardLock&&) = default;
#ifndef DEBUG_ENABLED
    ShardLock& operator=(ShardLock&&) = default;
#else
    ShardLock& operator=(ShardLock&& other) noexcept {
        if (this != &other) {
            for (auto si : shard_indices_) LOCK_ORDER_RELEASE(si);
            locks_ = std::move(other.locks_);
            shard_indices_ = std::move(other.shard_indices_);
        }
        return *this;
    }
#endif
    ~ShardLock() {
#ifdef DEBUG_ENABLED
        for (auto si : shard_indices_) LOCK_ORDER_RELEASE(si);
#endif
    }
    // Releases all held shard locks. Callers that re-lock periodically
    // (e.g. a long raycast refreshing its all-shard lock) MUST release
    // before re-acquiring: constructing a fresh lock_all() while the
    // previous one is still held is a recursive shared acquisition, which
    // SRW blocks forever once a writer is queued on any shard.
    void reset() noexcept {
#ifdef DEBUG_ENABLED
        for (auto si : shard_indices_) LOCK_ORDER_RELEASE(si);
        shard_indices_.clear();
#endif
        locks_.clear();
    }

#ifdef DEBUG_ENABLED
    // How many shards this lock actually holds. A lock's extent is otherwise
    // invisible from the outside, so tests read it here (debug builds only).
    [[nodiscard]] size_t shard_count() const noexcept { return locks_.size(); }
#endif
};

// RAII lock that holds unique_locks (exclusive) on one or more shards.
class [[nodiscard]] ExclusiveShardLock {
    friend class ChunkMap;
    std::vector<std::unique_lock<std::shared_mutex>> locks_;
#ifdef DEBUG_ENABLED
    std::vector<size_t> shard_indices_;
#endif
    ExclusiveShardLock() = default;
public:
    ExclusiveShardLock(ExclusiveShardLock&&) = default;
#ifndef DEBUG_ENABLED
    ExclusiveShardLock& operator=(ExclusiveShardLock&&) = default;
#else
    ExclusiveShardLock& operator=(ExclusiveShardLock&& other) noexcept {
        if (this != &other) {
            for (auto si : shard_indices_) LOCK_ORDER_RELEASE_EX(si);
            locks_ = std::move(other.locks_);
            shard_indices_ = std::move(other.shard_indices_);
            // The hold-telemetry state moves with the locks: a lock's
            // destructor reports the hold time from acquired_at_ over
            // held_count_ stats pointers, so leaving them behind would
            // report a zero hold on the moved-to lock and a bogus one on
            // the moved-from one. The source is zeroed, so destroying it
            // accounts nothing — which held_count_ == 0 already means.
            held_count_ = other.held_count_;
            for (size_t n = 0; n < held_count_; ++n)
                held_stats_[n] = other.held_stats_[n];
            acquired_at_ = other.acquired_at_;
            other.held_count_ = 0;
        }
        return *this;
    }
#endif
    ~ExclusiveShardLock() {
#ifdef DEBUG_ENABLED
        for (auto si : shard_indices_) LOCK_ORDER_RELEASE_EX(si);
#endif
        // Writer hold accounting for the multi-shard exclusive locks (the
        // light workers' 27-key region, the install path). These are rare
        // compared to the single-shard writes, so the clock read here is
        // nothing — and their hold time is the number that names a long
        // reader stall. No-op for the moved-from lock.
        if (held_count_ != 0) {
            const auto now = std::chrono::steady_clock::now();
            const uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - acquired_at_).count());
            for (size_t n = 0; n < held_count_; ++n) {
                held_stats_[n]->record_hold(ns);
            }
        }
    }

private:
    // The stats object of each acquired shard, filled at acquisition. Raw
    // pointers rather than a base+index because shards are bigger than
    // their stats — indexing from &shards_[0].stats would walk straight
    // into mutexes and maps.
    ShardLockStats* held_stats_[kShardCount] = {};
    size_t held_count_ = 0;
    std::chrono::steady_clock::time_point acquired_at_{};
};

} // namespace VoxelEngine

#endif // FARLANDS_SHARD_LOCK_HPP
