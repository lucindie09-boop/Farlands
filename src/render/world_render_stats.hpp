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

    // Chunks whose data holds liquid while the GPU holds no liquid geometry for
    // them. Should be 0 at all times; a non-zero value means water went missing
    // between the data and the screen (each such chunk is remeshed once to
    // repair it, which is what liquid_geometry_repairs counts).
    int32_t chunks_with_liquid_but_no_water_mesh = 0;
    int32_t liquid_geometry_repairs = 0;
    // Chunks that have geometry on the GPU, are inside the render distance and are
    // not covered by a far region, yet have no instance to draw them. Should be 0.
    int32_t chunks_with_geometry_but_no_instance = 0;

    // Upload bookkeeping since the process started, and the one number that says
    // whether water is being hidden by the upload dedup: a skip that dropped a
    // water mesh the build had changed. See MeshManager::mesh_uploads.
    int32_t mesh_uploads = 0;
    int32_t mesh_upload_dedup_skips = 0;
    int32_t mesh_upload_swallowed_water_changes = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_RENDER_STATS_HPP
