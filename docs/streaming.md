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
- **The initial fill's loading budgets start after the fill settles, not at a chunk count.**
  `WorldUpdater::process_mesh_budgets` now latches a startup mode only when the fill is
  settled (the sweep list is spent, generation and completed queues empty, loaded chunks >
  0) and mesh work is outstanding; it keeps the 64-rebuild/64-upload budgets and
  the 2 ms `mesh_queue_loading_budget_ms` until the pipeline has been quiet for a second.
  The old count test switched back to 8 rebuilds/32 uploads while ~19,700 chunks and the
  whole far tail were still arriving; it also never fired at RD 4/5, where the whole disc is
  below 500 chunks. Applying the larger budget from the first frame was
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
- **The last ~150 installs get a 2 ms drain budget once nothing else is left.** The 0.75 ms
  cap above is right while generation is still arriving, but it is also the final wave's
  clock: at RD32 the tail trace showed 127 completions at 4.433 s and the settled flag at
  4.521 s, about 25 installs a frame. `effective_completion_budget_ms`
  (`chunk_world_install_budget.hpp`) raises the completion budget to 2 ms only when
  `generating_count()==0`, the completed queue is at most 192, and the light/dirty stage
  queues are at most 1. Five AB/BA pairs with both arms rendering
  (`FARLANDS_FINAL_INSTALL_BUDGET_MS` 0.75 vs 2, 15 workers) put exact fill settle at
  **4.154 [3.999-4.762] s** vs **4.051 [3.912-4.101] s**, paired **+0.165 s
  [-0.102-+0.704], B faster 4/5**; loaded/end moved **+0.06 s** in the same 4/5 pairs (A's
  4.762 s run is the outlier). A tail trace at 2 ms drained its last 144 completions in
  37 ms (4.049 -> 4.086) against the baseline's 127 in 85 ms (4.433 -> 4.518); load frames
  were 7.74 vs 7.77 ms and the worst sampled frame 54.58 vs 55.12 ms, so the extra
  main-thread work did not raise the frame cost. This cuts the fill settle by ~0.1 s, not
  the ~4 s load. On the constrained 6-core/5-worker arm the same five-pair A/B is a wash:
  settle **9.685 [9.622-9.740] s vs 9.599 [9.546-9.784] s**, paired **+0.035 s
  [-0.099-+0.144], B faster 3/5**, load frame **4.54 vs 4.59 ms**, 21 slow frames in both.
  The boost helps only where the completion queue is the last work, not where the load
  itself is the wall.
- **The gap between fill settle and `end` is the probe's confirmation window, not a
  mesh/far tail.** A 15-worker trace had fill settle at 3.912 s, the first quiet chunk-load
  sample at 3.951 s, and `loaded`/`mesh`/`far` all at 4.71 s, while every tail-phase counter
  (`process_completed_meshes`, `dirty_mesh_queue`, `far_region_visibility`) stayed at zero.
  The ~0.8 s is the tail of the probe's 1 s `LOADED_STABLE_SECS` window; the engine's fill
  work is already done at settle. Shortening that window would only make the number smaller,
  not the fill shorter.
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
- **Retested under the deep completed queue, the install budget is still not the lever.**
  The earlier 1.5 ms arm ran with the completed queue 68-77 deep (generation was the shallow
  side); under the current feed-forward admission it sits at **~175**, so the install pass really
  is the throttle. `FARLANDS_INSTALL_BUDGET_MS=1.5` (env-gated, default 0) gave **load 4.78 s
  against 4.56 s**, wall **7.36 against 6.85 s**, slow frames **47 against 20**, installing 32
  chunks in 628 frames against 28.6 in 706. The extra 0.75 ms per frame lengthened the frames
  (load frame 8.35 against 7.57 ms) more than it added installs, and the per-install cost rose
  with contention (28.0 against 27.1 us), so the load stays frame-time x install-throughput. The
  override is reverted; a sub-5 s load needs cheaper installs or fewer chunks, not a longer
  budget.
- **The 27 us install, by subphase.** `FARLANDS_INSTALL_PROFILE=1` splits
  `process_completed_chunks` into the install core, the light and dirty stages, the completed-light
  result drain, and empty/real buckets (the probe prints it as `PROBE install profile`). Over the
  fill's 20,230 installs the core is **9.61 us for the 7,640 all-air chunks and 16.80 us for the
  12,590 with blocks** (earlier fills gave 9.36/13.30 and 9.72/16.06 -- insert and tail trade places
  between runs): insert 4.29/8.63, tail 2.89/4.42, data 1.65/1.82, prepare 0.39/0.63, placements
  0.11/0.55, listener 0.11/0.56, poll 0.17/0.19. The stage pass is **4.05/4.60 us for the 28-key
  light scan** and **3.57/2.42 for the dirty queue**, and the completed-light result drain is
  **0.00/10.62 us** -- a result exists only for a chunk with an emitter or shade, so every drain
  execution lands in the real bucket, which is **6.61 us per install across all installs**. The
  profiled total is **17.24/34.42 us, 27.9 us per install**, against the pass's own 28.0 us
  (`0.57 s` / 20,230): the account closes, and the drain is the largest single stage -- bigger than
  the 28-key scan -- and its per-result work is the mask walk (up to 27
  `mark_chunk_dirty_for_light` calls) plus a dirty-stage push, at the top of every loop iteration.
  So the 28-key
  scan is not the ~11 us the core profile left behind: it is ~4.4, the dirty stage ~2.9, and the
  drain ~6.6. The earlier ~3.8 us residual was that drain on a fill whose pass measured 26.0 us;
  the phase totals across fills vary by a few microseconds (insert and tail especially), so the
  residual and the measured drain do not line up exactly across runs. Zeroing
  the core would save 0.28 s, the two stages 0.15 s and the drain 0.13 s, so the install pass is
  ~0.5 s of a 5.5 s load: the 0.8 s target is not in it alone.
- **The light-result drain's per-bit mark resolves each chunk once.** The drain used to call
  `mark_chunk_dirty_for_light` for every set mask bit, and that path made two locking chunk-map
  lookups per chunk: one for the `mesh_version` bump, one inside `queue_dirty_chunk`. The drain now
  calls `mark_light_modified_chunks` with the pass's mask; it loops only the set bits and queues
  each from the `ChunkRenderData*` it already holds (the mark functions moved to
  `mesh_manager_lifecycle.cpp`, which keeps `mesh_manager.cpp` under the cap at 474 lines).
  Measured: `stage_results` **10.62 -> 7.17 us per non-air install** (6.61 -> 4.46 across all
  installs; ~0.13 -> 0.09 s), and the fill ran 5.54 s loaded / 5.81 s end against 5.57/5.83 before.
  The wall did not move: the drain was ~0.13 s of a 5.8 s fill and the frame budget absorbed the
  difference, so only the phase is evidence here, not the fill time. The pass's 22.5 us/install
  this run against 26-28 in the previous ones is mostly drift (insert and tail fell by more than
  this change can explain).
- **The load window is not render-bound: a paired NO_RENDER arm is, if anything, slower.**
  A five-pair AB/BA run of the current build (`probes/start_fill_ab.py`, one discarded warmup,
  `NO_RENDER=1` against the normal loop) loaded **5.53 s median [5.30-5.64]** with rendering on
  and **5.60 s [5.21-5.82]** with it off; the paired A-B median is **-0.19 s [-0.52-+0.32]**, with
  NO_RENDER faster in only 2 of 5 pairs. End times are **5.80 s [5.56-5.90]** against
  **6.07 s [5.48-6.12]**. The earlier 5.19/5.51 pair was the low side of a wide distribution, not
  a 0.3 s arm: the renderer owns no measurable part of the load window, so a hidden first fill
  would buy nothing (not the ~1.7 s the all-air admissions ceiling holds). The 5.82 s default run
  is an ordinary end time at the top of the normal range, not a separate regime. The tail's drain
  frames remain where `NO_RENDER` pays (the entry below: 5.75/5.62 -> 1.53/3.89 ms), because
  their cost is the upload-side RenderingServer work.
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
  installs needed, and 5,038 of 17,437 regions were cached; the old probe gate
  reported `active_uncovered` 63 at the 90 s cap, but that counter is not a hole count (see the
  300 s note below), so it does not prove the far path failed. Reverted. The lesson is stronger
  than "the two budgets trade": the fill's quiet is defined by
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

