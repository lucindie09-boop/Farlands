// Times the per-column cost of the scheduler's surface-bounds computation
// (get_chunk_height_range) vs a single center-column sample, across blend
// radii. Mirrors the in-game config. Used to decide how to cheapen the
// generation scheduler's per-column estimate.
// Build: scons time_column_cost
// Run:   bin/time_column_cost
#include "worldgen/chunk_generator.hpp"
#include <cstdio>
#include <cstdint>
#include <chrono>
#include <random>

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

template <typename F>
static double time_one(F&& f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    std::mt19937 rng(4242);
    for (int32_t radius : {0, 2, 8, 20}) {
        ChunkGenerator gen(game_params(radius, 4242));
        gen.set_biome_config(game_biomes());
        const int n = 300;
        double t_sample = 0.0, t_range = 0.0;
        for (int i = 0; i < n; ++i) {
            const int32_t cx = static_cast<int32_t>(rng() % 240) - 120;
            const int32_t cz = static_cast<int32_t>(rng() % 240) - 120;
            t_sample += time_one([&] { gen.sample_column_debug(cx * CHUNK_WIDTH + 8, cz * CHUNK_DEPTH + 8); });
            t_range  += time_one([&] { gen.get_chunk_height_range(cx, cz); });
        }
        printf("radius=%d:  sample_column(center)=%.4f ms   get_chunk_height_range=%.4f ms   ratio=%.1fx\n",
               radius, t_sample / n, t_range / n, t_range / t_sample);
    }
    return 0;
}
