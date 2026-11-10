// Tail of the completion drain: cross-boundary vegetation remeshes and the
// placement-queue prune, split out of chunk_world.cpp to stay under the cap.
#include "world/chunk_world.hpp"

#include "mesh/mesh_manager.hpp"

#include <cstdlib>
#include <mutex>
#include <vector>

namespace VoxelEngine {

void ChunkWorld::drain_cross_boundary_and_prune(int32_t player_cx, int32_t player_cy, int32_t player_cz, int32_t render_distance) {
    // Process cross-boundary vegetation modifications (neighbor chunks that need re-mesh).
    // Vegetation overflow is NOT persisted - it's regenerated on each load.
    for (const auto& pos : pending_cross_boundary_remesh.take_all()) {
        uint64_t key = chunk_map.get_chunk_key(pos.x, pos.y, pos.z);
        ChunkRenderData* rd = chunk_map.get_chunk_render_data(pos.x, pos.y, pos.z);
        if (rd && rd->data) {
            apply_vegetation_placements(key, pos.x, pos.y, pos.z, *rd);
        }
        if (mesh_manager) {
            mesh_manager->queue_dirty_chunk(pos.x, pos.y, pos.z);
        }
    }

    // Prune pending_block_placements for chunks beyond render distance.
    // Cross-chunk vegetation writes to never-generated neighbor chunks
    // would otherwise accumulate forever (but they're not persisted, so
    // this is purely a memory/queue-size concern, not disk growth).
    if (player_cx != INT32_MIN && player_cy != INT32_MIN && player_cz != INT32_MIN) {
        const int32_t hrd_sq = (render_distance + 1) * (render_distance + 1);
        const int32_t vertical_limit = 128;
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        for (auto it = pending_block_placements.begin(); it != pending_block_placements.end(); ) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(it->first, cx, cy, cz);
            const int32_t dx = cx - player_cx;
            const int32_t dy = cy - player_cy;
            const int32_t dz = cz - player_cz;
            if (dx * dx + dz * dz > hrd_sq || std::abs(dy) > vertical_limit) {
                it = pending_block_placements.erase(it);
            } else {
                ++it;
            }
        }

        // Also prune vegetation placements
        std::lock_guard<std::mutex> vlock(vegetation_placement_mutex);
        for (auto it = pending_vegetation_placements.begin(); it != pending_vegetation_placements.end(); ) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(it->first, cx, cy, cz);
            const int32_t dx = cx - player_cx;
            const int32_t dy = cy - player_cy;
            const int32_t dz = cz - player_cz;
            if (dx * dx + dz * dz > hrd_sq || std::abs(dy) > vertical_limit) {
                it = pending_vegetation_placements.erase(it);
            } else {
                ++it;
            }
        }
    }

}

} // namespace VoxelEngine
