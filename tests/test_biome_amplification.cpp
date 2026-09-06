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

TEST_CASE("biome amplification: defaults are neutral (1.0 on both knobs)") {
    BiomeConfig bc;
    bc.reset_defaults();
    for (int i = 0; i < static_cast<int>(BiomeType::Count); ++i) {
        CHECK(bc.amplification[static_cast<size_t>(i)].height == 1.0f);
        CHECK(bc.amplification[static_cast<size_t>(i)].weirdness == 1.0f);
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