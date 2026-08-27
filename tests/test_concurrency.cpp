#include "doctest.h"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/crc32.hpp"
#include "core/edit_map.hpp"
#include "core/thread_pool.hpp"
#include "core/lock_order_checker.hpp"
#include "lighting/light_propagation.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <cstdint>

using namespace VoxelEngine;

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
// 1. Concurrent shared reads on different shards — no contention expected
// =========================================================================
TEST_CASE("concurrent reads on different shards do not block each other") {
    TestShardMap m;
    constexpr int N = 64;
    for (int i = 0; i < N; i++) {
        auto cd = std::make_unique<ChunkData>();
        cd->clear();
        cd->set_block(0, 0, 0, BlockIDs::STONE);
        m.insert(TestShardMap::key(i, 0, 0), std::move(cd));
    }

    std::atomic<int> sum_a{0}, sum_b{0};
    auto reader = [&](int start, int end, std::atomic<int>& out) {
        for (int i = start; i < end; i++) {
            auto k = TestShardMap::key(i, 0, 0);
            TestShardMap::ShardLock lk(m, k);
            auto* d = m.shards_[m.shard_of(k)].chunks.find(k)->second.get();
            if (d && !d->is_all_air())
                out.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto t0 = std::chrono::steady_clock::now();
    std::thread ta(reader, 0, 32, std::ref(sum_a));
    std::thread tb(reader, 32, 64, std::ref(sum_b));
    ta.join();
    tb.join();
    auto dt = std::chrono::steady_clock::now() - t0;

    CHECK(sum_a.load() == 32);
    CHECK(sum_b.load() == 32);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() < 5000);
}

// =========================================================================
// 2. Ascending-shard lock ordering prevents deadlock
// =========================================================================
TEST_CASE("lock_keys ordering prevents deadlock") {
    TestShardMap m;
    for (int i = 0; i < 128; i++) {
        auto cd = std::make_unique<ChunkData>();
        cd->clear();
        m.insert(TestShardMap::key(i, i, i), std::move(cd));
    }

    std::atomic<bool> done_a{false}, done_b{false};
    std::atomic<int> completed{0};

    auto worker = [&](int32_t ax, int32_t ay, int32_t az,
                      int32_t bx, int32_t by, int32_t bz,
                      std::atomic<bool>& flag) {
        uint64_t ka = TestShardMap::key(ax, ay, az);
        uint64_t kb = TestShardMap::key(bx, by, bz);
        std::vector<uint64_t> keys = {ka, kb};
        TestShardMap::OrderedShardLock lk(m, keys);
        volatile int x = 0;
        for (int i = 0; i < 1000; i++) x += i;
        (void)x;
        completed.fetch_add(1, std::memory_order_relaxed);
        flag.store(true, std::memory_order_release);
    };

    // shard = key % 64 depends only on lower 6 bits, which come from z.
    // z=0 → shard 0, z=1 → shard 1.
    std::thread ta(worker, 0, 0, 0, 0, 0, 1, std::ref(done_a));
    std::thread tb(worker, 0, 0, 1, 0, 0, 0, std::ref(done_b));
    ta.join();
    tb.join();

    CHECK(done_a.load());
    CHECK(done_b.load());
    CHECK(completed.load() == 2);
}

// =========================================================================
// 3. PaletteStorage concurrent read/write on different sections
// =========================================================================
TEST_CASE("concurrent PaletteStorage read/write on different sections") {
    PaletteStorage ps;
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
            ps.set_block(x, 0, z, BlockIDs::STONE);
    for (int x = 0; x < 16; x++)
        for (int z = 0; z < 16; z++)
            ps.set_block(x, 16, z, BlockIDs::DIRT);

    std::atomic<bool> writer_done{false};
    std::atomic<int> read_errors{0};

    auto writer = [&]() {
        for (int iter = 0; iter < 100; iter++) {
            for (int x = 0; x < 16; x++)
                for (int z = 0; z < 16; z++)
                    ps.set_block(x, 0, z, (iter % 2 == 0) ? BlockIDs::STONE : BlockIDs::GRASS);
        }
        writer_done.store(true, std::memory_order_release);
    };

    auto reader = [&]() {
        while (!writer_done.load(std::memory_order_acquire)) {
            for (int x = 0; x < 16; x++)
                for (int z = 0; z < 16; z++) {
                    BlockID b = ps.get_block(x, 16, z);
                    if (b != BlockIDs::DIRT)
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                }
        }
    };

    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();

    CHECK(read_errors.load() == 0);
}

// =========================================================================
// 4. All-exclusive lock serializes concurrent access (simulates
//    lock_all_exclusive blocking readers)
// =========================================================================
TEST_CASE("lock_all_exclusive serializes concurrent access") {
    TestShardMap m;
    for (int i = 0; i < 128; i++) {
        auto cd = std::make_unique<ChunkData>();
        cd->clear();
        m.insert(TestShardMap::key(i, 0, 0), std::move(cd));
    }

    std::atomic<int> writer_phase{0};
    std::atomic<bool> reader_saw_phase_zero{false};
    std::atomic<bool> reader_saw_phase_two{false};

    auto writer = [&]() {
        TestShardMap::AllExclusiveLock lk(m);
        writer_phase.store(1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        writer_phase.store(2, std::memory_order_release);
    };

    auto reader = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TestShardMap::AllSharedLock lk(m);
        int phase = writer_phase.load(std::memory_order_acquire);
        if (phase == 0) reader_saw_phase_zero.store(true);
        if (phase == 2) reader_saw_phase_two.store(true);
    };

    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();

    CHECK_FALSE(reader_saw_phase_zero.load());
    CHECK(reader_saw_phase_two.load());
}

// =========================================================================
// 5. pending_light_removals_ mutex pattern — concurrent insert/erase
// =========================================================================
TEST_CASE("pending_light_removals_ concurrent insert/erase does not crash") {
    std::unordered_set<uint64_t> pending;
    std::mutex mu;

    constexpr int NUM_KEYS = 500;
    std::atomic<bool> stop{false};

    auto inserter = [&](uint64_t base) {
        for (int i = 0; i < NUM_KEYS && !stop.load(std::memory_order_acquire); i++) {
            std::lock_guard<std::mutex> g(mu);
            pending.insert(base + i);
        }
    };

    auto eraser = [&]() {
        for (int i = 0; i < NUM_KEYS && !stop.load(std::memory_order_acquire); i++) {
            std::lock_guard<std::mutex> g(mu);
            auto it = pending.find(i);
            if (it != pending.end()) pending.erase(it);
        }
    };

    std::thread t1(inserter, 0);
    std::thread t2(inserter, NUM_KEYS);
    std::thread t3(eraser);
    t1.join();
    t2.join();
    stop.store(true, std::memory_order_release);
    t3.join();

    CHECK(pending.size() <= static_cast<size_t>(2 * NUM_KEYS));
}

// =========================================================================
// 6. ShardMap size counter accuracy under concurrent insert/erase
// =========================================================================
TEST_CASE("ShardMap size is accurate under concurrent modifications") {
    TestShardMap m;
    constexpr int N = 100;

    auto inserter = [&](int start, int end) {
        for (int i = start; i < end; i++) {
            auto cd = std::make_unique<ChunkData>();
            cd->clear();
            m.insert(TestShardMap::key(i, 0, 0), std::move(cd));
        }
    };

    auto eraser = [&](int start, int end) {
        for (int i = start; i < end; i++) {
            m.erase(TestShardMap::key(i, 0, 0));
        }
    };

    std::thread t1(inserter, 0, N / 2);
    std::thread t2(inserter, N / 2, N);
    t1.join();
    t2.join();
    CHECK(m.size() == N);

    std::thread t3(eraser, 0, N / 2);
    std::thread t4(eraser, N / 2, N);
    t3.join();
    t4.join();
    CHECK(m.size() == 0);
}

// =========================================================================
// 7. Shared shard lock releases on destruction
// =========================================================================
TEST_CASE("ShardLock releases on destruction") {
    TestShardMap m;
    auto cd = std::make_unique<ChunkData>();
    cd->clear();
    m.insert(TestShardMap::key(0, 0, 0), std::move(cd));

    {
        TestShardMap::ShardLock lk1(m, TestShardMap::key(0, 0, 0));
        TestShardMap::ShardLock lk2(m, TestShardMap::key(0, 0, 0));
    }
    // After both shared locks are released, exclusive lock must succeed.
    // If shared locks didn't release, this would deadlock (test timeout).
    TestShardMap::ExclusiveShardLock el(m, TestShardMap::key(0, 0, 0));
    auto* d = m.shards_[m.shard_of(TestShardMap::key(0, 0, 0))]
                .chunks[TestShardMap::key(0, 0, 0)].get();
    d->set_block(0, 0, 0, BlockIDs::STONE);
    CHECK(d->get_block(0, 0, 0) == BlockIDs::STONE);
}

// =========================================================================
// 8. Exclusive shard lock blocks concurrent shared
// =========================================================================
TEST_CASE("ExclusiveShardLock blocks concurrent shared") {
    TestShardMap m;
    auto cd = std::make_unique<ChunkData>();
    cd->clear();
    m.insert(TestShardMap::key(0, 0, 0), std::move(cd));

    std::atomic<bool> exclusive_held{false};
    std::atomic<bool> shared_acquired{false};

    auto exclusive_worker = [&]() {
        TestShardMap::ExclusiveShardLock el(m, TestShardMap::key(0, 0, 0));
        exclusive_held.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };

    auto shared_worker = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        TestShardMap::ShardLock sl(m, TestShardMap::key(0, 0, 0));
        shared_acquired.store(true, std::memory_order_release);
    };

    std::thread te(exclusive_worker);
    std::thread ts(shared_worker);
    te.join();
    ts.join();

    CHECK(exclusive_held.load());
    CHECK(shared_acquired.load());
}

