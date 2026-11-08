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

#include <algorithm>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>

namespace VoxelEngine {
using namespace godot;

void VoxelEngineController::bind_render_viewport(godot::Node* node) {
    render_viewport_ = node ? node->get_viewport() : nullptr;
    if (!render_viewport_) return;
    if (RenderingServer* rs = RenderingServer::get_singleton()) {
        rs->viewport_set_measure_render_time(render_viewport_->get_viewport_rid(), true);
    }
}

FrameCaptureGuard VoxelEngineController::begin_frame_capture(double delta) {
    return FrameCaptureGuard(frame_capture, perf_timer, delta, sample_render_stats(),
                             sample_engine_stats());
}

FrameRenderStats VoxelEngineController::sample_render_stats() {
    FrameRenderStats stats;
    if (!render_viewport_) return stats;
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) return stats;
    const RID viewport_rid = render_viewport_->get_viewport_rid();
    stats.setup_cpu_ms = rs->get_frame_setup_time_cpu();
    stats.render_cpu_ms = rs->viewport_get_measured_render_time_cpu(viewport_rid);
    stats.render_gpu_ms = rs->viewport_get_measured_render_time_gpu(viewport_rid);
    if (Performance* perf = Performance::get_singleton()) {
        const auto count = [perf](Performance::Monitor monitor) {
            return static_cast<uint64_t>(std::max(0.0, perf->get_monitor(monitor)));
        };
        stats.draw_calls = count(Performance::RENDER_TOTAL_DRAW_CALLS_IN_FRAME);
        stats.objects = count(Performance::RENDER_TOTAL_OBJECTS_IN_FRAME);
        stats.primitives = count(Performance::RENDER_TOTAL_PRIMITIVES_IN_FRAME);
    }
    return stats;
}

FrameEngineStats VoxelEngineController::sample_engine_stats() {
    FrameEngineStats stats;
    if (Performance* perf = Performance::get_singleton()) {
        const auto ms = [perf](Performance::Monitor monitor) {
            return std::max(0.0, static_cast<double>(perf->get_monitor(monitor))) * 1000.0;
        };
        stats.process_ms = ms(Performance::TIME_PROCESS);
        stats.physics_ms = ms(Performance::TIME_PHYSICS_PROCESS);
        stats.navigation_ms = ms(Performance::TIME_NAVIGATION_PROCESS);
    }
    return stats;
}

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
    // The completion drain's downstream queues. The completed-chunk depth above
    // says how much generation is waiting to install; these say whether the wait
    // is in the light stages, the worker light results, or the dirty-mark pass.
    report += "  Light stages:   " + String::num_int64(static_cast<int64_t>(chunk_world.pending_lighting_count())) + "\n";
    report += "  Light results:  " + String::num_int64(static_cast<int64_t>(chunk_world.get_scheduler().completed_light_count())) + "\n";
    report += "  Dirty stages:   " + String::num_int64(static_cast<int64_t>(chunk_world.pending_dirty_stage_count())) + "\n";
    chunks_processed_last_interval = chunks_processed_total;
    frame_count = 0;
    frame_time_accumulator = 0.0;
    perf_timer.reset_all();
    // Start a new delta baseline; the captured history stays, so a dump after
    // this report can still describe the frames that produced it.
    frame_capture.restart_baseline();
    MeshBuilder::get_perf_timer().reset_all();
    ChunkGenerator::get_perf_timer().reset_all();
    MeshBuilder::reset_vertex_tracking();
    MeshBuilder::reset_greedy_vertical_stats();
    return report;
}

String VoxelEngineController::get_light_profile_report() {
    return String(chunk_world.get_light_profile_report().c_str());
}

String VoxelEngineController::get_generation_path_report() {
    return String(chunk_world.get_generation_path_report().c_str());
}

String VoxelEngineController::get_slow_frame_report(int32_t frames, double slow_frame_ms) {
    const std::string dump =
        frame_capture.describe_window(perf_timer, frames, slow_frame_ms);
    return String(dump.c_str());
}

void VoxelEngineController::print_debug_info(double delta) {
    // Debug printing removed
}

} // namespace VoxelEngine
