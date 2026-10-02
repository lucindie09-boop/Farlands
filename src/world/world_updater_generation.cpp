// The generation half of a frame: the chain's offers, the urgent requests a
// caller is blocked on, and the ring-ordered walk with the pressure-driven
// budget and the admission cap it runs under. Kept apart from
// world/world_updater.cpp, which owns the frame's order and its unload and mesh
// budget halves.

#include "world/world_updater.hpp"

#include "world/chunk_world.hpp"
#include "core/thread_pool.hpp"
#include "core/performance_timer.hpp"
#include <algorithm>
#include <chrono>

namespace VoxelEngine {
using namespace godot;

void WorldUpdater::update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch,
                                     int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    ScopedTimer t(*perf_timer, TimerID::ChunkLoadUnload);
    const auto sweep_start = std::chrono::steady_clock::now();
    ++generation_stats.frames;
    // Per-frame halves of the rolling window, written once at the end.
    uint32_t frame_checks = 0;
    uint32_t frame_generations = 0;
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    // The list holds player-relative columns with ABSOLUTE slice ranges, so it
    // goes stale when either half moves: the render distance, the terrain the
    // bands were read from, or the player's chunk in the horizontal axes. A
    // vertical crossing does not (the bands are absolute in chunk y), which is
    // why this compares x/z rather than using the `chunk_changed` flag it is
    // handed. Rebuilding is cheap now — ~3,200 entries against the 208,585 the
    // old list held — where a per-crossing rebuild of the old shape would have
    // been a stall.
    const bool columns_moved = pcx != sweep_origin_cx || pcz != sweep_origin_cz;
    if (sweep_columns.empty() || current_render_distance != active_render_distance ||
        columns_moved || sweep_bands_dirty) {
        rebuild_sweep_columns(active_render_distance, pcx, pcz);
    }
    service_sweep_bands();

    size_t generating_count = chunk_world->get_scheduler().generating_count();
    size_t worker_queue_size = thread_pool ? thread_pool->get_queue_size() : 0;
    size_t worker_count = thread_pool ? thread_pool->get_worker_count() : 0;
    const double worker_pressure =
        budgets.worker_queue_backlog > 0 ? static_cast<double>(worker_queue_size) / static_cast<double>(budgets.worker_queue_backlog) : 0.0;
    const double generating_pressure =
        worker_count > 0 ? static_cast<double>(generating_count) / static_cast<double>(worker_count * budgets.generating_per_worker) : 0.0;
    const double thread_pool_pressure = std::max(worker_pressure, generating_pressure);

    int32_t dynamic_max_generations = budgets.chunk_generations;
    if (thread_pool_pressure > 0.75) {
        const double generation_scale = std::clamp(1.5 - thread_pool_pressure, 0.25, 1.0);
        dynamic_max_generations = std::max(1, static_cast<int32_t>(
            std::round(static_cast<double>(budgets.chunk_generations) * generation_scale)));
    }

