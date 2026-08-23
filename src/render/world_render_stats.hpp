#ifndef FARLANDS_WORLD_RENDER_STATS_HPP
#define FARLANDS_WORLD_RENDER_STATS_HPP
#include <cstdint>

namespace VoxelEngine {

struct WorldRenderStats {
    int32_t visible_instances = 0;
    int32_t mesh_rids = 0;
    int32_t chunk_instances = 0;
    int32_t far_region_instances = 0;
    int32_t chunk_mesh_rids = 0;
    int32_t far_region_mesh_rids = 0;
    int32_t eligible_far_chunks = 0;
    int32_t cached_far_chunks = 0;
    int32_t active_region_member_chunks = 0;
    int32_t regions_partial_missing_cache = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_RENDER_STATS_HPP
