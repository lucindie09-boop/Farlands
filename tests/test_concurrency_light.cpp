#include "doctest.h"

#include "concurrency_test_support.hpp"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "lighting/light_propagation.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace VoxelEngine;
using namespace concurrency_test;

// =========================================================================
// Light propagation remove path — place emissive, propagate, remove,
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
// Light removal: replace emissive with opaque block clears neighbors
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
