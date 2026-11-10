# Generated-terrain cache: a plan (not implemented)

**Status:** design note, not approved; the cache codec is not implemented. The
measurement-only decider below (two admission env switches and an in-RAM memo)
has run and is recorded in [What paces the 3-worker fill](#what-paces-the-3-worker-fill-measured). If
the experiment loses, record the result in
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
workers on 4 cores. That is the number the cache has to beat.

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
  rewrite. The 2.46 ms figure was measured at 15 workers, where contention makes
  a chunk dearer (2.31 ms at 3 workers).
- The formula holds at 5 workers (predicted 11.06 s, measured load 10.91 s) and
  over-predicts at 8 (6.91 s against 7.86 s), where the main thread's install/
  light path becomes the next wall.
- A block cache removes the 1.05 ms terrain from a 1.9 ms generation task but
  still runs light and the edit map, so it cuts generation work by roughly
  55-60%. A cache alone does NOT pay under the current allowance: the rate stays
  ~1,100/s whatever a task costs, so the fill would stay near 18 s with the pool
  half idle. The combined case does: with the in-RAM memo hit path and the cost
  model at 1.44 with a 192 cap, the second fill is **10.35 s** against
  **18.87 s** without hits - the 10-11 s estimate, bounded above the ~7.9 s
  install wall. The hit must still read/decode in less than the terrain it
  replaces; the memo's hit path is a RAM copy, so the disk codec has the
  3.4 s / 0.17 ms-per-chunk margin below.

**Consequence:** the 4-core 25% target is met by the in-RAM hit path: the load
falls **18.87 -> 10.35 s** (45%), so the 13.8 s bar leaves **3.4 s** of margin
for reading and decoding the cache - about **0.17 ms per chunk** over the memo's
own copy. The cache removes about 55-60% of a generation task (terrain 1.05 ms
of 1.9 ms; light and the edit map remain), minus the read/decode. If decode
costs more than that margin, the end-to-end win shrinks; measure the real
codec's per-chunk read/decode against the memo copy.

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
lowering the cost model alone is refused by the cap. So the admission-only
ceiling is **6-8%**, not the ~1.7x the rate formula suggests: at 3 workers the
default controller already sits on the pool's throughput.

The combined case uses `FARLANDS_GEN_MEMO=1`, which stores each chunk's generated
base (before the edit map, light and solid pass) in a process-wide map, and
`probes/probe_fill_memo.gd`, which fills once to warm it, calls
`clear_editor_chunks()`, and fills the same world again. At 1.44/192 with 3
workers the warm pass (no hits) is **18.87 s** and the hit pass is **10.35 s**
(-45%). The hit path is still a RAM copy of `ChunkData`, not a disk read/decode,
so this is the optimistic side of the cache's range; it is enough to justify
the codec only if the codec stays inside the 0.17 ms/chunk margin above. The
memo does not replay cross-chunk vegetation writes on a hit (the first pass
already made them), and it is never enabled without the env var.

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
  tail all remain. On 16 threads the current hold already caps the blank at 2 s
  and ends at about 5.2 s; the cache can plausibly shorten the load on
  constrained machines, not the 2 s reveal ceiling, and should not be expected
  to save seconds on 16 threads. At 3 workers the memo hit path puts the load at
  **10.35 s** (18.87 s without hits); the end/far tail beyond that was not
  captured, so the full-fill saving still needs the real cache's end-to-end arm.
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
- **The 16-core return may be negative.** The read, decode and install can cost
  more than the ~1.4 s of generation they replace. This is the main reason to
  run the experiment before writing the implementation.

## Size and cost estimate

| Item | Estimate | Basis |
|---|---|---|
| Chunks in the spawn fill | 20,230 | 16-thread RD 32 fill |
| Chunk classes | 7,620 air / 7,660 solid / 4,950 mixed | same fill |
| Chunk payload | ~5.5 KB average block storage (loose) | 110.6 MB / 20,230 |
| Full spawn cache | **110.6 MB loose ceiling** | palette storage, not light |
| Region packs | ~2 MB each, ~51 packs | 8x8 columns, partially filled at the rim |
| Removed work | terrain slices and spill replay | edit probe, light, install remain |
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
packs, each small enough to read as a unit, evict on its own and decode on a
worker. Per-chunk files would be ~20,000 seeks and an unpleasant directory.

## Integration

### Read path

- `src/core/terrain_cache_format.*` is the pure codec and key: header/index
  encode/decode, section codec, CRC and bounds checks, no Godot types. The
  round-trip and corruption tests live beside the other core tests.
- `src/world/terrain_cache.*` owns the manifest, the immutable key and a small
  LRU of decoded region packs. It is the only code that opens cache files.
- `ChunkWorld::generate_chunk` checks the cache before enqueueing generation.
  A hit enqueues a cache-read closure through the **same** scheduler claim,
  completed-chunk and install path. The closure reads and decodes the block
  data, replays the stored spill list through the **same full `cross_writer`
  closure** a miss uses, applies the current edit map, runs the same sky/block
  light calls and `compute_fully_solid`, then returns the chunk. The closure is
  not just `queue_vegetation_placement`: it also pushes
  `pending_cross_boundary_remesh` under `cross_boundary_mutex`. Extract that
  body into a helper and call it from both generation and replay; replaying
  only the vegetation queue leaves the neighbour's mesh stale. Install does not
  need a branch. Replay is idempotent with respect to a neighbour that is also a
  hit: it queues the write whether or not the neighbour later generates, exactly
  like a normal generation.
- A cache hit still counts as in-flight and still takes an admission slot, so
  the fill's existing pacing and the hold's settle test see the same shape.
- `CompletedChunk::was_loaded_from_disk` is set by the scheduler but never read
  today; do not build the design on it. Add a small source enum or cache
  counters if the probe needs to distinguish a hit from a generation.
- Read on workers. The main thread only decides hit or miss and enqueues; the
  pack read/decode must not take `file_access_mutex` per chunk, for the same
  reason the edit-map load stopped doing it.
- The cache hit's light pass uses the same neighbour lookup as generation; the
  install light fix-ups still run. The cache never stores a light result that
  the current key did not produce.

### Hit/miss parity test (phase 2 gate)

Before the cache is allowed on by default, compare the complete installed
`ChunkData` for an all-air, a fully-solid and a mixed chunk. Run the miss path
(generator output, current edit map, sky/block light, `compute_fully_solid`,
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

Add cache counters to the fill probe and `GenerationStats`: hits, misses, bytes
read, bytes written, read/decode/write time, hit rate and pack count. Add a
`TERRAIN_CACHE=0/1` environment switch, default off, and a `--cache` arm to
`probes/start_fill_ab.py`.

The experiment needs a **prepared** cache. Use one warmup run with an empty
cache to write it, then discard that run and pair the normal build against the
cache-hit build in AB/BA order, five pairs at 16, 6 and 4 cores with `--vsync`.
Wipe the cache directory between pairs and rebuild it before each pair, so a
pair measures a cold read, not a filesystem cache that warmed during the
previous run. Measure the first-run write path separately, with an empty cache.

Report first upload, first visible, the in-frustum 50/90% times, load, end, slow
frames, cache phase totals and hit rate. The proposed go/no-go criteria:

- 4 cores: **load and end** improve by at least 25%, with no
  cache-attributable frame over 33 ms. Do not use first visible here: the hold
  is off below 12 workers and the 3-worker run already reaches it around 0.30 s,
  so a 25% first-visible target is meaningless. The matched baseline exists: 3
  workers on 4 cores, chunk load **18.38 s** and end **19.14 s**, hold off.
  The in-RAM decider already clears the load half: **10.35 s** against
  **18.87 s** (45%), so the 13.8 s bar leaves **3.4 s** - about **0.17 ms per
  chunk** - for the disk read and decode over the memo's own copy. Require the
  cache arm to improve both load and end by at least 25%, and require the hit
  path (read + decode + light + edit) to stay inside that margin; if it does
  not, stop. Report the 8-worker arm too, where the install path is the next
  wall.
- 6 cores: load improves by at least 15%.
- 16 threads: end is no worse than the current hold (~5.2 s on this machine)
  and first visible is no worse; hit rate is at least 90% of the fill's installs.
- On one worker, a hit's read + decode is cheaper than the generation it
  replaces. If it is not, stop: the cache is moving CPU to disk, not removing
  work.
- If 16 threads regress or the 4-core gain is under 10%, reject the cache and
  record the measured row.

Tests: round-trip all-uniform, all-air and mixed sections; spill-list
round-trip with zero, one and many writes; a cache hit that replays a spill into
a neighbour and produces the same canopy as a miss; CRC corruption, truncated
pack, wrong key, unknown block IDs, count/offset overflow, empty and edited
chunks; key changes for each config input; a concurrency test for a reader,
writer and `clear()`. Add the CI digest test described in the key section over a
few regenerated chunks, and keep the failure message naming
`kTerrainCacheDataVersion`. Keep the files under the 500-line cap by splitting
the format, the I/O and the tests, and keep the cache out of the Godot-free fuzz
lists unless a harness actually needs it.

Gates: `scons sizecheck`, `scons portability`, `scons docscheck` and the
clang-tidy job. Update `benchmark_baseline.txt` only if a cache timing becomes
a benchmark metric; otherwise the probe's cache fields are the evidence.

## Phases, if approved

1. **Key and format.** Pure codec, pack/index encode/decode, section codec,
   key hashing, round-trip and corruption tests. No I/O, no integration.
2. **Reader.** Manifest/pack I/O, the scheduler cache-read closure, counters,
   the environment switch off by default. A prepared cache must install and a
   miss must be indistinguishable from today's fill.
3. **Writer.** Generator-output capture, spill list, 8x8 pack writes, atomic
   manifest, cap and LRU, reset. A second boot must be a hit; a crash mid-write
   must leave a usable partial cache; no edit may be lost or baked into the
   base, and a hit's spill replay must match a miss's canopy.
4. **A/B and decision.** The 16/6/4-core matrix, the criteria above, and the
   decision row. If it loses, stop here.
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
