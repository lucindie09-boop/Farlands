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
- **Non-full block shapes**: Slabs, stairs, walls, and poles defined in `data/block_shapes.json`
- **Selection and collision boxes**: Each shape variant has explicit selection boxes (for raycast) and collision boxes (for physics)
- **Auto-detecting placement**: Slabs, stairs, and walls automatically orient based on clicked face and neighboring blocks
- **Double-slab merging**: Two stacked slabs of the same type merge into a double slab; breaking drops 2 slabs
- **Pole collision**: Fence-like collision boxes extend 1.5 blocks high for proper player interaction

## Terrain Generation

- **Signed 3D density field** over a macro heightmap: `density = (surface_y - y) + shape_noise * strength * surface_band` (positive = solid). The 3D fBm deforms only a band around the macro surface, producing overhangs, shelves, and arches.
- **4×4×4 world-aligned shape lattice**: the 3D shape noise is sampled once per lattice node and trilinearly interpolated per voxel, so chunk grids and single-point field queries stay bit-identical and lattice nodes land on shared world coordinates across chunk boundaries (no seams).
- **Chunk-level fast paths**: after the exact macro column pass, chunks entirely above/below the per-chunk height band fill plain water/air or stone over bedrock and skip all lattice/density/material work (~7× on deep-chunk generation).
- **Continental-scale elevation noise**: Additive-only ~1000 block wavelength noise for large-scale terrain variation
- **Elevation-based weirdness amplification**: Terrain features become 1-2x more dramatic at higher elevations
- **Continentalness warp**: Wavy coastlines through continental-scale warping
- **Widened beach biome band**: Proper shoreline coverage with expanded beach biome
- **Improved water placement**: Better near-water detection and ocean floor terrain
- **Data-driven worldgen**: Surfaces, climate thresholds, tree density/variants, and macro height centers all load from JSON configs at startup (`data/biomes.json`, `data/vegetation.json`, `data/terrain_config.json`).
- **Per-biome amplitude scaling**: Height-center `scale_m` is applied as a terrain amplitude multiplier — plains are genuinely flat, desert low/dry, forest hilly.
- Vegetation uses the real density surface with an underwater rejection guard; an isolated-singleton removal pass clears lone floating voxels the density field occasionally produces.

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

### Mesh Surfaces
- Primary surface: opaque terrain with greedy meshing
- Secondary surface: translucent water with edge fade, tint, shimmer, flowing texture animation, and separate blend-mix surface
- Emissive textures: second `Texture2DArray` for per-face glow maps
- Far regions: LOD-reduced chunk meshes merged into region instances (`far_regions`, `far_mesh_cache` in `mesh_manager.*`/`chunk_render_data.hpp`) so the coarse ring costs a handful of draw calls
- Vertex compression: 24 bytes per vertex (-40% VRAM) with fixed-point positions

## Collision

- Binary-search AABB approach (3D DDA variant was tried and reverted)
- Custom voxel collision queries chunk map directly instead of Godot physics nodes
- Player collision via `ChunkManager::resolve_voxel_collision()`
- **Step-up**: tests the player's full body AABB raised by `step_height`, then re-resolves horizontally and only accepts the step if it travels further than not stepping. (The old `[feet, feet+step_height)` headroom probe always contained the obstruction being stepped onto and could never succeed.)

## Player Controller

Two layers mirroring the ChunkManager/VoxelEngineController pattern:

