## Goal
A Minecraft-style voxel engine (Godot 4 + C++ GDExtension) with chunked streaming, greedy meshing, colored lighting, biome-based terrain, and frustum/LOD-prioritized rendering.

## Constraints & Preferences
- Bias toward minimal, high-impact changes that reuse existing infrastructure
- Camera3D child named "Camera3D" on the player node is the frustum source
- Chunk size is 32×32×32, world height 1024 blocks (`WORLD_HEIGHT_Y` in `chunk_coords.hpp`)
- Block properties live in `data/block_definitions.json` — single source of truth

## Major Completed Work

### Core Engine Foundation
- **Frustum-prioritized loading**: Camera frustum extracted each frame; visible chunks get priority for generation, meshing, retention, and LOD detail
- **Sharded chunk map**: 64 independently-locked shards (`shared_mutex` each), so a write on one shard never blocks readers on another
- **Palette-compressed storage**: Block and light data stored as 8 paletted 16³ sections per chunk instead of dense arrays, cutting per-chunk memory from ~130KB to ~1–20KB on uniform terrain
- **Budget-capped main thread**: Generation completion, mesh uploads, and light propagation are wall-clock-budgeted per frame; nearest-to-player completed mesh uploads first
- **Targeted shard locking**: `lock_keys_exclusive()` locks only shards whose keys appear in input, reducing contention from all-64-shard locks to only the 1–54 shards actually needed

### Rendering & Visual Features
- **Three-tier LOD system**: Per-chunk distance-based reduction (not chunk merging) — full detail, stride/detail reduction, and far-mode heightmap-only silhouette meshes
- **Dynamic water shader**: Translucent water with edge fade, depth absorption, bounce light, and sun glint
- **Vegetation generation**: Oak, spruce, and birch trees with minimum spacing, deferred cross-chunk writes
- **Slope triplanar cliff blending**: Steep slopes (>45°) automatically blend in rock face textures
- **Night sky & starfield**: Dynamic procedural twinkling starfield during night sun elevations
- **Emissive texture support**: Second `Texture2DArray` for per-face glow maps
- **Soft curved AO**: Non-linear power-curve smoothing to eliminate diagonal triangulation seams

### Inventory & GUI
- **C++ inventory core**: `Inventory` (9 hotbar + 27 main slots, 64 stack limit) with add/consume/can_add logic in `src/core/inventory.*`
- **Block break/place integration**: Breaking collects into the inventory (gated by `can_add_block`); placing consumes from the selected hotbar slot
- **GDScript GUI**: `hotbar.gd` / `inventory.gd` `Control` overlays — E toggles the inventory, mouse wheel cycles the hotbar, click-to-hold / drag-drop stack movement, hover/selection highlights built by pixel-color-keyed texture recolor (no hand-drawn art)
- **Inventory drag operations**: RMB drag-place (spread 1 unit per slot), LMB drag-collect (sweep matching blocks), shift-click/drag quick-transfer (move between hotbar/main), scroll wheel quick-transfer (push/pull 1 unit between zones), double-click gather (sweep all matching blocks into cursor)
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1), saved in `PlayerController::_exit_tree` (nodes still alive) with a cached `ChunkManager` pointer — the old destructor-time tree lookup always failed at teardown
- **Chat system**: `chat.gd` with advanced autocomplete — ghost text suggestions with pulsing effect (0.25-0.4 alpha), tab cycling through completions, up/down arrow navigation, hold-to-cycle (0.1875s intervals), parameter hints for commands (`/give <block> [count]`, `/tp <x> <y> <z>`), commands: `/help`, `/give`, `/tp`, `/fly`, `/clearchat`, `/clearinv`, `/version`

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

