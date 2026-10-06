# Frustum-prioritized loading

What gets generated, meshed and kept, in what order, and what it costs. The frustum
decides the ORDER of that work and never whether the work happens: a chunk behind the
player is still generated, just after everything in front of it.

This page is about streaming the world the engine generates. The far-field LOD
*modes* — the voxel tiers that reduce detail in those chunks, and the seed-grid
mode planned beside them, which draws terrain the seed implies without generating
it — are in [lod-modes.md](lod-modes.md).

- `Camera3D::get_frustum()` provides 6 world-space planes
- `DirtyChunkEntry` priority: `urgent > in_frustum > dist_sq`, and the `in_frustum` tier is **no
  longer unbounded** — see the backlog reserve below
- Two-phase generation: the **generation chain** first, then the distance-sorted sweep (the
  frustum-priority pass was replaced — see the chain bullet below)

## What outranks what

- **Caller-required chunks outrank all of it**: `ChunkWorld::request_urgent_chunk()` is a queue
  the updater drains BEFORE either pass (a paste is waiting on those chunks, and the sweep would
  mostly refuse them — the sky above a build sits outside its per-column band), and
  `pin_chunk()` keeps the unload pass off one until the caller is done. Both are the caller's to
  release; `unpin_all_chunks()` on `clear()`
- **Generation is frustum-free, meshing is view-first, and that split is deliberate after
  testing both.** Generation has been frustum-free since the chain replaced the pass (all three
  `generate_chunk` call sites are urgent/chain/walk). The MESH queue sorts rebuilds `in_frustum`
  first — including a fresh chunk's FIRST mesh — so terrain appears where the player is looking
  before it appears at their back. Removing that term (so a chunk becomes visible in its
  generation order) was tried and REVERTED: it read as slower mesh throughput in play, because
  the view-first term is not cosmetic — it is what concentrates the mesh budget on the part of
  the world on screen. The one other frustum use in the streaming path is the unload deferral (a
  visible chunk beyond render distance unloads last), which is eviction ordering and cannot
  reorder appearance
- **The frustum planes are flipped on the way in, and that is load-bearing.**
  `Camera3D::get_frustum()` returns normals pointing OUTWARD (a point inside the frustum has a
  NEGATIVE distance to all six), while `Frustum` tests `distance_to(center) < -r`, i.e. assumes
  inward. Measured, not read from docs: a camera at (500,100,500) facing -Z returns its near
  plane as normal `(0,0,1)` with `d=499.95`, and the engine's own `is_position_in_frustum` says
  a box three chunks in FRONT of it is visible (`probes/probe_frustum.gd`, and
  `tests/test_frustum.cpp` pins the same six plane values). Feeding those planes in unaltered
  made **every** chunk in the world test invisible. That was not theoretical: the generation
  sweep's frustum pass examined 2,376,704 candidates across one session and passed zero, so a
  third of the generation budget bought nothing and every mesh was queued with
  `in_frustum = false` — the frustum tier of `mesh_queue`'s priority never fired either.
  `Frustum::update` now negates them, so this class keeps ONE convention (inside = positive) and
  all four callers are fixed at once
- Dynamic mesh budget: the visible-chunk ratio scales it from 0.5× (sparse) to 1.0× (full). **A
  backlog-proportional scale-up was tried and REVERTED.** The reasoning looked sound — the queue
  is saturated while the player moves, so a bigger count drains it sooner — but the work is
  main-thread, and it is not cheap per rebuild: each rebuild does 7 chunk lookups, and the
  far-region share of the budget walks 64 chunks per scheduled region. Scaling the count 4× took
  `dirty_mesh_queue` from a **~1.0 ms median to ~2.7 ms** in a live session (that phase is
  measured per report interval, `avg`/`max`) with no visible gain, because the extra rebuilds
  are not more terrain — the same chunks arrive in the same order, only sooner