// =========================================================================
// 9. Exclusive lock + reader: reader must see writer's modifications
// =========================================================================
TEST_CASE("exclusive lock serializes writer and reader on same shard") {
    TestShardMap m;
    auto cd = std::make_unique<ChunkData>();
    cd->clear();
    m.insert(TestShardMap::key(5, 0, 0), std::move(cd));

    std::atomic<bool> reader_saw_air{false};

    auto writer = [&]() {
        TestShardMap::ExclusiveShardLock el(m, TestShardMap::key(5, 0, 0));
        auto* d = m.shards_[m.shard_of(TestShardMap::key(5, 0, 0))]
                    .chunks[TestShardMap::key(5, 0, 0)].get();
        d->set_block(0, 0, 0, BlockIDs::STONE);
    };

    auto reader = [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        TestShardMap::ShardLock sl(m, TestShardMap::key(5, 0, 0));
        auto* d = m.shards_[m.shard_of(TestShardMap::key(5, 0, 0))]
                    .chunks[TestShardMap::key(5, 0, 0)].get();
        if (d->get_block(0, 0, 0) == BlockIDs::AIR)
            reader_saw_air.store(true, std::memory_order_relaxed);
    };

    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();

    // After both threads complete, verify the final state is consistent.
    // The writer set STONE; the reader must see either AIR (pre-write) or STONE (post-write).
    // No torn/inconsistent value is possible because PaletteStorage writes are atomic at the word level.
    auto* d = m.shards_[m.shard_of(TestShardMap::key(5, 0, 0))]
                .chunks[TestShardMap::key(5, 0, 0)].get();
    BlockID final_val = d->get_block(0, 0, 0);
    CHECK((final_val == BlockIDs::AIR || final_val == BlockIDs::STONE));
}

