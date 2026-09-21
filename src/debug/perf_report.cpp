#include "debug/perf_report.hpp"

#include "worldgen/chunk_generator.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_map.hpp"

using namespace godot;
using namespace VoxelEngine;

String PerfReport::build(
    double frame_time_accumulator,
    uint64_t frame_count,
    double debug_print_interval,
    uint64_t chunks_processed_total,
    uint64_t chunks_processed_last_interval,
    const PerformanceTimer& perf_timer,
    size_t thread_pool_workers,
    size_t thread_pool_queue_size,
    size_t generating_count,
    size_t completed_chunk_count,
    size_t loaded_chunk_count,
    const WorldRenderStats& render_stats,
    const ChunkMap* chunk_map
) {
    String report = "=== Performance Report ===\n";

    double avg_frame_time = 0.0;
    if (frame_count > 0) {
        avg_frame_time = frame_time_accumulator / frame_count;
        report += "Frame time: avg=" + String::num(avg_frame_time * 1000.0, 2) +
                  "ms FPS=" + String::num(1.0 / avg_frame_time, 1) + "\n";
    }

    double proc_avg = perf_timer.get_avg(TimerID::ProcessTotal);
    double proc_min = perf_timer.get_min(TimerID::ProcessTotal);
    double proc_max = perf_timer.get_max(TimerID::ProcessTotal);
    uint64_t proc_count = perf_timer.get_count(TimerID::ProcessTotal);
    if (proc_count > 0) {
        if (proc_min >= 0.0) {
            report += "process_total:    avg=" + String::num(proc_avg, 3) +
                      "ms min=" + String::num(proc_min, 3) + "ms max=" + String::num(proc_max, 3) +
                      "ms (our C++ per frame)\n";
        } else {
            report += "process_total:    avg=" + String::num(proc_avg, 3) + "ms (our C++ per frame)\n";
        }
    }

    uint64_t chunks_processed_this_interval = chunks_processed_total - chunks_processed_last_interval;
    double chunks_per_sec = chunks_processed_this_interval / debug_print_interval;
    report += "Chunks processed: " + String::num_int64(chunks_processed_this_interval) +
              " (" + String::num(chunks_per_sec, 1) + "/sec)\n";

    report += "Thread Pool: " + String::num_int64(thread_pool_workers) +
              " workers, Queue: " + String::num_int64(thread_pool_queue_size) + " tasks\n";

    report += "Generating: " + String::num_int64(generating_count) + " chunks\n";
    report += "Completed queue: " + String::num_int64(completed_chunk_count) + " chunks\n";
    report += "Loaded chunks: " + String::num_int64(loaded_chunk_count) + "\n";
    report += "--- Render ---\n";
    report += "  Visible instances: " + String::num_int64(render_stats.visible_instances) +
              "  Meshes: " + String::num_int64(render_stats.mesh_rids) + "\n";
    report += "  Chunk instances: " + String::num_int64(render_stats.chunk_instances) +
              "  Region instances: " + String::num_int64(render_stats.far_region_instances) + "\n";
    report += "  Chunk meshes:    " + String::num_int64(render_stats.chunk_mesh_rids) +
              "  Region meshes:    " + String::num_int64(render_stats.far_region_mesh_rids) + "\n";
    report += "  Far eligible:    " + String::num_int64(render_stats.eligible_far_chunks) +
              "  Far cached:      " + String::num_int64(render_stats.cached_far_chunks) + "\n";
    report += "  Region members:  " + String::num_int64(render_stats.active_region_member_chunks) +
              "  Partial cache:   " + String::num_int64(render_stats.regions_partial_missing_cache) + "\n";
    // Both should sit at 0. A non-zero first value means water is missing on
    // screen right now (a chunk with liquid in its data and no liquid geometry
    // uploaded); the second is how many chunks have been remeshed to repair
    // that, which is the only way an intermittent drop shows up from inside the
    // game. See note_liquid_geometry in mesh_manager_upload.cpp.
    report += "  Liquid missing:  " + String::num_int64(render_stats.chunks_with_liquid_but_no_water_mesh) +
              "  Liquid repairs:  " + String::num_int64(render_stats.liquid_geometry_repairs) + "\n";
    // Cumulative upload bookkeeping. `water-only skips` must stay 0: it counts
    // uploads the content hash called unchanged while the water mesh differed —
    // water that exists in the world and not on screen.
    report += "  Mesh uploads:    " + String::num_int64(render_stats.mesh_uploads) +
              "  Dedup skips:     " + String::num_int64(render_stats.mesh_upload_dedup_skips) +
              "  Water-only skips: " + String::num_int64(render_stats.mesh_upload_swallowed_water_changes) + "\n";
    // Also 0: chunks that have geometry, are in range and are not covered by a far
    // region, but have no instance drawing them.
    report += "  Unrendered:      " + String::num_int64(render_stats.chunks_with_geometry_but_no_instance) + "\n";

    report += "--- per-frame breakdown (avg / max — the max column is what a spike frame paid) ---\n";
    auto phase_line = [&](const char* label, TimerID id) {
        report += String("  ") + label + String::num(perf_timer.get_avg(id), 3) +
                  "ms max=" + String::num(perf_timer.get_max(id), 3) + "ms\n";
    };
    phase_line("player_pos_update: avg=", TimerID::PlayerPosUpdate);
    phase_line("chunk_load_unload: avg=", TimerID::ChunkLoadUnload);
    phase_line("dirty_mesh_queue:  avg=", TimerID::DirtyMeshQueue);
    phase_line("process_completed_chunks: avg=", TimerID::ProcessCompletedChunks);
    phase_line("process_completed_meshes: avg=", TimerID::ProcessCompletedMeshes);
    uint64_t col_count = perf_timer.get_count(TimerID::UpdateCollision);
    if (col_count > 0) {
        double col_min = perf_timer.get_min(TimerID::UpdateCollision);
        double col_max = perf_timer.get_max(TimerID::UpdateCollision);
        if (col_min >= 0.0) {
            report += "  update_collision:  avg=" + String::num(perf_timer.get_avg(TimerID::UpdateCollision), 3) +
                      "ms min=" + String::num(col_min, 3) + "ms max=" + String::num(col_max, 3) +
                      "ms count=" + String::num_int64(col_count) + "\n";
        } else {
            report += "  update_collision:  avg=" + String::num(perf_timer.get_avg(TimerID::UpdateCollision), 3) +
                      "ms count=" + String::num_int64(col_count) + "\n";
        }
    }

    report += "  mesh_upload_gpu:   avg=" + String::num(perf_timer.get_avg(TimerID::MeshUploadGpu), 3) +
              "ms max=" + String::num(perf_timer.get_max(TimerID::MeshUploadGpu), 3) + "ms\n";
    report += "  world_update:      avg=" + String::num(perf_timer.get_avg(TimerID::WorldUpdate), 3) + "ms\n";
    report += "  scene_update:      avg=" + String::num(perf_timer.get_avg(TimerID::SceneUpdate), 3) + "ms\n";
    report += "  render_time:       avg=" + String::num(perf_timer.get_avg(TimerID::RenderTime), 3) + "ms\n";

    if (proc_count > 0) {
        double accounted = perf_timer.get_avg(TimerID::PlayerPosUpdate);
        accounted += perf_timer.get_avg(TimerID::ChunkLoadUnload);
        accounted += perf_timer.get_avg(TimerID::DirtyMeshQueue);
        accounted += perf_timer.get_avg(TimerID::ProcessCompletedChunks);
        accounted += perf_timer.get_avg(TimerID::ProcessCompletedMeshes);
        accounted += perf_timer.get_avg(TimerID::UpdateCollision);
        double unaccounted = proc_avg - accounted;

        if (unaccounted < 0.0) {
            report += "  unaccounted:     avg=" + String::num(unaccounted, 3) +
                      "ms (NEGATIVE \342\200\224 two timers overlap; see comment in perf_report.cpp)\n";
        } else {
            report += "  unaccounted:     avg=" + String::num(unaccounted, 3) + "ms\n";
        }
    }

    // Shard lock telemetry for the interval that just ended (drained, so each
    // report measures itself). This is where a spike frame gets its name: a
    // contended wait whose max approaches process_total's max means the frame
    // spent its spike waiting on a shard the light workers hold; zeroes across
    // the board exonerate the lock and point back at the phase maxima above.
    if (chunk_map != nullptr) {
        uint64_t ls[8];
        chunk_map->drain_shard_lock_stats(ls);
        auto ms = [](uint64_t ns) { return String::num(static_cast<double>(ns) / 1e6, 3); };
        report += "--- shard lock contention (this interval) ---\n";
        report += "  shared waits: " + String::num_int64(ls[0]) +
                  "  total=" + ms(ls[1]) + "ms  worst=" + ms(ls[2]) + "ms\n";
        report += "  excl waits:   " + String::num_int64(ls[3]) +
                  "  total=" + ms(ls[4]) + "ms  worst=" + ms(ls[5]) + "ms\n";
        report += "  excl holds:   total=" + ms(ls[6]) + "ms  worst=" + ms(ls[7]) + "ms\n";
    }

    report += "--- per-chunk (amortised) ---\n";
    double gen_avg = ChunkGenerator::get_perf_timer().get_avg(TimerID::GenerateChunk);
    uint64_t gen_count = ChunkGenerator::get_perf_timer().get_count(TimerID::GenerateChunk);
    if (gen_count > 0) {
        double gen_min = ChunkGenerator::get_perf_timer().get_min(TimerID::GenerateChunk);
        double gen_max = ChunkGenerator::get_perf_timer().get_max(TimerID::GenerateChunk);
        if (gen_min >= 0.0) {
            report += "  generate_chunk: avg=" + String::num(gen_avg, 3) +
                      "ms min=" + String::num(gen_min, 3) + "ms max=" + String::num(gen_max, 3) +
                      "ms count=" + String::num_int64(gen_count) + "\n";
        } else {
            report += "  generate_chunk: avg=" + String::num(gen_avg, 3) +
                      "ms count=" + String::num_int64(gen_count) + "\n";
        }
    }

    double mesh_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::BuildMesh);
    uint64_t mesh_count = MeshBuilder::get_perf_timer().get_count(TimerID::BuildMesh);
    if (mesh_count > 0) {
        double mesh_min = MeshBuilder::get_perf_timer().get_min(TimerID::BuildMesh);
        double mesh_max = MeshBuilder::get_perf_timer().get_max(TimerID::BuildMesh);
        if (mesh_min >= 0.0) {
            report += "  build_mesh:     avg=" + String::num(mesh_avg, 3) +
                      "ms min=" + String::num(mesh_min, 3) + "ms max=" + String::num(mesh_max, 3) +
                      "ms count=" + String::num_int64(mesh_count) + "\n";
        } else {
            report += "  build_mesh:     avg=" + String::num(mesh_avg, 3) +
                      "ms count=" + String::num_int64(mesh_count) + "\n";
        }
    }

    // The liquid surface pass, a slice of build_mesh rather than a phase of its
    // own. It is called out because it is the one pass whose cost scales with how
    // much WATER is in view instead of how much terrain is.
    if (MeshBuilder::get_perf_timer().get_count(TimerID::FluidMesh) > 0) {
        report += "    fluid_mesh:   avg=" +
                  String::num(MeshBuilder::get_perf_timer().get_avg(TimerID::FluidMesh), 3) + "ms\n";
    }

    double alloc_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::MeshArrayAlloc);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::MeshArrayAlloc) > 0) {
        report += "    array_alloc:  avg=" + String::num(alloc_avg, 3) + "ms\n";
    }
    double copy_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::MeshDataCopy);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::MeshDataCopy) > 0) {
        report += "    data_copy:    avg=" + String::num(copy_avg, 3) + "ms\n";
    }
    double gpu_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::MeshGpuUpload);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::MeshGpuUpload) > 0) {
        report += "    gpu_upload:   avg=" + String::num(gpu_avg, 3) + "ms\n";
    }


    // Mesh building sub-timers
    double solid_cache_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::SolidCachePopulation);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::SolidCachePopulation) > 0) {
        report += "    solid_cache:  avg=" + String::num(solid_cache_avg, 3) + "ms\n";
    }
    double greedy_h_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::GreedyMeshHorizontal);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::GreedyMeshHorizontal) > 0) {
        report += "    greedy_h:     avg=" + String::num(greedy_h_avg, 3) + "ms\n";
    }
    double greedy_v_avg = MeshBuilder::get_perf_timer().get_avg(TimerID::GreedyMeshVertical);
    if (MeshBuilder::get_perf_timer().get_count(TimerID::GreedyMeshVertical) > 0) {
        report += "    greedy_v:     avg=" + String::num(greedy_v_avg, 3) + "ms\n";
    }
    const auto greedy_v_stats = MeshBuilder::get_greedy_vertical_stats();
    if (greedy_v_stats.merge_attempts > 0) {
        const double merge_success_pct =
            100.0 * static_cast<double>(greedy_v_stats.merge_successes) /
            static_cast<double>(greedy_v_stats.merge_attempts);
        report += "    merges:       " + String::num_int64(greedy_v_stats.merge_successes) +
                  "/" + String::num_int64(greedy_v_stats.merge_attempts) +
                  " (" + String::num(merge_success_pct, 1) + "%)\n";
    }
    report += "    ao_reject:    mismatch=" + String::num_int64(greedy_v_stats.reject_ao_mismatch) +
              " occlusion=" + String::num_int64(greedy_v_stats.reject_ao_occlusion) + "\n";
    const uint64_t continuation_rejects =
        greedy_v_stats.reject_light_mismatch + greedy_v_stats.reject_rotation_mismatch +
        greedy_v_stats.reject_block_mismatch +
        greedy_v_stats.reject_distance_limit;
    if (continuation_rejects > 0) {
        report += "    split_run:    light=" + String::num_int64(greedy_v_stats.reject_light_mismatch) +
                  " rotation=" + String::num_int64(greedy_v_stats.reject_rotation_mismatch) +
                  " block=" + String::num_int64(greedy_v_stats.reject_block_mismatch) +
                  " distance=" + String::num_int64(greedy_v_stats.reject_distance_limit) + "\n";
    }
    if (greedy_v_stats.lod_cells_visited > 0) {
        report += "    lod_v:        visited=" + String::num_int64(greedy_v_stats.lod_cells_visited) +
                  " air_skip=" + String::num_int64(greedy_v_stats.lod_cells_skipped_air) +
                  " culled=" + String::num_int64(greedy_v_stats.lod_faces_culled) +
                  " emitted=" + String::num_int64(greedy_v_stats.lod_faces_emitted) + "\n";
    }
    // Chunk generation sub-timers
    double voxelization_avg = ChunkGenerator::get_perf_timer().get_avg(TimerID::Voxelization);
    if (ChunkGenerator::get_perf_timer().get_count(TimerID::Voxelization) > 0) {
        report += "    voxelization: avg=" + String::num(voxelization_avg, 3) + "ms\n";
    }

    if (gen_count > 0 && mesh_count > 0) {
        report += "  TOTAL per chunk: avg=" + String::num(gen_avg + mesh_avg, 3) + "ms\n";
    }

    double avg_vertices = MeshBuilder::get_avg_vertices_per_chunk();
    if (avg_vertices > 0) {
        report += "  vertices/chunk avg: " + String::num(avg_vertices, 1) + "\n";
    }

    if (proc_count > 0 && frame_count > 0) {
        double frame_time_ms = avg_frame_time * 1000.0;
        double godot_overhead = std::max(0.0, frame_time_ms - proc_avg);
        report += "--- engine/GPU overhead ---\n";
        report += "  frame - process_total: avg=" + String::num(godot_overhead, 3) + "ms\n";
        if (proc_avg > frame_time_ms) {
            report += "  (process_total > frame_time - timing inconsistency)\n";
        }
    }

    return report;
}