- **View-first meshing is a share of the budget, not a tier that can starve the rest.** The
  queue is split into two heaps — visible chunks, and everything else — instead of one heap
  ordered `in_frustum` first. Each frame the visible heap gets `max_rebuilds - reserve` and the
  visible heap's *unspent* share is handed to the other, so: at least `max_rebuilds/4` rebuilds
  per frame are always spent in plain distance order; a frame with nothing visible to build
  still spends the full budget; and a chunk that comes into view moves back into the visible
  heap on its next pop (entries are re-evaluated against the current frustum whenever their
  priority revision is stale) without waiting for the reserve. This is the "it forgets to mesh
  stuff until you look at it" fix: with one heap, a chunk that is merely off-screen — behind the
  player, or at the frustum's edge — never reached the front while any visible work existed,
  which during streaming is always, and it stayed un-meshed until the player turned round and it
  jumped the queue. With no frustum fed (benchmarks and tools) everything goes into the
  distance-ordered heap and no reserve applies, so the headless path is unaffected.
  `tests/test_mesh_queue.cpp` pins the share, the unused-budget hand-over, the no-frustum path
  and the move-back-into-view case
- **The LOD tier rescan walks the transition shell, not the box around it.**
  `MeshManager::reprioritize` re-checks the chunks in each tier's transition band, which is the
  Chebyshev shell `max(|dx|,|dy|,|dz|) ∈ [tier-1, tier+1]`. It used to test that condition cell
  by cell over the whole enclosing box: with the real tier radii that is **~1.7M iterations per
  tier per frame to reach ~100k band cells**, and it is what the `dirty_mesh_queue` phase was
  spending its milliseconds on (avg 1-4.7 ms/frame, max 157 ms) — pure loop overhead, since only
  band cells are looked up. `for_each_shell_cell` (`src/mesh/lod_shell.hpp`) now visits the band
  directly: for a column with `m = max(|dx|,|dz|) >= tier-1` every
  `|dy| <= min(vert_range, tier+1)` qualifies, and for a column inside the inner cube `|dy|`
  must itself be in the band. `tests/test_lod_shell.cpp` pins the visited set against the box
  filter it replaced, including the `vert_range` smaller than the shell case, so the fast walk
  cannot silently disagree with the filter

## The frustum generation pass was replaced

