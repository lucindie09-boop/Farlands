// Stage 1 of the seed-grid far mode (docs/lod-modes.md): what does it COST to
// take a column's surface height without generating the column?
//
// Three answers, all through the generator's public API:
//   1. find_surface_y   - the rigorous search that exists today (per-block scan)
//   2. march_surface    - the stride-then-bisect search the mode would use
//   3. get_chunk_height_range - what the scheduler pays per COLUMN before it
//      generates anything today, as the "what the world already spends" figure
//
// It measures a bounded sample of columns rather than a whole area, because the
// whole area does not finish (the first version of this tool was killed at ten
// minutes): per-column cost through the public API is dominated by re-deriving
// the climate/blend/weirdness stack on every call, and a tool that cannot be run
// cannot be a measurement. Per-column microseconds are the real result; the
// area figures printed beside them are extrapolations and are labelled as such.
//
// The march is checked against find_surface_y while it runs, because a cheap
// sampler that disagrees with the world's own surface is not a samplable one.
// Build: scons lod_sample_bench
// Run:   bin/lod_sample_bench [columns]
#include "lod/lod_march.hpp"
#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

using namespace VoxelEngine;

// Mirrors the in-game configuration (data/terrain_config.json and
// data/biomes.json), the way the other terrain tools do: the loaders take a
// godot::String, so a standalone tool restates the values it is measuring.
static TerrainParams game_params(int32_t seed) {
    TerrainParams p;
    p.seed = seed;
    p.height_base_y = 312.0f;
    p.climate_blend_radius_nodes = 16;
    return p;
}

static BiomeConfig game_biomes() {
    BiomeConfig bc;
    auto set = [&](BiomeType b, float h, float w, float mw) {
        bc.amplification[static_cast<size_t>(b)] = BiomeAmplification{h, w, mw};
    };
    set(BiomeType::Plains, 0.4f, 0.6f, 1.0f);
    set(BiomeType::Hills, 1.0f, 1.0f, 1.0f);
    set(BiomeType::Ocean, 1.0f, 1.0f, 1.0f);
    return bc;
}

