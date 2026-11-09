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
- **The initial fill's loading budgets start after the chunk load, not at chunk 500.**
  `WorldUpdater::process_mesh_budgets` now latches a startup mode only when the loaded set
  is quiet (generation and completed queues empty, at least `loading_threshold` chunks
  resident) and mesh work is outstanding; it keeps the 64-rebuild/64-upload budgets and
  the 2 ms `mesh_queue_loading_budget_ms` until the pipeline has been quiet for a second.
  The old count test switched back to 8 rebuilds/32 uploads while ~19,700 chunks and the
  whole far tail were still arriving. Applying the larger budget from the first frame was
  measured and is worse (chunk load **6.01 -> 9.86 s**, and 20 far regions were left with
  active gaps), because it competes with install and light work; the latch is post-load
  for that reason. `probes/probe_start_fill.gd` at RD32 on one world, two runs:
  loaded **6.01 -> 5.95-6.07 s**, mesh + far fill **11.78 -> 7.97-8.08 s**. The remaining
  floor is the chunk-load phase, so this change alone is a ~32% fill reduction rather
  than half
- **What the 8-9 s fill is made of: four stages with large CPU headroom.** The fill probe now sums each report interval's `avg x count`
  (`perf_report.cpp` prints a `count` on the phase lines), so the four stages are totals for
  the whole fill rather than per-frame averages. Four RD 32 runs on one world (16 logical
  cores -> 15 workers, vsync off): fill **8.03-8.38 s**, loaded **6.01-6.32 s** (the
  timestamp includes a 1 s quiet window), mesh + far **8.03-8.38 s**. Generation **19.2-20.8
  s** of worker time over 17,554-18,272 chunks at 1.09-1.14 ms (3.8-4.0 of 15 workers busy
  during the 5.0-5.3 s load); install **0.41-0.47 s** on the main thread (5.0-5.7% of the
  fill); mesh build **4.4-4.7 s** of worker time (0.9-1.0 workers during the load); uploads
  **0.21-0.23 s** (2.5-2.8%); the dirty-mesh queue **1.15-1.21 s** (14.2-15.0%); the sweep
  **0.43-0.53 s** (5.3-6.5%); light **1.61-1.69 s** of lock-held worker time (0.2 workers)
  with **7.7-8.1 s** of wait; the whole controller **2.46-2.64 s** (30.6-32.1%). Nothing is
  CPU-bound: the main thread is idle about two thirds of the fill and the workers are mostly
  idle too.
- **The old "512 checks/frame limits generation to ~6/chunk" reading was the whole-height
  list.** With the per-column band list the probe reads **26.7-28.8 checks/frame** (39,006-
  39,132 checks for 12,307-12,490 sweep generations, **3.1-3.2 checks per generation**), the
  band service is **53.7 ms over 3,209 reads** (max 10 columns in a frame, `rebuilds=1`), and
  the per-frame sweep costs **0.31-0.40 ms** of its 1 ms budget - none of the three is
  binding. What is engaged is the **pressure-scaled generation budget**: the sweep and chain
  together admit ~20 chunks/frame for the 5.0-5.3 s load. The in-flight set averages
  **27-31** (max 83-134 across runs) against the 120 admission cap; 30 is exactly `workers x
  generating_per_worker` (15 x 2), where `thread_pool_pressure` is 1.0 and
  `dynamic_max_generations` is **32**, down from 64, and the 83-97 peaks reach its **16**
  floor. The walk does not spend its whole 32 budget every frame (during the load it
  produces ~14 sweep generations/frame at ~40 checks/frame; the whole run, including the
  ~3 s tail, averages 8.4 generations and 27 checks/frame), and the 120 cap is never
  reached, but the budget and the set are the only pipeline quantities near a limit. The
  intervention says that is not the wall: three RD 32 arms, two runs each, with the fill
  probe's new load-frame metrics (baseline 64/2; divisor 8; budget 128). Baseline **fill
  8.34-8.60 s, load 5.32 s, completed queue avg 69-87, load frame 7.8-8.1 ms, worst
  50.7 ms, 20-27 slow frames**; divisor 8 **8.57-9.05 s, load 5.29-6.03 s, queue
  148-247, load frame 8.12-8.15 ms, 28-33 slow**; budget 128 **8.54-8.61 s, load
  5.27-5.33 s, queue 99-129, in-flight 41-65 (max 217), load frame 8.29-8.41 ms, worst
  71.6-88.9 ms, 39-49 slow**. Neither shortens the load; the extra completions queue
  instead of installing sooner, and both add slow frames (the budget arm also raises the
  worst frame). The generation arms left the drain as the next suspect, and the drain
  test clears it. On the baseline the drain's own queues are light stages avg 0.1
  (max 1), light results 2.0-2.1 (max 18-20) and dirty stages 13-21 (max 55-97) against
  a completed queue of 64-85, and `process_completed_chunks` spends 0.33-0.39 ms of its
  0.75 ms budget (4.8-4.9% of the fill): the light gate is idle, the dirty pass is about
  one frame ahead, and the completed chunks are a supply buffer, not an install
  backlog. Raising the drain budget to 1.5 ms (two runs) made the fill 10.61/11.55 s
  and the load 6.59/6.72 s against the baseline's 10.81/6.50 and 8.55/5.27 s (the
  baseline's first run is a slow outlier), while the drain average rose to 0.44-0.48 ms
  and the completed queue stayed at 77/68: the extra budget spent frame time and did
  not drain the queue. So neither generation admission nor the drain is the wall; the
  remaining suspect is the generation supply's own useful-chunk rate (the band walk and
  chain), not the downstream stages. Baseline restored (64/2, 0.75 ms).
