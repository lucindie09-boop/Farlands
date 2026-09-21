#ifndef FARLANDS_CHUNK_MAP_HPP
#define FARLANDS_CHUNK_MAP_HPP
#include "core/chunk_types.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <godot_cpp/variant/vector3.hpp>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <mutex>
#include <functional>
#include <array>
#include <vector>
#include <algorithm>
#include <cstring>
#include <atomic>
#include <chrono>
#include <utility>
#include <bit>
#include "core/lock_order_checker.hpp"

namespace VoxelEngine {

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

// -------------------------------------------------------------------------
// Chunk map — owns the chunk storage and provides thread-safe accessors.
// Every acquisition goes through shard_lock_detail's timed helpers, so the
// perf report can show lock waits and exclusive (writer) hold times directly.
// Uses 64 shards, each with its own unordered_map and shared_mutex, so a
// write on one shard never stalls readers on other shards.
//
// Cursor format for for_each_limited_resumable:
//   bits 0..31 = bucket index within the current shard
//   bits 32..63 = shard index
// -------------------------------------------------------------------------
class ChunkMap {
public:
    static constexpr size_t kNumShards = 64;

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
        ShardLockStats* held_stats_[kNumShards] = {};
        size_t held_count_ = 0;
        std::chrono::steady_clock::time_point acquired_at_{};
    };

    [[nodiscard]] inline uint64_t get_chunk_key(int32_t x, int32_t y, int32_t z) const noexcept {
        constexpr uint32_t OFFSET = 1u << 20;
        constexpr uint32_t MASK = 0x1FFFFF;
        uint64_t ux = static_cast<uint64_t>((static_cast<uint32_t>(x) + OFFSET) & MASK);
        uint64_t uy = static_cast<uint64_t>((static_cast<uint32_t>(y) + OFFSET) & MASK);
        uint64_t uz = static_cast<uint64_t>((static_cast<uint32_t>(z) + OFFSET) & MASK);
        return (ux << 42) | (uy << 21) | uz;
    }

    static inline void decode_chunk_key(uint64_t key, int32_t& x, int32_t& y, int32_t& z) noexcept {
        constexpr uint32_t OFFSET = 1u << 20;
        constexpr uint32_t MASK = 0x1FFFFF;
        x = static_cast<int32_t>((static_cast<uint32_t>((key >> 42) & MASK)) - OFFSET);
        y = static_cast<int32_t>((static_cast<uint32_t>((key >> 21) & MASK)) - OFFSET);
        z = static_cast<int32_t>((static_cast<uint32_t>(key & MASK)) - OFFSET);
    }

    void get_chunk_coords(const godot::Vector3& world_pos, int32_t& chunk_x, int32_t& chunk_y, int32_t& chunk_z) const noexcept {
        int32_t wx = static_cast<int32_t>(world_pos.x);
        int32_t wy = static_cast<int32_t>(world_pos.y);
        int32_t wz = static_cast<int32_t>(world_pos.z);
        int32_t dummy_x, dummy_y, dummy_z;
        world_to_chunk_local(wx, wy, wz, chunk_x, chunk_y, chunk_z, dummy_x, dummy_y, dummy_z);
    }

    [[nodiscard]] size_t shard_of(uint64_t key) const noexcept { return key_to_shard(key); }

    // -- Explicit shard locking (for callers that need multiple fast reads) --

    ShardLock lock_chunk(int32_t cx, int32_t cy, int32_t cz) const {
        ShardLock sl;
        size_t si = key_to_shard(get_chunk_key(cx, cy, cz));
        // Checked BEFORE the lock: a re-entrant read never returns, so there would be
        // nothing left to report with.
        LOCK_ORDER_REQUIRE_SHARED(si, "lock_chunk");
        sl.locks_.emplace_back(shard_lock_detail::lock_shared_timed(shards_[si].mutex, shards_[si].stats));
        LOCK_ORDER_ACQUIRE(si);
#ifdef DEBUG_ENABLED
        sl.shard_indices_.push_back(si);
#endif
        return sl;
    }

