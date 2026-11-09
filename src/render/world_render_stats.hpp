#ifndef FARLANDS_WORLD_RENDER_STATS_HPP
#define FARLANDS_WORLD_RENDER_STATS_HPP
#include <cstdint>

namespace VoxelEngine {

struct WorldRenderStats {
    int32_t visible_instances = 0;
    int32_t mesh_rids = 0;
    int32_t chunk_instances = 0;
    int32_t far_region_instances = 0;
    // Actual RenderingServer visibility, not the logical "not covered by a
    // region" count above. A chunk can have an instance that is hidden, and a
    // chunk can be both logically visible and still covered by an old region
    // mesh during a handoff; the two must be distinguishable in a draw spike.
    int32_t visible_chunk_instances_actual = 0;
    int32_t visible_chunk_full = 0;
    int32_t visible_chunk_mid = 0;
    int32_t visible_chunk_far = 0;
    int32_t visible_chunk_detail_unknown = 0;
    int32_t visible_chunk_region_overlap = 0;
    int32_t visible_chunk_detail_mismatch = 0;
    int32_t region_member_full = 0;
    int32_t region_member_mid = 0;
    int32_t region_member_far = 0;
    int32_t region_member_unknown = 0;
    // Why an individually drawn far-detail chunk is not covered by a region.
    int32_t visible_far_region_keys = 0;
    int32_t visible_far_no_region = 0;
    int32_t visible_far_inactive_region = 0;
    int32_t visible_far_active_uncovered = 0;
    int32_t visible_far_pending_region = 0;
    int32_t visible_far_debounced_region = 0;
    int32_t chunk_mesh_rids = 0;
    int32_t far_region_mesh_rids = 0;
    // Active regions whose RID carries no surface. Must stay 0: a region is
    // published only after its worker upload finishes, and an instance with no
    // geometry would hide the members that cover it (the loading flash).
    int32_t far_region_meshes_without_surfaces = 0;
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

    // Far-region upload volume since the process started: merged region surfaces
    // queued for the upload channel and the vertices in them. A region rebuild
    // re-uploads its whole surface, so this is what a coalescing window is meant
    // to cut (see docs/streaming.md).
    uint64_t far_region_uploads = 0;
    uint64_t far_region_upload_vertices = 0;

    // Where the buried test's six neighbour probes resolve (MeshManager's
    // solid_probe_cache_): resident is a loaded chunk answering from its cached
    // flag, hits and misses are the ungenerated-neighbour predicate's memo. The
    // tail spends 0.8 s in that test, so the split says whether the 25 us worldgen
    // sample is being reused at all.
    uint64_t solid_probe_resident = 0;
    uint64_t solid_probe_hits = 0;
    uint64_t solid_probe_misses = 0;

    // The seed-grid far mode (docs/lod-modes.md), all 0 while the mode is off. Its
    // instances are created straight on the RenderingServer rather than as nodes, so
    // they are not in any of the counts above however much of the frame they are:
    // a 27 km reach is 4 draw calls and 321,384 vertices on screen and 0 instances in
    // this struct's own ledger. This is where that half gets counted.
    int32_t far_grid_tiles = 0;
    int32_t far_grid_built = 0;
    int32_t far_grid_draw_calls = 0;
    int32_t far_grid_quads = 0;
    int32_t far_grid_vertices = 0;
    // Columns the sampler ran for, and the asks the shared node table answered
    // instead (lod_node_cache.hpp): the ratio is what sharing the table is worth.
    int64_t far_grid_columns_sampled = 0;
    int64_t far_grid_cache_hits = 0;
    // What one frame of merging cost, averaged since the report last asked. The far
    // field's only main-thread work; a settled reach pays 0.
    double far_grid_merge_ms_per_frame = 0.0;
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_RENDER_STATS_HPP