- **The frustum generation pass was REPLACED by the generation chain — the priority is "spread
  from built terrain", not "what the camera sees".** `ChunkWorld` calls one install listener per
  chunk it installs (`set_install_listener`), and `WorldUpdater::on_chunk_installed` queues the
  four horizontal neighbours of every chunk whose data holds a non-air block.
  `update_generation` drains that queue BEFORE the ring walk — through the same filters (world
  height, sweep disc, in-flight, loaded, band), the same generation budget (half the frame's
  allowance, the share the frustum pass had), and reorders nothing else. Three numbers decided
  it, all from the streaming benchmark at 60 fps: throughput **2,231 → 2,334 chunks/s**, cold
  start **~250-320 → 215-283 ms**, sweep wall time **1,916-2,125 → 1,346-1,402 ms**, holes still
  0 — and the chain does real work, ~10% of a flight's generations. Seeding is itself filtered:
  a neighbour whose COLUMN is already built (`built_columns`, a main-thread set — free next to
  the locked `contains` the drain would pay to discover the same thing) or that is already in
  flight (three relaxed atomic loads) is never queued, which cut one flight's offers **166,423 →
  88,923** and its skipped-at-drain count **145,297 → 79,406** with throughput unchanged. The
  drain takes **3/4 of the frame's generation budget** (the frustum pass had 1/2): its offers
  are neighbours of just-built terrain — the walk's own near rings, which the walk re-walks
  after every crossing — so a chain offer spends the budget better than the walk's next check,
  but the walk keeps a quarter for the disc beyond what has been built. The vertical arm
  (offering the chunk ABOVE when the column's cached bounds say content could reach it) is built
  and gated OFF: **18.4k offers per flight, zero generations** on this terrain — pure queue
  traffic. It is one bool (`kVerticalChain` in `on_chunk_installed`) to turn back on when
  terrain that crosses borders upward exists; the counters that judged it
  (`chain_vertical_offered/generated`) stay in `/genstats` Two implementation notes that were
  each a real bug: the listener reads `is_all_air()` BEFORE the chunk data is moved into the map
  (after the move it is null — segfault on the first boot, caught by the benchmark), and a
  REFUSED offer must be dropped, not re-queued: the queue is refilled by installs far faster
  than the budget drains it, so re-queueing turned 174k offers into 13.3M (a refused candidate
  returns on its own when a neighbour installs, and the walk reaches it regardless). Dedupe is a
  set beside the deque; a rebuild or world clear empties both, because queued offers were keyed
  to the old list's disc
- **The frustum pass looked like waste and was not (gating it was implemented, measured, and
  reverted) — but it still lost its job, to a priority that pays for itself.** Gating its re-arm
  on a view change removed its idle cost exactly while costing 8-10% of streaming throughput, so
  the gate was reverted with the numbers recorded here. What replaced it (the chain, above) won
  on every number rather than trading one for another: it reorders the SAME work by "what is
  next to what already exists", which is where streaming actually happens, and its per-offer
  cost is a handful of set lookups instead of 240 frustum tests a frame With the band list and
  the built-column skip in place, a real session showed the pass at **3,545,274 checks against
  the distance walk's 196,553** — 95% of every check in the sweep — with **2,406,288 of its
  2,426,296** in-frustum candidates already resident, plus 29,585 refusals of chunks already in
  flight (`ChunkScheduler::enqueue_generation` returns false for a chunk that is generating).
  That reads as a pass re-walking from ring zero for nothing: `ChunkManager` feeds the camera in
  every frame and `set_frustum` reset its cursor every time. So the re-arm was gated on a real
  view change (a position/angle threshold, with the near-left-top corner solved from three
  planes as the position proxy, and NORMALIZED normal comparison — an unnormalized `dot` reads
  an identical plane as 4 degrees apart and re-arms forever, which the first test run caught).
  A/B'd on ONE build with `probes/probe_stream_bench.gd`: **the idle cost went exactly to
  zero** (standing still 10 s: 306,845 checks and 88.5 ms → 0 checks and 0.6 ms) and
  **throughput fell** (1,903 → 1,750-1,798 chunks/s). The reason is the useful part: the pass is
  a SECOND, view-ordered consumer of the generation budget, and the walk cannot absorb its work
  — the walk refuses ~10% of its own checks on chunks already in flight (`walk_refused` was
  38,239 of 387,257 in one flight), so it has no headroom to pick up another 240 checks a frame.
  Its checks are cheap (88-101 ms per 10 s idle is **0.15 ms per frame**) and its ordering is
  what puts what the player is looking at first. The lesson is in the counters themselves: **a
  large share of checks is not the same as a large cost**, and `frustum_refused` now exists
  precisely to show how much of that pass is re-asking for work already under way rather than
  doing new work — the shape that would justify making it cheaper per check (rather than running
  it less).

## The per-column sweep

- **Asking "is this chunk already being generated?" takes no lock at all.**
  `ChunkScheduler::may_be_generating` is a 3-probe bitmap (8 KB, relaxed atomics) maintained by
  the enqueue and completion paths, and both sweep passes consult it BEFORE the chunk-map lookup
  on every candidate. Without it the only way to ask was `enqueue_generation` — a GLOBAL mutex
  plus a shard lock on the chunk map through its `is_already_loaded` callback, i.e. three lock
  operations to be refused — and the walk paid that 46,576 times in one session (15% of its
  checks), the frustum pass 18,269 times. A refusal there also consumed one of the walk's 512
  checks per frame, so the budget went to terrain that was already on its way: skipped
  candidates now cost no check at all (bounded by their own per-frame allowance, so a frame
  cannot walk the whole list on skips — past it a candidate falls through to the old path and is
  refused, which is bounded waste instead of unbounded cheap work). Measured over a 40 s flight:
  walk refusals 38,239 → 9,052, frustum refusals 79 → 0, walk checks 387,257 → 332,542,
  throughput unchanged (1,964 → 1,966 chunks/s best-of-two each side), holes zero. The wall
  clock does not move, and that is expected — its value is 29,000 fewer lock operations per 40 s
  of flight on a mutex the generation workers also take, which is a frame-VARIANCE win, not a
  throughput one. Approximate by construction and safe in the direction that matters: a false
  positive defers a chunk to the walk's next cycle (bits are cleared when that generation
  finishes, and unconditionally — before the epoch check — so a dropped generation cannot claim
  to be in flight forever), a false negative just takes the old path, and nothing is skipped
  permanently because the walk re-offers every candidate every cycle.
  `tests/test_chunk_scheduler_filter.cpp` asserts the wiring in both directions against a real
  worker, not just that a bitmap can be set
- **Standing still costs the sweep nothing, and that is a property to keep.** Measured at 60 fps
  with nothing moving: **0 checks, 0 generated, 0.6 ms** over 10 s of the distance walk (it
  reaches the end of the list, finds nothing, and retires until something invalidates it). The
  frustum pass is the exception and it is bounded, as above.

