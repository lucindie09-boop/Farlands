# Architecture

This document describes the current, stable architecture of the voxel engine. For historical context, progress tracking, and resolved issues, see [AGENTS.md](AGENTS.md).

## Core Constants

- **Chunk size**: 32×32×32 blocks (`CHUNK_WIDTH`, `CHUNK_HEIGHT`, `CHUNK_DEPTH`)
- **World height**: 1024 blocks (`WORLD_HEIGHT_Y`)
- **Section size**: 16³ blocks (8 sections per chunk)
- **Shards**: 64 shards for chunk map locking

## Threading Model

### Thread Pool
- Single shared worker pool with `hardware_concurrency() - 1` threads
- High-priority queue for critical operations
- No split generation/mesh pools (split approach was tried and reverted due to throughput starvation)

### Locking Hierarchy

**ChunkMap Sharded Locking:**
- 64 shards, each with its own `shared_mutex` + `unordered_map`
- Hash: `key % 64`
- Lock types:
  - `ShardLock` (shared, `std::shared_lock`)
  - `ExclusiveShardLock` (exclusive, `std::unique_lock`)
  - `lock_chunk()` - single shard
  - `lock_keys()` / `lock_keys_exclusive()` - multiple shards in ascending order (deadlock-safe)
  - `lock_all()` / `lock_all_exclusive()` - all 64 shards

**Locking Rules:**
1. Single-chunk/block accessors lock only their own shard
2. Batch methods lock all relevant shards in ascending order to avoid deadlock
3. Hot paths with many sequential reads (light propagation, dirty-neighbor checks) batch-lock instead of per-call locking
4. **ChunkData writes**: Must hold `lock_all_exclusive()` or `lock_keys_exclusive()` for targeted shards
5. **`_locked` methods**: Caller MUST already hold exclusive lock, uses `_fast` accessors only, MUST NOT call auto-locking methods
6. **Auto-locking methods** (`get_chunk_data`, `get_chunk_render_data`, `mark_chunks_dirty_for_light`, `queue_dirty_chunk`): Acquire their own shared locks — MUST NOT be called under exclusive lock
7. **Public wrappers**: Acquire exclusive lock → call `_locked` → release lock → call auto-locking accessors for dirty-marking
8. **No recursive shared acquisition**: Never re-acquire a shard lock you already hold shared. Windows SRW locks block new shared acquisitions once a writer is queued on a shard, so this deadlocks whenever a worker is waiting exclusive. Use `queue_dirty_chunk_fast()` (dirty-queue under a caller-held lock) inside `lock_keys` scopes, and `ShardLock::reset()` before re-acquiring a periodically-refreshed lock (e.g. `BlockEditor::raycast` re-locks `lock_all()` every 8 DDA steps)

**Targeted Shard Locking:**
- `lock_keys_exclusive<N>()` locks only shards whose keys appear in input, in ascending shard order
- Used in all hot paths to reduce contention:
  - `set_block_variant()` — 1 chunk key
  - `propagate_block_light_region()` — 27 keys (3×3×3 neighborhood)
  - `place_block` — 27 keys (3×3×3 center)
  - `light_propagate_add` / `light_propagate_remove` — origin 3×3×3 + each seed node's 3×3×3 (deduplicated)
  - `update_block_light_incremental` — 54 keys (origin + center 3×3×3)
  - `PlayerLight::update` — vector of up to 54 keys (old+new chunk 3×3×3)
  - `MeshBuildTask::execute` — 27 keys (center + 26 neighbors), **shared** `lock_keys` held for the whole data read, serializing the build against exclusive writers

**BFS Bounded Reach:**
- Max light level 15 < chunk size 32
- 3×3×3 neighborhood (27 keys) covers all BFS paths from a single seed chunk

## Chunk Lifecycle

### Frame Pipeline

Every frame the main thread walks a wall-clock-budgeted pipeline that turns a missing chunk into on-GPU geometry (Godot `_process` → `VoxelEngineController` → `WorldUpdater::update`). Heavy stages are handed to the shared worker pool; the `ChunkScheduler` queues between stages decouple them, so a slow worker never blocks a frame.

```
 ONE CHUNK'S JOURNEY:  generate → light → mesh → upload
 (each stage is wall-clock-budgeted; worker pool = hardware_concurrency()-1 threads)

   STAGE 1 · GENERATE ── runs on WORKER
     ChunkGenerator::generate_chunk             (thread_local generator)
       · density field, biomes, vegetation
       · edit maps layered on top
       · sky light propagated in-worker (reads chunk above)
             │
             ▼  completed_chunks           (queue)
   STAGE 2 · LIGHT + INSTALL ── runs on MAIN thread
     ChunkWorld::process_completed_chunks        (budgeted)
       · installs chunk into ChunkMap, queues neighbor remeshes
       · emissive blocks in 3×3×3? → worker task BlockLightRegion::propagate_additive
       · result polled back on main (completed_light_propagations),
         then chunk + rim neighbors marked dirty for remesh
             │
             ▼  dirty mesh queue
   STAGE 3 · MESH ── dispatched on MAIN, built on WORKER
     MeshManager::process_queue → enqueue MeshBuildTask
     MeshBuildTask::execute                      (worker)
       · shared 27-key lock (center + 26 neighbors) held for the whole read
       · greedy meshing, or incremental ~3³ re-emit for block edits
             │
             ▼  completed_meshes           (queue)
   STAGE 4 · UPLOAD ── runs on MAIN thread
     MeshManager::process_completed_meshes       (budgeted)
       · nearest-first poll (stale epoch completions dropped)
       · vertex/light arrays → RenderingServer → GPU
```

Async persistence shares the same pool: the main thread snapshots dirty chunks on a 5s timer and workers RLE-encode + atomically write them; generation-gated saves abort in-flight superseded workers.

### Generation
1. `ChunkWorld::generate_chunk()` checks if chunk exists in map
2. If not found, generates via `ChunkGenerator` (chunks are never stored whole; sparse `EditMap`s are layered on top)
3. Inserts into `ChunkMap` with `ChunkRenderData` wrapper
4. Queues for mesh build via `ChunkScheduler`

### Mesh Building
1. `MeshBuilder::build_mesh()` creates mesh data from `ChunkData`
2. Uses `ChunkNeighborAccessor` for 26 neighbor chunks
3. Greedy meshing with stride/detail reduction for LOD (controlled by `lod_distance`/`lod_detail_level`)
4. The build holds a shared `lock_keys` over the center chunk + 26 neighbors for the whole data read. This both pins the chunks (erasure requires an exclusive lock on the shard) and serializes the build against exclusive writers (block edits, light region recomputes, player light) that mutate neighbor section palettes mid-build. The center chunk additionally carries `pending_mesh_builds`, which `try_unload_chunk` checks before erasing
5. Block edits take the incremental path (`build_mesh_incremental()`): a tight dirty-AABB re-emit merged with the previously emitted mesh, with fallback to a full rebuild when the bounds grow beyond a threshold

### Unloading
1. `try_unload_chunk()` checks if chunk can be unloaded (no pending mesh builds, not in frustum)
2. Snapshots the chunk data and hands it to the background save queue (superseding any in-flight save of the same chunk) — no blocking file I/O on the main thread
3. Removes from `ChunkMap`

