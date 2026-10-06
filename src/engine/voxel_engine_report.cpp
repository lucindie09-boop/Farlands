// The engine controller's perf report: the single place the far mode's own ledger is
// merged into the render stats, because PerfReport (the debug half of the tree) does
// not include the far mode's header -- a report that skipped it would show the
// frame's world half as the whole frame.
//
// Split out of voxel_engine_controller.cpp, which keeps the lifecycle, the per-frame
// update and the property surface.
#include "engine/voxel_engine_controller.hpp"

#include "debug/perf_report.hpp"
#include "mesh/mesh_builder.hpp"
#include "worldgen/chunk_generator.hpp"

namespace VoxelEngine {
using namespace godot;

String VoxelEngineController::get_performance_report() {
    // The far mode's own ledger, merged into the render stats here rather than read
    // out of the grid inside PerfReport: the debug half of the tree does not include
    // the far mode's own header, and a report that skipped it would show the frame's
    // world half as the whole frame.
    WorldRenderStats render_stats = mesh_manager.gather_render_stats();
    const LodGrid::Stats far = lod_grid.gather_stats();
    render_stats.far_grid_tiles = far.tiles_live;
    render_stats.far_grid_built = far.tiles_built;
    render_stats.far_grid_draw_calls = far.draw_calls;
    render_stats.far_grid_quads = far.quads;
    render_stats.far_grid_vertices = far.vertices;
    render_stats.far_grid_columns_sampled = far.columns_sampled;
    render_stats.far_grid_cache_hits = far.cache_hits;
    render_stats.far_grid_merge_ms_per_frame = far.merge_ms_per_frame;
    String report = PerfReport::build(
        frame_time_accumulator,
        frame_count,
        2.0, // hardcoded interval
        chunks_processed_total,
        chunks_processed_last_interval,
        perf_timer,
        thread_pool ? thread_pool->get_worker_count() : 0,
        thread_pool ? thread_pool->get_queue_size() : 0,
        chunk_world.get_scheduler().generating_count(),
        chunk_world.get_scheduler().completed_chunk_count(),
        chunk_world.get_chunk_map().size(),
        render_stats,
        &chunk_world.get_chunk_map()
    );
    chunks_processed_last_interval = chunks_processed_total;
    frame_count = 0;
    frame_time_accumulator = 0.0;
    perf_timer.reset_all();
    MeshBuilder::get_perf_timer().reset_all();
    ChunkGenerator::get_perf_timer().reset_all();
    MeshBuilder::reset_vertex_tracking();
    MeshBuilder::reset_greedy_vertical_stats();
    return report;
}

void VoxelEngineController::print_debug_info(double delta) {
    // Debug printing removed
}

} // namespace VoxelEngine
