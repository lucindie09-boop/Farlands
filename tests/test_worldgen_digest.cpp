// The terrain-cache version guard. The cache key has a manual data-version
// constant, and a forgotten bump would let a stale base load as if it were
// current. This digest covers a few fixed chunks' generated blocks AND the
// cross-chunk spill writes their generation emits; a change to either is a
// deliberate change to the cached base, so it must also bump the cache's
// terrain-data version.
//
// Keep this test small and deterministic. The expected value is checked in, not
// derived from the current generator, or it would pass through every change.
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace VoxelEngine;

namespace {

// Set from the first deliberate run; see the failure message. Do not regenerate
// this value to make a red test green without deciding whether the cache key's
// terrain-data version must change.
constexpr uint64_t kExpectedWorldgenDigest = 5205118212672258947ULL;

struct SpillRecord {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    BlockID block = 0;
};

void mix(uint64_t& hash, uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
}

uint64_t hash_chunk(ChunkGenerator& gen, int32_t cx, int32_t cy, int32_t cz,
                    uint64_t& hash) {
    ChunkData chunk;
    std::vector<SpillRecord> spill;
    ChunkGenerator::CrossChunkWriter writer =
        [&spill](int32_t x, int32_t y, int32_t z, BlockID block) {
            spill.push_back({x, y, z, block});
        };
    gen.generate_chunk(chunk, cx, cy, cz, writer, true);

    for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                mix(hash, static_cast<uint64_t>(chunk.get_block(x, y, z)));
            }
        }
    }

    // The generator's emission order is not part of the cached meaning; sort so
    // a change that only reorders equivalent writes does not fail the guard.
    std::sort(spill.begin(), spill.end(), [](const SpillRecord& a, const SpillRecord& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        return a.block < b.block;
    });
    for (const SpillRecord& record : spill) {
        mix(hash, static_cast<uint64_t>(record.x));
        mix(hash, static_cast<uint64_t>(record.y));
        mix(hash, static_cast<uint64_t>(record.z));
        mix(hash, static_cast<uint64_t>(record.block));
    }
    return hash;
}

}  // namespace

TEST_CASE("worldgen digest: regenerated chunks match the cache version guard") {
    BlockRegistry::get_instance().initialize_default_blocks();

    TerrainParams params;
    params.seed = 0x5EED1234u;
    ChunkGenerator gen(params);

    uint64_t hash = 1469598103934665603ULL;
    hash_chunk(gen, 0, 2, 0, hash);
    hash_chunk(gen, 3, 2, -2, hash);
    hash_chunk(gen, -2, 0, 4, hash);

    INFO("worldgen digest = " << hash
         << " (bump the terrain-cache data version if this changed deliberately)");
    CHECK(hash == kExpectedWorldgenDigest);
}
