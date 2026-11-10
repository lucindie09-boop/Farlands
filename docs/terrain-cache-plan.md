# Generated-terrain cache: measured and rejected for default use

**Status:** phases 1 through 3 are implemented; phase 4 measured the cache and
rejected it for default use, so phase 5 is not entered. `src/core/terrain_cache_format.*`
holds the key/section/pack codec; `terrain_cache_manifest.*` and
`terrain_cache_reader.*` load `cache.meta` and every pack's trailing index into
RAM, then read a chunk payload by offset; `terrain_cache_writer.*` stages encoded
chunks per 8x8 region and merges each region's existing pack. `src/world/terrain_cache.*`
supplies the key, pack discovery, one shared `FileAccess` per pack guarded by a
per-pack mutex (62 handles, not one per worker), the 512 MiB least-recently-used
world cap and the pool write. `ChunkWorld::generate_chunk` uses a hit before
generating and falls back on any miss; a miss stages the generator's own answer
before the edit map, light and solid flags. `ChunkWorld::process_completed_chunks`
ticks the writer, and a world reset closes both halves. The switch stays
`FARLANDS_TERRAIN_CACHE=1`, off by default. The phase-4 matrix (5 AB/BA pairs at
4, 6 and 16 threads, `--vsync`, cache rebuilt before every pair) is below:
**23.4% load / 21.5% end at 4 cores** (25% bar), **13.7% / 13.2% at 6** (15% bar)
and **5.2% / 5.3% at 16** (no regression). The read path is no longer the limit
(0.33 s at 3 workers, 0.46 s at 15); decode (~4.3-4.5 s) plus the install/tail is
why the cache is not worth its disk, invalidation and parity surface.
The hit/miss parity gate is still open; it is a correctness gate for the
feature-flagged path, not a reason to ship it. The pool cost is now split by task class:
chunk mesh first build vs remesh, revision-driven region uploads, light, far and
prefetch (the older combined bucket was mesh 0.60 ms/chunk, light 0.17, far
0.02), and the reset queues are instrumented. The reader design is per-chunk payload reads from the 51 pack
indexes held in RAM, not a decoded-pack LRU. The phase-4 16-thread arm is a
cold-process run, but its reads were warm-OS-cache only; the cold
spinning-disk read was not available on this machine. The measurement-only
decider below (two admission env
switches and an in-RAM memo) is recorded in
[What paces the 3-worker fill](#what-paces-the-3-worker-fill-measured). If the
experiment loses, record the result in
[decisions/README.md](decisions/README.md) rather than growing this file.

The initial fill generates the spawn disc every boot. The terrain is a pure
function of the seed, the worldgen data and the block properties, and the save
stores only sparse edits. Saving the generated block map could make the next
boot skip generation. This note asks whether that is worth its disk, format and
invalidation cost.

**Short answer:** it is a warm-start cache, not a save. It can remove
generation from the fill; it cannot remove mesh build/upload, install/admission
or the initial mesh hold's release. The hold now caps the blank at 2 s, so the
cache's only defensible value is **full-fill time on constrained machines**. On
16 threads the current hold reaches first visible at 1.85 s and ends at about
5.2 s; on 6 cores it ends at about 11.1 s. At RD 32 with 3 workers the chunk
load reached 19.84 s, but the far-gap gate was the probe's `active_uncovered`
count. The corrected `Unrendered` gate gives **18.38 s load / 19.14 s end** at 3
workers on 4 cores. That is the number the cache has to beat; a no-edit world
is **16.04 s load / 16.04 s end**, the 381 edited chunks add about 2 s.

## What the hold is not

The initial mesh hold is not pregeneration. Generation and installation run
during the fill; the hold gates mesh completion/upload until the chunk load
settles, then drains the held meshes. The win is scheduling and reveal order,
not a precomputed chunk map. The cache below is a different optimisation: it
attacks the generation that fills the map. See
[streaming.md](streaming.md) for the fill's measured anatomy and the hold's
trade.

## What would be cached

- The generated **block base and its cross-chunk spill list**. No light, no
  meshes, no render data, no entity or fluid schedules. The spill list is the
  set of world positions and block IDs that this chunk's own generation wrote
  into its neighbours through `ChunkGenerator::CrossChunkWriter`.
- The initial fill's spawn region only (the RD 32 disc around spawn), not every
  chunk the player explores. Later streaming is already hidden by movement; the
  spawn fill is the one large, predictable load.
- Every chunk in that region, edited or not. The cache stores the generator's
  own answer; the current edit map is applied as a delta on a hit, so an edit
  added or removed later still starts from the same generated base. The edit
  maps remain the sole delta and are never baked into the cache.
- A disposable artifact. Deleting it costs one regeneration, never world data.

The boundary is `generate_chunk`'s generator output, not the installed chunk.
That is before the edit map, light and `compute_fully_solid`, and it is why the
no-edit rule and a post-settle snapshot are no longer needed. A cache hit must
replay the stored spill list into the same pending-vegetation queue a normal
generation would have pushed to; otherwise a neighbour that still generates
loses the canopy this chunk would have written into it.

## Win

- **It removes the generator from the fill, not the blank window.** The old
  11.9 s and 7.9 s blank figures are stale: the bounded hold makes the
  16-thread blank 1.85 s (first visible) and the 6-core blank 2.11 s, while the
  old ~3.9 s 16-thread figure is now ~1.85 s. Generation is still about 20.5 s
  of worker time, but the measured 3-worker RD32 chunk load was **18.38 s** and
  end **19.14 s**, not the ~6.8 s the worker arithmetic predicted. The profile
  below says why: the default controller's 0.9 utilization target lands on the
  3-worker pool's own throughput, and opening it 70% buys only 8%. That fill is
  **pool-throughput-bound in its default configuration**, not install- or
  admission-paced. The earlier 85% task-busy figure was a model figure (the
  controller's 90% target on a 2.46 ms allowance), not a measurement; the
  pool's own counters over the same load put the workers at **98.5% busy**
  (0.7% idle with work queued, 0.9% idle while empty, 58 steals), so the pool
  is saturated and the queue is a buffer, not a backlog.
  The old 19.84 s/90 s figure was the false `active_uncovered` gate. On 16
  threads generation is about 1.4 s of a ~5.2 s end-to-end fill, so the return is
  small and may not cover the read. **Do not use the 6.8/4.1 s generation-wall
  arithmetic as a cache estimate; at 3 workers the shared pool sets the floor,
  and at 16 the install path does.**
- **It keeps the existing pipeline.** A cache hit still occupies a scheduler
  claim and a completed-chunk slot. Install, light fix-ups, dirty-mesh fan-out,
  edit maps and the hold all run exactly as they do for a generated chunk.
- **The scope is bounded.** Spawn-only keeps the disk and the invalidation
  surface small. The cached base is immutable for a cache key; only the edit
  maps change, and they are applied after the read.
- **Reads parallelise.** 8x8 column packs are independent; a partial cache is a
  partial hit, and a missing or corrupt pack is simply a generation miss.
- **No streaming-path change.** A chunk outside the cached region misses and
  generates as today; the cache only helps when the player returns to spawn.

### What paces the 3-worker fill (measured)

`FARLANDS_GEN_PROFILE=1` on the same 4-core/3-worker arm (20,230 chunks,
`.freebuff/fill_path_3w`) splits a chunk's trip through the pipeline:

| Segment | p50 | Meaning |
|---|---|---|
| wait (admit -> start) | **50.4 ms** | queue residence behind the 64-chunk in-flight cap |
| work (start -> finish) | **1.9 ms** | terrain 1.05 + light 0.53 + edit 0.18 (disk, 381 edited chunks) + solid |
| drain (finish -> install) | **1.5 ms** | completed queue + install |
| total | **60.3 ms** | = 52 in flight / 1,101 per s by Little's law |

The 50 ms is not dispatch latency and not install: it is the cap holding ~52
tasks the pool drains at ~1,100/s. `generate_chunk` alone is 1.05 ms and looks
like 1.02 of 3 workers; the full worker body averages 1.77 ms (p50 1.9), so
generation occupies **~1.95 of the 3 workers** and mesh builds add ~0.6. The
earlier **~85% busy** was the feed-forward controller's 90% target applied to a
2.46 ms allowance, not a measurement. The pool's counters over the same 18.3 s
load put the workers at **98.5% busy**: 0.7% idle with work queued (361.8 ms in
58,152 episodes, ~6 us each, 58 steals) and 0.9% idle while empty (475.8 ms in
15 episodes). The in-flight cap is a buffer, not a lever; the queue is deep
because the cap holds it there, not because the workers cannot reach it. The
decider below shows the rate is not the lever either.

Consequences:

- Raising the allowance alone does nothing: at 3 workers the cap is 64 and the
  gate already refuses 42% of frames (2.46 -> 1.8 -> 1.44 ms, cap fixed:
  **20.32/19.98/20.05 s**). Moving the cap with it buys only **6-8%**
  (2.46/64 20.36 s, 1.8/128 19.18 s, 1.44/192 18.67 s), so the constant is
  nearly right here (2.31 ms measured against 2.46) and an admission rewrite is
  not the big win. That 6-8% is also the headroom a closed-loop rule could
  chase, at the cost of the main thread's spare time.
- A feedback rule - admit while the pool is draining and the completed queue is
  short - would chase at most those 6-8% and needs the main-thread headroom the
  worst frames showed is scarce; the evidence no longer justifies an admission
  rewrite. The 2.46 ms value is the admission constant, not the measured pool
  cost: the pool's own counters put the cost at **2.68 ms per chunk**
  (2.955 worker-seconds per second / 1,101 chunks/s). A later 3-worker
  per-class run measured **2.64 ms/chunk** in the old combined buckets:
  generation 1.78, mesh 0.60, light 0.17, prefetch 0.06, far 0.02, and no
  other/save/path tasks. The old 0.91 ms non-generation residual is therefore
  mesh (0.60, not the 0.54 estimate), light (0.17), prefetch (0.06) and far
  (0.02), with nothing unexplained. On the memo hit the same combined counters
  read generation 0.72, mesh 0.53, light 0.19, prefetch 0.05, far 0.01
  (**1.52 ms/chunk**). That mesh number was first builds, remeshes and region
  uploads together; the split below shows the lever is the remesh, not the
  region upload.
- **Reset diagnostic (3-worker, memo off).** Before `clear_editor_chunks()`:
  **1** pending vegetation chunk / **22** placements, **0** cross-remesh
  requests, **0** scheduler claims, and **0** completed chunks/meshes. After:
  all zero. `ChunkWorld::clear()` was missing the boundary-remesh clear; it now
  clears the queue and the block and vegetation placement maps under their own
  locks. The measured second fill is nevertheless slower: **17.87 -> 20.39 s**
  in the earlier arm (both controls ran with `FARLANDS_GEN_MEMO` unset; the
  discarded `=0` arm that read 10.08 s was a hit, not a control). The combined
  mesh bucket rose **11.86 -> 18.02 s**, 14,821 -> 18,699 tasks, while
  generation was flat (35.26 -> 35.12 s). The suspected spill/placement leak is
  not the cause. The split below says which mesh task carries the penalty; the
  New World button should still not assume the reset is cheap.
- **Mesh split (2026-11-10).** The 3-worker 1.44/192 arms were re-run with
  `mesh_first`, `mesh_remesh` and `region_upload` counted separately. The
  machine was faster than the 18.3 s arm (memo-on pass 1 12.22 s, true
  `FARLANDS_GEN_MEMO=off` pass 1 12.21 s), so only this run's ratios are
  compared:

  | Arm | First mesh | Remesh | Region upload |
  |---|---|---|---|
  | Memo on, pass 1 | 4.90 s / 8,917 | 2.07 s / 4,087 | 0.12 s / 591 |
  | Memo off, pass 1 | 5.04 s / 8,954 | 2.18 s / 4,132 | 0.11 s / 577 |
  | Memo off, pass 2 | 5.57 s / 9,428 | 4.65 s / 6,429 | 0.14 s / 752 |
  | Memo on, hit | 4.87 s / 8,476 | 2.14 s / 3,342 | 0.21 s / 1,666 |

  The memo-off second fill moved 12.21 -> 13.83 s (+13%); its mesh bucket
  7.33 -> 10.35 s (+41%). Of the 2,946 extra tasks, 2,297 are remeshes, 474
  first builds and 175 region uploads; remesh time contributes 2.47 of the
  3.02 s mesh increase, region uploads 0.02 s. The first-build count also rose
  5%, which a straight refill should not do; the classifier calls a build
  "first" when the chunk has no GPU mesh, so a reloaded or reinstalled chunk
  counts again. That is a caveat of the label, not the source of the penalty.
  Remesh count rose 56% and per-task wall time 0.53 -> 0.72 ms (+37%), so the
  count and cost effects both appear. These are wall times inside `execute()`,
  not CPU. With three workers plus the main and driver threads on four cores,
  preemption can contribute; generation is nearly flat (+2.4%), and the
  cross-boundary list was empty before the clear. The mechanism is still open.
  Region
  uploads are revision-driven and did rise, but they are too small to explain
  the old combined figure or the penalty.

  On the memo hit the mesh bucket is essentially unchanged (7.10 -> 7.23 s)
  while generation falls 24.54 -> 9.34 s; the fill itself is 12.22 -> 7.29 s.
  Region uploads rise 591 -> 1,666 tasks yet add only 0.09 s. The memo removes
  generation, not mesh.
- That 2.68 ms is the fill model: 20,230 x 2.68 / 3 = **18.1 s** at 3 workers
  (measured 18.10-18.41 s at 1.44/192; 19.32-19.61 s at the default cap), and
  20,230 x 2.68 / 5 = **10.8 s** (measured 10.03-10.19 s). Removing the 1.05 ms
  terrain leaves ~1.6 ms, or **10.8 s** at 3 (measured 10.06-11.82 s) and
  **6.5 s** at 5 (measured 5.84-6.06 s). The earlier 1,101-vs-1,098 agreement
  was pool saturation, not the allowance being the limit; the formula has no
  independent evidence.
- A block cache removes the 1.05 ms terrain from a 1.9 ms generation task but
  still runs light and the edit map, so it cuts generation work by roughly
  55-60%. The earlier claim that a cache alone does not pay under the default
  allowance was wrong: the memo hit at the default 2.46/64 is **11.82 s** against
  **19.32 s** with no hits; the 2.68 -> 1.6 ms pool-cost model predicts ~10.8 s,
  and the 64-cap's queueing puts the hit at 11.82 s. The cost-aware allowance
  still matters: at 1.44/192 the hit is **10.06 s load / 10.32 s end** against
  **18.10 s** for no-hit pass 1 (a memo-off pass 2 is 20.63 s; both controls
  ran unset, not `=0`). The memo now
  records and replays the cross-chunk spill, so this is the faithful hit, not
  just a terrain copy. The hit must still read/decode in less than the terrain
  it replaces; the memo's hit path is a RAM copy, so the disk codec has the
  3.5 s end margin below (about 0.46 ms of worker CPU per chunk).

**Consequence:** the 4-core 25% target is met by the in-RAM hit path: the load
falls **18.10 -> 10.06 s** and the end **18.10 -> 10.32 s** (44%/43%), so the
13.8 s end bar leaves **3.5 s** for reading and decoding the cache - about
**0.46 ms of worker CPU per chunk** (3.5 x 3 x 0.9 / 20,230), not the 0.17 ms
serial-wall figure. That budget is CPU only: a blocking read also occupies a
worker. The 110 MB read is about 1.1 s of worker time at hard-drive speeds, or
~0.4 s of wall at 3 workers, so it fits. At 16 threads the cold gain is only
~0.6 s (4.6 vs 5.2), and a slow disk or slow decode can erase it. Phase 4
needed the cold file-cache path on a spinning disk, not a warm OS cache; that
read was not available here, so the measured 16-thread gain is an optimistic
ceiling. The memo now replays the cross-chunk spill through the same
writer closure, so the hit includes the vegetation install and boundary
remeshes; this is the codec's honest bound. The benchmark world's 381 edited
chunks are paid on a hit too: the no-edit arm is **16.04 -> 8.05 / 8.31 s**,
about 2.0 s faster than the edited world, so the edited hit's 10.06 s is not all
cache work. The margin is still comfortable for ~5 KB. If decode costs more than
that, the end-to-end win shrinks; measure the real codec's per-chunk read/decode
against this faithful memo copy.

The 1.52 ms hit figure decomposes as generation 0.72 + mesh 0.53 + light 0.19
+ prefetch 0.05 + far 0.01. Generation's 0.72 is the edit map, light and solid
pass and spill replay, which the cache keeps by design. Adding the full
0.46 ms decode budget gives 1.98 ms per chunk: 20,230 x 1.98 / 3 = **13.4 s**,
under the 13.8 s end bar. A real ~5 KB decode with a CRC should land well under
that budget, so the margin is comfortable and consistent with the pool model;
this is arithmetic, not a codec measurement.

### The cheap decider (measured)

The two admission knobs are measurement-only environment overrides:
`FARLANDS_GEN_WORK_PER_CHUNK_MS` (cost model, default 2.46) and
`FARLANDS_GEN_MAX_INFLIGHT` (cap, default `max(64, workers x 6)`). One pair
each on the 4-core / 3-worker / RD 32 arm, A (render loop) chunk load:

| Work / cap | Chunk load | Against control |
|---|---|---|
| 2.46 / 64 (default) | 20.36 s | - |
| 1.80 / 128 | 19.18 s | -5.8% |
| 1.44 / 192 | 18.67 s | -8.3% |

With the cap fixed at 64 the same three work values are 20.32/19.98/20.05 s:
lowering the cost model alone is refused by the cap. The same-session memo-off
controls now span 18.41-19.61 s, and a no-hit second pass is **slower** than
the first (19.61 -> 23.40 s at 2.46/64; 18.41 -> 20.63 s at 1.44/192), so the
admission-only 6-8% is directional and sits inside the run-to-run spread; it is
not a precision measurement.

The memo 2x2 (memo on, the same two-pass probe, load / end):

| Work / cap | Pass 1 load | Hit load | Hit end |
|---|---|---|---|
| 2.46 / 64 | 19.32 s | **11.82 s** | 11.83 s |
| 2.46 / 192 | 18.15 s | 11.09 s | 11.10 s |
| 1.44 / 64 | 19.54 s | 10.84 s | 11.11 s |
| 1.44 / 192 | 18.10 s | **10.06 s** | 10.32 s |

The default 2.46/64 hit is 39% below its own no-hit pass, so the cache alone is
not capped at ~18 s: the 2.68 -> 1.6 ms pool-cost model predicts ~10.8 s, and
the default cap's queueing puts it at 11.82 s. Moving the cost model 2.46 -> 1.44
at a fixed cap buys 8-9%, and moving the cap 64 -> 192 at a fixed cost buys 6-7%.
The cells are one run each against a 6% memo-off spread, so those 6-9% effects
are directional; the 44-51% cache effect is not. A shipped cache should still
carry the cost-aware allowance (1.44/192) in the same change, but the cache is
useful without it. The memo's capture cost is below the run spread: pass 1 is
**19.32 vs 19.61 s** at 2.46/64 and **18.10 vs 18.41 s** at 1.44/192 (both
slightly faster, i.e. noise); the 5-worker pair is 10.03 vs 10.19 s. The
15-worker single run (5.75 vs 6.31 s) is inside that arm's wider noise.

At 5 workers on 6 cores the hit is **5.84 s load / 6.10 s end** against
**11.12 / 11.13 s** for the no-memo second pass (47%/45%), past the 15% bar. At
15 workers on 16 threads the **warm** hit is **3.05 s** against **4.69 s** for
the warm control's second pass (35%). That is not a cold result: the control's
second pass is 26% faster than its first, which a fresh process will not have.
The phase-4 cold-process arm measured **6.25 s against 6.59 s** (5.2%), so the
warm hit plus the ~1.6 s warm-state difference (~4.6 s, about 20-26% below the
5.75-6.31 s cold first pass) overstated the cold gain.

A no-edit world (empty `APPDATA`, probe prints `user_dir`) is **16.04 s pass 1 /
8.05 s load / 8.31 s end** at 3 workers; the 381 edited chunks add about 2.0 s
to both passes, so the edited hit is not all cache work.

The hit path is still a RAM copy of `ChunkData`, not a disk read/decode, so this
is the optimistic side of the cache's range. It is enough to justify the codec
only if the codec stays inside the 3.5 s / 0.45-0.46 ms-of-worker-CPU margin
above. The memo records the spill vector at capture and replays it through the
same cross-writer closure on a hit, so the canopy and boundary remeshes are now
part of the hit; it is never enabled without the env var.

## Drawback

- **Disk.** At RD 32 the fill installs 20,230 chunks: 7,620 all-air, 7,660
  fully-solid and 4,950 mixed. Of 161,840 block sections, 137,773 (85%) are
  uniform. The block sections occupy **110.6 MB** of storage (302 MB including
  light), so 110.6 MB is a loose payload ceiling; the old 180 MB figure was the
  9 KB mixed-chunk ceiling. That is per world and needs a cap and eviction.
- **First run pays and gets nothing.** The cache is written after the first
  fill; the benefit starts on the second boot. A first-run write of up to
  110.6 MB must not block play or the quit path.
- **It is not instant.** The mesh build, upload burst, install and far-region
  tail all remain. On 16 threads the current hold caps the blank at 2 s and the
  end at about 5.2 s; the **warm** memo hit is **3.05 s** against **4.69 s** for
  the warm second pass, so the reveal ceiling is the hold, not the fill. That is
  not a cold result; phase 4 ran the cold-process arm: **6.25 s against 6.59 s**
  (5.2%). At 3 workers the faithful memo hit is **10.06 s load / 10.32 s end**
  against **18.10 s** without hits; at 5 workers **5.84 / 6.10 s** against
  **11.12 / 11.13 s**. A no-edit 3-worker world is **8.05 / 8.31 s** against
  **16.04 s**; the edited world's 381 chunks add about 2.0 s. The real cache's
  end-to-end arm measured **23.4% / 13.7% / 5.2%** at 4 / 6 / 16 threads. The
  read is only 0.33-0.46 s; decode plus the install/tail is why the saving is
  small.
- **Edit maps are paid on a hit.** The benchmark world has 381 edited chunks;
  at 10-16 ms each that is ~4.9 s of worker CPU, about 2.0 s of the 3-worker
  hit (edited 10.06 vs no-edit 8.05 s). The reset clears `chunk_edit_maps` and
  `edit_scan_`, so pass 2 re-reads them and the comparison is not confounded,
  but the paste inflates both baselines. A no-edit world is 16.04 -> 8.05/8.31 s;
  the cache arm must be reported there too.
- **New format, key and invalidation.** A corrupt, stale or partial cache must
  fall back to generation. It must never load wrong blocks, never crash and
  never be treated as save data.
- **I/O contention.** The cache read and the first-run write compete with the
  edit-map saves and the mesh/far tail. They need a low-priority queue, a
  budget, and no per-chunk use of the global file lock.
- **Light is not free.** v1 caches blocks only, so a hit still runs the sky and
  block light passes. If the A/B shows light is the remaining cost, caching
  light is a separate decision with a wider invalidation surface.
- **The queue can deepen.** A fast cache reader can hand the install pass more
  chunks than it drains. Keep the scheduler's in-flight and admission gates;
  never bypass them for a hit.
- **The light-slice precedent says freed worker time may not shorten the fill.**
  Cutting the worker's light slice by 13% made the fill 0.7 s **slower**, because
  the workers finished ahead of the install budget and the completion queue grew
  (`drain` 52 -> 81-105 ms); at 16 threads the load is install-bound, not
  generation-bound. At 3 workers the profile above puts the wall on the shared
  pool (generation 2.1 workers + mesh 0.6), so the cache removes terrain from a
  task that is still 1.9 ms long.
  A cache is the same event on a larger scale: it removes generator work and
  hands the main thread the same installs sooner. The old "~1 s saving at 16
  threads" is therefore not a forecast; the cache has to shorten the end-to-end
  load, and the light-slice result is a reason to expect the opposite.
- **The 16-core return may be negative in a cold boot.** The warm hit saves
  **1.6 s** against the warm second pass (4.69 -> 3.05 s), but a cold process
  will not have the 26% warm control gain; the phase-4 cold arm measured
  **6.25 s against 6.59 s (5.2%)**, so the saving survived but is small. The
  constrained arms are worse (23.4% at 4 cores, 13.7% at 6), so neither case
  justifies default use.

## Size and cost estimate

| Item | Estimate | Basis |
|---|---|---|
| Chunks in the spawn fill | 20,230 | 16-thread RD 32 fill |
| Chunk classes | 7,620 air / 7,660 solid / 4,950 mixed | same fill |
| Chunk payload | ~5.5 KB average block storage (loose) | 110.6 MB / 20,230 |
| Full spawn cache | **110.6 MB loose ceiling** | palette storage, not light |
| Region packs | ~2 MB each, ~51 packs | 8x8 columns, partially filled at the rim |
| Removed work | terrain slices; spill replay still applies on a hit | edit probe, light, install remain |
| First-run write | one-time, seconds, background | encoded pack bytes, budgeted |

The measured class split is why the 9 KB mixed-chunk multiplication was wrong:
**75.5%** of the fill is all-air or fully-solid ((7,620 + 7,660) / 20,230), and
85% of block sections are uniform. Block-section storage is 110.6 MB; the
302 MB total includes light, which this cache does not store. That 110.6 MB is
a **loose ceiling**, not the encoded pack size: it works out to about 4.6 KB per
non-uniform section, and a section is 4,096 cells, so tighter bit-packing or RLE
can still go lower. Measure the actual encoded pack bytes before choosing the cap.

## Cache key and invalidation

`cache.meta` stores a 64-bit key and the cache format version. The key covers:

- a `kTerrainCacheDataVersion` constant, bumped on any change to the generator,
  the light rules or a block property that changes the base. A manual bump can
  be forgotten, so it is not the guard:  `tests/test_worldgen_digest.cpp`
  regenerates fixed chunks and hashes coarse per-block-ID counts plus the
  cross-chunk spill counts against a checked-in value. Three rows are hashed
  (a hills surface, the floor and an ocean column), so bedrock and the
  below-sea path are covered too. A red digest is the instruction to bump the
  constant. Coarse counts are not immune to a one-cell shift - a moved surface
  cell changes two counts by +/-1 - so the digest runs in the Linux, Windows and
  macOS jobs; if one platform goes red without a generator change, quantize
  (count >> 6) before mixing rather than encoding a platform. Keep the version as one named constant shared by the test and the
  cache key, not a literal repeated in two places;
- the seed and the `TerrainParams` fields `world.meta` persists (sea level,
  bedrock, caves, subsurface cover, climate scales, biome size);
- the raw bytes of `data/terrain_config.json`, `data/biomes.json` and
  `data/vegetation.json`;
- the raw bytes of `data/block_definitions.json`: block names, positional IDs,
  and the solid, light and emissive properties that generation and light read.

Not part of the key: block shapes, recipes, shaders, textures and UI data. Those
change meshing or rendering, not the block base; the mesh is rebuilt from the
current data after a cache hit. The append-only block-ID rule makes the cached
IDs stable across a block addition; a reorder would invalidate the cache, as it
already corrupts saves.

Use FNV-1a 64 for the key (no dependency) and the existing
`core/crc32.hpp` for payload integrity. A key or version mismatch means the
cache directory is ignored and deleted, and the fill generates normally. The
cache must never be read as a save format, and `world.meta` is not changed.

## Format

```
user://chunks/cache/<key16>/
    cache.meta
    r_<rx>_<rz>.ftc        # 8x8 column region packs
```

`cache.meta` is the manifest: magic `FTC1`, format version, key, pack count,
total bytes and creation time, written atomically. Each pack has:

- a header with magic, version, key, region coordinates, chunk count, index
  offset, index size and index CRC;
- an index of `cx, cy, cz, payload offset, payload size, payload CRC` per
  chunk;
- payloads of the eight 16-cubed sections, followed by the chunk's spill list:
  a count and `(world x, world y, world z, block id)` records. A section is
  either uniform (`kind=0`, one `u16` block) or paletted (`kind=1`, palette
  count, bits per index, `u16` palette entries and packed indices). This is the
  in-memory palette form, not RLE; it is exact and compact on both uniform and
  mixed terrain. The spill list is part of the chunk record and counts against
  the 1 MiB cap; a corrupt or oversized list is a miss.
- a per-chunk CRC and a per-pack index CRC. All integers are little-endian.
  Counts, offsets and sizes are bounds-checked; a chunk record is capped at
  1 MiB and a pack at a fixed maximum. A bad record is a miss, not an error
  the game reports.

Writes go to `<name>.tmp` and are renamed into place; `.bak` is unnecessary
because the cache is disposable. A crashed write leaves an orphan `.tmp` that
is removed on the next start, and an interrupted pack is simply absent from the
manifest. `cache.meta` is updated last, so a pack is used only after it is
complete.

The 8x8 grouping matches the far-region grid: 3,209 columns / 64 is about 51
packs. The pack is the writer's unit and the index's integrity unit, not the
reader's: the reader loads the 51 indexes into RAM once and then reads and
decodes per chunk by offset, so a hit touches only its own payload. Per-chunk
files would be ~20,000 seeks and an unpleasant directory.

## Integration

### Read path

- `src/core/terrain_cache_format.*` is the pure codec and key: header/index
  encode/decode, section codec, CRC and bounds checks, no Godot types. Phase 1
  implements this layer; `tests/test_terrain_cache_format.cpp` holds the
  round-trip, spill, corruption, bounds and key-sensitivity tests.
- `src/core/terrain_cache_manifest.*` writes and validates `cache.meta`, and
  `src/core/terrain_cache_reader.*` loads the manifest and every pack's trailing
  index into RAM once, keyed by chunk coordinate. It keeps **no decoded-pack
  cache**: `read_chunk` seeks and reads that chunk's payload, checks its CRC and
  decodes it. `tests/test_terrain_cache_manifest.cpp` and
  `tests/test_terrain_cache_reader.cpp` cover the manifest, missing/corrupt/stale
  data, offset reads and concurrent workers.
- `src/world/terrain_cache.*` owns the key, cache root and pack discovery, and
  supplies one shared `FileAccess` per pack, guarded by that pack's mutex. The
  62 handles stay under the engine CRT's 512-file limit even at 15 workers, and
  a worker does not pay a reopen per pack crossing. The fill is a nearest-first
  spiral that touches nearly every pack early, so an LRU smaller than the
  working set thrashes, and decoding a whole pack on first touch makes the other
  workers wait on it. The pack remains the write and integrity unit; the read
  unit is the chunk.
- `ChunkWorld::generate_chunk` calls `ensure_open` on the main thread and then
  checks the reader **inside the same generation closure** it would otherwise
  use for terrain. A hit goes through the **same** scheduler claim,
  completed-chunk and install path. The closure reads and decodes the block
  data, replays the stored spill list through the **same full `cross_writer`
  closure** a miss uses, applies the current edit map, runs the same sky/block
  light calls and `compute_fully_solid`, then returns the chunk. Install does not
  need a branch. Replay is idempotent with respect to a neighbour that is also a
  hit: it queues the write whether or not the neighbour later generates, exactly
  like a normal generation.
- A cache hit still counts as in-flight and still takes an admission slot, so
  the fill's existing pacing and the hold's settle test see the same shape.
- `CompletedChunk::was_loaded_from_disk` is set by the scheduler but never read
  today; do not build the design on it. Add a small source enum or cache
  counters if the probe needs to distinguish a hit from a generation.
- Read on workers. The main thread opens the cache and loads the indexes; the
  worker decides hit or miss. A hit reads its chunk's payload by offset from the
  pack's shared `FileAccess` handle under that pack's mutex; workers on
  different packs do not serialise, and no decoded pack is shared. The read must
  not take `file_access_mutex` per chunk, for the same reason the edit-map load
  stopped doing it.
- The cache hit's light pass uses the same neighbour lookup as generation; the
  install light fix-ups still run. The cache never stores a light result that
  the current key did not produce.

### Hit/miss parity test (phase 2 gate)

Use the in-RAM memo as the oracle. Pass 1 with `FARLANDS_GEN_MEMO=1` captures
the generator's `ChunkData` and its spill vector at the exact boundary the
writer captures. Feed the same chunk through the codec (encode at that
boundary, decode, replay) and compare the installed result against the memo
capture. Do this for one all-air, one fully-solid and one mixed chunk, so the
comparison starts from one generator answer rather than two separate runs that
can drift.

Before the cache is allowed on by default, compare the complete installed
`ChunkData` for those three chunks. Run the miss path (generator output,
current edit map, sky/block light, `compute_fully_solid`,
`compute_section_flags`, install) and the hit path (decode base, replay the full
spill closure, then the same post-generation helper and install). Compare every
cell's `get_block`, every cell's `get_light_word` (or `append_light_state`),
`fully_solid()`, `has_sky_shade()`, and every section's `is_section_all_air`.
Run each with an empty edit map and with one edit.

The light-walk flag is `ChunkData::has_sky_shade_`. It is set by
`propagate_sky_light`/`propagate_sky_light_column`, not by
`compute_section_flags`, which only recounts `section_block_count`. So the hit
must run the same light pass; do not copy the flag from the cache. A hit that
skips the fast path's light/flag bookkeeping is exactly the mismatch this test
must catch.

### Write path

- Capture at the **generator output**, inside the worker's generation closure,
  immediately after `generator.generate_chunk` returns and before the edit map,
  light and solid flags. The base is then exactly the generator's answer and
  the spill list is the full `CrossChunkWriter` output from that same call
  (vegetation placement plus `pending_cross_boundary_remesh`), not a later
  reconstruction. This is the boundary that removes the no-edit rule and the
  post-settle snapshot.
- Store the section payload plus the spill list; do not store installed
  neighbour writes. Applying a neighbour's spill is the install path's job, and
  baking it in would make a cache hit depend on which neighbours happened to be
  generated first.
- Write one region pack at a time on a low-priority worker, then update
  `cache.meta`. A partial cache is valid. Never block `_exit_tree` on the
  writer; the next boot can use the complete packs and generate the rest.
- Do not write a chunk whose generator aborted or whose epoch was retired; the
  attempt id already gates the completion, so use that same gate for the cache
  record. Do not hold `edit_maps_mutex` across file I/O, and do not take an
  exclusive shard lock for a whole pack.
- Cap the cache at 512 MiB by default (the 110.6 MB block-storage figure is a
  loose ceiling; leave room for a second world), evict the least-recently-used world
  cache directory at start or when over the cap, and touch only
  `user://chunks/cache/`. The `.edit` files, `world.meta`, inventory and
  screenshots are never eviction candidates.
- `ChunkWorld::clear()` and any world reset close the cache reader/writer and
  drop in-memory packs. A seed, terrain-params or config change changes the
  key; the old directory is ignored, then evicted by the cap.

### What stays unchanged

The hold, the loading budgets, the mesh queue, the install/light stages, edit
map loading and writing, and `world.meta` all stay as they are. The cache is an
acceleration layer with a hard fallback: disabled, missing, corrupt or stale all
mean today's fill.

## Instrumentation and experiment

The cache adds counters to `get_generation_stats`: hits, misses, corrupt, read
errors, payload/index/manifest bytes, read/decode time, hit rate, pack count and
index entries from the reader, plus staged/written/merged chunks, packs written,
bytes, encode/write time, failures and the staged backlog from the writer. The
switch is `FARLANDS_TERRAIN_CACHE=1`, default off; `--cache` in
`probes/start_fill_ab.py` is the phase-4 arm.

The experiment needs a **prepared** cache. Use one warmup run with an empty
cache to write it, then discard that run and pair the normal build against the
cache-hit build in AB/BA order, five pairs at 16, 6 and 4 cores with `--vsync`.
Wipe the cache directory between pairs and rebuild it before each pair, so a
pair measures a cold read, not a filesystem cache that warmed during the
previous run. Measure the first-run write path separately, with an empty cache.
The 16-thread pair restarts the process after preparing the cache, because the
memo's warm second pass is not a cold boot. Run one 3-worker arm against a
fresh user data directory (the memo probe prints `user_dir`) so the 381 edited
chunks are absent; that arm is 16.04 s miss / 8.05-8.31 s hit today.

Report first upload, first visible, the in-frustum 50/90% times, load, end, slow
frames, cache phase totals and hit rate. These were the pre-experiment criteria
and memo predictions; the measured phase-4 rows are above and win where they
differ. The proposed go/no-go criteria:

- 4 cores: **load and end** improve by at least 25%, with no
  cache-attributable frame over 33 ms. Do not use first visible here: the hold
  is off below 12 workers and the 3-worker run already reaches it around 0.30 s,
  so a 25% first-visible target is meaningless.  The matched baseline is 3
  workers on 4 cores, chunk load **18.38 s** and end **19.14 s**, hold off.
  The in-RAM decider clears both: **10.06 s load / 10.32 s end** against a
  same-session no-hit second pass of **20.63 / 20.64 s** (controls unset;
  51%/50%), and against
  the matched baseline 45%/46%. The 13.8 s end bar leaves **3.5 s** - about
  **0.46 ms of worker CPU per chunk** (3.5 x 3 x 0.9 / 20,230; the serial-wall
  figure is 0.17 ms) - for the disk read and decode over the memo's own copy.
  The memo now replays the spill vegetation and boundary remeshes, so this is
  the faithful hit, not the earlier 9.84 s copy-only arm; the margin is
  comfortable for ~5 KB. The benchmark world's 381 edits cost about 2.0 s of
  the hit (the no-edit arm is 8.05/8.31 s); the cache arm must also stay inside
  that margin on a no-edit world.
  Require the cache arm to improve both load and end by at least 25%, and
  require the hit path (read + decode + light + edit) to stay inside that
  margin; if it does not, stop. Report the 8-worker arm too, where the install
  path is the next wall.
- 6 cores: **5.84 s load / 6.10 s end** against a no-memo second pass of
  **11.12 / 11.13 s** (47%/45%), well past the 15% bar; the historical
  5-worker baseline is 10.9 s.
- 16 threads: the **warm** hit is **3.05 s** against **4.69 s** for the warm
  no-memo second pass (35%), so no regression against the ~5.2 s hold. The 40%
  figure is retired until a cold-process A/B: the control's own second pass is
  26% faster than its cold first pass, so the warm comparison overstates the
  cold gain. Phase 4 ran the engines as fresh processes after preparing the
  cache and measured **6.25 s against 6.59 s** (5.2%), below the earlier warm
  estimate. Hit rate is 1.000 over 20,230 chunks in every cached arm.
- On one worker, a hit's read + decode is cheaper than the generation it
  replaces. If it is not, stop: the cache is moving CPU to disk, not removing
  work.
- If 16 threads regress or the 4-core gain is under 10%, reject the cache and
  record the measured row.

Phase 1 tests (implemented) cover all-uniform, all-air and mixed sections;
spill-list round-trip with zero, one and many writes; CRC corruption, truncated
pack, wrong key, unknown block IDs, count/offset overflow; and a key change for
each persisted parameter and config file. The CI digest in
`tests/test_worldgen_digest.cpp` names `kTerrainCacheDataVersion` in its failure
message. The reader tests cover the manifest, missing/corrupt/stale data, offset
reads and concurrent workers; the writer tests cover idle/size/limit flush,
region separation, restaging, merged packs, corrupt/stale fallback and concurrent
staging. The rest of the integration gate is still open: a cache hit that
replays a spill into a neighbour and produces the same canopy as a miss; empty
and edited installed chunks; and a reader/writer/`clear()` concurrency test at
the adapter level. Keep the files under the 500-line cap by splitting the format,
the I/O and the tests, and keep the cache out of the Godot-free fuzz lists unless
a harness actually needs it.

Gates: `scons sizecheck`, `scons portability`, `scons docscheck` and the
clang-tidy job. Update `benchmark_baseline.txt` only if a cache timing becomes
a benchmark metric; otherwise the probe's cache fields are the evidence.

## Phases, if approved

1. **Key and format.** Pure codec, pack/index encode/decode, section codec,
   key hashing, round-trip and corruption tests. No I/O, no integration.
2. **Reader.** Manifest and the 51 in-RAM pack indexes; per-chunk payload
   reads by offset from one shared per-pack handle under its mutex; per-chunk decode, with no
   decoded-pack cache. Scheduler cache-read closure, counters, the environment
   switch off by default. A prepared cache must install and a miss must be
   indistinguishable from today's fill.
3. **Writer (implemented).** `terrain_cache_writer.*` holds the staging and merge
   core (`tests/test_terrain_cache_writer.cpp`); `src/world/terrain_cache_write.cpp`
   captures the generator output plus spill list on the worker, flushes a region
   after 1.5 s idle or 192 staged chunks (4096 chunks overall), merges the
   region's existing pack, writes `.tmp` → `.ftc` and publishes `cache.meta` last.
   The adapter caps `user://chunks/cache/` at 512 MiB and evicts the
   least-recently-used world directories; `ChunkWorld::clear()` stops the writer
   without waiting for an in-flight pack. A second boot must be a hit; a crash
   mid-write must leave a usable partial cache. The hit/miss parity gate still has
   to prove no edit is lost or baked into the base and that a hit's spill replay
   matches a miss's canopy.
4. **A/B and decision (measured 2026-11-10).** 5 AB/BA pairs at 4, 6 and 16
   threads. Each A run was a fresh engine process with a cache rebuilt in the
   preceding process, so the arm is cold-process; `--vsync` was on.

   | Cores (workers) | Cache load | No-cache load | Load gain | Cache end | No-cache end | End gain |
   |---|---|---|---|---|---|---|
   | 4 (3) | 12.60 s | 16.44 s | 23.4% | 13.11 s | 16.70 s | 21.5% |
   | 6 (5) | 9.68 s | 11.22 s | 13.7% | 9.77 s | 11.26 s | 13.2% |
   | 16 (15) | 6.25 s | 6.59 s | 5.2% | 6.26 s | 6.61 s | 5.3% |

   The 4-core bar was 25% and the 6-core bar 15%; both fail. The 16-thread arm
   does not regress and its hit rate is 1.000 over 20,230 chunks, but 5% does
   not justify the disk, key and parity surface. The reader now opens one
   handle per pack and serves reads under a per-pack mutex: 0.33 s at 3 workers
   (0.016 ms/chunk), 0.36 s at 5 and 0.46 s at 15, against 10.2 s before the
   handle fix. The earlier 16-thread arm's 0.633 hit rate was 15 workers x 62
   pack handles over the engine CRT's 512-file limit; the shared per-pack map
   keeps 62 handles and restores hit 1.000. Decode is 4.27-4.53 s, and the fill
   is install/tail-bound, so the cache stops at 23.4% / 13.7%. Those reads came
   from the warm OS cache because the pair's prep had just written the packs; a
   cold spinning disk can only reduce the gain, so this is the optimistic side.
   The no-edit-world arm was not run; it would remove the edit-map share from
   both arms and cannot rescue a 13.7% gain at 6 cores. **Verdict: reject
   default enablement; leave the code feature-flagged off.** The decision row is
   in [decisions/README.md](decisions/README.md).
5. **Only if it wins.** Optional polish: a per-world cap control, a prefetch of
   the next pack, or an optional light cache. Each is its own measured change;
   none is part of the default.

## Open questions

- Is 4-core or Steam Deck-class hardware a target? That is the case where the
  cache pays; on a 16-thread desktop it may not.
- Is the 110.6 MB loose block-storage ceiling acceptable? A smaller spawn
  radius or a stronger pack codec changes the answer.
- Should the first-run write happen during play, on a menu, or only on quit?
  Quit-only means many players never get a cache; during play needs a budget.
- Per world or shared by cache key? Per world is simpler; sharing a seed could
  reuse the base, but edits and world directories complicate it.
- Is spawn enough? Start there; a cache of all exploration is unbounded and is
  a different design.
- Does light become the remaining cost after the block cache? Measure before
  adding a light cache; light is derived, and its invalidation surface is wider.
