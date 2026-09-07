#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "core/terrain_params.hpp"
#include <cmath>

using namespace VoxelEngine;

// =========================================================================
// Temperature / humidity climate samplers
//
// Samplers read low-frequency 2D simplex fields (one feature per
// ~1/scale blocks, ~8000 at the default scale) mapped to [0,1]. The land
// biome grid currently maps every climate cell to Hills, so these tests pin
// the sampler contract only: bounded, non-flat, deterministic per seed.
// =========================================================================

TEST_CASE("climate noise: samplers stay in [0,1]") {
    TerrainParams params;
    ChunkGenerator gen(params);

    for (int32_t z = -20000; z <= 20000; z += 500) {
        for (int32_t x = -20000; x <= 20000; x += 500) {
            const float t = gen.sample_temperature_debug(static_cast<float>(x), static_cast<float>(z));
            const float h = gen.sample_humidity_debug(static_cast<float>(x), static_cast<float>(z));
            CHECK(t >= 0.0f);
            CHECK(t <= 1.0f);
            CHECK(h >= 0.0f);
            CHECK(h <= 1.0f);
        }
    }
}

TEST_CASE("climate noise: fields are not flat and vary at ~8000-block scale") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // Step 1000 blocks (input step 0.125 at default scale) so samples sit
    // inside noise lattice cells instead of on lattice nodes (nodes read
    // exactly 0.0 -> 0.5).
    float t_min = 1.0f, t_max = 0.0f, h_min = 1.0f, h_max = 0.0f;
    for (int32_t d = 0; d <= 32000; d += 1000) {
        const float t = gen.sample_temperature_debug(static_cast<float>(d), 0.0f);
        const float h = gen.sample_humidity_debug(0.0f, static_cast<float>(d));
        t_min = std::min(t_min, t); t_max = std::max(t_max, t);
        h_min = std::min(h_min, h); h_max = std::max(h_max, h);
    }
    // Four lattice cells (~8000 blocks each) must span real climate range.
    CHECK(t_max - t_min > 0.1f);
    CHECK(h_max - h_min > 0.1f);

    // Low frequency: adjacent 1-block samples are nearly equal.
    float max_delta_t = 0.0f, max_delta_h = 0.0f;
    for (int32_t x = -4000; x <= 4000; x += 100) {
        const float t0 = gen.sample_temperature_debug(static_cast<float>(x), 0.0f);
        const float t1 = gen.sample_temperature_debug(static_cast<float>(x + 1), 0.0f);
        const float h0 = gen.sample_humidity_debug(0.0f, static_cast<float>(x));
        const float h1 = gen.sample_humidity_debug(0.0f, static_cast<float>(x + 1));
        max_delta_t = std::max(max_delta_t, std::abs(t1 - t0));
        max_delta_h = std::max(max_delta_h, std::abs(h1 - h0));
    }
    CHECK(max_delta_t < 0.01f);
    CHECK(max_delta_h < 0.01f);
}

TEST_CASE("climate grid: temperate band maps to Plains, extremes to Hills") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // Boundaries: 0.43 / 0.57 split cold/neutral/hot (and dry/neutral/humid).
    const float t_neutral = 0.5f;

    // Cold and hot bands stay Hills regardless of humidity.
    CHECK(gen.biome_from_climate_debug(0.20f, 0.5f, 0.5f) == BiomeType::Hills);
    CHECK(gen.biome_from_climate_debug(0.20f, 0.9f, 0.5f) == BiomeType::Hills);
    CHECK(gen.biome_from_climate_debug(0.80f, 0.1f, 0.5f) == BiomeType::Hills);
    CHECK(gen.biome_from_climate_debug(0.80f, 0.9f, 0.5f) == BiomeType::Hills);

    // Neutral-temperature band is Plains at any humidity.
    CHECK(gen.biome_from_climate_debug(t_neutral, 0.2f, 0.5f) == BiomeType::Plains);
    CHECK(gen.biome_from_climate_debug(t_neutral, 0.5f, 0.5f) == BiomeType::Plains);
    CHECK(gen.biome_from_climate_debug(t_neutral, 0.8f, 0.5f) == BiomeType::Plains);

    // Exact threshold edges: <= cold_max is cold, < hot_min is neutral.
    CHECK(gen.biome_from_climate_debug(0.43f, 0.5f, 0.5f) == BiomeType::Hills);
    CHECK(gen.biome_from_climate_debug(0.569f, 0.5f, 0.5f) == BiomeType::Plains);
    CHECK(gen.biome_from_climate_debug(0.57f, 0.5f, 0.5f) == BiomeType::Hills);
}

TEST_CASE("climate grid: sample_column routes through the grid and Plains/Hills appear in the world") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // For land columns, sample_column's biome must equal the grid lookup of
    // its own sampled climate values; ocean stays height-decided.
    for (int32_t z = -4000; z <= 4000; z += 250) {
        for (int32_t x = -4000; x <= 4000; x += 250) {
            const auto col = gen.sample_column_debug(x, z);
            if (col.biome == BiomeType::Ocean) {
                CHECK(col.height < params.sea_level);
                continue;
            }
            CHECK(col.height >= params.sea_level);
            CHECK(col.biome == gen.biome_from_climate_debug(col.temperature, col.humidity, col.cont));
        }
    }

    // Plains and Hills both actually occur as land biomes. Find a neutral-
    // temperature land column (Plains) and a cold one (Hills).
    bool found_plains = false, found_hills = false;
    for (int32_t z = -24000; z <= 24000 && !(found_plains && found_hills); z += 500) {
        for (int32_t x = -24000; x <= 24000 && !(found_plains && found_hills); x += 500) {
            const float t = gen.sample_temperature_debug(static_cast<float>(x), static_cast<float>(z));
            if (!found_plains && t > 0.43f && t < 0.57f) {
                const auto col = gen.sample_column_debug(x, z);
                if (col.biome != BiomeType::Ocean) {
                    CHECK(col.biome == BiomeType::Plains);
                    found_plains = true;
                }
            }
            if (!found_hills && t <= 0.43f) {
                const auto col = gen.sample_column_debug(x, z);
                if (col.biome != BiomeType::Ocean) {
                    CHECK(col.biome == BiomeType::Hills);
                    found_hills = true;
                }
            }
        }
    }
    CHECK(found_plains);
    CHECK(found_hills);
}

TEST_CASE("climate noise: deterministic per seed, differs across seeds") {
    TerrainParams params;
    ChunkGenerator gen(params);

    const float t = gen.sample_temperature_debug(1234.0f, -5678.0f);
    const float h = gen.sample_humidity_debug(1234.0f, -5678.0f);

    // Same seed reproduces the same field.
    ChunkGenerator gen2(params);
    CHECK(gen2.sample_temperature_debug(1234.0f, -5678.0f) == t);
    CHECK(gen2.sample_humidity_debug(1234.0f, -5678.0f) == h);

    // Different seed changes the field.
    TerrainParams other = params;
    other.seed = params.seed + 1;
    ChunkGenerator gen3(other);
    CHECK(gen3.sample_temperature_debug(1234.0f, -5678.0f) != t);
    CHECK(gen3.sample_humidity_debug(1234.0f, -5678.0f) != h);
}