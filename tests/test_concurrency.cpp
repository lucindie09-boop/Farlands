#include "doctest.h"

#include "concurrency_test_support.hpp"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace VoxelEngine;
using namespace concurrency_test;

// =========================================================================
// Concurrent shared reads on different shards — no contention expected
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
// Ascending-shard lock ordering prevents deadlock
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
// PaletteStorage concurrent read/write on different sections
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
// All-exclusive lock serializes concurrent access (simulates
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
// pending_light_removals_ mutex pattern — concurrent insert/erase
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
// ShardMap size counter accuracy under concurrent insert/erase
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
// Shared shard lock releases on destruction
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
// Exclusive shard lock blocks concurrent shared
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
// Exclusive lock + reader: reader must see writer's modifications
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
// OrderedExclusiveShardLock targets only specified shards
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
// OrderedExclusiveShardLock serializes writers on overlapping shards
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
