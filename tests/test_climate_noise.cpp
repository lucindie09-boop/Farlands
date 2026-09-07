#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "core/terrain_params.hpp"
#include <cmath>
#include <vector>

using namespace VoxelEngine;

// =========================================================================
// Temperature / humidity climate samplers
//
// Samplers read low-frequency 2D simplex fields (one feature per
// ~1/scale blocks, ~8000 at the default scale) mapped to [0,1], sampled on
// a 4-block lattice with bilinear interpolation. These tests pin the
// sampler contract: bounded, non-flat, small per-block change,
// deterministic per seed.
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

TEST_CASE("climate noise: fields are sampled on a 4-block lattice") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // Pick a cell whose corner is an exact 4-block lattice node.
    constexpr float NX = 1236.0f, NZ = -4588.0f;
    const float c00 = gen.sample_temperature_debug(NX, NZ);
    const float c10 = gen.sample_temperature_debug(NX + 4.0f, NZ);
    const float c01 = gen.sample_temperature_debug(NX, NZ + 4.0f);
    const float c11 = gen.sample_temperature_debug(NX + 4.0f, NZ + 4.0f);

    // Bilinear interpolation: the cell midpoint equals the mean of the four
    // corners, and the per-block change along an edge is constant.
    const float mid = gen.sample_temperature_debug(NX + 2.0f, NZ + 2.0f);
    CHECK(std::abs(mid - (c00 + c10 + c01 + c11) * 0.25f) < 1e-5f);

    const float d0 = gen.sample_temperature_debug(NX + 1.0f, NZ) - c00;
    const float d1 = gen.sample_temperature_debug(NX + 2.0f, NZ) - gen.sample_temperature_debug(NX + 1.0f, NZ);
    const float d2 = gen.sample_temperature_debug(NX + 3.0f, NZ) - gen.sample_temperature_debug(NX + 2.0f, NZ);
    const float d3 = c10 - gen.sample_temperature_debug(NX + 3.0f, NZ);
    CHECK(std::abs(d0 - d1) < 1e-5f);
    CHECK(std::abs(d1 - d2) < 1e-5f);
    CHECK(std::abs(d2 - d3) < 1e-5f);

    // Same structure for humidity.
    const float h00 = gen.sample_humidity_debug(NX, NZ);
    const float h10 = gen.sample_humidity_debug(NX + 4.0f, NZ);
    const float h01 = gen.sample_humidity_debug(NX, NZ + 4.0f);
    const float h11 = gen.sample_humidity_debug(NX + 4.0f, NZ + 4.0f);
    const float hmid = gen.sample_humidity_debug(NX + 2.0f, NZ + 2.0f);
    CHECK(std::abs(hmid - (h00 + h10 + h01 + h11) * 0.25f) < 1e-5f);
}

TEST_CASE("climate noise: chunk-cached lattice matches the per-call samplers") {
    TerrainParams params;
    ChunkGenerator gen(params);

    // generate_chunk reads climate from a per-chunk 4-block lattice; one-off
    // queries (height estimation, /locatebiome, tools) read the per-call
    // samplers. Both must agree exactly at every column of a chunk.
    const int32_t cx = 7, cz = -11;
    const int32_t wx0 = cx * CHUNK_WIDTH, wz0 = cz * CHUNK_DEPTH;
    for (int32_t x = 0; x < CHUNK_WIDTH; x += 3) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z += 3) {
            const int32_t wx = wx0 + x, wz = wz0 + z;
            CHECK(gen.sample_temperature_lattice_debug(cx, cz, wx, wz) ==
                  gen.sample_temperature_debug(static_cast<float>(wx), static_cast<float>(wz)));
        }
    }
}

