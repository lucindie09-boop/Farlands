// Deterministic check of the generation scheduler's chunk window.
//
// The scheduler skips chunks whose whole y-range lies above the column's top
// content (+32) or below its lowest surface (-32). It previously estimated
// those bounds from ONE center-column sample, so a biome border / mountain
// wall crossing a chunk made it permanently skip chunks that genuinely contain
// the wall -> invisible-solid holes (no mesh, no data to place into).
//
// This tool finds chunk columns that contain a REAL vertical wall (adjacent
// sub-column surfaces far apart inside one chunk), generates the actual
// terrain there, and compares the two windows:
//   - "must-render" chunks (contain solid AND air: the surface crosses them)
//     are asserted to all fall inside the NEW rigorous-range window,
//   - and reports how many of them the OLD center-sample window dropped.
//
// Build: scons sched_window_check
// Run:   bin/sched_window_check [radius_nodes] [seed]
#include "worldgen/chunk_generator.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>
#include <algorithm>

using namespace VoxelEngine;

static TerrainParams game_params(int32_t radius_nodes, int32_t seed) {
    TerrainParams p;
    p.seed = seed;
    p.height_base_y = 312.0f;
    p.climate_blend_radius_nodes = radius_nodes;
    return p;
}

static BiomeConfig game_biomes() {
    BiomeConfig bc;
    auto set = [&](BiomeType b, float h, float w, float mw) {
        bc.amplification[static_cast<size_t>(b)] = BiomeAmplification{h, w, mw};
    };
    set(BiomeType::Plains, 0.6f, 0.5f, 1.0f);
    set(BiomeType::Hills,  1.0f, 1.0f, 1.1f);
    set(BiomeType::Ocean,  1.0f, 1.0f, 1.0f);
    return bc;
}