static double ms_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main(int argc, char** argv) {
    constexpr int32_t kAreaBlocks = 2048;
    constexpr int32_t kSeed = 4242;
    const int32_t columns = argc > 1 ? std::max(16, std::atoi(argv[1])) : 256;

    ChunkGenerator gen(game_params(kSeed));
    gen.set_biome_config(game_biomes());
    const float margin = gen.density_margin();

    std::mt19937 rng(99);
    std::uniform_int_distribution<int32_t> coord(0, kAreaBlocks - 1);

    printf("seed=%d  surface band=+/-%.1f blocks  sample=%d columns over a %dx%d area\n\n",
           kSeed, margin, columns, kAreaBlocks, kAreaBlocks);

    // --- 1. the column sample itself -------------------------------------
    double t_sample = 0.0;
    std::vector<ChunkGenerator::ColumnSample> samples;
    samples.reserve(columns);
    std::vector<std::pair<int32_t, int32_t>> positions;
    positions.reserve(columns);
    for (int32_t i = 0; i < columns; ++i) {
        const int32_t x = coord(rng);
        const int32_t z = coord(rng);
        positions.emplace_back(x, z);
        const auto t0 = std::chrono::steady_clock::now();
        samples.push_back(gen.sample_column_debug(x, z));
        t_sample += ms_since(t0);
    }

    // --- 2. the rigorous surface search ----------------------------------
    double t_rigorous = 0.0;
    std::vector<int32_t> reference(columns);
    for (int32_t i = 0; i < columns; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        reference[i] = gen.find_surface_y(positions[i].first, positions[i].second);
        t_rigorous += ms_since(t0);
    }

    printf("per column:\n");
    printf("  sample_column_debug %9.1f us\n", t_sample * 1000.0 / columns);
    printf("  find_surface_y     %9.1f us   (the rigorous search, per-block scan)\n",
           t_rigorous * 1000.0 / columns);
    printf("\n");

    // --- 3. where a column's microseconds actually go ---------------------
    // The sampler a grid needs is only as cheap as its most expensive ingredient,
    // so the ingredients are timed separately rather than inferred from the total.
    {
        struct Item {
            const char* name;
            double us;
        };
        std::vector<Item> items;
        auto time_it = [&](const char* name, auto&& fn) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int32_t i = 0; i < columns; ++i) fn(positions[i].first, positions[i].second);
            items.push_back({name, ms_since(t0) * 1000.0 / columns});
        };
        time_it("sample_temperature", [&](int32_t x, int32_t z) { (void)gen.sample_temperature_debug((float)x, (float)z); });
        time_it("sample_land_shape", [&](int32_t x, int32_t z) { (void)gen.sample_land_shape_debug((float)x, (float)z); });
        time_it("sample_continentalness", [&](int32_t x, int32_t z) { (void)gen.sample_continentalness_debug((float)x, (float)z); });
        time_it("sample_weirdness", [&](int32_t x, int32_t z) { (void)gen.sample_weirdness_debug((float)x, (float)z); });
        time_it("blend_amplification", [&](int32_t x, int32_t z) { (void)gen.blend_amplification_debug(x, z); });
        time_it("get_biome", [&](int32_t x, int32_t z) { (void)gen.get_biome(x, z); });
        time_it("get_terrain_height", [&](int32_t x, int32_t z) { (void)gen.get_terrain_height(x, z); });
        time_it("quick_height_estimate", [&](int32_t x, int32_t z) { (void)gen.quick_height_estimate(x, z); });
        double t_density = 0.0;
        for (int32_t i = 0; i < columns; ++i) {
            const int32_t y = static_cast<int32_t>(samples[i].height);
            const auto t0 = std::chrono::steady_clock::now();
            (void)gen.sample_terrain_density(positions[i].first, y, positions[i].second, samples[i]);
            t_density += ms_since(t0);
        }
        items.push_back({"sample_terrain_density(public)", t_density * 1000.0 / columns});
        printf("ingredients (public API, per call):\n");
        for (const Item& item : items) printf("  %-30s %9.2f us\n", item.name, item.us);
        printf("\n");
    }

    // --- 4. the march, at each candidate spacing --------------------------
    printf("march (stride then bisect, over the same columns):\n");
    for (int32_t step : {2, 4, 8, 16}) {
        double t_march = 0.0;
        int64_t evaluations = 0;
        int32_t mismatches = 0;
        int32_t worst_delta = 0;
        int32_t found = 0;
        for (int32_t i = 0; i < columns; ++i) {
            const int32_t x = positions[i].first;
            const int32_t z = positions[i].second;
            const auto& column = samples[i];
            const int32_t top = static_cast<int32_t>(column.height + margin);
            const int32_t bottom = static_cast<int32_t>(column.height - margin);
            const auto t0 = std::chrono::steady_clock::now();
            const lod::MarchResult r = lod::march_surface(top, bottom, step, [&](int32_t y) {
                return gen.sample_terrain_density(x, y, z, column) > 0.0f;
            });
            t_march += ms_since(t0);
            evaluations += r.evaluations;
            if (r.found) ++found;
            const int32_t delta = r.found ? std::abs(r.y - reference[i]) : 9999;
            if (delta != 0) ++mismatches;
            worst_delta = std::max(worst_delta, delta);
        }
        const double us = t_march * 1000.0 / columns;
        const double area_columns = static_cast<double>(kAreaBlocks / step) * (kAreaBlocks / step);
        printf("  step=%-2d  %9.1f us/column  %5.1f evals/column  found=%d/%d  "
               "vs rigorous: mismatches=%d worst|d|=%d\n",
               step, us, static_cast<double>(evaluations) / columns, found, columns,
               mismatches, worst_delta);
        printf("            extrapolated over the %dx%d area at this spacing: %.1f ms "
               "(%d columns, %.1f MB of uint16 heights)\n",
               kAreaBlocks, kAreaBlocks, us * area_columns / 1000.0,
               static_cast<int32_t>(area_columns), area_columns * 2.0 / (1024.0 * 1024.0));
    }

    // --- 5. what the world already spends per column ----------------------
    {
        constexpr int32_t kChunksPerAxis = kAreaBlocks / CHUNK_WIDTH;
        const int32_t chunk_columns = std::max(8, columns / 4);
        std::mt19937 rng2(7);
        std::uniform_int_distribution<int32_t> chunk_coord(-kChunksPerAxis, kChunksPerAxis);
        double t_range = 0.0;
        for (int32_t i = 0; i < chunk_columns; ++i) {
            const int32_t cx = chunk_coord(rng2);
            const int32_t cz = chunk_coord(rng2);
            const auto t0 = std::chrono::steady_clock::now();
            const auto range = gen.get_chunk_height_range(cx, cz);
            t_range += ms_since(t0);
            if (range.max_h < range.min_h) printf(" (empty range)\n");
        }
        const int32_t chunks = kChunksPerAxis * kChunksPerAxis;
        const double us = t_range * 1000.0 / chunk_columns;
        printf("\n");
        printf("  get_chunk_height_range %9.1f us/column  (what the scheduler pays per column "
               "before\n                          generating anything)\n", us);
        printf("            extrapolated over the same area: %.1f ms for %d chunk columns\n",
               us * chunks / 1000.0, chunks);
        // Palette-compressed chunks cost 1-20 KB each on uniform terrain
        // (AGENTS.md, "Palette-compressed storage"), which is what the same area
        // costs as blocks rather than as samples.
        printf("            that area is %d chunk columns; one 32-block slice of them is "
               "%.1f-%.1f MB of block storage,\n            against %.1f MB of uint16 heights "
               "at an 8-block spacing (%.1f MB at 2)\n",
               chunks, chunks * 1.0 / 1024.0, chunks * 20.0 / 1024.0,
               static_cast<double>(kAreaBlocks / 8) * (kAreaBlocks / 8) * 2.0 / (1024.0 * 1024.0),
               static_cast<double>(kAreaBlocks / 2) * (kAreaBlocks / 2) * 2.0 / (1024.0 * 1024.0));
    }
    return 0;
}
