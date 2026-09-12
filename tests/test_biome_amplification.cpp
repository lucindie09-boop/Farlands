#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "core/terrain_params.hpp"
#include <cmath>

using namespace VoxelEngine;

namespace {

bool find_seed_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    // Wide window (coarse step): climate features span ~8000 blocks, so a
    // small window can sit entirely inside one climate band and miss a target.
    for (int32_t z = -24000; z <= 24000; z += 500) {
        for (int32_t x = -24000; x <= 24000; x += 500) {
            if (gen.get_biome(x, z) == target) {
                wx = x;
                wz = z;
                return true;
            }
        }
    }
    return false;
}

// A column whose whole amplification-blend window (2 nodes = 8 blocks at the
// default radius) is the target biome: the blended knobs there equal the
// biome's own knobs exactly, which is what the exact-height expectations in
// the amplification tests assume.
bool find_isolated_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    for (int32_t z = -24000; z <= 24000; z += 500) {
        for (int32_t x = -24000; x <= 24000; x += 500) {
            if (gen.get_biome(x, z) != target) continue;
            bool isolated = true;
            for (int32_t dz = -8; dz <= 8 && isolated; dz += 4) {
                for (int32_t dx = -8; dx <= 8 && isolated; dx += 4) {
                    if (gen.get_biome(x + dx, z + dz) != target) isolated = false;
                }
            }
            if (isolated) {
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
        CHECK(bc.amplification[static_cast<size_t>(i)].min_weirdness == 1.0f);
        CHECK(bc.amplification[static_cast<size_t>(i)].weirdness_size == 1.0f);
        CHECK(bc.preferred_continentalness[static_cast<size_t>(i)] == 0.5f);
        CHECK(bc.preferred_temperature[static_cast<size_t>(i)] == 0.5f);
        CHECK(bc.preferred_humidity[static_cast<size_t>(i)] == 0.5f);
    }
}

// =========================================================================
// Height amplification: height = sea_level + (raw - sea_level) * amp
// =========================================================================

TEST_CASE("biome amplification: hills height scales around sea level") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    int32_t hx = 0, hz = 0;
    if (!find_isolated_column(baseline, BiomeType::Hills, hx, hz)) {
        MESSAGE("No isolated hills column found in the probe window; skipping");
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

TEST_CASE("biome amplification: ocean seabed is shaped by the climate land biome, not the ocean knob") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    // Find an ocean column with a moderate floor so no bedrock clamp or
    // pathological knob can interfere with the exact expectation.
    int32_t ox = 0, oz = 0;
    bool found = false;
    for (int32_t z = -24000; z <= 24000 && !found; z += 500) {
        for (int32_t x = -24000; x <= 24000 && !found; x += 500) {
            if (baseline.get_biome(x, z) != BiomeType::Ocean) continue;
            const float raw = baseline.get_terrain_height(x, z);
            if (raw > params.sea_level - 150.0f && raw < params.sea_level - 5.0f) {
                ox = x; oz = z; found = true;
            }
        }
    }
    if (!found) {
        MESSAGE("No moderate ocean column found in the probe window; skipping");
        return;
    }
    // Neutral config: every biome knob is 1.0, so the floor equals the raw macro.
    const float raw = baseline.get_terrain_height(ox, oz);

    // Lowering the ocean biome's own height knob must NOT move the seabed: the
    // floor was shaped first by the land biome the climate grid picked, and the
    // ocean override only comes last (biome + water), keeping that height.
    BiomeConfig bc;
    bc.reset_defaults();
    bc.amplification[static_cast<size_t>(BiomeType::Ocean)].height = 0.5f;
    ChunkGenerator amp_half(params);
    amp_half.set_biome_config(bc);

    // Neutral land knobs => the floor stays at the raw macro height.
    CHECK(std::abs(amp_half.get_terrain_height(ox, oz) - raw) < 0.01f);
    CHECK(amp_half.get_biome(ox, oz) == BiomeType::Ocean);
}

// =========================================================================
// Weirdness amplification: mask is scaled (clamped) before shaping strength
// =========================================================================

TEST_CASE("biome amplification: weirdness scales the shaping mask") {
    TerrainParams params;

    // The knob is per-biome, but land is now split between Hills and Plains by
    // the climate grid — so apply it to whatever land biome each scanned
    // column actually has, never assuming Hills.
    ChunkGenerator gen1(params);  // neutral defaults

    auto amp0_gen = [&](BiomeType b) {
        // Pin both the multiplier and its floor to 0 so the effective factor
        // is truly 0 (with the neutral min of 1.0, max(0, 1) would stay at 1).
        BiomeConfig bc;
        bc.reset_defaults();
        bc.amplification[static_cast<size_t>(b)].weirdness = 0.0f;
        bc.amplification[static_cast<size_t>(b)].min_weirdness = 0.0f;
        ChunkGenerator gen0(params);
        gen0.set_biome_config(bc);
        return gen0;
    };

    // (a) Where the raw mask is exactly zero the knob is invisible: both
    // generators must produce identical density (0 * amp == 0).
    bool found_zero_mask = false;
    for (int32_t z = -2048; z <= 2048 && !found_zero_mask; z += 32) {
        for (int32_t x = -2048; x <= 2048 && !found_zero_mask; x += 32) {
            const BiomeType b = gen1.get_biome(x, z);
            if (b == BiomeType::Ocean) continue;
            if (gen1.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) != 0.0f) continue;
            auto col = gen1.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            const ChunkGenerator gen0 = amp0_gen(b);
            CHECK(gen0.sample_terrain_density(x, y, z, col) ==
                  gen1.sample_terrain_density(x, y, z, col));
            found_zero_mask = true;
        }
    }
    CHECK(found_zero_mask);

    // (b) Where shaping is active the knob must actually change the density
    // field (amp 0 pins to minimum strength, neutral keeps the full mask).
    bool found_diff = false;
    for (int32_t z = -2048; z <= 2048 && !found_diff; z += 32) {
        for (int32_t x = -2048; x <= 2048 && !found_diff; x += 32) {
            const BiomeType b = gen1.get_biome(x, z);
            if (b == BiomeType::Ocean) continue;
            if (gen1.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) < 0.75f) continue;
            auto col = gen1.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            const ChunkGenerator gen0 = amp0_gen(b);
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
// min_weirdness_amplification: mask floor expressed as an offset above 1.0
// =========================================================================

TEST_CASE("biome amplification: min weirdness floors the mask above 1.0") {
    TerrainParams params;
    ChunkGenerator gen_default(params);  // amp 1, min 1 (no floor)

    // Same per-biome treatment as the weirdness test: apply the knobs to the
    // land biome each scanned column actually has.
    auto make_gen = [&](BiomeType b, float amp, float min) {
        BiomeConfig bc;
        bc.reset_defaults();
        bc.amplification[static_cast<size_t>(b)].weirdness = amp;
        bc.amplification[static_cast<size_t>(b)].min_weirdness = min;
        ChunkGenerator g(params);
        g.set_biome_config(bc);
        return g;
    };

    // (a) A minimum at or below 1.0 is inert (no floor): amp 0.5 with min 1.0
    // behaves bit-identically to amp 0.5 with min 0.0.
    bool compared = false;
    for (int32_t z = -2048; z <= 2048 && !compared; z += 32) {
        for (int32_t x = -2048; x <= 2048 && !compared; x += 32) {
            const BiomeType b = gen_default.get_biome(x, z);
            if (b == BiomeType::Ocean) continue;
            const ChunkGenerator gen_a = make_gen(b, 0.5f, 1.0f);
            const ChunkGenerator gen_b = make_gen(b, 0.5f, 0.0f);
            auto col = gen_a.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            for (int32_t dy = -6; dy <= 6; dy += 2) {
                const float d0 = gen_a.sample_terrain_density(x, y + dy, z, col);
                CHECK(gen_b.sample_terrain_density(x, y + dy, z, col) == d0);
            }
            compared = true;
        }
    }
    CHECK(compared);

    // 1.1 floors the mask at 0.1 (the old 0.1 behavior); 1.2 floors it at 0.2.
    bool found_zero_mask = false;
    for (int32_t z = -2048; z <= 2048 && !found_zero_mask; z += 32) {
        for (int32_t x = -2048; x <= 2048 && !found_zero_mask; x += 32) {
            const BiomeType b = gen_default.get_biome(x, z);
            if (b == BiomeType::Ocean) continue;
            if (gen_default.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) != 0.0f) continue;
            const ChunkGenerator gen_11 = make_gen(b, 1.0f, 1.1f);
            const ChunkGenerator gen_12 = make_gen(b, 1.0f, 1.2f);
            auto col = gen_11.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            // (b) Where the raw mask is zero the floor binds: the 1.1
            // generator must differ from the default (floor 0.1 > mask 0) and
            // from the 1.2 generator (0.1 vs 0.2 floors).
            bool differs_from_default = false;
            bool differs_from_12 = false;
            for (int32_t dy = -6; dy <= 6; dy += 2) {
                const float d11 = gen_11.sample_terrain_density(x, y + dy, z, col);
                const float d0 = gen_default.sample_terrain_density(x, y + dy, z, col);
                const float d12 = gen_12.sample_terrain_density(x, y + dy, z, col);
                if (d11 != d0) differs_from_default = true;
                if (d11 != d12) differs_from_12 = true;
            }
            CHECK(differs_from_default);
            CHECK(differs_from_12);
            found_zero_mask = true;
        }
    }
    CHECK(found_zero_mask);

    // (c) Where the raw mask exceeds both floors the floor is invisible:
    // 1.1 and 1.2 must agree exactly (both yield mask == raw). A pure
    // multiplier would always differ here, so this pins the semantics to
    // "mask floor", not "factor floor".
    bool found_above = false;
    for (int32_t z = -2048; z <= 2048 && !found_above; z += 32) {
        for (int32_t x = -2048; x <= 2048 && !found_above; x += 32) {
            const BiomeType b = gen_default.get_biome(x, z);
            if (b == BiomeType::Ocean) continue;
            if (gen_default.sample_weirdness_debug(static_cast<float>(x), static_cast<float>(z)) < 0.4f) continue;
            const ChunkGenerator gen_11 = make_gen(b, 1.0f, 1.1f);
            const ChunkGenerator gen_12 = make_gen(b, 1.0f, 1.2f);
            auto col = gen_11.sample_column_debug(x, z);
            const int32_t y = static_cast<int32_t>(std::round(col.height));
            for (int32_t dy = -6; dy <= 6; dy += 2) {
                const float d11 = gen_11.sample_terrain_density(x, y + dy, z, col);
                CHECK(gen_12.sample_terrain_density(x, y + dy, z, col) == d11);
            }
            found_above = true;
        }
    }
    CHECK(found_above);
}

// =========================================================================
// Amplification blending across biome borders
// =========================================================================

TEST_CASE("biome amplification: knobs blend smoothly across biome borders") {
    TerrainParams params;
    BiomeConfig bc;
    bc.reset_defaults();
    // Distinct knobs for a clean measurement: Plains neutral, Hills strong.
    bc.amplification[static_cast<size_t>(BiomeType::Plains)].height = 1.0f;
    bc.amplification[static_cast<size_t>(BiomeType::Hills)].height = 2.0f;
    ChunkGenerator gen(params);
    gen.set_biome_config(bc);

    // Find a Plains lattice node with a Hills node 4 or 8 blocks away along
    // an axis (both nodes, so the blend at each is a pure node blend).
    bool found = false;
    int32_t px = 0, pz = 0, hx = 0, hz = 0;
    for (int32_t z = -2048; z <= 2048 && !found; z += 64) {
        for (int32_t x = -2048; x <= 2048 && !found; x += 64) {
            if (gen.get_biome(x, z) != BiomeType::Plains) continue;
            for (int32_t dir = 0; dir < 4 && !found; ++dir) {
                const int32_t sx = (dir == 0) ? 1 : (dir == 1) ? -1 : 0;
                const int32_t sz = (dir == 2) ? 1 : (dir == 3) ? -1 : 0;
                for (int32_t d = 4; d <= 8 && !found; d += 4) {
                    if (gen.get_biome(x + sx * d, z + sz * d) == BiomeType::Hills) {
                        px = x;
                        pz = z;
                        hx = x + sx * d;
                        hz = z + sz * d;
                        found = true;
                    }
                }
            }
        }
    }
    if (!found) {
        MESSAGE("No plains/hills border pair found in the probe window; skipping");
        return;
    }

    const BiomeAmplification at_plains = gen.blend_amplification_debug(px, pz);
    const BiomeAmplification at_hills = gen.blend_amplification_debug(hx, hz);

    // Each side is pulled toward the other: Plains rises above its 1.0,
    // Hills drops below its 2.0, and both stay strictly between.
    CHECK(at_plains.height > 1.0f);
    CHECK(at_plains.height < 2.0f);
    CHECK(at_hills.height > 1.0f);
    CHECK(at_hills.height < 2.0f);

    // Uniform window average: each side is pulled toward the other equally
    // (no extreme-biome half-weight rule), so the two sides give up close to
    // the same share of the 1.0 gap.
    const float share_from_hills = (at_plains.height - 1.0f) / (2.0f - 1.0f);
    const float share_from_plains = (2.0f - at_hills.height) / (2.0f - 1.0f);
    CHECK(std::abs(share_from_hills - share_from_plains) < 0.05f);

    // Radius 0 disables blending: each node uses its own biome's knobs
    // exactly.
    TerrainParams p0 = params;
    p0.climate_blend_radius_nodes = 0;
    ChunkGenerator gen0(p0);
    gen0.set_biome_config(bc);
    const BiomeAmplification at_plains0 = gen0.blend_amplification_debug(px, pz);
    const BiomeAmplification at_hills0 = gen0.blend_amplification_debug(hx, hz);
    CHECK(at_plains0.height == 1.0f);
    CHECK(at_hills0.height == 2.0f);
}

// =========================================================================
// Amplification blend: ocean seabed follows the climate land blend
// =========================================================================

TEST_CASE("biome amplification: ocean columns use the climate land blend for their floor") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    // Moderate floor (raw just under sea level) so extreme land knobs can't
    // drive the floor below the bedrock clamp.
    int32_t ox = 0, oz = 0;
    bool found = false;
    for (int32_t z = -24000; z <= 24000 && !found; z += 500) {
        for (int32_t x = -24000; x <= 24000 && !found; x += 500) {
            if (baseline.get_biome(x, z) != BiomeType::Ocean) continue;
            const float raw = baseline.get_terrain_height(x, z);
            if (raw > params.sea_level - 45.0f && raw < params.sea_level - 5.0f) {
                ox = x; oz = z; found = true;
            }
        }
    }
    if (!found) {
        MESSAGE("No moderate ocean column found in the probe window; skipping");
        return;
    }
    const float raw = baseline.get_terrain_height(ox, oz);
    CHECK(raw < params.sea_level);

    // Land biomes with extreme knobs now reach into ocean basins: the floor
    // scales with the blended land field, NOT the ocean's own (neutral) amp.
    BiomeConfig bc;
    bc.reset_defaults();
    bc.amplification[static_cast<size_t>(BiomeType::Plains)].height = 4.0f;
    bc.amplification[static_cast<size_t>(BiomeType::Hills)].height = 4.0f;
    ChunkGenerator gen(params);
    gen.set_biome_config(bc);

    const float land_knob = gen.blend_amplification_debug(ox, oz).height;
    CHECK(land_knob > 1.0f);  // the blend picked up the extreme land knobs
    const float expected = params.sea_level + (raw - params.sea_level) * land_knob;
    CHECK(std::abs(gen.get_terrain_height(ox, oz) - expected) < 0.01f);
    CHECK(gen.get_biome(ox, oz) == BiomeType::Ocean);
}

