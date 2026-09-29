#ifndef FARLANDS_CHUNK_MAP_HPP
#define FARLANDS_CHUNK_MAP_HPP
#include "core/chunk_types.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/shard_lock.hpp"
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
#include <cmath>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Chunk map — owns the chunk storage and provides thread-safe accessors.
// Every acquisition goes through shard_lock_detail's timed helpers, so the
// perf report can show lock waits and exclusive (writer) hold times directly.
// Uses 64 shards, each with its own unordered_map and shared_mutex, so a
// write on one shard never stalls readers on other shards.
//
// The lock types and their telemetry live in core/shard_lock.hpp and are
// re-exported below as member aliases, so `ChunkMap::ShardLock` still names
// them. Every member that takes a shard lock FOR ITS CALLER — the explicit
// lock_* forms, the auto-locking accessors and writers, pin_chunk — is
// defined out of class in core/chunk_map_inline.hpp, included at the bottom
// of this file. What is left here is the key/coordinate codec, the LOCK-FREE
// surface (the _fast accessors, the batch neighbor lookups, the shard-at-a-
// time iteration) and the private storage.
//
// Cursor format for for_each_limited_resumable:
//   bits 0..31 = bucket index within the current shard
//   bits 32..63 = shard index
// -------------------------------------------------------------------------
class ChunkMap {
public:
    static constexpr size_t kNumShards = kShardCount;

    using ShardLock = VoxelEngine::ShardLock;
    using ExclusiveShardLock = VoxelEngine::ExclusiveShardLock;

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

    // Chunk of a world position. The components are FLOORED, not truncated:
    // static_cast truncates toward zero, which puts -0.5 blocks in chunk 0
    // rather than chunk -1 and makes every chunk-adjacency question near the
    // origin wrong by one on the negative side (the player's chunk-change
    // detection is the caller that cares). The integer branch inside
    // world_to_chunk_local is exact; this is the float boundary that was not.
    void get_chunk_coords(const godot::Vector3& world_pos, int32_t& chunk_x, int32_t& chunk_y, int32_t& chunk_z) const noexcept {
        int32_t wx = static_cast<int32_t>(std::floor(world_pos.x));
        int32_t wy = static_cast<int32_t>(std::floor(world_pos.y));
        int32_t wz = static_cast<int32_t>(std::floor(world_pos.z));
        int32_t dummy_x, dummy_y, dummy_z;
        world_to_chunk_local(wx, wy, wz, chunk_x, chunk_y, chunk_z, dummy_x, dummy_y, dummy_z);
    }

    [[nodiscard]] size_t shard_of(uint64_t key) const noexcept { return key_to_shard(key); }

    // -- Explicit shard locking (for callers that need multiple fast reads) --
    // The bodies of every member declared below live in core/chunk_map_inline.hpp
    // (included at the bottom of this file), not here.

    ShardLock lock_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    ShardLock lock_column(int32_t cx, int32_t cz) const;
    [[nodiscard]] size_t shard_of_column(int32_t cx, int32_t cz) const noexcept;
    ShardLock lock_keys(const std::vector<uint64_t>& keys) const;
    ShardLock lock_keys(const uint64_t* keys, size_t count) const;
    template<size_t N>
    ShardLock lock_keys(const uint64_t (&keys)[N]) const;
    ExclusiveShardLock lock_keys_exclusive(const std::vector<uint64_t>& keys) const;
    template<size_t N>
    ExclusiveShardLock lock_keys_exclusive(const uint64_t (&keys)[N]) const;
    ShardLock lock_all() const;
    ExclusiveShardLock lock_all_exclusive() const;

    // -- Per-shard chunk accessors and writers (auto-locking) --

    [[nodiscard]] ChunkData* get_chunk_data(int32_t cx, int32_t cy, int32_t cz) const;
    [[nodiscard]] ChunkRenderData* get_chunk_render_data(int32_t cx, int32_t cy, int32_t cz) const;
    [[nodiscard]] bool has_loaded_chunk(int32_t cx, int32_t cy, int32_t cz) const;
    [[nodiscard]] bool is_block_solid(int32_t wx, int32_t wy, int32_t wz) const;
    [[nodiscard]] int get_block_world(int32_t wx, int32_t wy, int32_t wz) const;
    [[nodiscard]] bool contains(uint64_t key) const;

    // Wipes the map. See chunk_map_inline.hpp for why every shard is taken
    // EXCLUSIVE rather than shared.
    void clear();
    void erase(uint64_t key);
    void insert(uint64_t key, std::unique_ptr<ChunkRenderData> render_data);
    // Pre-sizes every shard's map, exclusively (see chunk_map_inline.hpp).
    void reserve(size_t n);
    [[nodiscard]] std::unique_ptr<ChunkRenderData> find_and_erase(uint64_t key);
    template<typename Pred>
    [[nodiscard]] std::unique_ptr<ChunkRenderData> find_and_erase_if(uint64_t key, Pred&& predicate);

    // -- Mesh-build pins: hold the shard shared, bump the counter atomically --

