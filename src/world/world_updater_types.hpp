#ifndef FARLANDS_WORLD_UPDATER_TYPES_HPP
#define FARLANDS_WORLD_UPDATER_TYPES_HPP
#include <array>
#include <cstddef>
#include <cstdint>

// The sweep's counters, split out of world_updater.hpp: they are a reporting
// concern with a rolling window of their own, and the file they came from is
// mostly scheduling logic.
namespace VoxelEngine {

// Counters for the generation sweep, read by /genstats.
struct GenerationStats {
    uint64_t frames           = 0;  // update_generation calls
    uint64_t candidate_offsets = 0; // candidate CHUNKS in the built sweep list
    uint64_t candidate_columns = 0; // columns that list covers
    uint64_t cursor_resets    = 0;  // walks restarted by a chunk crossing

    // Phase 2, the distance-ordered sweep. `checks` is the number of
    // candidate offsets it examined; the reject counters sum to
    // checks - band_pass, since every examined offset takes one path.
    uint64_t checks            = 0;
    uint64_t band_pass         = 0;  // passed every filter, generation attempted
    uint64_t generations       = 0;  // generate_chunk enqueued it
    uint64_t generate_refused  = 0;  // generate_chunk declined (in flight / backlog)
    uint64_t reject_loaded     = 0;  // chunk already in the map
    // Candidates the walk passed over because the chunk is ALREADY being
    // generated, learned from ChunkScheduler's lock-free filter instead of by
    // asking (which costs a global mutex plus a shard lock, and is the reason
    // these used to show up as `generate_refused`). Counted separately from
    // `checks` because they do not consume the check budget: the point is that
    // those checks go to candidates that are not already on their way.
    uint64_t reject_inflight   = 0;
    uint64_t reject_above      = 0;  // entirely above the column's content
    uint64_t reject_below      = 0;  // entirely below the band (band-only columns)
    uint64_t reject_oob        = 0;  // outside [0, kWorldChunkSlices)
    uint64_t sweeps_completed  = 0;  // full walks that found nothing left to do

    // Columns whose whole band was already resident when the band was read,
    // and how many times the walk has since skipped one. `skipped` growing
    // while `checks` stays flat is the shape of the waste this removes: the
    // walk re-confirming chunks that exist (97.8% of phase 2 before it).
    uint64_t columns_built   = 0;
    uint64_t columns_skipped = 0;

    // Phase 1, the frustum pass. RETIRED — the chain queue replaced it (see
    // the chain comment above); the counters and their /genstats line stay for
    // the A/B record but nothing writes them any more.
    uint64_t frustum_checks      = 0;
    uint64_t frustum_visible     = 0;  // inside the frustum (BEFORE the loaded test)
    uint64_t frustum_loaded      = 0;  // inside the frustum and already resident
    uint64_t frustum_band_pass   = 0;
    uint64_t frustum_generations = 0;
    // Passed every filter and was refused anyway — the chunk is already in
    // flight. Counted because it is the one refusal that is NOT waste
    // discovered by a bug: it is this pass asking again for something it
    // asked for on a previous frame, so it measures exactly how much of the
    // pass's budget goes on work already under way.
    uint64_t frustum_refused     = 0;
    uint64_t frustum_inflight    = 0;  // skipped without the locks (see reject_inflight)

    // The chain queue (the current priority): candidates offered, filters
    // they passed, and generations they won. `chain_seeds` counts the
    // listener calls that queued at least one neighbour.
    uint64_t chain_offered    = 0;
    uint64_t chain_generations = 0;
    uint64_t chain_refused    = 0;
    uint64_t chain_skipped    = 0;  // already loaded or in flight when drained
    uint64_t chain_seeds      = 0;
    // How many of the offered candidates were VERTICAL neighbours, and how
    // many of those generated. The vertical arm exists for trees and
    // overhangs, which cross chunk borders upward where the horizontal ring
    // already covers the surface band; if `generated` does not follow
    // `vertical_offered`, the arm is pure queue traffic and should go.
    uint64_t chain_vertical_offered = 0;
    uint64_t chain_vertical_generated = 0;

    // Urgent requests (a paste waiting on chunks). These bypass the sweep's
    // filters, so they are counted separately rather than as sweep work.
    uint64_t urgent_requested = 0;
    uint64_t urgent_generated = 0;

    // How tall the bands actually are, per column READ (not per column in the
    // list): `band_size_hist` buckets `count(band)` over the 32 world slices.
    // The mean is candidate_offsets/candidate_columns, but a mean hides the
    // shape — a flat column's 2-slice band and a cliff's 12-slice band average
    // into a number that describes neither. Fill columns (inside
    // kUndergroundFillRadius) are counted separately: their band reaches the
    // world floor by design and would swamp the histogram.
    static constexpr size_t kBandBuckets = 8;
    std::array<uint64_t, kBandBuckets> band_size_hist{};
    uint64_t band_max_slices   = 0;
    uint64_t fill_columns_read = 0;
    uint64_t fill_band_slices  = 0;

