// Regression tests for the "random chunk borders underground" artifact.
//
// The generation worker has fast paths for uniform chunks: all air, all
// bedrock, and all solid subsurface. The two SOLID fast paths previously never
// called compute_fully_solid(), so every underground chunk reported
// fully_solid() == false. That disabled the buried-chunk mesh culling, and
// every underground chunk got meshed as a full 6-faced box — including ones
// fully buried in stone ("random chunk borders generate and stay").
//
// The other half of the fix: chunk_would_be_fully_solid() lets mesh culling
// treat an ungenerated underground neighbor as opaque instead of rendering a
// box wall into the void.

#include "doctest.h"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include "core/terrain_params.hpp"
#include "core/chunk_coords.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include "worldgen/chunk_generator.hpp"
#include <cmath>

using namespace VoxelEngine;

namespace {

// Bedrock raised so a whole in-world chunk can be entirely bedrock; sea level
// lowered so the cave band [bedrock+3, sea_level+10] is empty — with cave
// carving disabled anyway, that keeps the solid fast path unambiguous for any
// chunk entirely below the surface.
TerrainParams make_params() {
    TerrainParams p;
    p.bedrock_height = 96;
    p.sea_level = 50.0f;
    return p;
}

// Finds a column whose surface band sits comfortably mid-world so both the
// below-surface and above-surface assertions are in-world.
struct MidColumn {
    int32_t cx = 0;
    int32_t cz = 0;
    ChunkGenerator::HeightRange range;
    bool found = false;
};

MidColumn find_mid_column(const ChunkGenerator& gen) {
    MidColumn out;
    for (int32_t cx = -60; cx <= 60 && !out.found; cx += 4) {
        for (int32_t cz = -60; cz <= 60 && !out.found; cz += 4) {
            auto r = gen.get_chunk_height_range(cx, cz);
            if (r.min_h > 256.0f && r.max_h < 960.0f) {
                out.cx = cx;
                out.cz = cz;
                out.range = r;
                out.found = true;
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("solid fast paths mark underground chunks fully solid") {
    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainParams params = make_params();
    ChunkGenerator gen(params);

    // Whole chunk below the bedrock layer → all-bedrock fast path → solid.
    ChunkData bedrock;
    CHECK(gen.generate_fast_path(bedrock, 0, 1, 0));
    CHECK(bedrock.fully_solid());
    CHECK_FALSE(bedrock.is_all_air());

    // Deep underground chunk (entirely below the column's lowest possible
    // surface, above bedrock) → solid-subsurface fast path → must be marked
    // fully solid so buried-chunk mesh culling skips it.
    MidColumn mid = find_mid_column(gen);
    CHECK(mid.found);
    if (!mid.found) return;
    // Chunk whose top sits at least 3 blocks below the lowest possible
    // surface, and whose bottom sits above the bedrock layer.
    const int32_t fast_cy = static_cast<int32_t>(std::ceil((mid.range.min_h - 3.0f) / CHUNK_HEIGHT)) - 2;
    CHECK(fast_cy * CHUNK_HEIGHT > params.bedrock_height);
    if (fast_cy * CHUNK_HEIGHT <= params.bedrock_height) return;

    ChunkData solid;
    CHECK(gen.generate_fast_path(solid, mid.cx, fast_cy, mid.cz));
    CHECK(solid.fully_solid());
    CHECK_FALSE(solid.is_all_air());

    // High above any terrain → all-air fast path → NOT fully solid.
    ChunkData air;
    const int32_t high_cy = static_cast<int32_t>(std::ceil((mid.range.max_h + 1.0f) / CHUNK_HEIGHT));
    CHECK(high_cy * CHUNK_HEIGHT < WORLD_HEIGHT_Y);
    if (high_cy * CHUNK_HEIGHT >= WORLD_HEIGHT_Y) return;
    CHECK(gen.generate_fast_path(air, mid.cx, high_cy, mid.cz));
    CHECK(air.is_all_air());
    CHECK_FALSE(air.fully_solid());

    // Out-of-world → handled, cleared, never solid.
    ChunkData out;
    CHECK(gen.generate_fast_path(out, mid.cx, -1, mid.cz));
    CHECK(out.is_all_air());
    CHECK_FALSE(out.fully_solid());
}

TEST_CASE("chunk_would_be_fully_solid agrees with the surface") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkGenerator gen(make_params());
    MidColumn mid = find_mid_column(gen);
    CHECK(mid.found);
    if (!mid.found) return;

    // Deep below the lowest possible surface → would be solid.
    const int32_t deep_cy = static_cast<int32_t>(std::ceil(mid.range.min_h / CHUNK_HEIGHT)) - 2;
    CHECK(chunk_would_be_fully_solid(gen, mid.cx, deep_cy, mid.cz));

    // One chunk higher overlaps the surface band → not guaranteed solid.
    CHECK_FALSE(chunk_would_be_fully_solid(gen, mid.cx, deep_cy + 1, mid.cz));

    // Above the highest possible surface → never solid.
    const int32_t high_cy = static_cast<int32_t>(std::ceil((mid.range.max_h + 1.0f) / CHUNK_HEIGHT));
    CHECK_FALSE(chunk_would_be_fully_solid(gen, mid.cx, high_cy, mid.cz));

    // Out-of-world chunks are never treated as opaque so boundary faces at
    // the world edges still render.
    CHECK_FALSE(chunk_would_be_fully_solid(gen, mid.cx, -1, mid.cz));
    CHECK_FALSE(chunk_would_be_fully_solid(gen, mid.cx, WORLD_HEIGHT_Y / CHUNK_HEIGHT, mid.cz));
}