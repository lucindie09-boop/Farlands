// What a column is: the biome search, the per-column samplers, the chunk-scoped
// climate and amplification lattices they interpolate over, and the surface /
// subsurface material rules. The chunk writer is in chunk_generator.cpp and the
// cheap height and solidity queries are in chunk_generator_terrain.cpp.

#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace VoxelEngine {

bool ChunkGenerator::find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                                        int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                                        float& out_height) const {
    // Phase 1: concentric Chebyshev rings at 16-block step, walked from a side
    // midpoint so hits come back roughly nearest-first. Early-exit on the first
    // matching ring.
    constexpr int32_t STEP = 16;
    const int32_t max_cells = std::max(0, max_radius_blocks / STEP);
    bool found = false;
    int32_t cand_x = center_x;
    int32_t cand_z = center_z;

    auto match = [&](int32_t wx, int32_t wz) -> bool {
        if (get_biome(wx, wz) != target) return false;
        found = true;
        cand_x = wx;
        cand_z = wz;
        return true;
    };

    for (int32_t r = 0; r <= max_cells && !found; ++r) {
        if (r == 0) {
            match(center_x, center_z);
            continue;
        }
        for (int32_t i = -r; i <= r && !found; ++i) {
            match(center_x + i * STEP, center_z - r * STEP);
            match(center_x + i * STEP, center_z + r * STEP);
        }
        for (int32_t j = -r + 1; j <= r - 1 && !found; ++j) {
            match(center_x - r * STEP, center_z + j * STEP);
            match(center_x + r * STEP, center_z + j * STEP);
        }
    }
    if (!found) return false;

    // Phase 2: fine 4-block scan of the area around the coarse hit, keeping the
    // closest matching column to the requested center.
    constexpr int32_t FINE = 4;
    constexpr int32_t FINE_SPAN = 16;  // blocks on each side of the coarse hit
    int64_t best_dist2 = std::numeric_limits<int64_t>::max();
    int32_t best_x = cand_x;
    int32_t best_z = cand_z;
    for (int32_t wx = cand_x - FINE_SPAN; wx <= cand_x + FINE_SPAN; wx += FINE) {
        for (int32_t wz = cand_z - FINE_SPAN; wz <= cand_z + FINE_SPAN; wz += FINE) {
            if (get_biome(wx, wz) != target) continue;
            const int64_t dx = static_cast<int64_t>(wx) - center_x;
            const int64_t dz = static_cast<int64_t>(wz) - center_z;
            const int64_t d2 = dx * dx + dz * dz;
            if (d2 < best_dist2) {
                best_dist2 = d2;
                best_x = wx;
                best_z = wz;
            }
        }
    }

    out_x = best_x;
    out_z = best_z;
    out_height = sample_column(best_x, best_z).height;
    return true;
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column(int32_t world_x, int32_t world_z) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float t = sample_temperature(x, z);
    const float h = sample_humidity(x, z);
    return sample_column_with_climate(world_x, world_z, t, h,
                                      sample_land_shape(x, z, t, h),
                                      params.climate_blend_radius_nodes > 0
                                          ? blend_amplification_at(world_x, world_z)
                                          : BiomeAmplification{});
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column_with_climate(
        int32_t world_x, int32_t world_z, float temperature, float humidity,
        float land_height, const BiomeAmplification& blended) const {
    float x = static_cast<float>(world_x);
    float z = static_cast<float>(world_z);

    const float cont = sample_continentalness(x, z);
    const float saved_land_height = land_height;

    // Biomes are selected and shape the terrain FIRST: the climate grid picks
    // a land biome everywhere (ocean never enters the grid), and the column's
    // height is that biome's amplification of the macro surface around sea
    // level (1.0 = neutral; the blended field ramps across borders when
    // blending is enabled). Oceans are the LAST stage below.
    const BiomeType land_biome = biome_from_climate(temperature, humidity, cont);
    const BiomeAmplification amp = amplification_for(land_biome, blended);
    float height = params.sea_level + (land_height - params.sea_level) * amp.height;
    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);

    // Oceans are the last step in the generation scheme: any column whose
    // terrain, after the land biome altered it, still sits below sea level
    // becomes an ocean biome — water fills up to sea level and the biome
    // switches to the ocean set (water + sand surfaces). The sea bed keeps
    // the height the land biome gave it, so the floor is shaped by the
    // climate: temperate (plains) basins shelf out shallow, cold/hot (hills)
    // basins drop steeply.
    const bool ocean = height < params.sea_level;
    const BiomeType biome = ocean ? BiomeType::Ocean : land_biome;
    const float water_level = ocean ? params.sea_level : -1.0f;

    return ColumnSample{biome, height, water_level, false, saved_land_height, cont, temperature, humidity};
}


BlockID ChunkGenerator::get_surface_block(BiomeType biome, int32_t y, bool has_surface_water, bool near_water) const {
    if (has_surface_water) {
        return biome_config.underwater_surface;
    }
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_surface : s.surface;
}

BlockID ChunkGenerator::get_subsurface_block(BiomeType biome, bool near_water) const {
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_subsurface : s.subsurface;
}

} // namespace VoxelEngine