    void pin_chunk(uint64_t key);
    void unpin_chunk(uint64_t key);

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
    //
    // ONE SHARD AT A TIME, and only while it is being read — never `lock_all()`.
    // These are sweeps over "what is currently loaded" (free the instances, mark
    // the meshes dirty, count the far-eligible), not snapshots, and taking all 64
    // shards SHARED for the whole walk means every chunk insert in the game — the
    // install path and the light workers, exclusive, thousands a second — queues
    // behind a scan of a 36k-chunk map. That makes the queue of writers that a
    // reader then queues behind, which is exactly the convoy `for_each_limited_
    // resumable` was rewritten to stop feeding (see that function for the measured
    // wait/hold ratio). A visitor must therefore NOT touch the map itself: it holds
    // one shard, and taking another here would be a nested acquisition in the wrong
    // order. Chunks never move between shards (the key fixes the shard), so a chunk
    // cannot be visited twice or escape a shard's lock, and the only difference a
    // caller can observe is that a chunk may be inserted or erased between shards —
    // which is what "what is loaded" means anyway.

    template<typename Callback>
    void for_each(Callback&& callback) {
        for (size_t i = 0; i < kNumShards; ++i) {
            auto lock = shard_lock_detail::lock_shared_timed(shards_[i].mutex, shards_[i].stats);
            for (auto& pair : shards_[i].chunks)
                callback(pair.first, pair.second);
        }
    }

    template<typename Callback>
    void for_each(Callback&& callback) const {
        for (size_t i = 0; i < kNumShards; ++i) {
            auto lock = shard_lock_detail::lock_shared_timed(shards_[i].mutex, shards_[i].stats);
            for (auto& pair : shards_[i].chunks)
                callback(pair.first, pair.second);
        }
    }

    // Walks the map in cursor-resumable slices for callers that are SCANNING it
    // rather than taking a consistent snapshot (the unload pass: "which loaded
    // chunks are now out of range").
    //
    // One shard is locked at a time, and only while it is being read. Holding
    // lock_all() for the whole walk — which is what this did — takes every shard
    // SHARED for the entire slice, so on a 100k-chunk map every chunk insert
    // (exclusive, from the install path and the workers) queues behind a scan that
    // runs every frame or two. That is the convoy the reader-side telemetry shows
    // as 100-260 ms waits while the worst single hold is a few ms: the wait is the
    // queue of writers, not one writer. Readers are unaffected by another reader, so
    // acquiring per shard costs the map nothing and lets the writers interleave
    // between shards. The visitor must not touch the map itself — it holds one
    // shard, and taking another here would be a nested acquisition in the wrong
    // order.
    template<typename Callback>
    void for_each_limited_resumable(Callback&& callback, size_t max_count, size_t& cursor) const {
        size_t shard_idx = cursor >> 32;
        size_t bucket_idx = cursor & 0xFFFFFFFF;

        if (shard_idx >= kNumShards) { shard_idx = 0; bucket_idx = 0; }

        size_t visited = 0;
        while (shard_idx < kNumShards && visited < max_count) {
            {
                auto lock = shard_lock_detail::lock_shared_timed(shards_[shard_idx].mutex, shards_[shard_idx].stats);
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

    // A shard is chosen per COLUMN, not per chunk: the y field (bits 21..41) is
    // masked out before hashing, so every chunk of one (x, z) column lands on
    // one shard.
    //
    // That is what makes a column's questions cheap. The generation sweep asks
    // "is this whole column built?" once per column per pass, and the answer is
    // count(band) chunk lookups — a band reaches the world floor near the player.
    // With a shard per chunk, each of those lookups was its own shared_mutex
    // acquisition on its own shard, so one column spanned up to 16 shards and
    // could queue behind a writing worker on any one of them. Measured with
    // /genstats: the slowest single column spent 39.4 ms of its 39.6 ms in 7 such
    // lookups, and 39.4 ms of that was ONE lookup waiting for a shard. With the
    // column on one shard, the whole check costs ONE acquisition
    // (ChunkMap::lock_column) followed by lock-free probes.
    //
    // It also shrinks every multi-key lock, because the keys of a neighborhood
    // collapse onto shared shards: a 3x3x3 is 9 distinct (x, z) columns, so the
    // light region's 27-key exclusive pass takes 9 shards rather than up to 27,
    // and the install path's 28-key probe likewise.
    //
    // Not `key % kNumShards`: the raw key's low bits come only from z (x and y
    // sit at bits 21..62), so x would never reach the index. Fold x and z
    // together and avalanche via the murmur3 finalizer so unrelated columns
    // still spread across all 64 shards.
    static constexpr uint64_t kYFieldMask = uint64_t{0x1FFFFF} << 21;

    size_t key_to_shard(uint64_t key) const noexcept {
        uint64_t h = key & ~kYFieldMask;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDULL;
        h ^= h >> 33;
        h *= 0xC4CEB9FE1A85EC53ULL;
        h ^= h >> 33;
        return static_cast<size_t>(h % kNumShards);
    }
};

} // namespace VoxelEngine

// Bodies for every member that takes a shard lock for its caller. Included at
// the bottom so those definitions see a complete ChunkMap; never include it
// anywhere else.
#include "core/chunk_map_inline.hpp"

#endif // FARLANDS_CHUNK_MAP_HPP
