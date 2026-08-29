## Goal
A Minecraft-style voxel engine (Godot 4 + C++ GDExtension) with chunked streaming, greedy meshing, colored lighting, biome-based terrain, and frustum/LOD-prioritized rendering.

## Constraints & Preferences
- Bias toward minimal, high-impact changes that reuse existing infrastructure
- Camera3D child named "Camera3D" on the player node is the frustum source
- Chunk size is 32×32×32, world height 1024 blocks (`WORLD_HEIGHT_Y` in `chunk_coords.hpp`)
- Block properties live in `data/block_definitions.json` — single source of truth
- Block IDs are positional (entry order = save-format ID): **always append** new blocks to `block_definitions.json`, never insert mid-list — inserting shifts every later ID and corrupts existing worlds
- Crafting recipes live in `data/recipes.json` — shaped (`pattern`+`key`) and shapeless entries resolved by block name
- Block shapes live in `data/block_shapes.json` — shared shape registry for non-full blocks (slabs, stairs, walls, poles)
- Worldgen tuning is data-driven: `data/terrain_config.json` (macro surface + height centers), `data/biomes.json` (per-biome materials/trees + climate thresholds), `data/vegetation.json` (forest/plains/desert feature knobs)

## Major Completed Work

### Core Engine Foundation
- **Frustum-prioritized loading**: Camera frustum extracted each frame; visible chunks get priority for generation, meshing, retention, and LOD detail
- **Sharded chunk map**: 64 independently-locked shards (`shared_mutex` each), so a write on one shard never blocks readers on another
- **Palette-compressed storage**: Block and light data stored as 8 paletted 16³ sections per chunk instead of dense arrays, cutting per-chunk memory from ~130KB to ~1–20KB on uniform terrain
- **Budget-capped main thread**: Generation completion, mesh uploads, and light propagation are wall-clock-budgeted per frame; nearest-to-player completed mesh uploads first
- **Dynamic mesh budget**: Scales rebuild/upload budgets by visible-chunk ratio (0.5x to 1.0x) based on viewport load
- **Targeted shard locking**: `lock_keys_exclusive()` locks only shards whose keys appear in input, reducing contention from all-64-shard locks to only the 1–54 shards actually needed
- **Work stealing thread pool**: Adaptive idle polling with work stealing for better CPU utilization
- **Per-worker task queues**: Round-robin task distribution replaces single global mutex for better throughput

### Block Shapes & Collision
- **Non-full block shapes**: Slabs (bottom/top with auto-detecting placement and double-slab merging), stairs (8 orientations with auto-detecting placement), walls (4 orientations), and poles (fence-like collision boxes that extend 1.5 blocks high)
- **Shared shape registry**: `data/block_shapes.json` defines selection boxes and collision boxes for all non-full block shapes
- **Auto-detecting placement**: Slabs, stairs, and walls automatically orient based on the face clicked and neighboring blocks
- **Proper collision and raycast**: Multi-box shapes (stairs, slabs, walls, poles) have accurate collision detection and raycast selection
- **Fixed AO and UV mapping**: Ambient occlusion and texture mapping properly handle partial blocks with their irregular geometry

### Rendering & Visual Features
- **Three-tier LOD with region merging**: Per-chunk distance-based reduction (not chunk merging) — full detail, mid stride/detail reduction, and a far tier with its own detail level + render start (`far_lod_distance`/`far_lod_detail_level`); LOD-reduced chunks are cached and merged into 8×8-chunk region instances so the coarse rings cost only a handful of draw calls
- **Dynamic water shader**: Translucent water with edge fade, bounce light, sun glint, flowing texture animation, and separate blend-mix surface
- **Vegetation generation**: Oak and spruce trees (variant-weighted per biome from `data/biomes.json`), minimum spacing, deferred cross-chunk writes, per-biome density + forest/plains/desert knobs from `data/vegetation.json`, improved forest placement (80% chunk coverage, 15-25 trees per chunk)
- **Night sky & starfield**: Dynamic procedural twinkling starfield during night sun elevations
- **Emissive texture support**: Second `Texture2DArray` for per-face glow maps
- **Soft curved AO**: Non-linear power-curve smoothing to eliminate diagonal triangulation seams
- **Fog system**: 4 fog modes (Disabled, Edge, Linear, Exponential) with fog color matching sky color throughout day/night cycle
- **God rays**: Toggleable atmospheric lighting effects with dynamic sample count and twilight optimization
- **Sky turbidity**: Rayleigh/Mie haze effects gated by sky light for proper night darkness
- **GPU texture compression**: Optional S3TC/BC1-BC3 compression for texture arrays to reduce VRAM usage
- **Vertex compression**: 24 bytes per vertex (-40% VRAM) with fixed-point positions
- **Lighting customization**: Adjustable AO color/strength, darkness color, contrast, and saturation