    // Every install reaches WorldUpdater::on_chunk_installed, empty or not, so
    // that listener is where "what did that generation actually produce" is
    // countable. Each install is classified against its column's content
    // bounds [land_h, top_h]: a chunk wholly ABOVE top_h or BELOW land_h could
    // only be offered because of the band's ±32-block pad (or the fill rule),
    // and if it installed EMPTY, that padding bought nothing. A chunk
    // intersecting the bounds is the estimate's own slack rather than the
    // pad's, and `bounds_unknown` counts installs whose column was never read
    // (nothing to classify against; not split by emptiness).
    uint64_t installs_total             = 0;
    uint64_t installs_empty             = 0;
    uint64_t installs_above_top         = 0;
    uint64_t installs_above_top_empty   = 0;
    uint64_t installs_below_land        = 0;
    uint64_t installs_below_land_empty  = 0;
    uint64_t installs_in_range          = 0;
    uint64_t installs_in_range_empty    = 0;
    uint64_t installs_bounds_unknown    = 0;

    // Rolling window of the last kWindowFrames frames. A session total is
    // dominated by the initial load; mid-flight the question is what the
    // sweep costs *now*, and the two are wildly different numbers.
    static constexpr size_t kWindowFrames = 120;
    std::array<uint32_t, kWindowFrames> window_checks{};
    std::array<uint32_t, kWindowFrames> window_generations{};
    size_t   window_head   = 0;
    uint32_t window_frames = 0;

    // Wall time inside update_generation, milliseconds.
    double total_ms = 0.0;
    double last_ms  = 0.0;
    double max_ms   = 0.0;

    // Of that, the part spent rebuilding the sweep list. The rebuild reads one
    // band per column, so it is the only part of a walk whose cost is per
    // COLUMN rather than per candidate, and the only part that can hitch.
    uint64_t rebuilds         = 0;
    double   last_rebuild_ms  = 0.0;
    double   total_rebuild_ms = 0.0;
    double   max_rebuild_ms   = 0.0;

    // Of the walk, the part spent reading column bands (the frontier), and
    // how many columns that covers. This is the only per-COLUMN cost in the
    // sweep and the only one that needs a per-frame budget.
    uint64_t band_reads       = 0;
    double   total_band_ms    = 0.0;
    double   max_band_ms      = 0.0;
    // How many columns the worst band frame read. The budget is checked after
    // every column, so a frame cannot overshoot by more than ONE column's cost
    // — which makes this the number that says whether a fat band frame is many
    // columns (the budget is not doing its job) or a single column that stalled
    // (something outside this loop held the thread).
    uint64_t max_band_columns = 0;
    // The slowest single column ever read. If the worst band frame is large
    // AND this is large, one column stalled (the budget is fine and something
    // outside this loop took the thread); if the worst frame is large and this
    // is small, thousands of cheap columns ran past the budget.
    double   max_band_column_ms = 0.0;
    // What that slowest column was DOING, recorded together because the parts
    // only mean something for the column that was slow: its bounds read (cache
    // hit, cold range, or claimed prefetch answer) and its resident check —
    // ONE `ChunkMap::lock_column` acquisition (all of a column's chunks share
    // a shard) followed by lock-free probes. Whatever is left over is neither
    // of those — the bookkeeping is arithmetic, so a large remainder is this
    // thread being taken away rather than any code in this loop.
    double   max_band_bounds_ms   = 0.0;
    double   max_band_resident_ms = 0.0;
    // The slowest single column ACQUISITION of the session. That acquisition
    // is the only part of a resident check that can wait: a value near the
    // column's whole cost means it waited on the generation workers that write
    // the same map, while a small value beside a large column means the stall
    // was between the statements rather than inside one.
    double   max_contains_ms      = 0.0;
    // The slowest single cold bounds derivation. If a fat band frame coincides
    // with a fat value here, the rigorous height range is what stalled; if not,
    // that frame was spent after the range (the resident lookups) or this thread
    // was taken away entirely.
    double   max_cold_bounds_ms = 0.0;

    // Columns whose bounds a worker produced ahead of the frontier, and the
    // band reads that consumed one. With the prefetch running, `band_reads`
    // stays where it is while the cold part of each one moves off this
    // thread, so `total_band_ms`/`max_band_ms` are the measurement that says
    // whether that happened; `prefetch_taken` is the counter that proves a
    // band read was taken from a ready answer rather than computed here.
    uint64_t prefetch_taken   = 0;
    // Columns whose bounds THIS thread had to derive, which is the number the
    // prefetch exists to drive to zero. A column is counted once, on the pass
    // that first needed it: later passes hit the column cache instead.
    uint64_t cold_bounds      = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_UPDATER_TYPES_HPP