- **The band's ±32-block pad is what generations are spent on, and the measurement says so.**
  A chunk wholly above a column's `top_h` or below its `land_h` can only be offered because of
  the pad (or the fill rule), and `WorldUpdater::on_chunk_installed` now classifies every install
  against the column's cached bounds (peeked, never derived — it runs per install on the main
  thread). Measured over two 24-chunk flights (headless, RD 32, `probes/probe_gen_stats.gd`,
  11,339 and 12,665 installs): **every single chunk above the column's top came back empty
  (1,263/1,265 and 1,534/1,534)**, in-range installs were empty ~35% of the time (2,231/6,437
  and 2,649/7,457), and **0 of 3,637 / 3,674 below-land installs were empty** — those are the
  fill rule's real rock, not pad waste. The band histogram
  says the same from the other side: non-fill columns sit in the 4-7 bucket (64,082 of 69,082,
  mean band 4.56 slices over all columns, tallest 10) — the pad is not making bands huge, it is
  making the few slices it adds reliably empty. So the lever, if this is worth a change, is not
  the band size; it is the *top* edge, where a narrower margin above `top_h` would delete ~1,263
  empty generations per 24-chunk flight without touching the terrain the estimate actually
  claims. The bottom edge is protected by the fill rule and by `land_h` being the lowest
  possible surface, and the counter confirms it: no empty generations there.
- **The sweep list is per-column BANDS, not per-column world height.**
  `src/world/sweep_band.hpp` holds the band filter in one place (`chunk_in_band`) and the exact
  slice range it accepts (`band_for_column`, which TESTS each of the 32 slices instead of
  inverting the arithmetic — inverting it is where an off-by-one silently drops a chunk that
  genuinely contains terrain, which is the invisible-solid-hole class of bug
  `tools/sched_window_check.cpp` exists for; `tests/test_sweep_band.cpp` holds the range to the
  predicate exhaustively, including either side of every slice boundary). `WorldUpdater` builds
  one `SweepColumn` per column in the render distance `{dx, dz, band}` and `advance_sweep`
  offers candidates in ring order, and within a column outward from the player's own slice — so
  the near-player full-column fills offer the player's own rows first instead of hundreds of
  blocks of rock. At render distance 32 that is **~20,230 candidate chunks over ~3,209 columns,
  exactly**: `reject_above`, `reject_below` and `reject_oob` are now **0**, which is what "the
  list is exact" looks like from outside. On the same 24-chunk flight this moved
  candidates-generated-per-check from **1.2% → 4.1%** and generations per frame from **6.3 →
  23**, with the boot story sharper still (11% → 97% of checks generating). What remained was
  `reject_loaded` at ~96% — the near rings re-examined after every crossing — which the next
  bullet removes
- **A column whose band is already resident is skipped whole, so the walk stops re-confirming
  built terrain.** `sweep::band_fully_resident` decides it per slice (tested,
  `tests/test_sweep_band.cpp`: an empty band is deliberately NOT "built", and the predicate is
  cross-checked against set inclusion over a grid of band shapes); `service_sweep_bands` records
  the column in `built_columns` where the band is computed, and `advance_sweep` skips it with
  ONE chunk-map lookup instead of `count(band)` of them. Invalidation is the part that matters
  and it is deliberately narrow: a successful unload of any chunk in the column erases it
  (`try_unload` — skipping a column with a missing band chunk is exactly the invisible-hole
  bug), and a rebuild clears the whole set because every band in a new list is unknown. Measured
  on the same 40 s / 4000-block flight at 100 blocks/s (headless, 60 fps,
  `probes/probe_stream_bench.gd`): walk checks **2,368,883 → 316,302 (-87%)**, checks that
  generate **3% → 26%**, and generation throughput **1,505 → 2,035 chunks/s (+35%)**, with the
  same zero holes in a 5x5 sample every 10 frames and the same cold-start time (~200 ms to
  terrain after a 2048-block teleport)
