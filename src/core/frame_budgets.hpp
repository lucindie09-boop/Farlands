#ifndef FARLANDS_FRAME_BUDGETS_HPP
#define FARLANDS_FRAME_BUDGETS_HPP

#include <cstddef>
#include <cstdint>

namespace VoxelEngine {

// The admission controller's per-chunk price as a function of the pool's size.
// Contention is what makes a chunk dear, so the price falls with the pool: the
// 15-worker figure (2.46 ms) is the measured ceiling and the five-worker price
// is 1.1 ms, re-measured after the chunk itself got cheaper this session (the
// density-band skip and the edit-key scan). Swept with the env switch on the
// 5-worker arm, 3 alternating pairs each: 1.5 gave -0.05..-0.10 s of settle 3/3,
// 1.3 -0.06..-0.23 3/3, 1.1 -0.13..-0.29 3/3, and 0.9 stopped paying (-0.27,
// +0.04, -0.41 - 2/3 with a regression), so 1.1 is the knee. The old 1.8 was
// measured when a chunk was dearer (settle 6.695 -> 6.299 s, loaded 7.48 ->
// 7.19 s, 3/3 pairs). The 11-worker point is a straight-line interpolation
// (1.92 ms) and the obvious guess that it should sit at the 2.46 ceiling was
// tested: env probes liked 2.46 there (settle 3.70/3.66 s against 3.79/3.75 at
// 1.92, 2/2, with 1.4 at 3.83/3.81 and 1.1 at 4.09/4.10), but five binary pairs
// came out 2/5 - mean 3.745 s against 3.713 s, a wash - so the line is left
// where it is. A pool of 15 or more keeps the measured 2.46: re-pricing that
// end at 1.8 was a wash (settle A-B -0.015 s, faster 2/5). Re-swept after the
// all-air sky fast path took a chunk from 1.46 to 1.19 ms of worker work: 1.92
// gave 3.735/3.738 s, 1.5 gave 4.124/3.951 and 1.3 gave 4.278/4.512, so a lower
// price still over-admits and the interpolation stands. The price is not a
// free parameter to lower when chunks get cheaper - the contention it buys is
// dearer than the work it hides. A six-shot-per-arm sweep after the column cut
// (a chunk now 0.94 ms) put 1.92 at 3.672 s mean / 3.660 median, 1.7 at
// 3.696 / 3.630 and 2.2 at 3.813 / 3.821: the first two are tied inside noise
// and the dearer one loses, so the interpolation stands. Re-swept across the
// whole range after the loading clock went to 6.0 and the install path stopped
// being the wall, because an 11-worker profile made the price look like the new
// stop (install 1.08 ms against a 6 ms clock, the pool ~37% idle, the walk
// capped in 337 of 545 frames). Four shots an arm: 15 gave 1.5 at 3.859/3.993 s
// and 1.0 at 4.289/3.906 against 3.652/4.179/3.419 at the line's 2.46, and 13
// gave 1.7 at 4.036/4.015/4.084/3.275 and 1.3 at 3.721/4.223/3.960/3.224 against
// 4.314/4.087/3.536/3.362 at 2.188 -- a tie at 13, no win at 15, and spreads of
// 0.8-1.0 s inside every arm. At 11 the arm opened 2/2 for 1.2 (2.833/3.085
// against 3.081/3.304) but reached 2.833/3.085/3.175/4.016, mean 3.277 against
// the default's 3.193, so that was host drift. The line stands, and the finding
// is that the price is no longer a lever at this clock: admitting more a frame
// does not shorten the fill with a third of the pool idle, so a shorter fill has
// to come from the main thread's per-frame work.
[[nodiscard]] inline constexpr double generation_work_per_chunk_ms_for(std::size_t workers) noexcept {
    constexpr double kLowWorkers = 5.0;
    constexpr double kHighWorkers = 15.0;
    constexpr double kLowMs = 1.1;
    constexpr double kHighMs = 2.46;
    const double w = static_cast<double>(workers);
    if (w <= kLowWorkers) return kLowMs;
    if (w >= kHighWorkers) return kHighMs;
    return kLowMs + (kHighMs - kLowMs) * (w - kLowWorkers) / (kHighWorkers - kLowWorkers);
}

// The cap on the in-flight generation set. `workers x per_worker` is the general
// rule and the floor only binds where the pool is small enough for it to - which
// is where it was measured, by sweeping `FARLANDS_GEN_MAX_INFLIGHT`: at 5 workers
// (old floor 64) 30 in flight ran settle 6.47/6.38 s against 5.56/5.53 at 64, 90
// ran 5.24/5.19 and 128 ran 5.14/5.17 with loaded 5.97/5.99 against 6.46/6.40;
// at 11 workers (per-worker term 66) 128 ran 3.74/3.58 against 4.68/4.07, loaded
// 4.69/4.45 against 5.74/4.92. Both 2/2, so the floor is 128 up to 12 workers. At
// 15 workers, where the term already gives 90, 128 was a wash - settle 3.63/3.84
// against 3.71/3.72, loaded 0.02-0.23 s worse - so the per-worker rule decides
// there. A small pool admits fewer chunks a frame, so it needs the deeper queue
// to keep its workers fed. Deeper than 128 at such a pool has now been measured:
// at six workers 192 ran 6.912/6.824/5.709 s against 5.885/6.085/5.357 (3/3 for
// the floor, mean 6.482 against 5.776), with idle-empty rising to 16-22% from
// 9-15% and drain to 127-160 ms from 105-133. A deeper queue does not feed the
// pool, it lengthens the drain that already paces it, so the floor stays 128.
[[nodiscard]] inline constexpr std::size_t generation_in_flight_cap(std::size_t workers,
                                                                   int32_t per_worker) noexcept {
    const std::size_t floor = workers <= 12 ? 128 : 64;
    const std::size_t scaled =
        workers * static_cast<std::size_t>(per_worker > 0 ? per_worker : 1);
    return scaled > floor ? scaled : floor;
}

struct FrameBudgets {
    int32_t chunk_generations = 64;
    int32_t chunk_completions_initial = 128;
    // The radius, in chunks, of the full-column underground fill: columns this
    // close to the player generate from their band down to the world floor, so
    // the ground under the player is solid rock rather than void when they dig.
    // It is 24% of an RD 32 fill's installs (4,806 of 20,230 in the profile) and
    // every one of them is non-empty, which makes it the largest block of fill
    // work that cannot be seen before digging. `FARLANDS_UNDERGROUND_FILL_RADIUS`
    // sets it for the arm that measures what the block costs.
    int32_t underground_fill_radius = 8;
    int32_t chunk_completions_gameplay = 64;
    // The install cap while the fill is running. 0 keeps the derived
    // `min(active_max + 16, render_distance + 16)`, which is 48 at RD 32 and is
    // what the install-clock arm left as the next stop; a positive value
    // replaces the whole clamp for the fill only (gameplay keeps its own cap).
    // `FARLANDS_INSTALL_CAP_LOADING` is the measurement arm. Measured at six
    // workers with the loading clock at 3.0: 96 ran 3.702/3.688/3.793 s against
    // 3.724/3.637/3.843 (2/3, mean -0.007 s) -- a tie. The cap is not the
    // binding stop once the clock has room, so it stays 0 (derived).
    int32_t chunk_completions_loading = 0;
    int32_t mesh_rebuilds_immediate = 4;
    // Post-load share of the eager queue. The post-load caps were the fill's
    // limiter: raised with the clocks below, the tail fell 2.57/3.03 -> 1.26/1.51 s
    // and the wall 7.67/8.32 -> 6.31/6.55 s, load unchanged. See docs/streaming.md.
    int32_t mesh_rebuilds_immediate_loading = 512;
    int32_t mesh_rebuilds_idle = 4;
    int32_t mesh_rebuilds_active = 8;
    // The tail's cap, paired with mesh_queue_loading_budget_ms below. In the tail
    // the visible heap is empty and every build comes from the backlog drain,
    // which stopped on this cap in 40% of tail frames and on the 2 ms clock in
    // 48%, so raising either alone just hands the stop to the other. Raising both
    // together (128 and 4.0 ms) was measured and lost: tail 3.56/3.84 s against
    // 3.28/3.30 and wall 7.99/8.36 against 7.68/7.81, with fewer builds a frame
    // (6.6 against 11.9) over more tail frames (695 against 446). The stop
    // counters said why: the drains were handing entries to each other through
    // the stale-revision re-route, so both reported "empty" while thousands
    // were still queued and the ceiling only moved the stop. That re-route is
    // fixed now (each entry finishes in its own heap) and the fill was neutral,
    // so the ceiling is left alone.
    //
    // Raised to 128 and measured on the current tail: it lost. Tail 3.02/3.05 s
    // against 2.53/2.77, wall 7.49/7.84 s against 6.56/6.80, load 4.47/4.79 s
    // against 4.02/4.22. The stop counters moved as intended - the backlog drain
    // hit this cap in 53 of 751 tail frames instead of 278 of 518 - and the tail
    // got longer anyway: it then waited on arrivals with an empty backlog heap in
    // 680 of 751 frames and built 3.1 meshes a frame instead of 9.3. The cap
    // bounds callbacks (three of four never reach a build), so spending more of it
    // only pulls no-ops and uploads forward into the slower load frames.
    //
    // Retested at 128 after the all-air skip, on the reasoning that the queue was
    // now mostly real builds: it lost again and for the same reason. Tail
    // 3.02/3.28 s against 2.26/2.29 and wall 7.81/8.07 against 6.47/6.77, with
    // only 89/377 builds left in the tail and the heap empty in 581/692 frames -
    // the bigger cap drains the queue during the load and the tail then waits on
    // arrivals. The drain is not what paces the tail.
    //
    // 512, after the post-load arm: raising this cap alone had lost because the
    // drain still stopped on the clocks. Raised WITH the loading clocks
    // (mesh_queue_loading_budget_ms, mesh_completion_budget_loading_ms,
    // far_region_visibility_loading_budget_ms) and the upload count, the tail fell
    // 2.57/3.03 -> 1.26/1.51 s and the wall 7.67/8.32 -> 6.31/6.55 s (a shipped
    // run: 5.82 s), with the load unchanged. Tail frames run 9-10 ms against 7.
    int32_t mesh_rebuilds_loading = 512;
    int32_t mesh_uploads_idle = 2;
    int32_t mesh_uploads_active = 32;
    // The held drain uploads completed meshes at most this many per frame while
    // the initial-loading latch is up. 128 left the last batch in the tail; 512
    // finishes it inside the one-second window. `FARLANDS_MESH_UPLOADS_LOADING=N`
    // overrides it for an A/B arm.
    int32_t mesh_uploads_loading = 512;

