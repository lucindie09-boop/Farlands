// The terrain-cache version guard. The cache key has a manual data-version
// constant, and a forgotten bump would let a stale base load as if it were
// current. This digest is deliberately COARSE: it hashes per-block-ID counts
// and spill counts, not individual cells. Terrain noise can differ by a cell
// across MSVC/GCC/clang through FMA contraction or libm, which would make a
// cell-by-cell digest red on a platform without any generator change. Counts
// still move when the generator, materials, vegetation or spill rules change.
//
// Three rows are hashed so the guard is not just the surface path: a hills
// surface row (trees and cross-chunk spill), the floor (solid fill and
// bedrock), and an ocean row (a sea level above the macro surface base, so
// the below-sea material and water fill run).
//
// The expected value is checked in, not derived from the current generator, or
// it would pass through every change.
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "worldgen/chunk_generator.hpp"

#include <array>
#include <cstdint>
#include <vector>

using namespace VoxelEngine;

namespace {

using BlockCounts = std::array<uint32_t, BlockRegistry::MAX_BLOCK_TYPES>;

// Set from the first deliberate run; see the failure message. Do not regenerate
// this value to make a red test green without deciding whether the cache key's
// terrain-data version must change.
constexpr uint64_t kExpectedWorldgenDigest = 8771869678933030654ULL;

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

void hash_chunk(ChunkGenerator& gen, int32_t cx, int32_t cy, int32_t cz,
                uint64_t& hash, int64_t& total_spill, BlockCounts& totals) {
    ChunkData chunk;
    std::vector<SpillRecord> spill;
    ChunkGenerator::CrossChunkWriter writer =
        [&spill](int32_t x, int32_t y, int32_t z, BlockID block) {
            spill.push_back({x, y, z, block});
        };
    gen.generate_chunk(chunk, cx, cy, cz, writer, true);

    BlockCounts counts{};
    for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                const size_t id = static_cast<size_t>(chunk.get_block(x, y, z));
                if (id < counts.size()) ++counts[id];
            }
        }
    }
    for (size_t i = 0; i < counts.size(); ++i) {
        mix(hash, counts[i]);
        totals[i] += counts[i];
    }
    mix(hash, static_cast<uint64_t>(spill.size()));
    total_spill += static_cast<int64_t>(spill.size());

    // The generator's emission order is not part of the cached meaning, and a
    // position shift is the part most exposed to floating-point differences.
    // Hash the spill's block-ID counts; the hit/miss parity test owns positions.
    BlockCounts spill_counts{};
    for (const SpillRecord& record : spill) {
        const size_t id = static_cast<size_t>(record.block);
        if (id < spill_counts.size()) ++spill_counts[id];
    }
    for (uint32_t count : spill_counts) mix(hash, count);
    mix(hash, 0x9E3779B97F4A7C15ULL);  // boundary between chunks
}

}  // namespace

TEST_CASE("worldgen digest: regenerated chunks match the cache version guard") {
    BlockRegistry::get_instance().initialize_default_blocks();

    TerrainParams params;
    params.seed = 0x5EED1234u;
    ChunkGenerator gen(params);

    // Guarantee hills and a tree in every hills chunk so the spill assertion is
    // not vacuous. These are generator inputs, not the shipped defaults: the
    // cache key covers the raw biome/vegetation JSON separately, and this test
    // is guarding the generator code's output shape.
    BiomeConfig biome_config;
    biome_config.temp_cold_max = 1.0f;
    biome_config.temp_hot_min = 1.0f;
    gen.set_biome_config(biome_config);
    VegetationConfig vegetation_config;
    vegetation_config.hills.chunk_chance_pct = 100;
    gen.set_vegetation_config(vegetation_config);

    uint64_t hash = 1469598103934665603ULL;
    int64_t total_spill = 0;
    BlockCounts land_counts{};

    // A 5x5 surface grid makes the spill assertion meaningful: a single chunk
    // can be a plain with no canopy, which would make the guard vacuous.
    for (int32_t cz = -2; cz <= 2; ++cz) {
        for (int32_t cx = -2; cx <= 2; ++cx) {
            // The macro surface base is y=512, so cy 16 is the first chunk
            // around the surface; lower chunks are solid and have no vegetation.
            hash_chunk(gen, cx, 16, cz, hash, total_spill, land_counts);
        }
    }
    // The floor is a different path: the column fill below the terrain, with
    // bedrock at the bottom and no vegetation.
    for (int32_t cz = -2; cz <= 2; ++cz) {
        for (int32_t cx = -2; cx <= 2; ++cx) {
            hash_chunk(gen, cx, 0, cz, hash, total_spill, land_counts);
        }
    }

    // A sea level above the macro surface base makes every column ocean, so the
    // below-sea material and the water fill run. The generator emits
    // SURFACE_WATER for the whole body, not just the top layer.
    TerrainParams ocean_params = params;
    ocean_params.sea_level = 600.0f;
    ChunkGenerator ocean_gen(ocean_params);
    ocean_gen.set_biome_config(biome_config);
    ocean_gen.set_vegetation_config(vegetation_config);
    BlockCounts ocean_counts{};
    for (int32_t cz = -2; cz <= 2; ++cz) {
        for (int32_t cx = -2; cx <= 2; ++cx) {
            // cy 18 is y 576-607: below the raised sea level, above the floor.
            hash_chunk(ocean_gen, cx, 18, cz, hash, total_spill, ocean_counts);
        }
    }

    INFO("worldgen digest = " << hash << ", spill records = " << total_spill
         << ", bedrock cells = " << land_counts[BlockIDs::BEDROCK]
         << ", water cells = " << ocean_counts[BlockIDs::SURFACE_WATER]
         << " (bump the terrain-cache data version if this changed deliberately)");
    CHECK(total_spill > 0);
    CHECK(land_counts[BlockIDs::BEDROCK] > 0);
    CHECK(ocean_counts[BlockIDs::SURFACE_WATER] > 0);
    CHECK(hash == kExpectedWorldgenDigest);
}
