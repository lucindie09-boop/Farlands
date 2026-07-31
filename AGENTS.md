## Goal
Ongoing: a Minecraft-style voxel engine (Godot 4 + C++ GDExtension) with chunked streaming, greedy meshing, colored lighting, biome-based terrain, and frustum/LOD-prioritized rendering. The frustum-prioritized loading + sharded-locking + palette-storage rework described below is complete and on `main`; this file now also tracks the rendering/content work and repo hygiene that followed it.

## Constraints & Preferences
- Bias toward minimal, high-impact changes that reuse existing infrastructure.
- Camera3D child named "Camera3D" on the player node is the frustum source.
- Chunk size is 32×32×32, world height 1024 blocks (`WORLD_HEIGHT_Y` in `chunk_coords.hpp`).
- Block properties live in `data/block_definitions.json`, not scattered across C++ — that's the single source of truth now (see below).

## Progress
### Done
**Frustum-prioritized loading, sharded locking, palette storage (foundational rework):**
- Created `core/frustum.hpp` — header-only Frustum with 6-plane extraction and AABB test.
- `DirtyChunkEntry` priority: `urgent > in_frustum > dist_sq`; `MeshQueue`/`MeshManager` reprioritize accept an optional `Frustum*`.
- `WorldUpdater` two-phase generation: dedicated frustum-priority pass first, then distance-sorted sweep. `update_unload()` defers unloading visible chunks beyond render distance.
- Dynamic mesh budget: visible-chunk ratio scales `mesh_rebuild_budget`/`upload_budget` from 0.5× (sparse) to 1.0× (full).
- Collision: binary-search AABB approach (a 3D DDA variant was tried and reverted after side-face ghosting bugs).
- **Sharded chunk map**: 64 shards, each with its own `shared_mutex` + `unordered_map`. `ShardLock`/`ExclusiveShardLock` RAII types; `lock_chunk()`, `lock_keys()`, `lock_all()` for ordered multi-shard locking. Single-chunk/block accessors lock only their own shard; batch methods lock all relevant shards in ascending order to avoid deadlock.
- **PaletteStorage** (blocks and light): 8 × 16³ paletted sections per chunk replace the old dense arrays. Uniform sections (all air, all stone) cost only a palette entry; non-uniform sections use 4/8/16-bit indices as needed. Per-chunk memory dropped from ~130KB to ~1–20KB on typical terrain.
- Renamed Mountains→StonePlateau biome across enum, block selection, and debug renderers.
- Fixed several correctness bugs found via play-testing: incorrect interior-chunk mesh culling next to non-fully-solid neighbors; a Y-index formula bug in `count_section_blocks`; stale `solid_cache` entries causing missing faces (fixed by zeroing at build start); an editor-viewport `_process` bug.
- Added translucent water as a second mesh surface with its own shader (`voxel_shader_water.gdshader`, `voxel_material_water.tres`) — edge fade, tint, shimmer, Beer-Lambert depth absorption — and wired sky/fog/player-light parameter updates to both terrain and water materials every frame.
- Micro-optimizations across the greedy mesher (single-pass vertical sweep, solid-block fast paths, `BlockID`-typed `solid_cache`, incremental AO tracking) — measured greedy_v cost dropping roughly 2.68ms → ~0.73–0.98ms and greedy_h ~0.82ms → ~0.52ms in earlier profiling on this project's dev machine.
- **Removed the old LOD system entirely**: deleted `lod_controller`, `lod_types`, `lod_mesh_builder`, `merged_mesh_builder`, `mesh_manager_lod.*` (2,435 lines) — the 2×2×2 group-mesh-merging approach, along with `CompletedGroupMesh`, `ChunkRenderLod`/`LodLevel`, and the `lod0_radius`/`lod1_radius` inspector properties.