- **Benchmark the thing the player feels, not the counters.** Every counter above is per-CHECK
  or per-COLUMN, so none of them answers "how long after I move does the ground under me exist"
  — the question every change here is actually aimed at. `probes/probe_stream_bench.gd`
  measures it at a fixed 60 fps: (A) jump 2048 blocks into genuinely ungenerated terrain and
  time until the 3x3 columns around the landing spot have ground, and (B) move at 100 blocks/s
  for 40 s while sampling a 5x5 column square every 10 frames for columns still without ground.
  Baseline: cold start **~200 ms**, flight **0 holes in 505 samples**, 1,505 chunks/s generated.
  Two ways this measurement lies if written naively, both hit and fixed while building it: a hop
  shorter than the render distance lands inside the already-generated disc and measures nothing,
  and a column cached as "empty" must be re-scanned or a cold region reads as empty forever (the
  first version reported all three cold starts as never settling while generation was filling
  them)

## The band frontier and the pump

- **With the walk cheap, the band frontier was what the sweep's time was.** Same 40 s flight,
  splitting `total_ms` by phase: **walk 839 ms, band reads 2,820 ms, rebuilds 37 ms** out of
  3,660 ms — and `band_reads` is exactly **397,907 ≈ 124 crossings x 3,209 columns**, because a
  crossing rebuilds the list and the frontier re-reads every band in it even though a column's
  band depends only on that column's own surface bounds and not on where the player stands. The
  per-column tail is what that 2,820 ms actually was, and it is what the next bullet moves off
  this thread
- **A column's bounds are produced by a worker ahead of the frontier, and the main thread only
  computes one when no answer has landed.** `src/world/column_prefetch.hpp` is the handoff: the
  frontier REQUESTS the columns of the list it has no bounds for, and
  `get_column_surface_bounds` CLAIMS a ready answer keyed by absolute chunk coordinates before
  falling back to doing the range itself — so a lagging prefetch is slower, never wrong, and can
  never stall the frontier it feeds (the fallback is the old path, unchanged). Requests are
  scanned independently of the cursor and nearest-first, because the columns the frontier lacks
  are NOT the ones just ahead of it: after a crossing they are the ring that just entered the
  disc, which sits at the END of a nearest-first list. The first pass over a freshly built list
  therefore covers the whole list in one frame (a slice-per-frame scan left **1,910 of 4,848**
  entered columns — 39% — derived on the main thread, measured), and later passes only replace
  what has been claimed. Correctness is an epoch: a column's bounds depend on the terrain
  configuration alone, so `refresh_prefetch_config` republishes it and `ColumnPrefetch::publish`
  REFUSES an answer computed under a retired one instead of letting it into the cache — and
  every request is retired in every path, including the refused and the over-cap ones, since a
  key left looking in flight is a column the prefetch quietly stops covering. One generator per
  worker thread configured from that published copy (the same shape the generation workers use),
  and the state is shared-ownership so a task still queued when the updater dies writes into
  memory it owns
- **Measured with the same 40 s / 4000-block flight, A/B on ONE build by disabling the pump
  (`band time` 745-901 ms vs 3,577 ms, -75% to -79%):** band reads are unchanged
  (**392,164-397,715**, the frontier still reads every column of every rebuilt list) while the
  columns derived on this thread fall **7,741 → 346-475 (-94%)** and `prefetch_taken` accounts
  for 4,373-4,953 of them; generation throughput **1,974.6 → 2,233-2,298 chunks/s (+13% to
  +16%)**; total sweep time **4,995 → 1,916-2,125 ms (-58%)**; holes **0** in both arms, cold
  start unchanged within noise (~250-320 ms to terrain after a 2048-block teleport). 521 tests
  pass, including `tests/test_column_prefetch.cpp`
- **The in-flight generation set is capped, because pressure alone never bounded it.**
  `FrameBudgets::max_generating_in_flight_per_worker` (8, floored at 64) is a hard admission
  gate in `update_generation`: above it the sweep, the chain and the walk stop enqueueing for
  that frame, while urgent (paste) requests keep their own separate allowance and are
  deliberately exempt — a caller is blocked on those. Before this, the only bound was the
  completed queue filling at 512, so a flight reached **6,524 chunks in flight at once** with a
  **6,747-task worker queue**. Everything downstream followed from that backlog: completions
  landed in bursts (the install phase's 143 ms worst frame, with a 2,759-chunk completed queue),
  the column prefetch's ~181 us answers sat behind thousands of generation tasks (so **8,863
  columns were derived on the main thread against 8,318 taken from a worker**, with 1,303
  answers dropped), and the walk spent its whole check budget on candidates it could only refuse
  (`585,788` refused in one session). Refilling is not a risk: the sweep may enqueue
  `dynamic_max_generations` per frame against a cap of `workers x 8`, and a generation is ~1 ms
  of worker time, so a few dozen in flight already saturate 15 workers
