// The lattice cache a chunk is generated from: the climate fields, the macro land
// shape and the blended amplification knobs of one chunk, each sampled once per
// 4-block node (9x9 over a 32-block chunk) and read back per column.
//
// The node values come from ChunkGenerator's own samplers, which is what makes the
// cache and the per-call path agree: a chunk edge meets its neighbour's queries
// without a seam, and the tools that cross-check one against the other compare
// bit-identical values (see the *_lattice_debug accessors).
//
// The builders were ChunkGenerator members taking the node arrays as parameters;
// they are methods here now, reading the generator they are handed.

#include "worldgen/chunk_generator_lattice.hpp"

#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace VoxelEngine {

struct ChunkGeneratorLattice::Node ChunkGeneratorLattice::node_for(int32_t world_x,
                                                                  int32_t world_z) const {
    const int32_t ix = (world_x - origin_x_) / SPACING;
    const int32_t iz = (world_z - origin_z_) / SPACING;
    Node node;
    node.ix = ix;
    node.iz = iz;
    node.fx = static_cast<float>(world_x - origin_x_ - ix * SPACING) / static_cast<float>(SPACING);
    node.fz = static_cast<float>(world_z - origin_z_ - iz * SPACING) / static_cast<float>(SPACING);
    return node;
}

float ChunkGeneratorLattice::temperature(int32_t world_x, int32_t world_z) const {
    const Node n = node_for(world_x, world_z);
    const float v00 = temp_[n.ix][n.iz],     v10 = temp_[n.ix + 1][n.iz];
    const float v01 = temp_[n.ix][n.iz + 1], v11 = temp_[n.ix + 1][n.iz + 1];
    return ChunkGenerator::lerp(ChunkGenerator::lerp(v00, v10, n.fx),
                                ChunkGenerator::lerp(v01, v11, n.fx), n.fz);
}

float ChunkGeneratorLattice::humidity(int32_t world_x, int32_t world_z) const {
    const Node n = node_for(world_x, world_z);
    const float v00 = hum_[n.ix][n.iz],     v10 = hum_[n.ix + 1][n.iz];
    const float v01 = hum_[n.ix][n.iz + 1], v11 = hum_[n.ix + 1][n.iz + 1];
    return ChunkGenerator::lerp(ChunkGenerator::lerp(v00, v10, n.fx),
                                ChunkGenerator::lerp(v01, v11, n.fx), n.fz);
}

float ChunkGeneratorLattice::land_shape(int32_t world_x, int32_t world_z) const {
    const Node n = node_for(world_x, world_z);
    const float v00 = land_[n.ix][n.iz],     v10 = land_[n.ix + 1][n.iz];
    const float v01 = land_[n.ix][n.iz + 1], v11 = land_[n.ix + 1][n.iz + 1];
    return ChunkGenerator::lerp(ChunkGenerator::lerp(v00, v10, n.fx),
                                ChunkGenerator::lerp(v01, v11, n.fx), n.fz);
}

BiomeAmplification ChunkGeneratorLattice::amplification(int32_t world_x, int32_t world_z) const {
    const Node n = node_for(world_x, world_z);
    const BiomeAmplification& v00 = amp_[n.ix][n.iz];
    const BiomeAmplification& v10 = amp_[n.ix + 1][n.iz];
    const BiomeAmplification& v01 = amp_[n.ix][n.iz + 1];
    const BiomeAmplification& v11 = amp_[n.ix + 1][n.iz + 1];
    BiomeAmplification out;
    out.height = ChunkGenerator::lerp(ChunkGenerator::lerp(v00.height, v10.height, n.fx),
                                      ChunkGenerator::lerp(v01.height, v11.height, n.fx), n.fz);
    out.weirdness = ChunkGenerator::lerp(ChunkGenerator::lerp(v00.weirdness, v10.weirdness, n.fx),
                                         ChunkGenerator::lerp(v01.weirdness, v11.weirdness, n.fx),
                                         n.fz);
    out.min_weirdness =
        ChunkGenerator::lerp(ChunkGenerator::lerp(v00.min_weirdness, v10.min_weirdness, n.fx),
                             ChunkGenerator::lerp(v01.min_weirdness, v11.min_weirdness, n.fx),
                             n.fz);
    out.weirdness_size =
        ChunkGenerator::lerp(ChunkGenerator::lerp(v00.weirdness_size, v10.weirdness_size, n.fx),
                             ChunkGenerator::lerp(v01.weirdness_size, v11.weirdness_size, n.fx),
                             n.fz);
    return out;
}