**Rendering, terrain content, and a second LOD approach (later work — not previously logged here):**
- **Replaced the removed LOD system with a simpler, per-chunk distance-based approach**: `lod_distance` and `lod_detail_level` properties on `ChunkManager` drive stride/detail reduction directly inside the greedy mesher for far chunks, instead of merging chunks into groups. Iterated through several correctness passes: boundary footprint aliasing, corner-sampling aliasing in `solid_cache`, majority-vote (instead of first-hit) macro-cell occupancy, a stride-1 "skirt" ring at the LOD transition to prevent T-junction cracks, and a cap of 128 LOD remeshes/frame.
- Fixed a longstanding null-neighbor handling bug across the mesher: missing neighbor chunks are now treated as air for culling purposes rather than force-culling faces (this had been reintroduced/reverted at least once before landing correctly).
- Fixed a race in `cross_writer` (vegetation cross-chunk block writes): now holds an exclusive shard lock across the read-check-write instead of two separate locked operations.
- Fixed a use-after-free: neighbor chunks are now pinned (via `pending_mesh_builds` atomic) during mesh build so they can't be unloaded mid-build.
- **Unified block definitions into a single JSON source of truth** (`data/block_definitions.json`): `BlockRegistry::load_from_json` replaces scattered per-block C++ definitions; texture/emissive arrays are generated from the same file.
- **Added emissive texture support**: a second `Texture2DArray` for per-face glow maps, sourced from `data/block_definitions.json`, threaded through `mesh_builder_faces.cpp` and `material_manager.cpp`.
- **Added vegetation generation**: tree placement with minimum spacing between trunks, deferred cross-chunk writes to the main thread to avoid shard-lock contention, exposed as a `vegetation_enabled` inspector toggle. Fixed a greedy-mesh vertex winding bug (CW vs CCW) and an AO flip condition discovered while re-enabling it.
- **Disabled the sun's shadow map**: both terrain shaders are unshaded, so the shadow pass was pure GPU cost with no visible effect.
- Fixed godray shader white-tint and crosshair rendering artifacts; removed unused textures.
- Added far-region chunk merging, deferred auto-update, and unload notification to reduce redundant work when regions are only partially cached.

**Repo hygiene / process (most recent):**
- Removed a committed Windows `.lnk` shortcut (`src/fuk-minecraft.lnk`) that leaked a local filesystem path; scrubbed it from git history (not just HEAD) via a history rewrite, then force-pushed. Added `*.lnk` to `.gitignore`.
- Removed a stray empty file (`s`) and three orphaned Godot `.obj.import` files (`RealSkeleton.obj.import` and two `tools/*.obj.import` files pointing at build binaries, not real assets). Added `/s` and `*.import` to `.gitignore`.
- Added a `LICENSE` file (GPL-3.0) — the repo had none before, meaning it was technically all-rights-reserved despite being public.
- Added CI (`.github/workflows/build.yml`): cross-platform matrix (ubuntu-latest, macos-latest, windows-latest), SCons build + test on every push/PR, TSan on Linux via `TSAN=1` flag, `tsan_suppressions.txt` for doctest/godot-cpp noise, timeouts on all steps. Added ASan+UBSan leg on ubuntu.