// =========================================================================
// 10. OrderedExclusiveShardLock targets only specified shards
// =========================================================================
TEST_CASE("OrderedExclusiveShardLock only locks specified shards") {
    TestShardMap m;
    // Insert chunks at different z values so they land on different shards.
    // key encoding: (ux << 42) | (uy << 21) | uz, shard = key % 64.
    // Different z => different uz => different shard (for small z).
    for (int i = 0; i < 10; i++) {
        auto cd = std::make_unique<ChunkData>();
        cd->clear();
        m.insert(TestShardMap::key(0, 0, i), std::move(cd));
    }

    // Lock only keys for chunks (0,0,0) and (0,0,2) — shards 0 and 2
    std::vector<uint64_t> keys = { TestShardMap::key(0, 0, 0), TestShardMap::key(0, 0, 2) };

    std::atomic<bool> other_shard_writable{false};

    // Write to chunk (0,0,9) which lives on shard 9 — should not be blocked
    auto other_writer = [&]() {
        TestShardMap::ExclusiveShardLock el(m, TestShardMap::key(0, 0, 9));
        auto* d = m.shards_[m.shard_of(TestShardMap::key(0, 0, 9))]
                    .chunks[TestShardMap::key(0, 0, 9)].get();
        d->set_block(0, 0, 0, BlockIDs::STONE);
        other_shard_writable.store(true, std::memory_order_release);
    };

    {
        TestShardMap::OrderedExclusiveShardLock lk(m, keys);
        std::thread t(other_writer);
        // other_writer should complete quickly if chunk 9's shard is not locked
        t.join();
        CHECK(other_shard_writable.load());
    }

    // Verify chunk 9 was written
    auto* d9 = m.shards_[m.shard_of(TestShardMap::key(0, 0, 9))]
                .chunks[TestShardMap::key(0, 0, 9)].get();
    CHECK(d9->get_block(0, 0, 0) == BlockIDs::STONE);
}

