// Standalone probe: measure how per-biome height amplification (and the final
// macro height) ramps across a real Hills/Plains border, for a given blend
// radius. Mirrors the in-game config (data/terrain_config.json + biomes.json)
// so results match what generation produces.
//
// Build: scons blend_measure
// Run:   bin/blend_measure [radius_nodes] [seed]
#include "worldgen/chunk_generator.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <vector>

using namespace VoxelEngine;

static char biome_char(BiomeType b) {
    switch (b) {
        case BiomeType::Ocean:  return 'O';
        case BiomeType::Hills:  return 'H';
        case BiomeType::Plains: return 'P';
        default:                return '?';
    }
}

static TerrainParams game_params(int32_t radius_nodes, int32_t seed) {
    // Mirror data/terrain_config.json (defaults already match; only the
    // base height differs between default and the JSON).
    TerrainParams p;
    p.seed = seed;
    p.height_base_y = 312.0f;
    p.climate_blend_radius_nodes = radius_nodes;
    return p;
}

static BiomeConfig game_biomes() {
    // Mirror data/biomes.json.
    BiomeConfig bc;
    auto set = [&](BiomeType b, float h, float w, float mw) {
        bc.amplification[static_cast<size_t>(b)] = BiomeAmplification{h, w, mw};
    };
    set(BiomeType::Plains, 0.6f, 0.5f, 1.0f);
    set(BiomeType::Hills,  1.0f, 1.0f, 1.1f);
    set(BiomeType::Ocean,  1.0f, 1.0f, 1.0f);
    return bc;
}

int main(int argc, char** argv) {
    const int32_t radius = (argc > 1) ? std::atoi(argv[1]) : 2;
    const int32_t seed   = (argc > 2) ? std::atoi(argv[2]) : 12345;

    TerrainParams params = game_params(radius, seed);
    BiomeConfig biomes = game_biomes();
    ChunkGenerator gen(params);
    gen.set_biome_config(biomes);

    // ---- Scan for a Hills/Plains land border (coarse) ----
    int32_t border_x = 0;
    int32_t border_z = 0;
    bool found = false;
    for (int32_t z = -20000; z <= 20000 && !found; z += 8) {
        for (int32_t x = -20000; x <= 20000 && !found; x += 8) {
            const BiomeType a = gen.sample_column_debug(x, z).biome;
            const BiomeType b = gen.sample_column_debug(x + 1, z).biome;
            if (a != b &&
                ((a == BiomeType::Plains && b == BiomeType::Hills) ||
                 (a == BiomeType::Hills  && b == BiomeType::Plains))) {
                border_x = x;
                border_z = z;
                found = true;
            }
        }
    }
    if (!found) {
        printf("no plains/hills border found in scan range\n");
        return 1;
    }
    printf("radius=%d seed=%d  border near (%d, %d)\n", radius, seed, border_x, border_z);

    // ---- Transect along x through the border ----
    const int x0 = border_x - 96;
    const int x1 = border_x + 96;
    // Sample per-block macro heights + the amp field over the same span for
    // reference profiles.
    std::vector<int32_t> biome_at;
    std::vector<float> amp_at;
    std::vector<float> raw_at;
    const bool blended = radius > 0;
    for (int32_t x = x0; x <= x1; ++x) {
        ChunkGenerator::ColumnSample col = gen.sample_column_debug(x, border_z);
        biome_at.push_back(static_cast<int32_t>(col.biome));
        const float amp_eff = blended
            ? gen.blend_amplification_debug(x, border_z).height
            : gen.get_biome_config().amplification[static_cast<size_t>(col.biome)].height;
        amp_at.push_back(amp_eff);
        raw_at.push_back(col.land_height);
    }
    const int N = static_cast<int>(biome_at.size());

    // Summarise the transition: where does the biome switch, and where is the
    // steepest 1-block height step?
    int switch_at = -1;
    for (int i = 1; i < N; ++i) {
        if (biome_at[i] != biome_at[i - 1]) { switch_at = i; break; }
    }
    // real per-column macro height:
    std::vector<float> hgt;
    for (int32_t x = x0; x <= x1; ++x) {
        hgt.push_back(gen.sample_column_debug(x, border_z).height);
    }
    int steep_at = -1;
    float steep_dh = -1.0f;
    for (int i = 1; i < N; ++i) {
        const float dh = std::abs(hgt[i] - hgt[i - 1]);
        if (dh > steep_dh) { steep_dh = dh; steep_at = i; }
    }
    // height delta between the two plateau averages (outermost 32 cols each side)
    float pa = 0.0f, pb = 0.0f;
    for (int i = 0; i < 32; ++i) { pa += hgt[i]; pb += hgt[N - 1 - i]; }
    pa /= 32.0f; pb /= 32.0f;

    printf("cols: x=%d..%d (z=%d)  plateau heights: left=%8.1f right=%8.1f  delta=%8.1f\n",
           x0, x1, border_z, pa, pb, std::abs(pb - pa));
    printf("biome switch at transect col %d (x=%d); steepest 1-block height step: %6.1f at col %d (x=%d)\n",
           switch_at, x0 + switch_at, steep_dh, steep_at, x0 + steep_at);

    printf("profile (dx, biome, amp, raw, hgt):\n");
    for (int i = 0; i < N; i += 2) {
        printf("%6d  %c  amp=%5.3f raw=%7.1f hgt=%7.1f\n",
               x0 + i, biome_char(static_cast<BiomeType>(biome_at[i])),
               amp_at[i], raw_at[i], hgt[i]);
    }
    return 0;
}