**Light-propagation lock fixes (7 deadlock classes, all resolved):**
- Discovered that `lock_all()` returns `ShardLock` (shared_lock) but write paths in light propagation assumed mutual exclusion. Identified 5 deadlock classes (A–E) in the locking hierarchy.
- **Deadlock A**: `process_completed_chunks:142` held `lock_all()` shared → else-branch called `propagate_block_light_region` → tried `lock_all_exclusive()` → shared→exclusive upgrade fails.
- **Deadlocks B–D**: `propagate_block_light_region`, `light_propagate_add`, `light_propagate_remove` held `lock_all_exclusive()` → called `mark_chunks_dirty_for_light` → `get_chunk_render_data()` → shared_lock on same shards → self-deadlock. Fixed by moving `mark_chunks_dirty_for_light` calls outside the lock scope in all public wrappers.
- **Deadlock E**: `block_editor.cpp:place_block` — unlocked `set_block` writes to ChunkData while mesh builders read under shared shard locks. Fixed by wrapping all ChunkData reads/writes in `lock_all_exclusive()`, using `_fast` accessors, calling `_locked` light variant, then releasing lock before auto-locking operations (mark_dirty, queue_dirty_chunk).
- **Deadlock F (naming trap)**: `update_block_light_incremental_locked` acquired its own `lock_all_exclusive()` despite the `_locked` naming convention meaning "caller already holds lock". If `place_block` had called it from inside its own lock scope, same self-deadlock. Fixed by moving the lock acquisition to the public wrapper, making `_locked` truly lock-free.
- **Bug G**: `player_light.hpp` lines 70/91 wrote `set_light_rgb` without any lock while light propagation BFS could run concurrently on worker threads. Fixed by adding `ChunkMap&` parameter to `update()`, acquiring `lock_all_exclusive()` for all ChunkData operations, binding BFS callbacks to public `_locked` methods, and deferring `mark_dirty` calls to after lock release.
- `pending_light_removals_` protected with `std::mutex` — narrow scope in `try_fixup_chunk` (find/erase only, release before calling `propagate_block_light_region`).
- Added `lock_all_exclusive()` to `ChunkMap` (returns `ExclusiveShardLock` with `std::unique_lock`).
- Public `_locked` BFS methods (`light_propagate_add_locked`, `light_propagate_remove_locked`, `update_block_light_incremental_locked`) moved to public — contract documented: caller MUST already hold `lock_all_exclusive()`, uses `_fast` accessors only, MUST NOT call `mark_chunks_dirty_for_light`.

**Targeted shard locking (`lock_keys_exclusive`) — fully migrated:**
- Added `lock_keys_exclusive<N>()` to `ChunkMap` — returns `ExclusiveShardLock` on only the shards whose keys appear in the input, in ascending shard order (deadlock-safe). Template and vector overloads.
- Migrated ALL hot paths from `lock_all_exclusive()` (all 64 shards) to `lock_keys_exclusive()`:
  - `set_block_variant()` — 1 chunk key
  - `propagate_block_light_region()` — 27 keys (3×3×3 neighborhood)
  - `fire_and_forget` lambda in `chunk_world.cpp` — 27 keys (3×3×3 neighborhood)
  - `update()` outer scope in `chunk_world.cpp` — 28 keys (1 center + 3×3×3 neighbors)
  - `place_block` (block_editor.cpp) — 27 keys (3×3×3 center)
  - `light_propagate_add` — origin 3×3×3 + each seed node's 3×3×3, deduplicated via `seen[]`
  - `light_propagate_remove` — same pattern as add
  - `update_block_light_incremental` — 54 keys (origin + center 3×3×3)
  - `PlayerLight::update` — vector of up to 54 keys (old+new chunk 3×3×3)
- BFS bounded reach: max light level 15 < chunk size 32, so 3×3×3 neighborhood (27 keys) covers all BFS paths from a single seed chunk.