TEST_CASE("climate noise: warp makes biome borders meander, not straight lines") {
    TerrainParams params;
    ChunkGenerator gen(params);  // default recursive anisotropic warp

    // Control: same generator with the climate warp disabled, so borders are
    // bare contours of the smooth 8000-block field.
    TerrainParams flat = params;
    flat.climate_warp_amp_x1 = 0.0f;
    flat.climate_warp_amp_z1 = 0.0f;
    flat.climate_warp_amp_x2 = 0.0f;
    flat.climate_warp_amp_z2 = 0.0f;
    ChunkGenerator gen_flat(flat);

    // Land-biome classification only (no ocean/height involvement). Humidity
    // never changes the label today, so flips are pure temperature crossings.
    auto land_biome_at = [](const ChunkGenerator& g, int32_t x, int32_t z) {
        return g.biome_from_climate_debug(
            g.sample_temperature_debug(static_cast<float>(x), static_cast<float>(z)),
            g.sample_humidity_debug(static_cast<float>(x), static_cast<float>(z)), 0.5f);
    };

    // Locate a Plains/Hills boundary in the control field that runs almost
    // straight down the z-axis (the flip stays within a tight x-window over
    // a long z-range), so the base contour's own drift is negligible and any
    // meander measured below comes from the warp, not the underlying slope.
    int32_t start_x = 0, start_z = 0;
    bool found = false;
    for (int32_t z = -16000; z <= 16000 && !found; z += 200) {
        for (int32_t x = -16000; x <= 16000 && !found; x += 200) {
            bool persists = true;
            for (int32_t dz = -48; dz <= 48 && persists; dz += 8) {
                bool flip_near = false;
                for (int32_t dx = -4; dx <= 4 && !flip_near; ++dx) {
                    flip_near = land_biome_at(gen_flat, x + dx, z + dz) !=
                                land_biome_at(gen_flat, x + dx + 1, z + dz);
                }
                persists = flip_near;
            }
            if (persists) {
                start_x = x;
                start_z = z;
                found = true;
            }
        }
    }
    if (!found) return;

    // Track the boundary down the z-axis (nearest flip to the previous
    // position, so a single border is followed) and record its x-position per
    // row. Both generators are walked over the same run.
    auto track = [&](const ChunkGenerator& g, int32_t z0, int32_t z1) -> std::vector<int32_t> {
        std::vector<int32_t> xs;
        int32_t center = start_x;
        for (int32_t z = z0; z <= z1; z += 4) {
            int32_t bx = std::numeric_limits<int32_t>::max();
            for (int32_t dx = 0; dx <= 400; ++dx) {
                if (land_biome_at(g, center + dx, z) != land_biome_at(g, center + dx + 1, z)) {
                    bx = center + dx;
                    break;
                }
                if (land_biome_at(g, center - dx, z) != land_biome_at(g, center - dx + 1, z)) {
                    bx = center - dx;
                    break;
                }
            }
            if (bx == std::numeric_limits<int32_t>::max()) continue;
            xs.push_back(bx);
            center = bx;
        }
        return xs;
    };

    const int32_t z0 = start_z - 6000, z1 = start_z + 6000;
    const std::vector<int32_t> xs_flat = track(gen_flat, z0, z1);
    const std::vector<int32_t> xs_warped = track(gen, z0, z1);
    CHECK(xs_flat.size() > 2000);
    CHECK(xs_warped.size() > 1000);  // local warp races the tracking window sometimes

    // The warp displaces the sample coordinates by up to a few hundred
    // blocks, so at each row the warped border sits far from the control's,
    // and that offset varies along the border (the warp deforms it into a
    // flowing curve instead of merely translating it). A removed or zeroed
    // warp makes both tracks identical and fails these checks.
    const size_t n = std::min(xs_flat.size(), xs_warped.size());
    int64_t sum_abs = 0;
    int32_t min_off = std::numeric_limits<int32_t>::max();
    int32_t max_off = std::numeric_limits<int32_t>::min();
    for (size_t i = 0; i < n; ++i) {
        const int32_t off = xs_warped[i] - xs_flat[i];
        sum_abs += std::abs(off);
        min_off = std::min(min_off, off);
        max_off = std::max(max_off, off);
    }
    CHECK(sum_abs / static_cast<int64_t>(n) > 2);    // border moved from the control
    CHECK(max_off - min_off > 30);                   // and deformed, not just shifted
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