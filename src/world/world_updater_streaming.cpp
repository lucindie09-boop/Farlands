// Keeping terrain ahead of the frontier: the column prefetch scan, the sweep
// band frontier and its budget, the generation chain's queue, and the cursor the
// walk advances. Kept apart from world/world_updater.cpp, which decides when
// each of these runs and in what order.

#include "world/world_updater.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include "core/thread_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

int32_t WorldUpdater::drain_chain_queue(uint64_t epoch, int32_t budget, int32_t pcy,
                                        double budget_ms) {
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    int32_t generations = 0;
    // Bounds OFFERS, not successes: the count budget below only advances on a
    // generation, so a queue whose entries all skip or refuse ran to the end of
    // the queue in one frame - the fill profile's 24 ms generation frames
    // against a 0.64 ms average. One offer is always made, like the band
    // service, so a dear first entry cannot stall the chain.
    const auto drain_start = std::chrono::steady_clock::now();
    int32_t offers = 0;
    while (generations < budget && !chain_queue.empty()) {
        if (offers > 0 && budget_ms > 0.0 &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - drain_start).count() >= budget_ms) {
            break;
        }
        ++offers;
        const uint64_t key = chain_queue.front();
        chain_queue.pop_front();
        chain_queued.erase(key);
        int32_t cx = 0, cy = 0, cz = 0;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        ++generation_stats.chain_offered;
        // The SAME filters the walk applies, in the same order. Outside the world
        // height or outside the sweep's disc: not a failure, just not offered.
        if (cy < 0 || cy >= kWorldChunkSlices) {
            ++generation_stats.chain_skipped;
            continue;
        }
        const int32_t dx = cx - sweep_origin_cx;
        const int32_t dz = cz - sweep_origin_cz;
        if (dx * dx + dz * dz > current_render_distance * current_render_distance) {
            ++generation_stats.chain_skipped;
            continue;
        }
        // Already on its way, or already here: the chain exists to ORDER work, and
        // both mean the work exists. The free checks answer first, so the locked
        // `contains` only runs for a chunk that is neither built-column-complete
        // nor visibly in flight — the seed-time filters already removed most of
        // those, and each remaining one is a shard lock on the map the workers
        // are writing.
        if (chunk_world->get_scheduler().may_be_generating(key)) {
            ++generation_stats.chain_skipped;
            continue;
        }
        if (built_columns.count(chunk_world->get_chunk_map().get_chunk_key(cx, 0, cz)) != 0 ||
            chunk_world->get_chunk_map().contains(key)) {
            ++generation_stats.chain_skipped;
            continue;
        }
        // The band the walk generates within. The list was built from these same
        // bounds, so a reject here means the band is exactly the walk's.
        const bool fill_column = column_fill_enabled(dx, dz);
        const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
        if (!sweep::chunk_in_band(cy, surface.land_h, surface.top_h, fill_column, band_pad())) {
            ++generation_stats.chain_skipped;
            continue;
        }
        if (generate_chunk(cx, cy, cz, epoch)) {
            ++generations;
            ++generation_stats.chain_generations;
        } else {
            // Not placed: in flight past the lock-free filter's view, or the
            // backlog guard is holding. Re-queueing here would LOOP — the queue
            // is refilled by installs far faster than the budget drains it, and
            // a refused offer comes back on its own when a neighbour installs
            // again. The walk reaches it regardless; the chain is a priority,
            // not the only path.
            ++generation_stats.chain_refused;
        }
    }
    if (generations == budget && !chain_queue.empty()) {
        ++generation_stats.chain_budget_stops;
    }
    return generations;
}

