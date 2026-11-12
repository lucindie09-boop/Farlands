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
                                     int32_t pcx, int32_t pcy, int32_t pcz) {
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
    const auto generation_start = std::chrono::steady_clock::now();
    const auto generation_ms = [&generation_start]() {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - generation_start).count();
    };

    size_t generating_count = chunk_world->get_scheduler().generating_count();
    size_t worker_count = thread_pool ? thread_pool->get_worker_count() : 0;

    // Feed-forward admission: admit at the rate the pool can finish rather than
    // scaling a fixed budget by pressure. `workers x frame / work-per-chunk x
    // 0.9`, with the frame from the last update and the per-chunk cost measured
    // by the fill profile (2.46 ms). This arm pairs it with a 90-chunk in-flight
    // cap instead of 180, so the completed queue cannot grow into the deep
    // backlog that slowed every drain iteration in the last attempt.
    // Assigned below in every path; the initializer that used to sit here was a
    // value the next block always overwrote.
    int32_t dynamic_max_generations;
    {
        // The controller's cost model, not this frame's measurement. The env
        // override is a measurement switch (see FrameBudgets).
        // A cheaper chunk during the fill: the line is the gameplay model, and
        // its sweeps ran with the loading caps closed, where a lower price only
        // bought refused offers. See the field for the open-cap pairs.
        const double derived_work_per_chunk_ms = fill_settled_
            ? generation_work_per_chunk_ms_for(worker_count)
            : budgets.generation_work_per_chunk_ms_loading;
        const double work_per_chunk_ms = std::max(0.01,
            budgets.generation_work_per_chunk_ms > 0.0
                ? budgets.generation_work_per_chunk_ms
                : derived_work_per_chunk_ms);
        const double frame_ms = std::clamp(last_frame_ms, 1.0, 50.0);
        const double rate = static_cast<double>(worker_count) * frame_ms / work_per_chunk_ms * 0.9;
        dynamic_max_generations = std::clamp(static_cast<int32_t>(std::lround(rate)), 1,
                                             budgets.chunk_generations * 2);
    }
    generation_stats.dynamic_max_last = static_cast<uint64_t>(dynamic_max_generations);
    generation_stats.dynamic_max_max = std::max(generation_stats.dynamic_max_max,
                                                static_cast<uint64_t>(dynamic_max_generations));

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
    // The set is sized differently for the fill: the gameplay term keeps the
    // completed queue short for a 0.75 ms drain, while the loading term feeds a
    // pool whose drain has a 6 ms clock and a 128-install cap. See
    // FrameBudgets::max_generating_in_flight_loading_per_worker for the arms.
    const int32_t in_flight_per_worker = fill_settled_
        ? budgets.max_generating_in_flight_per_worker
        : budgets.max_generating_in_flight_loading_per_worker;
    const size_t max_in_flight = budgets.generation_max_in_flight > 0
        ? static_cast<size_t>(budgets.generation_max_in_flight)
        : generation_in_flight_cap(worker_count, in_flight_per_worker);
    const bool in_flight_ok = generating_count < max_in_flight;
    const bool backlog_ok = chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog);
    const bool generation_admitted = in_flight_ok && backlog_ok;

    // The shared gate in front of both sources. Count it only on a frame that
    // actually had work to offer, so a closed gate is not confused with an idle
    // world.
    if (!generation_admitted &&
        ((!generation_pass_complete && !sweep_columns.empty()) || !chain_queue.empty())) {
        ++generation_stats.stop_admission;
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
            generation_stats.urgent_pending_max = std::max<uint64_t>(
                generation_stats.urgent_pending_max,
                static_cast<uint64_t>(chunk_world->urgent_chunk_count()));
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
        generation_stats.chain_pending_max = std::max<uint64_t>(
            generation_stats.chain_pending_max, static_cast<uint64_t>(chain_queue.size()));
        if (generation_admitted) {
            constexpr int32_t kChainShare = 4;  // the walk keeps 1/4
            const int32_t chain_budget = std::max(
                dynamic_max_generations - dynamic_max_generations / kChainShare, 1);
            // The chain's generations do NOT wake the walk (see the end-of-list
            // case below): its queue is refilled by installs, while a walk that
            // has reached the end of its list has nothing left to offer.
            const int32_t chained = drain_chain_queue(epoch, chain_budget, pcy,
                                                      budgets.generation_chain_budget_ms);
            if (chained > 0) {
                frame_generations += chained;
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
            if (generation_ms() >= budgets.generation_budget_ms) {
                ++generation_stats.stop_budget;
                break;
            }

            SweepCandidate candidate;
            if (!advance_sweep(generation_cursor, pcy, candidate)) {
                if (generation_cursor.column < sweep_columns.size()) {
                    // Stopped at an unread band: the list is not spent, this frame
                    // has simply reached the frontier. Resetting the cursor here
                    // (as the spent case does) would re-walk the same prefix every
                    // frame and never advance the frontier it is waiting on.
                    ++generation_stats.stop_band;
                    break;
                }
                // The cursor reached the end, and that is the end of the list's
                // usefulness: its bands are exact (above/below/oob rejects are 0
                // on every run) and fixed until the list is rebuilt, so every
                // in-band chunk of every column was offered exactly once on the
                // way here. Such an offer can only have generated (in flight
                // now), found the chunk resident, or been refused (the chain
                // re-offers a refused neighbour when one installs). A second
                // pass therefore cannot generate anything, and it used to make
                // one on every load: `generation_sweep_generated` kept the walk
                // re-walking all 20,230 candidates once they were resident,
                // which was 27,133 loaded rejects - two thirds of the walk's
                // checks - for zero chunks.
                // Left AT the end rather than reset: a rebuild reads that as the
                // radius the walk had covered and resumes at the new list's rim,
                // instead of sending the walk back to the near ring.
                ++generation_stats.sweep_passes;
                generation_cursor.column = sweep_columns.size();
                generation_cursor.slice = 0;
                generation_pass_complete = true;
                ++generation_stats.sweeps_completed;
                ++generation_stats.stop_spent;
                break;
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
            if (generate_chunk(cx, cy, cz, epoch, candidate.band_lo, candidate.band_hi)) {
                ++generations_this_frame;
                ++generation_stats.generations;
                ++frame_generations;
            } else {
                ++generation_stats.generate_refused;
            }
        }
        // The loop's own caps are not break statements, so record them after it:
        // the check cap is tested first in the condition, matching the loop.
        if (!generation_pass_complete && generation_admitted && checks >= max_checks_per_frame) {
            ++generation_stats.stop_check_cap;
        } else if (!generation_pass_complete && generation_admitted &&
                   generations_this_frame >= dynamic_max_generations) {
            ++generation_stats.stop_generation_cap;
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
