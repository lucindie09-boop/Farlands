// What a column's surface bounds are, who derives them, and how a worker's
// prefetched answer is claimed: the bounds cache the sweep's band filter reads,
// the one definition of the bounds themselves, and the install listener that
// seeds the generation chain. Kept apart from world/world_updater.cpp, whose
// per-frame ordering reads these and never computes them.

#include "world/world_updater.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include <algorithm>
#include <chrono>

namespace VoxelEngine {
using namespace godot;

WorldUpdater::ColumnSurfaceBounds WorldUpdater::get_column_surface_bounds(int32_t cx, int32_t cz) {
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                 | static_cast<uint64_t>(static_cast<uint32_t>(cz));
    auto it = column_height_cache.find(key);
    if (it != column_height_cache.end()) {
        return it->second;
    }
    // A worker already derived these bounds: claim its answer rather than spend
    // ~181 us of this thread on the same lattice. This is the whole point of the
    // prefetch, and `prefetch_taken` counts it so /genstats can show whether the
    // requests are landing ahead of the frontier or behind it.
    if (column_prefetch) {
        ColumnBounds ready;
        if (column_prefetch->try_take(key, ready)) {
            ColumnSurfaceBounds b;
            b.land_h = ready.land_h;
            b.top_h  = ready.top_h;
            store_column_bounds(key, b);
            ++generation_stats.prefetch_taken;
            return b;
        }
    }
    // Rigorous content bounds over the WHOLE chunk area (all 4-block lattice
    // nodes, not just the center column). On steep terrain a biome border or
    // mountain wall can cross a chunk while the center column sits far below
    // the local high side — a single center sample then understates the top,
    // and the scheduler permanently skips chunks that genuinely contain the
    // wall, leaving invisible-solid holes (no mesh, no data to place into).
    // This range already pads by ChunkGenerator::density_margin(), so it bounds every column's
    // real content; land_h is the lowest possible surface (everything below is
    // solid rock), top_h the highest (air above, with water to sea level).
    // Timed only on the cold path, so the clock reads cost nothing in the common
    // case. This is the number that says whether a band frame that blew its budget
    // was spent in here (the rigorous height range really did take that long) or
    // somewhere after it (the resident lookups, or this thread being taken away).
    const auto range_start = std::chrono::steady_clock::now();
    const ChunkGenerator::HeightRange range = height_estimator->get_chunk_height_range(cx, cz);
    generation_stats.max_cold_bounds_ms = std::max(generation_stats.max_cold_bounds_ms,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - range_start).count());
    ++generation_stats.cold_bounds;
    const ColumnSurfaceBounds b = bounds_of_height_range(range.min_h, range.max_h, range.max_water_h);
    store_column_bounds(key, b);
    return b;
}

const WorldUpdater::ColumnSurfaceBounds* WorldUpdater::peek_column_surface_bounds(int32_t cx,
                                                                                  int32_t cz) const {
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                       | static_cast<uint64_t>(static_cast<uint32_t>(cz));
    auto it = column_height_cache.find(key);
    return it == column_height_cache.end() ? nullptr : &it->second;
}

// The one definition of "what a column's bounds are", so the main thread's
// fallback path and a worker's prefetched answer cannot disagree about them.
WorldUpdater::ColumnSurfaceBounds WorldUpdater::bounds_of_height_range(float min_h, float max_h,
                                                                      float max_water_h) {
    ColumnSurfaceBounds b;
    b.land_h = min_h;
    // Water is content too: an ocean chunk is air above the sea floor but not
    // above sea level, and the band filter has to keep those slices.
    b.top_h  = std::max(max_h, max_water_h);
    return b;
}

void WorldUpdater::store_column_bounds(uint64_t key, const ColumnSurfaceBounds& b) {
    if (column_height_cache.size() >= 65536) {
        uint64_t oldest = column_height_fifo.front();
        column_height_fifo.pop_front();
        column_height_cache.erase(oldest);
    }
    column_height_cache[key] = b;
    column_height_fifo.push_back(key);
}

