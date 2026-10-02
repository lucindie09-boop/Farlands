// The squish test toggle (worldgen/terrain_squish.hpp) compresses the whole
// vertical relief into a KEPT REGION of chunk slices -- one by default, but
// squish_span widens it (8 slices = a 256-block world), scaling the relief with
// it. What makes it a measurement rather than a toy is that the compression is
// exact: no surface may leave the region, the scheduler's height range may not
// either, and the sweep band may therefore be exactly the region's slices -- if
// any of those leaked, the "horizontal generation" number the toggle exists to
// produce would still contain vertical work.
//
// These tests pin each link of that chain: the map itself, the sampled columns,
// the rigorous chunk height range, the band filter, and finally a real
// generate_chunk inside the region.
#include "doctest.h"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/terrain_squish.hpp"
#include "world/sweep_band.hpp"
#include <algorithm>
#include <cmath>

using namespace VoxelEngine;

namespace {

constexpr int32_t kSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;  // 32

// The squished configuration the tests describe: slice 1 (the floor slice holds
// the bedrock layer, so the default keeps terrain out of it), default sea level.
TerrainParams squished_params(int32_t slice = 1, int32_t span = 1) {
    TerrainParams p;
    p.squish_enabled = true;
    p.squish_slice = slice;
    p.squish_span = span;
    return p;
}

} // namespace

TEST_CASE("squish: the height map is monotone, centred on sea level and bounded") {
    const TerrainParams p = squished_params(1);
    const float centre = squish::slice_center(p);
    CHECK(squish::sea_level(p) == doctest::Approx(centre));
    // Sea level maps to the slice centre: that is what keeps the ocean/land
    // split (below/above sea level) meaningful after the squash.
    CHECK(squish::height(p, p.sea_level) == doctest::Approx(centre));

    // Every height lands in the slice with the band's room to spare: the map's
    // own reach is kHalfRange, and kHalfRange + margin < half a slice.
    for (float h : {-100000.0f, -2000.0f, -800.0f, -100.0f, 0.0f, 100.0f,
                    p.sea_level, 500.0f, 800.0f, 5000.0f, 100000.0f}) {
        CHECK(squish::height(p, h) >= centre - squish::kHalfRange);
        CHECK(squish::height(p, h) <= centre + squish::kHalfRange);
    }
    // The padded bound has to stay strictly inside the slice, or a surface could
    // reach the slice edge and the band filter would have to accept the next one.
    CHECK(squish::kHalfRange + squish::margin(p) < static_cast<float>(CHUNK_HEIGHT) / 2.0f);

    // Monotone: the map may saturate but it may never fold, or the ocean test
    // (raw below sea level) would stop agreeing with the squished comparison.
    float previous = -1e30f;
    for (int32_t i = -50; i <= 50; ++i) {
        const float mapped = squish::height(p, static_cast<float>(i) * 40.0f);
        CHECK(mapped >= previous);
        previous = mapped;
    }
}

TEST_CASE("squish: off is the identity, so the toggle cannot alter normal terrain") {
    TerrainParams off;
    CHECK_FALSE(off.squish_enabled);
    CHECK(squish::height(off, 1234.5f) == 1234.5f);
    CHECK(squish::height(off, -700.0f) == -700.0f);
    CHECK(squish::sea_level(off) == off.sea_level);

    // The neutral margin must be the unsquished envelope's (SURFACE_BAND_OUTER +
    // DENSITY_MARGIN_SLACK = 30) with the default biome config, not the squish's.
    TerrainParams params;
    params.sea_level = 512.0f;  // keep default macro heights well above water
    ChunkGenerator gen(params);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);
    CHECK(gen.density_margin() == doctest::Approx(30.0f));
}

TEST_CASE("squish: sampled columns live in the slice and water tops out at its centre") {
    const TerrainParams p = squished_params(1);
    ChunkGenerator gen(p);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);

    const float bottom = squish::slice_bottom(p);
    const float top = squish::slice_top(p);
    const float centre = squish::slice_center(p);
    for (int32_t z = -128; z <= 128; z += 32) {
        for (int32_t x = -128; x <= 128; x += 32) {
            const ChunkGenerator::ColumnSample col = gen.sample_column_debug(x, z);
            CHECK(col.height >= bottom);
            CHECK(col.height <= top);
            // Land columns carry no water; ocean columns flood exactly to the
            // squished sea level, not the configured one.
            CHECK((col.water_level < 0.0f || col.water_level == doctest::Approx(centre)));
            if (col.water_level >= 0.0f) CHECK(col.height < centre);
        }
    }
}