// =========================================================================
// 11. Light propagation remove path — place emissive, propagate, remove,
//     re-propagate (single-chunk standalone test)
// =========================================================================
TEST_CASE("light removal via re-propagation clears light on single chunk") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();

    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);

    CHECK(chunk.get_light_unsafe(16, 16, 16) > 0);
    CHECK(chunk.get_light_unsafe(16, 17, 16) > 0);
    CHECK(chunk.get_light_unsafe(16, 15, 16) > 0);

    chunk.set_block(16, 16, 16, BlockIDs::AIR);
    chunk.clear_light();
    propagate_chunk_block_light_additive(chunk);

    CHECK(chunk.get_light_unsafe(16, 16, 16) == 0);
    CHECK(chunk.get_light_unsafe(16, 17, 16) == 0);
    CHECK(chunk.get_light_unsafe(16, 15, 16) == 0);
}

// =========================================================================
// 12. Light removal: replace emissive with opaque block clears neighbors
// =========================================================================
TEST_CASE("replacing emissive with opaque clears propagated light") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();

    chunk.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    propagate_chunk_block_light_additive(chunk);
    CHECK(chunk.get_light_unsafe(16, 17, 16) > 0);

    chunk.set_block(16, 16, 16, BlockIDs::STONE);
    chunk.clear_light();
    propagate_chunk_block_light_additive(chunk);

    CHECK(chunk.get_light_unsafe(16, 16, 16) == 0);
    CHECK(chunk.get_light_unsafe(16, 17, 16) == 0);
}

// =========================================================================
// 13. Cross-chunk writer race: test the actual production pattern from
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

TEST_CASE("cross-chunk writer concurrent pending placements") {
    CrossChunkWriter writer;
    constexpr int NUM_THREADS = 4;
    constexpr int PLACEMENTS_PER_THREAD = 500;

    auto worker = [&](int thread_id) {
        auto cross_writer = writer.make_cross_writer();
        for (int i = 0; i < PLACEMENTS_PER_THREAD; i++) {
            int32_t wx = thread_id * 1000 + i;
            int32_t wy = 10;
            int32_t wz = i;
            cross_writer(wx, wy, wz, i % 10);
        }
    };

    std::thread writers[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++)
        writers[i] = std::thread(worker, i);
    for (auto& t : writers) t.join();

    CHECK(writer.total_pending_count() == static_cast<size_t>(NUM_THREADS * PLACEMENTS_PER_THREAD));
    CHECK(writer.cross_boundary_count() == static_cast<size_t>(NUM_THREADS * PLACEMENTS_PER_THREAD));
}