    size_t completed_queue_backlog = 512;
    size_t dirty_mesh_backlog = 512;
    size_t worker_queue_backlog = 512;

    int32_t max_loaded_chunks = 50000;

    // The chunk drain's clock. Doubling it to 2.0 was measured twice and changed
    // nothing: alone, chunk load 8.39 s against 8.36 s; paired with the 1.3 ms
    // admission price, settle 4.252/4.029 s against 4.240/4.678 s at the same
    // price (1/2 pairs, and both arms worse than the 1.92 ms price's 4.0-4.1 s).
    // The loop empties what the workers finished each frame - raising the drain
    // cannot install what admission did not enqueue. Re-measured at six workers
    // with the install stage profile (an install is 36 us, the fill's
    // finished-to-installed latency 61 ms, so this clock is the stop):
    // `FARLANDS_PROCESSING_BUDGET_MS` at 3.0 ran settle 3.722/3.794/3.912 s
    // against 4.127/3.815/4.119 (3/3, mean -0.21 s), with installs a frame 28 ->
    // 32 and the phase still ~1 ms. It stays 0.75 because this clock is also the
    // gameplay drain, where the fill's trade does not apply; the arm is the env.
    double processing_budget_ms = 0.75;
    // The same clock while the fill has not settled (and through the post-settle
    // latch), which is the only place the fill's trade applies: a bigger clock admits more installs per
    // frame, so the finished-to-installed queue and the frame count both fall.
    // Measured at six workers as the loading value: 3.722/3.794/3.912 s against
    // 4.127/3.815/4.119 (3/3, mean -0.21 s), installs a frame 28 -> 32, the
    // phase still ~1 ms. The fifteen-worker check is larger, not neutral:
    // 3.474/3.388/3.441 s against 3.915/3.784/3.809 (3/3, mean 3.434 against
    // 3.836), frames 550/595/565 against 645/655/643. The gain grows with the
    // pool rather than peaking: 8 workers 3.244 against 3.462 (-0.22), 11
    // workers 3.224 against 3.734 (-0.51), every pair at every count for 3.0.
    // A value sweep at 11 workers then moved the knee: 1.5 gave 3.443/3.302 s,
    // 3.0 3.234/3.301 and 6.0 3.145/3.138, both 6.0 blocks fastest. The
    // cross-check ran before the default moved: at six workers 6.0 ran
    // 3.710/3.659 against 3.706/3.725 (1/2, a tie) and at fifteen 3.441/3.338
    // against 3.480/3.390 (2/2, -0.05 s), so 6.0 was faster in five of the six
    // pairs and never slower, and the default is 6.0. Gameplay keeps 0.75.
    // `FARLANDS_PROCESSING_BUDGET_LOADING_MS` overrides it for an A/B.
    double processing_budget_loading_ms = 6.0;
    double mesh_completion_budget_ms = 0.5;
    // Post-load clock for the same pass: a ceiling, not a target - the tail's
    // frames averaged 10-12 ms in the arm that shipped this.
    double mesh_completion_budget_loading_ms = 1000.0;
    // The dirty chunk queue and the far-region scheduler share one wall-clock
    // budget. They used to get 0.75 ms each, so a crossing could spend 1.5 ms in
    // DirtyMeshQueue alone; the immediate queue is inside this budget too.
    double mesh_queue_budget_ms = 0.35;
    // Initial streaming is allowed to spend more on the mesh queue than live
    // play. The 0.35 ms budget is a border-smoothness budget; with it the 64
    // loading rebuilds below can only schedule a handful, which is why the
    // far/mesh tail outlived the chunk load by five seconds.
    // 2.0 kept. Alone, 4.0 lost (tail 4.05/3.78 s against 3.55) because the
    // drain kept stopping on the 64-rebuild cap; paired with a 128 cap it lost
    // too (3.56/3.84 against 3.28/3.30) because the heaps then empty into each
    // other and stop early - see mesh_rebuilds_loading. Stamped entries to drop
    // unchanged ones fired 52 times in 24,600 pops, so the cost was not stale
    // work either; 91% of the phase was the buried test's worldgen probes, now
    // memoised.
    // 1000 after the post-load arm: with the rebuild and upload counts raised,
    // this clock was the stop; the tail's frames averaged 10-12 ms in the arm.
    double mesh_queue_loading_budget_ms = 1000.0;
    // The generation ring walk used to run to its full count/check budget; on
    // a crossing that was 3.15 ms in one frame. Enqueueing is main-thread work,
    // so cap it in wall time like the other phases.
    //
    // 1.0 and 3.0 were both measured on the RD 32 fill and the walk did not
    // move: 12.4 checks and 9.1 generations a frame at 1.0 against 12.5 and 8.9
    // at 3.0, over ~1,300 frames. Wall came out 7.47/7.50 s at 3.0, inside the
    // 7.7-8.2 s the same probe was already reporting, so the arm is left here:
    // the walk stops on the sweep's band frontier (and the mesh tail drains
    // behind it), not on this clock, and a wider ceiling would only be spent on
    // a crossing frame. A mesh-side arm that held first builds for missing
    // neighbours was measured on the same probe and lost outright (tail 3.5 ->
    // 4.0 s, queue pops 24.7k -> 45.3k), so the tail is a consequence of the
    // fill's pace, not something that can be meshed away first.
    double generation_budget_ms = 1.0;
    // The generation chain's wall clock. Its count budget advances only on a
    // real generation, so a queue full of skips - the walk refuses a candidate
    // the chain wants, or the chain re-offers a neighbour that is already built
    // - drains unbounded within a frame. The fill profile showed the shape: the
    // worst generation frame ran 24 ms against a 0.64 ms average, all of it in
    // the chain, while the band service and the walk both already stop on a
    // clock. The queue is refilled by installs, so an offer cut off here is
    // re-offered rather than lost. Zero disables the bound for an A/B arm;
    // `FARLANDS_CHAIN_BUDGET_MS` overrides it.
    double generation_chain_budget_ms = 1.5;
    // The incremental far-region visibility walk. The full walk cost ~2.4 ms on
    // every crossing frame; a slice this size keeps the same work in the frame
    // but out of one frame's critical path.
    double far_region_visibility_budget_ms = 0.1;
    // Post-load clock for the incremental far-region walk: with the mesh caps
    // raised this was the next stop, so the tail gets an effectively open clock.
    double far_region_visibility_loading_budget_ms = 1000.0;
    int32_t max_mesh_completions_per_frame = 16;
    // The shipping initial-fill path holds mesh completion and upload until the
    // fill settles (the sweep list is spent and generation/completion are
    // empty), then drains the held work at 512/512. It is one-shot and bounded:
    // later chunk-load busy periods must not re-arm it, and a world that cannot
    // settle releases at this deadline instead of never meshing (which is what
    // a small render distance did under the old 500-chunk gate). The old overlap
    // path is available with
    // `FARLANDS_HOLD_MESH_UNTIL_LOADED=0` for an A/B arm. The price is measured:
    // first terrain at the 2 s deadline is 1.9 s on the 16-thread machine and
    // 2.1 s on 6 cores (against 0.3-0.6 s for overlap), while the end moves
    // ~0.5 s sooner on 16 and ~0.1 s sooner on 6. The matched 4-core arm (3
    // workers, hold off) reaches chunk load 18.38 s and end 19.14 s with the
    // corrected Unrendered gate; the old 19.84 s/90 s no-end figure counted
    // chunks already drawn by their own instances. See docs/streaming.md.
    // `FARLANDS_HOLD_MESH_MAX_SEC` overrides the deadline for an A/B arm.
    // The hold is only a win when the pool can drain it: at 5 workers it bought
    // ~0.1 s of end-to-end fill for ~1.6 s of extra blank; at 15 workers it
    // bought ~0.45 s. Five-pair medians at the in-between points are why the
    // floor is 12: 8 workers 6.46 s hold vs 6.48 s off (A-B -0.07 s, a wash),
    // 10 workers 6.25 vs 6.28 (A-B -0.02 s, still a wash), and 12 workers 5.51
    // vs 5.77 (A-B -0.26 s, hold faster in 4/5 pairs). Worker count is a
    // logical-thread proxy, not physical cores: a 4c/8t machine defaults to 7
    // workers and holds off, 6c/12t to 11 and holds off, 8c/16t to 15 and holds on.
    // `FARLANDS_HOLD_MESH_MIN_WORKERS` overrides that floor, and an explicit
    // `FARLANDS_HOLD_MESH_UNTIL_LOADED=1` forces it on for a slow-machine A/B.
    double hold_mesh_max_sec = 2.0;
    bool hold_mesh_until_load_done = true;
    bool hold_mesh_force = false;
    int32_t hold_mesh_min_workers = 12;
    // While the one-shot initial mesh hold is armed, allow this many nearest
    // chunk rebuilds and uploads through per frame. Zero restores the old
    // all-or-nothing hold (nothing meshes until the fill settles). Two is the
    // shipping partial hold: five AB/BA pairs at 15 workers moved first visible
    // 1.83 [1.81-1.88] -> 0.29 [0.27-0.31] s while loaded/end stayed 4.99/4.99
    // against 4.99/5.00 s (paired end -0.02 s, B faster 2/5; load frame 7.66 vs
    // 7.92 ms, process 1.29 vs 1.36 s). The full overlap cost 0.69 s of end;
    // this spends a few builds a frame on the nearest chunks and none of it on
    // the far scheduler. `FARLANDS_HOLD_MESH_PARTIAL=N` overrides it for an A/B.
    int32_t hold_mesh_partial = 2;

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
    // The same set while the fill has not settled. The gameplay term is sized to
    // keep the completed queue short so every drain iteration stays cheap; during
    // the fill the drain has a 6 ms clock and a 128-install cap instead, so the
    // set is what keeps the pool fed rather than what slows the drain. Measured
    // at 11 workers with the install cap open: 264 in flight ran load 2.66 s
    // against 2.89-2.93; at 15 workers the same shape ran 2.13 s. 24 per worker
    // gives 264 at 11 and 360 at 15, which is where both arms were measured.
    // Shipped as a pair with the loading install cap (the render-distance clamp
    // is skipped while the fill runs): three pairs at 11 workers put settle
    // 2.84/2.81/2.72 s against 3.22/3.17/3.09 and load 2.66/2.61/2.68 against
    // 3.18/3.15/3.13, 3/3, with idle-while-queued down from 24-26% to 19-21%.
    int32_t max_generating_in_flight_loading_per_worker = 24;
    // The admission price while the fill has not settled. The line above is the
    // gameplay cost model, sized from sweeps in which the caps were closed and
    // the walk's extra offers were refused before they could be exercised; with
    // the loading caps open the rate matters again, and a cheaper chunk is right
    // for exactly as long as the fill runs. Measured under the shipped caps, 3/3
    // pairs at each count: 11 workers settle 2.550/2.538/2.610 s against
    // 2.875/2.739/2.811 and load 2.37/2.42/2.38 against 2.88/2.71/2.73; 15
    // workers settle 2.220/2.116/2.151 against 2.577/2.563/2.567 and load
    // 2.14/1.93/2.18 against 2.46/2.45/2.62. The five-worker knee (1.1) was
    // measured on the old, capped path, so the small-pool arm is checked against
    // this value rather than assumed.
    double generation_work_per_chunk_ms_loading = 0.9;

    // The feed-forward admission controller's cost model, in milliseconds per
    // generation. Zero means "derive it from the pool size" through
    // generation_work_per_chunk_ms_for(): at 15 workers the old 2.46 still
    // holds, and below it the price ramps down to the measured five-worker
    // 1.8. `FARLANDS_GEN_WORK_PER_CHUNK_MS` overrides it for measurement: it is
    // the switch that separates "the controller's rate is the wall" from "the
    // pool is".
    double generation_work_per_chunk_ms = 0.0;
    // The in-flight cap normally follows the worker count (max(64, workers x
    // per-worker)). At 3 workers the 64 floor binds, so a lower cost model
    // cannot turn into a higher rate until the cap moves too. An override here
    // is what lets an arm raise both together and measure the admission
    // ceiling instead of the cap.
    int32_t generation_max_in_flight = 0;

    double flush_interval = 5.0;
};

} // namespace VoxelEngine

#endif // FARLANDS_FRAME_BUDGETS_HPP