TEST_CASE("squish: every chunk's content bounds fit one slice, and the band is that slice") {
    for (int32_t slice : {0, 1, 7, 31}) {
        const TerrainParams p = squished_params(slice);
        ChunkGenerator gen(p);
        BiomeConfig bc;
        bc.reset_defaults();
        gen.set_biome_config(bc);

        const float bottom = squish::slice_bottom(p);
        const float top = squish::slice_top(p);
        const float centre = squish::slice_center(p);
        CHECK(gen.density_margin() == doctest::Approx(squish::margin(p)));

        for (int32_t cz = -10; cz <= 10; cz += 5) {
            for (int32_t cx = -10; cx <= 10; cx += 5) {
                const ChunkGenerator::HeightRange range = gen.get_chunk_height_range(cx, cz);
                INFO("chunk " << cx << "," << cz);
                // The rigorous range is the scheduler's answer to "what can this
                // column contain": it may not leave the slice either, or the
                // band filter would accept the slice above or below.
                CHECK(range.min_h >= bottom);
                CHECK(range.max_h <= top);
                CHECK(range.min_h <= range.max_h);
                CHECK((range.max_water_h < 0.0f || range.max_water_h == doctest::Approx(centre)));

                // The band the sweep builds from those bounds, with the squish's
                // own pad (WorldUpdater::band_pad(): 0 when squished), is exactly
                // the slice: one candidate, and the one that holds the terrain.
                const float land_h = range.min_h;
                const float top_h = std::max(range.max_h, range.max_water_h);
                const sweep::ChunkBand band =
                    sweep::band_for_column(land_h, top_h, false, kSlices, 0.0f);
                CHECK(sweep::count(band) == 1);
                CHECK(band.lo == slice);
                CHECK(band.hi == slice);
            }
        }
    }
}

TEST_CASE("squish: a span is the one-slice world scaled, inside its own region") {
    // 8 slices = 256 blocks: the shape a lower world height limit would have.
    const TerrainParams p = squished_params(1, 8);
    const float bottom = squish::slice_bottom(p);
    const float top = squish::slice_top(p);
    const float centre = squish::slice_center(p);
    CHECK(squish::span(p) == 8);
    CHECK(bottom == doctest::Approx(32.0f));
    CHECK(top == doctest::Approx(288.0f));
    CHECK(centre == doctest::Approx(160.0f));
    CHECK(squish::sea_level(p) == doctest::Approx(centre));

    // The map is the span-1 map scaled about the centre: the same monotone
    // squash at 8x the amplitude, so relief keeps its shape (and with it the
    // horizontal structure the mesher pays for) at the new size. That is what
    // lets the span be read as a height-cost knob rather than a new world.
    const TerrainParams one = squished_params(1, 1);
    for (float h : {-3000.0f, -400.0f, -50.0f, 0.0f, 25.0f, p.sea_level, 400.0f, 3000.0f}) {
        const float scaled = centre + 8.0f * (squish::height(one, h) - squish::slice_center(one));
        CHECK(squish::height(p, h) == doctest::Approx(scaled));
    }
    for (float h : {-100000.0f, -2000.0f, -300.0f, 0.0f, 500.0f, 100000.0f}) {
        CHECK(squish::height(p, h) >= centre - squish::half_range(p));
        CHECK(squish::height(p, h) <= centre + squish::half_range(p));
    }
    // The invariant every span has to satisfy: the padded relief still fits
    // inside the region, so the band filter cannot reach the slices around it.
    // margin = 3*span + the generator's 2-block density slack, not scaled.
    CHECK(squish::margin(p) == doctest::Approx(26.0f));
    CHECK(squish::half_range(p) + squish::margin(p) < (top - bottom) * 0.5f);

    // A span that would run past the top of the world shortens instead: the
    // region has to fit in the world it is in.
    const TerrainParams top_heavy = squished_params(30, 8);
    CHECK(squish::span(top_heavy) == 2);
    CHECK(squish::slice_top(top_heavy) == doctest::Approx(1024.0f));
    const TerrainParams zero = squished_params(1, 0);
    CHECK(squish::span(zero) == 1);
}

