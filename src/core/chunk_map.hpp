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

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Chunk map — owns the chunk storage and provides thread-safe accessors.
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
        ShardLock() = default;
    public:
        ShardLock(ShardLock&&) = default;
        ShardLock& operator=(ShardLock&&) = default;
        // Releases all held shard locks. Callers that re-lock periodically
        // (e.g. a long raycast refreshing its all-shard lock) MUST release
        // before re-acquiring: constructing a fresh lock_all() while the
        // previous one is still held is a recursive shared acquisition, which
        // SRW blocks forever once a writer is queued on any shard.
        void reset() noexcept {
            locks_.clear();
        }
    };

    // RAII lock that holds unique_locks (exclusive) on one or more shards.
    class [[nodiscard]] ExclusiveShardLock {
        friend class ChunkMap;
        std::vector<std::unique_lock<std::shared_mutex>> locks_;
        ExclusiveShardLock() = default;
    public:
        ExclusiveShardLock(ExclusiveShardLock&&) = default;
        ExclusiveShardLock& operator=(ExclusiveShardLock&&) = default;
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
        sl.locks_.emplace_back(shards_[key_to_shard(get_chunk_key(cx, cy, cz))].mutex);
        return sl;
    }

    ShardLock lock_keys(const std::vector<uint64_t>& keys) const {
        ShardLock sl;
        if (keys.empty()) return sl;
        bool seen[kNumShards] = {};
        for (auto k : keys) seen[key_to_shard(k)] = true;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i)
            if (seen[i]) sl.locks_.emplace_back(shards_[i].mutex);
        return sl;
    }

    template<size_t N>
    ShardLock lock_keys(const uint64_t (&keys)[N]) const {
        ShardLock sl;
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
                sl.locks_.emplace_back(shards_[i].mutex);
            }
        }
        return sl;
    }

    ExclusiveShardLock lock_keys_exclusive(const std::vector<uint64_t>& keys) const {
        ExclusiveShardLock sl;
        if (keys.empty()) return sl;
        bool seen[kNumShards] = {};
        for (auto k : keys) seen[key_to_shard(k)] = true;
        sl.locks_.reserve(kNumShards);
        for (size_t i = 0; i < kNumShards; ++i)
            if (seen[i]) sl.locks_.emplace_back(shards_[i].mutex);
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
                sl.locks_.emplace_back(shards_[i].mutex);
            }
        }
        return sl;
    }

    ShardLock lock_all() const {
        ShardLock sl;
        sl.locks_.reserve(kNumShards);
        for (auto& s : shards_)
            sl.locks_.emplace_back(s.mutex);
        return sl;
    }

    ExclusiveShardLock lock_all_exclusive() const {
        ExclusiveShardLock sl;
        sl.locks_.reserve(kNumShards);
        for (auto& s : shards_)
            sl.locks_.emplace_back(s.mutex);
        return sl;
    }

    // -- Per-shard chunk accessors (auto-locking) --

    [[nodiscard]] ChunkData* get_chunk_data(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second->data.get() : nullptr;
    }

    [[nodiscard]] ChunkRenderData* get_chunk_render_data(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        auto it = s.chunks.find(key);
        return (it != s.chunks.end()) ? it->second.get() : nullptr;
    }

    [[nodiscard]] bool has_loaded_chunk(int32_t cx, int32_t cy, int32_t cz) const {
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        return s.chunks.find(key) != s.chunks.end();
    }

    [[nodiscard]] bool is_block_solid(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return false;
        return static_cast<BlockID>(it->second->data->get_block(lx, ly, lz)) != BlockIDs::AIR;
    }

    [[nodiscard]] int get_block_world(int32_t wx, int32_t wy, int32_t wz) const {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        uint64_t key = get_chunk_key(cx, cy, cz);
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        auto it = s.chunks.find(key);
        if (it == s.chunks.end()) return static_cast<int>(BlockIDs::AIR);
        return static_cast<int>(it->second->data->get_block(lx, ly, lz));
    }

    [[nodiscard]] bool contains(uint64_t key) const {
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
        return s.chunks.find(key) != s.chunks.end();
    }

    [[nodiscard]] size_t size() const {
        return chunk_count_.load(std::memory_order_relaxed);
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
        std::unique_lock lock(s.mutex);
        if (s.chunks.erase(key) > 0) {
            chunk_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void insert(uint64_t key, std::unique_ptr<ChunkRenderData> render_data) {
        auto& s = shards_[key_to_shard(key)];
        std::unique_lock lock(s.mutex);
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
        std::unique_lock lock(s.mutex);
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
        std::unique_lock lock(s.mutex);
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
        std::shared_lock lock(s.mutex);
        auto it = s.chunks.find(key);
        if (it != s.chunks.end()) {
            it->second->pending_mesh_builds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void unpin_chunk(uint64_t key) {
        auto& s = shards_[key_to_shard(key)];
        std::shared_lock lock(s.mutex);
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