void WorldUpdater::invalidate_height_cache() {
    column_height_cache.clear();
    column_height_fifo.clear();
    // The prefetched answers come from that same cache's inputs, so they go with
    // it: republishing the configuration retires every request in flight.
    refresh_prefetch_config();
    // The sweep list carries a slice range per column, read from this cache, so a
    // change of seed, sea level, terrain params or biome config invalidates the
    // list with it — otherwise the sweep would keep generating against bands from
    // the previous terrain.
    sweep_bands_dirty = true;
}

void WorldUpdater::on_chunk_installed(int32_t cx, int32_t cy, int32_t cz, bool has_blocks) {
    if (!has_blocks) return;
    ++generation_stats.chain_seeds;
    // One set insert per neighbour pair is the whole cost here; the dedupe set
    // keeps a chunk surrounded by built neighbours from being offered once per
    // one of them.
    const ChunkMap& map = chunk_world->get_chunk_map();
    // Horizontal first — the surface band spreads sideways. Then UP: terrain
    // legitimately crosses a border upward (trees, overhangs, snow caps), and a
    // chunk whose top content could reach into the chunk above is the only
    // vertical neighbour worth offering. DOWN is left to the ring walk: the
    // fill rule already brings full columns down inside the player radius, and
    // outside it the walk's band decides — seeding down from every terrain
    // chunk would queue the world's bedrock. The bound is the column's cached
    // content top: is there anything this chunk can PUT in the chunk above?
    // Vertical arm TEMPORARILY OFF: 18.4k offers per flight, ZERO generations.
    // See the vertical_offered/vertical_generated counters — this terrain has no
    // overhangs, so the arm is pure queue traffic here. Re-enable when terrain
    // that crosses borders upward exists (trees, caps); the gate is one bool.
    const bool kVerticalChain = false;
    const bool spread_up = kVerticalChain && [&]() {
        if (static_cast<uint32_t>(cy + 1) >=
            static_cast<uint32_t>(WORLD_HEIGHT_Y / CHUNK_HEIGHT)) return false;
        // The column's CACHED bounds answer "can this chunk put anything into
        // the chunk above?" — peeked, never computed: seeding is per-install
        // and must stay a few hash lookups, not a cold 181 us range. Unknown
        // bounds spread (the drain's band filter is the real gate anyway); the
        // band filter pads by +32, so the same slack here means this check only
        // declines columns whose top is a chunk BELOW the offer — pure sky.
        const ColumnSurfaceBounds* bounds = peek_column_surface_bounds(cx, cz);
        if (bounds == nullptr) return true;  // unknown: let the drain decide
        const float chunk_top = static_cast<float>((cy + 1) * CHUNK_HEIGHT);
        return chunk_top < bounds->top_h + 32.0f;
    }();
    uint64_t neighbours[5];
    size_t n = 0;
    neighbours[n++] = map.get_chunk_key(cx - 1, cy, cz);
    neighbours[n++] = map.get_chunk_key(cx + 1, cy, cz);
    neighbours[n++] = map.get_chunk_key(cx, cy, cz - 1);
    neighbours[n++] = map.get_chunk_key(cx, cy, cz + 1);
    if (spread_up) neighbours[n++] = map.get_chunk_key(cx, cy + 1, cz);
    const size_t horizontal_count = 4;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t key = neighbours[i];
        // A neighbour whose COLUMN is already fully built needs no offer — and
        // this lookup is free next to the locked `contains` the drain would pay
        // to discover the same thing (87% of the first flight's offers were
        // skipped as already-loaded, each one a shard lock). `built_columns` is
        // main-thread-only, and this listener runs on the main thread.
        int32_t nx = 0, ny = 0, nz = 0;
        ChunkMap::decode_chunk_key(key, nx, ny, nz);
        if (built_columns.count(map.get_chunk_key(nx, 0, nz)) != 0) continue;
        // Already being generated: an offer would be skipped at the drain, and
        // this filter is three relaxed atomic loads.
        if (chunk_world->get_scheduler().may_be_generating(key)) continue;
        if (chain_queued.insert(key).second) {
            chain_queue.push_back(key);
            // Counted at seed time, where the vertical decision was made, so the
            // counters show whether the arm earns its queue traffic.
            if (i >= horizontal_count) ++generation_stats.chain_vertical_offered;
        }
    }
    (void)horizontal_count;
}

} // namespace VoxelEngine