### Testing & CI
- **164 test cases / 91,454 assertions** across 18 doctest files
- **Cross-platform CI**: 5-leg matrix (ubuntu plain/TSan/ASan+UBSan, macos plain, windows plain) plus fuzz, static-analysis, and coverage jobs
- **Concurrency tests**: 19 tests for shard locking, deadlock prevention, PaletteStorage, and cross-chunk patterns
- **Benchmark tool**: 5 hot paths with `--check` regression detection mode
- **Fuzzing**: 4 libFuzzer harnesses for palette, chunk load, light propagation, and mesh builder

### Persistence & Format
- **Save format v3**: RLE-compressed with CRC32 checksum; atomic writes with `.tmp` → `.bak` → target pattern
- **Legacy support**: Handles v3 (CRC32), v2 (legacy RLE), and v1 (flat) transparently
- **Corrupted file recovery**: Attempts to load from `.bak` backup on CRC mismatch
- **Async background saves**: Dirty chunks are snapshotted and written on the thread pool; per-key generation + epoch gating ensures the newest state reaches disk
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1) written at `PlayerController::_exit_tree`

### Locking & Concurrency Fixes
- **7 deadlock classes resolved**: Fixed self-deadlock in light propagation, shard lock ordering, and `_locked` method contracts
- **Player light thread safety**: Fixed unlocked writes while BFS runs concurrently
- **Cross-chunk writer race**: Fixed vegetation cross-chunk block writes with proper exclusive locking
- **Neighbor chunk pinning**: Fixed use-after-free by pinning chunks during mesh build

### Collision & Physics
- **Binary-search AABB collision**: Custom voxel collision queries directly against chunk map
- **Step-up fix**: Tests player's full body AABB raised by step_height (old approach always failed)
- **Minecraft-accurate physics**: Fixed 20-tick/s simulation with vanilla jump/sprint/sneak ordering

### Code Quality & Hygiene
- **Single source of truth**: `data/block_definitions.json` for all block properties
- **Removed old LOD system**: Deleted 2,435 lines of 2×2×2 group-mesh-merging code
- **Clang compatibility**: Fixed NSDMI compile error for nested structs
- **Repo cleanup**: Removed leaked `.lnk` shortcuts, orphaned `.import` files, added `.gitignore` rules
- **License**: Added GPL-3.0 license (repo was previously all-rights-reserved)
- **Shadow optimization**: Disabled sun shadow map (both terrain shaders are unshaded)

## Technical Details

### Locking Hierarchy
- 64 shards, each with `shared_mutex` + `unordered_map`
- Hash: `key % 64`
- Lock types: `ShardLock` (shared), `ExclusiveShardLock` (exclusive)
- Single-chunk accessors lock only their own shard
- Batch methods lock all relevant shards in ascending order to avoid deadlock
- `_locked` methods: Caller MUST already hold exclusive lock, uses `_fast` accessors only
- Auto-locking methods: Acquire their own shared locks — MUST NOT be called under exclusive lock

### Targeted Shard Locking Usage
- `set_block_variant()` — 1 chunk key
- `propagate_block_light_region()` — 27 keys (3×3×3 neighborhood)
- `place_block` — 27 keys (3×3×3 center)
- `light_propagate_add` / `light_propagate_remove` — origin 3×3×3 + each seed node's 3×3×3 (deduplicated)
- `update_block_light_incremental` — 54 keys (origin + center 3×3×3)
- `PlayerLight::update` — vector of up to 54 keys (old+new chunk 3×3×3)

### LOD System Details
- Three tiers: full detail → stride/detail reduction → far-mode silhouette
- Stride-1 "skirt" ring at mid-tier transition prevents T-junction cracks
- Cap of 128 LOD remeshes/frame
- Far-region rebuilds debounced (250 ms)

### Save Format v3
- Header: `[width:u32][height:u32][depth:u32][version:u32=3][crc32:u32]`
- Body: RLE-compressed block data
- CRC32 verified on load; corrupted files rejected
- Atomic write pattern: `.tmp` → `.bak` → target

## Notes
- For current architecture details, see [ARCHITECTURE.md](ARCHITECTURE.md)
- For build and running instructions, see [README.md](README.md)