- **The tail is frame-bound, and the frames outside the updater are mostly the scene's own
  `_process`.** The probe now reconciles the tail's wall against its own frames (`PROBE fill tail
  accounting`): **2,308 ms wall = 2,236 ms frames + 72 ms gaps** over 664 frames, and **1,518 =
  1,470 + 48** over 263, so there is no hidden wait - the tail is CPU time per frame. The frame
  capture's window average over the **first 256** tail frames (the far drain) is **5.75/5.62 ms**,
  of which `ChunkManagerProcess`/`ProcessTotal` are **1.42/1.37 ms**, the renderer measures **0.55
  ms CPU / 0.57 ms GPU** at 248 draw calls and 1.36 M primitives, and `PlayerProcess`/`Outline`
  are 0.01 ms. The **last 256** frames fall to **1.14 ms** once the drain stops. `QUIET=1` (every
  scripted node of the world scene silenced except the player controller, 20 nodes) first looked
  like a **1.1-2.6 ms** per-frame win, but that window is phase-sensitive: some runs captured the
  tail's idle end (`chunk_manager` 0.04 ms) and some the drain, and the same 256-frame average then
  moves by more than the effect being looked for. Timing the bodies directly (`SCRIPT_COST=1`, 500
  `_process` calls per node) settles it: **78.6 us** total over 18 nodes - viewmodel **20.5 us**,
  crafting_table_menu 14.0, inventory 13.8, shader_overlay 9.5, hotbar 5.6, the other thirteen
  below 3. Single fills agree: shader_overlay off 7.43/7.02 s, viewmodel off 7.08/7.32, all twenty
  off 6.54/6.78, all on 7.52-8.40 - the one-script runs do not reproduce the all-off wall, so no
  script is the drain frames' millisecond item. With the render loop off (`NO_RENDER=1`) the same
  drain frame falls from **5.75/5.62 ms to 1.53/3.89**, and to **1.26/0.85** with the scripts off
  too, so the rest is the renderer's main-thread side rather than scene processing or physics. The
  viewport's own measures are only **0.55 ms CPU / 0.57 ms GPU at 248 draw calls**, which rules out
  drawing time: the cost is the RenderingServer work the *drain* keeps dirty - mesh uploads and
  instance updates - not the static frame (the idle tail is 1.14 ms with rendering ON, and the
  updater itself halves with the loop off, 1.42 -> 0.49/1.25 ms, because its upload calls stop
  paying for the server's processing). The probe's own report costs 3.9-4.6 ms a call and
  0.12-0.15 s in total, so the instrument is not the wall. Two consequences: the drain's per-frame
  renderer work (fed by the mesh uploads) is the largest single item in a tail frame, and **no
  tail-only change reaches 5 s** - the load window alone is 5.0 s, so the lever is the drain's
  frame COUNT and its upload churn together with the load, not a cheaper updater or quieter
  scripts. **A per-frame cap on the region uploads was A/B'd and lost** (`FARLANDS_REGION_UPLOAD_CAP=1`
  against the budget's 8): the per-frame drain fell from **4.27/6.61 ms to 1.02/2.17/2.89** and the
  updater from 0.90/1.50 to 0.05-0.71, but the wall came out **6.75/6.83/6.83 s against 6.68/6.78**
  (loads 4.48-4.81 against 4.40-4.48) over more tail frames (451 against 281) - the same renderer
  work spread thinner is not less work. The cap is reverted. **Cutting the region upload volume
  does not shorten the fill either** (`FARLANDS_FAR_REGION_DEBOUNCE_MS`, a longer coalescing window
  with the immediate-rebuild bypass off): it took the volume from **2,164/2,237 surfaces and
  42.8/43.4 M vertices to 234/4.97 M at 500 ms, 234/4.97 M at 1000 ms and 217/4.70 M at 4000 ms**,
  and the wall rose with the window - **7.32 s, 8.64 s and 11.57 s** against **6.85/7.09 s** (tails
  2.81/3.82/6.56 s against 2.28/2.55 s). The window resets on every arrival, so a region whose
  members keep streaming in never rebuilds until streaming stops: its far chunks stay uncovered by
  the region for the rest of the drain (drawn individually), and the far condition closes only after
  the last rebuild. The volume is real - a region re-uploads its whole merged surface per rebuild -
  but partial rebuilds are what keep the far chunks covered, so the load window is the remaining
  lever, not the rate or the volume. The window is reverted; the `Region uploads:` counters stay as
  the volume instrument. **The post-load caps WERE the fill's limiter, and opening them cut 1.3-1.8
  s.** `FARLANDS_NO_FRAME_LIMITS=1` (every load-path clock and count opened) first took the tail from
  2.79 s to 1.00 s but the load from 4.80 s to 5.94 s - frames 8.3 -> 29.2 ms and installs 27 ->
  143 us under contention - so the wall (6.94 against 7.60) sat inside drift. Narrowed to the
  post-load latch, it reproduced in both orders: **tail 1.26/1.51 s against 2.57/3.03 s, wall
  6.31/6.55 s against 7.67/8.32 s, load unchanged (4.81/5.29 against 5.10/5.29)**, the tail
  accounting falling from 1,580-2,032 ms over 323-519 frames to 266-514 ms over 21-43. The loading
  budgets now ship as the arm's values - **512 rebuilds, 128 uploads** (from 64), the **1000 ms**
  queue/completion/far-visibility clocks - and the five-pair end time was **5.80 s
  [5.56-5.90]**; the earlier 5.82 s single run (load 4.56, tail 1.25) is inside that range
  rather than a separate regime. The cost is tail frame time: those frames average 9-12 ms against ~5, and the fill's slow
  frames are 45-56 over 618-649 frames against 47-51 over 993-1,196 - a slower, shorter tail. The
  load's own caps stay closed (the 5.94 s arm is why), so the remaining lever for <5 s is still the
  load window. The numbers come from `probe_start_fill.gd`'s tail windows, `tail accounting`,
  `QUIET`, `SCRIPT_COST` and `NO_RENDER`.
- **A tail-clock ceiling does not reproduce the 1.3 s tail; this session's control is already
  0.52 s.** Four five-pair blocks (`probes/start_fill_ab.py --tail-budget-ms N`, normal render,
  default cores, vsync off; outputs `.freebuff/tail_clock_{1000,8,12,16}_20261109_122733`)
  measured, for
  1000 / 8 / 12 / 16 ms, tail **0.52 [0.26-0.54] / 0.27 [0.25-0.52] / 0.26 [0.25-0.28] /
  0.52 [0.26-0.53] s**, load **5.77 [5.52-5.83] / 5.78 [5.51-5.80] / 6.02 [5.53-6.33] /
  5.87 [5.55-6.07] s**, and end **6.09 [6.02-6.34] / 6.07 [5.76-6.29] / 6.27 [5.81-6.59] /
  6.14 [6.07-6.59] s**. Whole-fill worst frames were **64.26 / 63.10 / 61.54 / 67.33 ms**
  and slow frames **34 / 33 / 39 / 50**. The 1.3 s reference is the 5.82 s run (load 4.56,
  tail 1.25), not this session's control; the capped tails do not trend with the cap (8 and 12
  are shorter, 16 equals the control), and the sequential blocks' load medians differ by 0.25 s,
  so this is a direction check, not a causal 0.2 s. The override sets all three clocks - queue,
  completion and far visibility; the probe has no tail-only p99, so the whole-fill worst/slow
  figures are the frame evidence.
- **Interleaving removes the apparent clock effect: the tail is ~0.53 s at every value.**
  Five rotated rounds with one normal-render run per clock per round
  (`FARLANDS_LOADING_TAIL_BUDGET_MS`; output
  `.freebuff/tail_clock_interleaved_20261109_124531`) give tail medians **0.53
  [0.51-0.83] / 0.53 [0.51-0.78] / 0.55 [0.51-0.80] / 0.54 [0.52-0.77] s** for
  1000 / 8 / 12 / 16 ms. Within-round differences from the 1000 ms control are **-0.01
  [-0.05-+0.04] / +0.00 [-0.04-+0.29] / +0.01 [-0.27-+0.23] s**, lower in 3/5, 2/5 and 2/5.
  The block run's 8/12 advantage was session drift; this arm has no measurable clock effect.
  The 1.3 s reference is still the different 5.82 s run; this control is 0.53 s. Whole-fill
  worst/slow medians were **76.34/104, 72.93/98, 75.92/100 and 73.67/100**; the probe has no
  tail-only p99.
- **Fast flight after a fill now starts with the tail already drained - and, while the latch is
  up, under the loading budgets themselves.** A report from play: flying at a high speed
  (`/fly 25`) felt roughly ten times faster than before the budget change and no longer slower
  than the initial radius. Two mechanisms fit that. The mesh/far tail now finishes in 1.2-1.5 s
  instead of 2.6-3.0 s, so a player who starts moving soon after spawn is no longer competing
  with a multi-second meshing backlog; and `initial_loading_` keeps the loading budgets in force
  until 1 s without pending mesh, so the same window that drains the tail also meshes live
  streaming at 512 rebuilds/128 uploads per frame. The 30 s cap ends that boost for good: it
  stays latched off until the mesh backlog it was covering has drained, so a sustained flight
  past 30 s drops to the active 8 rebuilds / 32 uploads / 0.35 ms. The A/B below measures the
  trade.
- **The 30 s cap binds once it latches; what it buys is frame time, paid for with mesh work.**
  Five AB/BA pairs of `probes/start_fill_ab.py --latch` (RD 32, one discarded warmup, a 36 s
  flight at 100 blocks/s started when the latch raised the mesh budget; A = the shipping 30 s
  cap, B = `FARLANDS_LOADING_LATCH_MAX_SEC=1000000000`, both with the 1 s quiet exit) show the
  cap firing in every A run at **29.99 s [29.98-30.00]** and never in B, and **A finished the
  flight at budget 8 where B was still at 512**. The release condition had to be measured:
  clearing the latch when the chunk load leaves its quiet state was built first, and a flight
  this fast leaves that state every few frames, so the next quiet frame restarted the window
  and A still ended at 512. It now stays capped until mesh work has been quiet for the usual
  second - the backlog has drained - which is what makes it a ceiling. The last six seconds
  separate cleanly: mean **2.23 ms [2.09-2.29] against 8.68 [7.01-9.15]**, p99 **9.16
  [8.56-9.53] against 22.80 [18.24-30.81]**, slow frames **0 [0-0] against 1 [0-6]**, with A
  lower in all five pairs (late mean -6.45 ms [-6.92 to -4.92], late p99 -14.03 [-21.65 to
  -9.68]). Over the whole flight the means are **3.01 [2.89-3.23] against 3.75 [3.23-4.07]
  ms** and the p99s **17.05 [16.55-18.11] against 21.01 [18.06-22.41]**. That cheaper frame is
  work not done: for the last six seconds A ran 8 rebuilds/32 uploads where B ran 512/128.  That run did not count the backlog left at the end (the probe gained the counter for the
  constrained rerun below), so what the lower budget left unmeshed there is inferred from the
  budget rather than counted; the single worst frame also does not follow the distribution (A
  **62.85 ms [56.46-87.33]** against B **55.54 [54.07-60.98]**, one frame each). A also counted
  **40,731 [40,649-40,856]** generations against B's **42,716
  [42,053-42,980]**; the cap does not touch generation, so this is reported and not attributed.
  The transitions are the pure `LoadingLatch` (`src/world/loading_latch.hpp`), pinned by
  `tests/test_loading_latch.cpp`.
- **On six cores with vsync the cap's cheaper frames leave 14.3k chunks queued, and of the
  2.8k in view only 61 would mesh empty: the rest is missing geometry.** The same five AB/BA
  pairs (`--cores 6 --vsync`, mask 0x3f inherited by every Godot run, `PROBE fill vsync=1`
  in both arms; output `.freebuff/start_fill_ab_20261109_102253`) put the latch under the
  constrained case, with the end-of-flight queue split by the current frustum, LOD tier,
  distance band and what the frame draws for the far tier, then each no-geometry far chunk
  put through the mesher's own buried test. A again ended at budget **8** against B's
  **512**, and the last six seconds are again apart: mean **8.03 ms [7.78-8.48] against
  14.79 [13.77-16.33]**, p99 **26.08 [24.95-30.71] against 48.06 [42.99-60.76]**, slow
  frames **2 [1-5] against 21 [9-23]**, with A lower in all five pairs (late mean -6.76 ms
  [-8.24 to -5.97], late p99 -21.98 [-33.78 to -12.28]). Over the whole flight the means
  are **12.06 [11.19-12.32] against 13.81 [12.41-14.84] ms**, p99 **50.71 [48.32-51.49]
  against 51.58 [47.33-55.88]**, and A rendered 2,983 frames to B's 2,604.
  At the end of the 36 s flight A had **14,305 [13,850-14,673]** chunks waiting in the
  dirty/backlog queue, of which **2,806 [2,513-3,163]** (20%) passed the current frustum; B
  had **0 [0-0]** in both. The visible set is still mostly far: medians **177 full, 814 mid,
  1,822 far** (6% / 29% / 65%), with bands **3 / 26 / 148 / 685 / 1,799 / 137** for 0-1 /
  2-3 / 4-7 / 8-15 / 16-31 / 32+ chunks.
  The far-geometry count answers what those 1,822 far-tier entries are. **1,676 have no
  geometry at all, 51 have no resident chunk, 53 have hidden geometry, and 15 are still
  drawing an older mesh; none is all-air.** The buried-test refinement then asks whether the
  1,676 are real: **206 are fully solid, and 61 pass the mesher's buried test** (fully solid
  and every face neighbour solid, or predicted solid when ungenerated; the bottom edge counts
  as solid), so **1,610 [1,347-1,926] would produce faces**. Of the whole 1,822-entry far set,
  **1,740 [1,451-2,018] are neither drawn nor would-be-empty** - the potential holes - while
  15 still draw an older mesh, 51 have no resident chunk, 53 have hidden geometry and 0 are
  all-air. The queue is not holding old far LOD in place; only 0.8% is stale geometry. It is
  leaving unbuilt terrain: 96% of the no-geometry set would emit geometry, and only 3.6%
  would be skipped as empty. The 206 fully solid chunks that are not enclosed still emit
  boundary faces, so the empty count is 61, not 206. `empty = 0` rules out sky chunks. B's
  512 rebuilds drain the queue and end at zero. B pays 21 late slow frames against A's 2 and
  ends with an empty queue, while A keeps the frame cheap and ends with 14.3k chunks of mesh
  work still waiting, about 2.8k of them in front of the player and ~1.7k of those far-tier
  chunks with no mesh. The `reasons_end` mask in `results.csv` names the other pending kinds
  (B's is 26-58 despite its empty queue). A also counted 36,118 generations against B's
  32,341, the same reversal of the 16-core run, so the backlog is not a lack of new chunks -
  it is mesh work the cap deliberately did not do. The medians of the categories need not
  add up to the median total, because each field is a median of its own; every run's
  categories sum to its own far total, `solid` and `enclosed` are subsets of `no_geometry`,
  and its tiers and bands sum to its own visible count. That count is a frustum-only,
  chunk-level set: the 1,740 are potential holes, not a screen-area measurement, and the 61
  are exact to the engine's skip rule; a fully solid chunk that fails enclosure builds only
  its exposed boundary, and a non-solid one may be mostly underground. The absolute frame
  times belong to the display's refresh and to the session; the comparison is the two arms
  inside it. This is the constrained case the review asked for; the four-core run below is
  the smaller-CPU check.
- **On four cores with vsync the cap still fires at 30 s; the smaller end queue is a smaller
  workload, not a weaker cap.** A second five-pair run (`--latch --cores 4 --vsync`, mask 0xf;
  output `.freebuff/start_fill_ab_4core_20261109_120817`) fired the cap in all five A runs at
  **29.97 s [29.94-30.00]** against B's full **36.00 s**, and ended A at **8/32** against B's
  **512/128**. The last six seconds: mean **8.43 [8.00-9.14] against 14.68 [10.71-14.91]
  ms**, p99 **45.00 [42.01-51.45] against 58.81 [41.97-64.94]**, slow **14 [13-24] against
  32 [19-35]**; over the whole flight, mean **12.57 [12.35-12.71] against 14.02
  [11.89-14.08] ms** and p99 **51.56 [49.15-54.25] against 58.81 [45.99-61.05]**. The end
  queue is **10,328 [10,314-10,652]** against 0, with **1,821 [1,793-2,117]** in view. Do not
  read that as the cap doing better on fewer cores: A also counted **22,672 [22,029-22,986]**
  generations against the six-core run's 36,118, so it had less mesh work to leave behind.
  The cap's timing and budget transition are the same; only the workload and frame cost move.
- **The 30 s cap is still two-state; a middle budget is an untested idea, not a shipped one.**
  The cap selects the loading 512/128/1000 ms budgets, then the gameplay 8/32/0.35 ms budgets.
  The six- and four-core latch runs above price that trade: late p99 **26 vs 48 ms** at 6 cores
  and **45 vs 59 ms** at 4 cores, against **14.3k / 10.3k** chunks left queued and **2.8k /
  1.8k** of them in view. A middle or millisecond-based budget would need its own 36 s-flight
  A/B; the backlog-proportional count was tried and reverted (its note is above), and the cap
  windows are already env-overridable (`FARLANDS_LOADING_LATCH_MAX_SEC`,
  `..._QUIET_SEC`) for an arm. No arm has been run, so the two-state cap stands; a user-facing
  settings entry would be a separate change.
- **A 27-ray occlusion pass finds about 88% of the potential far holes in sight.** Output
  `.freebuff/start_fill_ab_20261109_105458`, the same five pairs. The pass samples a 3x3x3
  lattice in each potential chunk's AABB, walks the drawn full opaque blocks along the
  eye-to-sample ray and stops at the chunk entry, so a chunk is visible if any sample has an
  unoccluded sightline. The potential set is the far-tier in-frustum entries that are neither
  drawn nor skipped as empty; medians **1,844 [1,673-2,080]**, of which **1,594
  [1,489-1,869] are visible** and **229 [184-354] occluded**. The medians do not add (1,594
  + 229 = 1,823 against 1,844); the per-run visible share is **87.6% [81.6-89.9%]**. The A
  arm's frame and backlog numbers are the same shape as the preceding run: late mean **8.64
  [8.03-9.08] against 14.72 [13.88-16.14] ms**, late p99 **28.41 [26.56-34.12] against
  47.49 [41.29-55.18]**, late slow **3 [1-8] against 16 [15-18]**, end backlog **14,404
  [14,307-14,624] against 0**, and the whole-flight mean **12.35 [12.26-12.76] against 13.82
  [12.87-14.46] ms**. The roughly 12% that are occluded are the part of the backlog a closer
  drawn hill already covers; the other 88% have at least one unoccluded corner. This is a
  chunk-level sightline estimate, not a depth buffer or a pixel count: one clear corner
  marks a whole chunk visible, a surface between the 27 samples can be missed, transparent
  and non-full blocks do not occlude, and no-chunk and hidden entries are included. Its
  total is larger than the 1,740 from the preceding run because it is a different session
  and because it also drops buried entries among the hidden set, not just among
  no-geometry. The screen-area pass below adds the pixel estimate.
- **The screen-area pass says almost every one of those holes is on screen, but their union
  has a median of 7.3% of the viewport.** Output `.freebuff/start_fill_ab_20261109_114513`, the same five pairs. The
  pass projects each 27-ray-visible hole's AABB, samples an 8x8 grid over its clipped screen
  rectangle, unprojects each sample, and tests the ray against the drawn full opaque blocks.
  A hit fills the 4x4-pixel screen cells under its sample cell, and the cells are unioned, so
  overlapping holes cannot add up. **481 [450-981] holes were visible to the 27-ray pass; 481
  [450-980] of them hit at least one screen cell** - 0 to 2 holes lost per run, **99.6-100%**
  of the visible set. The union covers **54,720 [52,992-86,288] pixels**, **7.33%
  [7.10-11.56%]** of the viewport. The count is the answer to "how many cover pixels": the
  screen test is not where the holes disappear. The area is the useful new measure: the far
  holes that do project are a small, scattered part of the frame. This is a 4x4-cell estimate;
  a cell counts only if its centre ray reaches the AABB before a drawn opaque block, the AABB
  is the potential volume rather than the built surface, and no-chunk and hidden entries are
  included. It is not a depth buffer or a per-pixel count. The 481 visible set is this
  session's, not the 1,594 from the preceding run (a different spawn); the screen test's 0-2
  loss says that, applied at the same rate, about 99.6% of the 1,594 would cover pixels.
  The A arm still costs less than B: late mean **7.29 [7.19-7.56] against 12.69
  [11.74-14.04] ms**, late p99 **23.08 [20.73-24.38] against 44.90 [39.52-56.07]**, late slow
  **1 [0-2] against 17 [10-19]**, end backlog **13,224 [12,968-13,600] against 0**.
- **The rate jump a player sees at 4-5.9 s is the load/tail switch.** The probe's boundary (load
  4.56 s, tail 1.25 s) is where chunks start popping faster: at the fill settle the budgets move
  to the loading 512/128/1000 ms and the backlog the load-phase caps held back - installed but
  unmeshed chunks and the far regions - drains in the last second (18,060 builds before the
  boundary, 983 after). This is the latch described in the fill-budget entry, not spare capacity:
  the arm that applies the loading budgets from the first frame is measured there as worse,
  because the mesh work competes with install and light on the main thread. The first phase
  cannot be doubled by moving the tail's burst into it; it can only be shortened by doing less
  work. **The mild arm was measured and is neutral-to-worse.** 16 rebuilds / 64 uploads with the
  clocks untouched absorbed the whole tail (512 tail builds -> 0, tail accounting 271 ms -> 6 ms
  over 0 frames) but delayed the load (5.74 -> 6.03 s) by the same amount, so the wall stayed flat
  (6.01 vs 6.04 s): the burst is redistributed, not removed. It is reverted. **The all-air
  dirty-stage fast path is neutral for the same reason**: cutting the drain's dirty stages from
  45.0 to 13.3 a frame (the band's pad empties were most of them) left the load (5.79 vs 5.78 s)
  and the wall (6.05 vs 6.06 s) where they were, so the stage is cheap next to the install itself.
  It is reverted too.
- **The per-face sky gate is a fill win on constrained hardware.** The sky install
  pass used to walk all six 32x32 border planes of every arriving chunk. A face
  whose neighbour has no sky shade cannot be improved -- no shade means every
  non-opaque, non-liquid cell is already 15, and the walk only raises -- so the
  face is now skipped before its plane is walked. On 6 logical cores / 5 workers,
  three paired RD32 fills went end **10.63 [10.62-10.66] -> 10.38 [10.34-10.42] s**
  (B faster 3/3), tail **0.25 -> 0.00 s**, settle **9.60 -> 9.44 s**, process total
  **2.71 -> 2.64 s**, and load frame **4.50 -> 4.13 ms**. On 16 threads the eight
  paired fills moved the median end **5.02 -> 4.98 s** and settle **4.09 -> 3.97 s**
  (settle faster 8/8, end 6/8), with process total **1.37 -> 1.25 s** in the first
  three pairs and **1.39 -> 1.30 s** in the next five; two candidate runs were
  0.03-0.04 s slower, so this is a small unconstrained gain and a clear
  constrained one. The light profile confirms the mechanism: sky pass **1.07 ->
  0.605 s**, pass hold **1.59 -> 1.14 s**. Binary A/B
  (`probes/start_fill_ab.py --a-dll/--b-dll`) kept the baseline DLL from HEAD and
  the candidate DLL from the gate; the comparisons are in
  `.freebuff/sky_gate_ab_1`, `_2` and `_6c`, with the profiles in
  `.freebuff/fill_profile_now.log` and `.freebuff/sky_gate_profile_candidate.log`.

- **The admission controller was over-pricing a chunk on small pools, and the ramp pays.**
  The controller's per-frame generation allowance is `workers x frame / cost`, with the cost
  model a flat 2.46 ms measured at 15 workers, "where contention makes a chunk dearer" - its
  own comment recorded ~2.31 ms at 3. A five-worker fill asked the question directly, three
  alternating pairs through `FARLANDS_GEN_WORK_PER_CHUNK_MS`: at 1.8 ms the settle fell
  **6.695 -> 6.299 s**, loaded **7.48 -> 7.19 s**, arrivals rose **4.3 -> 4.9 a frame**, and
  the win repeated 3/3. `generation_work_per_chunk_ms_for()` now ramps the price from 1.8 ms
  at 5 workers to 2.46 ms at 15, so a large pool keeps the measured constant exactly and only
  small pools are re-priced. Binary A/B against the shipped sky-gate DLL, five pairs at 6
  logical cores / 5 workers: settle **9.391 -> 9.284 s** (faster 5/5), loaded **10.26 ->
  10.12 s** (4/5), chunk load **9.51 -> 9.37 s**, end **10.36 -> 10.18 s** (3/5, one pair
  0.12 s the other way). This is what the two install-side trims below were looking for: the
  fill is admission-bound, but by the controller's price rather than by the drain. Outputs:
  `.freebuff/cm_a*`/`cm_b*`, `.freebuff/gen_cost_ramp_ab_6c`, candidate DLL
  `.freebuff/gen_cost_ramp.dll`.

- **The in-flight generation cap's floor was too shallow for a small pool.** The general rule
  is 6 per worker, but the 64 floor is what a 5-worker pool actually gets, and it was starving
  the queue: sweeping `FARLANDS_GEN_MAX_INFLIGHT` there, 30 in flight (6 per worker) ran settle
  6.47/6.38 s against 5.56/5.53 at 64, 90 ran 5.24/5.19, and 128 ran 5.14/5.17 with loaded
  5.97/5.99 against 6.46/6.40 - 2/2 faster at every step. The same sweep at 11 workers, where
  the per-worker term gives 66, ran 128 at 3.74/3.58 s against 4.68/4.07 (loaded 4.69/4.45
  against 5.74/4.92, 2/2). At 15 workers, where 6 per worker already gives 90, 128 was a wash
  (settle 3.63/3.84 against 3.71/3.72, loaded 0.02-0.23 s worse), so the floor is 128 for
  pools of 12 or fewer and the per-worker rule decides above that. A small pool admits fewer chunks a frame and needs the deeper queue to keep its
  workers fed. Deeper than 128 at such a pool is unmeasured.
- **The in-flight cap's knee at 11 workers is 128.** 192 was tested there on top of the 128
  floor: settle 3.83/3.89 s against 3.72/3.68 and loaded 4.69/4.71 against 4.70/4.66, worse in
  2/2, so more depth only lengthens the completed queue. Left at 128.
- **The mesh hold at 11 workers is a wash.** The floor is 12 because 8 and 10 were washes and 12
  won; 11 is the stock 6c/12t default, so it was asked directly. Forcing the hold (partial 2)
  gave settle 3.72/3.65/3.76 s against 3.87/3.70/3.66 - faster 2/3, mean 3.710 against 3.744, a
  wash - so the floor stays 12 and the default machine keeps meshing during the fill.
- **The completion clock over the fill does not move it either.** The arithmetic looked like a
  pacer: 20,230 installs at ~22 us inside a 0.75 ms a frame clock is ~590 frames, and the fill
  runs ~590 frames. A loading-only clock (`FARLANDS_COMPLETION_BUDGET_LOADING_MS`, since deleted),
  swept at 2.0 and 3.0 ms with the mesh hold already off, was flat in alternating runs: 4.47/4.53 s
  and 4.67/4.40 s against 4.53/4.45 s at 0.75. The install count cap is min(active_max+16,
  RD+16) = 48 at RD 32, and the light and dirty stages share the same clock, so widening it only
  moves the same fan-out into the same frames. Reverted; the clock stays at its gameplay value.
- **The initial fill's equilibrium, as measured.** On the 16-thread machine at 11 workers and RD
  32, 20,230 chunks settle in 4.3-4.5 s: ~580-660 frames at ~35 chunks a frame (the ring walk
  ~19, the chain ~14), on a 6.7-8.3 ms frame of which the whole updater is ~2.2 ms, with the
  pool ~4 of 11 workers busy and the main thread ~27%. The drain is 0.51 s of that (12%):
  `process_completed_chunks` runs 0.87 ms against its 0.75 ms clock at 25 us an install, and the
  install split is insert 5.3 us, listener 0.6, prepare 1.0, data 1.6, placements 0.6, with
  stage_results 7.3, stage_dirty 5.1 and stage_light 4.7 us of interleaved fan-out. Every knob
  on that loop has been measured and none moves the wall: the admission price (1.1 and 0.8 made
  it worse), the in-flight cap (192 and 256 worse), the install clock during loading (1.5/3.0 ms
  no gain, and the queue absorbs it), a cheaper light slice (-62% slice, +0.7 s fill), the
  shared macro-column cache (no hit rate, then a serialisation loss), and the band frontier
  (3,209 reads cost 67 ms total, so its 2 ms budget is never spent). The fill is a feedback
  equilibrium, not a hot spot; breaking it needs the install/mesh work moved off the frame
  cadence, not another constant.
- **The admission rate is at its knee in both directions.** The one knob the earlier arms
  never moved at 11 workers was the cost model itself, since it is interpolated rather than
  measured there. `FARLANDS_GEN_WORK_PER_CHUNK_MS=1.3` (a rate of ~57 a frame against the
  interpolated 1.92's ~39) on an ABBA alternation ran settle 4.780/5.099 s against
  4.518/4.415 s and loaded 5.56/6.05 s against 5.52/5.40 s: both pairs worse, with admission
  stops up (331/371 against 203/240), the generation-cap stops down (200/205 against
  254/256) and the frame count unmoved at 590-653. So the extra chunks were admitted and did
  not become a shorter fill - the pool is ~4 of 11 busy and the set merely waits longer. With
  the 192/256 caps and the 1.1/0.8 prices already measured worse, the controller sits on its
  knee and admission is not the fill's wall.
- **The chain drain had no clock, and that was the fill's 24 ms frame.** The generation
  phase's own timer read 0.64 ms average and 24-30 ms maximum, all of it in
  `drain_chain_queue`, whose budget counts *generations*: `offered=31,750 gens=8,480
  skipped=23,090 refused=180` on a 4.1 s load. Since a skip costs the same offer machinery
  (a band lookup, a resident `contains`, a scheduler probe) and never advances the count,
  a stale queue drained unbounded inside one frame. It now stops on a 1.5 ms wall clock
  (`generation_chain_budget_ms`), always making the first offer like the band service, and
  the queue is refilled by installs so a cut-off offer returns. Four alternating pairs at 11
  workers: generation total 421.5/471.3/432.9/422.1 ms off against 307.0/275.8/296.8/306.2 ms
  on (mean -32%, ranges do not overlap), worst generation frame 29.65/21.77/21.67/25.87 ms
  against 15.16/7.24/10.35/12.59 ms, and settle 4.079/4.133/4.557/4.012 s against
  4.040/3.966/4.020/4.130 s - paired -0.156 s on average, the clock faster in 3/4, which is
  smaller than this host's noise. The CPU and the spike are proven; the wall time moves at
  the  edge of what one session can resolve. `FARLANDS_CHAIN_BUDGET_MS=0` restores the
  unbounded arm.
- **The chunk drain's clock is not the fill's pacer either.** Its per-frame average sits on
  the 0.75 ms clock (0.76 ms over 638 frames) with a completed queue ~290 deep behind it,
  which reads as a clock-bound loop. Doubling the clock to 2.0 ms in a three-pair DLL A/B
  changed nothing: chunk load 8.36 [8.35-8.43] s against 8.39 [8.35-8.42] s, settle paired
  +0.030 s (B faster 3/3, at the noise floor), `process_total` 2.35 -> 2.40 s and the load
  frame 5.85 -> 5.45 ms. The loop empties what the workers have finished in a frame; the
  average landing on the clock is a coincidence of an arrival-limited loop. The absolute
  times in that arm are the six-core A/B machine's, not the 16-thread probe's. Pairing it
  with the cheap-price arm does not rescue the price either: at 1.3 ms, drain 2.0 gave
  settle 4.252/4.029 s against 4.240/4.678 s for the 0.75 ms clock (1/2 pairs), and both
  sit worse than the 1.92 ms price's 4.0-4.1 s. The two halves were each measured alone and
  are worse together, so the drain really is arrival-limited and the price really is at its
  knee.
- **The fill is worker-CPU-bound, and what a chunk's 1.46 ms is made of.** With the path
  profile armed (`FARLANDS_GEN_PROFILE=1`) the fill reports 20,230 chunks at work p50
  1.239 ms / avg 1.461 ms, against a drain wait of 68 ms in the completed queue - so the
  workers, not the drain, are the wall: 20,230 x 1.46 ms is 29.5 s of worker CPU, and the
  pool's 69% busy (15.6% idle-with-work + 15.0% idle-empty) is ~6.7 workers, which is the
  4.4 s. Per chunk the slices are light 0.50 ms, column pass 0.48, density 0.22, materials
  0.22, disk 0.03. The one piece of that which was provably free to remove is the
  continentalness read in the column pass: `biome_from_climate` ignores its `cont`
  argument and nothing read `ColumnSample::cont`, so it was a 12,000-block noise read per
  column, 1,024 a chunk, and the field is now gone. Two A/B pairs: settle +0.010 s median,
  loaded and end -0.03 s - neutral, as one noise read a column should be. No speedup is
  claimed for it; the remaining slices all change generated terrain if touched, which is a
  data decision rather than an optimisation.
- **More workers buy about 5%, which is the last word on where the pacer sits.** 11 against
  15 workers at RD 32 on the 16-thread machine, two shots each: settle 4.027/4.349 s against
  3.986/3.925 s. The extra four workers shorten the fill by ~0.23 s while making every frame
  *longer* (8.15/8.58 -> 9.05/9.15 ms) and every chunk dearer on the workers (0.86-0.90 ->
  1.00-1.01 ms), i.e. more concurrency against more contention, nearly cancelling. That is
  what a main-thread-paced fill looks like from the pool's side, and it is why the remaining
  lever is the frame (off-frame install) or the chunk's own work (a worldgen-data change),
  not another admission or pool constant. The 16-thread shipping default is already 15, so
  this is not a switch to flip - the probe forces 11 to keep arms comparable.
- **The fill time is the admission model's own arithmetic, and that is why frame-side
  work never moved it.** The controller admits `workers x last_frame_ms / price x 0.9` a
  frame, which is frame-proportional, so the rate in chunks a SECOND is `0.9 x workers /
  price` and the fill is `20,230 x price / (0.9 x workers)` - 3.92 s at 11 workers and
  3.69 s at 15 with the model's own 1.92/2.46 ms, against 4.03-4.44 s and 3.93-3.99 s
  measured. A shorter frame admits proportionally fewer chunks, so it cannot shorten the
  fill: `NO_RENDER` frames measured 5.5-6.1 ms against 8.2-8.6 ms and the fill was SLOWER
  (5.65 s), and `--disable-render-loop` at 7.53 ms frames settled in 3.77 s, inside the
  noise of the 4.0 s baseline. The leftover ~6 ms of an 8.2 ms frame is real - render_cpu
  is 0.26 ms and process_total 2.1-2.4 ms - but it is compensated for, not paid. It is not
  presentation either: the panel reports 165 Hz (6.06 ms), vsync-on measured 10.7 ms a
  frame and vsync-off 8.2, and headless (no swapchain at all) was the slowest arm at 8.1 s
  with the pool 69% idle. Only the worker's own 1.46 ms a chunk moves the wall, which is
  why every frame-side arm this session failed and why the price must be re-measured
  whenever a chunk gets cheaper.
- **An all-air chunk was paying 32,768 block decodes and 32,768 light writes for a value
  that is uniform by construction.** 7,620 of the fill's 20,230 chunks are empty, and their
  sky light is 15 in every cell. `propagate_sky_light` now takes that case in one
  `fill_light_uniform`, keeping the `chunk_above == nullptr` guard because a neighbour above
  can shade the top layer. Worker work 1.461 -> 1.192 ms a chunk, the light slice 0.50 ->
  0.28 ms, and two ABBA pairs at the default price 4.157/4.267 -> 3.980/3.739 s: -0.35 s of
  fill, the first wall movement this session. The first A/B lost 0.7 s because the fill used
  `pack_light(15,15,15,15)` and saturated the coloured channels as well, so every empty chunk
  glowed; `pack_light(15,0,0,0)` is the uniform. Re-priced afterwards at the cheaper chunk:
  1.92 gave 3.735/3.738 s, 1.5 gave 4.124/3.951 and 1.3 gave 4.278/4.512, so the
  interpolation stands - a lower price still over-admits, and the contention it buys is
  dearer than the work it hides.
- **A chunk of nothing but opaque blocks is 0 in every cell, so its scan is one fill.**
  7,660 of the fill's 20,230 chunks are all Solid+Opaque: the top block stops the sky and 0
  never recovers. `propagate_sky_light` now takes that case with `fill_light_uniform(0)`,
  behind `is_fully_solid` as a prefilter (a mixed chunk pays nothing) and
  `check_fully_solid()` as the authority, because an edit can leave the flag stale - on
  uniform sections that check is eight palette lookups, not a decode of 32,768 cells. The
  light slice falls 0.28 -> 0.17 ms and worker work 1.192 -> 1.096 ms. Four probe pairs:
  4.175/3.924/4.019/4.121 s before against 4.106/3.971/4.071/3.817 s after - 0.069 s mean,
  faster in 3/4, inside this host's noise, so the wall claim is "not slower"; the worker
  saving is the measured part. An earlier two-pair attempt at a noisier moment read the
  opposite way (3.734/4.420 against 5.081/4.556), which is why the four-pair run is the
  record.
- **The climate lattice warped every node twice, and sampled a value nobody reads.**
  `build_climate` called `sample_temperature_raw` and `sample_humidity_raw`, which each warp
  the same point with the same amplitudes - four noise reads apiece - and
  `build_amplification` still passed `sample_continentalness` into `biome_from_climate` for
  every interior and ring node, where the grid ignores it. `sample_climate_raw` now warps once
  and evaluates both fields from that point, and the continentalness reads are gone. The
  values are bit-identical by construction (same noise calls, same inputs) and the tests agree.
  Column slice 0.45 -> 0.31 ms and worker work 1.096 -> 0.942 ms. Wall, four pairs:
  3.571/3.458/3.802/3.629 s before against 3.658/3.681/3.800/3.613 s after - +0.07 s mean,
  1/4, inside the host's noise, so this one is recorded as a worker saving, not a fill win.
  Re-priced at 1.7 ms it is worse (3.707/3.745 s against the 3.688 s mean at 1.92), so the
  price stays where it is.
- **The admission price is flat between 1.7 and 1.92 at the cheaper chunk, and 2.2 is worse.**
  Six shots an arm at 11 workers after the column cut (0.94 ms a chunk): 1.92 gave 3.672 s
  mean / 3.660 median, 1.7 gave 3.696 / 3.630, and 2.2 gave 3.813 / 3.821. The first two are
  tied inside the host's noise - 1.7's mean is dragged by a single 4.005 s outlier -  and the dearer arm loses on both statistics, so neither direction is a lever: contention is not
  relieved by admitting less any more than it is by admitting more. The interpolation stays
  at 1.92.
- **A cheaper chunk at six workers does not buy admission, it fills the in-flight cap sooner.**
  The derived price there is 1.236 ms; three interleaved pairs against 0.9 gave settle
  5.675/6.062/5.853 s against 7.581/6.006/5.920 (mean 5.863 against 6.502, the default faster
  in 2 of 3), and the counters say why: the lower price raised the pool's idle-empty share
  (12.3/14.0/13.1% against 29.8/19.2/16.7%) and *lowered* generations a frame (19.2/18.8/18.7
  against 17.7/17.6/17.2). Admitting more chunks per frame does not reach the idle pool: the
  128-chunk in-flight floor binds first, and a frame that admits past it only lengthens itself.
  So the 16.6% idle at six workers is not price-limited capacity, which leaves the in-flight
  floor as the thing to move if that idle is to be spent. This block also ran a baseline of
  5.7-6.1 s for a build that measured 3.98-4.31 s earlier the same day, so only the pairing is
  meaningful; the 0.9 arm is rejected.
- **The in-flight floor does not spend that idle either, and its failure names what does.**
  At six workers the 128-chunk floor was tested against 192, on the price test's own reading that
  the floor binds first: 192 ran settle 6.912/6.824/5.709 s against 5.885/6.085/5.357 (3/3 for
  the floor, mean 6.482 against 5.776). The counters repeat the price arm's shape -- idle-empty
  rose to 16-22% from 9-15% and the drain to 127-160 ms from 105-133 -- but the deeper queue did
  reach further: in-flight maxima went 147/146/118 to 217/185/144. So the idle at six workers is
  neither price-limited nor depth-limited; a deeper queue hands the drain more per frame, the
  drain is the long per-chunk term (wait 11.6 + work 0.75 + drain 61.3 ms), and the extra
  arrivals sit behind it. Whatever spending that 16.6% requires, it is a change to the drain or
  the frame budget that walks it, not to admission.
- **The drain is latency, not work: 36 us an install against 61 ms of queueing.** With
  `FARLANDS_INSTALL_PROFILE=1` at six workers a real install costs poll 0.21 + prepare 1.04 +
  data 1.85 + placements 0.52 + insert 10.45 + listener 0.57 + tail 5.51 + stage_light 3.86 +
  stage_dirty 5.77 + stage_results 6.51 = **36.3 us** (an empty chunk 25.3), so all 20,230
  installs are **0.65 s** of main-thread time across the whole fill (the phase total says 0.64 s
  over 672 frames). Against that, the finished-to-installed latency averages 61 ms: **99.9% of
  it is waiting for its turn**, 28 installs a frame through a 0.75 ms processing budget that
  stopped 57 frames and a 128-install dynamic cap that stopped 186. The per-install split has
  no single hog -- `chunk_map.insert` plus the light-band index is the largest at 10.5 us,
  light results 6.5, neighbour dirtiness 5.8, the 6-shard neighbour fan-out 5.5, light 3.9 --
  so cutting the drain means cutting the queueing (a larger install budget or cap while the
  pool has idle capacity), not any one stage. Note the existing comment on
  `processing_budget_ms`: doubling it was neutral at 15 workers, and it has never been tried at
  six, where the queue latency is now the measured long pole.
- **The install clock is the lever that profile predicted, and it pays at six workers.**
  With a `FARLANDS_PROCESSING_BUDGET_MS` arm (added with the override block's move to
  `world_updater_budgets.cpp`, because `world_updater.cpp` was at the 500-line cap), three
  interleaved pairs at 3.0 ms gave settle 3.722/3.794/3.912 s against 4.127/3.815/4.119 at the
  shipping 0.75 (3/3, mean 3.809 against 4.020). Installs a frame rose 28 -> 32 and the phase
  itself stayed ~1 ms, which is the point: the queue shortens, the work does not; budget stops
  were unchanged (52-70) because the per-frame install cap then binds. The clock is shared with
  gameplay, where a 4x budget would cost frame time for no load, so the win ships as a
  **loading-only field** (`processing_budget_loading_ms`, 3.0) that the drain selects while
  `!fill_settled || is_initial_loading`; gameplay keeps 0.75. `FARLANDS_PROCESSING_BUDGET_LOADING_MS`
  overrides the loading value and `FARLANDS_PROCESSING_BUDGET_MS` the gameplay one, so the two
  arms stay independent. The first wiring selected on `is_initial_loading` alone and changed
  nothing -- a 0.75 arm ran 3.898/3.842 s against 4.046/3.957 -- because `LoadingLatch` arms
  only on `fill_settled && pending_mesh`: it is the post-fill mesh latch, so the fill itself ran
  at the gameplay clock. The arm's earlier 3/3 result came from the global env, which applied
  during the fill as well; `!fill_settled` is the state that reproduces it.
- **The loading clock's gain grows with the pool instead of peaking at six.** Three interleaved
  pairs at each count against the 0.75 gameplay value: 8 workers 3.244 s against 3.462
  (-0.22), 11 workers 3.224 against 3.734 (-0.51), 15 workers 3.434 against 3.836 (-0.40);
  six was -0.21 in the wiring check and -0.21 in its own three-pair arm. Every pair at every
  count went to 3.0 -- all 11 pairs across the four counts -- with the same signature each time: a dearer phase
  (0.93-1.16 ms against 0.63-0.87) over fewer load frames (560-620 against 620-690). Absolute
  levels are not comparable across those blocks, because the host drifted by up to 0.4 s
  between them, but the pairing is, and it says the clock is worth most where the most workers
  wait on the drain. The old "doubling the clock was neutral at 15" note was about the global
  2.0 arm, not this field, so the field's default stands.
- **The loading clock's knee is 6.0, and it is now the default.** A value sweep at 11 workers
  (two shots an arm) gave 1.5 -> 3.443/3.302 s, 3.0 -> 3.234/3.301 and 6.0 -> **3.145/3.138**,
  with 6.0 fastest in both blocks and fewer load frames (520/565 against 565/580). The ordering
  is monotone, and it is the opposite of the old "raise the clock and nothing moves" reading:
  with the install keyed on the fill, more clock keeps paying. The cross-check ran before the
  default moved: at six workers 6.0 ran 3.710/3.659 s against 3.706/3.725 (a tie, 1/2) and at
  fifteen 3.441/3.338 against 3.480/3.390 (2/2, -0.05 s), so 6.0 was faster in five of the six
  pairs across the three counts and never slower. That is the bar 3.0 itself was promoted on,
  so the default follows the pairing rather than the one-count sweep. Gameplay keeps 0.75.
- **The install cap is not the next stop; the clock was enough.** The same keying bug held the
  fill's install count: `chunk_completions_initial` was selected on `is_initial_loading`, so the
  fill ran on the gameplay count, and at RD 32 the `render_distance + 16` clamp (48) dominated
  either way. A fill-scoped override (`FARLANDS_INSTALL_CAP_LOADING`, keyed on
  `!fill_settled || is_initial_loading`) tested 96 against the derived 48 at six workers with
  the loading clock at 3.0: **3.702/3.688/3.793 s against 3.724/3.637/3.843** (2/3, mean 3.728
  against 3.735), frame counts and phase averages equal. A tie, so the cap stays derived --
  once the clock has room the drain is not cap-bound, and what remains is the admission pacing
  measured earlier.
- **With the clock at 6.0 the fill is priced, not installed or generated.** Two 11-worker
  profile shots (settle 3.090 s, wall 3.93 s, load 2.93 s, tail 1.00 s) put every other stage
  well clear of its ceiling: installs are 12,590 real at 31.5 us plus 7,640 empty at 25.3 us,
  i.e. 0.59 s of main thread over the whole load, 1.08 ms a frame on `process_completed_chunks`
  against the 6 ms clock; the pool is ~63% busy (25-26% idle while work was queued, 9-11%
  idle-empty) with 13.00 s of generation over 18,914 chunks and 5.95 s of meshing over 8,148
  against 32.2 worker-seconds; and the walk stops on its generation cap in 337 of 545 frames
  (admission 73, budget 58, band 0), with `dynamic_max_last` as low as 8. The cap that stops it
  is the feed-forward rate `workers x frame / price x 0.9`. Testing that directly did not hold
  up: at 11 workers price 1.2 opened 2/2 (2.833/3.085 s against 3.081/3.304), but its third and
  fourth shots ran 3.175 and 4.016, so the arm's mean 3.277 sits inside the default's 3.193 --
  drift, not a knee. Four shots an arm at 13 (the line's 2.188) tie: 1.7 at
  4.036/4.015/4.084/3.275 and 1.3 at 3.721/4.223/3.960/3.224 against 4.314/4.087/3.536/3.362,
  means 3.852/3.782/3.825. At 15 (the line's 2.46) lowering loses: 1.5 at 3.859/3.993 and 1.0 at
  4.289/3.906 against 3.652/4.179/3.419, with one 1.0 shot above every baseline shot. Spreads
  inside an arm reach 0.8-1.0 s, wider than any difference, and the 13-worker block's last three
  shots (3.224, 3.275, 3.362, one per arm) are all fast, so the ordering follows when a shot ran
  rather than what it ran. The price is therefore binding without being worth anything to lower:
  an extra admission a frame does not shorten the fill even with a third of the pool idle, which
  puts the main thread's per-frame work -- the walk, the chain and the installs -- where a
  shorter fill would have to buy. The line through 1.1 and 2.46 is unchanged.
- **The fill's throttle is three gates in series, and it only moves when they open together.**
  The single-knob arms are washes against 2.89-2.93 s of load: the price at 1.2 (2.89-2.91), an
  in-flight cap of 264 on top of it (2.89), and the chain budget at 3.0/5.0 ms (2.93, with its
  stops unmoved at 319-352 against 337 -- it stops because the work is gone). Neither is the frame: `NO_RENDER=1` reproduces the drawn run exactly (2.92 s
  over 550 frames at 7.13 ms a frame), `--headless` saves ~0.5 s, and `QUIET=1`, which silences
  20 scene scripts, takes a frame from 7.10 to 6.79 ms for no change in the fill. Open the three
  together and it moves: price 0.9, in-flight 264 and install cap 128 ran load 2.43/2.36 s and
  settle 2.46/2.42 over 400-410 frames, against 520-555 frames at 2.89-2.93 s. The counters show
  why: 28.9 generations a frame against 21-22, and the stop table leaving `gen_cap` (329 -> 138)
  once the in-flight cap stops refusing the walk and the install cap stops holding the
  completions. Any two of the three give about two thirds of it (2.72 for price+install-cap,
  2.66 for in-flight+install-cap), so no single constant is the answer: the price sets the rate,
  the in-flight cap bounds the set that rate fills, and the install cap -- derived as
  min(initial+16, RD+16), i.e. 48 at RD 32 -- bounds how fast the completions leave it. A change
  that ships has to move the two caps for the loading window and leave the price alone, because
  the price is a function of pool size and was measured wrong at 15 workers. Pushing the same
  three further does not pay: price 0.6 with in-flight 512 and install cap 256 ran 2.42/2.42 s of
  load, and price 0.7 with 384/192 ran 2.44/2.38, both flat against 0.9/264/128's 2.43/2.36 even
  though generations a frame rose to 30-31. Past ~50 admissions a frame the fill buys frame time
  instead of frames -- 50 installs are 1.6 ms of main thread on their own -- so ~2.4 s is the
  knee with every gate open at 11 workers, ~17% off the 2.89-2.93 s baseline. The gate chain also
  changes the pool-size answer, which is the more useful half: the same arm at 15 workers ran
  load 2.13/2.13 s and settle 2.23/2.23 over 335 frames, against 3.17 s of load for the 15-worker
  baseline and 2.30-4.18 s across its earlier shots, with idle-while-queued down from 33.6% to
  19.5-22.1% and 36.1 generations a frame against 20.6. The caps had been holding the larger pool
  back, so 15 workers with the gates open is the fastest fill measured: 2.13 s against 2.43/2.36
  at 11 with the same gates and 2.89-2.93 for the 11-worker baseline. Widening the same three at
  15 moves it again but noisily: price 0.6 with in-flight 512 and install cap 256 ran 2.11/2.18 s
  of load, and price 0.9 with the same caps 2.20/1.89, i.e. 270-325 frames at 37-45 generations a
  frame against 335 at 36, with the pool's idle-queued share down to 16-18%. The direction is
  consistent -- fewer frames, more work a frame -- but the 2.20-against-1.89 spread inside an arm
  is wider than the gain over 2.13, so the size of it needs more pairs before it is a number.
  The phase table at that arm names what is left: per frame the install drain is 2.81 ms of the
  5.42 ms `process_total` (251 frames at 80 installs each), the walk 0.88 ms
  (`chunk_load_unload`), mesh upload 0.91 and the dirty queue 0.49; worker work is 14.42 s of
  generation plus 6.57 s of meshing, 21.0 s against 28.35 worker-seconds, so the pool is busy
  ~74% and no longer the story. The install itself is: 0.71 s of the 1.89 s, and 7,640 of those
  installs are all-air chunks paying 25.3 us each, of which 3.2 us go to the light stage and 6.0
  to the dirty stage. Only the first of those is waste: 7,640 x 3.2 us is ~0.025 s, not the 0.1 s
  a first reading of the two stages suggested. The dirty stage's 6.0 us is a 7-key lock and six
  neighbour probes, and a neighbour with blocks DOES have faces to expose once an empty chunk is
  resident beside it, so that half is work. With the install total at 0.71 s against a 6 ms clock
  (2.81 ms a frame, so the clock is not yet the limit), the larger targets are the 12,590 real
  installs at 31.5 us and the ~2.4 ms a frame the updater does not account for at all. The mesh
  hold is right to stay at 2 rebuilds a frame while the fill runs: the dirty queue reaches
  5,000-7,000 there (read for its depth only -- the traced runs report 11-13 ms frames because the
  trace itself is expensive), and opening the hold is worse in every direction.
  `FARLANDS_HOLD_MESH_PARTIAL` at 16 gave settle 2.63 s and visible-90 2.89 s, at 64 gave 3.23 s
  and never reached visible-90, against 2.32/2.63 at the shipped 2. The fill and the visible
  world are decoupled on purpose, and the 1.00 s tail in every phase line is the probe's own
  one-second stability window rather than work. The two caps are now shipped: the render-distance
  clamp is skipped while the fill runs (`loading_drain`), so the install budget is
  `chunk_completions_initial` (128) rather than `min(active+16, RD+16)` = 48 at RD 32, and the
  in-flight set uses `max_generating_in_flight_loading_per_worker` (24) instead of the gameplay 6
  for as long as the fill has not settled -- 264 at 11 workers and 360 at 15. Three pairs at 11
  workers came out 3/3: settle 2.84/2.81/2.72 s against 3.22/3.17/3.09, load 2.66/2.61/2.68
  against 3.18/3.15/3.13, mean -0.37 s of settle and -0.50 s of load, with idle-while-queued down
  from 24-26% to 19-21%. The price is untouched, and it is now the next gate: at 15 workers the
  shipped line price (2.46) ran settle 2.577/2.563/2.567 s and load 2.46/2.45/2.62, while
  `FARLANDS_GEN_WORK_PER_CHUNK_MS=0.9` ran 2.220/2.116/2.151 and 2.14/1.93/2.18 -- 3/3 for the
  lower price, mean -0.41 s of settle and -0.43 s of load, with idle-while-queued 27.3/27.7/23.6%
  against 19.3/18.3/17.9%. That is the opposite of the closed-gate sweep at the same count, where
  a lower price lost: the caps were refusing what the price asked for, so the rate was never
  exercised. Under the shipped caps it is right at 11 too: three pairs gave settle
  2.550/2.538/2.610 s against 2.875/2.739/2.811 and load 2.37/2.42/2.38 against 2.88/2.71/2.73,
  3/3. The loading window now has its own price -- `generation_work_per_chunk_ms_loading = 0.9`,
  used until the fill settles while the gameplay line and the env override are untouched. Checked
  at the small pool the old sweep warned about: 5 workers ran 4.021/4.068 s against 4.054/4.053 at
  the line's 1.1, a tie, so the value costs nothing there. With it wired in, the defaults run
  settle 2.414/2.470 s at 11 workers and 2.117/2.176 at 15 (load 2.43/2.47 and 1.92/2.09), against
  the 3.16/3.15 s settle and 3.18/3.15 s load the same binary's old caps gave.
  A mid-load window dump (one `get_slow_frame_report` taken while the fill runs, added to the
  probe because the per-0.1 s trace costs more than it measures) answers where the rest goes: the
  frames there average 12.3/11.7 ms, the updater 5.29/4.92 ms and 7.05/6.76 ms outside it, of which
  only 1.0-1.1 ms is `render_cpu` and 1.6-2.3 ms `render_gpu`. The window's worst frames are
  142/150 ms with `outside_process` 136/144 ms and no render cost at all, so what is left is
  engine-side work and stalls -- not the chunk pipeline, the renderer or the scene scripts. The
  fill runs at ~80 fps while it loads because of it, and that is the next thing to chase. The
  renderer is not it: `NO_RENDER=1` leaves the window unchanged (outside_updater 6.65/6.81 ms
  against 6.84, `render_cpu` 0.56-0.65 against 0.84) and the fill time identical (2.17/2.14 s
  against 2.16). With the updater, the renderer, the GPU and the scene's scripts (QUIET was a wash)
  all excluded, what remains is engine-side work with 140-150 ms stalls that carry no render cost --
  physics and allocation churn from the installs are the two candidates left, and the next probe
  is a changed physics rate rather than another budget. Physics is not it either: the probe gained
  a `PHYSICS_TPS` switch, and 10 and 240 ticks a second left the window's outside cost at
  6.50/6.21 ms against 6.63 and the fill at 2.08/2.15 s against 2.19. With the updater, the
  renderer, the GPU, the scene's scripts and the physics step all excluded, the remaining ~6.5 ms
  a frame plus the 140-150 ms stalls is engine main-loop and allocator behaviour, which is a job
  for a profiler rather than another engine switch. That window was the startup one, though: taken
  later (`MIDLOAD_SECS=1.6`) the fill's frames are 8.13/8.40 ms with a 39.6/21.0 ms max,
  outside_updater 5.26/5.20 ms and render 0.24/0.34 ms, so the 140-150 ms stalls belong to the
  first frames rather than to the steady fill. Physics re-tested there -- 4.89 ms at 10 ticks a
  second, 5.44 at 240, 5.26/5.20 at the default, fill 2.09/2.18 against 2.14/2.16 -- stays
  excluded. The steady fill runs at ~120 fps with ~5.2 ms a frame outside the updater and under
  0.6 ms of it renderer, so engine main-loop work is still the remainder, and it is what a profiler
  should be pointed at. The pool size is at its knee too: with the shipped defaults, 15 workers
  ran settle 2.196 s and load 2.11, 19 ran 2.095/1.94 and 23 ran 2.163/2.08, with idle-while-queued
  rising 20.8 -> 25.1 -> 29.1% as the pool grows. The default pool is
  `hardware_concurrency - 1`, so a 16-core machine already runs the 15 this arm measures; 19 buys
  ~0.1 s inside single-shot noise and 23 buys nothing. Time-to-visible is the player-facing number
  and it trails the chunk load by ~0.4 s: at 15 workers the shipped hold (partial 2) reaches
  visible-90 at 2.666 s against settle 2.269 and load 2.18; partial 4 reaches 2.621 and partial 8
  2.599, both inside noise, and the full-overlap arm (`HOLD_MESH_UNTIL_LOADED=0`) never reaches
  visible-90 at all. So the visible fill is the released hold's mesh drain -- settle plus ~0.4 s --
  and the partial budget is at its knee: 16 and 64 were worse, 4 and 8 change nothing. Cutting the
  0.4 s further means releasing the hold on the near field finishing rather than on the whole chunk
  set, which is a policy change and not a budget. That policy is not worth writing, though: a proxy
  for it already exists in `HOLD_MESH_MAX_SEC`, which frees the drain by wall clock, and 1.0 and 0.5 s
  gave visible-90 2.583/2.634 and 2.615 s against 2.644 at the shipped 2.0 -- all inside 0.06 s,
  with settle unchanged within noise and load 2.09-2.14 against 2.16. The visible fill is bounded by
  the drain's own throughput after release, not by when the release happens, so an earlier release
  would buy nothing; the next lever on it is the upload/rebuild budget in the post-settle window,
  not the hold's trigger. Ordering is already the one this analysis would ask for:
  `sweep::slice_cy` offers a column's band from the player's own slice outward, alternating below
  and above, so the surface beside the player generates before the rock under it -- the
  near-player full-column bands reach the world floor, and plain ascending order would have spent
  them on hundreds of blocks of stone first. There is no surface-first reordering left to try.
  The underground full-column fill is not a lever either, and it is much smaller than its share of
  the install counters suggests: `FARLANDS_UNDERGROUND_FILL_RADIUS` at 4 and 0 (the field is
  `FrameBudgets::underground_fill_radius`, default 8) cuts installs only 20,230 -> 19,046 ->
  18,638, because 3,214 of the below-land installs belong to columns outside the fill radius --
  the walk's own band reaches them. Settle moved 2.237 -> 2.178/2.172/2.107 s and load 2.09 ->
  2.15/2.15/1.92, inside noise for a change that would open void under the player, so the radius
  stays 8. The generation path profile (`FARLANDS_GEN_PROFILE=1`) puts what is left in one place:
  20,230 chunks at a mean 1.09 ms of work each (p50 0.772, p90 2.177, p99 4.94) is ~22 s of worker
  time, a 1.5 s floor at 15 workers, and the fill runs 2.1-2.2 s -- about 0.6 s of scheduling above
  that floor, which no budget owns any more. The distribution is the interesting half: the mean is
  1.09 ms while the median is 0.77, so the slow 1% (200 chunks at ~5 ms) carries roughly 0.8 s of
  the pool's work, and the admission wait (avg 21 ms, p90 42) is the queue's normal depth at 360 in
  flight. The next fill gain, if any, is in the tail of a chunk's own generation -- vegetation and
  the denser columns -- not in admission or the drain. The column pass looks like the next block
  to remove -- `columns` is 0.30 ms of a 1.09 ms body and is a function of (chunk_x, chunk_z)
  alone, so every vertical slice of a column recomputes it -- but a shared two-entry cache of it
  (keyed on the column, invalidated by every config setter, `FARLANDS_COLUMN_CACHE=0` to disable)
  measured as a wash: 2.111/2.194/2.207 s with it against 2.117/2.146/2.182 without, no pair
  separating. The dispatch is why: the sweep queues a column's six slices together and the
  scheduler hands them to six different workers at once, so they all miss before any of them has
  stored an answer. Reusing the pass needs those slices generated as one task -- a scheduler
  change, not a cache in front of the generator -- and that is what shipped: the sweep hands its
  accepted slice range to `ChunkWorld::generate_chunk`, the scheduler claims every key in the band
  before the task starts (so the walk's later offers for those slices are refused rather than
  duplicated) and one worker builds them in order. Paired at 15 workers, settle 1.858/1.918/1.932 s
  with the band against 2.114/2.140/2.127 without, 3/3 with every pair separating, installs
  identical at 20,230; at 11 workers 2.084 against 2.424. It is on by default
  (`FARLANDS_GEN_BAND=0` is the opt-out) and it is the first change since the loading caps to put
  settle under two seconds: the column pass is paid once per column instead of once per slice, which
  is 5 of every 6 passes on a band that spans six slices.

  Batching made the worker work ~5 s lighter and its unit of work coarser, so the two knobs tuned
  against the old balance were re-swept the same day. Neither moved: 19 workers settled 1.919 s
  against 1.881 at 15 (the pool knee is a property of the machine, not of the work's granularity),
  and the install cap's 192 arm gave 1.830/1.835/1.914/1.784 against 1.881/1.937/1.821 at 128 -- a
  0.046 s mean difference with the pairs split 1-1, which is noise, so 128 stays. What the re-sweep
  does establish is that the fill is scheduling-flat: with the column pass shared, ~1.9 s at 15
  workers is within a seventh of the 1.5 s worker floor the generation profile measures, and the
  remaining gap is not admission (cap), not pool size, and not the drain.

  The mesher's one clean cut was measured and dropped. In a fully solid chunk every interior column has
  four solid side neighbours at every y, so the vertical greedy pass can never emit a face there -- only
  the perimeter (124 of 1,024 columns) faces outward -- and the underground fill is 1,592 such chunks.
  Skipping those columns (`FARLANDS_MESH_SOLID_SKIP=0` as the arm) gave settle 1.851/1.914/1.797 s against
  1.866/1.827/1.949, means 1.854 against 1.881 with the pairs split 2-1: inside noise, exactly as its
  ~0.4 s of pool time (under 0.05 s of fill) predicted. A change inside the hottest mesher path is not
  worth a result that cannot be told from the machine, so it was reverted rather than kept on faith.

  The band's own span was swept the same way, and it confirms the mechanism rather than finding an
  optimum: `FARLANDS_GEN_BAND_SPAN=2` -- two slices per task, so three passes per six-slice band --
  settled 2.126/2.153 s against 1.880 for the whole band, while span 3 and 4 gave 1.938 and 1.882.
  The gain tracks how many column passes are shared, not how the work is scheduled, and most bands
  are four slices or fewer, which is why span 4 is indistinguishable from the whole band. The whole
  band stays the default; the switch stays as the arm that shows the pass, not the task size, is
  what moved.

  Deferring the far half of the meshing was the last candidate with a large number behind it --
  6.6 s of pool time, most of it for chunks behind the camera -- and it measured as a null too.
  With a mesh request beyond N chunks held back (dirty flag kept, the whole deferred set flushed in
  one pass at the settle edge, so nothing is left unqueued for a later turn) settle was
  2.078/1.930 s at N=16 and 2.024/1.921 at N=24 against 2.023 off, with visible-90 marginally
  better (2.345-2.398 against 2.462) and equally inside noise. The pool was already absorbing the
  mesh work in its idle time, which is the useful finding: **the fill is limited by generation and
  install, not by what else the workers were doing**, so no amount of moved-off work shortens it.
- **The material pass and the artifact scan stop above the density margin.** No cell can be
  solid above `max_height + shape_margin` (outside the band the density is just `delta`, and
  the margin bounds the band), and no cell can be water above the column's `water_top`, so
  both the 32-row material loop and the 27,000-cell isolated-voxel scan can break there --
  the dense buffer is already air. Worker work 0.942 -> 0.920 ms an chunk (density 0.21 ->
  0.20, materials 0.20), wall 3.648 s old against 3.626 s new over two pairs: a worker saving
  like the column cut, not a visible one, because the fill is admission-bound. The first form
  stored a per-column band array (same semantics, 0.907 ms) and pushed
  `src/worldgen/chunk_generator.cpp` to 513 lines; the shipped form recomputes the bound once
  from the chunk-wide `max_height + shape_margin` and landed the file at 499 lines -- one under
  the cap.
- **The cheaper chunk pays off at low worker counts, where there is less contention to hide
  it.** The same A/B (the 0.84 ms pre-cut build against the 0.92 ms current one) run at **six
  workers**: 4.163/4.148/4.071 s against 3.983/3.923/3.854 s, a mean of 4.127 against 3.920 --
  **207 ms, three pairs of three**, where the same change at eleven workers was a 22 ms tie.
  Per-chunk work also reads lower at six (0.84 -> 0.73) because fewer workers contend for the
  same caches, so the slice cuts show through instead of being spread over a saturated pool.
  The fill is still admission-bound, but the admission rate scales with worker count, which is
  why the worker saving converts to wall clock only when workers are scarce.
- **`chunk_generator.cpp` was split after the bound work, at 499 lines.** The three
  post-density repairs -- the isolated-voxel scan, the thin-solid-sheet repair and the water
  gap flood fill -- moved verbatim into `chunk_generator_cleanup.cpp` behind one call, taking
  the entry file to 429 lines and leaving the new TU at 105. They read only the density
  buffer, the dense buffer and the columns, so the split needed no context struct or lambda
  surgery; `cleanup_generated_columns()` takes the buffers, the world-Y origin, the solid
  ceiling and the public `ChunkGenerator::ChunkColumn` array. `scons docscheck` caught the new
  file the only way it can -- the file counts in `AGENTS.md` and `ARCHITECTURE.md` now read
  135.
- **`chunk_generator.hpp` was at the cap too, and a class-scope `using` is not a safe way out.**
  It went from 500 to 470 lines: `density_math.hpp` now holds `clamp01`, `smoothstep`, `lerp`,
  `lattice_base`, `trilinear_interp` and `density_from_shape` as inline free functions, and
  `chunk_solidity.hpp` holds the two free `chunk_would_be_fully_solid*` predicates (it only
  forward-declares the generator, so the include is not circular). The class keeps thin inline
  forwarders instead of `using density_math::...`: the first attempt used using-declarations and
  MSVC failed the build with, in order, `C3615 constexpr function 'lattice_base' cannot result
  in a constant expression` and `C3861 'trilinear_interp': identifier not found` -- a later
  same-named member declaration hides the WHOLE using-declared overload set, and once that
  happens the names are gone for every member body that follows. The forwarders cost nothing
  (all inline) and are what the lattice TU's explicit `ChunkGenerator::lerp` still calls. The
  .cpp count is unchanged at 135; sizecheck now reports 436 C++ files.
- **The per-column band was re-tried once the file had headroom, and lost to noise.** Keeping
  `band_outer` per column (4 KB of stack, ~14 lines) lets each column stop at its own surface
  instead of the chunk's tallest one, and a single profile shot suggested 0.907 ms against
  0.920. Three interleaved pairs say otherwise: work 0.915 ms against 0.924 and settle 3.693 s
  against 3.709 s, with two of the three pairs favouring the chunk-wide form and the per-column
  mean carried by one 3.525 s outlier. A 1% work difference is inside this host's noise, so the
  array and its bookkeeping were reverted; the chunk-wide `max_height + shape_margin` stands.
- **At six workers the fill stops being CPU-paced, and the profile says so in three numbers.**
  One run: generation window 3,961 ms, pool capacity 23,766 ms (six workers), worker CPU
  20,230 x 0.747 = 15,112 ms -- **63.6% of the wall and 76.3% of busy** -- with 3,948 ms idle
  (**16.6%**: 9.5% idle-with-work and 7.1% idle-empty) and 4,706 ms of other busy time. The
  per-chunk lifecycle is wait 11.6 + work 0.75 + drain 61.3 ms: drain is the long pole, but it
  is main-thread and budgeted, so it overlaps the pool rather than blocking it. A perfectly
  packed pool would therefore take 2.52 s, and the missing 1.4 s is admission pacing (739
  frames x 27.4 admitted chunks, chain 10.4 a frame of 17 generated).
- **There is no one-second mesh tail after that settle; the probe's own window made one.**
  `settle t=3.98 s` is generation; `loaded=4.96 s` looked like a 1 s meshing stage, but
  `loaded_at` is set only after the load has been quiet for `LOADED_STABLE_SECS` (1 s), and
  `mesh_at`/`far_at` are polled only once `loaded_at` is set -- so both metrics trail the real
  end by construction and cannot precede it. The tail window after `loaded` records zero
  builds, zero completions and 5 ms of wall, which is what "nothing left to do" looks like;
  the real post-generation work is the 0.22 s between `settle` and `first chunk_load` (3.98 ->
  4.20 s, install plus mesh uploads), and the mesh count at the end is the fill's total, not a
  tail's. Use `first chunk_load` when the end of the load matters. The same committed build
  measured settle 4.31 s in the next run, so the host's +-0.3 s swing is wider than any
  single-digit lever left on this axis; the one lever still open at low worker counts is the
  chunks admitted per frame (a lower admission price while the pool has idle capacity --
  untested at six, and the opposite of what eleven wanted).
- **The fill's extra milliseconds a frame are not the renderer's or the scripts' either.** With the
  generator's rate pinned (previous note), the remaining ~4.5 ms of a 6.7 ms load frame had to be
  attributed. Disabling the render loop (`NO_RENDER=1`) made the fill SLOWER - settle 5.65 s and a
  10.47 ms load frame against 4.2-4.5 s and 7.9 ms - and silencing all 20 scripted world nodes
  (`QUIET=1`) also slowed it to 6.08 s / 10.29 ms, with the per-script cost itself negligible
  (viewmodel 29 us, inventory 17.6 us, crafting menu 17.4 us, shader overlay 10.9 us; the whole
  list is ~100 us a frame). So the frame is not paying for chunk-instance submission or GDScript:
  it is paying for the streaming work itself plus the engine's own loop, and removing a consumer
  only lets the time-based budgets stretch the frame further. Throughput sits at ~4,800 chunks/s
  (20,230 in ~4.2 s), which is the admission controller's own rate at the 1.92 ms price, and every
  attempt to raise it is absorbed by the in-flight cap closing the gate. No change shipped from
  this arm; the switches are env-only.
- **Feeding the generator harder does not shorten the fill: the walk's generations a frame are
  pinned at ~19-20 whatever the admission knobs say.** At 11 workers the sweep stops on its own
  per-frame generation cap (~40% of frames) or on admission (~35%) far more than on the 1.0 ms
  clock (~9%), which reads like the controller is the wall. It is not. Lowering the price from
  the interpolated 1.92 ms to 1.1 and 0.8 raised settle (alternating runs: 1.92 -> 4.52/4.47 s,
  1.1 -> 4.91/4.53 s, 0.8 -> 5.89/5.00 s) while the walk's generations a frame moved only
  17.9 -> 18.9 -> 20.1 and admission stops rose 222 -> 388/340 -> 481/446 as the in-flight set
  filled. Pairing the low price with a 256 in-flight cap (the two had only ever been tested
  apart) was worse still - 5.05/6.19 s and 5.59/5.88 s against a 4.25/5.03 s baseline, in-flight
  peaking 269-315 with generations a frame pinned at 19-20 again. The pool is ~4 of 11 workers
  busy in generation, the main thread ~27%, and a frame is 6.7-8.3 ms of which ~2.2 ms is the
  updater, so the hand-off rate is not what the fill waits on. Both arms reverted; env-only, so
  no source changed.
- **Sharing the macro-column pass between a column's slices does not pay, and the reason is
  the fill's shape, not the cache.** The macro pass (0.45 ms a chunk, the largest slice) is a
  function of (chunk_x, chunk_z) only, and a world column is ~6 vertical chunks, so the fill
  computes it six times - roughly 7 s of pool CPU. A one-entry per-worker cache changed nothing
  (0.45-0.47 ms, settle 3.73/3.93 s against the 3.70-3.79 baseline): the sweep hands a column's
  slices to different workers, so every worker sees a different column. A shared 16-shard cache
  - first arrival computes and publishes, the others wait on a condition variable and copy the
  72 KB result - measured 4.28/4.49 s with `columns` at 0.48-0.50 ms, but that window is not a
  comparison: the unchanged tree measured 4.24, 4.54 and 5.02 s minutes later, so the arm sits
  inside the same host noise and is UNPROVEN rather than rejected. It was reverted unshipped
  (the tree only carries what a pairing credits), and re-testing needs a quiet host. The reason
  to doubt it even then is the fill's shape: 26.9 s of pool work over a 3.7 s wall is 7.3 of 11
  workers busy, so the fill is not worker-CPU-bound, and turning six parallel computations into
  one plus five waits adds a dependency between chunks that used to generate independently.
- **Raising the install pass's clock during the fill does not shorten it.** The pass is paced
  by its 0.75 ms clock in steady state - about 33-35 installs a frame at 20-22 us each - while
  the pool could offer ~50, so a loading-only clock (`FARLANDS_PROCESSING_BUDGET_LOADING_MS`)
  looked like the ceiling. Swept at 11 workers, two pairs per point: 0.75 ms gave settle
  3.69/3.79 s, 1.5 gave 3.71/3.80, and 3.0 gave 3.93/3.71 - no gain, because the frames stretch
  to absorb the extra install time and the installs-per-frame barely move. Reverted.
- **The admission price's low end was re-priced after the chunk got cheaper.** The 1.8 ms
  five-worker price dates from a ~2.3 ms chunk; the density-band skip and the edit-key scan
  took ~0.26 ms off the worker body, so the controller was admitting at the old rate. Swept
  with `FARLANDS_GEN_WORK_PER_CHUNK_MS` on the 5-worker arm, three alternating pairs per point:
  1.5 ms gave 3/3 faster settle (-0.05..-0.10 s), 1.3 3/3 (-0.06..-0.23 s), **1.1 3/3
  (-0.13..-0.29 s, loaded -0.22 s)**, and 0.9 stopped paying (-0.27, +0.04, -0.41 s - 2/3
  with a regression). The ramp's low anchor is now **1.1 ms**; the 15-worker end stays at the
  measured 2.46. The straight line between them was then tested at 11 workers, where it
  interpolates 1.92 ms: two env pairs liked the 2.46 ceiling (settle 3.70/3.66 s against
  3.79/3.75, with 1.4 at 3.83/3.81 and 1.1 at 4.09/4.10), but five binary pairs came out
  **2/5** (mean 3.745 s against 3.713 s), so moving the knee to 11 was a wash and the line
  stays where it is.
- **A whole-chunk "nothing is shaped here" skip does not pay for its test.** With the weirdness
  mask zero across a chunk, density is exactly the macro delta at every cell, so the 729-sample
  3D shape lattice and every per-cell interpolation of it are pure reproduction. Testing the
  1,024 columns for that up front cost more than it saved: four pairs on the 6-core arm were
  slower on settle in **4/4** (8.33 -> 8.40 s, [-0.05, -0.01]) and on loaded in 4/4 (9.13 ->
  9.34 s), chunk load 8.37 -> 8.58 s. So a wholly unshaped 32x32 chunk is rare enough that the
  test is not repaid, while the per-cell band test below it is decided per cell and does fire.
  Reverted.
- **The edit scan kept the answer but threw the keys away.** As soon as a save holds any
  `.edit` file, every chunk without a resident edit map paid `FileAccess::file_exists` twice
  (the file and its `.bak`) to be told its own is missing: `GenSlice::EditDisk` was **0.13 ms**
  on 19,848 of 20,230 chunks in a fill, against 382 chunks that actually had edits. The
  directory scan already reads every name, so it now parses `chunk_<x>_<y>_<z>.edit[.bak]`
  into a key set once and the per-chunk question is a set lookup. An edit made before the
  first scan inserts its key and leaves the scan pending, so the later scan MERGES rather than
  pretending to have run; a world reset clears the set with the scan state. Four pairs on the
  6-core arm: settle **9.09 -> 8.35 s (4/4)**, loaded **9.88 -> 9.24 s (4/4)**, end **10.01 ->
  9.50 s (4/4)**, and process CPU over the fill 2.63 -> 2.42 s - the work is gone, not moved.
- **The density field's shape band is a skip, not a cost.** `density_from_shape` fades the 3D
  displacement to zero by the column's `band_outer`, so for every cell further than that from
  the macro surface the result is exactly `delta + shape * strength * 0` = the macro delta: the
  8-tap trilinear lattice read cannot change the value. The chunk loop paid for it anyway, and
  a chunk whose surface sits elsewhere in its column is nearly all such cells. Testing
  `|delta| >= band_outer` before the interpolation is bit-identical and skips it: four pairs on
  the 6-core arm gave settle **9.28 -> 9.07 s (4/4)**, loaded **10.07 -> 9.86 s (4/4)**, chunk
  load 9.32 -> 9.10 s, and whole-session end 10.08 -> 9.98 s (2/4, a wash inside its own
  spread). The slice it removes was 0.28 ms of a 2.3 ms per-chunk worker body at 5 workers.
- **Capping concurrent light passes buys fill time and loses the session.** Each pass takes an
  exclusive band of up to 27 shards and the light profile prices the wait at ~0.77 ms a pass
  (7.9 s of worker time over a fill), so the convoy looked like the lever: gating posts on a
  posts-minus-drained counter moved the 6-core arm **5/5 faster at a cap of 2** - settle
  9.25 -> 8.97 s, chunk load 9.36 -> 9.01 s, loaded 10.11 -> 9.76 s - and ended the session
  **5.3 s later** (10.13 -> 15.07 s, 2,352 -> 3,965 frames). At 2 in flight a 0.12 ms pass that
  waits 0.77 ms for its band tops out near 1,100 passes/s, under what the light queue needs, so
  the fill finished before its light did and the tail paid for it. A looser cap does not rescue
  it: at 6 the fill was slower than baseline (10.07 -> 10.36, 11.26, 10.24 s) with a 2.7-3.3 s
  tail, and at 8 the tail was still 2.8 s. Reverted - and the counter's failure mode (one leaked
  post stalls all lighting) is not worth a scheduling win even when there is one.
- **The 15-worker end of that price is a wash, so it stays at 2.46.** The same switch on the
  default pool looked like a win in two unpaired alternating passes (2.46 / 1.8 / 1.4: settle
  **4.005, 3.902 / 3.834, 3.767 / 4.022, 3.937 s**, loaded 4.98, 4.96 / 4.70, 4.74 / 4.97,
  4.77 s), but the paired binary A/B of flat 1.8 against the ramp over five pairs says the
  difference is drift: settle A-B median **-0.015 s [-0.086-+0.082]**, candidate faster 2/5,
  loaded -0.03 s, end -0.02 s. At 15 workers the fill's frames are mostly the engine's own
  drawing, so the controller has little room to move it. The ramp therefore re-prices only
  the small pools, where the paired result is 5/5, and 1.4 was not pursued past the unpaired
  look (4.022/3.937 s settle, no better than 2.46). Outputs: `.freebuff/pw_*` and
  `.freebuff/gen_cost_18_ab`.

- **The install path's per-chunk work is not the fill's pace: two trims of it lost.** A
  fresh install profile of the shipping build priced an install at **28.2 us for a chunk
  with blocks / 15.1 us for an all-air one** (real bucket: stage_results 7.2, insert 5.5,
  stage_dirty 4.9, tail 3.7, stage_light 3.5, data 1.6), and `process_completed_chunks`
  sits at **0.71 ms of its 0.75 ms clock** over 643 frames for exactly 20,230 installs, so
  making those microseconds cheaper looks like the fill's next lever. Two were measured as
  binary A/Bs against the shipped sky-gate DLL, five pairs at 6 logical cores / 5 workers:
  1. **Drop the install-time neighbour fan-out** (the dirty stage fans the same six faces
     out again once the arriving chunk's light is final, so the copy looked duplicate):
     chunk load **9.39 -> 9.61 s**, end 10.40 -> 10.58 s, candidate slower 5/5; on 16
     threads chunk load 3.98 -> 3.94 s but end 4.76 -> 4.93 s, a wash. The first copy is
     what retires a neighbour's boundary holes a light-pass round trip earlier, so it
     stays.
  2. **Batch the completed light-result drain** (one lock and one swap per batch instead of
     a mutex and a deque node per result) with the fan-out restored: chunk load
     **9.35 -> 9.61 s**, end 10.11 -> 10.36 s, candidate slower 5/5, with the same process
     total (2.61/2.63 s) over **2,296 -> 2,424 frames**. Reverted.
  Both arms have the same shape - a cheaper drain frame buys *more frames* for the same
  work - which is the self-balancing admission controller doing its job: at 5 workers the
  fill's 8.3 installs a frame already match 5 workers x 2.75 ms of work per chunk, so
  arrivals, not the drain, are the ceiling. That closes the install side with numbers
  rather than by argument. The profile that started it is
  `.freebuff/install_profile_now.log`; the A/Bs are `.freebuff/install_trim_ab_1`,
  `.freebuff/install_trim_ab_6c` and `.freebuff/light_batch_ab_6c`.

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

## The initial fill's remaining gap

- **QUIET scripts are not the load's missing half-second.** Five AB/BA pairs of the RD 32 fill
  (normal against `QUIET=1`, one discarded warmup) gave load **6.13 [5.28-6.65] s** normally
  and **5.47 [5.27-6.40] s** with every world-scene script except the player controller
  silenced. The paired load difference is **-0.18 s [-0.66 to +0.04]**, quiet faster in only
  **4/5** pairs, while the load frame does move **-0.48 ms [-2.63 to -0.16] in 5/5** and
  `process_total` is unchanged (**+0.11 s** median, quiet faster in 1/5). The later pairs were
  already in the 5.3-5.5 s band, and the one quiet-lost pair (5.32 against 5.28) shows the
  effect is not a reliable half-second. Player physics measures **0.015 ms**, and the title
  curtain is idle in the probe's release path, so neither explains the outside-updater frame.
  A loading-time script pause is not the lever for whole-fill time.
- **Holding mesh until the chunk map settles cuts the load but not the fill.** Five AB/BA pairs
  with `FARLANDS_HOLD_MESH_UNTIL_LOADED=1` skip the mesh completion and dirty-mesh stages
  while `process_mesh_budgets` sees the chunk load busy. Normal load **5.22 [5.20-5.57] s**,
  end **5.48 [5.46-5.84] s**, load frame **8.19 [8.01-8.54] ms**, process **1.65 s**; held load
  **4.72 [4.67-4.93] s**, end **5.47 [5.24-5.71] s**, load frame **7.56 [7.26-8.07] ms**,
  process **1.32 s**. Paired load **-0.55 s [-0.87 to -0.29] in 5/5**, frame **-0.47 ms
  [-1.17 to -0.12] in 5/5**, process **-0.28 s**, but end only **-0.22 s [-0.48 to +0.23] in
  4/5** and the tail grows **0.26 -> 0.78 s**. The load side is now under 5 s, but the whole
  fill is not: the 0.55 s was moved into the mesh tail, not removed, and the median end is
  still 5.47 s. That arm alone only moved time; the held tail was the next lever, not chunk
  admission.
- **The held tail was the loading upload cap; 512 finishes it.** The held cap128 tail is 91
  frames and 0.78 s: `process_completed_meshes` 0.18 s, `far_region_visibility` 0.05 s,
  `process_total` 0.26 s, with the remaining ~0.52 s outside the updater in the renderer. The
  held tail popped 2,620 queue entries, and the 128 uploads/frame drain left the last batch in
  the tail. A 256 cap cut the tail to 0.26 s and the end to **5.02 [4.97-5.23] s**; five paired
  cap256/cap512 runs then gave cap512 **4.79 [4.65-4.98] s** end, **0.00 [0.00-0.01] s** tail,
  all five under 5, and cap512 beat cap256 in **5/5** by **-0.20 s [-0.58 to -0.04]**. The
  previous 128 arm's end was 5.48 [5.30-5.51] s. Shipping is now
  `hold_mesh_until_load_done = true` with `mesh_uploads_loading = 512` and the
  partial-hold trickle described in the next bullet;
  `FARLANDS_HOLD_MESH_UNTIL_LOADED=0` restores the overlap path, and
  `FARLANDS_MESH_UPLOADS_LOADING=N` overrides the cap for an arm. The hold is one-shot and
  bounded: it is disarmed by the first fill settle (the sweep list spent and
  generation/completion empty) or by `FARLANDS_HOLD_MESH_MAX_SEC` (2 s), whichever comes
  first, and is not re-armed by a later flight, so edits and
  light remeshes do not pause again when the world briefly leaves the quiet-load state. This
  changes the initial fill's order: the chunk map settles first, then the held completions
  upload at 512/frame. A shipped-default check with no environment overrides measured
  **4.98 s** end to end, inside the 512 arm's range. The first-visible measurement below is
  the other half of that trade.
- **The partial hold removes most of the blank world without paying for it in end time.**
  Five AB/BA pairs at RD 32 on the 15-worker machine, both arms keeping the hold and the
  only difference `FARLANDS_HOLD_MESH_PARTIAL` 0 vs 2 (A = all-or-nothing, B = at most two
  nearest rebuilds/uploads a frame; `probes/start_fill_ab.py --hold-partial 2`, no warmup,
  `.freebuff/hold_partial_2_ab`): first upload and first visible **1.83 [1.81-1.88] s ->
  0.29 [0.27-0.31] s**, loaded **4.99 [4.94-5.27] -> 4.99 [4.96-5.04] s**, end **4.99
  [4.95-5.27] -> 5.00 [4.96-5.04] s**. The paired end difference is **-0.02 s [-0.04 to
  +0.28], B faster in 2/5 and slower by 0.02-0.04 s in 3/5**; the +0.28 is one 5.27 s A
  outlier. Load frame **7.66 -> 7.92 ms**, process total **1.29 -> 1.36 s**, worst frame
  **57.4 -> 54.9 ms**, slow frames **17 -> 19**. The hold still batches the bulk of the mesh
  work; the two-a-frame trickle is enough to put the nearest terrain on screen almost
  immediately, without the full overlap's 0.69 s end cost. This is a perceived-fill win, not
  an end-to-end speedup: the chunk map still settles on the same schedule. Shipping is now
  `hold_mesh_partial = 2`; zero restores the all-or-nothing hold, and
  `FARLANDS_HOLD_MESH_PARTIAL=N` overrides it for an arm. The first-visible measurement below
  is the all-or-nothing arm this replaces.
- **The hold is now bounded, and the blank world it buys is bounded with it.** The fill probe
  reports first upload, first visible and the in-frustum drain (`PROBE fill first ...`). The
  shipping hold releases at the first fill settle, or at `FARLANDS_HOLD_MESH_MAX_SEC` (2 s),
  whichever comes first. Five paired hold-on/hold-off fills at RD 32 (one warmup discarded,
  A = `FARLANDS_HOLD_MESH_UNTIL_LOADED=1`, B = `0`; `--hold`, with `--vsync` on the
  constrained arms): on the unconstrained **16-thread** machine first visible is **1.85
  [1.83-1.89] s against 0.27 [0.27-0.28] s**, end **5.24 [4.97-5.28] against 5.69
  [5.45-5.83] s** (paired **-0.50 [-0.59--0.27]** in 5/5), so the bounded hold still buys
  ~0.5 s of end-to-end fill for ~1.6 s of blank world (`.freebuff/hold_ab_16c_bounded_20261110`).
  At **6 cores, 5 workers** first visible is **2.11 [2.11-2.14] s against 0.55 [0.53-0.56] s**,
  end **11.05 [10.90-11.19] against 11.19 [10.92-11.28] s** (paired **-0.11
  [-0.23-+0.03]**, A faster in 4/5): the old 7.85 s blank world is gone, but the end win is
  only ~0.1 s (`.freebuff/hold_ab_6c_bounded_20261110`). The old unbounded 6-core arm measured
  7.85 s first visible and a 0.69 s end win; it also ran 15 workers on 6 logical processors, so
  its magnitudes were oversubscribed. The **4-core, 3-worker** arm is an end-to-end comparison
  with the corrected `Unrendered` gate: the matched hold-off control reaches chunk
  load **18.38 s** and end **19.13-19.14 s**. A `FARLANDS_GEN_PROFILE=1` run on the same
  4-core/3-worker arm (`.freebuff/fill_path_3w`) splits a chunk's trip: wait (admit ->
  worker start) p50 **50.4 ms**, work (start -> finish) p50 **1.9 ms**, drain (finish ->
  install) p50 **1.5 ms**, total p50 **60.3 ms**. The wait is queue residence, not a wake
  bug: Little's law on the **52**-deep in-flight set at 1,101/s predicts ~47 ms. Work is
  1.9 ms (1.77 mean) because the worker body also runs the light pass (0.53 ms), the edit
  map (0.18 ms when edited) and `compute_fully_solid`, so generation occupies **~1.95 of the
  3 workers** - the 1.02 figure counted only `generate_chunk` - and mesh builds add ~0.6.
  The 0.19 ms edit slice is not the shared lock: a split on the same arm puts **0.00 ms in
  the lock and 0.00 ms in the pure apply**, and all of it in the disk load - 381 chunks
  have edit maps, each takes ~10-16 ms to read and decode, and that is 0.18-0.31 ms spread
  over the 20,230. A world with no edit files skips the branch after the one-time scan.
  The disk work is content, not per-chunk overhead; a cost-aware allowance cannot remove
  it, only keeping the decoded maps resident or reading them faster can.
  The **85% was a model figure, not measured idleness**: the feed-forward controller's 90%
  target applied to a 2.46 ms allowance. The pool's own counters over the same 18.3 s load
  put the workers at **98.5% busy** - 0.7% idle with work queued (361.8 ms in 58,152
  episodes, ~6 us each, 58 steals) and 0.9% idle while empty (475.8 ms in 15 episodes).
  The 52-deep in-flight set is a buffer the workers drain into the next admission, not a
  backlog they cannot reach: there is no per-worker imbalance or steal latency to chase,
  and the pool is the wall. The decider confirms that reading. With the 64-chunk cap fixed,
  lowering the cost model alone changes nothing (2.46/1.8/1.44 ms: **20.32/19.98/20.05 s**)
  because the cap refuses the extra allowance; moving the cap with it buys **8.3%**
  (2.46/64 **20.36 s**, 1.8/128 **19.18 s**, 1.44/192 **18.67 s**, one pair each in the
  same session), so the 0.9 target already sits on the 3-worker pool's throughput. The
  constant is the admission input, not the measured cost: the pool's own counters give
  **2.68 ms per chunk** (2.955 worker-seconds per second / 1,101 chunks/s), which predicts
  **18.1 s** at 3 workers with no memo (measured 18.10-18.41 s at 1.44/192) and **10.8 s**
  at 5 (measured 10.03-10.19 s). Removing the 1.05 ms terrain leaves ~1.6 ms, or **10.8 s**
  at 3 (measured 10.06-11.82 s) and **6.5 s** at 5 (measured 5.84-6.06 s). The earlier
  1,101-vs-1,098 agreement was pool saturation, not the allowance being the limit, and a
  closed-loop rule chasing 100% would spend main-thread headroom for that 6-8%.
  The case where the rate stops being the question is the cache: at the default 2.46/64
  the faithful memo's second fill is **11.82 s** against **19.32 s** with no hits, and at
  1.44/192 it is **10.06 s load / 10.32 s end** against **18.10 s**; the 2x2 is in
  `docs/terrain-cache-plan.md`. The memo now records and replays the cross-chunk spill,
  so this is the faithful hit. A memo-off second pass is slower at 3 and 5 workers
  (20.63 s against 18.41 s; 11.12 against 10.19 s), so the gain is the memo, not warm
  state. At 15 workers the warm hit is **3.05 s** against the warm second pass's **4.69 s**;
  that is not a cold result; the phase-4 cold-process arm measured **6.25 s against
  6.59 s** at 16 threads (5.2%), and 23.4% / 13.7% at 4 / 6 cores in the plan. Five
  workers: 11.12 -> 5.84 s. A no-edit 3-worker world is 16.04 -> 8.05/8.31 s; the 381
  edited chunks add about 2.0 s.
  The old 19.84 s/90 s no-end figure was the
  probe's `active_uncovered` gate: at 300 s it stayed flat at **92** with no pending work,
  while `Unrendered` stayed **0** and 608 instances drew. Those 92 are far chunks whose region
  is active but does not list them as members; their own chunk instances draw them.
  `active_uncovered` is not a hole count, and the old `PROBE FAIL` named the wrong metric. The
  probe now gates on `Unrendered` (geometry present, no instance drawing it). The old 15-worker
  arm finished in 14.33 s. On the small discs
  the old gate could never release: at **RD 4** the hold
  disarmed at **0.27 s** and at **RD 5** at **0.54 s**, with first visible on the same frame.
  The one-shot/bounded behavior is in `InitialMeshHold`; the 2 s default is env-overridable, and
  the A/B harness exposes `--hold` and `--workers`. Shipping now enables the hold only with **at
  least 12 workers**. Five-pair medians: 8 workers 6.46 s hold against 6.48 s off (A-B -0.07 s,
  a wash), 10 workers 6.25 against 6.28 (A-B -0.02 s, still a wash), and 12 workers 5.51 against
  5.77 (A-B -0.26 s, hold faster in 4/5 pairs). Five workers bought ~0.1 s while 15 bought ~0.45 s.
  The default worker count is a logical-thread proxy, not physical cores: a 4c/8t machine uses 7
  and holds off, a 6c/12t machine uses 11 and holds off, an 8c/16t machine uses 15 and holds on.
  `FARLANDS_HOLD_MESH_MIN_WORKERS` overrides the floor, and an explicit
  `FARLANDS_HOLD_MESH_UNTIL_LOADED=1` forces the hold on so slow-machine A/B runs still measure it.
- **The whole-fill worst frame is the reveal burst on 16 threads and the scheduler on 6.** The
  probe keeps the 24 worst frames (by cost, not arrival) and prints a phase dump each plus a
  before/after-settle count. On the 16-thread bounded hold the worst is **58.1 [53.2-58.8] ms**,
  in the same reveal burst the unbounded arm measured at **62.9 ms at 4.37 s**: a **22.0 ms**
  `process_total` with
  `ProcessCompletedMeshes` **17.6 ms** (`MeshUploadGpu` **10.9 ms**), `FarRegionVisibility`
  3.2 ms and `DirtyMeshQueue` 1.3 ms; the rest of the wall interval is outside the updater.
  The frame drew 248 calls and 1.21M primitives. The pre-settle slow frames are a different
  phase: `ChunkLoadUnload` up to **29-41 ms** and `BlockOutlineProcess` up to **12 ms**, in the
  first second. On 6 cores the bounded worst is **285.8 [279.2-291.5] ms**, `process_total`
  **2.6 ms**, `ChunkLoadUnload` 2.2 ms, `outside_process` **114-129 ms**, 57 draw calls /
  1,667 objects, and the engine's own delta matches the stall. The event is at **t=0.28 s**,
  before the chunk load, with Godot's `TIME_PROCESS=80 ms` and `TIME_PHYSICS_PROCESS=47 ms` in
  the same dump: it is startup engine work, not the hold or the updater. The old 4-core medians **334 vs
  318 ms** and the **1,603 ms** outlier came from the 15-worker arm; the matched 3-worker arm
  did not complete inside 300 s, so there is no matched 4-core worst-frame number. The
  60-76 ms class is the 512-upload reveal, count-bounded rather than clock-bounded (which is
  why no clock moved it), while the constrained worst frames are outside `process_total`; the
  load-to-tail switch itself is 2.2 ms.
