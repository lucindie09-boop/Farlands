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
                                        bool& generated_anything) {
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    int32_t generations = 0;
    while (generations < budget && !chain_queue.empty()) {
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
        const bool fill_column = std::abs(dx) <= kUndergroundFillRadius &&
                                 std::abs(dz) <= kUndergroundFillRadius;
        const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
        if (!sweep::chunk_in_band(cy, surface.land_h, surface.top_h, fill_column)) {
            ++generation_stats.chain_skipped;
            continue;
        }
        if (generate_chunk(cx, cy, cz, epoch)) {
            ++generations;
            ++generation_stats.chain_generations;
            // Attribution for the vertical arm: does it generate what it offers?
            // (Seed-time counted the offer; this completes the pair. A vertical
            // offer is always exactly one ABOVE its seeder, so "the offer's own
            // slice is above the seeder's" is the same test without carrying the
            // seeder's y through the queue.)
            generated_anything = true;
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
    return generations;
}

void WorldUpdater::refresh_prefetch_config() {
    if (!column_prefetch) return;
    ColumnPrefetch::Config config;
    config.terrain    = terrain_params;
    config.biomes     = biome_config;
    config.vegetation = vegetation_config;
    column_prefetch->set_config(config);
}

bool WorldUpdater::enqueue_column_prefetch(int32_t cx, int32_t cz, uint32_t epoch) {
    if (!column_prefetch || thread_pool == nullptr) return false;
    const uint64_t key = ColumnPrefetch::key_of(cx, cz);
    if (!column_prefetch->request(key, epoch)) return false;
    // The task holds the prefetch state, not this updater: it can still be queued
    // or running when the world is torn down, and must not reach back into memory
    // that has gone.
    std::shared_ptr<ColumnPrefetch> state = column_prefetch;
    thread_pool->fire_and_forget([state, key, epoch, cx, cz] {
        // One generator per thread, configured from the published copy of the
        // terrain configuration — the same shape the generation workers use, and
        // for the same reason: never shared, and never the main thread's, whose
        // setters mutate it while this runs.
        static thread_local ChunkGenerator generator;
        static thread_local ColumnPrefetch::Config config;
        static thread_local uint32_t seen_epoch = 0;
        bool copied = false;
        if (!state->worker_config(epoch, seen_epoch, config, copied)) return;
        if (copied) {
            generator.set_params(config.terrain);
            generator.set_biome_config(config.biomes);
            generator.set_vegetation_config(config.vegetation);
        }
        const ChunkGenerator::HeightRange range = generator.get_chunk_height_range(cx, cz);
        // Same helper the main thread's fallback path uses, so an answer cannot
        // depend on which thread produced it.
        const WorldUpdater::ColumnSurfaceBounds b =
            bounds_of_height_range(range.min_h, range.max_h, range.max_water_h);
        state->publish(key, epoch, ColumnBounds{b.land_h, b.top_h});
    });
    return true;
}

void WorldUpdater::pump_column_prefetch() {
    if (!column_prefetch || thread_pool == nullptr) return;
    if (sweep_columns.empty() || prefetch_idle) return;
    // Two bounds on what is offered: only a slice of the list is scanned per
    // frame, and only so many requests may be out at once. The scan is
    // deliberately independent of the frontier, because the columns the frontier
    // has no bounds for are not the ones just ahead of it — after a crossing they
    // are the ring that just entered the disc, which sits at the END of a
    // nearest-first list, and the frontier would reach them only after re-reading
    // every column already known (all ~3,200 of them) on the way.
    constexpr size_t kScanPerFrame = 1024;
    constexpr size_t kMaxOutstanding = 256;
    size_t outstanding = column_prefetch->outstanding();
    if (outstanding >= kMaxOutstanding) return;
    const uint32_t epoch = column_prefetch->epoch();
    const size_t list_size = sweep_columns.size();
    // The first pass over a freshly built list covers ALL of it, rather than a
    // slice per frame. The frontier starts at position 0 on that same frame and can
    // reach the far end — where the ring that just entered the disc sits, since the
    // list is nearest-first — before the next frame, so a request made a slice at a
    // time arrives after the frontier has already asked for the column and derived
    // it here. Measured with the benchmark: slicing every pass left 1,910 of 4,848
    // entered columns derived on this thread (39%), i.e. two fifths of the work this
    // exists to move was still being paid on the main thread. Later passes only
    // replace what has been claimed, so they stay on the small budget.
    const size_t scan_budget = prefetch_pass_fresh ? list_size : kScanPerFrame;
    size_t scanned = 0;
    while (scanned < scan_budget) {
        if (prefetch_scan >= list_size) {
            // A whole pass over the list is done. One that wanted nothing will
            // keep wanting nothing until the list is rebuilt, so stop scanning
            // rather than re-reading the same 3,209 positions every frame.
            if (prefetch_pass_requests == 0) {
                prefetch_idle = true;
                return;
            }
            prefetch_pass_requests = 0;
            prefetch_scan = 0;
        }
        const SweepColumn& column = sweep_columns[prefetch_scan++];
        ++scanned;
        const int32_t cx = sweep_origin_cx + column.dx;
        const int32_t cz = sweep_origin_cz + column.dz;
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                           |  static_cast<uint64_t>(static_cast<uint32_t>(cz));
        // Already derived: the crossing that entered this ring re-requests the
        // whole list, and all but the ~130 columns that just appeared are cached.
        if (column_height_cache.find(key) != column_height_cache.end()) continue;
        if (enqueue_column_prefetch(cx, cz, epoch)) {
            ++prefetch_pass_requests;
            ++outstanding;
            if (outstanding >= kMaxOutstanding) break;
        }
    }
    prefetch_pass_fresh = false;
}

void WorldUpdater::service_sweep_bands() {
    // Top the requests up before the frontier, so a column entering the disc
    // usually has its answer waiting by the time the frontier gets to it.
    pump_column_prefetch();
    if (sweep_band_frontier >= sweep_columns.size()) return;  // caught up
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    // Two milliseconds of a 16 ms frame, always at least one column so the
    // frontier can never stall on a column slower than the budget. The list is
    // nearest-first, so this always banks the terrain the player is standing in
    // before anything far away.
    constexpr double kBandBudgetMs = 2.0;
    const auto start = std::chrono::steady_clock::now();
    double band_ms = 0.0;
    uint64_t columns_read = 0;
    double worst_column_ms = 0.0;
    double worst_bounds_ms = 0.0, worst_resident_ms = 0.0;
    while (sweep_band_frontier < sweep_columns.size()) {
        const double before_column_ms = band_ms;
        SweepColumn& column = sweep_columns[sweep_band_frontier];
        const int32_t cx = sweep_origin_cx + column.dx;
        const int32_t cz = sweep_origin_cz + column.dz;
        const double before_bounds = band_ms;
        const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
        const bool fill_column = std::abs(static_cast<int32_t>(column.dx)) <= kUndergroundFillRadius &&
                                 std::abs(static_cast<int32_t>(column.dz)) <= kUndergroundFillRadius;
        column.band = sweep::band_for_column(surface.land_h, surface.top_h, fill_column, kWorldChunkSlices);
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
            out.fill_column = std::abs(static_cast<int32_t>(column.dx)) <= kUndergroundFillRadius &&
                              std::abs(static_cast<int32_t>(column.dz)) <= kUndergroundFillRadius;
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
    // Every band in the new list is unknown, and a column's contents may have
    // changed under the old marks, so the whole set goes with the list and is
    // re-earned as the frontier reads each new band.
    built_columns.clear();
    sweep_band_frontier = 0;
    // Queued neighbours were keyed to positions the OLD list served; a rebuild
    // re-earns everything, so the chain starts clean rather than draining stale
    // offers through the new list's disc test.
    chain_queue.clear();
    chain_queued.clear();
    // Every band in the new list is unknown, so the candidate total starts over
    // and climbs as the frontier reads them (it reaches the true total within a
    // frame or two once the heights are cached, and over a couple of seconds on
    // the very first build, where every one of them is cold).
    generation_stats.candidate_offsets = 0;
    unload_queue.clear();

    for (int32_t dx = -horizontal_rd; dx <= horizontal_rd; ++dx) {
        for (int32_t dz = -horizontal_rd; dz <= horizontal_rd; ++dz) {
            if (dx * dx + dz * dz > horizontal_rd * horizontal_rd) continue;
            // A column's band is read later, in list order (service_sweep_bands).
            // Columns whose band turns out to be empty are simply walked past.
            sweep_columns.push_back(SweepColumn{ static_cast<int16_t>(dx),
                                                 static_cast<int16_t>(dz),
                                                 sweep::ChunkBand{},
                                                 false });
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

    generation_stats.candidate_columns = sweep_columns.size();
    const std::chrono::duration<double, std::milli> rebuild_elapsed =
        std::chrono::steady_clock::now() - rebuild_start;
    generation_stats.last_rebuild_ms = rebuild_elapsed.count();
    generation_stats.total_rebuild_ms += rebuild_elapsed.count();
    generation_stats.max_rebuild_ms = std::max(generation_stats.max_rebuild_ms, generation_stats.last_rebuild_ms);
}

} // namespace VoxelEngine
