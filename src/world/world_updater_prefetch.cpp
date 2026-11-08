// The off-thread producer of per-column content bounds: the request sent to the
// pool, the worker that derives a column's height range, and the main-thread scan
// that keeps it fed. Kept apart from world/world_updater_streaming.cpp, whose
// subject is the sweep list and the walk that drains it.

#include "world/world_updater.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include "core/thread_pool.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

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

} // namespace VoxelEngine
