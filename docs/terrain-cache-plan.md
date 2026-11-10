# Generated-terrain cache: a plan (not implemented)

**Status:** design note, not approved, no code. This records the win and the
drawback, and the experiment I would run before writing an implementation. If
the experiment loses, record the result in
[decisions/README.md](decisions/README.md) rather than growing this file.

The initial fill generates the spawn disc every boot. The terrain is a pure
function of the seed, the worldgen data and the block properties, and the save
stores only sparse edits. Saving the generated block map could make the next
boot skip generation. This note asks whether that is worth its disk, format and
invalidation cost.

**Short answer:** it is a warm-start cache, not a save. It can remove
generation from the fill; it cannot remove mesh build/upload, install/admission
or the initial mesh hold's release. It is most defensible on 4- and 6-core
machines, where generation is the larger share of the blank window.

## What the hold is not

The initial mesh hold is not pregeneration. Generation and installation run
during the fill; the hold gates mesh completion/upload until the chunk load
settles, then drains the held meshes. The win is scheduling and reveal order,
not a precomputed chunk map. The cache below is a different optimisation: it
attacks the generation that fills the map. See
[streaming.md](streaming.md) for the fill's measured anatomy and the hold's
trade.

## What would be cached

- The generated **block base** only. No light, no meshes, no render data, no
  entity or fluid schedules.
- The initial fill's spawn region only (the RD 32 disc around spawn), not every
  chunk the player explores. Later streaming is already hidden by movement; the
  spawn fill is the one large, predictable load.
- Only chunks with **no edit map** at snapshot time. Edited chunks regenerate.
  The edit maps remain the sole delta and are applied on top of a cache hit.
- A disposable artifact. Deleting it costs one regeneration, never world data.

The no-edit rule is deliberate. `EditMap::remove_block` exists, so a snapshot
taken after an edit cannot be trusted as the base if that edit is later removed.
A chunk that had no edit when it was copied is safe: an edit added afterwards is
applied on the next load, and if it is removed again the cached copy is still
exactly the generated base.

## Win

- **It removes the generator from the fill.** On the measured constrained runs
  the blank is about 11.9 s on 4 cores and 7.9 s on 6 cores; the fill's
  generation is about 20.5 s of worker time, or roughly 6.8 s of wall at 3
  workers and 4.1 s at 5. That is the largest single block the cache can take
  out. On 16 threads generation is about 1.4 s of the ~3.9 s blank, so the
  return there is small and may not cover the read.
- **It keeps the existing pipeline.** A cache hit still occupies a scheduler
  claim and a completed-chunk slot. Install, light fix-ups, dirty-mesh fan-out,
  edit maps and the hold all run exactly as they do for a generated chunk.
- **The scope is bounded.** Spawn-only and no-edit-only keep the disk and the
  invalidation surface small. The cached base is immutable for a cache key;
  only the edit maps change.
- **Reads parallelise.** 8x8 column packs are independent; a partial cache is a
  partial hit, and a missing or corrupt pack is simply a generation miss.
- **No streaming-path change.** A chunk outside the cached region misses and
  generates as today; the cache only helps when the player returns to spawn.

## Drawback

- **Disk.** At RD 32 the fill installs about 20,230 chunks at 1-20 KB each:
  roughly 20-400 MB, about 180 MB at the 9 KB benchmark chunk. That is per
  world and needs a cap and eviction.
- **First run pays and gets nothing.** The cache is written after the first
  fill; the benefit starts on the second boot. A first-run write of ~180 MB
  must not block play or the quit path.
- **It is not instant.** The mesh build is another ~7.0-7.3 s of worker time
  (about 0.5 s of wall at 15 workers, but ~2.4 s at 3), and the upload burst and
  install remain. On 16 threads the current hold ends at ~4.8 s; the cache can
  plausibly save ~1 s there, not seconds.
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
- **The 16-core return may be negative.** The read, decode and install can cost
  more than the ~1.4 s of generation they replace. This is the main reason to
  run the experiment before writing the implementation.

## Size and cost estimate