- **The per-chunk path is ~49 ms, and the load is the admission rate, not any one stage.**
  `FARLANDS_GEN_PROFILE=1` timestamps admission (just before the enqueue), worker
  start/finish and install, keyed by a unique attempt id (a chunk can be regenerated after
  an unload while an older attempt is still waiting, so the chunk key alone mixed the two).
  Two RD 32 runs, 20,230 installed samples each (23,286/23,410 admitted and ~3.1k pending
  attempts at the end), give a median total of **49.2/50.1 ms**, split into **wait
  16.2/16.3 ms** (enqueue to worker start, the pool queue), **work 2.9/2.9 ms** (terrain,
  edit map and light on the worker) and **drain 26.7/28.8 ms** (completed queue, light/dirty
  stages, install); p90 total 74.6/76.7 ms, p99 113/120 ms. The drain is the largest stage
  because the completed queue is ~75 chunks deep at the ~3k chunks/s arrival rate
  (75/3k ~ 25 ms), not because the pass is busy - the 1.5 ms budget arm showed raising it
  does not help. The wait is the worker-pool queue (38 in flight, 15 workers, 3 ms tasks;
  the workers are ~60% busy), and the worker itself is 3 ms. The 6.8 s load is 20,230
  chunks at ~3.0k/s (~17/frame): the limiter is the sweep/admission rate, not any per-chunk
  stage. The fill's absolute is in the session's slow mode (10.9/11.1 s against the fast
  baseline's 8.55 s), but both runs give the same split. The profile is off unless
  `FARLANDS_GEN_PROFILE=1`; the probe prints it as `PROBE gen path: ...`.
- **The frame's admission is capped by the pressure-scaled generation budget, not the band or
  the chain.** The fill probe snapshots `get_generation_stats()` when the chunk load settles
  and prints it as `PROBE load sweep`, so the mesh/far tail does not dilute the window. Two
  RD 32 runs (6.86/6.67 s of load, 725/770 update_generation frames) measured 38,793/39,385
  checks, 12,259/12,589 sweep generations, 11,156/11,423 chain generations and 23,415/24,012
  admitted (32.3/31.2 per frame; the earlier ~17 divided by the whole-fill frame rate, not
  the load's ~95-100 fps). The walk's per-frame exit table is **generation cap 456/454,
  wall budget 82/84, admission gate 72/92, check cap 39/39, unread band 0/0, list spent
  1/1**. The pressure-scaled `dynamic_max_generations` is what ends most frames: base 64,
  scaled by the in-flight set (33-36 against 15 workers x `generating_per_worker` 2) to
  ~26; the chain takes 3/4 of it and hit its share in 526/517 frames with 1,061/1,016
  offers still queued at the peak. The band was always ready (0 stalls; 3,209 reads, worst
  frame 6.7/9.2 ms) and the chain was stopping on its budget, not on an empty queue, so
  neither supply is the limit; the 1 ms wall budget is the second exit at 11%, the
  admission gate 10%, the check cap 5%. The pressure scale's 0.25 floor caps the effective
  budget at 32 for any base >=128, which is why the earlier 128 arm did not shorten the
  load.
- **The pressure feedback is not a lever - the pipeline sits at the admission cap, not the
  budget.** Raising `generating_per_worker` from 2 to 3 puts the pressure threshold at 45
  in-flight chunks, so the same 30-35 chunk set no longer scales the budget down and the
  sweep runs at the full 64 instead of ~26. Five RD 32 runs against five at 2 measure no
  change in what the frame actually enqueues: **15.6-16.0 admitted/frame on both arms**
  (23,316-23,623 against 23,415-24,041 in the load window), chain 14.4-15.3/frame against
  14.4-15.3, ~760 frames each. What moves is only *where* the sweep stops: generation-cap
  stops fall from 444-456 to 406-419 (~9%, the budget is no longer the throttle) while the
  admission gate rises from 72-110 to 114-135, because the 120 in-flight cap is what paces
  refills once it exists beside the budget. Load and frame time follow the session's warm-up
  instead: the 2 arm's own load frame drifted 10.66 -> 9.44 ms across the session with no
  setting change between runs, and the last pair, interleaved 3 then 2 back to back, gave
  **6.46 s / 9.71 ms against 6.45 s / 9.58 ms**. So `generating_per_worker` stays 2 and the
  0.25 floor still bounds a larger base; the admission ceiling, and the walk's checks landing
  on already-loaded chunks (`columns_built=0` throughout), are what is left.
- **The admission cap, not the per-frame budget, was the last thing pacing the load.** The
  pressure arm above could not turn a bigger budget into admission because the shared in-flight
  gate (`max_generating_in_flight_per_worker` 8, so 120 at 15 workers) kept closing: it stopped
  93 frames of an RD 32 load in the baseline run and 114/135 in the pressure arms. At 12 per
  worker (180) the stops fall to **8/9** and the load falls with them: **6.41 s -> 5.04/5.30 s**,
  load frame **9.76 -> 8.23/7.93 ms**, frames over 33 ms **80/721 -> 21/711 and 18/765**, whole
  fill wall 10.49 -> 8.86/8.34 s. The walk's exit table moves the way an open gate predicts —
  generation cap 443 -> 520/525 (the per-frame budget is now what ends the frame), admission
  93 -> 8, budget 61/83 — and the set still averages 28.2/31.1 in flight against 112/106 peaks,
  far under 180, so the change removes transient closures rather than pre-loading the pool. The
  `generating_per_worker = 3` arm was measured **under** the old 120 cap, where a bigger budget
  could not become admission; retested on top of the 180 cap it still loses. Three per worker is
  45, so the controller no longer scales until ~34 in flight: the walk's budget stays at 64,
  admission arrives in bursts, and the set peaks **139/155** against 112/106 with **208/251**
  completions per drain against 123/126. Gate stops return (**8/9 -> 74/97**), admitted
  generations fall slightly (17.8/16.3 -> 16.5/16.2) and the load runs **5.60/5.90 s** against
  5.04/5.30 s — the same work in larger, later arrivals, not more of it. So the two constants are
  not additive: the 180 cap is worth having *because* the budget at 2 keeps the set small enough
  for the gate to stay open.
- **The generation claim now lasts until install, not until the worker finishes.** A finished
  chunk waits 37–62 ms in the completed queue, and the in-flight claim (and its filter bits) used
  to be cleared at the worker's handoff — so the chain generated 3,667 chunks in that window and
  the install discarded each one as already resident (23,897 finished for 20,230 installed).
  `ChunkScheduler::release_claim` now drops the claim on install or on any drop (stale epoch,
  already resident, no data), and `generating_count()` deliberately still counts only
  worker-held chunks so the admission cap behaves exactly as measured above. A fill now admits
  exactly its 20,230 candidates (`admitted = finished = installed`, `pending = 0`) at 28.9/28.6
  admits per frame instead of 32.5/30.3, in 700/706 frames instead of 735/790. The load barely
  moved (5.04/4.98 s against 5.04/5.30) because the pool had those 3,667 generations' worth of
  idle time to absorb: the removed work was real, but the load is paced by how many unique
  chunks the per-frame budget admits, not by worker throughput.
- **A feed-forward budget was tried and lost.** Replacing the pressure law with
  `workers × frame / work-per-chunk × 0.9` — at 15 workers, a 6.8 ms frame and a 2.46 ms chunk,
  about **39 admits a frame** — is the rate the pool could finish, but it is not the rate the
  pipeline can *drain*: the set hit the 180 cap in **569/525** frames (admission stops 5 → 569/525),
  the completed queue grew to **333/361** chunks per drain (drain 43 → 150/159 ms), admitted
  throughput fell to **18/frame** (12.8 + 5.2) and the load ran **6.73/6.56 s** against 5.04/4.98.
  The exit table shows the inversion — gen-cap stops fell 428 → 224, because the in-flight gate,
  not the budget, now ended the frame. The pressure law was right by accident: 180 in flight at
  ~43 ms drain sustains  ~29/frame, which is what it admitted. A rate above that only deepens the
  queue, so the budget is paced by the install pass rather than the workers.
  **Retested after the light band narrowed to the resident set, because the collapse had been
  blamed on exactly that contention.** It is smaller and it is still a loss: load 6.13/5.50 s
  against 4.75/4.51 at the pressure law's 29/frame, admission stops 363/286 against 16/4, drain
  131/115 ms against 62/46, gen-cap stops 299/313 against 443/449. Band wait fell to 0.44/0.40 ms
  per band (from 0.68/0.65 in the first arm), so the previous 6.73/6.56 s improved with it -- the
  contention was real and worth a second or so. The rest is the completed queue: at 39 admissions
  a frame a 180-deep in-flight set still hands the drain several hundred chunks at once, and the
  drain's per-iteration cost rises with that depth whatever the bands cost. That arm was reverted
  on its own; the next bullet is the same rate with a cap that keeps the queue shallow.
- **The rate and the cap only work together.** The feed-forward rate was retried under a 6-per-worker
  in-flight cap (90 in flight, half of 180). Load 4.44/4.46 s against 4.75/4.51 for the pressure
  law at 180, wall 7.72/7.76 s against 8.29/8.32, drain 33/37 ms against 62/46, and per-chunk work
  2.21/2.23 ms against 2.46 -- a shallow queue makes every drain iteration cheaper, which is the
  cost the deep arm added. Admission stops rise to 251/271 because the in-flight gate is now what
  ends a frame (gen-cap stops fall 443/449 -> 239/243), which is the intended shape: the cap paces
  to the drain instead of the budget outrunning it. Both runs admit exactly 20,230 chunks
  (`admitted = finished = installed`, `pending = 0`). Kept.
  **Isolated afterwards, because the arm changed two things at once.** The pressure law at the same
  90 in flight loads 4.78/4.71 s and walls 7.80/7.76 s, so the CAP carries the wall win (7.7 vs
  8.3 at 180 in flight) and the rate adds the last ~0.3 s of load; the pressure law's drain is
  even shorter (15/16 ms against 33/37) because it admits fewer chunks a frame. The rate is kept
  because both of its runs load faster than both pressure-law runs at the same cap and the wall
  is identical, which is the one direction the spread does not cover.
- **The install pass now skips the work that is almost never there.** Each install used to take
  three unconditional mutexes and one 7-shard band per chunk: `pending_block_placements`,,
  `pending_vegetation_placements` and `pending_light_removals_` were locked and hash-probed even
  when empty (and the vegetation one is pushed by every generation worker, so the install
  contended with the pool), and the self+6-neighbour fan-out band included the chunk's own
  shard. The three maps carry atomic entry counts that answer "nothing waiting" without the
  mutex, and the fan-out reuses the pointer the install already holds, so it takes a 6-key band
  and no self lookup. The fill moves only in the right direction rather than by seconds —
  admission stops 14 -> 3 in the run after, drain 56 -> 47 ms, load 4.77 s in both — which is
  the expected size: the drain is ~0.4 ms of a 6.9 ms frame. The budget is the pace, not this
  pass.
- **The drain is supply-bound, not budget-bound.** `process_completed_chunks` runs installs, light
  results, lighting bands and dirty stages out of one wall-clock budget, and one install costs
  several loop iterations (install -> lighting band -> dirty stage), so the obvious test is that
  budget: doubling it for loading (0.75 -> 1.5 ms) left installs at 28.1/29.3 a frame and the
  load at 5.04/4.78 s against 4.77 — no change. The loop stops when no completed chunk is
  waiting at that instant, and the arrivals are paced by the generation budget above it. The
  light-result wait is not the stopper either: the light stage runs ~13 bands a frame and a
  result usually marks nothing (its `modified_mask` is empty over daylight terrain). The
  feed-forward arm's collapse at 39 admissions was therefore not a missing drain budget — with
  a 180-deep in-flight set every drain iteration competes with more worker light bands (27-shard
  exclusive) and a deeper completed queue, so the same budget bought fewer installs (18 a frame).
  Budget and contention are different problems, and the fill is bounded by neither at 29/frame.
- **The light worker now locks only the chunks that exist, and the band wait halved.** 27 keys is
  the floor for correctness (a diagonal neighbour is two steps from the centre chunk's corner and
  light travels 15), but not every one of those chunks is resident. The install gate already walks
  the 3x3x3; it now records which of them are present and hands the list to the worker, which
  locks exactly those. Both passes run from the caller's grid and never the map:
  `scatter_sky_light_region_locked` takes the `ChunkData*[3][3][3]` the worker built, so a chunk
  installed between the gate and the band is simply absent from the grid -- it cannot be read or
  written without its shard -- and its own stage covers the boundary from its side. Measured over
  ~8,800 bands: lock wait 10.2/10.1 -> 4.6/5.2 s, 0.52/0.59 ms per band instead of 1.16; hold
  unchanged at ~0.19 ms; the fill wall stayed flat at 8.29/8.32 s against 8.33/8.59 and load
  4.51/4.75 s against 4.78. The band cost was real and is down by half; the wall is bound by
  admission, so recovering worker time did not move it.
- **The walk's second pass was the load's largest block of wasted checks, and it is gone.** The
  band is exact — `above`/`below`/`oob` rejects are 0 in every run — and fixed until the list is
  rebuilt, so once the cursor reaches the end, every in-band chunk of every column has been
  offered exactly once. A second pass can only meet those offers now resident, yet
  `generation_sweep_generated` (set by ANY generation, including the chain's) reset the cursor
  and made one: an instrumented RD 32 load walked the 3,209-column, 20,230-candidate list
  **twice**, spending 39,113 checks on 11,979 generations of which **27,133 were loaded
  rejects** (69%), at 3.3 checks/generation. Re-confirming a resident chunk is not just
  wasted work, it consumes the per-frame check allowance and displaces the pass's own progress
  toward terrain that does not exist yet. The cursor now ends the walk at the end of the list,
  and neither a chain generation nor the flag wakes it: the chain's offers are neighbours of
  installs and keep their own queue, and a refused neighbour is re-offered when one installs.
  Two RD 32 runs against one instrumented run of the old code: **checks 18,747/18,821 (down
  52%), loaded rejects 6,721/6,784 (down 75%), checks/generation 1.6/1.6, check-cap exits
  39 -> 0/0**, with the same 20,230 chunks installed, `Unrendered: 0`, the same 3,209 band
  reads and the same 0.9 ms/frame inside the sweep. The load window fell from 6.41 s to
  6.11/6.01 s (the five baseline runs before it averaged 6.59 s), and the shape of the gain is
  the first pass reaching new terrain sooner rather than cheaper per-check work. `window
  checks=0 gens=0` over the load's last 120 frames is the walk correctly idle there: the chain
  finished the disc. `columns_built`/`columns_skipped` stay 0 because a band is read before its
  chunks exist, so the "column already resident" mark cannot be earned during a fill — the
  single pass is what removes the repeats, not the mark. `sweep_passes` and the probe's
  `list columns=...` line are the instruments that show it (`1` and `3209/20230` now,
  `2` with `39,113` checks before).
- **A crossing resumes the walk by RADIUS, because the list's index cannot survive a rebuild.**
  `rebuild_sweep_columns` re-sorts the disc around the new origin on every crossing, so a cursor
  index means a different column afterwards — which is why it was reset to the near ring, and why
  a 40 s flight spent 2.4 M checks, 98% of them `reject_loaded`, re-confirming a disc that had
  barely changed. The cursor's DISTANCE does survive, because the walk goes outward: every
  column closer than the cursor was offered on the way there. A column at new distance d was at
  most `d + |move|` from the old origin, so every column with `d < r_walked - |move|` is
  provably walked and skipping it is exact; the rebuild resumes at the first column at or beyond
  that radius. A spent list leaves the cursor at the END (`column == size`, the "spent" state
  `advance_sweep` already understands), so its radius is `rd` and the resume lands on the rim
  that just entered the disc. Only a move of at most 2 chunks may resume: a teleport carries no
  bands, so a resumed cursor would sit at a column the frontier has not read and stall the near
  ring behind it. Measured on the same 40 s, 100 blocks/s flight (3.1 crossings/s): **checks
  2,410,854 -> 159,558 (-93%), loaded rejects 2,366,564 -> 108,231 (-95%), cursor resets
  135 -> 0, resumes 124**, walk 1,616 -> 1,083 ms and sweep 1,740 -> 1,223 ms over the 40 s,
  with 51,320 chunks generated in the run (up from 44,288) because the walk's budget now goes
  to terrain rather than to re-confirmation. Holes stayed 0 over 642 samples (100% smooth) and
  all three 2 km cold starts settled, so what the skip removes is exactly what was already built.
  The stationary load is unchanged where it should be — 18,877 checks against 18,747/18,821, one
  pass, one reset — since a load without a crossing has no rim to resume at. `cursor_resumes` is
  in `/genstats` and in the flight probe's walk line.
  The rule was then run in the two cases its slack is tightest in: **diagonal** crossings
  (|move| = sqrt(2) rather than 1) and a **render distance of 4**, where the disc is 49 columns
  and a one-chunk move is a quarter of its radius (`RD` / `DIAGONAL` are the flight probe's
  knobs now). Both were clean: 88 resumes with 0 resets in each 40 s run, every cold start
  settled (6/21/36 frames at RD 4, 0 holes in the 3x3), and the walk cost 472 ms of the 40 s
  (1.2%) against 1,083 ms at RD 32. The small disc does produce single-column gaps — 5 of 2,326
  samples and 6 of 2,299, max one column — so the probe grew a gap ledger that re-checks each
  empty column while it is still inside the trustworthy radius: **seen 6, filled later 6, still
  empty 0, max age 0 samples**, i.e. every one had ground again by the next sample ten frames
  later. A leading-edge transient, not a skip: a column the resume had lost would keep its age
  and end in `still empty`.
- **The 2-3 s after the load is the mesh/far drain, not mesh CPU.** The last generation lands
  at ~5 s (that is where the 1 s quiet starts), and the rest of the fill is the tail: its
  worker cost is 0.9-1.0 builders, its main cost is the dirty queue (1.15-1.21 s) and uploads
  (0.21-0.23 s), and it is paced by the 8/32 then 64/64 rebuild and upload slots plus 17-20
  frames over 33 ms (worst 46-57 ms), not by the builders. The probe now snapshots its phase
  totals at the load, so the tail is measured on its own: 4,530 chunk-mesh builds at ~7.5 a
  frame, 62% of the tail's worker time, with the loading budget active but only 1.21 of its
  2.0 ms spent -- the queue, not the budget, paces it.
- **Moving the mesh backlog into the load was measured at a middle dose and loses.** A startup
  tier of 24 rebuilds and 32 uploads under a 1.5 ms queue budget (the 0.35 ms live-play budget
  only lets ~2 rebuilds through) moved 1,500 builds out of the tail -- 4,530 -> 3,033/3,132, tail
  3.54 -> 2.77/2.51 s -- but grew the load by more, 4.52 -> 5.53/5.78 s. Wall 8.29/8.29 s against
  7.72-8.06 for the same build without the tier. The per-rebuild main-thread cost (0.16 ms: 7
  chunk lookups plus a 64-chunk far walk) is paid on every load frame, and the load's frames are
  what set the fill's pace, so the tail gains less than the load pays. Reverted; the 8/32 active
  budget and the post-load latch stand.
- **The dirty queue's cost was one probe, and none of the four cheaper ideas touched it.**
  `dirty_mesh_queue` was 1.19 s of the fill (0.77 s of the tail), and a per-pop split put 91% of
  it in the buried test: for a fully solid chunk it asks `chunk_would_be_solid` up to six times,
  and a missing neighbour falls through to real worldgen sampling, ~25 us a call on the main
  thread. The cheaper ideas were measured first and all lost or did nothing. Filtering
  out-of-range chunks at enqueue rejected 1 entry in 24,700. Stamping each entry with the chunk's
  `mesh_version` and dropping it at pop when the chunk was clean and already built fired 52
  times in 24,600 and ran the tail 0.28 s longer (3.83/3.83 against 3.55) - the queue was not
  holding stale work, every pop carried a real dirty mark. Replacing the center lookup plus
  `get_neighbors` with one shared 3x3x3 band and `_fast` reads measured 3.82 s against 3.55:
  `get_neighbors` already took that band, so only one acquisition was removed. What worked is the
  memo: keyed by chunk coordinate, cleared on world reset and when the predicate is replaced,
  8,192 entries with drop-everything on overflow, because a re-probe is slower but never wrong.
  The queue falls to 1.02-1.07 s and its tail to 0.68-0.69 s; the fill's tail stays 3.54 s
  because `build_mesh` is 2.49 s of it, so the next lever is the builder, not the queue. About
  four probes in five still miss: the frontier asks mostly new coordinates, so a larger cache
  would buy memory, not hits.
- **Raising the tail's rebuild cap and queue budget together lost, and the stop counters say why.**
  In the tail the visible heap is empty in 96% of frames (everything in the frustum is already
  meshed) and every build comes from the backlog drain, which stopped on the 2 ms clock in 48% of
  tail frames and on the 64-rebuild cap in 40%, with the pop cap never firing. Raising both to
  128 and 4.0 ms, after the load settled and with the load's own 8/0.35 ms budgets untouched, ran
  the tail 3.56/3.84 s against 3.28/3.30 and the wall 7.99/8.36 against 7.68/7.81. It also
  dispatched fewer builds a frame (6.6 against 11.9) over more tail frames (695 against 446):
  with more room each drain empties its heap into the other through the stale-revision re-route
  and then reports "empty" while thousands of entries are still queued, so the bigger ceiling
  only changes where the drain stops. Reverted to 64 and 2.0 ms.
- **The two heaps no longer hand entries to each other, and the fill did not move.** A pop whose
  priority was stale was re-inserted through `push_entry`, which routes on `in_frustum`, so a
  reprioritise made each drain empty its own heap into the other. At the raised ceilings that was
  visible as 563-608 backlog-empty stops in a tail while ~3,800 entries were still queued and only
  6.6 builds were dispatched a frame. Re-inserting into the heap the entry came from (the frustum
  decides the heap at enqueue; an entry in a drain finishes in its own heap one pop later) removes
  the exchange by construction, leaves the backlog's tail-empty stops at 111-117, and keeps all 53
  mesh/lod/concurrency tests passing. The fill, however, is unchanged:
  wall 7.76/8.22 s and tail 3.31/3.52 s against 7.68/7.81 and 3.28/3.30. The migration was costing
  pops, and pops were not what the tail was short of; it still spends 75-78% of its time in
  `build_mesh` and stops on the 2 ms clock and the 64-rebuild cap in roughly equal shares. Kept as
  the structural fix, not as a speedup.
- **A merged far-region mesh is published only after its upload finishes.** The build on the
  pool fills a fresh RID; the region keeps drawing its previous mesh and the instance is
  swapped only when the server reports at least one surface on the new RID. Publishing at dispatch is the
  loading flash: the instance pointed at a mesh that had just been cleared, the content hash
  marked it uploaded, and the member chunks were already hidden — so a region with no geometry
  stood in for them until a later upload happened to finish. The completion also drains in
  `clear()` and before the pool is shut down, and `Region empty` in the performance report
  counts active regions with no surface (0 is the invariant).
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

- **The per-chunk worker body is 2.25 ms, and a fifth of it was the edit-map probe.** Eight
  `GenSliceTimer` slices (off unless `FARLANDS_GEN_PROFILE=1`, appended to the path profile's
  line as `slices/chunk ...`) sum exactly to `work` over 20,230 chunks: **fast-path 0.50,
  columns 0.38, light 0.38, edit 0.48, density 0.28, materials 0.23, vegetation 0.00, solid
  0.00**. The edit slice was not parsing anything - a world with no saved edits still took
  `file_access_mutex` and probed two paths per chunk. `ChunkWorld::has_edit_files_on_disk`
  now lists `user://chunks/` once (cached, reset by `clear()`, forced to "some" by both edit
  writers, and `.edit.bak` counts so a crash-recovery backup is never skipped), and
  `load_edit_map_from_disk` probes existence before the lock, so a chunk with no file never
  queues on the save lock. Same-world A/B, the real world copied into a scratch `APPDATA`
  with the same `world.meta` and the 628 `.edit` files present or absent: **A1 (0) wall
  7.19 s / load 3.91 s, B (628) 7.72 / 4.43, A2 (0) 7.22 / 3.68**; `edit` 0.00/0.16/0.00 ms,
  `work` 1.86/2.06/1.87, `wait` 8.2/9.8/8.6 ms, `drain` 31/45/26 ms. The load is **0.5-0.75 s
  faster with no edit files**, more than 0.16 ms x 20,230 / 15 workers ~ 0.22 s of worker
  time: the per-chunk use of the global file lock was also in the way of saves and the drain.
  The dev world itself has 628 edit files, so it takes the "found" branch; the zero-edit
  branch was verified with a scratch user dir (`gen edit scan: saved edit files none`), a
  line `run_probe_shot.sh`'s stdout grep hides from the log unless the filter is widened.

- **Half the band's top pad was air, and cutting it bought no fill time.** The band pads one
  slice (32 blocks) on both sides of a column's surface estimate, and the install counters put
  **3,181 of 3,209** above-top installs EMPTY: 15.7% of the 20,230 generations were chunks of
  sky, while the only content that can sit above the estimate is a tree (5 trunk plus a canopy,
  under 10 blocks). Splitting the pad - the bottom keeps 32, the top becomes 16, via a new
  `top_pad` on `sweep::chunk_in_band`/`band_for_column` - removed exactly the predicted work:
  candidates 20,230 -> 18,606, empty installs 7,640 -> 6,026, above-top 3,209 -> 1,585. The
  fill did not move: wall 8.24/8.08/7.97 s against 7.49 for the same build, load
  4.18/4.03/4.07 against 3.93, tail 4.06/4.05/3.90 against 3.56. The worker total says why:
  19.46 s for 18,739 chunks became 19.74-21.07 s for 16,993-18,166, because the slices removed
  are the uniform-air fast path - the cheapest generations in the fill, already running in the
  workers' spare capacity (4.95 busy across the load's 4.0 s). The fill is paced by how long
  the expensive chunks take, not by how many empty ones are in the list. Reverted; the
  above-top install counters stay in the report.

- **The light slice is 62% smaller and the fill is 0.7 s slower: the load is install-bound.**
  The worker's initial sky scan walked all 32,768 cells even for the two uniform shapes the band
  generates by the thousand - 3,209 chunks of air above the surface (15 in every cell) and 4,806
  chunks of stone below it (0 from the column's first opaque block) - so `propagate_sky_light`
  was changed to answer those two with one section fill (`fill_sky_light(15)`, and
  `clear_sky_light()` for the solid shape, with the above chunk's first row deciding "open
  sky"). The slice did exactly what it says: `light` **0.53/0.54 -> 0.20/0.21 ms a chunk**, the
  worker body 1.81/1.87 -> 1.54-1.60 ms, **-13%**. The fill went the other way, consistently
  over four runs: wall **8.48/8.80/8.83/9.11 s** against 7.81/8.30, load 4.43-4.82 against
  4.22/4.25. The mechanism is the completion queue: `drain` (finished -> installed) doubled on
  three of the four runs, **52 -> 81-105 ms**, because the workers now finish ahead of the main
  thread's install budget. The load is bounded by install, not by generation, so freed worker
  time becomes queue depth and the nearest-first drain pays for it. Reverted. This is the
  measurement that says a cheaper generator cannot shorten this fill until the install path
  can.

- **The install path is 22 us a chunk, and it is not what the fill waits for.** The load window
  (every phase counter snapshotted when the chunk set settled, printed as
  `PROBE load phase ... per_install=`) puts `process_completed_chunks` at 0.69 ms of its 0.75 ms
  clock a frame - **22.0 us an install**, 30-31 installs a frame - with the generation walk at
  21.8 us an install and the whole updater at **1.42 s of the 4.75 s load**. That clock was at
  96% of its budget, which is why the light-slice arm above lost: it was the ceiling the freed
  worker time ran into. Raising it to 2.0 ms while more than 128 completions were queued did
  absorb the faster workers - load **4.18/4.40 s** against the 4.43-4.82 the light cut alone
  measured (baseline 4.25/4.22) - but the tail grew to **4.63/4.59 s** against 3.55/4.08, so the
  wall was 8.81/8.99 against 7.81/8.30. The wall is load + tail and the tail is a fixed pile of
  mesh builds drained at a fixed rate: generation finishing sooner only hands the tail more work
  at the load boundary, so no cut upstream of the mesh drain moves it. Both halves reverted; the
  load-window print stays as the instrument.

- **Building the tail's meshes during the load starves the load's own pipeline.** The loading
  mesh budgets (64 rebuilds, 2 ms queue) engaged only after the chunk load went quiet, so the
  streaming window ran on the 8-a-frame gameplay cap (4,306 builds) and the tail then drained
  the rest. Extending the loading budgets over the whole first fill is the obvious overlap arm,
  and it lost on every counter: load **10.24 s** against 4.21, checks a frame 22.7 -> 8.8 and
  generations 17.1 -> 7.7, with each chunk's drain wait (`completed -> installed`) **32 -> 207 ms
  p50**. The mechanism is the shared mesh clock: `process_queue`'s budget is the far scheduler's
  too, so 17,466 stream builds during the load (against 4,306) spent the main-thread time the
  installs needed, and the far path the probe waits on never settled - 5,038 of 17,437 regions
  cached, `active_uncovered` 63 at the 90 s cap (`PROBE FAIL: active far gaps did not close`).
  Reverted. The lesson is stronger than "the two budgets trade": the fill's quiet is defined by
  the far regions, so spending the mesh budget on stream meshes delays the quiet itself.

- **The tail's builds are far-tier region sources, and what paces them is the buried test.** At
  RD 32 the far tier starts at `far_lod_distance + 2`, so most of the 5-6k meshes the tail builds
  are per-chunk sources for the 8x8 region meshes: already merged, with nothing left to merge,
  and not the limit either - `build_mesh` is 70-89% of the tail on 0.6-0.8 of the pool's workers.
  The paced work is the drain: ~34 pops in its 2 ms, ~11 of them constructing. 5.5k of the pops
  are the buried test at **0.15 ms each**, 0.8 s of a 4.1 s tail spent deciding *not* to build.
  That is not the solid-probe memo - raising its cap from 8k to 64k moved `mesh_pop_buried` not
  at all (0.83/0.85 s), so the 25 us worldgen sample it guards is not what the pop pays. A 0.5 ms
  floor for the far scheduler (the drain otherwise eats the whole shared ceiling) was the other
  arm: also a no-op, because the far pass has no dirty region in the tail at all and spends
  0.03 ms a frame, so its starvation is not what holds the tail open. Both reverted; the caps and
  the budget are back where they were.

- **The buried test is the six neighbour lookups, and nothing else in the branch.** `MeshPopBuried`
  was split into probes / RID frees / far-cache mark / bookkeeping and measured over two fills:
  the six `neighbor_solid` calls are **0.77/0.80 s of the branch's 0.77/0.80 s** (0.15 ms a pop,
  5,064/5,310 pops), the two `free_rid`s are **0.00 s** over the 2,875/3,018 pops that reach them,
  the bookkeeping is **0.00 s**, and the far-cache mark never runs at all (count 0 - no buried
  chunk in the tail still holds a far cache). `ChunkData::fully_solid()` is a cached flag, so the
  ~25 us a probe is the ungenerated-neighbour predicate (`chunk_would_be_fully_solid` ->
  `get_chunk_height_range`), not the loaded-neighbour check. That the memo for exactly that
  predicate moved nothing when raised from 8k to 64k means it is not capacity thrash: the next
  instrument is a hit/miss count on the memo, not a bigger table. The four sub-timers stay (they
  nest inside `MeshPopBuried`, so they sum rather than double-count).

- **The probe memo is not thrashing - it is barely reachable.** Counting each resolution of the six
  neighbour tests: **9,562** probes before the chunk set settled (174 a loaded chunk's cached flag,
  781 memo hits, **8,607 misses**) and **30,707** by the end (860 resident, 3,757 hits, **26,090
  misses**). That is about one fresh coordinate per loaded chunk: the frontier pops each boundary
  chunk once and asks about neighbours outside the loaded band that will mostly never be
  generated, so there is almost nothing for a memo to reuse and its 8k ceiling was never the
  reason it missed (which is why 64k changed nothing). The ~25 us is one
  `get_chunk_height_range` worldgen sample per new coordinate. Whatever cuts this has to sample
  fewer coordinates - one column range answers both vertical probes and the horizontal pair lookups
  ask distinct columns - not cache harder.

- **One height sample per column instead of one per neighbour cut the tail by 0.7-1.2 s.** The
  six probes around a pop span five columns, and 87% of them missed the chunk-keyed memo; the
  column's lowest possible surface is already in `WorldUpdater`'s column cache (the sweep derives
  it, a worker may have prefetched it), so `chunk_would_be_solid` now reads `land_h` from there and
  calls `chunk_would_be_fully_solid_at_column` - the same rule minus the sample. Measured
  same-session against the probe-count arm: wall **8.60/8.43 s** against 9.03/9.69, tail
  **3.58/3.34 s** against 4.30/4.59, `mesh_pop_buried` **0.07/0.06 s** against 0.81/0.83 s (0.01 ms
  a pop against 0.15), and `dirty_mesh_queue` **0.16/0.15 s** against 0.91/0.92. The load window is
  unchanged (5.03/5.09 against 4.73/5.10): the probe now pays the cold sample for a column the
  sweep had not asked for yet, which is the same work moved, not added. With the drain no longer
  probe-bound the tail is worker-build-bound (`build_mesh` 3.55/3.62 s over a 3.3-3.6 s tail),
  which is where the next arm points.

- **A build is 0.6 ms, 58% of it vertical greedy, and the tail is not build-bound.** The task
  timer (3x3x3 band, 26 neighbour lookups, build, two vertex packs, content hash) reads 0.61-0.63
  ms against `build_mesh`'s 0.57-0.58, so only ~8% of a task is outside the mesher. Inside it:
  `greedy_v` 0.35 ms (58-59%), `solid_cache` 0.11 (19%), `greedy_h` 0.11 (18%). Replacing the
  vertical pass's per-cell, per-direction registry lookups with a per-pass 256-entry table and
  `get_block_fast` in the cull cut `greedy_v` to 0.32-0.33 ms and the build to 0.54-0.56, and
  moved the tail not at all (2.53/2.77 s against 2.55/2.79). The reason is in the same profile:
  `worker_task` totals 2.7-2.9 s across a 2.5 s tail, so only ~1.1 of eight workers is busy.
  The frames themselves say the rest: the tail is 518 frames in 2.53 s at 4.9 ms each, and
  `process_total` in that window is 0.36 s - 0.70 ms a frame, 14.3%. Inside it: completed-mesh
  uploads 0.26, far-region visibility 0.23, the dirty-queue drain 0.18, everything else 0.03. The
  other 4.2 ms is outside our C++ callback, in Godot's own frame drawing the RD 32 world. And the
  drain is a budget-paced backlog walk, not a worker-limited one: its queue averages 5,800
  entries and it calls back ~26 times a frame, of which only 9.3 become real builds (4,802 over
  the tail), stopping on its per-frame rebuild allowance in 278 frames and on an empty backlog
  heap in 232. So neither the builds nor the workers pace the tail - the dispatch allowance and
  the frame cost do. Both arms reverted; the per-part timers stay.

- **Raising that allowance made the tail longer.** `mesh_rebuilds_loading` 64 -> 128 was exactly
  the arm the stop counters pointed at, and it lost outright: tail 3.02/3.05 s against 2.53/2.77,
  wall 7.49/7.84 s against 6.56/6.80, load 4.47/4.79 s against 4.02/4.22. It changed where the
  work happens, not how much: the queue at settle fell to 5,727/8,877 from 11,066/10,880, the cap
  stopped binding (53 of 751 tail frames instead of 278 of 518), and the tail then waited on
  arrivals with an empty backlog heap in 680 of 751 frames while building 3.1 meshes a frame
  instead of 9.3 (2,328/3,841 tail builds against 4,802/4,514). The cap bounds callbacks, and
  three of four callbacks never reach a build, so spending more of it pulls no-ops and uploads
  forward into the load's slower frames and leaves the tail waiting on whatever is still pending
  with an empty heap. Reverted to 64.

- **What outlives the chunk heaps is the far field.** `has_pending_mesh_work()` now returns the
  disjunction of six named reasons (the two chunk heaps, the immediate queue, the far-visibility
  walk, a far region dirty/building/uploading, completed chunk meshes, completed far-region
  meshes) and the bitmask is bound, so a probe can name the holder instead of inferring it. In a
  running tail all six are set in most frames (queue 329, far_vis 365, far_region 447,
  completed_mesh 329, completed_region 312), which is over-determined; restricted to the 364
  tail frames whose chunk heaps are EMPTY, only the far field remains: far_region 118, far_vis
  40, completed_region 4, and zero for the queue, the immediate queue and completed chunk
  meshes. The final tail frame's mask is 0 - it does clear. So the ~680 empty-heap frames of the
  128-cap arm were waiting on far-region rebuilds (the 250 ms debounce and their uploads) and the
  incremental far-visibility walk, not on anything the chunk mesher owns.

- **Moving that far work earlier was neutral.** The follow-up arm shortened the fill's far-region
  debounce (250 -> 60 ms) and thickened the visibility slice (0.1 -> 0.5 ms) while the chunk
  queue drains, and it did exactly what it was built to do: far-visibility pending frames fell
  from 320/361 to 265/259, the walk held an empty-heap tail frame 13/51 times against 6/6, and
  empty-heap tail frames fell from 231/399 to 109/206. The fill did not move: tail 2.52/2.50 s
  against 2.54/2.79, wall 6.98/6.74 s against 6.78/6.98, load 4.46/4.23 against 4.25/4.19 (same
  session, arm then baseline). The far-region dirty/build/upload bit still held 59 of the arm's
  109 and 59 of its 206 empty-heap frames, and the chunk queue itself is pending in 311/315 of
  the tail's frames - so neither the debounce nor the walk was the tail's length, only the last
  residue of it, and moving them earlier just trades load for tail. Reverted; the reason
  instrumentation stays.

- **Two-thirds of the tail's drain pops cannot build.** Counting the outcome of every queue
  callback over the tail: 15,176/14,632 callbacks, of which 5,152/5,056 built a mesh
  (34.0/34.6%), 4,964/4,813 were all-air chunks (32.7/32.9%), 2,787/2,742 had every own and
  neighbour version already built (18.4/18.7%), 2,236/1,987 were fully solid and enclosed
  (14.7/13.6%) and 37/34 had no render data (0.2%); not one pop was refused for distance, a
  missing pipeline or an in-flight build. So 66.1/65.4% of pops produce nothing - but they are
  cheap: the whole pop path is 0.16-0.17 s of callback, 0.07 s of chunk/neighbour lookup,
  0.05-0.06 s of the buried decision and 0.02 s of task construction, ~0.31 s across a 3.0 s
  tail, and the drains still stopped on an empty heap in 738/753 frames rather than on the
  allowance. Skipping them at pop therefore cannot shorten the tail by more than a fraction of
  that; the one bucket worth moving is the all-air third, whose answer is already known at
  install time (mark it clean there instead of queueing it), not a fourth early-out in the
  callback.

- **All-air chunks no longer enter the mesh queue, and the tail lost three quarters of a second.**
  The outcome counters above showed a third of the fill's pops were all-air chunks, so every
  enqueue path now skips them: the light stage that queues a freshly installed chunk, the
  boundary fan-out (the install and light copies both go through `should_dirty_neighbor`, which
  now asks only "can this neighbour draw at all" - an all-air one never can, whatever arrived
  beside it, which made the old `has_boundary_blocks_on_face` test dead for every remaining
  caller), and the light-write dirtying, whose 3x3x3 mask used to queue sky chunks the pass had
  written even though they can never draw. Measured in the same session: callbacks in the tail 7,977/8,452 against
  15,176/14,632, air pops **0/0** against 4,964/4,813, built 4,041/4,298 (the work that was
  really there), tail **2.26/2.29 s** against 3.05/3.01 and wall 6.47/6.77 s against 7.04/7.23
  (and against the 2.54/2.79 tail of the pair before that, the same direction). Kept.

- **A far region can read its members' data cheaply, but reading it removes no work.** The
  spike (`FARLANDS_REGION_SPIKE=<regions>`, `mesh/mesh_manager_region_spike.cpp`) runs inside the
  far-region worker: for each member it takes that chunk's own shard shared, copies the chunk,
  releases, and meshes the snapshot at far detail with a throwaway builder - no upload, no change
  to the visible mesh. Two armed regions of 16 members each measured, over two fills: **856 KB**
  copied, snapshot **0.5/2.3 ms** a region, shard wait **0.20/1.51 ms max**, hold **0.09/0.01 ms
  max**, far-detail mesh **0.46/0.48 ms a chunk** (15.4/14.6 ms a region), 30,524 vertices. So the
  read does not need a region-wide band and does not stall installs: a one-chunk shared hold is
  tens of microseconds, and the single 1.5 ms wait was one install holding that shard. Full size
  was measured too: a pair armed at 64 members found the fill produces exactly **one** such
  region per run (64 reached, of 4,039/4,050 region builds, so `FARLANDS_REGION_SPIKE=2` sampled
  one), and it read **1.74 MB** in a **0.4/0.5 ms** snapshot with a **0.11/0.09 ms** max hold,
  then meshed in **17.9/20.7 ms** (0.28-0.32 ms a chunk, 68,680 vertices). That is sub-millisecond
  reading at full width, and the meshing is the same work the per-chunk path does. But that meshing is the
  same 64 far-detail builds the per-chunk path already runs, so a direct-data region task trades
  dispatch for one sequential loop and removes no work. The tail confirms it: 2.80/2.53 s armed
  against 2.78/2.52 baseline in the same session (wall 7.35/7.29 against 7.60/7.59, inside the
  drift), with the counters otherwise untouched. Decision: the read path is proven safe, and the
  full rewrite is NOT justified on fill time, because the tail is not build-bound (see the
  0.61 ms-task entry above). The spike stays as the instrument; it is inert without the env var.

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
  only: **0.5 ms** for the first one, **0.25 ms** per crossing; band reads are capped at
  **2 ms/frame** on a full rebuild and **0.6 ms** on a movement crossing (below), and the
  worst `update_generation` frame at boot is 743 ms → 47 ms. `/genstats` reports `list
  rebuilds` and `bands read` as separate timings because their shapes have nothing in common

## A crossing carries its bands

- **A movement rebuild carries every band it already has instead of re-reading the disc.** A
  column's band depends only on that column's own surface bounds, not on where the player
  stands, so the columns shared by the old and new discs keep their band — and their
  fully-resident mark, because the loaded map only loses a column through `try_unload`, which
  already erases that mark. Only the crescent entering the disc is read. A dense carry grid
  (one cell per old offset) avoids allocating a map per crossing, and `built_columns` is
  pruned to the new disc rather than cleared. A render-distance change or a terrain
  invalidation still rebuilds every band; the carry is only for a pure move. This supersedes
  the "frontier still reads every column" reading in the prefetch A/B above: that experiment
  moved the derivation to a worker but left the re-read in place.
- **The band budget follows the rebuild.** `sweep_band_budget_ms` is 2 ms for a full rebuild
  and 0.6 ms for a movement rebuild, because the latter has only the entering rim left;
  spreading that rim over a few frames removes the boot-sized batch from the crossing frame.
  `service_sweep_bands` still always reads at least one column, so the frontier cannot stall,
  and `advance_sweep` still stops at the first unread band rather than reading it.
- **Measured with the windowed 4-border probe** (`probes/probe_border_fps.gd`, 5.6 blocks/s,
  1152×648, vsync off, cap 0, frames within 0.5 blocks of a border): crossing p95 **6.36 →
  2.77 ms**, crossing max **22.55 → 9.18 ms**, and crossing stutters **27/397 (6.8%) → 4/504
  (0.8%)**. The session's worst band frame fell from **2.00 ms over 2,300 columns** to **0.76
  ms over 5**, and `update_generation`'s worst frame from **11.11 ms to 4.24 ms**; the list
  rebuild is unchanged at ~0.3 ms.
- **The full 12-border flight agrees.** Crossing p95 **6.73 → 2.81 ms**, crossing max **27.80 →
  23.52 ms**, crossing stutters **93/1,112 (8.4%) → 17/1,649 (1.0%)**, near-border p95 **4.26 →
  3.12 ms**, interior p95 **1.61 → 1.28 ms**. The crossing maximum is still one 23.52 ms
  frame, but it is not a band read (band max 0.78 ms over 6 columns) and the interior has the
  same class of >20 ms frames (21.30 ms); the remaining first-frame cost is outside the band
  path.
- **The 100 blocks/s streaming benchmark does not pay for it.** Same 40 s flight as the
  prefetch measurements: **124 crossings, 8,060 band reads (~65 per crossing, the entering
  rim)** where the historical shape was **397,907 (~3,209 per crossing)**; band time **111.9 ms
  vs 2,820 ms**, rebuild time 34.6 ms, total sweep 2,017 ms. Cold starts **197.5-229.6 ms
  (mean 215.3)**, **0 holes in 1,130 flight samples**, **0** prefetch requests outstanding at
  the end, and the session's 2.0 ms worst band frame is the full-rebuild boot budget, not a
  crossing. Flight throughput was 1,159 chunks/s in this run; that counter is route and
  world-state dependent, so the controlled comparison here is the band work.

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
