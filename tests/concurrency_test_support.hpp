#ifndef FARLANDS_TESTS_CONCURRENCY_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_CONCURRENCY_TEST_SUPPORT_HPP

// =========================================================================
// Fixtures for the concurrency tests.
//
// The three race patterns the tests below exercise need a subject to race on:
// TestShardMap is a 64-shard map with the same locking shape as ChunkMap (and
// without godot::RID, which needs the engine runtime), CrossChunkWriter is the
// queue_pending_placement + pending_cross_boundary_remesh pattern from
// chunk_world.cpp, and PendingLightRemovals is the light_propagator.cpp insert /
// fixup-erase pattern. Each mirrors the production code it stands in for.
//
// These were file-local to test_concurrency.cpp; the split moved them here.
// =========================================================================

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace VoxelEngine;

namespace concurrency_test {

// =========================================================================
// Helper: lightweight shard map that mirrors ChunkMap's 64-shard locking
// without pulling in godot::RID (which needs the engine runtime).
// =========================================================================
class TestShardMap {
public:
    static constexpr size_t kNumShards = 64;

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, std::unique_ptr<ChunkData>> chunks;
    };

    std::array<Shard, kNumShards> shards_;
    std::atomic<size_t> count_{0};

    static uint64_t key(int32_t x, int32_t y, int32_t z) {
        constexpr uint32_t OFF = 1u << 20;
        constexpr uint32_t MASK = 0x1FFFFF;
        uint64_t ux = static_cast<uint64_t>((static_cast<uint32_t>(x) + OFF) & MASK);
        uint64_t uy = static_cast<uint64_t>((static_cast<uint32_t>(y) + OFF) & MASK);
        uint64_t uz = static_cast<uint64_t>((static_cast<uint32_t>(z) + OFF) & MASK);
        return (ux << 42) | (uy << 21) | uz;
    }

    size_t shard_of(uint64_t k) const { return k % kNumShards; }

    void insert(uint64_t k, std::unique_ptr<ChunkData> d) {
        auto& s = shards_[shard_of(k)];
        std::unique_lock lk(s.mutex);
        auto [it, ins] = s.chunks.insert_or_assign(k, std::move(d));
        (void)it;
        if (ins) count_.fetch_add(1, std::memory_order_relaxed);
    }

    void erase(uint64_t k) {
        auto& s = shards_[shard_of(k)];
        std::unique_lock lk(s.mutex);
        if (s.chunks.erase(k) > 0)
            count_.fetch_sub(1, std::memory_order_relaxed);
    }

    ChunkData* get(uint64_t k) const {
        auto& s = shards_[shard_of(k)];
        std::shared_lock lk(s.mutex);
        auto it = s.chunks.find(k);
        return (it != s.chunks.end()) ? it->second.get() : nullptr;
    }

    size_t size() const { return count_.load(std::memory_order_relaxed); }

    // RAII shared lock on a single shard
    class [[nodiscard]] ShardLock {
        std::shared_lock<std::shared_mutex> lk_;
    public:
        ShardLock(const TestShardMap& m, uint64_t k)
            : lk_(m.shards_[m.shard_of(k)].mutex) {}
    };

    // RAII exclusive lock on a single shard
    class [[nodiscard]] ExclusiveShardLock {
        std::unique_lock<std::shared_mutex> lk_;
    public:
        ExclusiveShardLock(const TestShardMap& m, uint64_t k)
            : lk_(m.shards_[m.shard_of(k)].mutex) {}
    };

    // Lock shards in ascending order (deadlock-safe)
    class [[nodiscard]] OrderedShardLock {
        std::vector<std::shared_lock<std::shared_mutex>> lks_;
    public:
        OrderedShardLock() = default;
        OrderedShardLock(const TestShardMap& m, const std::vector<uint64_t>& keys) {
            bool seen[kNumShards] = {};
            for (auto k : keys) seen[m.shard_of(k)] = true;
            lks_.reserve(kNumShards);
            for (size_t i = 0; i < kNumShards; ++i)
                if (seen[i]) lks_.emplace_back(m.shards_[i].mutex);
        }
    };

    // Lock all shards with shared locks
    class [[nodiscard]] AllSharedLock {
        std::vector<std::shared_lock<std::shared_mutex>> lks_;
    public:
        AllSharedLock() = default;
        explicit AllSharedLock(const TestShardMap& m) {
            lks_.reserve(kNumShards);
            for (auto& s : m.shards_)
                lks_.emplace_back(s.mutex);
        }
    };

    // Lock all shards with exclusive locks
    class [[nodiscard]] AllExclusiveLock {
        std::vector<std::unique_lock<std::shared_mutex>> lks_;
    public:
        AllExclusiveLock() = default;
        explicit AllExclusiveLock(const TestShardMap& m) {
            lks_.reserve(kNumShards);
            for (auto& s : m.shards_)
                lks_.emplace_back(s.mutex);
        }
    };