**Concurrency tests and cross-platform CI:**
- 16 concurrency regression tests in `tests/test_concurrency.cpp` using `TestShardMap` (mirrors ChunkMap's 64-shard architecture without needing `godot::RID`). Covers: shard lock concurrency, ascending-order deadlock prevention, exclusive serialization, PaletteStorage R/W, `pending_light_removals_` pattern, RAII lock correctness, `OrderedExclusiveShardLock` target verification, light removal tests, cross-chunk writer race tests, and `pending_light_removals_` stress test.
- 102 tests / 525 assertions total across 11 test files.
- CI: cross-platform matrix with 4 legs — ubuntu TSan, ubuntu ASan+UBSan, ubuntu plain, macos plain, windows plain. Benchmark runs on non-sanitizer legs with `--check` regression detection.

**Expanded benchmark tool:**
- `tools/benchmark.cpp` benchmarks four hot paths: `generate_chunk` (1,000 iters), `build_mesh` (1,000 iters), `palette_write` (100 full-chunk fills), and `light_propagation` (1,000 iters on 3×3×3 grid with emissive source).
- `benchmark_baseline.txt` checked into repo with 2× measured values as regression thresholds.
- `--check <baseline>` mode exits non-zero on regression; wired into CI.
- `LightPropagation` TimerID added to `performance_timer.hpp`.

**Save format versioning (chunk persistence):**
- Chunk files bumped from v2 to v3: header now includes a CRC32 checksum of the RLE body.
- Format: `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32][RLE body...]`.
- `load_chunk_from_disk` handles v3 (CRC32 verified), v2 (legacy RLE, no check), and v1 (flat, legacy) transparently.
- Corrupted v3 files are rejected at load time (returns `false`).
- `crc32.hpp` is a header-only IEEE 802.3 CRC32 implementation in `src/core/`.

**Sanitizer/fuzz infrastructure:**
- Added `ASAN=1` build flag to SConstruct (ASan+UBSan, Linux/Clang only, mirrors TSan pattern).
- Added ASan+UBSan CI leg on ubuntu-latest with `halt_on_error=1` and `detect_leaks=1`.
- Added libFuzzer harnesses (Linux/Clang only, `-fsanitize=fuzzer,address,undefined`):
  - `tools/fuzz_palette.cpp` — random set/get on `ChunkData`
  - `tools/fuzz_chunk_load.cpp` — random `ChunkData` header parsing
- Built via `scons fuzz` target.

**5 Major Visual Improvements (Water, Vegetation, Terrain Cliffs, Atmosphere, Shading):**
- **Dynamic Water Shader**: Translucent water rendering with edge fade, depth absorption, bounce light, and sun glint (`voxel_shader_water.gdshader`).
- **Vegetation Variety & Wind Swaying**: Added `place_spruce_tree` (tapered conical pine structures for mountain/highland biomes) and `place_birch_tree` (rounded dome foliage in forest biomes), plus procedural vertex swaying in `voxel_shader.gdshader` for foliage.
- **Slope Triplanar Cliff Blending**: Steep slopes (>45° / `world_normal.y < 0.75`) automatically blend in triplanar projected rock face textures in `voxel_shader.gdshader`, replacing stretched dirt/grass textures with mountain cliff faces.
- **Night Sky & Starfield**: Added dynamic procedural twinkling starfield to sky atmosphere rendering triggered during night sun elevations (`voxel_shader.gdshader`).
- **Soft Curved AO & Surface Detail**: Applied non-linear power-curve smoothing (`pow(raw_ao, 1.35)`) to eliminate diagonal triangulation seams.

**Signed 3D density field terrain (overhangs/shelves/arches):**
- Migrated terrain from a pure macro heightmap to a signed 3D density field: `density = (surface_y - y) + shape_noise * strength * surface_band` (positive = solid). The macro heightmap stays as the base surface; a normalized 3D fBm deforms only a band around it.
- `density_noise(seed+7000)` / `density_warp_noise(seed+8000)` (allocated, unused — 3D domain warp deferred) / `weirdness_noise(seed+9000)` — the last is a very-low-frequency 2D mask deciding where deformation is strong (`WEIRDNESS_SCALE=0.0012`), producing a geological feel.
- Band-limited shaping: `DENSITY_MARGIN=12`, `SURFACE_BAND_INNER=9`, `SURFACE_BAND_OUTER=28`, `SHAPE_STRENGTH_MIN=1.5`/`MAX=10`, `SHAPE_FREQUENCY=0.026` (~38-block scale), `SHAPE_Y_ANISOTROPY=1.35`. No ridged/absolute noise — it would shift average height.
- `generate_chunk` rewritten as: macro column pass (with cached weirdness) → `near_water` distance transform → full-chunk density buffer (+1 row for top-voxel decision) → per-column density surface (`columns[].surface_y`) → material pass (surface = `density > 0 && above <= 0`, `near_macro_surface` guard) → cave carve → water rule (fill only `!solid && wy > macro height && wy <= water_level`, preserving arches/avoiding flooded new caves) → **isolated-singleton removal** (any interior non-air voxel with all 6 orthogonal neighbors air is cleared — the density field occasionally produced lone floating cubes, e.g. 1 per ~360 chunks, confirmed via 3D flood fill).
- `get_chunk_height_range` padded by `DENSITY_MARGIN`; `chunk_world.cpp` gained a `may_contain_caves` guard so below-surface chunks overlapping the cave band aren't fast-pathed as fully solid.
- Vegetation now uses the real density surface (`columns[].surface_y`) with an underwater rejection guard instead of the macro heightmap.
- Verified by rendering vertical PGM density slices (temp tool, removed after inspection) and a 3D flood-fill connectivity check (temp tool, removed): surface stays within the band, no floating solids, ocean coastlines preserved.
- Added `tests/test_density_field.cpp` (4 tests: density/find_surface_y sanitation, generate_chunk↔density agreement, material validity, isolated-singleton removal). Suite is now 153 tests.
- **4×4×4 world-aligned shape lattice**: the 3D shape noise (`sample_shape_3d`) is sampled once per 4-block lattice node and trilinearly interpolated per voxel (`trilinear_interp`). `sample_shape_3d_interp()` is the canonical single-point field query (used by `sample_terrain_density`), and `generate_chunk` precomputes the same 9×9×9 lattice per chunk, so chunk grids and point queries stay bit-identical (proven by the density-agreement test). `SHAPE_LATTICE_SPACING=4` divides the chunk size, so nodes land on the same world coordinates across chunk boundaries — no seams. Only the noise sampling is coarse; the density field and block grid stay full resolution. All terrain parameters are unchanged. Cave carving remains disabled (`kCavesEnabled=false`). Measured generate_chunk barely moved (~5 ms), meaning the 3D shape sampling was not the dominant cost of `generate_chunk` (macro column pass and material pass dominate).

**Capped main-thread mesh completion + nearest-first uploads:**
- `process_completed_meshes` previously ignored `budget_ms` and drained the whole backlog in one frame. Now uses a strict wall-clock budget (`FrameBudgets::mesh_completion_budget_ms = 0.75` → `MESH_COMPLETION_BUDGET_MS`) plus the per-frame completion cap, following the `ChunkWorld::process_completed_chunks` time-loop pattern (`elapsed_ms >= budget_ms → break`).
- Nearest-first scheduling: `ChunkScheduler::poll_completed_mesh_nearest()` scans both completion queues (backed by `std::deque` now) for the chunk closest to the player, so the frame's few uploads go to the most visible chunks. Stale completions (epoch mismatch) are dropped during the scan and their `mesh_count_a` decremented. High-priority queue is always drained first.

**Third LOD tier — far-mode heightmap-only meshes:**
- New `lod_far_distance` (0 disables; default 0) + `far_detail_level` (default 0.25) inspector properties on `ChunkManager`, bridging `ChunkManager → VoxelEngineController → WorldUpdater → MeshManager`.
- `MeshBuilder::set_far_mode(true)` + `build_far_mesh()`: one merged top quad per macro column (stride_xz_² footprint), runs of equal block/height merge along z, flat-light AO, no side/underside faces, no caves, no water columns — a silhouette mesh feeding the existing far-region merge pipeline (`far_mesh_cache`/`CachedFarChunkMesh`).
- `compute_chunk_detail_level` is now three tiers: `dist <= lod_distance+1` → full, `dist > lod_far_distance` (and `lod_far_distance > lod_distance+1`) → `far_detail_level`, else `lod_detail_level`. `is_chunk_far_mode()` is a pure distance test; `ChunkRenderData::last_built_far_mode` records the emitter used so the reprioritize far-shell ring (dist ∈ `[lod_far_distance, lod_far_distance+1]`) can trigger mode switches even when `far_detail_level == lod_detail_level`.
- Far-region rebuild debounce: `FarRegionRenderData::last_dirty_at` timestamps dirty marks; `process_far_region_queue` skips regions marked within `kFarRegionDebounceMs = 250ms`, coalescing a burst of child-cache arrivals into one rebuild instead of one per child.

### In Progress
- (none)

### Blocked
- (none)

## Key Decisions
- Use `Camera3D::get_frustum()` for direct 6-plane extraction in world space; frustum recalculated every frame.
- Two-phase generation: dedicated frustum-priority pass first, then distance-sorted sweep.
- Collision uses the original binary-search approach (a DDA approach was tried and reverted).
- Sharded locking: 64 shards, per-shard `unordered_map` + `shared_mutex`, hash is `key % 64`. Hot paths doing many sequential reads (light propagation, dirty-neighbor checks) batch-lock instead of locking per call.
- Palette sections are 16³ (8 per chunk), matching `dirty_subchunks`; block and light storage share the same generic `PalSection`/`section_get`/`section_set` machinery.
- Greedy mesher reads through `get_block_unsafe()`/`get_light_packed_word_unsafe()` rather than raw pointers or thread-local shared buffers.
- Thread pool stays a single shared pool (not split into separate gen/mesh pools) — a split was tried and reverted after it starved generation throughput ~7×; a high-priority queue path handles contention instead.
- **LOD approach**: after removing the group-merge system, the project settled on per-chunk distance-based stride/detail reduction (`lod_distance`/`lod_detail_level`) rather than re-introducing chunk merging — simpler to reason about and to keep correct at boundaries, at the cost of not reducing draw-call count the way merged groups did. A third tier (`lod_far_distance`/`far_detail_level`) drops chunks beyond the mid tier to a far-mode heightmap-only silhouette mesh that feeds the far-region merge pipeline.
- **Block definitions are data, not code**: `data/block_definitions.json` is the only place block properties/textures/emissive maps are defined; C++ and the texture-array generator both read from it.
- Solid-block fast paths in the mesher require zeroing `solid_cache` at the start of every build — stale `BlockID`s from a reused thread-local builder previously caused wrong registry lookups in all-air sections.
- **Locking hierarchy for ChunkData writes**: all ChunkData reads/writes must hold `lock_all_exclusive()` or `lock_keys_exclusive()` for targeted shards. Public `_locked` BFS methods use `_fast` accessors under that lock. Auto-locking accessors (`get_chunk_data`, `get_chunk_render_data`, `mark_chunks_dirty_for_light`, `queue_dirty_chunk`) acquire their own shared locks — they MUST NOT be called under an exclusive lock. Public wrappers: acquire exclusive lock → call `_locked` → release lock → call auto-locking accessors for dirty-marking.
- **Terrain is a signed 3D density field, not a heightmap**: the macro heightmap sets `surface_y`, then a normalized 3D fBm (no ridged/absolute noise) deforms a band around it. Vegetation and culling must use the density surface, not the macro height. A post-pass removes isolated singleton voxels (rare 3D-noise artifacts) using only in-chunk neighbors so boundary blocks are never wrongly cleared.
- **Main-thread mesh completion is budget-capped**: `process_completed_meshes` spends at most `mesh_completion_budget_ms` (0.75) and one completion slot per frame per upload budget, choosing the nearest-to-player completion first (deque scan) so the visible world converges fastest; stale completions are dropped from the queue during the scan.

## Next Steps
- Expand automated test coverage further: the `tests/` suite now has 153 test cases covering palette storage, mesh culling, greedy mesher, LOD, neighbor accessor, face emission, density field, concurrency (shard locking, exclusive serialization, PaletteStorage R/W, cross-chunk writer races, light removal), but edge cases in light propagation BFS paths across chunk boundaries still lack regression tests.
- Play-test the far-mode tier (`lod_far_distance`/`far_detail_level`) and the nearest-first completion scheduling; tune defaults (debounce window, `lod_far_distance` vs render distance).
- No gameplay layer exists yet beyond block break/place (no inventory, crafting, mobs, multiplayer); `player.gd` remains an explicit temporary placeholder pending a C++ player controller.
- Expand fuzz coverage: harness for `propagate_chunk_block_light_additive`, harness for `build_mesh` with random block layouts.

## Critical Context
- Chunk size is 32×32×32 (`CHUNK_WIDTH`/`CHUNK_HEIGHT`/`CHUNK_DEPTH` in `chunk_coords.hpp`), world height 1024 (`WORLD_HEIGHT_Y`).
- `Camera3D::get_frustum()` returns `TypedArray<Plane>` (6 planes, world-space, normals pointing inward), recalculated every frame.
- Generator `insert` locks only one shard — doesn't block readers on other shards.
- Block + light memory per chunk: dense layout was ~130KB; paletted layout is ~1–20KB on typical (uniform-heavy) terrain.
- `data/block_definitions.json` is the single source of truth for block properties, per-face textures, and emissive maps — do not hardcode block properties in C++.
- The old merged-mesh LOD system is gone; the current LOD system is stride/detail-based inside the greedy mesher, controlled by `lod_distance`/`lod_detail_level`, with a third far-mode tier (`lod_far_distance`/`far_detail_level`) producing heightmap-only silhouette meshes for the far-region merge pipeline.
- CI runs on GitHub Actions (`.github/workflows/build.yml`): 4 legs — ubuntu TSan, ubuntu ASan+UBSan, ubuntu plain, macos plain, windows plain. Benchmark with `--check` on non-sanitizer legs.
- Light propagation: `propagate_chunk_block_light_additive` is additive-only (single-chunk, no ChunkMap needed). Multi-chunk BFS requires `LightPropagator` + ChunkMap. To simulate removal, call `clear_light()` before re-propagating.
- BFS bounded reach: max light level 15 < chunk size 32, so `lock_keys_exclusive` with 3×3×3 neighborhoods (27 keys) covers all BFS paths from a single seed chunk.

## Relevant Files
- `src/core/chunk_data.hpp/cpp`: `PaletteStorage`, `PalSection`, section-based block + light accessors, `clear_light()`, `clear_block_light()`, `clear_sky_light()`
- `src/core/chunk_map.hpp`: Sharded locking (64 shards, `ShardLock`, `lock_keys`, `lock_keys_exclusive`, `lock_all`, `lock_all_exclusive`, auto-locking methods)
- `src/core/chunk_coords.hpp`: Constants (`CHUNK_WIDTH`, `SECTION_HEIGHT`, `WORLD_HEIGHT_Y`, etc.)
- `src/core/frustum.hpp`: Frustum utility (AABB test, chunk visibility)
- `src/core/block_types.hpp/cpp`: `BlockRegistry`, `load_from_json` — reads `data/block_definitions.json`
- `src/core/crc32.hpp`: Header-only IEEE 802.3 CRC32 — used for chunk save checksum
- `src/core/performance_timer.hpp`: `TIMER_LIST` macro (includes `LightPropagation`), `PerformanceTimer`, `ScopedTimer`
- `src/core/thread_pool.hpp`: Shared worker pool, `hardware_concurrency() - 1` sizing, high-priority queue, uses `std::abort()` for TSan compat
- `data/block_definitions.json`: Single source of truth for block properties, textures, emissive maps
- `src/mesh/mesh_manager.hpp/cpp`: Per-chunk mesh builds, upload, instance management, `lod_distance`/`lod_detail_level` stride logic
- `src/mesh/mesh_builder.cpp` / `mesh_builder_greedy.cpp` / `mesh_builder_faces.cpp`: Greedy meshing, solid-cache fast paths, water/emissive face routing
- `src/mesh/chunk_neighbor_accessor.hpp/cpp`: 26 neighbor pointers for mesh building
- `src/worldgen/chunk_generator.hpp/cpp`: Biome/terrain generation — signed 3D density field (`density_noise`/`weirdness_noise`, `sample_terrain_density`, `find_surface_y`), `generate_chunk` with singleton-removal pass, `get_chunk_height_range` padded by `DENSITY_MARGIN`
- `src/worldgen/vegetation_generator.hpp/cpp`: Tree placement, cross-chunk deferred writes
- `src/worldgen/texture_array_generator.hpp`: Diffuse + emissive `Texture2DArray` generation from block definitions
- `src/world/world_updater.hpp/cpp`: `set_frustum()`, budgets, frustum pass
- `src/world/chunk_world.cpp`: Save/load (v3+CRC32), all hot paths use `lock_keys_exclusive()`
- `src/world/block_editor.cpp`: `place_block` (27 keys), `set_block_variant` (1 key)
- `src/world/player_light.hpp`: `update()` uses `lock_keys_exclusive()` with vector of up to 54 keys
- `src/lighting/light_propagator.hpp/cpp`: Public wrappers + public `_locked` variants, all 4 use `lock_keys_exclusive()`
- `src/lighting/block_light_region.hpp/cpp`: `propagate_chunk_block_light_additive()` — single-chunk additive-only
- `src/render/environment_controller.cpp`: `update_player_light` passes `ChunkMap&`, binds `_locked` BFS methods
- `src/engine/collision_resolver.hpp/cpp`: Binary-search collision
- `src/core/frame_budgets.hpp`: Budget constants (incl. `mesh_completion_budget_ms` = 0.75, `max_mesh_completions_per_frame`)
- `src/godot_bindings/chunk_manager.cpp`: All inspector-exposed properties (incl. `lod_far_distance`, `far_detail_level`), block ID constants, camera/frustum entry point
- `src/engine/voxel_engine_controller.hpp/cpp`: Bridges frustum and other state from `ChunkManager` to `WorldUpdater`
- `src/mesh/mesh_manager.cpp`: `process_completed_meshes` strict wall-clock budget + nearest-first completion scan; `compute_chunk_detail_level` three-tier; far-region debounce (`kFarRegionDebounceMs`)
- `src/world/chunk_scheduler.hpp`: `poll_completed_mesh_nearest` (deque-based completion queues, stale-epoch sweep)
- `src/mesh/mesh_builder.cpp`: `build_far_mesh` (far-mode heightmap-only emitter), `set_far_mode`
- `shaders/voxel_shader_water.gdshader`, `materials/voxel_material_water.tres`: Water rendering
- `src/render/material_manager.hpp/cpp`: `get_water_material()`, per-frame parameter push to terrain + water
- `.github/workflows/build.yml`: CI — 4-leg matrix (ubuntu TSan, ubuntu ASan+UBSan, ubuntu plain, macos plain, windows plain), benchmark with `--check`
- `tsan_suppressions.txt`: TSan suppressions for doctest/godot-cpp noise
- `LICENSE`: GPL-3.0
- `SConstruct`: Build targets (`test`, `bench`, `fuzz`), `VariantDir`, TSan/ASan support, bench sources include light propagation
- `tools/benchmark.cpp`: 4 benchmarks with `--check baseline` mode
- `tools/fuzz_palette.cpp`: libFuzzer harness for PaletteStorage random set/get
- `tools/fuzz_chunk_load.cpp`: libFuzzer harness for ChunkData header parsing
- `tests/doctest.h`: doctest v2.5.0
- `tests/test_main.cpp`: `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`
- `tests/test_concurrency.cpp`: 16 tests — TestShardMap, shard lock concurrency, ascending-order deadlock prevention, exclusive serialization, PaletteStorage R/W, pending_removals pattern, RAII correctness, OrderedExclusiveShardLock, light removal tests, cross-chunk writer race tests, pending_removals stress, serialization verification
- `tests/test_mesh_builder.cpp`: 7 tests — 2x2x2 cube, empty chunk, fully solid, LOD stride computation, LOD vertex reduction, LOD all levels valid
- `tests/test_density_field.cpp`: 4 tests — density/find_surface_y sanitation, generate_chunk↔density agreement, material validity, isolated-singleton removal
- `tests/test_palette_storage.cpp`, `test_mesh_culling.cpp`, `test_mesh_greedy.cpp`, `test_chunk_neighbor_accessor.cpp`, `test_mesh_face_emission.cpp`, `test_chunk_data.cpp`, `test_chunk_map.cpp`, `test_noise.cpp`, `test_light_propagation.cpp`
