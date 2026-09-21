#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "core/chunk_map.hpp"

using namespace VoxelEngine;

TEST_CASE("key encode/decode round-trip (0,0,0)") {
    ChunkMap cm;
    uint64_t key = cm.get_chunk_key(0, 0, 0);
    int32_t x, y, z;
    ChunkMap::decode_chunk_key(key, x, y, z);
    CHECK(x == 0);
    CHECK(y == 0);
    CHECK(z == 0);
}

TEST_CASE("key encode/decode round-trip (1,-1,2)") {
    ChunkMap cm;
    uint64_t key = cm.get_chunk_key(1, -1, 2);
    int32_t x, y, z;
    ChunkMap::decode_chunk_key(key, x, y, z);
    CHECK(x == 1);
    CHECK(y == -1);
    CHECK(z == 2);
}

TEST_CASE("key encode/decode round-trip (100000,-50000,0)") {
    ChunkMap cm;
    uint64_t key = cm.get_chunk_key(100000, -50000, 0);
    int32_t x, y, z;
    ChunkMap::decode_chunk_key(key, x, y, z);
    CHECK(x == 100000);
    CHECK(y == -50000);
    CHECK(z == 0);
}

TEST_CASE("key uniqueness") {
    ChunkMap cm;
    uint64_t k1 = cm.get_chunk_key(1, 2, 3);
    uint64_t k2 = cm.get_chunk_key(1, 2, 4);
    CHECK(k1 != k2);
}

TEST_CASE("key symmetry (x vs z different axis)") {
    ChunkMap cm;
    uint64_t k_xz = cm.get_chunk_key(5, 0, 7);
    uint64_t k_zx = cm.get_chunk_key(7, 0, 5);
    CHECK(k_xz != k_zx);
}

// A lock's extent is the whole point of lock_keys, and the two forms differ: the
// array form locks every element of the array it is handed, the count form locks
// only what the caller filled. The fluid window fills an 8-slot array with as few
// keys as the window touches and must use the count form — the array form would
// read the untouched slots and take shards nothing asked for.
TEST_CASE("lock_keys locks exactly the keys it is given") {
    ChunkMap cm;
    // The shard is the low bits of the key, which come from z: (0,0,0) and
    // (0,0,1) are one shard apart by construction.
    const uint64_t first = cm.get_chunk_key(0, 0, 0);
    const uint64_t second = cm.get_chunk_key(0, 0, 1);
    CHECK(cm.shard_of(first) != cm.shard_of(second));

    const uint64_t keys[4] = {first, second, second, second};
    CHECK(cm.lock_keys(keys).shard_count() == 2);
    CHECK(cm.lock_keys(keys, 1).shard_count() == 1);
    CHECK(cm.lock_keys(keys, 2).shard_count() == 2);
    CHECK(cm.lock_keys(keys, 0).shard_count() == 0);
}

TEST_CASE("a held band reads a neighbouring chunk through the fast accessor") {
    // The paste's fluid-wake predicate reads a written cell's face neighbours while
    // holding the 3x3x3 exclusive band, and a face neighbour can be one chunk away.
    // It must use the _fast accessor there: the locking one takes a shared lock on a
    // shard this thread already owns exclusively, which std::shared_mutex answers by
    // blocking forever (a debug build now reports it instead of hanging). This pins
    // the accessor that is legal under the lock, and that it answers the same as the
    // locking one does outside it.
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    chunktest::insert_chunk(cm, 1, 0, 0, [](ChunkData& data) {
        data.set_block(0, 0, 0, static_cast<BlockID>(BlockIDs::STONE));
    });

    // Exactly the band the paste locks: the written chunk and its 26 neighbours.
    uint64_t keys[27];
    int idx = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                keys[idx++] = cm.get_chunk_key(dx, dy, dz);
    {
        auto band = cm.lock_keys_exclusive(keys);
        // One block into the neighbouring chunk: local (0,0,0) of chunk (1,0,0).
        CHECK(cm.get_block_world_fast(CHUNK_WIDTH, 0, 0) == static_cast<int>(BlockIDs::STONE));
    }

    // With the band gone, the locking accessor agrees — so the fast path is the same
    // read, not a weaker one.
    CHECK(cm.get_block_world(CHUNK_WIDTH, 0, 0) == static_cast<int>(BlockIDs::STONE));
}