    // --- Admission control: cap the in-flight generation set -----------------
    // The pressure above only scales the per-frame generation budget, so the set
    // still grew for as long as the workers lagged. This is the actual bound, and
    // the reason it helps beyond memory: a smaller set means completions arrive in
    // a trickle instead of landing in one 1,000-chunk burst (which is what the
    // install phase's worst frames are), the worker queue stays short enough for a
    // column-bound answer to come back before the frontier reaches it, and the walk
    // stops spending checks on candidates it can only refuse. Refilling is not a
    // risk: the sweep can enqueue `dynamic_max_generations` per frame against a cap
    // of `workers * per_worker`, so the set is topped up many times over between
    // completions. Urgent (paste) requests keep their own separate allowance and
    // are deliberately NOT gated on this cap — a caller is blocked on those.
    const size_t max_in_flight = std::max<size_t>(
        64, worker_count * static_cast<size_t>(std::max(1, budgets.max_generating_in_flight_per_worker)));
    const bool in_flight_ok = generating_count < max_in_flight;
    const bool backlog_ok = chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog);
    const bool generation_admitted = in_flight_ok && backlog_ok;

    if (chunk_changed) {
        generation_cursor          = SweepCursor{};
        generation_pass_complete   = false;
        generation_sweep_generated = false;
        ++generation_stats.cursor_resets;
    }

    // --- Urgent requests, before anything else ------------------------------
    // A caller (a paste) is waiting on these chunks, and the sweep would mostly
    // refuse them anyway: the sky above a build sits outside the per-column band
    // and is never generated on its own, and a build can reach past the render
    // distance. They are generated here rather than by the sweep because the
    // sweep's filters are exactly what is in the way. The allowance is small and
    // separate from the sweep's, so a huge request streams in over frames
    // instead of stalling this one.
    {
        constexpr size_t kMaxUrgentPerFrame = 8;
        size_t allowance = kMaxUrgentPerFrame;
        // The completed-queue guard, and deliberately not the in-flight cap: a
        // paste is blocked on these chunks, so it gets to keep its small separate
        // allowance even when the sweep has been told to stop enqueueing.
        if (!backlog_ok) {
            allowance = 0;
        }
        if (allowance > 0) {
            const std::vector<ChunkPos> urgent =
                chunk_world->take_urgent_chunk_requests(allowance);
            for (const ChunkPos& pos : urgent) {
                ++generation_stats.urgent_requested;
                if (pos.y < 0 || pos.y >= kWorldChunkSlices) continue;
                if (generate_chunk(pos.x, pos.y, pos.z, epoch)) {
                    ++generation_stats.urgent_generated;
                }
            }
        }
    }

    const size_t max_checks_per_frame = static_cast<size_t>(
        std::max(dynamic_max_generations * 2, 512));

    // --- Phase 1: the generation chain ----------------------------------
    // The only chunk-selection priority (the frustum pass is retired; its
    // counters and the A/B that retired it are in ARCHITECTURE.md). A chunk that
    // installed with terrain in it queued its four horizontal neighbours, and
    // this drains them before the ring walk gets a say — through the same
    // filters and the same budget as any other candidate, so it reorders work,
    // never bypasses a filter. The share is a constant with a measured reason:
    // the chain's offers are neighbours of just-built terrain — the walk's own
    // near rings, which its cursor re-walks after every crossing — so its offer
    // spends the walk's checks better than the walk's next check does. But the
    // walk owns the disc beyond what has been built, so the chain takes 3/4 and
    // leaves the walk a quarter rather than starving it. (The frustum pass had
    // 1/2.)
    if (!chain_queue.empty()) {
        if (generation_admitted) {
            constexpr int32_t kChainShare = 4;  // the walk keeps 1/4
            const int32_t chain_budget = std::max(
                dynamic_max_generations - dynamic_max_generations / kChainShare, 1);
            const int32_t chained = drain_chain_queue(epoch, chain_budget, pcy, generation_sweep_generated);
            if (chained > 0) {
                frame_generations += chained;
                // A chain generation is real progress: retire the "nothing left"
                // verdict so the walk re-checks the (now cheaper) near rings.
                generation_pass_complete = false;
            }
        }
    }

    // --- Phase 2: Normal ring-ordered pass ---
    if (!generation_pass_complete && !sweep_columns.empty()) {
        size_t   checks              = 0;
        size_t   inflight_skips      = 0;
        int32_t  generations_this_frame = 0;

        while (checks < max_checks_per_frame &&
               generations_this_frame < dynamic_max_generations &&
               generation_admitted) {

            SweepCandidate candidate;
            if (!advance_sweep(generation_cursor, pcy, candidate)) {
                if (generation_cursor.column < sweep_columns.size()) {
                    // Stopped at an unread band: the list is not spent, this frame
                    // has simply reached the frontier. Resetting the cursor here
                    // (as the spent case does) would re-walk the same prefix every
                    // frame and never advance the frontier it is waiting on.
                    break;
                }
                // The whole list has been offered. If none of it generated
                // anything there is nothing left to do until something
                // invalidates the list, so stop walking it every frame.
                generation_cursor = SweepCursor{};
                if (!generation_sweep_generated) {
                    generation_pass_complete = true;
                    ++generation_stats.sweeps_completed;
                    break;
                }
                generation_sweep_generated = false;
                continue;
            }
            const int32_t cx = candidate.x;
            const int32_t cy = candidate.y;
            const int32_t cz = candidate.z;

            uint64_t key = chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz);
            // Already on its way: skip it WITHOUT spending a check, which is what
            // moves this budget onto terrain that does not exist yet. Bounded by
            // its own allowance so a frame cannot walk the whole list on skips —
            // past the allowance a candidate falls through to the old path, where
            // generate_chunk refuses it (bounded waste instead of unbounded
            // cheap work). The filter takes no lock (see
            // ChunkScheduler::may_be_generating).
            if (inflight_skips < max_checks_per_frame &&
                chunk_world->get_scheduler().may_be_generating(key)) {
                ++inflight_skips;
                ++generation_stats.reject_inflight;
                continue;
            }

            ++checks;
            ++generation_stats.checks;
            ++frame_checks;

            if (chunk_world->get_chunk_map().contains(key)) {
                ++generation_stats.reject_loaded;
                continue;
            }

            int32_t chunk_bottom = cy * CHUNK_HEIGHT;
            int32_t chunk_top    = (cy + 1) * CHUNK_HEIGHT;
            ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
            // Same window the list was built from: full column (band +
            // underground fill) within kUndergroundFillRadius of the player,
            // near-surface band only beyond it, and the squish's narrower pad
            // when the test toggle is on (band_pad()).
            const bool fill_column = candidate.fill_column;
            const float pad = band_pad();
            if (static_cast<float>(chunk_bottom) > surface.top_h + pad) {
                ++generation_stats.reject_above;
                continue;
            }
            if (!fill_column && static_cast<float>(chunk_top) < surface.land_h - pad) {
                ++generation_stats.reject_below;
                continue;
            }
            // World bounds: never generate chunks outside [0, kSlices) — the
            // fill path no longer has a bottom height filter to catch them.
            if (cy < 0 || cy >= kWorldChunkSlices) {
                ++generation_stats.reject_oob;
                continue;
            }

            ++generation_stats.band_pass;
            if (generate_chunk(cx, cy, cz, epoch)) {
                ++generations_this_frame;
                ++generation_stats.generations;
                ++frame_generations;
                generation_sweep_generated = true;
            } else {
                ++generation_stats.generate_refused;
            }
        }
    }

    {
        const std::chrono::duration<double, std::milli> elapsed =
            std::chrono::steady_clock::now() - sweep_start;
        generation_stats.last_ms = elapsed.count();
        generation_stats.total_ms += elapsed.count();
        generation_stats.max_ms = std::max(generation_stats.max_ms, generation_stats.last_ms);
        generation_stats.window_checks[generation_stats.window_head] = frame_checks;
        generation_stats.window_generations[generation_stats.window_head] = frame_generations;
        generation_stats.window_head =
            (generation_stats.window_head + 1) % GenerationStats::kWindowFrames;
        if (generation_stats.window_frames < GenerationStats::kWindowFrames) {
            ++generation_stats.window_frames;
        }
    }
}

} // namespace VoxelEngine
