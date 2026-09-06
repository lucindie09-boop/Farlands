#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "core/terrain_params.hpp"
#include <cmath>

using namespace VoxelEngine;

namespace {

bool find_seed_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    for (int32_t z = -2048; z <= 2048; z += 32) {
        for (int32_t x = -2048; x <= 2048; x += 32) {
            if (gen.get_biome(x, z) == target) {
                wx = x;
                wz = z;
                return true;
            }
        }
    }
    return false;
}

} // namespace

// =========================================================================
// Biome amplification defaults
// =========================================================================

TEST_CASE("biome amplification: defaults are neutral") {
    BiomeConfig bc;
    bc.reset_defaults();
    for (int i = 0; i < static_cast<int>(BiomeType::Count); ++i) {
        CHECK(bc.amplification[static_cast<size_t>(i)].height == 1.0f);
        CHECK(bc.amplification[static_cast<size_t>(i)].weirdness == 1.0f);
        CHECK(bc.amplification[static_cast<size_t>(i)].min_weirdness == 0.0f);
    }
}

// =========================================================================
// Height amplification: height = sea_level + (raw - sea_level) * amp
// =========================================================================

TEST_CASE("biome amplification: hills height scales around sea level") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    int32_t hx = 0, hz = 0;
    if (!find_seed_column(baseline, BiomeType::Hills, hx, hz)) {
        MESSAGE("No hills column found in the probe window; skipping");
        return;
    }
    const float raw = baseline.get_terrain_height(hx, hz);
    CHECK(raw >= params.sea_level);

    BiomeConfig bc;
    bc.reset_defaults();
    bc.amplification[static_cast<size_t>(BiomeType::Hills)].height = 2.0f;
    ChunkGenerator amp2(params);
    amp2.set_biome_config(bc);

    const float expected2 = params.sea_level + (raw - params.sea_level) * 2.0f;
    CHECK(std::abs(amp2.get_terrain_height(hx, hz) - expected2) < 0.01f);
    // Classification is decided on the raw height, so the biome is unchanged.
    CHECK(amp2.get_biome(hx, hz) == BiomeType::Hills);

    bc.amplification[static_cast<size_t>(BiomeType::Hills)].height = 0.5f;
    ChunkGenerator amp_half(params);
    amp_half.set_biome_config(bc);

    const float expected_half = params.sea_level + (raw - params.sea_level) * 0.5f;
    CHECK(std::abs(amp_half.get_terrain_height(hx, hz) - expected_half) < 0.01f);
    CHECK(amp_half.get_biome(hx, hz) == BiomeType::Hills);
}

TEST_CASE("biome amplification: ocean seabed depth scales around sea level") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    int32_t ox = 0, oz = 0;
    if (!find_seed_column(baseline, BiomeType::Ocean, ox, oz)) {
        MESSAGE("No ocean column found in the probe window; skipping");
        return;
    }
    const float raw = baseline.get_terrain_height(ox, oz);
    CHECK(raw < params.sea_level);

    BiomeConfig bc;
    bc.reset_defaults();
    bc.amplification[static_cast<size_t>(BiomeType::Ocean)].height = 0.5f;
    ChunkGenerator amp_half(params);
    amp_half.set_biome_config(bc);

    const float expected = params.sea_level + (raw - params.sea_level) * 0.5f;
    CHECK(std::abs(amp_half.get_terrain_height(ox, oz) - expected) < 0.01f);
    CHECK(amp_half.get_biome(ox, oz) == BiomeType::Ocean);
}

// =========================================================================
// Weirdness amplification: mask is scaled (clamped) before shaping strength
// =========================================================================

