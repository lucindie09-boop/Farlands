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
    const HillsVegConfig& hills_cfg = veg_config.hills;

    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
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

            const BiomeVegetation& veg = biomes.vegetation[static_cast<size_t>(columns[x][z].biome)];

            // Any biome whose tree_density is nonzero can carry sparse trees;
            // the per-biome density scales the shared per-chunk chance (e.g.
            // Hills 1.0 keeps the historical 25%; Plains 0.3 -> ~7%).
            if (veg.tree_density > 0.0f) {
                // Per-chunk sparse tree: some qualifying chunks get exactly one
                // tree. Only triggers on the first column (0,0) to ensure one
                // tree per qualifying chunk.
                if (x == 0 && z == 0) {
                    uint32_t ch = hash_pos(chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);
                    const uint32_t chance_pct = static_cast<uint32_t>(
                        std::max(0.0f, std::min(1.0f, veg.tree_density)) *
                        static_cast<float>(hills_cfg.chunk_chance_pct));
                    if ((ch % 100u) < chance_pct) {
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

} // namespace VoxelEngine
