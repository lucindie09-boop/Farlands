#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "core/chunk_coords.hpp"
#include <algorithm>
#include <cmath>

using namespace VoxelEngine;

// The generation sweep in WorldUpdater skips chunks whose Y range is more
// than 32 blocks outside [land height, top of content]. For ocean columns the
// top of content is the WATER SURFACE (sea level), not the sea bed — a filter
// that only knew the terrain height skipped every water chunk more than 32
// blocks above the floor, so deep oceans were missing their upper water
// chunks until the 3x3 near-player exception generated them.
TEST_CASE("deep-ocean generation bounds include the water surface") {
    TerrainParams params;
    // Force every column to be deep ocean: the sea bed (macro terrain height,
    // a few hundred blocks) sits far below the water surface, which is exactly
    // the case the old terrain-only filter mishandled.
    params.sea_level = 100000.0f;
    ChunkGenerator gen(params);

    auto col = gen.sample_column_debug(0, 0);
    CHECK(col.water_level >= 0.0f); // ocean column

    const float floor_h = col.height;
    const float water_surface = col.water_level;
    CHECK(water_surface - floor_h > 40.0f); // genuinely deep

    // The water-aware top bound used by the sweep: max(terrain, water level).
    const float top_h = std::max(floor_h, water_surface);
    CHECK(top_h == water_surface);

    // The chunk containing the water surface.
    const int32_t surface_cy = static_cast<int32_t>(std::floor(water_surface / static_cast<float>(CHUNK_HEIGHT)));
    const int32_t surface_bottom = surface_cy * CHUNK_HEIGHT;

    // Regression: the old filter (terrain height only) rejected this chunk
    // (its bottom is more than 32 blocks above the sea bed).
    CHECK(static_cast<float>(surface_bottom) > floor_h + 32.0f);

    // The fixed filter accepts it (top bound = water surface, lower bound
    // still the terrain height).
    CHECK(static_cast<float>(surface_bottom) <= top_h + 32.0f);
    CHECK(static_cast<float>((surface_cy + 1) * CHUNK_HEIGHT) >= floor_h - 32.0f);

    // The sea-bed chunk still passes (lower bound unchanged), so the ocean
    // column generates from floor to surface.
    const int32_t floor_cy = static_cast<int32_t>(std::floor(floor_h / static_cast<float>(CHUNK_HEIGHT)));
    CHECK(static_cast<float>((floor_cy + 1) * CHUNK_HEIGHT) >= floor_h - 32.0f);
    CHECK(static_cast<float>(floor_cy * CHUNK_HEIGHT) <= top_h + 32.0f);
}