// =========================================================================
// Weirdness size: scales BOTH halves of the 3D-shape envelope
// =========================================================================

namespace {

// Largest |y - macro height| at which the density still carries any shape
// contribution, i.e. where the surface band has not yet faded to zero. This
// is the observable "how many blocks can the zone alter" reach.
// ColumnSample is a private generator type, so take the column generically.
template <typename Column>
float shape_envelope_reach(ChunkGenerator& gen, int32_t x, int32_t z, const Column& col) {
    float reach = 0.0f;
    const int32_t lo = static_cast<int32_t>(std::floor(col.height)) - 60;
    const int32_t hi = static_cast<int32_t>(std::ceil(col.height)) + 60;
    for (int32_t y = lo; y <= hi; ++y) {
        const float delta = col.height - static_cast<float>(y);
        if (gen.sample_terrain_density(x, y, z, col) != delta) {
            reach = std::max(reach, std::abs(delta));
        }
    }
    return reach;
}

} // namespace

TEST_CASE("biome amplification: weirdness_size scales the shape envelope") {
    TerrainParams params;
    ChunkGenerator baseline(params);

    // An isolated land column: its whole blend window is one biome, so the
    // blended knobs equal that biome's own exactly (which the exact size
    // expectations below rely on). The envelope is observable at any mask
    // value, since the strength floor is non-zero (lerp(5, 50, 0) = 5).
    int32_t x = 0, z = 0;
    bool found = find_isolated_column(baseline, BiomeType::Hills, x, z);
    if (!found) found = find_isolated_column(baseline, BiomeType::Plains, x, z);
    if (!found) {
        MESSAGE("No isolated land column found in the probe window; skipping");
        return;
    }
    const auto col = baseline.sample_column_debug(x, z);
    const BiomeType biome = baseline.get_biome(x, z);
    CHECK(biome != BiomeType::Ocean);

    // Neutral 1.0 reproduces the built-in band (SURFACE_BAND_OUTER = 28).
    const float reach1 = shape_envelope_reach(baseline, x, z, col);
    CHECK(reach1 > 27.0f);
    CHECK(reach1 <= 28.0f);

    // 0 = the zone alters nothing: the density collapses to the plain macro
    // delta, with no shape term at any distance.
    BiomeConfig bc0;
    bc0.reset_defaults();
    bc0.amplification[static_cast<size_t>(biome)].weirdness_size = 0.0f;
    ChunkGenerator gen0(params);
    gen0.set_biome_config(bc0);
    const int32_t probe_y = static_cast<int32_t>(std::floor(col.height)) + 3;
    CHECK(gen0.sample_terrain_density(x, probe_y, z, col) ==
          col.height - static_cast<float>(probe_y));
    CHECK(shape_envelope_reach(gen0, x, z, col) == 0.0f);

    // 2 = double the reach (band 9/28 -> 18/56).
    BiomeConfig bc2;
    bc2.reset_defaults();
    bc2.amplification[static_cast<size_t>(biome)].weirdness_size = 2.0f;
    ChunkGenerator gen2(params);
    gen2.set_biome_config(bc2);
    const float reach2 = shape_envelope_reach(gen2, x, z, col);
    CHECK(reach2 > 55.0f);
    CHECK(reach2 <= 56.0f);

    // ...and double the displacement: inside the inner band (|delta| < 9) the
    // shape term is exactly proportional to the knob.
    const float delta = col.height - static_cast<float>(probe_y);
    const float term1 = baseline.sample_terrain_density(x, probe_y, z, col) - delta;
    const float term2 = gen2.sample_terrain_density(x, probe_y, z, col) - delta;
    CHECK(std::abs(term1) > 1e-3f);
    CHECK(std::abs(term2 - 2.0f * term1) < 1e-3f);
}