### Persistence
- **Save format v3**: `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32][RLE body...]`
- **Atomic writes**: Write to `.tmp` file → create `.bak` backup of existing → atomic rename to target
- **CRC recovery**: On CRC mismatch, attempt to load from `.bak` backup; delete corrupted files if no valid backup
- Supports v3 (CRC32), v2 (legacy RLE), v1 (flat legacy) transparently
- **Async saves**: `flush_dirty_chunks(wait_for_completion, timeout_sec)` deep-copies each dirty chunk under its shard lock (cheap — 1–20KB palette-compressed data), clears the dirty flag at snapshot time, and hands the snapshot to the thread pool for RLE encode + write. All file I/O is serialized by `file_access_mutex`. `WorldUpdater::flush_dirty()` triggers a periodic non-blocking flush every 5s (`flush_interval`).
- **Generation gating**: each in-flight save carries a per-key generation from `next_save_generation`; a newer snapshot for the same chunk bumps the generation, and the superseded worker aborts at its gate (checked under `file_access_mutex`) instead of clobbering newer data. An epoch gate additionally drops stale saves after a world reset. This guarantees the newest state is always the last one written, with at most one live write per chunk file.
- **Flush on quit**: `ChunkManager::_exit_tree()` calls `flush_dirty_chunks(true, 5.0)`, which blocks until all outstanding saves finish so recent edits are never lost on exit.
- **Inventory persistence**: `Inventory` serializes to `user://chunks/inventory.bin` (magic `INVE`, version 1, hotbar + 27 main slots + selected slot). `PlayerController::_exit_tree()` saves it while nodes are still alive (the destructor's tree lookups always failed at teardown).
- **Cross-chunk writes**: `apply_pending_placements()` marks the receiving chunk dirty, so deferred vegetation canopy writes to a neighbor chunk are persisted by the next flush.

## Memory Layout

### PaletteStorage
- 8 sections per chunk, each 16³ blocks
- Uniform sections (all air, all stone) cost only a palette entry
- Non-uniform sections use 4/8/16-bit indices as needed
- Per-chunk memory: ~1–20KB on typical terrain (vs ~130KB with dense layout)
- Block and light storage share generic `PalSection`/`section_get`/`section_set` machinery

### Block Definitions
- **Single source of truth**: `data/block_definitions.json`
- C++ `BlockRegistry::load_from_json()` reads from JSON
- Texture/emissive arrays generated from same file
- Do not hardcode block properties in C++

### Block Shapes
- **Non-full block shapes**: Slabs, stairs, walls, and poles defined in `data/block_shapes.json`. A shape is an arbitrary list of AABBs, so a model is data rather than code: the crucible is 9 boxes (4 legs, a floor plate, 4 open-topped walls). `collision_boxes` is optional and defaults to the visible boxes, so the crucible's walls stop a body while its open top lets one stand inside on the inner floor plate; the pole is the counter-example that overrides it (1.5-high collision under a 1-block-tall stick). A `shape` name that does not resolve leaves the block a full cube (error log only), and per-AABB emission never emits Bottom faces
- **Selection and collision boxes**: Each shape variant has explicit selection boxes (for raycast) and collision boxes (for physics)
- **Auto-detecting placement**: Slabs, stairs, and walls automatically orient based on clicked face and neighboring blocks
- **Double-slab merging**: Two stacked slabs of the same type merge into a double slab; breaking drops 2 slabs
- **Pole collision**: Fence-like collision boxes extend 1.5 blocks high for proper player interaction

## Terrain Generation

- **Signed 3D density field** over a macro heightmap: `density = (surface_y - y) + shape_noise * strength * surface_band` (positive = solid). The 3D fBm deforms only a band around the macro surface, producing overhangs, shelves, and arches.
- **4×4×4 world-aligned shape lattice**: the 3D shape noise is sampled once per lattice node and trilinearly interpolated per voxel, so chunk grids and single-point field queries stay bit-identical and lattice nodes land on shared world coordinates across chunk boundaries (no seams).
- **Chunk-level fast paths**: after the exact macro column pass, chunks entirely above/below the per-chunk height band fill plain water/air or stone over bedrock and skip all lattice/density/material work (~7× on deep-chunk generation). The height-band estimate runs on the cached lattices (81 macro/climate nodes + the blended-amplification lattice, combined per cell into rigorous min/max bounds) instead of per-column queries — ~2× cheaper and denser than the old 5-corner samples, and it covers every column.
- **Per-chunk macro-surface lattice**: the macro land height is evaluated once per 4-block node (81 evaluations per chunk) and bilinearly interpolated per column — the same lattice idiom as the climate fields — instead of 4 corner evaluations per column (~4096). Node values and interpolation are bit-identical to the per-call sampler, and the single-point query paths reuse the same lattice for the fast-path estimates.

Terrain is produced in three stages — a macro surface from stacked noise layers, a height-based water post-pass, then a strength-gated 3D density field wrapped around that surface:

```
 MACRO SURFACE ── noise layers summed at a domain-warped sample point
 (sampled once per 4×4 world-aligned lattice node, bilinearly interpolated
  per column; one continuous field, so chunk seams cannot appear)

    domain warp   ~500-block field, two recursive octaves (amps 18/30/10/16)
    layers summed at the warped point:
      base       12,000-block octave    height_base_y (512) ± 500
      detail     ~1,000-block octave    ± 100
      flow       ~300-block ridged fBm  × 16 (3 octaves)
      mid-relief   ~500-block lattice field   (amplitude 90,  8-block nodes)
      small-relief ~150-block lattice field   (amplitude 25,  2-block nodes)
             │
             ▼  per-column macro height
 BIOME-FIRST / OCEAN-LAST ── the climate grid picks a land biome
      (Plains/Hills) on every column, then that biome's height knob shapes
      the column around sea level. Columns that still end below
      sea_level (200) are ocean LAST: biome → Ocean, water fills to sea
      level, and the sea bed keeps the height the land biome gave it
             │
             ▼
 WEIRDNESS MASK ── 2D fBm with ~42-block lobes, smoothstepped 0.10 → 0.75
      (most of the world at minimum strength; only the tail of each lobe
       ramps up)   →   strength = lerp(5, 50, weirdness)
             │
             ▼
 3D DENSITY ── density = (macro_height − y) + shape · strength · surface_band
      shape = 3-octave fBm (~38-block features, y ×1.35, ~250-block
              horizontal domain warp) on a 4³ world lattice, trilinearly
              interpolated per voxel
      surface_band fades the shaping fully out 9 → 28 blocks from the macro
      surface (so the 3D field can never leave density_margin() of it — 30
      at the neutral weirdness_size 1.0, grown by the widest biome's knob)
             │
             ▼
 PER-CHUNK MATERIALIZATION (32³ chunk, three passes)
      1  macro columns + weirdness, chunk-level fast paths:
           chunk entirely above terrain → air + per-column water surface
           chunk entirely below terrain → stone over bedrock, no density work
      2  density lattice pass → material pass: biome topsoil only near the
         macro surface, stone below (bedrock 0-5)
      3  cleanup: isolated-voxel removal, thin solid-sheet fix, water
         flood-fill below the water surface; vegetation on the real
         density surface, rejected underwater
```

- **Macro surface layers**: all tuning is data-driven (`data/terrain_config.json` → `TerrainParams`); the layer defaults are `height_base_y` 512, a ±500-block 12,000-block octave, ±100-block detail, ×16 ridged flow, plus the two bilinearly-lerped relief fields (mid ~500-block / amplitude 90 on 8-block nodes, small ~150-block / amplitude 25 on 2-block nodes). Domain-warp amplitudes (18/30/10/16) and frequencies are the flowing-ridge dials.
- **Biomes first, oceans last**: the 3×3 climate grid selects a land biome (Plains/Hills) on every column and that biome's amplification knobs shape the terrain first. Only after that shaping does height decide water: any column still below `sea_level` (200) becomes **Ocean** — the biome switches to the ocean set (water fills to sea level, sand surfaces) while the sea bed keeps the height the land biome gave it. Plains (height amp < 1) basins therefore shelf out shallow near coasts while Hills (amp ~1) basins drop steeply, and coasts stay seamless by construction. Shoreline material swaps come from a per-chunk 2-pass Manhattan distance transform seeded from the *actual* density surface (not the macro heightmap).
- **Weirdness gating**: a 2D fBm (`weirdness_scale` 0.024 → ~42-block lobes) through `smoothstep(0.10, 0.75)` picks where the 3D shaping is strong: `strength = lerp(shape_strength_min 5, shape_strength_max 50, weirdness)`.
- **Biome selection**: temperature/humidity samplers are live low-frequency 2D fields (~8000-block features at default `biome_size` 1, scales `climate_*_base_scale` in `data/terrain_config.json`), read through a recursive anisotropic domain warp (`climate_warp_amp_x1/z1/x2/z2`, same scheme as the macro height warp) so biome boundaries flow and meander instead of drawing smooth lines, and sampled on a 4-block world-aligned lattice with bilinear interpolation (same lattice idiom as the macro height field), so every chunk reads the same global nodes — no seams. The 3×3 climate grid maps the temperate (neutral-temperature) band to **Plains** and the cold/hot bands to **Hills**. **Continentalness** — the 12000-block base layer of the macro height stack, normalized to [0,1] — is now sampled live and carried in every column sample (`ColumnSample::cont`), but selection never gates on it: oceans are the final below-sea stage above, so a column's cont is what future profile-based selection will compare against each biome's `preferred_continentalness` (reference data in `data/biomes.json`). Worldgen emits **Plains**/**Hills** on land and **Ocean** below sea level. The `BiomeConfig` tables (surfaces + tree variants from `data/biomes.json`) drive per-biome surface blocks and vegetation weights.

Per-biome amplification knobs (`height`/`weirdness`/`min_weirdness`/`weirdness_size`) are **blended across biome borders** (`weirdness_size` scales both halves of the 3D-shape envelope — strength and surface band — and drives `ChunkGenerator::density_margin()`, which every height range is padded by): each 4-block lattice node's effective knobs are the arithmetic mean over the biome nodes within `climate_blend_radius_nodes` (2 nodes = 8 blocks at the default, in `data/terrain_config.json`), which ramps the knobs linearly from one biome's plateau to the next across the whole window (each node of radius ≈ 4 blocks of transition per side). An inverse-distance kernel was tried first but front-loaded the transition into the two lattice cells touching the border, leaving a sharp height step no matter how large the radius — the uniform mean is what makes the band genuinely widen with the radius. Radius 0 disables blending entirely: every column uses its own biome's knobs exactly, so a border is a clean step with no lattice smear. The blend is evaluated on the climate lattice (cached per chunk, matching the single-point path to within float rounding). Each node's window mean uses a separable 2D prefix pass over the per-chunk biome grid, so the per-node window cost is O(1) regardless of radius (cap `CLIMATE_BLEND_MAX_RADIUS = 20`, ≈160-block full band); at large radii the remaining cost is classifying the wider biome halo once per chunk, not per window. The blend field is climate-derived and never contains ocean, so ocean columns keep the ocean biome's own knobs for 3D shaping — but their macro seabed height comes from the land biome's (blended) knob applied before the ocean override.
- Vegetation uses the real density surface with an underwater rejection guard; an isolated-singleton removal pass clears lone floating voxels the density field occasionally produces, and thin-solid-sheet/water-flood-fill cleanup keeps underwater columns clean.

## Rendering

### Frustum-Prioritized Loading
- `Camera3D::get_frustum()` provides 6 world-space planes
- `DirtyChunkEntry` priority: `urgent > in_frustum > dist_sq`
- Two-phase generation: dedicated frustum-priority pass first, then distance-sorted sweep
- Dynamic mesh budget: visible-chunk ratio scales budget from 0.5× (sparse) to 1.0× (full)

### LOD System
- Per-chunk distance-based reduction (not chunk merging), in three tiers:
  1. **Full detail** within `lod_distance` (+1)
  2. **Mid tier**: stride/detail reduction controlled by `lod_detail_level` inside the greedy mesher
  3. **Far tier**: identical stride/detail mechanism, with its own render start `far_lod_distance` and detail `far_lod_detail_level`; far-detail chunks are upgraded/downgraded via the same reprioritize transition-shell + tracked-set logic
- LOD-reduced chunks (detail < 1.0) are cached (`far_mesh_cache`) and merged into 8×8-chunk **far regions** — the coarse rings render as a handful of region instances instead of one per-chunk instance, keeping draw calls low
- Per-tier stride-1 "skirt" rings at each LOD transition prevent T-junction cracks
- Cap of 512 LOD remeshes/frame; far-region rebuilds debounced (250 ms)

### Mesh Completion
- `process_completed_meshes` is wall-clock-budgeted (`mesh_completion_budget_ms` = 0.75) plus a per-frame completion cap, so a backlog can never stall one frame
- Nearest-first scheduling: `ChunkScheduler::poll_completed_mesh_nearest()` scans both completion queues (backed by `std::deque`) for the chunk closest to the player; stale completions (epoch mismatch) are dropped during the scan and the frame's uploads go to the most visible chunks

### Upload dedup and the liquid invariant
- Upload deduplication compares a content hash against the last upload, so an unchanged chunk never re-uploads. That hash **covers both surfaces** (`src/mesh/mesh_content_hash.hpp`): the opaque and the liquid mesh go up in one `mesh_add_surface_from_arrays` pair, so hashing the opaque one alone skipped the whole upload whenever only the water changed — which is the common case, a liquid being transparent (same blocks, same light). The failure mode is water that exists in the world, collides and outlines, and has no geometry on screen until an unrelated edit moves an opaque vertex. Pinned by `tests/test_liquid_mesh_geometry.cpp`
- The renderer checks the invariant in the other direction too, from data to GPU: at every applied mesh, a chunk whose data holds liquid (`ChunkData::liquid_count`, maintained beside `block_count` on every write path) and whose opaque geometry is on the GPU must have liquid geometry on the GPU as well. If it does not, the chunk is remeshed once (guarded per `mesh_version`) and counted (`note_liquid_geometry`, mesh_manager_upload.cpp)
- The performance report carries those numbers every interval, because a water drop is otherwise unobservable from inside the game: `Liquid missing` (chunks holding liquid with no liquid geometry, 0 expected), `Liquid repairs` (remeshes spent repairing that), `Mesh uploads / Dedup skips / Water-only skips` (a skip that carried a different water mesh — must be 0), and `Unrendered` (geometry in range, not covered by a far region, and no instance drawing it — must be 0)

### Mesh Surfaces
- Primary surface: opaque terrain with greedy meshing
- Secondary surface: translucent water with edge fade, tint, shimmer, flowing texture animation, and separate blend-mix surface. Its geometry is the per-corner surface from `mesh_fluid.hpp` / `mesh_builder_fluid.cpp` (a liquid cell's four corners take independent heights, so a pool's rim slopes and a waterfall's sides reach the cell boundary), which is why liquids are excluded from the greedy and per-AABB emitters — one merged quad carries one top height, and that is what used to render every pool as a stepped box
- Emissive textures: second `Texture2DArray` for per-face glow maps
- Far regions: LOD-reduced chunk meshes merged into region instances (`far_regions`, `far_mesh_cache` in `mesh_manager.*`/`chunk_render_data.hpp`) so the coarse ring costs a handful of draw calls
- Vertex compression: 24 bytes per vertex (-40% VRAM) with fixed-point positions

### Procedural liquid textures (`src/render/liquid_texture.hpp`)
- Animated water/lava/acid sprites are generated, not drawn: a three-field cellular automaton over an N×N grid of floats (`surface` = the visible height that becomes the pixel colour, `flow` = momentum the surface pushes into, `surge` = a decaying energy source re-ignited by a per-cell dice roll) stepped once per frame and mapped through a colour ramp. That is how the classic block game produced its still-water and still-lava sprites before textures became image files; the field names, kernels, defaults and colours here are ours (see the file header)
- Three kernels (`row` 3×1, `box` 3×3, `warp` 3×3 with a sine-wandering window, `plus` 5-point) and per-style presets. **The divisor must exceed the number of cells the kernel sums** (row→3.3, box→9.9) or the surface amplifies itself every step until a frame is one flat ramp stop and the animation stops animating; the suite pins both the flattening and the field-inside-the-ramp calibration of every preset
- The surge is deliberately not floored at zero: it settles at a small negative mean, which is what stops `flow` (which *is* floored) from integrating forever
- Output is one vertical strip, `resolution` wide and `resolution * frames` tall, frames stacked downward — the shape the classic resource-pack format used. `interpolate` emits cross-faded sub-frames that are blended in field space before the ramp, so a blend never invents palette entries
- **Looping**: the automaton is not periodic, so a strip's last frame does not naturally sit next to its first and the wrap pops. With `loop` on (the default) the last `loop_window` frames are morphed in field space into the run that led *into* frame 0, ending exactly on it: the last frame **is** the first frame, and the step into it is a normal-sized frame change. Measured on the shipped water preset at 16 frames: the seam step is 5 and the worst step inside the strip is 6, where the wrap used to be 36. A window of 1 is the bare duplicate (it just moves the pop one frame earlier), which is why the default is 8 — about the kernels' own decorrelation time. `Strip::looped` reports this, and a looping player must advance from the last frame to **index 1**, not 0: index 0 already played as the last frame, so wrapping to it would hold the image for two frame times. The lab does that in `_advance()`, and sub-frames after the last one blend towards index 1 for the same reason
- `LiquidTextureGen` (static binding) is the GDScript face: settings travel as a Dictionary whose keys are the field names, `default_settings(style, resolution)` returns a complete one, `describe()` echoes the clamped values the generator would really use, and `generate_strip()` returns the `Image`
- Live preview writes frames into the world's texture-array layer (`ChunkManager::push_texture_frame` → `Texture2DArray::update_layer`), which is why `TextureArrayGenerator::find_texture_layer()` exists separately from `get_texture_index()`: the latter answers 0 for "unknown", i.e. the fallback layer, so a writer using it would repaint stone. A GPU-compressed array cannot take a raw RGBA frame, so the lab rebuilds uncompressed for the duration of the preview and restores the user's setting afterwards
- `ChunkManager::fit_texture_frame()` is the verifiable half of that path (RGBA8, snapped to the array's resolution, mipmaps matched to the generator's flag). `get_texture_layer_image()` is best effort: Godot 4.7's `TextureLayered::get_layer_data()` answers null even for an array built in the same process, so nothing depends on it
- **It runs in the world, not only in the tool.** `liquid_animator.gd` (autoload) plays each liquid's strip into its texture-array layer whenever the game is running: `user://liquids/<liquid>.json` if the Lab bound one, else the built-in preset, regenerated in C++ from those settings (deterministic, so the JSON is the whole source and the saved PNG is never read). Frames advance every `frame_time` ticks with the panel's rule (a looping strip wraps to index 1). Compression is off for the session while it animates, the Lab's Live pauses it per liquid, and `get_status()` exposes source/frames/frame/pushed per liquid, which is what a probe can hold to account. A liquid's still texture is baked from its own preset by `tools/bake_liquid_textures.gd` — needed, because no PNG means no layer to write into
- `tests/test_liquid_texture.cpp` covers the automaton (kernel wrapping, torus translation equivariance, flattening, scroll translation, determinism, interpolation, grain, posterization, clamping, ramp calibration); `.freebuff/probe_liquid_lab.gd` drives the real tool against the real world and the real texture array, and `.freebuff/probe_lava_acid.gd` drives the animator (frames landing, the compression refusal, the Lab's Bind handoff) plus lava's and acid's blocks, buckets and floods

### Block file import (`src/schematic/`)
- **The file formats are read, never written, and decoded without Godot or a compression library.** There are two families, and both decode into one shape: the *classic* file (`.schematic`, and `.nbt` in the same style) is a numbered-id build, and the *palette* file (`.schem`, the newer format) names its states and stores one base-128 varint index per cell — v2 at the root, v3 nested under a `Blocks` compound, the two told apart by that nesting rather than by the version number in the file. A classic file is a gzip'd NBT tree; NBT is tagged and byte-ordered per writer, so `nbt_reader` is a forward cursor (walk what you want, skip the rest — a tile-entity list can be the largest thing in a file and is never wanted) rather than a parsed tree, and `gzip_inflate` provides DEFLATE plus the gzip/zlib/raw framings with their checksums verified and the decompressed size capped. The standalone tools and the test binary link neither the engine nor zlib, and the reader has to run there, which is why the inflater is ours rather than a `FileAccess` call: the same code opens the file in-game and in `bin/schematic_report`
- **Three writer quirks are absorbed rather than assumed away, because each one produces a file that decodes into plausible-but-wrong blocks instead of failing.** The nibble-packed arrays are sometimes written one byte longer than the packed size (accepted and reported, since a length that is neither layout could only be read by guesswork, and guessing the `Data` layout scrambles every oriented block); a build may be wrapped in an unnamed root, so the build compound is located by shape — a `Blocks` array or a palette — and the root's name is reported rather than required; and a palette's indices are compacted on read, with a cell pointing outside its declared palette refused by name. `schematics/14664.schematic` and `schematics/29761.schematic` (a v3 file wearing a `.schematic` name) are both in the tree for exactly these reasons
- **Decoding and meaning are separate steps.** The reader returns a palette of states with one palette index per cell — compact for a large build, and free of any assumption about which of our blocks an id should become. That mapping is data: `mc_palette.hpp/cpp` resolves `(id, data)` against `data/minecraft_blocks.json` into Unknown / Skipped / Mapped (with `substitute` and `fluid` flags and the row's own note), and a state the table has never heard of stays Unknown and is reported rather than guessed at. Rows share orientation conventions through named `variant_sets` with a `{family}` placeholder, so the eight stair orientations are written once and each stair material is one line
- **The table is read by `minimal_json.hpp/cpp`, not `godot::JSON`.** The table has to load in `bin/schematic_report` and in the test binary, neither of which has an engine runtime, so the reader is ours and is the same code in all three places. It accepts the JSON subset a data file uses and refuses the rest with the byte offset (trailing commas, comments, bare keys, duplicate keys, nesting past the cap), and a failed parse leaves the caller's value empty rather than half-built
- **Two arrays ride beside `Blocks`, and one of them has two possible layouts.** `Data` is a 4-bit value per cell, and it is either one byte per cell or nibble-packed two to a byte; the layout is deduced from the array length (cell count vs `(cell count + 1) / 2`) and a file matching neither is refused with both numbers. Assuming one layout silently mis-decodes every oriented block — a per-byte array read as nibbles yields stair facings that look plausible and are wrong. `AddBlocks` carries the high nibble of ids above 255, always nibble-packed
- **Cell order is `(y * Length + z) * Width + x`.** Getting it wrong produces a file that still decodes into a building-shaped thing, so the tests assert every cell of a 4×3×5 fixture against the formula the fixture was written from instead of sampling a few ids
- **The tile-entity and entity lists are counted, not decoded.** Signs, chests and dropped items have no block to become yet; dropping them silently would be a lie, so the counts and the first tile-entity positions are reported and placement ignores them
- **Planning and writing are separate, and the plan is engine-free.** `paste_plan.hpp/cpp` resolves a decoded file through `mc_palette` into world coordinates plus block ids and counts every cell into exactly one bucket (placed, substituted, declined liquid, declined stand-in, skipped, unknown, unresolved, air ignored). Nothing about the world enters that decision except `replace_solid`, which the writer enforces, so the numbers a player is shown are the file's own numbers. The pass is per distinct STATE rather than per cell — the table's reachable names resolve once up front, the table is asked once per palette slot, and the cells are walked flat in their stored order — because a build repeats a few states across a million cells and re-asking per cell costs 1.5× to 3× more (measured: 6.3 → 4.2 ms on a 1.39M-cell file, 3.8 → 1.2 ms on a 353-state `.schem`). The write itself is `BlockEditor::apply_paste`: one exclusive 3×3×3 band per chunk (not per block), sky light recomputed per touched column, one relight and dirty per touched chunk, every cell persisted through the edit map (which also wakes pasted fluid), and the displaced blocks kept as a one-level undo. It cannot go through `BlockEditor::place_block`, which refuses any cell where the old and new blocks are both non-air and would therefore drop every cell of a build that overlaps terrain. `ChunkManager.inspect_schematic` is the read-only half (decode + plan, no write) and is what a preview or a size query uses; `/paste` is the chat command over the writing half

## Collision

- Binary-search AABB approach (3D DDA variant was tried and reverted)
- Custom voxel collision queries chunk map directly instead of Godot physics nodes
- Player collision via `ChunkManager::resolve_voxel_collision()`
- **Step-up**: tests the player's full body AABB raised by `step_height`, then re-resolves horizontally and only accepts the step if it travels further than not stepping. (The old `[feet, feet+step_height)` headroom probe always contained the obstruction being stepped onto and could never succeed.)

## Player Controller

Two layers mirroring the ChunkManager/VoxelEngineController pattern:

- **`VoxelEngine::PlayerSim`** (`src/engine/player_controller.*`) — pure, deterministic fixed-timestep simulation (20 ticks/s, velocities in blocks/tick). Vanilla-accurate ordering (jump + sprint boost applied before friction, matching the vanilla tick order), sticky sprint with a one-tick airborne staleness, sneak multiplier on ground only, and `on_floor` derived from the final resolved position (no swept floor probe). Standing/sneaking/landing eye-height transitions are smoothed at render time from the accumulator's partial-tick fraction. Also tracks fall distance from per-tick position deltas; a landing past `SAFE_FALL_DISTANCE` (3 blocks) queues `floor(distance − 3)` half-hearts for `consume_pending_fall_damage()`.
- **`PlayerController`** (`src/godot_bindings/player_controller.*`) — Godot `Node3D` scene node registered via ClassDB (used directly by `Main.tscn`). Polls input, drives the tick accumulator from `_process(delta)`, handles mouse look (pitch clamped ±90°), fly mode (`fly_speed`), camera eye-height smoothing, raycast-based break/place, block selection (keys 1–9), and the **F5 three-view camera cycle** (`third_person_view_` 0/1/2: first person → behind → in front). Left-click punches the K-key pose-clone dummy when it's under the crosshair (`try_punch_dummy`, 3.0-block reach, vanilla 1.8.8 knockback with sprint bonus + 10-tick hurt-resistance gate; mining is suppressed while the dummy blocks the aim). After ticking it consumes queued fall damage into a clamped `health` property (0–20 half-hearts, `get_health`/`set_health` bindings).
- **Third-person camera & aiming** — The back/front cameras sit on the player's look ray at `kThirdPersonOffset` (4.0) blocks and are pulled in before any solid block by `camera_clear_distance` (0.25-block samples through `CollisionResolver::is_solid_at`, min 0.25) so they never clip terrain. The camera rotation is the player's look rotation directly (front view mirrors: `(−pitch, π)`) rather than an aim-at-player direction — aim-derived rotations locked up near vertical, where the horizontal component of the aim vector vanishes. Block targeting is view-independent: the bound `get_aim_origin()`/`get_aim_direction()` cast from the player's eye along the look ray (vanilla's eye-ray trace), and `ChunkManager::raycast_from_camera` prefers them over the camera (falling back for scenes without a controller), so first/back/front views always aim at the same block and the crosshair/outline agree.
- **Third-person body & head look** — The visual body is reparented under a runtime **`ModelPivot`** wrapper in `_ready` so it can lag behind the look: the torso eases toward the horizontal travel direction at 0.3 of the gap per 20 Hz tick (moving, including midair) and holds while standing, clamped so the head can lead the body by at most ±35° (`kBodyMaxYaw`) before dragging it — vanilla's body-yaw model. `player_model.gd`'s `_track_head_look()` points the `head` mesh at the player's **aim** (controller world yaw + pitch), never the camera — the front-view camera is yaw-flipped 180°, so camera-tracking spun the head around. The model's x/z is centered on the player (head rotation axis on the eye axis), and the glb node pivots are baked onto the true joints (see `tools/rebake_player_pivots.py`), so the head tilts around the neck like `vanilla model`.
- **Health integration** — `healthbar.gd` renders 10 hearts (`heart_full/half/empty.png`, 9×9 art) above the hotbar's left edge, sized off the hotbar's on-screen width so the row spans ~40% of it (9-texel sprites on a 10-texel pitch). It polls `get_health()` each frame and redraws only on change. Hearts are linear-filtered: the row's fractional scale makes nearest sampling render 1-texel outlines at inconsistent widths.
- **Death & respawn** — `set_health` hitting 0 calls `die()`: a `dead_` flag freezes `_process`/`_input` (movement, look, break/place, hotbar keys), `update_mouse_mode()` releases the cursor, and the `died` signal fires. `respawn()` restores full health, teleports to the spawn point captured in `_ready` (via `teleport_to`, clearing fall state), and emits `respawned`. `death_screen.gd` (HUD overlay) listens to both signals and shows/hides a "You died!" + Respawn button screen; the button calls `respawn()`. Inventory is kept on death.
- **Inventory integration** — `PlayerController` owns a `VoxelEngine::Inventory` (9 hotbar + 27 main slots, 64 stack limit). Breaking a block collects it only if `can_add_block` succeeds; placing consumes from the selected hotbar slot. Hotbar/inventory state is exposed to GDScript via ClassDB bindings (`get_hotbar_slot_block_id`, `set_inventory_slot`, `select_hotbar_slot`, etc.) and rendered by the `hotbar.gd` / `inventory.gd` `Control` overlays (E toggles, mouse wheel cycles the hotbar, click-to-hold/drag-drop stack movement). The inventory UI uses isometric 3D block icons (300×300) rendered by `BlockIconRenderer` at vanilla's dimetric angle (45° yaw, 30° pitch) with support for custom block shapes (slabs, stairs, walls, poles) built from `data/block_shapes.json` selection boxes. Icons are pre-rendered asynchronously at startup and cached for performance.
- **Crafting integration** — `RecipeBook` (`src/core/crafting.*`) loads recipes from `data/recipes.json` in `load_world_configs()` and is exposed through two ClassDB bindings: `match_recipe(grid_ids, grid_counts)` previews the output slot (gated on ingredient availability so the preview disappears once the grid runs dry), and `craft_recipe(grid_ids, grid_counts)` atomically verifies + deducts the grid and returns the new counts plus the result. The grid dimension comes from the cell count (4 → 2×2, 9 → 3×3), so both the inventory's 2×2 grid (`inventory.gd`) and the crafting table's 3×3 grid (`crafting_table_menu.gd`) use the same RecipeBook; both live GUI-side and persist across open/close. Crafting-area geometry is measured from the color-coded slot pixels in the atlas (`#7e7d7e` inputs, `#7e7d7f` output vs. `#7e7d7d` regular slots).
- **Chat integration** — `PlayerController` provides chat state management (`set_chat_open`, `is_chat_open`) and inventory clearing (`clear_inventory`). The chat system (`chat.gd`) features advanced autocomplete with ghost text suggestions, tab cycling, and parameter hints.
- **Lifecycle hooks** — `PlayerController::_ready()` caches the `ChunkManager` pointer (no per-call tree lookups) and loads the saved inventory; `_exit_tree()` saves the inventory while every node is still allocated, guarded by `inventory_saved_` so the destructor's fallback save is a no-op.
- **Viewmodel & animation** — First-person hand + held item/block live in thin `viewmodel.gd` glue (child of `Camera3D`, eye space), while the per-frame math and mesh geometry are native. `ViewmodelPose` (`src/core/viewmodel_math.*`) owns the walk bob (`step_walk_bob`), mouse sway (`step_sway`), punch/place swing + equip pose (`compute_swing_pose`), held item/block swing transform (`compute_swing_transform`), and `smoothstep_01`. `ViewmodelMeshes` (`src/core/viewmodel_meshes.*`) builds the held-block cube, shaped-block selection boxes, and extruded-sprite item meshes (also reused by `block_break_overlay.gd`, `block_preview.gd`, and the block gallery). The punch (0.225s) drives an arm depth curve reshaped by a cubic smoothstep plus a two-sided circular arc sweep; it **loops while breaking** (`get_break_state()["active"]`) and is gated on captured mouse so UI clicks never swing. A separate weaker **place animation** (75% endpoint) fires only on the C++ `block_placed` signal (verified land + inventory consumed). Walk bobbing (`_walk_dist*PI*0.6`) uses a `_bob` envelope that decays to zero when airborne, driven by the `PlayerController::is_on_floor()` binding (wraps `sim_.is_on_floor()`). Peak pose constants (`PEAK_ROT`/`PEAK_POS`, etc.) stay in `viewmodel.gd` and are passed in as Vector3 pairs so they remain the single source of truth.
- **Block break progress** — `update_break_progress` accumulates `delta * tool_speed / hardness` while LMB is held on the raycast target, where `tool_speed` comes from `mining_speed_multiplier()` (`src/core/mining.hpp`) against the block's `preferred_tool`/`min_tier` (plus a `hammer` against any block carrying a `crush_result`); releasing LMB or losing the target resets `break_progress_`/`break_target_valid_` (so the crack and looping punch stop). `get_break_state()` exposes `{active, x/y/z, stage 0-9}` to `block_break_overlay.gd`. The same pass resolves the drop through `resolve_block_drop()`, so the inventory gate and `break_block()` agree on what a break yields.
- **Hammer crushing** — a block's optional `crush_result` (`data/block_definitions.json`, resolved to an id in a `load_from_json` post-pass) is the hammer's whole contract: a hammer-class tool is fast against any block that names one, and `resolve_block_drop()` yields that block instead of the broken one. Crushing is keyed on the block actually broken and is not tier-gated (tier only scales speed). Cobblestone → gravel, gravel → sand.
- **Block drops** — a block's optional `drops` names what it yields when broken, whatever the tool; it is separate from `crush_result` (a crush wins) and both resolve in the same post-pass. Stone → cobblestone, so stone blocks themselves are no longer obtainable by mining.

- **Liquids are passable** — `BlockType::stops_bodies()` (false for any block with the `Liquid` property) is the single answer used by `CollisionResolver::is_aabb_solid_fast`, the sneak edge-guard and the camera's `is_solid_at` clear distance; `chunk_map.is_block_solid()` stays a raw non-air query. A liquid's shape is a surface height, not a wall, so a body falls into water instead of standing on it and the camera looks through it.
- **Water movement** — `PlayerSim::tick` samples the liquid state once per tick at foot and head level (any submerged part of the body counts, so standing on a submerged slab is wet) and, when wet, replaces gravity with a slow sink, horizontal friction with water drag, scales acceleration to 1/5, rises while jump is held, lifts the body when it swims into a wall, and zeroes the tracked fall distance so water landings never hurt. `WATER_*` constants live in `src/engine/player_controller.hpp`; `PlayerController::is_in_water()` exposes the state.

No `CharacterBody3D`, `move_and_slide`, or `CollisionShape3D` — all collision goes through `CollisionResolver` against the chunk map.

## Removed/Experimental Features

The following experimental features were attempted but removed or reverted:

- **2×2×2 LOD group merging**: Replaced with per-chunk three-tier LOD with 8×8 region merging
- **Cloud layer system**: Removed atmospheric cloud layer with fbm noise
- **Lighting preset system**: Reverted Main/Spooky preset system with separate visual sky
- **Occluder boxes**: Reverted Godot occluder boxes for fully-solid chunks
- **Complex biome systems**: Removed Tundra/Taiga/Savanna/StonePlateau biomes in favor of the current JSON system (Ocean/Hills/Plains)
- **Erosion-driven mountains**: Removed experimental mountain generation systems
- **3D DDA collision**: Reverted to binary-search AABB collision
- **11-biome climate system**: Simplified from 11 biomes to the current JSON system (Ocean/Hills/Plains)

## Legacy/Disabled Code

The following code remains in the codebase but is disabled or unused:

- **Cave system**: `kCavesEnabled = false` in `ChunkGenerator` - cave carving code exists but is globally disabled
- **is_occluder() method**: Defined in `ChunkNeighborAccessor` but never called anywhere in the codebase
- **mountain_scale parameter**: Read from save files in persistence but ignored in current terrain generation
- **Generated water is inert**: `surface_water` (what worldgen fills every sea, lake and cave pool with) declares no fluid state, so it never enters the flow simulation. Only water a player pours flows
- **Lava and acid are blocks of their own**: `lava` + `lava_runoff_1..3` + `lava_fallen` and `acid` + `acid_runoff_1..7` + `acid_fallen` in `data/block_definitions.json`, placed by pouring the matching bucket (`lava_bucket`/`acid_bucket` carry the same `"use": {"kind": "pour"}` the water bucket does). They flow by the same rules with their own numbers — lava stops three cells out and creeps a cell a second, acid matches water's reach on a faster clock and is the one substance that does not pool from a pair of sources — and they are meshed by the liquid surface pass because `mesh_fluid::family_of` reads the kind off the block. Their still textures are baked from the generator's own presets by `tools/bake_liquid_textures.gd`

## Key Files

### Core
- `src/core/chunk_data.hpp/cpp` — `PaletteStorage`, `PalSection`, section-based accessors
- `src/core/chunk_map.hpp` — Sharded locking, `lock_keys_exclusive`, auto-locking methods
- `src/core/chunk_coords.hpp` — Constants (`CHUNK_WIDTH`, `SECTION_HEIGHT`, `WORLD_HEIGHT_Y`)
- `src/core/frustum.hpp` — Frustum utility (AABB test, chunk visibility)
- `src/core/block_types.hpp/cpp` — `BlockRegistry`, `load_from_json`
- `src/core/inventory.hpp/cpp` — `Inventory`, `InventorySlot`: hotbar/main storage, add/consume/can_add, 64 stack limit
- `src/core/crafting.hpp/cpp` — `RecipeBook`, `CraftingRecipe`, `craft_item`: shapeless (sorted-multiset) and shaped (bounding-box trim + mirror) matching over an N×N grid; Godot-guarded JSON loader keeps the matching core fuzz/test friendly
- `src/core/item_registry.hpp/cpp` — `ItemRegistry`, `ItemToolStats`, `ItemUseAction`, `ItemLight`: non-placeable inventory objects in their own id space above blocks, loaded from `data/items.json`, with optional per-item tool stats (`class`/`tier`/`speed`), an optional in-world use action (`"use": {"kind", "block"}` — `pour` writes that block into the cell the crosshair is against, `fill` empties the fluid SOURCE the crosshair is on and swaps the item for that fluid's bucket, the two halves of one loop; resolved by name at load since blocks load first), and an optional held light (`"light": {"level", "color"}` — while it is in the selected slot it owns the player's dynamic light, see `PlayerController::update_held_light`). Entry order is the id, so new entries are appended. The file's optional `"pose"` field is read by `viewmodel.gd` (not the registry) since it selects a viewmodel resting position, not an item mechanic
- `src/core/mining.hpp` — header-only `tool_mining_speed` / `crushed_block` (pure, unit-tested) plus `mining_speed_multiplier` and `resolve_block_drop` (registry lookups): the break-speed multiplier a held tool grants against a block's `preferred_tool`/`min_tier` (and a hammer against anything crushable), and the inventory stack a broken block yields after variant collapse, `drops` and any crush
- `src/core/crc32.hpp` — IEEE 802.3 CRC32 for chunk save checksum (also verifies gzip members)
- `src/schematic/gzip_inflate.hpp/cpp` — Self-contained DEFLATE plus gzip/zlib/raw framing, checksum-verified and size-capped, for readers that must run without the engine or zlib
- `src/schematic/nbt_reader.hpp/cpp` — Strict forward NBT cursor (big- or little-endian, value skipping without allocation, nesting cap)
- `src/schematic/schematic_reader.hpp/cpp` — Decoder for both families: the classic `Schematic` root (dimensions, `Blocks`/`Data`/`AddBlocks`, both `Data` layouts, writer origin) and the palette formats v2/v3 (named states with properties, varint indices), each normalised to a palette of states plus one index per cell, with tile-entity/entity counts kept
- `src/schematic/minimal_json.hpp/cpp` — Strict JSON reader for data files that must load without the engine runtime (the translation table, in the report tool, the tests and the game alike)
- `src/schematic/mc_palette.hpp/cpp` — Reader and resolver for `data/minecraft_blocks.json`: legacy `(id, data)` → a block name, a deliberate skip, or Unknown, with `substitute`/`fluid` flags and the row's note
- `data/minecraft_blocks.json` — The translation table itself: one row per legacy id, plus named `variant_sets` sharing each shape's data layout
- `src/schematic/paste_plan.hpp/cpp` — The paste policy: what a file would change, in world coordinates, with a counter for every cell and a one-level undo record
- `tools/schematic_report.cpp` — `scons schematic_report`: decodes a block file off disk and prints its format, layout, dimensions, fill, bounding box, the full block-state palette with counts and per-state targets, and the coverage summary with the gaps left; `--plan` builds the real paste plan and times it, which is how a file's paste cost is measured without launching the game
- `src/world/block_editor.cpp` (`apply_paste` / `undo_paste`) — The bulk write: one locked band per chunk, per-column sky light, one relight and remesh per touched chunk, edit-map persistence and one-level undo
- `ChunkManager.inspect_schematic` / `paste_schematic` / `undo_paste` — The script-facing half, driven by `/paste` in `chat.gd`
- `src/core/thread_pool.hpp` — Shared worker pool, high-priority queue

### Mesh
- `src/mesh/mesh_manager.hpp` + `mesh_manager.cpp` / `mesh_manager_worker.cpp` / `mesh_manager_upload.cpp` / `mesh_manager_rebuild.cpp` / `mesh_manager_far.cpp` / `mesh_manager_lifecycle.cpp` / `mesh_manager_internal.hpp` — Per-chunk mesh builds, upload, instance management, three-tier LOD, far-region merging, nearest-first completion
- `src/mesh/mesh_builder.cpp` / `mesh_builder_solid.cpp` / `mesh_builder_greedy.cpp` / `mesh_builder_faces.cpp` / `mesh_builder_fluid.cpp` — Greedy meshing, incremental partial remeshes, and the liquid surface pass
- `src/mesh/mesh_fluid.hpp` — The liquid surface rule, pure and header-only: per-corner heights, the surface family a block belongs to, and whether a face shows against a given neighbour. No chunk, no mesh builder, no registry scan, so `tests/test_fluid_surface.cpp` drives it over a map of cells
- `src/mesh/chunk_neighbor_accessor.hpp/cpp` — 26 neighbor pointers for mesh building
- `src/mesh/chunk_render_data.hpp` — `ChunkRenderData` (per-chunk render state stored in the chunk map), `CachedFarChunkMesh`, `CompletedMesh`
- `src/mesh/mesh_types.hpp` — Mesh types, light checksum grid for incremental rebuilds
- `src/mesh/mesh_queue.hpp` — `DirtyChunkEntry` + frustum/distance-prioritized mesh rebuild queue

### World
- `src/world/chunk_world.cpp` + `chunk_world_edits.cpp` / `chunk_world_persistence.cpp` — Edit application (block edits, pending/vegetation placements, unload/clear) and save/load (async `flush_dirty_chunks`, generation + epoch gated `enqueue_chunk_save` / `save_chunk_snapshot`, `write_chunk_file_locked`, inventory save/load). All hot paths use `lock_keys_exclusive()`
- `src/world/block_editor.cpp` — `place_block` with targeted locking
- `src/world/player_light.hpp` — Player light with targeted locking
- `src/world/world_updater.hpp/cpp` — Frustum integration, budgets, periodic dirty flush, and the fluid step: `update()` runs generation → unload → `fluid_sim.advance(delta)` → mesh budgets, so the cells it looks at are the ones that exist now and the chunks it dirties can remesh the same frame. Its `FluidSink` is what turns a tick's writes into persisted edits (with `notify=false`: the simulation schedules what it wrote itself) and one remesh request per chunk
- `src/world/chunk_scheduler.hpp` — Completion queues, `poll_completed_mesh_nearest`
- `src/world/day_night_cycle.hpp` — Sky-light cycle

### Worldgen
- `src/worldgen/chunk_generator.hpp/cpp` — Stacked-noise macro surface, height-based oceans, signed 3D density field with 4×4×4 shape lattice, chunk-level fast paths (see Terrain Generation above)
- `src/worldgen/vegetation_generator.hpp/cpp` — Tree placement with variant-weighted per biome, minimum spacing, deferred cross-chunk writes
- `src/worldgen/biome_config.hpp` — Biome config loaded from `data/biomes.json`
- `src/worldgen/vegetation_config.hpp` — Vegetation config loaded from `data/vegetation.json`
- `src/core/terrain_params.cpp` — Terrain parameters loaded from `data/terrain_config.json`

### UI (GDScript)
- `chat.gd` — Chat system with autocomplete: ghost text suggestions with pulsing effect, tab cycling through completions, up/down arrow navigation, hold-to-cycle, parameter hints for commands, command execution (`/help`, `/give` with unlimited count, `/tp`, `/fly`, `/clearchat`, `/clearinv`, `/version`), mouse wheel scrolling for chat history, caret blink, wrapped messages with proper input box anchoring
- `hotbar.gd` — Hotbar UI with mouse wheel cycling, click-to-hold block selection
- `healthbar.gd` — Health bar UI: 10 hearts above the hotbar's left edge (~40% of its width), full/half/empty sprites resolved from the half-heart count polled off `PlayerController.get_health()`
- `death_screen.gd` — Death overlay: "You died!" + Respawn button, shown on the `PlayerController.died` signal and hidden on `respawned`
- `inventory.gd` - Full inventory screen with drag-drop stack movement, shift-click quick-transfer, RMB drag-place, LMB drag-collect, scroll wheel quick-transfer, double-click gather; live 2×2 crafting grid + output preview (click/drag/shift/scroll interactions mirrored on the crafting cells; shift-click output crafts as many as possible)
- `data/recipes.json` — Crafting recipes (shaped/shapeless), resolved by block name; loaded into `RecipeBook` at startup. A shaped `key` entry may list several acceptable ingredients, expanded at load into one concrete recipe per combination (per symbol, so all cells of a symbol are the same ingredient) — the matcher, the preview gate and the consumption path stay id-exact
- `settings_menu.gd` — Adjustable settings with persistence (render, lighting, crosshair, controls) opened with Escape key; includes a **Skin Maker** page (color wheel, hex readout, orbitable preview) with a dark-mode toggle and a **Block Maker** page (16×16 cube painter) with paint tools, noise slider, and gallery. Escape opens a bare pause menu (Resume / Settings / Controls / Tools over the dimmed world, no chrome) and **Settings** is one `_build_scrolling_page` stacking the categories General, Block Outline, Crosshair, Advanced Rendering and Render with a transparent content box (Controls and the Tools launcher have their own pages from the pause-menu buttons): a title bar, a centred content box, an action bar, and a vertical scrollbar. A resettable row's reset is the square `undo_button.png` icon (20 units, 1:1 with the row height); a transparent content box draws no border, which is why the settings/controls/tools pages show no grey frame over the world. Each category heading is an HBox: beside the title sit square export/import icons (`export_button.png`/`import_button.png`) and a reset-all icon (the same `undo_button.png`), all at `UNIT_HEADING_ICON_W` (half the row reset) and tight against the non-expanding title. Export, import and reset all are all wired: each category carries a codec (its own `FG`/`FO`/`FC`/`FAR`/`FR` code plus a refresh callable that resyncs its rows), and the three icons beside its title drive it; reset all replays the section's own row resets. Every settings area (GUI, lighting, video, controls, crosshair, block outline, the two editors) is a section of that page, each introduced by a `"category"`-marked section heading — the reference interface's Video Settings layout, with the areas stacked instead of split across screens; the crosshair category interleaves its Cross and Dot rows so each reads as one full column. Section builders return row arrays (`_build_lighting_sections()`), so a setting is one row and an area is one more section. Every size is a `UNIT_*` constant times `_ui_scale()` (one unit is one GUI-scale pixel, so a 200x20-unit button is 400x40 px with 16 px text at GUI scale 2), which is what the maker pages' chrome and the gallery cards were brought onto; nothing in the menu is sized in raw pixels any more
- `liquid_texture_lab.gd` — Liquid Texture Lab (O key, autoload): procedural animated-texture authoring for water/lava/acid — style presets, every automaton and ramp knob as a slider, live animated preview plus a 3×3 tiled seam check and frame thumbnails, save/load as a vertical strip PNG + settings JSON in `user://liquids/`, a **Live** mode that pushes frames into the running world's texture-array layer for the chosen liquid (turning texture compression off for the duration, since a compressed layer cannot take an RGBA frame), and **Bind to world**, which saves the current strip as `<liquid>.json`/`.png` — the file the animator below loads for that liquid — and a **World anim** switch onto that animator
- `liquid_animator.gd` — LiquidAnimator (autoload, always on): animates water, lava and acid in the running world with no panel open. Per liquid it takes `user://liquids/<liquid>.json` if the Lab bound one and the built-in preset otherwise, regenerates the strip in C++ (deterministic, so the saved settings are the whole source), slices it, and pushes a frame into that liquid's array layer every `frame_time` ticks, wrapping a looping strip to index 1. It turns texture compression off while it animates and restores the setting when disabled, and it is paused for whichever liquid the Lab's Live currently owns (`pause`/`resume`) — one writer per layer. `get_status()` reports source, frames, current frame, the frame last pushed, and why a liquid is idle; `.freebuff/probe_lava_acid.gd` drives all of it against the real world
- `skin_preview.gd` — Transparent-background sub-viewport that orbits `player.glb` behind the skin maker; the camera orbits the model's AABB center rather than being a child of the rotating node
- `block_manager.gd` — Autoload holding the single persistent 16×16 block texture (one `ImageTexture` shared by every cube face), with debounced saves to `user://current_block.png`, a restart-recovery noise base (`user://block_noise_base.png`), and a reversible grayscale-noise slider living on the autoload so it survives page rebuilds
- `block_preview.gd` — Transparent-background sub-viewport behind the block maker that drag-orbits a cube; DRAW/FILL/BOX painting over primitive triangle raycasts, undo (Ctrl+Z), noise slider integration, and clamped zoom
- `player_model.gd` — Applies the skin texture to `player.glb`'s `StandardMaterial3D` surfaces with nearest filtering (no mipmaps, avoiding smeared UV islands), and drives the Minecraft-style head look (`_track_head_look`): it rebuilds the aim basis from the controller's world yaw + pitch (`get_aim_direction()`) so the `head` mesh follows the player's look in every view, with a camera-follow fallback for scenes without a `PlayerController` (skin preview)
- `player.glb` — Voxel-style player model with a tightly-packed 64×64 skin-texture atlas; node pivots re-baked onto the true Blockbench joints (limb tops, head neck) by `tools/rebake_player_pivots.py` because Blockbench's glTF export flattens cube-pivot metadata
- `pose_clone_debug.gd` — Debug tool (K key): spawns a rigid, punchable physics dummy — a frozen copy of the player model (no Idle animation, no head tracking) running `dummy.gd`'s vanilla 1.8.8 gravity/drag/knockback at 20 tps — standing on the aimed block, with a bright depth-test-off cube at every mesh's origin (see `shaders/pose_pivot_marker.gdshader`); prints each part's pivot for marker-vs-transform verification
- `dummy.gd` — Combat physics for the K-key pose clone: 20 tps vanilla 1.8.8 gravity/drag, knockback (`apply_knockback`: base 0.4 + sprint bonus, 10-tick hurt-resistance gate), interpolated rendering between ticks
- `tools/bake_liquid_textures.gd` — Headless baker for a liquid's still block texture: runs the generator's own preset for `lava`/`acid` (or any style named on the command line) and writes frame 1/4 of the strip to `textures/blocks/<style>.png`, the PNG the texture array builds its layer from. `godot --headless --path . --script res://tools/bake_liquid_textures.gd`, then `--import`
- `tools/rebake_player_pivots.py` — Idempotent re-baker for `player.glb`: moves each node origin onto its pivot while shifting that mesh's vertices by the delta so world placement is byte-identical (glTF has no pivot field — a node's origin is its rotation anchor)
- `viewmodel.gd` — Thin first-person hand + held item/block glue (child of `Camera3D`): node tree, `_input`, F12 HUD, and peak-pose constants. Per-frame animation math lives in `ViewmodelPose` and held-mesh geometry in `ViewmodelMeshes` (both native C++)
- `block_break_overlay.gd` — Draws the 10-stage crack overlay (`textures/animated/l0_sprite_01..10.png`) on the mined block, driven by `get_break_state()`
- `block_textures.gd` — ~~Block texture atlas generation from `textures/blocks/`~~ ported to the native `BlockTextures` GDExtension binding (registered in `src/godot_bindings/register_types.cpp`); the GDScript file has been deleted

### Engine
- `src/engine/collision_resolver.hpp/cpp` — Binary-search collision, step-up
- `src/pathfinding/nav_types.hpp` — Planner cell classes, `NavCosts`, packed node keys, `NavPath`/`NavStats`
- `src/pathfinding/nav_view.hpp/cpp` — Lazy memoised view of the world: per-column topmost standable surface, body clearance, liquid flag; unresident chunks resolve to `Unknown` and are never traversable. Takes an optional ranged `Reader` and prefetches each resolved column's scan window into a buffer, so a scan costs one map lock instead of one per cell; cells the buffer does not hold fall back to the per-cell sampler
- `src/pathfinding/move_generator.hpp` — Movement primitives between columns (walk, diagonal, step up, drop, hop across a one-cell gap) with their legality rules and costs
- `src/pathfinding/pathfinder.hpp/cpp` — Budgeted deterministic A* over the movement graph (octile + vertical heuristic, expressed in `NavCosts` units). Two caps: `max_expansions` and an optional `max_ms` wall clock, checked every 64 expansions; `truncated` = a cap ran out (not "no route"), and either way the partial route is a legal chain of moves starting at the agent. The goal test requires the goal's own anchored cell, not merely its column: a column can hold several standable surfaces (the ground under a floating staircase is in the same column as the step above it), so a column-only test accepts an arrival well below the target
- `src/pathfinding/block_class.hpp` — The single block→nav-cell conversion (air / air-passable liquid / partial shape / full solid), deliberately not trusting `BlockType::is_full_cube()`
- `src/pathfinding/chunk_nav_source.hpp` — Reads the live `ChunkMap` for the planner. `sample()` = one locked accessor per cell; `read_column()` = a whole column range under one `lock_keys` (ascending shard order), released before returning. Every lock is scoped to a single call: holding one across calls stalls generation writes on that shard and can deadlock. Unresident chunks report Unknown and stay that way for the life of the source
- `src/pathfinding/path_service.hpp/cpp` — Async job runner: queues searches on the engine `ThreadPool`, returns finished `PathResult`s (support-block coordinates) to the main thread by job id. The pool is passed to `submit()` per call rather than held: the controller tears its pool down and rebuilds it on a runtime reset (`clear_editor_chunks`), so a stored `ThreadPool&` would be a dangling queue target afterwards
- `src/pathfinding/path_smoother.hpp/cpp` — String-pull smoothing over an exact 8-connected walkability test
- `src/engine/player_controller.hpp/cpp` — `PlayerSim` (fixed-timestep simulation, fall-distance tracking + landing damage)
- `src/engine/voxel_engine_controller.hpp/cpp` — Bridges `ChunkManager` state to the world

### Fluids
- `src/fluids/fluid_rules.hpp` — The flow rules and their whole interface: `FluidKind`, `FluidCell` (source / runoff depth / falling), `FluidTraits` per kind, the `FluidWorld` query interface the rules ask, and `FluidStep` (what this cell becomes, plus the writes it pushes outward). The header carries the rule list as prose because the rules ARE the specification
- `src/fluids/fluid_rules.cpp` — `tick()`: recompute the cell from its neighbours, the falling rule (fed from above, which can revive a cell that has no side supply at all), the two-sources-over-something-solid source rule (per substance: water and lava pool, acid does not), dry-up, and the push outward — down first, else sideways toward the directions with the shortest distance to a drop. That search is bounded to the substance's `search_distance` (4 steps for water, 3 for lava, 5 for acid) and short-circuits entirely when a neighbour can drop straight off, which keeps it constant and world-size independent (~1400 reads in the worst case, measured by `tests/test_fluid_rules.cpp`). `traits_for(kind)` above it is the whole per-substance table: water is the struct's defaults, lava is `{1, 3, 20, 3, true}` (three cells out, a second between cells, pools) and acid is `{1, 7, 3, 5, false}` (water's depth on a faster clock, hunts a drop further, never pools from a pair of sources)
- `src/fluids/fluid_state_table.hpp/cpp` — The only place a fluid state meets a block id: built by SCANNING the registry for blocks that declare a `fluid` state, never from a hardcoded id list, because the JSON the game loads and the C++ defaults the tests run on number the same states differently
- `src/fluids/chunk_fluid.hpp/cpp` — The chunk-side adapter: `read_window()` copies an 11×11×3 window under one ranged `lock_keys` and reports whether every chunk it needed was resident (a missing chunk reads as air, so judging against one would pour fluid into space that gets regenerated over it and would delete the water at the edge of loaded space); `apply_writes()` groups a tick's writes by chunk and takes ONE exclusive lock, one mesh-dirty and one `FluidWriteSink` call per chunk
- `src/fluids/fluid_sim.hpp/cpp` — The 20 Hz tick driver: a due-ordered pending set (earliest tick per cell wins, and a cell whose turn has already come keeps its token even when a write pushes its next wake later — both ticks are owed, which is what makes a flood spread as a diamond rather than a rectangle), a per-tick cell budget, catch-up capped at two ticks a frame, and "recompute; if the cell did not change, stop asking it" instead of the reference's flowing/settled block pair. Nothing about the schedule is saved — a chunk that loads is seeded from the fluid states in its edit map, so a flood survives a reload. `WorldUpdater` advances it and `world_updater.hpp`'s `FluidSink` puts its writes back into the edit map and the remesh queue

### Rendering
- `src/render/environment_controller.cpp` — Sky/fog/player-light parameter pushes
- `src/render/material_manager.hpp/cpp` — Terrain + water materials, texture arrays
- `src/render/texture_array_generator.hpp` — Diffuse + emissive `Texture2DArray` generation; `find_texture_layer()` reports "no such layer" as -1 (the writer's lookup) and `get_texture_index()` keeps its 0 fallback (the mesh path's)
- `src/render/liquid_texture.hpp` — Pure procedural animated-liquid generator (three-field automaton, style presets, colour ramp, vertical frame strip), no Godot types, so the lab, the animator and the tests run the same code
- `src/render/world_render_stats.hpp` — `WorldRenderStats` snapshot consumed by `PerfReport`

### Godot bindings
- `src/godot_bindings/register_types.cpp` — Registers every GDExtension class/static binding (`BlockTextures`, `BlockOutline`, `BlockOutlineBuilder`, `ViewmodelPose`, `ViewmodelMeshes`, `SkinPixels`, `ChunkManager`, `PlayerController`, ...)
- `src/godot_bindings/chunk_manager.cpp` — Inspector properties, camera/frustum entry point, block API, `flush_dirty_chunks`, `_exit_tree` quit flush; `raycast_from_camera` casts from the `PlayerController`'s eye-ray (`get_aim_origin`/`get_aim_direction`) with a Camera3D fallback
- `src/godot_bindings/player_controller.cpp` — `PlayerController` node: input, mouse look (±90°), fly mode, F5 view cycle, camera-collision sampling, eye-based aim bindings, `ModelPivot` body-yaw lag, break/place, inventory bindings, chat bindings, health bindings, `_exit_tree` inventory save
- `src/godot_bindings/block_outline.hpp/cpp` — Native `BlockOutline` `Node3D` (replaces `block_outline.gd`): 16 exposed settings, raycast throttling, pulse animation, material/geometry management; mesh built by the tested `BlockOutlineBuilder`/`block_outline_mesh.cpp` core
- `src/godot_bindings/viewmodel_pose.hpp/cpp` — `ViewmodelPose` static binding over the `src/core/viewmodel_math.*` per-frame animation math (bob/sway/swing)
- `src/godot_bindings/viewmodel_meshes.hpp/cpp` — `ViewmodelMeshes` static binding over `src/core/viewmodel_meshes.*` held-block/shaped-box/sprite geometry
- `src/godot_bindings/liquid_texture_gen.hpp/cpp` — `LiquidTextureGen` static binding over `src/render/liquid_texture.hpp`: `style_names()`, `default_settings(style, resolution)`, `describe(settings)`, `generate_strip(settings)`
- `src/godot_bindings/skin_pixels.hpp/cpp` — `SkinPixels` static binding: native pixel/noise helpers (noise map, gray noise, UV-to-texel bounds) used by `skin_manager.gd` and the settings galleries

### Lighting
- `src/lighting/light_propagator.hpp/cpp` — Public wrappers + `_locked` variants
- `src/lighting/block_light_region.hpp/cpp` — Single-chunk additive-only propagation

### Data
- `data/block_definitions.json` — Single source of truth for block properties
- `data/block_shapes.json` — Shared shape registry for non-full blocks (slabs, stairs, walls, poles, crucible); a list of AABBs per shape, with optional collision boxes distinct from the visible ones
- `data/biomes.json` — Biome definitions with per-biome materials, climate thresholds, tree density, and tree variant weights
- `data/vegetation.json` — Vegetation parameters for the hills biome
- `data/terrain_config.json` — Macro-surface tuning: `height_base_y`, domain-warp amplitudes, mid/small relief fields, shape-strength range, weirdness thresholds
- `textures/` — Asset organization:
  - `textures/blocks/` — Block textures (bedrock, dirt, grass, stone, sand, water, etc.)
  - `textures/gui/` — UI textures (hotbar, inventory background, effects)
  - `textures/sprites/` — Sprite textures (hearts, etc.)
  - `textures/atmosphere/` — Atmospheric textures (sun, north star)
  - `textures/Archive/` — Archived/deprecated textures (old versions kept for reference)

### Testing
- `tests/` — 30 test files, 255 test cases / 181,725 assertions, auto-discovered via `Glob("tests/*.cpp")`
- `tests/test_concurrency.cpp` — 27 tests for shard locking, deadlock prevention, PaletteStorage, cross-chunk writers, and thread-pool work stealing
- `tests/test_inventory.cpp` — Inventory add/consume/edge-case tests
- `tests/test_crafting.cpp` — Shapeless/shaped matching (trim, mirror, offset, rotation/partial misses) + atomic craft_item tests (success, insufficient ingredients, full inventory rejection)
- `tests/test_mining.cpp` — Tool-vs-block break-speed decisions: neutral defaults, class match/mismatch, `min_tier` gating (which never blocks the break), speed read from the tool, and the registry lookup only rewarding real item tools
- `tests/test_light_propagation.cpp` — Cross-chunk BFS edge case tests
- `tests/test_light_removal.cpp` — Overlapping multi-source light removal tests
- `tests/test_player_controller.cpp` — PlayerSim movement/sprint/sneak/collision tests, fall damage (safe jump landing, 7.5-block drop → 4 half-hearts, teleport reset clearing pending damage)
- `tests/test_soak.cpp` — Multi-threaded fly-through-the-world stress test
- `tests/test_persistence.cpp`, `tests/test_collision_resolver.cpp`, `tests/test_density_field.cpp` — format, collision, and terrain tests
- `tests/test_viewmodel_math.cpp` — 10 byte-for-byte cases pinning the viewmodel bob/sway/swing math
- `tests/test_viewmodel_meshes.cpp` — 5 byte-for-byte cases for the held-block/shaped-box/sprite mesh geometry (locked against an independent Python port)
- `tests/test_block_outline_mesh.cpp` — Outline mesh union/dedup/thickness cases for the native `BlockOutline`
- `tools/benchmark.cpp` — 5 hot paths + memory, with `--check <baseline>` regression mode
- `tools/fuzz_*.cpp` — libFuzzer harnesses (`fuzz_palette`, `fuzz_chunk_load`, `fuzz_chunk_recovery`, `fuzz_light_propagation`, `fuzz_mesh_builder`)
