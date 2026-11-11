// The post-density cleanup half of chunk generation: three passes that repair
// what the density field and the material pass leave behind, all of them
// reading the signed density for classification and writing only the dense
// block buffer. Moved verbatim out of generate_chunk() so that file keeps its
// headroom under the 500-line cap; the entry point there is a single call.

#include "worldgen/chunk_generator_cleanup.hpp"

#include "core/chunk_types.hpp"

#include <algorithm>

namespace VoxelEngine {

void cleanup_generated_columns(BlockID* dense_buffer,
                               const float* density_buffer,
                               int32_t density_stride_y,
                               int32_t world_y_start,
                               int32_t top_wy,
                               float sea_level,
                               const ChunkGenerator::ChunkColumn (*columns)[CHUNK_DEPTH]) {
    auto dens = [&](int32_t x, int32_t ly, int32_t z) -> const float& {
        return density_buffer[(static_cast<size_t>(x) * density_stride_y + ly) * CHUNK_DEPTH + z];
    };
    auto dense = [&](int32_t x, int32_t y, int32_t z) -> BlockID& {
        return dense_buffer[static_cast<size_t>(x) + static_cast<size_t>(y) * CHUNK_WIDTH +
                            static_cast<size_t>(z) * CHUNK_WIDTH * CHUNK_HEIGHT];
    };

    // Remove isolated single-block artifacts. A non-air voxel whose six
    // orthogonal neighbors are all air is a density-noise glitch (a lone
    // floating cube). Only interior blocks are touched — boundary-layer
    // neighbors may live in adjacent, not-yet-generated chunks.
    // Optimized: work directly on the dense buffer using dens() for detection.
    // Positive density = solid, negative density = air.
    for (int32_t ly = 1; ly < std::min(CHUNK_HEIGHT - 1, top_wy - world_y_start); ly++) {
        for (int32_t x = 1; x < CHUNK_WIDTH - 1; x++) {
            for (int32_t z = 1; z < CHUNK_DEPTH - 1; z++) {
                if (dens(x, ly, z) <= 0.0f) continue;  // Skip air voxels
                if (dens(x + 1, ly, z) > 0.0f) continue;
                if (dens(x - 1, ly, z) > 0.0f) continue;
                if (dens(x, ly + 1, z) > 0.0f) continue;
                if (dens(x, ly - 1, z) > 0.0f) continue;
                if (dens(x, ly, z + 1) > 0.0f) continue;
                if (dens(x, ly, z - 1) > 0.0f) continue;
                dense(x, ly, z) = BlockIDs::AIR;
            }
        }
    }

    // Replace thin solid noise artifacts within water columns.  Near the macro
    // surface the 3D shape noise (density_from_shape) can flip a single voxel
    // to positive density while both Y-neighbours stay negative, creating a
    // 1-block-thick solid sheet inside what should be water.  Any solid voxel
    // with non-solid density both above AND below, and whose world Y is below
    // the column's water surface, is replaced with water.
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkGenerator::ChunkColumn& col = columns[x][z];
            const int32_t wt = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < sea_level
                   ? static_cast<int32_t>(sea_level) : -1);
            if (wt < 0) continue;
            for (int32_t ly = 1; ly < CHUNK_HEIGHT - 1; ly++) {
                const int32_t wy = world_y_start + ly;
                if (wy >= wt) break;
                if (dens(x, ly, z) <= 0.0f) continue;
                if (dens(x, ly + 1, z) > 0.0f) continue;
                if (dens(x, ly - 1, z) > 0.0f) continue;
                dense(x, ly, z) = BlockIDs::SURFACE_WATER;
            }
        }
    }

    // Flood-fill water gaps below the water surface.  After the thin-layer
    // cleanup above, some AIR voxels may remain below water_top — e.g. when
    // a noise spike raised surface_y during the material pass, leaving AIR
    // voxels at the chunk bottom that the water fill missed.  Scan each water
    // column from water_top downward: any AIR becomes WATER until we hit solid
    // terrain.  Enclosed caves (surrounded by solid above) stay dry because the
    // scan stops at the first solid block.
    for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
            const ChunkGenerator::ChunkColumn& col = columns[x][z];
            const int32_t wt = col.water_level >= 0
                ? col.water_level
                : (col.surface_y >= 0 && col.surface_y < sea_level
                   ? static_cast<int32_t>(sea_level) : -1);
            if (wt < 0) continue;
            for (int32_t ly = CHUNK_HEIGHT - 1; ly >= 0; ly--) {
                const int32_t wy = world_y_start + ly;
                if (wy >= wt) continue;
                BlockID& bid = dense(x, ly, z);
                if (bid == BlockIDs::AIR) {
                    bid = BlockIDs::SURFACE_WATER;
                } else if (bid != BlockIDs::WATER && bid != BlockIDs::SURFACE_WATER) {
                    break;
                }
            }
        }
    }
}

} // namespace VoxelEngine