TEST_CASE("every chunk of a column shares one shard") {
    // This is load-bearing, not incidental: the generation sweep answers "is this
    // whole column built?" for the count(band) slices of one column under a SINGLE
    // ChunkMap::lock_column acquisition and probes the rest with the lock-free
    // `_fast` accessors. That is only correct while every (cx, cy, cz) with the
    // same (cx, cz) resolves to one shard, so it is pinned here rather than
    // assumed at the one call site that depends on it.
    ChunkMap cm;
    const int32_t ys[] = {-1000000, -33, -1, 0, 1, 31, 32, 33, 1024, 1000000};
    const int32_t columns[][2] = {
        {0, 0}, {1, 0}, {0, 1}, {-1, -1}, {1234, -5678}, {-1000000, 1000000}, {999983, 999979},
    };
    for (const auto& col : columns) {
        const size_t column_shard = cm.shard_of_column(col[0], col[1]);
        for (int32_t cy : ys) {
            CHECK(cm.shard_of(cm.get_chunk_key(col[0], cy, col[1])) == column_shard);
        }
    }
    // Distinct columns are NOT forced apart — the hash decides, exactly as it did
    // per chunk before. Nothing may start depending on neighbouring columns
    // landing on different shards.
    CHECK(cm.shard_of_column(0, 0) == cm.shard_of(cm.get_chunk_key(0, 0, 0)));
}

TEST_CASE("column shards still spread across the shard array") {
    // Masking y out reduces the hash's input space; it must not collapse it. Every
    // shard has to be reachable and none may take a large share, or the sweep's one
    // acquisition per column would land on a hot shard for whole regions of the map.
    ChunkMap cm;
    size_t counts[ChunkMap::kNumShards] = {};
    constexpr int32_t kSpan = 64;
    size_t total = 0;
    for (int32_t cx = 0; cx < kSpan; ++cx) {
        for (int32_t cz = 0; cz < kSpan; ++cz) {
            ++counts[cm.shard_of_column(cx, cz)];
            ++total;
        }
    }
    const size_t expected = total / ChunkMap::kNumShards;
    for (size_t s = 0; s < ChunkMap::kNumShards; ++s) {
        CHECK(counts[s] > 0);
        CHECK(counts[s] <= expected * 3);
    }
}

TEST_CASE("a column band read under one lock agrees with the locking accessor") {
    // The sweep replaces count(band) locked `contains` calls with one lock_column
    // plus lock-free probes. Prove the batched form answers what the locking one
    // answers, for a resident slice and for a missing one.
    ChunkMap cm;
    chunktest::insert_floor_chunk(cm, 5, 3, -7);
    chunktest::insert_floor_chunk(cm, 5, 4, -7);
    {
        auto column_lock = cm.lock_column(5, -7);
        CHECK(cm.contains_fast(cm.get_chunk_key(5, 3, -7)));
        CHECK(cm.contains_fast(cm.get_chunk_key(5, 4, -7)));
        CHECK_FALSE(cm.contains_fast(cm.get_chunk_key(5, 5, -7)));
        CHECK_FALSE(cm.contains_fast(cm.get_chunk_key(6, 3, -7)));
    }
    CHECK(cm.contains(cm.get_chunk_key(5, 3, -7)));
    CHECK(cm.contains(cm.get_chunk_key(5, 4, -7)));
    CHECK_FALSE(cm.contains(cm.get_chunk_key(5, 5, -7)));
}

TEST_CASE("key encode/decode near max range") {
    ChunkMap cm;
    int32_t max_val = 1000000;
    uint64_t key = cm.get_chunk_key(max_val, -max_val, max_val);
    int32_t x, y, z;
    ChunkMap::decode_chunk_key(key, x, y, z);
    CHECK(x == max_val);
    CHECK(y == -max_val);
    CHECK(z == max_val);
}
