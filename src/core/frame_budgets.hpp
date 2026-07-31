#ifndef FUK_MINECRAFT_FRAME_BUDGETS_HPP
#define FUK_MINECRAFT_FRAME_BUDGETS_HPP

#include <cstddef>
#include <cstdint>

namespace VoxelEngine {

struct FrameBudgets {
    int32_t chunk_generations = 256;
    int32_t chunk_completions_initial = 128;
    int32_t chunk_completions_gameplay = 64;
    int32_t mesh_rebuilds_immediate = 16;
    int32_t mesh_rebuilds_idle = 4;
    int32_t mesh_rebuilds_active = 16;
    int32_t mesh_rebuilds_loading = 64;
    int32_t mesh_uploads_idle = 2;
    int32_t mesh_uploads_active = 32;
    int32_t mesh_uploads_loading = 64;

    size_t completed_queue_backlog = 512;
    size_t dirty_mesh_backlog = 512;
    size_t worker_queue_backlog = 512;

    int32_t max_loaded_chunks = 50000;

    double processing_budget_ms = 2.5;
    double mesh_completion_budget_ms = 0.75;
    int32_t max_mesh_completions_per_frame = 16;
    int32_t loading_threshold = 500;
    double loading_duration = 3.0;

    int32_t unload_checks_per_frame = 500;
    int32_t unloads_per_frame = 200;
    int32_t max_generation_checks_per_frame = 100000;
    int32_t generating_per_worker = 2;

    double flush_interval = 5.0;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_FRAME_BUDGETS_HPP