| Item | Estimate | Basis |
|---|---|---|
| Chunks in the spawn fill | ~20,230 | RD 32 fill |
| Chunk payload | 1-20 KB, ~9 KB measured | palette-compressed chunk |
| Full spawn cache | ~20-400 MB, ~180 MB typical | payload x chunks |
| Region packs | ~4 MB each, ~51 packs | 8x8 columns, partially filled at the rim |
| Removed work | terrain slices only | edit probe, light, install remain |
| First-run write | one-time, seconds, background | ~180 MB, budgeted |

## Cache key and invalidation

`cache.meta` stores a 64-bit key and the cache format version. The key covers:

- a `kTerrainCacheDataVersion` constant, bumped on any change to the generator,
  the light rules or a block property that changes the base;
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
- payloads of the eight 16-cubed sections. A section is either uniform
  (`kind=0`, one `u16` block) or paletted (`kind=1`, palette count, bits per
  index, `u16` palette entries and packed indices). This is the in-memory
  palette form, not RLE; it is exact and compact on both uniform and mixed
  terrain.
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
  data, applies the current edit map, runs the same sky/block light calls and
  `compute_fully_solid`, then returns the chunk. Install does not need a
  branch.
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

### Write path

- Start after the initial fill's mesh/far tail has drained (the probe's `end`),
  or on a genuinely idle window. Do not write during the chunk load or while
  the initial hold is still withholding meshes.
- Snapshot only chunks in the spawn region with no edit map. Take the block
  copy under the chunk's shared shard lock, with the edit-map check and the
  copy ordered so an edit cannot enter between them; if that is awkward, copy
  and re-check, and pin the choice with a concurrency test. Do not hold
  `edit_maps_mutex` across file I/O, and do not take an exclusive lock for a
  whole pack.
- The snapshot is taken after cross-chunk vegetation and placements have
  settled, so the cached base includes the trees and boundary writes the first
  fill installed. Writing earlier would cache a chunk before its neighbour's
  canopy and produce a base that differs from the generator's final answer.
- Write one region pack at a time on a low-priority worker, then update
  `cache.meta`. A partial cache is valid. Never block `_exit_tree` on the
  writer; the next boot can use the complete packs and generate the rest.
- Cap the cache at 512 MiB by default (one spawn cache is ~180 MB; two worlds
  fit with room), evict the least-recently-used world cache directory at start
  or when over the cap, and touch only `user://chunks/cache/`. The `.edit`
  files, `world.meta`, inventory and screenshots are never eviction candidates.
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

- 4 cores: first visible and end improve by at least 25%, with no
  cache-attributable frame over 33 ms.
- 6 cores: load improves by at least 15%.
- 16 threads: end is no worse than the current hold (~4.79 s) and first visible
  is no worse; hit rate is at least 90% of the fill's installs.
- On one worker, a hit's read + decode is cheaper than the generation it
  replaces. If it is not, stop: the cache is moving CPU to disk, not removing
  work.
- If 16 threads regress or the 4-core gain is under 10%, reject the cache and
  record the measured row.

Tests: round-trip all-uniform, all-air and mixed sections; CRC corruption,
truncated pack, wrong key, unknown block IDs, count/offset overflow, empty and
edited chunks; key changes for each config input; a concurrency test for a
reader, writer and `clear()`. Keep the files under the 500-line cap by splitting
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
3. **Writer.** Post-fill snapshot, 8x8 pack writes, atomic manifest, cap and
   LRU, reset. A second boot must be a hit; a crash mid-write must leave a
   usable partial cache; no edit may be lost or baked into the base.
4. **A/B and decision.** The 16/6/4-core matrix, the criteria above, and the
   decision row. If it loses, stop here.
5. **Only if it wins.** Optional polish: a per-world cap control, a prefetch of
   the next pack, or an optional light cache. Each is its own measured change;
   none is part of the default.

## Open questions

- Is 4-core or Steam Deck-class hardware a target? That is the case where the
  cache pays; on a 16-thread desktop it may not.
- Is ~180 MB per world acceptable? A smaller spawn radius or a stronger pack
  codec changes the answer.
- Should the first-run write happen during play, on a menu, or only on quit?
  Quit-only means many players never get a cache; during play needs a budget.
- Per world or shared by cache key? Per world is simpler; sharing a seed could
  reuse the base, but edits and world directories complicate it.
- Is spawn enough? Start there; a cache of all exploration is unbounded and is
  a different design.
- Does light become the remaining cost after the block cache? Measure before
  adding a light cache; light is derived, and its invalidation surface is wider.
