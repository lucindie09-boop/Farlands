#include "doctest.h"
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