- **`VoxelEngine::PlayerSim`** (`src/engine/player_controller.*`) — pure, deterministic fixed-timestep simulation (20 ticks/s, velocities in blocks/tick). Vanilla-accurate ordering (jump + sprint boost applied before friction, matching `tickMovement()` → `travel()`), sticky sprint with a one-tick airborne staleness, sneak multiplier on ground only, and `on_floor` derived from the final resolved position (no swept floor probe). Standing/sneaking/landing eye-height transitions are smoothed at render time from the accumulator's partial-tick fraction. Also tracks fall distance from per-tick position deltas; a landing past `SAFE_FALL_DISTANCE` (3 blocks) queues `floor(distance − 3)` half-hearts for `consume_pending_fall_damage()`.
- **`PlayerController`** (`src/godot_bindings/player_controller.*`) — Godot `Node3D` scene node registered via ClassDB (used directly by `Main.tscn`). Polls input, drives the tick accumulator from `_process(delta)`, handles mouse look, fly mode (`fly_speed`), camera eye-height smoothing, raycast-based break/place, and block selection (keys 1–9). After ticking it consumes queued fall damage into a clamped `health` property (0–20 half-hearts, `get_health`/`set_health` bindings).
- **Health integration** — `healthbar.gd` renders 10 hearts (`heart_full/half/empty.png`, 9×9 art) above the hotbar's left edge, sized off the hotbar's on-screen width so the row spans ~40% of it (9-texel sprites on a 10-texel pitch). It polls `get_health()` each frame and redraws only on change. Hearts are linear-filtered: the row's fractional scale makes nearest sampling render 1-texel outlines at inconsistent widths.
- **Death & respawn** — `set_health` hitting 0 calls `die()`: a `dead_` flag freezes `_process`/`_input` (movement, look, break/place, hotbar keys), `update_mouse_mode()` releases the cursor, and the `died` signal fires. `respawn()` restores full health, teleports to the spawn point captured in `_ready` (via `teleport_to`, clearing fall state), and emits `respawned`. `death_screen.gd` (HUD overlay) listens to both signals and shows/hides a "You died!" + Respawn button screen; the button calls `respawn()`. Inventory is kept on death.
- **Inventory integration** — `PlayerController` owns a `VoxelEngine::Inventory` (9 hotbar + 27 main slots, 64 stack limit). Breaking a block collects it only if `can_add_block` succeeds; placing consumes from the selected hotbar slot. Hotbar/inventory state is exposed to GDScript via ClassDB bindings (`get_hotbar_slot_block_id`, `set_inventory_slot`, `select_hotbar_slot`, etc.) and rendered by the `hotbar.gd` / `inventory.gd` `Control` overlays (E toggles, mouse wheel cycles the hotbar, click-to-hold/drag-drop stack movement). The inventory UI uses isometric 3D block icons (300×300) rendered by `BlockIconRenderer` at Minecraft's dimetric angle (45° yaw, 30° pitch) with support for custom block shapes (slabs, stairs, walls, poles) built from `data/block_shapes.json` selection boxes. Icons are pre-rendered asynchronously at startup and cached for performance.
- **Crafting integration** — `RecipeBook` (`src/core/crafting.*`) loads recipes from `data/recipes.json` in `load_world_configs()` and is exposed through two ClassDB bindings: `match_recipe(grid_ids, grid_counts)` previews the output slot (gated on ingredient availability so the preview disappears once the grid runs dry), and `craft_recipe(grid_ids, grid_counts)` atomically verifies + deducts the grid and returns the new counts plus the result. The 2×2 grid lives GUI-side in `inventory.gd` (contents persist across open/close); crafting-area geometry is measured from the color-coded slot pixels in the atlas (`#7e7d7e` inputs, `#7e7d7f` output vs. `#7e7d7d` regular slots).
- **Chat integration** — `PlayerController` provides chat state management (`set_chat_open`, `is_chat_open`) and inventory clearing (`clear_inventory`). The chat system (`chat.gd`) features advanced autocomplete with ghost text suggestions, tab cycling, and parameter hints.
- **Lifecycle hooks** — `PlayerController::_ready()` caches the `ChunkManager` pointer (no per-call tree lookups) and loads the saved inventory; `_exit_tree()` saves the inventory while every node is still allocated, guarded by `inventory_saved_` so the destructor's fallback save is a no-op.
- **Viewmodel & animation** — First-person hand + held item/block live in `viewmodel.gd` (child of `Camera3D`, eye space). The punch (0.225s) drives an arm depth curve reshaped by a cubic smoothstep plus a two-sided circular arc sweep; the held item/block swings are applied inside `_update_item_transform`/`_update_block_transform` on `_item_scale_node` (these own that node each frame, so parent-pivot offsets can't hit the tuned poses). The punch **loops while breaking** (`get_break_state()["active"]`) and is gated on captured mouse so UI clicks never swing. A separate weaker **place animation** (75% endpoint) fires only on the C++ `block_placed` signal (verified land + inventory consumed). Walk bobbing (`_walk_dist*PI*0.6`) uses a `_bob` envelope that decays to zero when airborne, driven by the `PlayerController::is_on_floor()` binding (wraps `sim_.is_on_floor()`).
- **Block break progress** — `update_break_progress` accumulates `delta / hardness` while LMB is held on the raycast target; releasing LMB or losing the target resets `break_progress_`/`break_target_valid_` (so the crack and looping punch stop). `get_break_state()` exposes `{active, x/y/z, stage 0-9}` to `block_break_overlay.gd`.

No `CharacterBody3D`, `move_and_slide`, or `CollisionShape3D` — all collision goes through `CollisionResolver` against the chunk map.

## Removed/Experimental Features

The following experimental features were attempted but removed or reverted:

- **2×2×2 LOD group merging**: Replaced with per-chunk three-tier LOD with 8×8 region merging
- **Cloud layer system**: Removed atmospheric cloud layer with fbm noise
- **Lighting preset system**: Reverted Main/Spooky preset system with separate visual sky
- **Occluder boxes**: Reverted Godot occluder boxes for fully-solid chunks
- **Complex biome systems**: Removed Tundra/Taiga/Savanna/StonePlateau biomes in favor of current 5-biome JSON system
- **Erosion-driven mountains**: Removed experimental mountain generation systems
- **3D DDA collision**: Reverted to binary-search AABB collision
- **11-biome climate system**: Simplified from 11 biomes to current 5-biome temperature/humidity grid

## Legacy/Disabled Code

The following code remains in the codebase but is disabled or unused:

- **Cave system**: `kCavesEnabled = false` in `ChunkGenerator` - cave carving code exists but is globally disabled
- **is_occluder() method**: Defined in `ChunkNeighborAccessor` but never called anywhere in the codebase
- **mountain_scale parameter**: Read from save files in persistence but ignored in current terrain generation
- **Domain warp**: Anisotropic domain warp code still present in terrain generation but its impact is minimal with current parameters (provides flowing terrain ridges)

## Key Files

### Core
- `src/core/chunk_data.hpp/cpp` — `PaletteStorage`, `PalSection`, section-based accessors
- `src/core/chunk_map.hpp` — Sharded locking, `lock_keys_exclusive`, auto-locking methods
- `src/core/chunk_coords.hpp` — Constants (`CHUNK_WIDTH`, `SECTION_HEIGHT`, `WORLD_HEIGHT_Y`)
- `src/core/frustum.hpp` — Frustum utility (AABB test, chunk visibility)
- `src/core/block_types.hpp/cpp` — `BlockRegistry`, `load_from_json`
- `src/core/inventory.hpp/cpp` — `Inventory`, `InventorySlot`: hotbar/main storage, add/consume/can_add, 64 stack limit
- `src/core/crafting.hpp/cpp` — `RecipeBook`, `CraftingRecipe`, `craft_item`: shapeless (sorted-multiset) and shaped (bounding-box trim + mirror) matching over an N×N grid; Godot-guarded JSON loader keeps the matching core fuzz/test friendly
- `src/core/crc32.hpp` — IEEE 802.3 CRC32 for chunk save checksum
- `src/core/thread_pool.hpp` — Shared worker pool, high-priority queue

### Mesh
- `src/mesh/mesh_manager.hpp` + `mesh_manager.cpp` / `mesh_manager_worker.cpp` / `mesh_manager_upload.cpp` / `mesh_manager_rebuild.cpp` / `mesh_manager_far.cpp` / `mesh_manager_lifecycle.cpp` / `mesh_manager_internal.hpp` — Per-chunk mesh builds, upload, instance management, three-tier LOD, far-region merging, nearest-first completion
- `src/mesh/mesh_builder.cpp` / `mesh_builder_solid.cpp` / `mesh_builder_greedy.cpp` / `mesh_builder_faces.cpp` — Greedy meshing, incremental partial remeshes
- `src/mesh/chunk_neighbor_accessor.hpp/cpp` — 26 neighbor pointers for mesh building
- `src/mesh/chunk_render_data.hpp` — `ChunkRenderData` (per-chunk render state stored in the chunk map), `CachedFarChunkMesh`, `CompletedMesh`
- `src/mesh/mesh_types.hpp` — Mesh types, light checksum grid for incremental rebuilds
- `src/mesh/mesh_queue.hpp` — `DirtyChunkEntry` + frustum/distance-prioritized mesh rebuild queue

### World
- `src/world/chunk_world.cpp` + `chunk_world_edits.cpp` / `chunk_world_persistence.cpp` — Edit application (block edits, pending/vegetation placements, unload/clear) and save/load (async `flush_dirty_chunks`, generation + epoch gated `enqueue_chunk_save` / `save_chunk_snapshot`, `write_chunk_file_locked`, inventory save/load). All hot paths use `lock_keys_exclusive()`
- `src/world/block_editor.cpp` — `place_block` with targeted locking
- `src/world/player_light.hpp` — Player light with targeted locking
- `src/world/world_updater.hpp/cpp` — Frustum integration, budgets, periodic dirty flush
- `src/world/chunk_scheduler.hpp` — Completion queues, `poll_completed_mesh_nearest`
- `src/world/day_night_cycle.hpp` — Sky-light cycle

### Worldgen
- `src/worldgen/chunk_generator.hpp/cpp` — Signed 3D density field, 4×4×4 shape lattice, biome-based macro surface, chunk-level fast paths
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
- `data/recipes.json` — Crafting recipes (shaped/shapeless), resolved by block name; loaded into `RecipeBook` at startup
- `settings_menu.gd` — Adjustable settings with persistence (render, lighting, crosshair, controls) opened with Escape key; includes a **Skin Maker** page (color wheel, hex readout, orbitable preview) with a dark-mode toggle and a **Block Maker** page (16×16 cube painter) with paint tools, noise slider, and gallery
- `skin_preview.gd` — Transparent-background sub-viewport that orbits `player.glb` behind the skin maker; the camera orbits the model's AABB center rather than being a child of the rotating node
- `block_manager.gd` — Autoload holding the single persistent 16×16 block texture (one `ImageTexture` shared by every cube face), with debounced saves to `user://current_block.png`, a restart-recovery noise base (`user://block_noise_base.png`), and a reversible grayscale-noise slider living on the autoload so it survives page rebuilds
- `block_preview.gd` — Transparent-background sub-viewport behind the block maker that drag-orbits a cube; DRAW/FILL/BOX painting over primitive triangle raycasts, undo (Ctrl+Z), noise slider integration, and clamped zoom
- `player_model.gd` — Applies the skin texture to `player.glb`'s `StandardMaterial3D` surfaces with nearest filtering (no mipmaps, avoiding smeared UV islands)
- `player.glb` — Voxel-style player model with a tightly-packed 64×64 skin-texture atlas
- `viewmodel.gd` — First-person hand + held item/block (child of `Camera3D`): punch/swing + place animations, looping break punch, walk bobbing
- `block_break_overlay.gd` — Draws the 10-stage crack overlay (`textures/animated/l0_sprite_01..10.png`) on the mined block, driven by `get_break_state()`
- `block_textures.gd` — Block texture atlas generation from `textures/blocks/`

### Engine
- `src/engine/collision_resolver.hpp/cpp` — Binary-search collision, step-up
- `src/engine/player_controller.hpp/cpp` — `PlayerSim` (fixed-timestep simulation, fall-distance tracking + landing damage)
- `src/engine/voxel_engine_controller.hpp/cpp` — Bridges `ChunkManager` state to the world

### Rendering
- `src/render/environment_controller.cpp` — Sky/fog/player-light parameter pushes
- `src/render/material_manager.hpp/cpp` — Terrain + water materials, texture arrays
- `src/render/texture_array_generator.hpp` — Diffuse + emissive `Texture2DArray` generation
- `src/render/world_render_stats.hpp` — `WorldRenderStats` snapshot consumed by `PerfReport`

### Godot bindings
- `src/godot_bindings/chunk_manager.cpp` — Inspector properties, camera/frustum entry point, block API, `flush_dirty_chunks`, `_exit_tree` quit flush
- `src/godot_bindings/player_controller.cpp` — `PlayerController` node: input, mouse look, fly mode, break/place, inventory bindings, chat bindings, health bindings, `_exit_tree` inventory save

### Lighting
- `src/lighting/light_propagator.hpp/cpp` — Public wrappers + `_locked` variants
- `src/lighting/block_light_region.hpp/cpp` — Single-chunk additive-only propagation

### Data
- `data/block_definitions.json` — Single source of truth for block properties
- `data/block_shapes.json` — Shared shape registry for non-full blocks (slabs, stairs, walls, poles)
- `data/biomes.json` — Biome definitions with per-biome materials, climate thresholds, tree density, and tree variant weights
- `data/vegetation.json` — Vegetation parameters for forest/plains/desert biomes
- `data/terrain_config.json` — Macro height centers, climate scales, and terrain amplitude parameters
- `textures/` — Asset organization:
  - `textures/blocks/` — Block textures (bedrock, dirt, grass, stone, sand, water, etc.)
  - `textures/gui/` — UI textures (hotbar, inventory background, effects)
  - `textures/sprites/` — Sprite textures (hearts, etc.)
  - `textures/atmosphere/` — Atmospheric textures (sun, north star)
  - `textures/Archive/` — Archived/deprecated textures (old versions kept for reference)

### Testing
- `tests/` — 24 test files, 225 test cases / 163,385 assertions, auto-discovered via `Glob("tests/*.cpp")`
- `tests/test_concurrency.cpp` — 27 tests for shard locking, deadlock prevention, PaletteStorage, cross-chunk writers, and thread-pool work stealing
- `tests/test_inventory.cpp` — Inventory add/consume/edge-case tests
- `tests/test_crafting.cpp` — Shapeless/shaped matching (trim, mirror, offset, rotation/partial misses) + atomic craft_item tests (success, insufficient ingredients, full inventory rejection)
- `tests/test_light_propagation.cpp` — Cross-chunk BFS edge case tests
- `tests/test_light_removal.cpp` — Overlapping multi-source light removal tests
- `tests/test_player_controller.cpp` — PlayerSim movement/sprint/sneak/collision tests, fall damage (safe jump landing, 7.5-block drop → 4 half-hearts, teleport reset clearing pending damage)
- `tests/test_soak.cpp` — Multi-threaded fly-through-the-world stress test
- `tests/test_persistence.cpp`, `tests/test_collision_resolver.cpp`, `tests/test_density_field.cpp` — format, collision, and terrain tests
- `tools/benchmark.cpp` — 5 hot paths + memory, with `--check <baseline>` regression mode
- `tools/fuzz_*.cpp` — libFuzzer harnesses (`fuzz_palette`, `fuzz_chunk_load`, `fuzz_chunk_recovery`, `fuzz_light_propagation`, `fuzz_mesh_builder`)