- **Shard-lock telemetry, and what it found: the waits are a convoy, and the install path was
  feeding it.** `ChunkMap` records per-shard contention (`drain_shard_lock_stats`, printed by
  the perf report): contended-only wait timing for both shared and exclusive acquisitions (an
  uncontended acquisition is a single `try_lock` and pays no clock), plus hold time for the
  multi-shard EXCLUSIVE locks, which are the rare writer locks worth naming. The reading that
  matters is the RATIO: over a fast flight, shared waits reached 7,950 with a worst of 382 ms
  while the worst single exclusive HOLD was ~8 ms — waits an order of magnitude larger than any
  hold are queueing behind a stream of writers, not waiting on one, i.e. a convoy. Two sources
  were feeding it and both are fixed: the per-chunk lighting stage took a **28-shard EXCLUSIVE
  lock to answer a read-only question** (does this chunk's 3x3x3 contain an emitter) on the main
  thread, once per staged chunk — now a shared lock, with the propagation keeping its own
  exclusive lock in the worker; and two neighbour-dirty passes allocated a `std::vector` for a
  7-key lock on every installed chunk — now the array overload. The remaining writer pressure is
  one exclusive lock per chunk insert, which is why the completed queue (`Completed queue: 1811`
  against a drain of `render distance + 16` per frame) is the throughput cap during flight:
  generation is throttled by `can_enqueue` refusing on a full completed queue (`585,788` refused
  candidates in one session), not by the workers, which sit at roughly a third utilization
- **The worst band frame did NOT fall, and the answer is not the budget — it is one
  `ChunkMap::contains` lock acquisition.** Against a 2 ms per-frame budget `max_band_ms` stays
  12-26 ms in both arms of that A/B, so it is not the prefetch. The split counters settle it: a
  worst column of 26.09 ms was `bounds=26.09, resident=26.09, contains=23.61` — the resident
  lookups ARE the whole stall, one shard lock waiting on the generation workers that write the
  same map (the cold bounds read in the same session was 0.65-1.31 ms, so the range computation
  is definitively not it). The budget bounds work fine: `max_band_columns` shows the overshoot
  is one or a few columns, never thousands. The fix is not in the sweep — it is shortening the
  writer's critical sections or moving the read off the locked map

## Shard locks and the convoy

- **The fix for that stall was `key_to_shard`: a shard is chosen per COLUMN, not per chunk.**
  The resident check asks "is this whole column built?" and the answer is `count(band)` chunk
  lookups, and a band reaches the world floor near the player. With a shard per chunk, each of
  those lookups was its OWN `shared_mutex` acquisition on its own shard, so a single column
  spanned up to 16 shards and could queue behind a writing worker on any one of them — which is
  why the counter above showed one lookup of 39.4 ms inside a 39.6 ms column. Masking the y
  field out of the hash (bits 21..41; the same packing `encode/decode_chunk_key` uses) puts
  every chunk of a column on one shard, and the check becomes ONE `ChunkMap::lock_column`
  acquisition followed by lock-free `contains_fast` probes (`tests/test_chunk_map.cpp` pins the
  invariant the batching depends on, plus that column shards still cover all 64 shards evenly).
  It also shrinks every multi-key lock, because a neighborhood's keys collapse onto shared
  shards: a 3x3x3 is 9 distinct (x, z) columns, so the light region's 27-key EXCLUSIVE pass
  takes 9 shards rather than up to 27 and the install path's 28-key probe likewise. Measured A/B
  on ONE build via a temporary env toggle, same 40 s flight, back to back: throughput **1,891.2
  → 1,971.6 chunks/s**, sweep total **1,118.3 → 889.6 ms**, walk **566.8 → 424.4 ms**, band
  reads **551.5 → 465.2 ms** (**397,916** band reads in both arms, so this is per-column cost:
  2.4 → 1.26-1.33 us), worst sweep frame **43.9 → 29.7 ms**, worst single column **5.11 → 3.90
  ms** (and in the live game, 39.6 → 8.5-10.1 ms), cold start **222.1 → 137.5 ms**, holes **0**
  in both arms. 524 tests pass
- **Reading a column's band is spread over frames, nearest column first.** One band costs a
  rigorous chunk height range over that column's 4-block lattice, measured at **~235 us cold**,
  so all 3,209 of them is ~755 ms of work. Paid eagerly that was a **single 743 ms frame** at
  world load and **19 ms on every chunk crossing** while flying — a dropped frame every time the
  player crossed into a new chunk, from the only per-COLUMN cost in the sweep.
  `service_sweep_bands` now reads at most 2 ms of them per frame and `advance_sweep` STOPS at
  the first unread column rather than reading it, so the walk cannot outrun the frontier and the
  frontier cannot stall (one column minimum per frame); because both go nearest-first, the work
  always lands on the terrain the player is standing in. Rebuilding the list is bookkeeping
  only: **0.5 ms** for the first one, **0.25 ms** per crossing, band reads capped at **2.5
  ms/frame**, worst `update_generation` frame at boot 743 ms → 47 ms. `/genstats` reports
  `list rebuilds` and `bands read` as separate timings because their shapes have nothing in
  common

## What the instrumentation paid for

- **What the sweep's check budget is spent on is measured, not assumed** — these are the
  readings that forced the change above. `WorldUpdater::GenerationStats` counts every candidate
  a pass examines and why each one was rejected (`reject_loaded`, `reject_above`,
  `reject_below`, `reject_oob`), what reached `generate_chunk` and what it refused, the frustum
  pass separately from the distance-sorted one, the urgent (paste) requests, and a rolling
  window over the last 120 frames; `/genstats` prints them and `/genstats reset` zeroes them
  while keeping the candidate-list size. The list WAS the render-distance disc × the WHOLE world
  height — the y offsets spanned ±32 slices, i.e. 65 per column (`WORLD_HEIGHT_Y`/`CHUNK_HEIGHT`
  = 32) — so at render distance 32 it was **208,585 entries** while only a column's near-surface
  band could ever pass the filters. (That is what makes the two bullets above a change in kind
  rather than in tuning.) Measured over a 24-chunk flight (headless, RD 32,
  `probes/probe_gen_stats.gd`): **209,408 offsets examined for 2,616 generated (1.2%)**, with
  96,541 rejected as above the column's content (46.1%), 76,054 out of world bounds (36.3%),
  34,102 already loaded (16.3%) and 95 below the band — so **82.4% of every pass is vertically
  impossible entries**, and because the 512-check per-frame budget is exhausted on every frame
  it, not `chunk_generations` (256), is what limits generation: ~6 chunks/frame come out of it.
  The sweep is also cheap in wall time (0.66 ms/frame average, 28 ms worst frame), so the cost
  is not frame time, it is what the budget buys. While flying the cursor restarts on every chunk
  crossing (about every 8 frames here), which is exactly why `reject_loaded` is a sixth of the
  walk: the near rings are re-examined rather than progressed past. Boot looks healthier — 17%
  of checks generate — because with the player parked the first rings are the near-surface band
  rather than a moving frontier. The frustum pass was the first thing the counters caught: a
  real in-game reading (`/genstats` after a 9,379 frames) showed it had spent **2,376,704 checks
  and passed 0 chunks, ever** — the cause was the plane convention, see the loading section
  above, not the budget. With the planes flipped the same reading over 14,045 frames shows it
  examining **129,895 chunks in the frustum**, of which all but ~7 were already resident — which
  is what that pass is for, and why its `generated` count stays tiny in a settled world. The
  other end of the same reading is the phase-2 sweep spending its **entire 512-check budget on
  every frame for 0 generations** (`last 120 frames: 61,440 checks, 0 generated`), which is the
  state a `generated` count of 0 beside a full check budget means. `sweeps_completed` was 0 for
  a reason that is arithmetic rather than mysterious: one complete walk is 208,585 offsets at
  512 checks/frame ≈ **407 frames**, while the cursor restarts on every chunk crossing — 61
  times in 14,045 frames, one per ~230 — so the walk is reset before it can reach the end, and
  the frames right after a crossing are spent re-confirming the ring that is already loaded.
  (`generation_sweep_generated` is also SHARED between the two passes, so a frustum-pass
  generation mid-walk discards phase 2's "this walk found nothing" conclusion as well — a
  second, smaller contributor that only appeared once the frustum pass started working at all)