TEST_CASE("squish: a spanned column's bounds and band stay inside the region") {
    const TerrainParams p = squished_params(1, 8);
    ChunkGenerator gen(p);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);

    CHECK(gen.density_margin() == doctest::Approx(squish::margin(p)));

    for (int32_t cz = -64; cz <= 64; cz += 32) {
        for (int32_t cx = -64; cx <= 64; cx += 32) {
            const ChunkGenerator::ColumnSample col = gen.sample_column_debug(cx, cz);
            INFO("column " << cx << "," << cz);
            CHECK(col.height >= squish::slice_bottom(p));
            CHECK(col.height <= squish::slice_top(p));
            CHECK((col.water_level < 0.0f || col.water_level == doctest::Approx(squish::slice_center(p))));
            const ChunkGenerator::HeightRange range = gen.get_chunk_height_range(cx, cz);
            CHECK(range.min_h >= squish::slice_bottom(p));
            CHECK(range.max_h <= squish::slice_top(p));
            CHECK(range.min_h <= range.max_h);

            // The band the sweep builds from those bounds, with the squish's own
            // pad (WorldUpdater::band_pad(): 0 when squished), stays inside the
            // region: a column costs at most the span's slices, and never the
            // slice just below or above it.
            const float top_h = std::max(range.max_h, range.max_water_h);
            const sweep::ChunkBand band =
                sweep::band_for_column(range.min_h, top_h, false, kSlices, 0.0f);
            CHECK(sweep::count(band) >= 1);
            CHECK(sweep::count(band) <= squish::span(p));
            CHECK(band.lo >= p.squish_slice);
            CHECK(band.hi < p.squish_slice + squish::span(p));
        }
    }
}

TEST_CASE("squish: a spanned world holds its terrain and nothing above the region") {
    const TerrainParams p = squished_params(1, 8);
    ChunkGenerator gen(p);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);

    // The slice that holds the sampled surface must generate blocks, and the
    // first slice above the region must be empty: if the span's relief or its
    // margin were off by a slice, that is where it would show up as terrain
    // generated to hold nothing.
    const ChunkGenerator::ColumnSample col = gen.sample_column_debug(0, 0);
    const int32_t content_slice = static_cast<int32_t>(std::floor(col.height)) / CHUNK_HEIGHT;
    CHECK(content_slice >= p.squish_slice);
    CHECK(content_slice < p.squish_slice + squish::span(p));

    ChunkData chunk;
    gen.generate_chunk(chunk, 0, content_slice, 0, nullptr, false);
    CHECK(chunk.get_block_count() > 0);

    gen.generate_chunk(chunk, 0, p.squish_slice + squish::span(p), 0, nullptr, false);
    CHECK(chunk.get_block_count() == 0);
}

TEST_CASE("squish: generated terrain fills its slice and never the slice above") {
    const TerrainParams p = squished_params(1);
    ChunkGenerator gen(p);
    BiomeConfig bc;
    bc.reset_defaults();
    gen.set_biome_config(bc);

    ChunkData chunk;
    for (int32_t cz = 0; cz <= 1; ++cz) {
        for (int32_t cx = 0; cx <= 1; ++cx) {
            // The slice holds the terrain: full generation, with blocks in it.
            CHECK_FALSE(gen.generate_fast_path(chunk, cx, p.squish_slice, cz));
            gen.generate_chunk(chunk, cx, p.squish_slice, cz, nullptr, false);
            CHECK(chunk.get_block_count() > 0);

            // Everything above is air -- no water either, because the squished
            // sea level is the slice centre, well below the slice's top edge.
            gen.generate_chunk(chunk, cx, p.squish_slice + 1, cz, nullptr, false);
            CHECK(chunk.get_block_count() == 0);
        }
    }
}