TEST_CASE("biome amplification: density margin covers the widest weirdness size") {
    TerrainParams params;
    ChunkGenerator gen(params);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);

    // Neutral config: SURFACE_BAND_OUTER (28) + slack (2).
    CHECK(gen.density_margin() == 30.0f);

    bc.amplification[static_cast<size_t>(BiomeType::Hills)].weirdness_size = 2.0f;
    gen.set_biome_config(bc);
    CHECK(gen.density_margin() == 58.0f);

    // Never below the neutral band, even with every biome switched off.
    for (auto& a : bc.amplification) a.weirdness_size = 0.0f;
    gen.set_biome_config(bc);
    CHECK(gen.density_margin() == 30.0f);
}

TEST_CASE("biome amplification: weirdness_size blends like the other knobs") {
    TerrainParams params;
    ChunkGenerator gen(params);
    BiomeConfig bc;
    bc.reset_defaults();
    bc.amplification[static_cast<size_t>(BiomeType::Hills)].weirdness_size = 1.5f;
    gen.set_biome_config(bc);

    // A column whose whole blend window is Hills: the window mean equals the
    // biome's own value exactly.
    int32_t hx = 0, hz = 0;
    if (!find_isolated_column(gen, BiomeType::Hills, hx, hz)) {
        MESSAGE("No isolated hills column found in the probe window; skipping");
        return;
    }
    CHECK(std::abs(gen.blend_amplification_debug(hx, hz).weirdness_size - 1.5f) < 1e-4f);
}