void ChunkGeneratorLattice::build_climate(const ChunkGenerator& gen, int32_t chunk_x,
                                          int32_t chunk_z) {
    origin_x_ = chunk_x * CHUNK_WIDTH;
    origin_z_ = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < NODES; ++i) {
        for (int32_t j = 0; j < NODES; ++j) {
            const float x = static_cast<float>(origin_x_ + i * SPACING);
            const float z = static_cast<float>(origin_z_ + j * SPACING);
            temp_[i][j] = gen.sample_temperature_raw(x, z);
            hum_[i][j] = gen.sample_humidity_raw(x, z);
        }
    }
}

void ChunkGeneratorLattice::build_land_shape(const ChunkGenerator& gen, int32_t chunk_x,
                                             int32_t chunk_z) {
    origin_x_ = chunk_x * CHUNK_WIDTH;
    origin_z_ = chunk_z * CHUNK_DEPTH;
    for (int32_t i = 0; i < NODES; ++i) {
        for (int32_t j = 0; j < NODES; ++j) {
            const float x = static_cast<float>(origin_x_ + i * SPACING);
            const float z = static_cast<float>(origin_z_ + j * SPACING);
            land_[i][j] = gen.sample_land_shape_raw(x, z);
        }
    }
}

void ChunkGeneratorLattice::build_amplification(const ChunkGenerator& gen, int32_t chunk_x,
                                                int32_t chunk_z) {
    origin_x_ = chunk_x * CHUNK_WIDTH;
    origin_z_ = chunk_z * CHUNK_DEPTH;

    // Blend windows extend up to climate_blend_radius_nodes nodes beyond the
    // chunk; biome values inside come from the cached climate lattice, just
    // outside from the raw samplers (bit-identical node values).
    const int32_t R = std::min(std::max(gen.params.climate_blend_radius_nodes, 0),
                               ChunkGenerator::CLIMATE_BLEND_MAX_RADIUS);
    const int32_t EXT = NODES + 2 * R;
    constexpr int32_t MAX_EXT = 2 * ChunkGenerator::CLIMATE_BLEND_MAX_RADIUS + NODES;
    BiomeType bio_grid[MAX_EXT][MAX_EXT];
    for (int32_t j = 0; j < EXT; ++j) {
        for (int32_t i = 0; i < EXT; ++i) {
            const int32_t nxi = i - R;   // node index, 0..8 inside the chunk
            const int32_t nzj = j - R;
            const int32_t wx = origin_x_ + nxi * SPACING;
            const int32_t wz = origin_z_ + nzj * SPACING;
            if (nxi >= 0 && nxi < NODES &&
                nzj >= 0 && nzj < NODES) {
                bio_grid[i][j] = gen.biome_from_climate(
                    temp_[nxi][nzj], hum_[nxi][nzj],
                    gen.sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
            } else {
                bio_grid[i][j] = gen.biome_from_climate(
                    gen.sample_temperature_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    gen.sample_humidity_raw(static_cast<float>(wx), static_cast<float>(wz)),
                    gen.sample_continentalness(static_cast<float>(wx), static_cast<float>(wz)));
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
        for (int32_t j = 0; j < NODES; ++j) {
            for (int32_t i = 0; i < NODES; ++i) {
                const size_t ix = static_cast<size_t>(bio_grid[i + R][j + R]);
                amp_[i][j] = gen.biome_config.amplification[ix];
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
                gen.biome_config.amplification[static_cast<size_t>(bio_grid[i][j])];
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
    for (int32_t j = 0; j < NODES; ++j) {
        for (int32_t i = 0; i < NODES; ++i) {
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
            amp_[i][j].height     = static_cast<float>(sh * inv_window);
            amp_[i][j].weirdness  = static_cast<float>(sw * inv_window);
            amp_[i][j].min_weirdness = static_cast<float>(sm * inv_window);
            amp_[i][j].weirdness_size = static_cast<float>(ss * inv_window);
        }
    }
}

} // namespace VoxelEngine