### Inventory & GUI
- **C++ inventory core**: `Inventory` (9 hotbar + 27 main slots, 64 stack limit) with add/consume/can_add logic in `src/core/inventory.*`
- **C++ crafting core**: `RecipeBook`/`CraftingRecipe`/`craft_item` in `src/core/crafting.*` — shapeless (sorted-multiset) and shaped (bounding-box trim + mirror) matching over an N×N grid; atomic all-or-nothing crafting; recipes load from `data/recipes.json` at startup
- **Block break/place integration**: Breaking collects into the inventory (gated by `can_add_block`); placing consumes from the selected hotbar slot
- **GDScript GUI**: `hotbar.gd` / `inventory.gd` `Control` overlays — E toggles the inventory, mouse wheel cycles the hotbar, click-to-hold / drag-drop stack movement, hover/selection highlights built by pixel-color-keyed texture recolor (no hand-drawn art)
- **Health bar**: `healthbar.gd` draws 10 hearts (`heart_full/half/empty.png`, 9×9) floating above the hotbar's left edge; sized off the hotbar's on-screen width so the row spans ~40% of it (9-texel sprites on a 10-texel pitch), linear-filtered because nearest sampling wobbles 1-texel outlines at fractional ratios; polls `PlayerController.get_health()` (half-hearts 0–20) and redraws only on change
- **2×2 crafting menu**: The atlas' color-coded slots (`#7e7d7e` inputs / `#7e7d7f` output vs. `#7e7d7d` regular) are located by their fill colors and wired to the C++ RecipeBook via `match_recipe(grid_ids, grid_counts)` (availability-gated preview — no ghost results after ingredients run out) and `craft_recipe` (atomic verify + deduct). Grid state lives GUI-side and persists across open/close so items are never lost
- **Inventory drag operations**: RMB drag-place (spread 1 unit per slot), LMB drag-collect (sweep matching blocks), shift-click/drag quick-transfer (move between hotbar/main), scroll wheel quick-transfer (push/pull 1 unit between zones), double-click gather (sweep all matching blocks into cursor); all mirrored on the crafting grid cells, shift-click output crafts as many as possible
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1), saved in `PlayerController::_exit_tree` (nodes still alive) with a cached `ChunkManager` pointer — the old destructor-time tree lookup always failed at teardown
- **Chat system**: `chat.gd` with advanced autocomplete — ghost text suggestions with pulsing effect (0.25-0.4 alpha), tab cycling through completions, up/down arrow navigation, hold-to-cycle (0.1875s intervals), parameter hints for commands (`/give <block> [count]`, `/tp <x> <y> <z>`), commands: `/help`, `/give` (unlimited count), `/tp`, `/fly`, `/clearchat`, `/clearinv`, `/version`, mouse wheel scrolling for chat history, caret blink, wrapped messages with proper input box anchoring
- **Settings menu**: `settings_menu.gd` with adjustable settings (render — including an MSAA 3D Off/2x/4x/8x cycle button that sets the root viewport's `msaa_3d` live — plus lighting, crosshair, controls) opened with Escape key, all settings persist across sessions
- **Skin Maker**: Settings → Skin Maker page — a MUNRO-font restyled ColorPicker (custom `Theme` on the picker plus per-node overrides via `_tint_picker_internals`, applied to its internal `get_children(true)` controls — bare `get_children()` returns 0 for internals in 4.7), live hex readout, and `skin_preview.gd`, a transparent-background sub-viewport that drag-orbits `player.glb` around its AABB center (the camera must NOT be a child of the rotating pivot, and 4.7's `own_world_3d` leaves `world_3d` null so the viewport environment has to be built manually). The internal picker headers (Swatches / Recent Colors) default to `font_pressed` 1.0 white — the reason hovered text looked white
- **Skin Maker paint tools**: DRAW/FILL/BOX paint tools with inclusive box semantics and off-face freeze. Grayscale noise slider owned by SkinManager (persists per skin + across restarts). Mirror paint edits into the noise base so slider changes keep your work. Fixed stale ImageTexture swaps so every model always renders the live skin.
- **Skin gallery**: 3D spinning previews with load and delete functionality. Fixed stale ImageTexture swaps so every model always renders the live skin.
- **Skin dark-mode toggle**: Top-right "DARK MODE"/"LIGHT MODE" toggle (`_skin_dark_mode`, persisted in `settings.cfg` under `gui/skin_dark_mode`); `_apply_skin_palette()` flips page bg, fg/borders, hex/hint colors, swatch tiles, and rebuilds the picker theme; the toggle's `PRESET_TOP_RIGHT` offsets must stay positive or it renders off-screen. Preview needs no changes — its transparent bg shows the page behind
- **Player model**: `player.glb` is a voxel-style model with slim 3-px arms and a tightly-packed 64×64 skin atlas laid out over pixel-face UV islands (`patch_slim.py` in temp is the source of truth if the glb ever needs re-patching — it must read the pristine file); `player_model.gd` applies the skin texture with nearest filtering (linear/mipmap blends texels across UV islands)
- **Texture pack system**: Custom texture packs with per-block texture overrides loaded from `user://packs/`, converter tool in `tools/pack_converter.py`
- **Block outline system**: Adjustable block outline with pulse effects, thickness control (0.0-0.99), fill box with separate color/opacity controls
- **Crosshair customization**: Adjustable crosshair with rotation, spacing, dot, and color-inversion modes
- **Shareable setting codes**: Crosshair and block outline settings can be exported/imported as compact base32-encoded strings (CS-style format like `FC-ABCDE-FGHIJ-KLMNO` for crosshair, `FO-12345-67890-ABCDE-FGHIJ` for outline). Codes use hyphenated 5-character chunks, versioned format for future compatibility, and clipboard integration for easy sharing. Case-insensitive decoding with on-page status hints and auto-clear after 3 seconds. Visible feedback for export/import success/failure states with consistent UX pattern.
- **Controls rebinding**: Settings → Controls page with per-action rebindable keys/buttons. Click a binding then press a key/button to rebind; Escape cancels. Bindings applied live to InputMap and persisted to settings.cfg. Per-row Reset restores the pristine project.godot binding.
- **Controls conflict detection**: Rejects rebinds that would map two actions to the same key/button with on-page hints. Physical-key aware comparison ensures captures and persistence agree.
- **Reset All controls**: Restores every action to its project.godot binding with single button click. On-page hints provide clear feedback for conflict resolution and reset operations.

### Async Chunk Saving
- **Off-main-thread writes**: `flush_dirty_chunks` deep-copies each dirty chunk under its shard lock and hands the snapshot to the thread pool for RLE encode + atomic write; periodic 5s flush is non-blocking
- **Generation gating**: per-key save generations (`next_save_generation`) so a newer snapshot supersedes an older in-flight save — superseded workers abort at their gate instead of clobbering newer data; epoch gate drops stale saves after a world reset
- **Flush on quit**: `ChunkManager::_exit_tree()` blocks on `flush_dirty_chunks(true, 5.0)` so recent edits are never lost on exit
- **Cross-chunk canopy persistence**: `apply_pending_placements` marks the receiving neighbor chunk dirty so deferred tree-canopy writes survive reload

### Terrain Generation
- **Signed 3D density field**: Overhangs, shelves, and arches via 3D fBm deformation around macro heightmap surface
- **4×4×4 world-aligned shape lattice**: Ensures bit-identical results across chunk boundaries
- **Chunk-level fast paths**: Chunks entirely above/below height band skip all lattice/density work (~7× speedup on deep chunks)
- **Biome-based macro surface**: Multiple biomes with distinct terrain characteristics
- **Continental-scale elevation noise**: Additive-only ~1000 block wavelength noise for large-scale terrain variation
- **Elevation-based weirdness amplification**: Terrain features become 1-2x more dramatic at higher elevations
- **Continentalness warp**: Wavy coastlines through continental-scale warping
- **Widened beach biome band**: Proper shoreline coverage with expanded beach biome
- **Improved water placement**: Better near-water detection and ocean floor terrain
- **Data-driven worldgen**: Surfaces, climate thresholds, tree density/variants, and macro height centers all load from JSON configs at startup (see "Worldgen Config Data" below)
- **Per-biome amplitude scaling**: Height-center `scale_m` is applied as a terrain amplitude multiplier (was previously dead config) — plains are genuinely flat, desert low/dry, forest hilly

### Worldgen Config Data
- **`data/biomes.json`** → `BiomeConfig` (`src/worldgen/biome_config.hpp`): hosts the `BiomeType` enum (Ocean/Beach/Plains/Forest/Desert), per-biome surface/subsurface + near-water block names (resolved via `BlockRegistry::get_block_id_by_name`), `tree_density`, `tree_variants` weights, and climate-grid thresholds (`temp_cold_max`/`temp_hot_min`/`hum_dry_max`/`hum_humid_min`)
- **`data/vegetation.json`** → `VegetationConfig` (`src/worldgen/vegetation_config.hpp`): forest (chunk tree chance, min/max trees, column chance, spacing, boulders), plains (single-tree chance), desert (cactus density + min/max heights)
- **`data/terrain_config.json`** → `TerrainParams::load_from_json` (`src/core/terrain_params.cpp`): `height_base_y`, fixed-base climate scales (`climate_temp_base_scale`/`climate_humidity_base_scale`), and the 3 Voronoi height centers (`temp`/`hum`/`base_off`/`scale_m`)
- **`data/block_shapes.json`** → Shared shape registry for non-full blocks (slabs, stairs, walls, poles) with selection/collision boxes
- **`data/recipes.json`** → `RecipeBook::load_from_json` (`src/core/crafting.cpp`): `grid_size` plus shaped (`pattern` + `key`, `' '` = empty) and shapeless (`ingredients`) entries; results resolved by block name; unknown names/ragged rows skip that recipe with a WARN_PRINT
- Loaded in `VoxelEngineController::load_world_configs()` at startup; missing files/keys fall back to built-in defaults that mirror the old hardcoded values
- Config threads to generation workers via `WorldUpdater` → `ChunkWorld::generate_chunk` (captured per call) → the `thread_local ChunkGenerator`

### Testing & CI
- **225 test cases / 163,385 assertions** across 24 doctest files
- **Cross-platform CI**: 5-leg matrix (ubuntu plain/TSan/ASan+UBSan, macos plain, windows plain) plus fuzz, static-analysis, and coverage jobs
- **Concurrency tests**: 27 tests for shard locking, deadlock prevention, PaletteStorage, cross-chunk writers, and thread-pool work stealing
- **Integration soak tests**: Concurrent pipeline simulation with Phase 4 unload to exercise unload vs active work races
- **Benchmark tool**: 5 hot paths with `--check` regression detection mode
- **Fuzzing**: 5 libFuzzer harnesses for palette, chunk load, chunk recovery, light propagation, and mesh builder
- **Code coverage**: lcov coverage with integration tests and memory benchmark

### Persistence & Format
- **Save format v3**: RLE-compressed with CRC32 checksum; atomic writes with `.tmp` → `.bak` → target pattern
- **Legacy support**: Handles v3 (CRC32), v2 (legacy RLE), and v1 (flat) transparently
- **Corrupted file recovery**: Attempts to load from `.bak` backup on CRC mismatch
- **Async background saves**: Dirty chunks are snapshotted and written on the thread pool; per-key generation + epoch gating ensures the newest state reaches disk
- **Sparse edit map persistence**: Replace whole-chunk snapshots with sparse edit map persistence for cross-version compatibility
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1) written at `PlayerController::_exit_tree`

### Locking & Concurrency Fixes
- **8 deadlock classes resolved**: Self-deadlock in light propagation, shard lock ordering, `_locked` method contracts, and **recursive shared shard-lock acquisition** (Windows SRW locks block new shared acquisitions once a writer is queued on a shard, so re-acquiring a shard you already hold shared freezes the whole game). The recursive class was fixed with `queue_dirty_chunk_fast()` (dirty-queue under a caller-held lock) and `ShardLock::reset()` (release-then-re-acquire for periodic lock refreshes like the block raycast's all-shard lock)
- **Player light thread safety**: Fixed unlocked writes while BFS runs concurrently
- **Cross-chunk writer race**: Fixed vegetation cross-chunk block writes with proper exclusive locking
- **Mesh-build serialization**: `MeshBuildTask::execute` holds a shared 3×3×3 `lock_keys` over the center chunk + 26 neighbors for the whole data read, serializing the build against writers (block edits, light region recomputes, player light) that mutate neighbor section palettes mid-build
- **Overlapping light removal**: Per-channel removal clears a channel only when the removed source emitted it and the cell's level is strictly below the source's; surviving channels are re-added so they refill the cleared region, and each (cell, channel) is cleared at most once so the BFS terminates with overlapping sources

### Collision & Physics
- **Binary-search AABB collision**: Custom voxel collision queries directly against chunk map
- **Multi-box collision support**: Non-full block shapes (stairs, slabs, walls, poles) have accurate multi-box collision detection
- **Step-up fix**: Tests player's full body AABB raised by step_height (old approach always failed)
- **Minecraft-accurate physics**: Fixed 20-tick/s simulation with vanilla jump/sprint/sneak ordering, removed 0.98 input scaling, proper sprint state machine with sticky flag and one-tick stale airborne, sneak multiplier only on ground
- **Move speed multiplier**: Adjustable player movement speed multiplier property
- **Fall damage**: `PlayerSim` accumulates fall distance from per-tick position deltas (including the collision-clipped landing segment — it must be added before the landing check or every fall loses its final stretch), and a landing past `SAFE_FALL_DISTANCE` (3 blocks) queues `floor(distance − 3)` half-hearts via `consume_pending_fall_damage()`; `PlayerController` applies it to a clamped `health` property (0–20 half-hearts, `get_health`/`set_health` bindings) that `healthbar.gd` renders; ascending doesn't accumulate and `reset()` (teleport/fly) clears everything
- **Death & respawn**: `set_health` reaching 0 triggers `die()` — sets a `dead_` flag that freezes `_process`/`_input` (no movement, look, break/place, hotbar), forces the mouse cursor visible via `update_mouse_mode()`, and emits `died`; `respawn()` restores full health, teleports to the `_ready`-captured spawn point (clearing fall state), emits `respawned`. `death_screen.gd` shows a vanilla-style "You died!" + Respawn overlay on those signals; inventory is kept on death

### Code Quality & Hygiene
- **GDScript lint fixes**: Fixed extra closing parentheses in timer connections, renamed shadowed parameters (scale → scale_factor to avoid Control base class conflict, char → ch to avoid shadowing built-in function), resolved integer division warnings with explicit float division and int() casts, removed redundant int division casts, dropped unused variables, cast deserialized bindings to Key/MouseButton enums, renamed lambda locals to avoid shadowing Node.name
- **Documentation format**: Migrated roadmap from .txt to .md for proper markdown formatting and GitHub rendering
- **Single source of truth**: `data/block_definitions.json` for all block properties
- **Stable block-name storage**: `BlockRegistry::load_from_json` stores `bt.name` pointers into a static `std::deque<std::string>` (was `std::vector`, which dangled every previously-stored name on reallocation — late lookups like recipe resolution read freed memory and intermittently failed)
- **Source reorganization**: Split `mesh_manager.cpp` into worker/upload/rebuild/far/lifecycle TUs (shared `mesh_manager_internal.hpp`), `mesh_builder.cpp` (`mesh_builder_solid.cpp`), and `chunk_world.cpp` (`chunk_world_edits.cpp`, `chunk_world_persistence.cpp`). Moved render-facing types out of the `core/chunk_types.hpp` junk drawer: `ChunkRenderData`/`CachedFarChunkMesh`/`CompletedMesh` → `mesh/chunk_render_data.hpp`, `DirtyChunkEntry` → `mesh/mesh_queue.hpp`, `WorldRenderStats` → `render/world_render_stats.hpp`; relocated `texture_array_generator.hpp` to `render/`; deleted orphaned `.obj` artifacts
- **Removed old LOD system**: Deleted 2,435 lines of 2×2×2 group-mesh-merging code, replaced with per-chunk three-tier LOD with region merging
- **Removed experimental features**: Cloud layer system, lighting preset system (Main/Spooky), occluder boxes - all reverted or removed
- **Terrain simplification**: Removed complex biome systems (Tundra/Taiga/Savanna/StonePlateau) and experimental mountain/erosion systems in favor of current 5-biome JSON-driven system; domain warp retained for flowing terrain ridges
- **Dead code cleanup**: `is_occluder()` method defined but unused, legacy `mountain_scale` parameter in persistence (ignored), cave system disabled (`kCavesEnabled = false`)
- **Clang compatibility**: Fixed NSDMI compile error for nested structs
- **Repo cleanup**: Removed leaked `.lnk` shortcuts, orphaned `.import` files, added `.gitignore` rules
- **License**: Added GPL-3.0 license (repo was previously all-rights-reserved)
- **Shadow optimization**: Disabled sun shadow map (both terrain shaders are unshaded)

### Performance Optimizations
- **Vertex format optimization**: Fixed-point positions reduce quad cache memory usage, ARRAY_CUSTOM format for GPU compatibility
- **Chunk generation optimization**: Fill blocks for uniform chunks, chunk-level fast paths for chunks far from terrain surface
- **AO optimization**: Pass BlockRegistry by reference, use get_block_fast() for hot path, incremental AO tracking removes redundant passes
- **GDScript performance**: Gate queue_redraw() calls, throttle block outline raycast, skip per-frame redraw churn
- **Mesh queue optimization**: O(1) mesh-queue removes, bounded lock_keys instead of whole-map locks
- **Generator buffer reuse**: Reuse generation buffers to reduce allocations
- **Shader fog gating**: Gate sun-scatter/haze math behind fog check for performance
- **LOD solid cache fix**: Fix O(stride²) solid_cache population in LOD mesh builder, stores BlockID instead of bool
- **Light propagation optimization**: Offload to worker threads with fast-path atomic check, poll results on main thread
- **Sub-chunk dirty tracking**: Fine-grained invalidation instead of full-chunk rebuilds

## Technical Details

### Locking Hierarchy
- 64 shards, each with `shared_mutex` + `unordered_map`
- Hash: `key % 64`
- Lock types: `ShardLock` (shared), `ExclusiveShardLock` (exclusive)
- Single-chunk accessors lock only their own shard
- Batch methods lock all relevant shards in ascending order to avoid deadlock
- `_locked` methods: Caller MUST already hold exclusive lock, uses `_fast` accessors only
- Auto-locking methods: Acquire their own shared locks — MUST NOT be called under exclusive lock
- Recursive shared acquisition is forbidden: never re-acquire a shard you already hold shared (e.g. calling `queue_dirty_chunk` inside a `lock_keys` scope, or building a fresh `lock_all()` while one is alive). Use `queue_dirty_chunk_fast()` / `get_chunk_render_data_fast()` and `ShardLock::reset()` before re-locking. Windows SRW blocks new shared acquisitions once a writer is queued, turning the recursion into a hard deadlock

### Targeted Shard Locking Usage
- `set_block_variant()` — 1 chunk key
- `propagate_block_light_region()` — 27 keys (3×3×3 neighborhood)
- `place_block` — 27 keys (3×3×3 center)
- `light_propagate_add` / `light_propagate_remove` — origin 3×3×3 + each seed node's 3×3×3 (deduplicated)
- `update_block_light_incremental` — 54 keys (origin + center 3×3×3)
- `PlayerLight::update` — vector of up to 54 keys (old+new chunk 3×3×3)
- `MeshBuildTask::execute` — 27 keys (center + 26 neighbors), **shared** `lock_keys` held for the whole data read

### LOD System Details
- Three tiers: full detail → mid stride/detail reduction → far tier with its own detail level
- Per-tier stride-1 "skirt" rings at each LOD transition prevent T-junction cracks
- Cap of 512 LOD remeshes/frame
- Far-region rebuilds debounced (250 ms)

### Save Format v3
- Header: `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32]`
- Body: RLE-compressed block data
- CRC32 verified on load; corrupted files rejected
- Atomic write pattern: `.tmp` → `.bak` → target

## Notes
- For current architecture details, see [ARCHITECTURE.md](ARCHITECTURE.md)
- For build and running instructions, see [README.md](README.md)
- For project roadmap, see [roadmap.md](roadmap.md)
