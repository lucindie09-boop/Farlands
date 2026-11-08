#ifndef FARLANDS_FRAME_BUDGETS_HPP
#define FARLANDS_FRAME_BUDGETS_HPP

#include <cstddef>
#include <cstdint>

namespace VoxelEngine {

struct FrameBudgets {
    int32_t chunk_generations = 64;
    int32_t chunk_completions_initial = 128;
    int32_t chunk_completions_gameplay = 64;
    int32_t mesh_rebuilds_immediate = 4;
    int32_t mesh_rebuilds_idle = 4;
    int32_t mesh_rebuilds_active = 8;
    int32_t mesh_rebuilds_loading = 64;
    int32_t mesh_uploads_idle = 2;
    int32_t mesh_uploads_active = 32;
    int32_t mesh_uploads_loading = 64;

    size_t completed_queue_backlog = 512;
    size_t dirty_mesh_backlog = 512;
    size_t worker_queue_backlog = 512;

    int32_t max_loaded_chunks = 50000;

    double processing_budget_ms = 0.75;
    double mesh_completion_budget_ms = 0.5;
    // The dirty chunk queue and the far-region scheduler share one wall-clock
    // budget. They used to get 0.75 ms each, so a crossing could spend 1.5 ms in
    // DirtyMeshQueue alone; the immediate queue is inside this budget too.
    double mesh_queue_budget_ms = 0.35;
    // Initial streaming is allowed to spend more on the mesh queue than live
    // play. The 0.35 ms budget is a border-smoothness budget; with it the 64
    // loading rebuilds below can only schedule a handful, which is why the
    // far/mesh tail outlived the chunk load by five seconds.
    // 2.0 -> 4.0 measured and reverted: the tail ran 4.05/3.78 s against 3.55 s
    // and wall 8.25/8.22 against 8.05, because the call never spends its 2 ms -
    // it ends on the pop cap. Stamping entries to drop unchanged ones fired 52
    // times in 24,600 pops, so the cost was not stale work either. 91% of the
    // phase was the buried test's worldgen probes; those are memoised now.
    double mesh_queue_loading_budget_ms = 2.0;
    // The generation ring walk used to run to its full count/check budget; on
    // a crossing that was 3.15 ms in one frame. Enqueueing is main-thread work,
    // so cap it in wall time like the other phases.
    double generation_budget_ms = 1.0;
    // The incremental far-region visibility walk. The full walk cost ~2.4 ms on
    // every crossing frame; a slice this size keeps the same work in the frame
    // but out of one frame's critical path.
    double far_region_visibility_budget_ms = 0.1;
    int32_t max_mesh_completions_per_frame = 16;
    int32_t loading_threshold = 500;

    // Entries the unload scan visits per pass. It walks the whole map, so this is
    // what decides how long a loaded set takes to be re-examined at all: 500 per
    // pass against a 100k-chunk set on a 15-frame skip is a ~50 s sweep, far longer
    // than the interval in which the moving disc makes chunks leave range. Each
    // visit is a decode, a distance test and a hash-set lookup under one shard's
    // SHARED lock (see ChunkMap::for_each_limited_resumable — no longer lock_all),
    // so a visit is tens of nanoseconds and the scan can afford to be thorough.
    int32_t unload_checks_per_frame = 512;
    int32_t unloads_per_frame = 200;
    int32_t max_generation_checks_per_frame = 100000;
    // 2 per worker is both the pressure controller's scale (30 in flight at 15
    // workers) and, with the admission cap at 180, what keeps the stream from
    // bursting. Retested at 3 on top of the 180 cap: pressure stays below 0.75
    // until ~34 in flight, so the walk's budget stays at 64, the set peaks
    // 139/155 instead of 112/106, completions arrive 208/251 per drain instead of
    // 123/126, gate stops return (8/9 -> 74/97) and the load runs 5.60/5.90 s
    // against 5.04/5.30 s. More work in larger, later arrivals, not more of it.
    int32_t generating_per_worker = 2;
    // Admission control for the in-flight generation set. Pressure above scales
    // the per-frame generation budget DOWN, but nothing stopped the set itself from
    // growing: generations were enqueued every frame until the completed queue hit
    // `completed_queue_backlog` (512), which is how a flight reached 6,524 chunks in
    // flight at once. That backlog is what fills the completed queue in bursts (the
    // install phase's 143 ms frames), keeps the column prefetch's answers behind
    // thousands of generation tasks (8,863 columns derived on the main thread at
    // ~181 us each), and makes the walk burn its check budget on refusals. Capping
    // the set keeps the pipeline full — a worker is 1 ms of work, so a few dozen in
    // flight saturate 15 workers — without letting it grow into a backlog.
    // 6 per worker (90 in flight) under the feed-forward rate above. The shallow
    // cap is what makes the rate work: it keeps the completed queue short enough
    // that every drain iteration stays cheap. RD 32 fill 4.44/4.46 s against
    // 4.75/4.51 at 12 per worker under the pressure law, wall 7.72/7.76 s against
    // 8.29/8.32, drain 33/37 ms against 62/46. The 12-per-worker numbers below are
    // the historical comparison.
    // 12 per worker (180 in flight at 15 workers). At 8 the gate closed in ~12% of
    // an RD 32 load's frames and the load ran 6.41 s; at 12 it closed in ~1% and the
    // load ran 5.04/5.30 s, load frame 9.76 -> 8.23/7.93 ms, slow frames 80 -> 18/21.
    // The set still averages ~30 and peaks ~110, so this bounds the tail rather than
    // pre-loading the pool with thousands of chunks.
    int32_t max_generating_in_flight_per_worker = 6;

    double flush_interval = 5.0;
};

} // namespace VoxelEngine

#endif // FARLANDS_FRAME_BUDGETS_HPP