void WorldUpdater::service_sweep_bands() {
    // Top the requests up before the frontier, so a column entering the disc
    // usually has its answer waiting by the time the frontier gets to it.
    pump_column_prefetch();
    if (sweep_band_frontier >= sweep_columns.size()) return;  // caught up
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    // Always at least one column, so the frontier can never stall on a column
    // slower than the budget. The list is nearest-first, so this always banks
    // the terrain the player is standing in before anything far away.
    const double kBandBudgetMs = sweep_band_budget_ms;
    const auto start = std::chrono::steady_clock::now();
    double band_ms = 0.0;
    uint64_t columns_read = 0;
    double worst_column_ms = 0.0;
    double worst_bounds_ms = 0.0, worst_resident_ms = 0.0;
    while (sweep_band_frontier < sweep_columns.size()) {
        SweepColumn& column = sweep_columns[sweep_band_frontier];
        // A movement rebuild carried this band across: it is already read and
        // its columns are already in candidate_offsets. Skipping it is what
        // turns "re-read the whole list" into "read the entering rim".
        if (column.band_ready) {
            ++sweep_band_frontier;
            continue;
        }
        const double before_column_ms = band_ms;
        const int32_t cx = sweep_origin_cx + column.dx;
        const int32_t cz = sweep_origin_cz + column.dz;
        const double before_bounds = band_ms;
        const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
        const bool fill_column = column_fill_enabled(column.dx, column.dz);
        column.band = sweep::band_for_column(surface.land_h, surface.top_h, fill_column,
                                             kWorldChunkSlices, band_pad());
        column.band_ready = true;
        // What the band's ±32-block pad costs is a question about the SHAPE of
        // the distribution, not its mean: record it here, where the band is
        // built and fill is known. Fill columns are kept out of the histogram
        // because their band reaches the world floor by design.
        const int32_t band_slices = sweep::count(column.band);
        if (fill_column) {
            ++generation_stats.fill_columns_read;
            generation_stats.fill_band_slices += static_cast<uint64_t>(band_slices);
        } else {
            ++generation_stats.band_size_hist[sweep::band_size_bucket(band_slices)];
            generation_stats.band_max_slices = std::max(generation_stats.band_max_slices,
                                                        static_cast<uint64_t>(band_slices));
        }
        // Decided here, where the band was just computed, rather than by the walk
        // that would otherwise look up every slice of it on every pass. A chunk in
        // flight is not resident yet, so such a column stays walkable until a
        // rebuild — a wasted walk, never a hole.
        double resident_ms = 0.0;
        // ONE shard acquisition for the whole column, then lock-free probes.
        // Every chunk of a column shares a shard (ChunkMap::key_to_shard hashes
        // x and z, not y), so the band's count(band) questions are answered under
        // a single lock instead of one acquisition per slice. That matters
        // because a slice's acquisition is the only part that can WAIT, and each
        // one could queue behind a generation worker writing its own shard:
        // /genstats recorded the slowest single column spending 39.4 ms of its
        // 39.6 ms in 7 lookups, of which 39.4 ms was ONE lookup waiting for a
        // shard. The wait is now paid once per column, at the acquisition timed
        // below, and the probes that follow cannot contend with anything.
        {
            const auto t_resident = std::chrono::steady_clock::now();
            auto column_lock = chunk_world->get_chunk_map().lock_column(cx, cz);
            // The slowest single column ACQUISITION of the session, not the
            // average: one contended shard lock is the whole hypothesis.
            generation_stats.max_contains_ms = std::max(generation_stats.max_contains_ms,
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_resident).count());
            const bool fully_resident = sweep::band_fully_resident(column.band, [&](int32_t cy) {
                return chunk_world->get_chunk_map().contains_fast(
                    chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz));
            });
            resident_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_resident).count();
            if (fully_resident) {
                built_columns.insert(chunk_world->get_chunk_map().get_chunk_key(cx, 0, cz));
                ++generation_stats.columns_built;
            }
        }
        generation_stats.candidate_offsets += static_cast<uint64_t>(sweep::count(column.band));
        ++generation_stats.band_reads;
        ++columns_read;
        ++sweep_band_frontier;
        band_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        // Parts of THIS column, so the record matches the frame it explains.
        const double this_column_ms = band_ms - before_column_ms;
        if (this_column_ms > worst_column_ms) {
            worst_column_ms = this_column_ms;
            worst_bounds_ms = band_ms - before_bounds;
            worst_resident_ms = resident_ms;
        }
        // The budget can only bound WORK: it is checked after each column, so a
        // frame can overshoot it by the cost of the column it was in when the
        // budget ran out. That cost is what separates a budget that is not bounding
        // anything (many cheap columns, every value small) from this thread being
        // taken away mid-column (one value as large as the whole overshoot).
        if (band_ms >= kBandBudgetMs) break;
    }
    // The elapsed value is written straight into the stats so the timing costs
    // nothing per column; `max` is the worst frame the budget allowed to slip.
    generation_stats.total_band_ms += band_ms;
    // Recorded together with the worst frame, because the two answers to "a 20 ms
    // band frame" mean opposite things: many columns says the budget is not
    // bounding anything, one column says this thread was taken away from us.
    if (band_ms > generation_stats.max_band_ms) {
        generation_stats.max_band_ms = band_ms;
        generation_stats.max_band_columns = columns_read;
        generation_stats.max_band_column_ms = worst_column_ms;
        generation_stats.max_band_bounds_ms = worst_bounds_ms;
        generation_stats.max_band_resident_ms = worst_resident_ms;
    }
}

