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
    const BiomeAmplification& amp = amplification_for(land_biome, blended);
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

void ChunkGenerator::build_climate_lattice(int32_t chunk_x, int32_t chunk_z,
                                           float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                           float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t wx0 = chunk_x * CHUNK_WIDTH;
    const int32_t wz0 = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
            const float x = static_cast<float>(wx0 + i * CLIMATE_LATTICE_SPACING);
            const float z = static_cast<float>(wz0 + j * CLIMATE_LATTICE_SPACING);
            temp_lat[i][j] = sample_temperature_raw(x, z);
            hum_lat[i][j] = sample_humidity_raw(x, z);
        }
    }
}

void ChunkGenerator::build_land_shape_lattice(int32_t chunk_x, int32_t chunk_z,
                                              float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t wx0 = chunk_x * CHUNK_WIDTH;
    const int32_t wz0 = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
            const float x = static_cast<float>(wx0 + i * CLIMATE_LATTICE_SPACING);
            const float z = static_cast<float>(wz0 + j * CLIMATE_LATTICE_SPACING);
            land_lat[i][j] = sample_land_shape_raw(x, z);
        }
    }
}

float ChunkGenerator::interp_climate_lattice(const float lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                             int32_t wx, int32_t wz, int32_t wx0, int32_t wz0) {
    const int32_t ix = (wx - wx0) / CLIMATE_LATTICE_SPACING;
    const int32_t iz = (wz - wz0) / CLIMATE_LATTICE_SPACING;
    const float fx = static_cast<float>(wx - wx0 - ix * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float fz = static_cast<float>(wz - wz0 - iz * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float v00 = lat[ix][iz],     v10 = lat[ix + 1][iz];
    const float v01 = lat[ix][iz + 1], v11 = lat[ix + 1][iz + 1];
    return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
}

void ChunkGenerator::build_amp_lattice(int32_t chunk_x, int32_t chunk_z,
                                       const float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                       const float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                       BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const {
    const int32_t world_x_start = chunk_x * CHUNK_WIDTH;
    const int32_t world_z_start = chunk_z * CHUNK_DEPTH;

    // Blend windows extend up to climate_blend_radius_nodes nodes beyond the
    // chunk; biome values inside come from the cached climate lattice, just
    // outside from the raw samplers (bit-identical node values).
    const int32_t R = std::min(std::max(params.climate_blend_radius_nodes, 0),
                               CLIMATE_BLEND_MAX_RADIUS);
    const int32_t EXT = CLIMATE_LATTICE_NODES + 2 * R;
    constexpr int32_t MAX_EXT = 2 * CLIMATE_BLEND_MAX_RADIUS + CLIMATE_LATTICE_NODES;
    BiomeType bio_grid[MAX_EXT][MAX_EXT];
    for (int32_t j = 0; j < EXT; ++j) {
        for (int32_t i = 0; i < EXT; ++i) {
            const int32_t nxi = i - R;   // node index, 0..8 inside the chunk
            const int32_t nzj = j - R;
            const int32_t wx = world_x_start + nxi * CLIMATE_LATTICE_SPACING;
            const int32_t wz = world_z_start + nzj * CLIMATE_LATTICE_SPACING;
            if (nxi >= 0 && nxi < CLIMATE_LATTICE_NODES &&
                nzj >= 0 && nzj < CLIMATE_LATTICE_NODES) {
                bio_grid[i][j] = biome_from_climate(
                    temp_lat[nxi][nzj], hum_lat[nxi][nzj],
                    sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
            } else {
                bio_grid[i][j] = biome_from_climate(
                    sample_temperature_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    sample_humidity_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
            }
        }
    }

    // Per-node window means via a separable 2D prefix sum over the three
    // knob channels: each (2R+1)^2 window is then O(1) (4 corner reads)
    // instead of O((2R+1)^2), so raising the radius no longer multiplies the
    // per-chunk window work. Values agree with the single-point path to
    // within float rounding (same node values, different summation order).
    // The bio grid is indexed [i][j] = [x][z], so prefixes are laid out
    // x-major to match the window reads.
    if (R == 0) {
        // Radius 0 = the node's own biome exactly (no prefix arithmetic).
        for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
            for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
                const size_t ix = static_cast<size_t>(bio_grid[i + R][j + R]);
                amp_lat[i][j] = biome_config.amplification[ix];
            }
        }
        return;
    }

    const int32_t W = EXT + 1;
    const int32_t window = 2 * R + 1;
    const double inv_window = 1.0 / (static_cast<double>(window) * static_cast<double>(window));
    std::vector<double> ph(static_cast<size_t>(W) * W, 0.0);
    std::vector<double> pw(static_cast<size_t>(W) * W, 0.0);
    std::vector<double> pm(static_cast<size_t>(W) * W, 0.0);
    std::vector<double> ps(static_cast<size_t>(W) * W, 0.0);
    for (int32_t j = 0; j < EXT; ++j) {
        for (int32_t i = 0; i < EXT; ++i) {
            const BiomeAmplification& a =
                biome_config.amplification[static_cast<size_t>(bio_grid[i][j])];
            const size_t idx = static_cast<size_t>(j + 1) * W + static_cast<size_t>(i + 1);
            ph[idx] = ph[static_cast<size_t>(j) * W + static_cast<size_t>(i + 1)]
                    + ph[static_cast<size_t>(j + 1) * W + static_cast<size_t>(i)]
                    - ph[static_cast<size_t>(j) * W + static_cast<size_t>(i)]
                    + static_cast<double>(a.height);
            pw[idx] = pw[static_cast<size_t>(j) * W + static_cast<size_t>(i + 1)]
                    + pw[static_cast<size_t>(j + 1) * W + static_cast<size_t>(i)]
                    - pw[static_cast<size_t>(j) * W + static_cast<size_t>(i)]
                    + static_cast<double>(a.weirdness);
            pm[idx] = pm[static_cast<size_t>(j) * W + static_cast<size_t>(i + 1)]
                    + pm[static_cast<size_t>(j + 1) * W + static_cast<size_t>(i)]
                    - pm[static_cast<size_t>(j) * W + static_cast<size_t>(i)]
                    + static_cast<double>(a.min_weirdness);
            ps[idx] = ps[static_cast<size_t>(j) * W + static_cast<size_t>(i + 1)]
                    + ps[static_cast<size_t>(j + 1) * W + static_cast<size_t>(i)]
                    - ps[static_cast<size_t>(j) * W + static_cast<size_t>(i)]
                    + static_cast<double>(a.weirdness_size);
        }
    }
    for (int32_t j = 0; j < CLIMATE_LATTICE_NODES; ++j) {
        for (int32_t i = 0; i < CLIMATE_LATTICE_NODES; ++i) {
            const size_t x1 = static_cast<size_t>(i);
            const size_t y1 = static_cast<size_t>(j);
            const size_t x2 = x1 + static_cast<size_t>(window);
            const size_t y2 = y1 + static_cast<size_t>(window);
            const auto at = [&](size_t x, size_t y) {
                return y * static_cast<size_t>(W) + x;
            };
            const double sh = ph[at(x2, y2)] - ph[at(x1, y2)] - ph[at(x2, y1)] + ph[at(x1, y1)];
            const double sw = pw[at(x2, y2)] - pw[at(x1, y2)] - pw[at(x2, y1)] + pw[at(x1, y1)];
            const double sm = pm[at(x2, y2)] - pm[at(x1, y2)] - pm[at(x2, y1)] + pm[at(x1, y1)];
            const double ss = ps[at(x2, y2)] - ps[at(x1, y2)] - ps[at(x2, y1)] + ps[at(x1, y1)];
            amp_lat[i][j].height     = static_cast<float>(sh * inv_window);
            amp_lat[i][j].weirdness  = static_cast<float>(sw * inv_window);
            amp_lat[i][j].min_weirdness = static_cast<float>(sm * inv_window);
            amp_lat[i][j].weirdness_size = static_cast<float>(ss * inv_window);
        }
    }
}

BiomeAmplification ChunkGenerator::interp_amp_lattice(
        const BiomeAmplification lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
        int32_t wx, int32_t wz, int32_t wx0, int32_t wz0) {
    const int32_t ix = (wx - wx0) / CLIMATE_LATTICE_SPACING;
    const int32_t iz = (wz - wz0) / CLIMATE_LATTICE_SPACING;
    const float fx = static_cast<float>(wx - wx0 - ix * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const float fz = static_cast<float>(wz - wz0 - iz * CLIMATE_LATTICE_SPACING) /
                     static_cast<float>(CLIMATE_LATTICE_SPACING);
    const BiomeAmplification& v00 = lat[ix][iz];
    const BiomeAmplification& v10 = lat[ix + 1][iz];
    const BiomeAmplification& v01 = lat[ix][iz + 1];
    const BiomeAmplification& v11 = lat[ix + 1][iz + 1];
    BiomeAmplification out;
    out.height = lerp(lerp(v00.height, v10.height, fx), lerp(v01.height, v11.height, fx), fz);
    out.weirdness = lerp(lerp(v00.weirdness, v10.weirdness, fx),
                         lerp(v01.weirdness, v11.weirdness, fx), fz);
    out.min_weirdness = lerp(lerp(v00.min_weirdness, v10.min_weirdness, fx),
                             lerp(v01.min_weirdness, v11.min_weirdness, fx), fz);
    out.weirdness_size = lerp(lerp(v00.weirdness_size, v10.weirdness_size, fx),
                              lerp(v01.weirdness_size, v11.weirdness_size, fx), fz);
    return out;
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