// Real macro surface spread across a chunk, from 5 sub-column samples (corners
// + centre). This tracks actual adjacent-column height differences, not the
// deliberately loosened range used for fast-path classification.
static float real_spread(const ChunkGenerator& gen, int32_t cx, int32_t cz) {
    const int32_t wx = cx * CHUNK_WIDTH;
    const int32_t wz = cz * CHUNK_DEPTH;
    float lo = 1e9f, hi = -1e9f;
    for (int32_t ox : {4, 8, 12}) {
        for (int32_t oz : {4, 8, 12}) {
            float h = gen.sample_column_debug(wx + ox, wz + oz).height;
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    return hi - lo;
}

int main(int argc, char** argv) {
    const int32_t radius = (argc > 1) ? std::atoi(argv[1]) : 0;
    const int32_t seed   = (argc > 2) ? std::atoi(argv[2]) : 12345;

    TerrainParams params = game_params(radius, seed);
    BiomeConfig biomes = game_biomes();
    ChunkGenerator gen(params);
    gen.set_biome_config(biomes);

    // ---- Find chunk columns whose sub-columns differ by a real wall ----
    int32_t best_cx = 0, best_cz = 0;
    float best_spread = -1.0f;
    for (int32_t cz = -96; cz <= 96; ++cz) {
        for (int32_t cx = -96; cx <= 96; ++cx) {
            const float spread = real_spread(gen, cx, cz);
            if (spread > best_spread) {
                best_spread = spread;
                best_cx = cx;
                best_cz = cz;
            }
        }
    }
    if (best_spread < 40.0f) {
        printf("no real-wall chunk found (best sub-column spread %.1f < 40)\n", best_spread);
        return 1;
    }
    printf("radius=%d seed=%d  wall chunk (%d,%d) real sub-column spread %.1f\n",
           radius, seed, best_cx, best_cz, best_spread);

    // Test that column plus its 4 edge-neighbours (a chunk can also hold a wall
    // when the sub-column spread of its neighbours puts the step inside it).
    int dropped_old = 0, must_render_total = 0, new_dropped = 0;
    int tested = 0;
    const int32_t cols[][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto& off : cols) {
        const int32_t cx = best_cx + off[0];
        const int32_t cz = best_cz + off[1];
        if (real_spread(gen, cx, cz) < 40.0f) continue;
        ++tested;

        const auto range = gen.get_chunk_height_range(cx, cz);
        const float land_h = range.min_h;                                  // new lower
        const float top_h  = std::max(range.max_h, range.max_water_h);     // new upper
        const auto center = gen.sample_column_debug(
            cx * CHUNK_WIDTH + CHUNK_WIDTH / 2, cz * CHUNK_DEPTH + CHUNK_DEPTH / 2);
        const float old_land = center.height;                              // old lower
        const float old_top  = (center.water_level >= 0.0f)
                                   ? std::max(center.height, center.water_level)
                                   : center.height;                        // old upper

        // Generate a generous vertical window so every chunk touching the real
        // surface band is produced (fast-path air/solid chunks aside, what we
        // generate is what the surface actually is).
        const int32_t cy_lo = std::max(0, static_cast<int32_t>(std::floor((range.min_h - 64.0f) / CHUNK_HEIGHT)));
        const int32_t cy_hi = std::min(96, static_cast<int32_t>(std::ceil((top_h + 64.0f) / CHUNK_HEIGHT)));
        for (int32_t cy = cy_lo; cy <= cy_hi; ++cy) {
            ChunkData chunk;
            gen.generate_chunk(chunk, cx, cy, cz, nullptr, false);
            const int32_t chunk_bottom = cy * CHUNK_HEIGHT;
            const int32_t chunk_top    = chunk_bottom + CHUNK_HEIGHT;
            if (chunk.is_all_air()) continue;

            int n_solid = 0;
            for (int32_t lz = 0; lz < CHUNK_DEPTH; ++lz)
                for (int32_t ly = 0; ly < CHUNK_HEIGHT; ++ly)
                    for (int32_t lx = 0; lx < CHUNK_WIDTH; ++lx)
                        if (chunk.get_block(lx, ly, lz) != BlockIDs::AIR) ++n_solid;
            const bool must_render = n_solid < CHUNK_WIDTH * CHUNK_HEIGHT * CHUNK_DEPTH;

            const bool old_above = chunk_bottom > static_cast<int32_t>(old_top + 32.0f);
            const bool old_below = chunk_top   < static_cast<int32_t>(old_land - 32.0f);
            const bool old_kept  = !(old_above || old_below);
            const bool new_above = chunk_bottom > static_cast<int32_t>(top_h + 32.0f);
            const bool new_below = chunk_top   < static_cast<int32_t>(land_h - 32.0f);
            const bool new_kept  = !(new_above || new_below);

            if (must_render) ++must_render_total;
            if (must_render && !new_kept) {
                printf("FAIL: chunk (%d,%d,%d) surface crosses it but the new window drops it "
                       "(range min=%.1f top=%.1f, chunk y %d..%d)\n",
                       cx, cy, cz, land_h, top_h, chunk_bottom, chunk_top);
                ++new_dropped;
            }
            if (must_render && !old_kept) {
                if (dropped_old < 12)
                    printf("  old window dropped (%d,%d,%d) y %d..%d (old land=%.1f top=%.1f)\n",
                           cx, cy, cz, chunk_bottom, chunk_top, old_land, old_top);
                ++dropped_old;
            }
        }
    }

    printf("tested columns=%d  must-render chunks: %d   dropped by OLD center-sample window: %d   dropped by NEW window: %d\n",
           tested, must_render_total, dropped_old, new_dropped);
    if (new_dropped > 0) return 2;
    if (dropped_old == 0) {
        printf("note: no must-render chunk was dropped by the old window in this area; the wall may sit\n"
               "exactly on the center sample or the seed's border runs between chunk columns\n");
        return 3;
    }
    return 0;
}