bool WorldUpdater::advance_sweep(SweepCursor& cursor, int32_t pcy, SweepCandidate& out) {
    while (cursor.column < sweep_columns.size()) {
        const SweepColumn& column = sweep_columns[cursor.column];
        // Not read yet: the walk stops here rather than reading it, so the cost
        // stays on service_sweep_bands' budget and both stay in ring order.
        if (!column.band_ready) return false;
        // Already fully built: skip the whole column for one lookup instead of
        // re-confirming each of its chunks. Both passes share this walk, so the
        // frustum pass gets the same skip.
        if (!built_columns.empty()) {
            const uint64_t column_key = chunk_world->get_chunk_map().get_chunk_key(
                sweep_origin_cx + column.dx, 0, sweep_origin_cz + column.dz);
            if (built_columns.count(column_key) != 0) {
                ++generation_stats.columns_skipped;
                ++cursor.column;
                cursor.slice = 0;
                continue;
            }
        }
        int32_t cy = 0;
        if (sweep::slice_cy(column.band, pcy, cursor.slice, cy)) {
            ++cursor.slice;
            out.x = sweep_origin_cx + column.dx;
            out.y = cy;
            out.z = sweep_origin_cz + column.dz;
            out.fill_column = column_fill_enabled(column.dx, column.dz);
            out.band_lo = column.band.lo;
            out.band_hi = column.band.hi;
            return true;
        }
        ++cursor.column;
        cursor.slice = 0;
    }
    return false;
}