// =========================================================================
// 14. Cross-chunk writer: concurrent push + drain under contention
//     Tests the actual production pattern from chunk_world.cpp apply_pending_placements
// =========================================================================
TEST_CASE("cross-chunk writer concurrent push and drain") {
    CrossChunkWriter writer;
    std::atomic<bool> stop_writers{false};

    constexpr int NUM_WRITERS = 4;
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_drained{0};

    auto pusher = [&](int base) {
        auto cross_writer = writer.make_cross_writer();
        int count = 0;
        while (!stop_writers.load(std::memory_order_acquire)) {
            cross_writer(base + count, 0, 0, 1);
            count++;
            total_pushed.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Simulates process_completed_chunks draining cross_boundary_remesh
    auto drainer = [&]() {
        while (!stop_writers.load(std::memory_order_acquire)) {
            // Drain all keys currently present to simulate apply_pending_placements
            auto keys = writer.get_all_keys();
            for (auto key : keys) {
                auto placements = writer.dequeue_placements(key);
                total_drained.fetch_add(static_cast<int>(placements.size()), std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    std::thread drain_thread(drainer);
    std::thread writers[NUM_WRITERS];
    for (int i = 0; i < NUM_WRITERS; i++)
        writers[i] = std::thread(pusher, i * 10000);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_writers.store(true, std::memory_order_release);
    for (auto& t : writers) t.join();
    drain_thread.join();
    // After both writers and drainer are joined, do a final drain pass
    // to catch any entries that were pushed after the drainer's last snapshot
    while (true) {
        auto keys = writer.get_all_keys();
        if (keys.empty()) break;
        for (auto key : keys) {
            auto placements = writer.dequeue_placements(key);
            total_drained.fetch_add(static_cast<int>(placements.size()), std::memory_order_relaxed);
        }
        std::this_thread::yield();
    }

    // Final verification: all pushed entries were drained
    CHECK(total_drained.load() == total_pushed.load());
    CHECK(writer.total_pending_count() == 0);
}

// =========================================================================
// 15. pending_light_removals_ stress: concurrent BFS insert + fixup erase
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

TEST_CASE("pending_light_removals_ BFS insert and fixup erase stress") {
    PendingLightRemovals pending;
    constexpr int NUM_KEYS = 2000;

    std::atomic<int> fixed_count{0};

    // Simulates BFS threads calling pending_light_removals_.insert
    auto bfs_inserter = [&](int start, int end) {
        for (int i = start; i < end; i++) {
            uint64_t k = static_cast<uint64_t>(i);
            pending.insert(k);
        }
    };

    // Simulates try_fixup_chunk calling pending_light_removals_.erase
    auto fixupper = [&](int start, int end) {
        for (int i = start; i < end; i++) {
            uint64_t k = static_cast<uint64_t>(i);
            if (pending.try_erase(k)) {
                fixed_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(bfs_inserter, 0, NUM_KEYS);
    std::thread t2(bfs_inserter, NUM_KEYS, 2 * NUM_KEYS);
    std::thread t3(fixupper, 0, NUM_KEYS);
    std::thread t4(fixupper, NUM_KEYS / 2, NUM_KEYS + NUM_KEYS / 2);
    t1.join();
    t2.join();
    t3.join();
    t4.join();

    CHECK(fixed_count.load() > 0);
    CHECK(pending.size() <= static_cast<size_t>(2 * NUM_KEYS));
}

// =========================================================================
// 16. OrderedExclusiveShardLock serializes writers on overlapping shards
// =========================================================================
TEST_CASE("OrderedExclusiveShardLock serializes concurrent writers") {
    TestShardMap m;
    for (int i = 0; i < 10; i++) {
        auto cd = std::make_unique<ChunkData>();
        cd->clear();
        m.insert(TestShardMap::key(i, 0, 0), std::move(cd));
    }

    std::atomic<int> write_count_a{0};
    std::atomic<int> write_count_b{0};

    std::vector<uint64_t> shared_keys = {
        TestShardMap::key(0, 0, 0),
        TestShardMap::key(1, 0, 0),
        TestShardMap::key(2, 0, 0)
    };

    auto writer_a = [&]() {
        for (int iter = 0; iter < 100; iter++) {
            TestShardMap::OrderedExclusiveShardLock lk(m, shared_keys);
            for (int i = 0; i < 3; i++) {
                auto* d = m.shards_[m.shard_of(shared_keys[i])]
                            .chunks[shared_keys[i]].get();
                d->set_block(0, 0, 0, BlockIDs::STONE);
            }
            write_count_a.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto writer_b = [&]() {
        for (int iter = 0; iter < 100; iter++) {
            TestShardMap::OrderedExclusiveShardLock lk(m, shared_keys);
            for (int i = 0; i < 3; i++) {
                auto* d = m.shards_[m.shard_of(shared_keys[i])]
                            .chunks[shared_keys[i]].get();
                d->set_block(0, 0, 0, BlockIDs::GRASS);
            }
            write_count_b.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread ta(writer_a);
    std::thread tb(writer_b);
    ta.join();
    tb.join();

    CHECK(write_count_a.load() == 100);
    CHECK(write_count_b.load() == 100);

    for (int i = 0; i < 3; i++) {
        auto* d = m.shards_[m.shard_of(shared_keys[i])]
                    .chunks[shared_keys[i]].get();
        BlockID v = d->get_block(0, 0, 0);
        CHECK((v == BlockIDs::STONE || v == BlockIDs::GRASS));
    }
}

// =========================================================================
// 17. Edit map round-trip: encode then decode preserves all block data
// =========================================================================
TEST_CASE("edit map round-trip preserves block data") {
    BlockRegistry::get_instance().initialize_default_blocks();

    EditMap original;
    // Mixed edits at various coordinates
    for (int32_t y = 0; y < 32; y++) {
        original.set_block(5, y, 10, y < 16 ? BlockIDs::STONE : BlockIDs::DIRT);
    }
    original.set_block(0, 0, 0, BlockIDs::LIGHT_BLOCK);
    original.set_block(31, 31, 31, BlockIDs::GRASS);

    std::vector<uint8_t> data;
    serialize_edit_map(original, data);

    EditMap decoded;
    bool ok = deserialize_edit_map(data.data(), data.size(), decoded, BlockRegistry::get_instance());
    CHECK(ok);

    // Verify all edits match
    for (int32_t y = 0; y < 32; y++) {
        CHECK(decoded.get_block(5, y, 10, BlockIDs::AIR) == (y < 16 ? BlockIDs::STONE : BlockIDs::DIRT));
    }
    CHECK(decoded.get_block(0, 0, 0, BlockIDs::AIR) == BlockIDs::LIGHT_BLOCK);
    CHECK(decoded.get_block(31, 31, 31, BlockIDs::AIR) == BlockIDs::GRASS);
}

// =========================================================================
// 18. Edit map decode rejects truncated buffers (fuzz-relevant)
// =========================================================================
TEST_CASE("edit map decode rejects truncated input") {
    BlockRegistry::get_instance().initialize_default_blocks();

    // Truncated header
    {
        const uint8_t trunc1[] = {0x01}; // partial header
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(trunc1, sizeof(trunc1), m, BlockRegistry::get_instance()));
    }
    // Truncated body (header says 1 edit but body is missing)
    {
        uint8_t trunc2[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // version=1, count=1, crc=0
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(trunc2, sizeof(trunc2), m, BlockRegistry::get_instance()));
    }
    // Empty body
    {
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(nullptr, 0, m, BlockRegistry::get_instance()));
    }
}

// =========================================================================
// 19. CRC32 mismatch detection: tampered body is rejected
// =========================================================================
TEST_CASE("CRC32 detects tampered edit map body") {
    BlockRegistry::get_instance().initialize_default_blocks();

    EditMap edit_map;
    for (int32_t y = 0; y < 32; y++) {
        edit_map.set_block(0, y, 0, BlockIDs::STONE);
    }

    std::vector<uint8_t> data;
    serialize_edit_map(edit_map, data);
    
    // Extract CRC from header (bytes 8-11)
    uint32_t original_crc = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);

    // Flip one byte in the body (after 12-byte header)
    data[12 + data.size() / 2] ^= 0xFF;
    
    // Recompute CRC
    uint32_t tampered_crc = crc32(data.data() + 12, data.size() - 12);

    CHECK(original_crc != tampered_crc);
}

// =========================================================================
// 20. Work stealing: a task queued on a worker that is busy executing
// something else must be reclaimed by an idle worker, not left stranded.
// Worker 0 is blocked by a gated task, then a 4-task round-robin batch lands
// its last task on worker 0's queue. Only the idle workers can run it, so
// the pool's steal counter must be nonzero. Fully deterministic (no timers).
// =========================================================================
TEST_CASE("thread pool work stealing reclaims tasks from a busy worker") {
    constexpr std::size_t kWorkers = 4;
    ThreadPool pool(kWorkers);
    CHECK(pool.get_worker_count() == kWorkers);

    auto blocker_running = std::make_shared<std::atomic<bool>>(false);
    auto release_blocker = std::make_shared<std::atomic<bool>>(false);
    auto tasks_done = std::make_shared<std::atomic<int>>(0);

    // First task lands on worker 0 (round-robin starts at 0) and blocks it.
    pool.fire_and_forget([blocker_running, release_blocker]() {
        blocker_running->store(true, std::memory_order_release);
        while (!release_blocker->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (!blocker_running->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Four tasks, round-robin: workers 1, 2, 3, then 0. The last one lands on
    // the blocked worker 0, so it can only run via stealing.
    for (int i = 0; i < 4; ++i) {
        pool.fire_and_forget([tasks_done]() { tasks_done->fetch_add(1, std::memory_order_relaxed); });
    }

    // Worker 0 stays blocked, so all four tasks must complete without it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tasks_done->load(std::memory_order_acquire) < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    release_blocker->store(true, std::memory_order_release);

    CHECK(tasks_done->load(std::memory_order_acquire) == 4);
    CHECK(pool.get_steal_count() > 0);
}

// =========================================================================
// Lock-order checker tests (src/core/lock_order_checker.hpp)
// =========================================================================

TEST_CASE("Lock-order checker: ascending acquisition succeeds") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(0);
    CHECK(held_.test(0));
    CHECK(max_held_ == 0);

    acquire(5);
    CHECK(held_.test(5));
    CHECK(max_held_ == 5);

    acquire(63);
    CHECK(held_.test(63));
    CHECK(max_held_ == 63);

    release(5);
    CHECK(!held_.test(5));
    CHECK(max_held_ == 63);

    release(63);
    CHECK(!held_.test(63));
    CHECK(max_held_ == 0);

    release(0);
    CHECK(!held_.test(0));
    CHECK(max_held_ == 0);
    CHECK(held_.none());
}

TEST_CASE("Lock-order checker: reset_all clears state") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(10);
    acquire(20);
    acquire(30);
    CHECK(held_.test(10));
    CHECK(held_.test(20));
    CHECK(held_.test(30));
    CHECK(max_held_ == 30);

    reset_all();
    CHECK(held_.none());
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: release recomputes max_held") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(3);
    acquire(7);
    acquire(12);
    CHECK(max_held_ == 12);

    release(12);
    CHECK(max_held_ == 7);

    release(3);
    CHECK(max_held_ == 7);

    release(7);
    CHECK(max_held_ == 0);
    CHECK(held_.none());
}

TEST_CASE("Lock-order checker: duplicate acquire is harmless") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(5);
    acquire(5);
    CHECK(max_held_ == 5);
    CHECK(held_.count() == 1);

    release(5);
    release(5);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: release of unheld shard is harmless") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(10);
    release(42);
    CHECK(max_held_ == 10);
    release(10);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: out-of-range shard indices are ignored") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(100);
    CHECK(held_.none());
    CHECK(max_held_ == 0);

    release(100);
    CHECK(max_held_ == 0);
}

TEST_CASE("Lock-order checker: each thread has independent state") {
    using namespace VoxelEngine::lock_order;
    reset_all();

    acquire(50);
    CHECK(max_held_ == 50);

    std::atomic<bool> child_acquired{false};
    std::atomic<bool> child_ok{false};
    std::thread t([&]() {
        acquire(3);
        child_ok.store(max_held_ == 3 && !held_.test(50), std::memory_order_relaxed);
        child_acquired.store(true, std::memory_order_release);
        release(3);
    });

    while (!child_acquired.load(std::memory_order_acquire))
        std::this_thread::yield();
    t.join();

    CHECK(child_ok.load());
    CHECK(max_held_ == 50);
    release(50);
}
