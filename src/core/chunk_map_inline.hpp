// Out-of-class bodies for every ChunkMap member that takes a shard lock for
// its caller: the explicit lock_* forms, the auto-locking accessors and
// writers, and the mesh-build pin counters.
//
// This file is included at the BOTTOM of core/chunk_map.hpp, after the class
// definition, and must never be included anywhere else: defining members of a
// complete ChunkMap is the whole point of the placement. The definitions are
// `inline`, exactly as they were inside the class body, so nothing about
// inlining or codegen changes — only where the source is written.
//
// What is deliberately NOT here: the lock-free surface (_fast accessors, the
// batch neighbor lookups, the shard-at-a-time iteration). Those read the map
// without taking one of its locks, and they stay next to the class so that a
// caller reading chunk_map.hpp can see what is safe under an existing lock.

#ifndef FARLANDS_CHUNK_MAP_HPP
#error "chunk_map_inline.hpp is included at the bottom of chunk_map.hpp; include that instead"
#endif

namespace VoxelEngine {

// -- Explicit shard locking (for callers that need multiple fast reads) --

inline ChunkMap::ShardLock ChunkMap::lock_chunk(int32_t cx, int32_t cy, int32_t cz) const {
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

// Shared lock on the ONE shard that owns a COLUMN (see key_to_shard). A
// caller that must ask several questions about one column — the sweep's "is
// the whole band resident?" is count(band) of them — takes this once and then
// uses the `_fast` accessors, which take no lock at all. This is only sound
// because sharding is by column: every chunk key of (cx, cz) resolves to the
// shard this returns.
inline ChunkMap::ShardLock ChunkMap::lock_column(int32_t cx, int32_t cz) const {
    ShardLock sl;
    const size_t si = key_to_shard(get_chunk_key(cx, 0, cz));
    LOCK_ORDER_REQUIRE_SHARED(si, "lock_column");
    sl.locks_.emplace_back(shard_lock_detail::lock_shared_timed(shards_[si].mutex, shards_[si].stats));
    LOCK_ORDER_ACQUIRE(si);
#ifdef DEBUG_ENABLED
    sl.shard_indices_.push_back(si);
#endif
    return sl;
}

// The number of a column's shard, for callers that want to assert the
// invariant above rather than assume it.
inline size_t ChunkMap::shard_of_column(int32_t cx, int32_t cz) const noexcept {
    return key_to_shard(get_chunk_key(cx, 0, cz));
}

inline ChunkMap::ShardLock ChunkMap::lock_keys(const std::vector<uint64_t>& keys) const {
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
inline ChunkMap::ShardLock ChunkMap::lock_keys(const uint64_t* keys, size_t count) const {
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
ChunkMap::ShardLock ChunkMap::lock_keys(const uint64_t (&keys)[N]) const {
    return lock_keys(keys, N);
}

inline ChunkMap::ExclusiveShardLock ChunkMap::lock_keys_exclusive(const std::vector<uint64_t>& keys) const {
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
ChunkMap::ExclusiveShardLock ChunkMap::lock_keys_exclusive(const uint64_t (&keys)[N]) const {
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

inline ChunkMap::ShardLock ChunkMap::lock_all() const {
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

inline ChunkMap::ExclusiveShardLock ChunkMap::lock_all_exclusive() const {
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

inline ChunkData* ChunkMap::get_chunk_data(int32_t cx, int32_t cy, int32_t cz) const {
    uint64_t key = get_chunk_key(cx, cy, cz);
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "get_chunk_data");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    auto it = s.chunks.find(key);
    return (it != s.chunks.end()) ? it->second->data.get() : nullptr;
}

inline ChunkRenderData* ChunkMap::get_chunk_render_data(int32_t cx, int32_t cy, int32_t cz) const {
    uint64_t key = get_chunk_key(cx, cy, cz);
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "get_chunk_render_data");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    auto it = s.chunks.find(key);
    return (it != s.chunks.end()) ? it->second.get() : nullptr;
}

inline bool ChunkMap::has_loaded_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    uint64_t key = get_chunk_key(cx, cy, cz);
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "has_loaded_chunk");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    return s.chunks.find(key) != s.chunks.end();
}

inline bool ChunkMap::is_block_solid(int32_t wx, int32_t wy, int32_t wz) const {
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

inline int ChunkMap::get_block_world(int32_t wx, int32_t wy, int32_t wz) const {
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

inline bool ChunkMap::contains(uint64_t key) const {
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "contains");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    return s.chunks.find(key) != s.chunks.end();
}

// -- Write operations (auto-locking) --

// Wipes the map. Every shard is taken EXCLUSIVE in ascending order: clear()
// mutates each shard's unordered_map, and a shared lock on a shard being
// cleared is a data race against any other reader of that shard (a rehash
// under a reader is a crash, not a wrong answer). Callers are teardown
// paths — free_loaded_chunks() and the world reset — so the exclusive cost
// is nil, and the callers' own contract is that the worker pool is already
// shut down or an epoch bump has fenced the in-flight jobs.
inline void ChunkMap::clear() {
    auto all = lock_all_exclusive();
    for (auto& s : shards_)
        s.chunks.clear();
    chunk_count_.store(0, std::memory_order_relaxed);
}

inline void ChunkMap::erase(uint64_t key) {
    auto& s = shards_[key_to_shard(key)];
    auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
    if (s.chunks.erase(key) > 0) {
        chunk_count_.fetch_sub(1, std::memory_order_relaxed);
    }
}

inline void ChunkMap::insert(uint64_t key, std::unique_ptr<ChunkRenderData> render_data) {
    auto& s = shards_[key_to_shard(key)];
    auto lock = shard_lock_detail::lock_excl_timed(s.mutex, s.stats);
    auto [it, inserted] = s.chunks.insert_or_assign(key, std::move(render_data));
    (void)it;
    if (inserted) {
        chunk_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

// Pre-sizes every shard's map. Exclusive per shard, for the same reason
// clear() is: rehashing a map a reader is walking is a data race, and a
// shared lock does not stop readers. Mostly called before the worker pool
// exists (initialize()); set_render_distance() can call it mid-session,
// which is exactly when the exclusive lock matters.
inline void ChunkMap::reserve(size_t n) {
    auto all = lock_all_exclusive();
    for (auto& s : shards_)
        s.chunks.reserve(n / kNumShards + 1);
}

inline std::unique_ptr<ChunkRenderData> ChunkMap::find_and_erase(uint64_t key) {
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
std::unique_ptr<ChunkRenderData> ChunkMap::find_and_erase_if(uint64_t key, Pred&& predicate) {
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

// -- Mesh-build pins (hold the shard shared, bump the counter atomically) --

inline void ChunkMap::pin_chunk(uint64_t key) {
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "pin_chunk");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    auto it = s.chunks.find(key);
    if (it != s.chunks.end()) {
        it->second->pending_mesh_builds.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void ChunkMap::unpin_chunk(uint64_t key) {
    auto& s = shards_[key_to_shard(key)];
    LOCK_ORDER_REQUIRE_SHARED(key_to_shard(key), "unpin_chunk");
    auto lock = shard_lock_detail::lock_shared_timed(s.mutex, s.stats);
    auto it = s.chunks.find(key);
    if (it != s.chunks.end()) {
        it->second->pending_mesh_builds.fetch_sub(1, std::memory_order_relaxed);
    }
}

} // namespace VoxelEngine