    ShardLock lock_keys(const std::vector<uint64_t>& keys) const {
        ShardLock sl;
        if (keys.empty()) return sl;
        bool seen[kNumShards] = {};
        for (auto k : keys) seen[key_to_shard(k)] = true;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            if (seen[i]) {
                LOCK_ORDER_REQUIRE_SHARED(i, "lock_keys");
                sl.locks_.emplace_back(shard_lock_detail::lock_shared_timed(shards_[i].mutex, shards_[i].stats));
                LOCK_ORDER_ACQUIRE(i);
#ifdef DEBUG_ENABLED
                sl.shard_indices_.push_back(i);
#endif
            }
        }
        return sl;
    }

    // Locks exactly `count` of `keys`. A caller that fills an array only partly
    // must use this form: the array overload below locks its whole extent, so a
    // half-filled array would read uninitialised entries and take shards that
    // have nothing to do with the request.
    ShardLock lock_keys(const uint64_t* keys, size_t count) const {
        ShardLock sl;
        if (count == 0) return sl;
        bool seen[kNumShards] = {};
        for (size_t i = 0; i < count; ++i) {
            seen[key_to_shard(keys[i])] = true;
        }
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            if (seen[i]) {
                LOCK_ORDER_REQUIRE_SHARED(i, "lock_keys");
                sl.locks_.emplace_back(shard_lock_detail::lock_shared_timed(shards_[i].mutex, shards_[i].stats));
                LOCK_ORDER_ACQUIRE(i);
#ifdef DEBUG_ENABLED
                sl.shard_indices_.push_back(i);
#endif
            }
        }
        return sl;
    }

    template<size_t N>
    ShardLock lock_keys(const uint64_t (&keys)[N]) const {
        return lock_keys(keys, N);
    }

    ExclusiveShardLock lock_keys_exclusive(const std::vector<uint64_t>& keys) const {
        ExclusiveShardLock sl;
        if (keys.empty()) return sl;
        bool seen[kNumShards] = {};
        for (auto k : keys) seen[key_to_shard(k)] = true;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            if (seen[i]) {
                sl.locks_.push_back(shard_lock_detail::lock_excl_timed(shards_[i].mutex, shards_[i].stats));
                sl.held_stats_[sl.held_count_++] = &shards_[i].stats;
                LOCK_ORDER_ACQUIRE_EX(i);
#ifdef DEBUG_ENABLED
                sl.shard_indices_.push_back(i);
#endif
            }
        }
        sl.acquired_at_ = std::chrono::steady_clock::now();
        return sl;
    }

    template<size_t N>
    ExclusiveShardLock lock_keys_exclusive(const uint64_t (&keys)[N]) const {
        ExclusiveShardLock sl;
        if constexpr (N == 0) {
            return sl;
        }
        bool seen[kNumShards] = {};
        for (size_t i = 0; i < N; ++i) {
            seen[key_to_shard(keys[i])] = true;
        }
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            if (seen[i]) {
                sl.locks_.push_back(shard_lock_detail::lock_excl_timed(shards_[i].mutex, shards_[i].stats));
                sl.held_stats_[sl.held_count_++] = &shards_[i].stats;
                LOCK_ORDER_ACQUIRE_EX(i);
#ifdef DEBUG_ENABLED
                sl.shard_indices_.push_back(i);
#endif
            }
        }
        sl.acquired_at_ = std::chrono::steady_clock::now();
        return sl;
    }

    ShardLock lock_all() const {
        ShardLock sl;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            LOCK_ORDER_REQUIRE_SHARED(i, "lock_all");
            sl.locks_.emplace_back(shard_lock_detail::lock_shared_timed(shards_[i].mutex, shards_[i].stats));
            LOCK_ORDER_ACQUIRE(i);
#ifdef DEBUG_ENABLED
            sl.shard_indices_.push_back(i);
#endif
        }
        return sl;
    }

    ExclusiveShardLock lock_all_exclusive() const {
        ExclusiveShardLock sl;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i) {
            sl.locks_.push_back(shard_lock_detail::lock_excl_timed(shards_[i].mutex, shards_[i].stats));
            sl.held_stats_[sl.held_count_++] = &shards_[i].stats;
            LOCK_ORDER_ACQUIRE_EX(i);
#ifdef DEBUG_ENABLED
            sl.shard_indices_.push_back(i);
#endif
        }
        sl.acquired_at_ = std::chrono::steady_clock::now();
        return sl;
    }

    // -- Per-shard chunk accessors (auto-locking) --

    [[nodiscard]] ChunkData* get_chunk_data(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "get_chunk_data");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second->data.get() : nullptr;
    }

    [[nodiscard]] ChunkRenderData* get_chunk_render_data(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "get_chunk_render_data");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second.get() : nullptr;
    }

    [[nodiscard]] bool has_loaded_chunk(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "has_loaded_chunk");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        return s.chunks.find(key) != s.chunks.end();
    }

    [[nodiscard]] bool is_block_solid(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "is_block_solid");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return false;
        return static_cast<BlockID>(it->second->data->get_block(lx, ly, lz)) != BlockIDs::AIR;
    }

    [[nodiscard]] int get_block_world(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "get_block_world");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return static_cast<int>(BlockIDs::AIR);
        return static_cast<int>(it->second->data->get_block(lx, ly, lz));
    }

    [[nodiscard]] bool contains(uint64_t key) const {
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "contains");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        return s.chunks.find(key) != s.chunks.end();
    }

    [[nodiscard]] size_t size() const {
        return chunk_count_.load(std::memory_order_relaxed);
    }

    // Aggregated, drained lock telemetry for the perf report (see ShardLockStats).
    // out: {shared_contended, shared_wait_total_ns, shared_wait_max_ns,
    //       excl_contended, excl_wait_total_ns, excl_wait_max_ns,
    //       excl_hold_total_ns, excl_hold_max_ns}
    void drain_shard_lock_stats(uint64_t out[8]) const {
        for (size_t i = 0; i < 8; ++i) out[i] = 0;
        for (auto& s : shards_) s.stats.drain_into(out);
    }

    // -- Fast-path accessors (caller must hold a ShardLock on the relevant shard) --

    [[nodiscard]] bool is_block_solid_fast(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return false;
        return static_cast<BlockID>(it->second->data->get_block(lx, ly, lz)) != BlockIDs::AIR;
    }

    [[nodiscard]] int get_block_world_fast(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return static_cast<int>(BlockIDs::AIR);
        return static_cast<int>(it->second->data->get_block(lx, ly, lz));
    }

    [[nodiscard]] ChunkData* get_chunk_data_fast(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second->data.get() : nullptr;
    }

    [[nodiscard]] ChunkRenderData* get_chunk_render_data_fast(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second.get() : nullptr;
    }

    [[nodiscard]] bool contains_fast(uint64_t key) const {
        auto& s = shards_[key_to_shard(key)];
        return s.chunks.find(key) != s.chunks.end();
    }

    // -- Write operations (auto-locking) --

    void clear() {
        auto all = lock_all();
        for (auto& s : shards_)
            s.chunks.clear();
        chunk_count_.store(0, std::memory_order_relaxed);
    }

    void erase(uint64_t key) {
        auto& s = shards_[key_to_shard(key)];
        auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
        if (s.chunks.erase(key) > 0) {
            chunk_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void insert(uint64_t key, std::unique_ptr<ChunkRenderData> render_data) {
        auto& s = shards_[key_to_shard(key)];
        auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
        auto [it, inserted] = s.chunks.insert_or_assign(key, std::move(render_data));
        (void)it;
        if (inserted) {
            chunk_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void reserve(size_t n) {
        auto all = lock_all();
        for (auto& s : shards_)
            s.chunks.reserve(n / kNumShards + 1);
    }

    [[nodiscard]] std::unique_ptr<ChunkRenderData> find_and_erase(uint64_t key) {
        auto& s = shards_[key_to_shard(key)];
        auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return nullptr;
        auto result = std::move(it->second);
        s.chunks.erase(it);
        chunk_count_.fetch_sub(1, std::memory_order_relaxed);
        return result;
    }

    template<typename Pred>
    [[nodiscard]] std::unique_ptr<ChunkRenderData> find_and_erase_if(uint64_t key, Pred&& predicate) {
        auto& s = shards_[key_to_shard(key)];
        auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return nullptr;
        if (!predicate(*it->second)) return nullptr;
        auto result = std::move(it->second);
        s.chunks.erase(it);
        chunk_count_.fetch_sub(1, std::memory_order_relaxed);
        return result;
    }

    // -- Batch neighbor lookups (lock multiple shards in order) --

    void get_neighbors(int32_t cx, int32_t cy, int32_t cz, ChunkRenderData* out[6]) const {
        uint64_t keys[6] = {
            get_chunk_key(cx - 1, cy,     cz    ),
            get_chunk_key(cx + 1, cy,     cz    ),
            get_chunk_key(cx,     cy - 1, cz    ),
            get_chunk_key(cx,     cy + 1, cz    ),
            get_chunk_key(cx,     cy,     cz - 1),
            get_chunk_key(cx,     cy,     cz + 1)
        };
        auto sl = lock_keys(keys);
        for (int i = 0; i < 6; ++i) {
            auto it = shards_[key_to_shard(keys[i])].chunks.find(keys[i]);
            out[i] = (it != shards_[key_to_shard(keys[i])].chunks.end()) ? it->second.get() : nullptr;
        }
    }

    // Returns all 26 neighbors as ChunkRenderData* pointers in this order:
    //   0: neg_x     1: pos_x     2: neg_y     3: pos_y
    //   4: neg_z     5: pos_z
    //   6-9: X-Z diagonals (neg_x_neg_z, neg_x_pos_z, pos_x_neg_z, pos_x_pos_z)
    //  10-13: Y-X diagonals (neg_x_neg_y, pos_x_neg_y, neg_x_pos_y, pos_x_pos_y)
    //  14-17: Y-Z diagonals (neg_y_neg_z, neg_y_pos_z, pos_y_neg_z, pos_y_pos_z)
    //  18-25: triple corners (neg_x_neg_y_neg_z .. pos_x_pos_y_pos_z)
    void pin_chunk(uint64_t key) {
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "pin_chunk");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it != s.chunks.end()) {
            it->second->pending_mesh_builds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void unpin_chunk(uint64_t key) {
        auto& s = shards_[key_to_shard(key)];
        LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "unpin_chunk");
        auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
        auto it = s.chunks.find(key);
        if (it != s.chunks.end()) {
            it->second->pending_mesh_builds.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void get_all_neighbors(int32_t cx, int32_t cy, int32_t cz,
                           ChunkRenderData* out[26]) const {
        uint64_t keys[26] = {
            get_chunk_key(cx - 1, cy,     cz    ),     // 0: neg_x
            get_chunk_key(cx + 1, cy,     cz    ),     // 1: pos_x
            get_chunk_key(cx,     cy - 1, cz    ),     // 2: neg_y
            get_chunk_key(cx,     cy + 1, cz    ),     // 3: pos_y
            get_chunk_key(cx,     cy,     cz - 1),     // 4: neg_z
            get_chunk_key(cx,     cy,     cz + 1),     // 5: pos_z
            get_chunk_key(cx - 1, cy,     cz - 1),     // 6: neg_x_neg_z
            get_chunk_key(cx - 1, cy,     cz + 1),     // 7: neg_x_pos_z
            get_chunk_key(cx + 1, cy,     cz - 1),     // 8: pos_x_neg_z
            get_chunk_key(cx + 1, cy,     cz + 1),     // 9: pos_x_pos_z
            get_chunk_key(cx - 1, cy - 1, cz    ),     //10: neg_x_neg_y
            get_chunk_key(cx + 1, cy - 1, cz    ),     //11: pos_x_neg_y
            get_chunk_key(cx - 1, cy + 1, cz    ),     //12: neg_x_pos_y
            get_chunk_key(cx + 1, cy + 1, cz    ),     //13: pos_x_pos_y
            get_chunk_key(cx,     cy - 1, cz - 1),     //14: neg_y_neg_z
            get_chunk_key(cx,     cy - 1, cz + 1),     //15: neg_y_pos_z
            get_chunk_key(cx,     cy + 1, cz - 1),     //16: pos_y_neg_z
            get_chunk_key(cx,     cy + 1, cz + 1),     //17: pos_y_pos_z
            get_chunk_key(cx - 1, cy - 1, cz - 1),     //18: neg_x_neg_y_neg_z
            get_chunk_key(cx + 1, cy - 1, cz - 1),     //19: pos_x_neg_y_neg_z
            get_chunk_key(cx - 1, cy + 1, cz - 1),     //20: neg_x_pos_y_neg_z
            get_chunk_key(cx + 1, cy + 1, cz - 1),     //21: pos_x_pos_y_neg_z
            get_chunk_key(cx - 1, cy - 1, cz + 1),     //22: neg_x_neg_y_pos_z
            get_chunk_key(cx + 1, cy - 1, cz + 1),     //23: pos_x_neg_y_pos_z
            get_chunk_key(cx - 1, cy + 1, cz + 1),     //24: neg_x_pos_y_pos_z
            get_chunk_key(cx + 1, cy + 1, cz + 1)      //25: pos_x_pos_y_pos_z
        };
        auto sl = lock_keys(keys);
        for (int i = 0; i < 26; ++i) {
            auto it = shards_[key_to_shard(keys[i])].chunks.find(keys[i]);
            out[i] = (it != shards_[key_to_shard(keys[i])].chunks.end()) ? it->second.get() : nullptr;
        }
    }

    // -- Global iteration --

    template<typename Callback>
    void for_each(Callback&& callback) {
        auto all = lock_all();
        for (auto& s : shards_)
            for (auto& pair : s.chunks)
                callback(pair.first, pair.second);
    }

    template<typename Callback>
    void for_each(Callback&& callback) const {
        auto all = lock_all();
        for (auto& s : shards_)
            for (auto& pair : s.chunks)
                callback(pair.first, pair.second);
    }

    template<typename Callback>
    bool for_each_limited(Callback&& callback, size_t max_count) const {
        auto all = lock_all();
        size_t count = 0;
        for (auto& s : shards_) {
            for (auto& pair : s.chunks) {
                if (count >= max_count) return false;
                callback(pair.first, pair.second);
                count++;
            }
        }
        return true;
    }

    template<typename Callback>
    void for_each_limited_resumable(Callback&& callback, size_t max_count, size_t& cursor) const {
        auto all = lock_all();

        size_t shard_idx = cursor >> 32;
        size_t bucket_idx = cursor & 0xFFFFFFFF;

        if (shard_idx >= kNumShards) { shard_idx = 0; bucket_idx = 0; }

        size_t visited = 0;
        while (shard_idx < kNumShards && visited < max_count) {
            auto& shard_map = shards_[shard_idx].chunks;
            size_t n_buckets = shard_map.bucket_count();
            if (n_buckets == 0) { ++shard_idx; bucket_idx = 0; continue; }
            if (bucket_idx >= n_buckets) { bucket_idx = 0; ++shard_idx; continue; }

            size_t buckets_scanned = 0;
            size_t b = bucket_idx;
            while (buckets_scanned < n_buckets && visited < max_count) {
                for (auto it = shard_map.begin(b); it != shard_map.end(b) && visited < max_count; ++it) {
                    callback(it->first, it->second);
                    ++visited;
                }
                b = (b + 1) % n_buckets;
                ++buckets_scanned;
            }

            if (visited >= max_count) {
                cursor = (shard_idx << 32) | b;
                return;
            }
            ++shard_idx;
            bucket_idx = 0;
        }

        cursor = 0;
    }

private:
    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, std::unique_ptr<ChunkRenderData>> chunks;
        ShardLockStats stats;
    };

    mutable std::array<Shard, kNumShards> shards_;
    std::atomic<size_t> chunk_count_{0};

    size_t key_to_shard(uint64_t key) const noexcept {
        // Mix all three packed coordinates into the shard index. The raw key's
        // low bits come only from z (x/y occupy bits 21..62), so a plain
        // `key % 64` put every chunk sharing a Z coordinate on the same shard
        // and collapsed a 3x3x3 neighborhood lock (3 distinct z values) to just
        // 3 shards. Fold x ^ y ^ z together, then avalanche via the murmur3
        // finalizer so unrelated (x,y,z) spread across all 64 shards.
        uint64_t h = (key >> 42) ^ (key >> 21) ^ key;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        return static_cast<size_t>(h % kNumShards);
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_MAP_HPP