TEST_CASE("biome amplification: weirdness scales the shaping mask") {
    TerrainParams params;

    BiomeConfig bc0;
    bc0.reset_defaults();
    bc0.amplification[static_cast<size_t>(BiomeType::Hills)].weirdness = 0.0f;
    ChunkGenerator gen0(params);
    gen0.set_biome_config(bc0);

    ChunkGenerator gen1(params);  // neutral defaults

    // (a) Where the raw mask is exactly zero the knob is invisible: both
    // generators must produce identical density (0 * amp == 0).
    bool found_zero_mask = false;
    for (int32_t z = -512; z <= 512 && !found_zero_mask; z += 16) {
        for (int32_t x = -512; x <= 512 && !found_zero_mask; x += 16) {
            if (gen1.get_biome(x, z) != BiomeType::Hills) continue;
            if (gen1.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) != 0.0f) continue;
            auto col = gen1.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            CHECK(gen0.sample_terrain_density(x, y, z, col) ==
                  gen1.sample_terrain_density(x, y, z, col));
            found_zero_mask = true;
        }
    }
    CHECK(found_zero_mask);

    // (b) Where shaping is active the knob must actually change the density
    // field (amp 0 pins to minimum strength, neutral keeps the full mask).
    bool found_diff = false;
    for (int32_t z = -512; z <= 512 && !found_diff; z += 16) {
        for (int32_t x = -512; x <= 512 && !found_diff; x += 16) {
            if (gen1.get_biome(x, z) != BiomeType::Hills) continue;
            if (gen1.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) < 0.75f) continue;
            auto col = gen1.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            for (int32_t dy = -8; dy <= 8 && !found_diff; dy += 2) {
                if (gen0.sample_terrain_density(x, y + dy, z, col) !=
                    gen1.sample_terrain_density(x, y + dy, z, col)) {
                    found_diff = true;
                }
            }
        }
    }
    CHECK(found_diff);
}

// =========================================================================
// min_weirdness_amplification: floor for the amplified shaping mask
// =========================================================================

TEST_CASE("biome amplification: min weirdness floors the shaping mask") {
    TerrainParams params;

    // Baseline: neutral (amp 1, no floor).
    ChunkGenerator gen_default(params);

    // Floored at 0.5 with the multiplier pinned to 0: every Hills column gets
    // exactly 0.5 (0 * 1 floored to 0.5).
    BiomeConfig bc_floor;
    bc_floor.reset_defaults();
    bc_floor.amplification[static_cast<size_t>(BiomeType::Hills)].weirdness = 0.0f;
    bc_floor.amplification[static_cast<size_t>(BiomeType::Hills)].min_weirdness = 0.5f;
    ChunkGenerator gen_floor(params);
    gen_floor.set_biome_config(bc_floor);

    // (a) Where the raw mask is below the floor the floor raises the mask:
    // densities must differ from the neutral generator. Pick a zero-mask
    // Hills column (deterministic).
    bool found_zero_mask = false;
    for (int32_t z = -512; z <= 512 && !found_zero_mask; z += 16) {
        for (int32_t x = -512; x <= 512 && !found_zero_mask; x += 16) {
            if (gen_default.get_biome(x, z) != BiomeType::Hills) continue;
            if (gen_default.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) != 0.0f) continue;
            auto col = gen_default.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            bool differs = false;
            for (int32_t dy = -6; dy <= 6 && !differs; dy += 2) {
                if (gen_floor.sample_terrain_density(x, y + dy, z, col) !=
                    gen_default.sample_terrain_density(x, y + dy, z, col)) {
                    differs = true;
                }
            }
            CHECK(differs);
            found_zero_mask = true;
        }
    }
    CHECK(found_zero_mask);

    // (b) With neutral amplification the floor is invisible above its value:
    // max(raw, 0.5) == raw for raw > 0.5, so densities must match the neutral
    // generator exactly.
    BiomeConfig bc_floor_neutral;
    bc_floor_neutral.reset_defaults();
    bc_floor_neutral.amplification[static_cast<size_t>(BiomeType::Hills)].min_weirdness = 0.5f;
    ChunkGenerator gen_floor_neutral(params);
    gen_floor_neutral.set_biome_config(bc_floor_neutral);

    bool found_above_floor = false;
    for (int32_t z = -512; z <= 512 && !found_above_floor; z += 16) {
        for (int32_t x = -512; x <= 512 && !found_above_floor; x += 16) {
            if (gen_default.get_biome(x, z) != BiomeType::Hills) continue;
            if (gen_default.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) < 0.75f) continue;
            auto col = gen_default.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            CHECK(gen_floor_neutral.sample_terrain_density(x, y, z, col) ==
                  gen_default.sample_terrain_density(x, y, z, col));
            found_above_floor = true;
        }
    }
    CHECK(found_above_floor);
}