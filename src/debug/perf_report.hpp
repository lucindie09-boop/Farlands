#ifndef FUK_MINECRAFT_PERF_REPORT_HPP
#define FUK_MINECRAFT_PERF_REPORT_HPP
#include <godot_cpp/variant/string.hpp>
#include <cstdint>
#include <cstddef>
#include "core/performance_timer.hpp"
#include "render/world_render_stats.hpp"

namespace VoxelEngine {

class PerfReport {
public:
    static godot::String build(
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
        const WorldRenderStats& render_stats
    );
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_PERF_REPORT_HPP