void WorldUpdater::rebuild_sweep_columns(int32_t horizontal_rd, int32_t pcx, int32_t pcz) {
    const auto rebuild_start = std::chrono::steady_clock::now();
    ++generation_stats.rebuilds;
    // A movement rebuild can carry the bands it already has. A band is derived
    // from the column's own terrain, not from the player's position, so the
    // columns shared by the old and new discs keep exactly the band they had;
    // only the crescent entering the disc is new. Without this, every crossing
    // marked all ~3,200 bands unknown and the frontier re-read them through
    // service_sweep_bands' 2 ms budget -- a 2 ms main-thread bill on every
    // crossing, for columns whose answers were already in hand. Only a pure
    // move qualifies: a render-distance change or a terrain invalidation has to
    // rebuild every band. The old disc is saved in a dense grid (one cell per
    // offset) so the new list can look its band up without allocating a map
    // per crossing.
    const bool can_carry = !sweep_columns.empty() && !sweep_bands_dirty &&
                           horizontal_rd == current_render_distance &&
                           sweep_origin_cx != INT32_MIN && sweep_origin_cz != INT32_MIN;
    const int32_t old_origin_cx = sweep_origin_cx;
    const int32_t old_origin_cz = sweep_origin_cz;
    const int32_t old_rd = current_render_distance;
    const int32_t old_side = 2 * old_rd + 1;
    // --- Where the previous walk had got to, for the resume below ------------
    // A rebuilt list is re-sorted around the new origin, so the cursor's INDEX
    // cannot be carried across; its RADIUS can, because the walk goes outward and
    // every column closer than the cursor was offered on the way there. A spent
    // list leaves the cursor at the end, so the radius is the old list's own
    // outer edge. Without this, every crossing restarted at the near ring and
    // re-offered a disc that was 98% resident: 2.4 M checks over a 40 s flight,
    // nearly all of them `reject_loaded`.
    int32_t walked_r2 = -1;
    if (!sweep_columns.empty()) {
        const size_t walked_at = std::min(generation_cursor.column, sweep_columns.size() - 1);
        const SweepColumn& walked = sweep_columns[walked_at];
        walked_r2 = static_cast<int32_t>(walked.dx) * walked.dx +
                    static_cast<int32_t>(walked.dz) * walked.dz;
    }
    // Only a genuine crossing may resume. A teleport carries none of the old
    // bands, so a resumed cursor would sit at a column the frontier has not read
    // yet; and the radius argument only holds for a move of a couple of chunks.
    // Anything larger resets, which is the old behaviour.
    const int32_t move_dx = can_carry ? pcx - old_origin_cx : 0;
    const int32_t move_dz = can_carry ? pcz - old_origin_cz : 0;
    const bool small_move = std::max(std::abs(move_dx), std::abs(move_dz)) <= 2;
    if (can_carry) {
        const size_t side = static_cast<size_t>(old_side);
        carry_bands.assign(side * side, sweep::ChunkBand{});
        carry_ready.assign(side * side, 0);
        for (const SweepColumn& column : sweep_columns) {
            const int32_t ix = static_cast<int32_t>(column.dx) + old_rd;
            const int32_t iz = static_cast<int32_t>(column.dz) + old_rd;
            if (ix < 0 || ix >= old_side || iz < 0 || iz >= old_side) continue;
            const size_t index = static_cast<size_t>(iz) * side + static_cast<size_t>(ix);
            carry_bands[index] = column.band;
            carry_ready[index] = column.band_ready ? 1 : 0;
        }
    }
    // Created once, never reconfigured here. Every setter that changes the
    // terrain (seed, sea level, biome size, terrain params, biome config) already
    // pushes the new settings into the estimator AND invalidates the height cache
    // — which is what sends us back through this function — so reconfiguring it
    // again is not just redundant: it was 19 ms per chunk crossing, which is a
    // dropped frame every time the player crossed into a new chunk while flying.
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    if (column_height_cache.empty()) column_height_cache.reserve(65536);
    // A new list means a new set of columns to ask for, so the request scan
    // starts over. (Its "nothing left to ask for" verdict only holds for the list
    // it was reached on.)
    prefetch_scan = 0;
    prefetch_pass_requests = 0;
    prefetch_idle = false;
    prefetch_pass_fresh = true;
    // No vertical render distance: a column whose terrain sits far above or
    // below the player is still reachable, because what bounds generation is the
    // column's own content band, not a window around the player. The band is
    // computed ONCE per column here (see world/sweep_band.hpp) instead of being
    // re-tested for all 65 slices of every column while walking — which is where
    // 84% of a 208,585-entry walk was being thrown away.
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    current_render_distance = horizontal_rd;
    sweep_origin_cx = pcx;
    sweep_origin_cz = pcz;
    sweep_bands_dirty = false;
    sweep_columns.clear();
    // On a pure move the bands are still valid, and so is every "this whole
    // column is resident" mark: the loaded map only loses a column through
    // try_unload, which already erases that mark. Prune the columns that left
    // the disc rather than throwing the set away; on a full rebuild the set
    // goes with the bands it was earned against.
    if (can_carry) {
        for (auto it = built_columns.begin(); it != built_columns.end();) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(*it, cx, cy, cz);
            const int32_t dx = cx - pcx;
            const int32_t dz = cz - pcz;
            if (dx * dx + dz * dz > horizontal_rd * horizontal_rd) {
                it = built_columns.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        built_columns.clear();
    }
    sweep_band_frontier = 0;
    // Queued neighbours were keyed to positions the OLD list served; a rebuild
    // re-earns everything, so the chain starts clean rather than draining stale
    // offers through the new list's disc test.
    chain_queue.clear();
    chain_queued.clear();
    // A carried band is already read, so the candidate total starts at what
    // those bands account for; the frontier adds the entering rim as it reads
    // it. A full rebuild starts from zero and climbs as before.
    generation_stats.candidate_offsets = 0;
    // A move only has to read the entering rim; a full rebuild has the disc.
    sweep_band_budget_ms = can_carry ? 0.6 : 2.0;
    unload_queue.clear();

    for (int32_t dx = -horizontal_rd; dx <= horizontal_rd; ++dx) {
        for (int32_t dz = -horizontal_rd; dz <= horizontal_rd; ++dz) {
            if (dx * dx + dz * dz > horizontal_rd * horizontal_rd) continue;
            SweepColumn column{ static_cast<int16_t>(dx), static_cast<int16_t>(dz),
                                sweep::ChunkBand{}, false };
            if (can_carry) {
                // The same absolute column in the old disc: its band is still the
                // answer. An absolute column outside the old disc is new terrain
                // to this list and keeps band_ready = false for the frontier.
                const int32_t odx = pcx + dx - old_origin_cx;
                const int32_t odz = pcz + dz - old_origin_cz;
                if (odx >= -old_rd && odx <= old_rd && odz >= -old_rd && odz <= old_rd) {
                    const size_t index = static_cast<size_t>(odz + old_rd) *
                                         static_cast<size_t>(old_side) +
                                         static_cast<size_t>(odx + old_rd);
                    column.band = carry_bands[index];
                    column.band_ready = carry_ready[index] != 0;
                    if (column.band_ready) {
                        generation_stats.candidate_offsets += sweep::count(column.band);
                    }
                }
            }
            sweep_columns.push_back(column);
        }
    }

    // Nearest columns first — rings expand outward around the player. This is now
    // the whole ordering: the old list also sorted by vertical distance from the
    // player's slice, which only existed because it carried 62 unusable slices
    // per column. With those gone, the slice order inside a single column is the
    // only vertical question left, and the walk answers it (centre-out from the
    // player, see advance_sweep).
    std::sort(sweep_columns.begin(), sweep_columns.end(),
        [](const SweepColumn& a, const SweepColumn& b) {
            const int32_t ha = static_cast<int32_t>(a.dx) * a.dx + static_cast<int32_t>(a.dz) * a.dz;
            const int32_t hb = static_cast<int32_t>(b.dx) * b.dx + static_cast<int32_t>(b.dz) * b.dz;
            if (ha != hb) return ha < hb;
            if (a.dx != b.dx) return a.dx < b.dx;
            return a.dz < b.dz;
        }
    );

    // --- Resume at the rim ----------------------------------------------------
    // A column at new distance d was at old distance at most d + |move|, so any
    // column with d < r_walked - |move| lay inside the radius the walk had
    // already passed: skipping it is exact rather than an approximation, and the
    // columns between that and r_walked are re-walked (a ring or two). Leaving
    // the cursor at the end is the "spent" state the walk already understands,
    // for the case where the new list holds nothing beyond the walked radius.
    generation_pass_complete = false;
    generation_cursor = SweepCursor{};
    if (can_carry && walked_r2 >= 0 && small_move) {
        const double move_len =
            std::sqrt(static_cast<double>(move_dx * move_dx + move_dz * move_dz));
        const double resume_r =
            std::max(0.0, std::sqrt(static_cast<double>(walked_r2)) - move_len);
        const double resume_r2 = resume_r * resume_r;
        size_t resume = sweep_columns.size();
        for (size_t i = 0; i < sweep_columns.size(); ++i) {
            const SweepColumn& column = sweep_columns[i];
            const int32_t r2 = static_cast<int32_t>(column.dx) * column.dx +
                               static_cast<int32_t>(column.dz) * column.dz;
            if (static_cast<double>(r2) >= resume_r2) {
                resume = i;
                break;
            }
        }
        generation_cursor.column = resume;
        ++generation_stats.cursor_resumes;
    } else {
        ++generation_stats.cursor_resets;
    }
    generation_stats.candidate_columns = sweep_columns.size();
    const std::chrono::duration<double, std::milli> rebuild_elapsed =
        std::chrono::steady_clock::now() - rebuild_start;
    generation_stats.last_rebuild_ms = rebuild_elapsed.count();
    generation_stats.total_rebuild_ms += rebuild_elapsed.count();
    generation_stats.max_rebuild_ms = std::max(generation_stats.max_rebuild_ms, generation_stats.last_rebuild_ms);
}

} // namespace VoxelEngine
