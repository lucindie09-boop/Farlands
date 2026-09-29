#include "doctest.h"

#include "biome_amplification_test_support.hpp"

#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "core/terrain_params.hpp"

#include <cmath>

using namespace VoxelEngine;
using namespace biome_amplification_test;

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
