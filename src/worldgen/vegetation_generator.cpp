#include "worldgen/vegetation_generator.hpp"
#include "core/block_types.hpp"
#include <algorithm>

namespace VoxelEngine {

void VegetationGenerator::generate_vegetation(
    ChunkData& chunk,
    const ChunkGenerator::ChunkColumn (&columns)[CHUNK_WIDTH][CHUNK_DEPTH],
    int32_t chunk_x, int32_t chunk_z,
    int32_t world_y_start, int32_t world_y_end,
    const BiomeConfig& biomes,
    const VegetationConfig& veg_config,
    const CrossChunkWriter& cross_writer)
{
    // Track placed tree positions within this chunk to enforce minimum spacing
    bool tree_placed[CHUNK_WIDTH][CHUNK_DEPTH] = {};

    // Per-chunk randomness drives whether a chunk has trees and how many.
    uint32_t chunk_seed = hash_pos(chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);

    const ForestVegConfig& forest_cfg = veg_config.forest;
    const PlainsVegConfig& plains_cfg = veg_config.plains;
    const DesertVegConfig& desert_cfg = veg_config.desert;

    const bool forest_has_trees =
        (chunk_seed % 100u) < static_cast<uint32_t>(std::max(0, forest_cfg.chunk_chance_pct));
    const int32_t tree_span = std::max(1, forest_cfg.max_trees - forest_cfg.min_trees + 1);
    const uint32_t target_trees =
        static_cast<uint32_t>(std::max(0, forest_cfg.min_trees)) +
        (chunk_seed % static_cast<uint32_t>(tree_span));
    uint32_t trees_placed_count = 0;

    // Shuffle column positions so trees are placed randomly, not in scan order
    struct Cell { int32_t x; int32_t z; };
    Cell cells[CHUNK_WIDTH * CHUNK_DEPTH];
    for (int32_t i = 0; i < CHUNK_WIDTH * CHUNK_DEPTH; i++)
        cells[i] = { i / CHUNK_DEPTH, i % CHUNK_DEPTH };
    // Fisher-Yates shuffle with chunk seed
    uint32_t rng = chunk_seed;
    for (int32_t i = CHUNK_WIDTH * CHUNK_DEPTH - 1; i > 0; i--) {
        rng = rng * 1664525u + 1013904223u;
        int32_t j = static_cast<int32_t>(rng % static_cast<uint32_t>(i + 1));
        Cell tmp = cells[i];
        cells[i] = cells[j];
        cells[j] = tmp;
    }

    for (int32_t idx = 0; idx < CHUNK_WIDTH * CHUNK_DEPTH; idx++) {
        int32_t x = cells[idx].x;
        int32_t z = cells[idx].z;
            // Density-based surface (topmost air→solid transition), NOT the macro
            // heightmap — the real surface can sit above or below it now.
            int32_t surface_y = columns[x][z].surface_y;
            if (surface_y < 0)
                continue;
            // Reject submerged surfaces (e.g. dips carved below the water level).
            if (surface_y <= columns[x][z].water_level)
                continue;

            // Surface must be inside this chunk for us to place vegetation on it
            if (surface_y < world_y_start || surface_y >= world_y_end)
                continue;

            BiomeType biome = columns[x][z].biome;
            const BiomeVegetation& veg = biomes.vegetation[static_cast<size_t>(biome)];

            int32_t wx = chunk_x * CHUNK_WIDTH + x;
            int32_t wz = chunk_z * CHUNK_DEPTH + z;
            uint32_t h = hash_pos(wx, wz);

            BlockID surface_block = chunk.get_block(x, surface_y - world_y_start, z);

            if (biome == BiomeType::Forest && veg.tree_density > 0.0f) {
                if (forest_has_trees && trees_placed_count < target_trees) {
                    // Column candidate rate; spacing filter + target cap control final count
                    if ((h % 100u) < static_cast<uint32_t>(std::max(0, forest_cfg.column_chance_pct))) {
                        // Enforce minimum spacing between trees
                        const int32_t radius = std::max(0, forest_cfg.spacing_radius);
                        bool too_close = false;
                        for (int32_t dx = -radius; dx <= radius && !too_close; dx++) {
                            for (int32_t dz = -radius; dz <= radius && !too_close; dz++) {
                                int32_t nx = x + dx;
                                int32_t nz = z + dz;
                                if (nx >= 0 && nx < CHUNK_WIDTH && nz >= 0 && nz < CHUNK_DEPTH) {
                                    if (tree_placed[nx][nz]) too_close = true;
                                }
                            }
                        }
                        if (!too_close) {
                            place_tree(chunk, x, z, surface_y, world_y_start, world_y_end,
                                       h, chunk_x, chunk_z, veg, veg_config, cross_writer);
                            tree_placed[x][z] = true;
                            trees_placed_count++;
                        }
                    }
                }
                // Sparse boulders mixed into forests
                if ((h % 10000u) < static_cast<uint32_t>(std::max(0, forest_cfg.boulder_chance_per_10000))) {
                    // Enforce minimum spacing from trees
                    constexpr int32_t MIN_BOULDER_TREE_RADIUS = 2;
                    bool too_close_to_tree = false;
                    for (int32_t dx = -MIN_BOULDER_TREE_RADIUS; dx <= MIN_BOULDER_TREE_RADIUS && !too_close_to_tree; dx++) {
                        for (int32_t dz = -MIN_BOULDER_TREE_RADIUS; dz <= MIN_BOULDER_TREE_RADIUS && !too_close_to_tree; dz++) {
                            int32_t nx = x + dx;
                            int32_t nz = z + dz;
                            if (nx >= 0 && nx < CHUNK_WIDTH && nz >= 0 && nz < CHUNK_DEPTH) {
                                if (tree_placed[nx][nz]) too_close_to_tree = true;
                            }
                        }
                    }
                    if (!too_close_to_tree) {
                        const int32_t radius = std::max(1, forest_cfg.boulder_radius) + static_cast<int32_t>(h & 1u);
                        place_boulder(chunk, x, z, surface_y, world_y_start, world_y_end,
                                      h, chunk_x, chunk_z, radius, cross_writer);
                    }
                }
            } else if (biome == BiomeType::Plains && veg.tree_density > 0.0f) {
                // Per-chunk sparse tree: some plains chunks get exactly one tree.
                // Only triggers on the first column (0,0) to ensure one tree per qualifying chunk.
                if (x == 0 && z == 0) {
                    uint32_t ch = hash_pos(chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);
                    if ((ch % 100u) < static_cast<uint32_t>(std::max(0, plains_cfg.chunk_chance_pct))) {
                        // Pick a random column within the chunk for the single tree
                        int32_t tx = static_cast<int32_t>(ch >> 8) % CHUNK_WIDTH;
                        int32_t tz = static_cast<int32_t>(ch >> 16) % CHUNK_DEPTH;
                        int32_t ts = columns[tx][tz].surface_y;
                        if (ts >= world_y_start && ts < world_y_end &&
                            ts > columns[tx][tz].water_level) {
                            uint32_t th = hash_pos(chunk_x * CHUNK_WIDTH + tx, chunk_z * CHUNK_DEPTH + tz);
                            place_tree(chunk, tx, tz, ts, world_y_start, world_y_end,
                                       th, chunk_x, chunk_z, veg, veg_config, cross_writer);
                        }
                    }
                }
            } else if (biome == BiomeType::Desert && surface_block == BlockIDs::SAND) {
                if ((h % 1000u) < static_cast<uint32_t>(std::max(0, desert_cfg.cactus_chance_per_1000))) {
                    place_cactus(chunk, x, z, surface_y, world_y_start, world_y_end, desert_cfg);
                }
            }
    }
}

uint32_t VegetationGenerator::hash_pos(int32_t wx, int32_t wz) {
    uint32_t h = static_cast<uint32_t>(wx) * 374761393u + static_cast<uint32_t>(wz) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

int VegetationGenerator::pick_variant(const std::array<float, 2>& weights, uint32_t seed) {
    const float total = weights[0] + weights[1];
    if (total <= 0.0f) return 0;
    const float r = (static_cast<float>((seed >> 3) & 0xFFFFu) / 65535.0f) * total;
    if (r < weights[0]) return 0;
    return 1;
}

void VegetationGenerator::place_tree(
    ChunkData& chunk,
    int32_t local_x, int32_t local_z,
    int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
    uint32_t seed, int32_t chunk_x, int32_t chunk_z,
    const BiomeVegetation& veg, const VegetationConfig& veg_config,
    const CrossChunkWriter& cross_writer)
{
    if (pick_variant(veg.tree_variants, seed) == 1) {
        place_spruce(chunk, local_x, local_z, surface_y, world_y_start, world_y_end,
                     seed, chunk_x, chunk_z, cross_writer);
    } else {
        place_oak(chunk, local_x, local_z, surface_y, world_y_start, world_y_end,
                  seed, chunk_x, chunk_z, veg_config.tree_trunk_height, cross_writer);
    }
}

void VegetationGenerator::place_oak(
    ChunkData& chunk,
    int32_t local_x, int32_t local_z,
    int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
    uint32_t seed, int32_t chunk_x, int32_t chunk_z,
    int32_t trunk_height, const CrossChunkWriter& cross_writer)
{
    (void)seed;
    for (int32_t dy = 1; dy <= trunk_height; dy++) {
        int32_t y = surface_y + dy;
        if (y >= world_y_start && y < world_y_end) {
            chunk.set_block(local_x, y - world_y_start, local_z, BlockIDs::WOOD);
        } else if (cross_writer) {
            int32_t wx = chunk_x * CHUNK_WIDTH + local_x;
            int32_t wz = chunk_z * CHUNK_DEPTH + local_z;
            cross_writer(wx, y, wz, BlockIDs::WOOD);
        }
    }

    auto leaf = [&](int32_t dx, int32_t dz, int32_t dy) {
        int32_t lx = local_x + dx;
        int32_t lz = local_z + dz;
        int32_t ly = surface_y + dy;
        if (ly < world_y_start || ly >= world_y_end) {
            if (cross_writer) {
                int32_t wx = chunk_x * CHUNK_WIDTH + lx;
                int32_t wz = chunk_z * CHUNK_DEPTH + lz;
                cross_writer(wx, ly, wz, BlockIDs::LEAVES);
            }
            return;
        }
        if (lx >= 0 && lx < CHUNK_WIDTH && lz >= 0 && lz < CHUNK_DEPTH) {
            if (chunk.get_block(lx, ly - world_y_start, lz) == BlockIDs::AIR)
                chunk.set_block(lx, ly - world_y_start, lz, BlockIDs::LEAVES);
        } else if (cross_writer) {
            int32_t wx = chunk_x * CHUNK_WIDTH + lx;
            int32_t wz = chunk_z * CHUNK_DEPTH + lz;
            cross_writer(wx, ly, wz, BlockIDs::LEAVES);
        }
    };

    for (int32_t dy = 3; dy <= 4; dy++) {
        for (int32_t dx = -2; dx <= 2; dx++) {
            for (int32_t dz = -2; dz <= 2; dz++) {
                if (std::abs(dx) == 2 && std::abs(dz) == 2) continue;
                if (dy <= trunk_height && dx == 0 && dz == 0) continue;
                leaf(dx, dz, dy);
            }
        }
    }
    for (int32_t dy = 5; dy <= 6; dy++) {
        for (int32_t dx = -1; dx <= 1; dx++) {
            for (int32_t dz = -1; dz <= 1; dz++) {
                if (dy <= trunk_height && dx == 0 && dz == 0) continue;
                leaf(dx, dz, dy);
            }
        }
    }
}

void VegetationGenerator::place_spruce(
    ChunkData& chunk,
    int32_t local_x, int32_t local_z,
    int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
    uint32_t seed, int32_t chunk_x, int32_t chunk_z,
    const CrossChunkWriter& cross_writer)
{
    const int32_t trunk_height = 8 + static_cast<int32_t>(seed & 1u);

    for (int32_t dy = 1; dy <= trunk_height; dy++) {
        int32_t y = surface_y + dy;
        if (y >= world_y_start && y < world_y_end) {
            chunk.set_block(local_x, y - world_y_start, local_z, BlockIDs::WOOD);
        } else if (cross_writer) {
            int32_t wx = chunk_x * CHUNK_WIDTH + local_x;
            int32_t wz = chunk_z * CHUNK_DEPTH + local_z;
            cross_writer(wx, y, wz, BlockIDs::WOOD);
        }
    }

    auto leaf = [&](int32_t dx, int32_t dz, int32_t dy) {
        int32_t lx = local_x + dx;
        int32_t lz = local_z + dz;
        int32_t ly = surface_y + dy;
        if (ly < world_y_start || ly >= world_y_end) {
            if (cross_writer) {
                int32_t wx = chunk_x * CHUNK_WIDTH + lx;
                int32_t wz = chunk_z * CHUNK_DEPTH + lz;
                cross_writer(wx, ly, wz, BlockIDs::LEAVES);
            }
            return;
        }
        if (lx >= 0 && lx < CHUNK_WIDTH && lz >= 0 && lz < CHUNK_DEPTH) {
            if (chunk.get_block(lx, ly - world_y_start, lz) == BlockIDs::AIR)
                chunk.set_block(lx, ly - world_y_start, lz, BlockIDs::LEAVES);
        } else if (cross_writer) {
            int32_t wx = chunk_x * CHUNK_WIDTH + lx;
            int32_t wz = chunk_z * CHUNK_DEPTH + lz;
            cross_writer(wx, ly, wz, BlockIDs::LEAVES);
        }
    };

    // Conical rings tapering toward the trunk top, then a leaf spike.
    for (int32_t dy = trunk_height - 2; dy <= trunk_height; dy++) {
        const int32_t spread = (dy == trunk_height - 2) ? 2 : 1;
        for (int32_t dx = -spread; dx <= spread; dx++) {
            for (int32_t dz = -spread; dz <= spread; dz++) {
                if (spread == 2 && std::abs(dx) == 2 && std::abs(dz) == 2) continue;
                if (dy <= trunk_height && dx == 0 && dz == 0) continue;
                leaf(dx, dz, dy);
            }
        }
    }
    leaf(0, 0, trunk_height + 1);
}

void VegetationGenerator::place_cactus(
    ChunkData& chunk, int32_t local_x, int32_t local_z,
    int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
    const DesertVegConfig& cfg)
{
    const int32_t span = std::max(1, cfg.max_height - cfg.min_height + 1);
    const int32_t height = cfg.min_height + static_cast<int32_t>(
        hash_pos(local_x * 7 + 13, local_z * 11 + 7) % static_cast<uint32_t>(span));
    for (int32_t dy = 1; dy <= height; dy++) {
        int32_t y = surface_y + dy;
        if (y >= world_y_start && y < world_y_end) {
            chunk.set_block(local_x, y - world_y_start, local_z, BlockIDs::CACTUS);
        }
    }
}

void VegetationGenerator::place_boulder(
    ChunkData& chunk, int32_t local_x, int32_t local_z,
    int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
    uint32_t seed, int32_t chunk_x, int32_t chunk_z,
    int32_t radius, const CrossChunkWriter& cross_writer)
{
    auto place_block = [&](int32_t dx, int32_t dz, int32_t dy) {
        int32_t lx = local_x + dx;
        int32_t lz = local_z + dz;
        int32_t ly = surface_y + dy;

        if (ly < world_y_start || ly >= world_y_end) {
            if (cross_writer) {
                int32_t wx = chunk_x * CHUNK_WIDTH + lx;
                int32_t wz = chunk_z * CHUNK_DEPTH + lz;
                cross_writer(wx, ly, wz, BlockIDs::STONE);
            }
            return;
        }
        if (lx >= 0 && lx < CHUNK_WIDTH && lz >= 0 && lz < CHUNK_DEPTH) {
            if (chunk.get_block(lx, ly - world_y_start, lz) == BlockIDs::AIR)
                chunk.set_block(lx, ly - world_y_start, lz, BlockIDs::STONE);
        } else if (cross_writer) {
            int32_t wx = chunk_x * CHUNK_WIDTH + lx;
            int32_t wz = chunk_z * CHUNK_DEPTH + lz;
            cross_writer(wx, ly, wz, BlockIDs::STONE);
        }
    };

    // Place boulder in slightly irregular spherical shape
    for (int32_t dy = -radius; dy <= radius; dy++) {
        for (int32_t dx = -radius; dx <= radius; dx++) {
            for (int32_t dz = -radius; dz <= radius; dz++) {
                int32_t dist_sq = dx*dx + dy*dy + dz*dz;
                // Add slight irregularity using seed-based noise (only subtracts blocks, never adds)
                float noise_offset = (static_cast<float>((seed >> (dx + dz + 3)) & 7u)) * 0.15f;
                if (dist_sq <= radius * radius - noise_offset * radius) {
                    place_block(dx, dz, dy);
                }
            }
        }
    }
}

} // namespace VoxelEngine