    // Lock shards in ascending order with exclusive locks (mirrors ChunkMap::lock_keys_exclusive)
    class [[nodiscard]] OrderedExclusiveShardLock {
        std::vector<std::unique_lock<std::shared_mutex>> lks_;
    public:
        OrderedExclusiveShardLock() = default;
        OrderedExclusiveShardLock(const TestShardMap& m, const std::vector<uint64_t>& keys) {
            bool seen[kNumShards] = {};
            for (auto k : keys) seen[m.shard_of(k)] = true;
            lks_.reserve(kNumShards);
            for (size_t i = 0; i < kNumShards; ++i)
                if (seen[i]) lks_.emplace_back(m.shards_[i].mutex);
        }
    };
};

// =========================================================================
// Cross-chunk writer race: test the actual production pattern from
//     chunk_world.cpp (queue_pending_placement + pending_cross_boundary_remesh)
// =========================================================================
struct PendingBlockPlacement {
    int32_t world_x = 0;
    int32_t world_y = 0;
    int32_t world_z = 0;
    int block_id = 0;
};

struct TestChunkPos {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

// Mirrors the production pattern in chunk_world.cpp lines 625-631 and 73-81
class CrossChunkWriter {
public:
    void queue_pending_placement(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
        int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
        world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);
        uint64_t key = TestShardMap::key(chunk_x, chunk_y, chunk_z);
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        pending_block_placements[key].push_back({world_x, world_y, world_z, block_id});
    }

    void queue_cross_boundary_remesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
        std::lock_guard<std::mutex> lock(cross_boundary_mutex);
        pending_cross_boundary_remesh.push_back({chunk_x, chunk_y, chunk_z});
    }

    // Simulates the production cross_writer lambda from chunk_world.cpp lines 73-81
    auto make_cross_writer() {
        return [this](int32_t wx, int32_t wy, int32_t wz, int block_id) {
            queue_pending_placement(wx, wy, wz, block_id);
            int32_t tc_x, tc_y, tc_z, lx, ly, lz;
            world_to_chunk_local(wx, wy, wz, tc_x, tc_y, tc_z, lx, ly, lz);
            queue_cross_boundary_remesh(tc_x, tc_y, tc_z);
        };
    }

    size_t total_pending_count() const {
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        size_t total = 0;
        for (const auto& [k, v] : pending_block_placements)
            total += v.size();
        return total;
    }

    size_t cross_boundary_count() const {
        std::lock_guard<std::mutex> lock(cross_boundary_mutex);
        return pending_cross_boundary_remesh.size();
    }

    // Mirrors apply_pending_placements from chunk_world.cpp lines 633-651
    std::vector<PendingBlockPlacement> dequeue_placements(uint64_t key) {
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        auto it = pending_block_placements.find(key);
        if (it != pending_block_placements.end()) {
            std::vector<PendingBlockPlacement> result = std::move(it->second);
            pending_block_placements.erase(it);
            return result;
        }
        return {};
    }

    // Get all keys currently in the map (for draining)
    std::vector<uint64_t> get_all_keys() const {
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        std::vector<uint64_t> keys;
        keys.reserve(pending_block_placements.size());
        for (const auto& [k, v] : pending_block_placements)
            keys.push_back(k);
        return keys;
    }

private:
    std::unordered_map<uint64_t, std::vector<PendingBlockPlacement>> pending_block_placements;
    mutable std::mutex pending_placement_mutex;
    std::vector<TestChunkPos> pending_cross_boundary_remesh;
    mutable std::mutex cross_boundary_mutex;
};

// =========================================================================
// pending_light_removals_ stress: concurrent BFS insert + fixup erase
//     Tests the actual production pattern from light_propagator.cpp
// =========================================================================
// Mirrors the production pattern in light_propagator.cpp lines 39-40 and 253-258
class PendingLightRemovals {
public:
    // Called from BFS in light_propagate_add_locked/light_propagate_remove_locked
    void insert(uint64_t chunk_key) {
        std::lock_guard<std::mutex> guard(mutex_);
        pending_.insert(chunk_key);
    }

    // Called from try_fixup_chunk (light_propagator.cpp lines 253-258)
    bool try_erase(uint64_t chunk_key) {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = pending_.find(chunk_key);
        if (it != pending_.end()) {
            pending_.erase(it);
            return true;
        }
        return false;
    }

    size_t size() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return pending_.size();
    }

private:
    std::unordered_set<uint64_t> pending_;
    mutable std::mutex mutex_;
};

} // namespace concurrency_test

#endif // FARLANDS_TESTS_CONCURRENCY_TEST_SUPPORT_HPP
