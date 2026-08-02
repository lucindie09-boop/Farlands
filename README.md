# fuk-minecraft

A Minecraft-style voxel engine built in Godot 4 with a custom C++ GDExtension. Procedural terrain generation (signed 3D density field with overhangs and shelves), chunked world streaming, greedy meshing with per-chunk incremental rebuilds, colored block lighting, day/night cycle, multi-tier distance-based mesh LOD (including far-mode silhouette meshes), frustum-prioritized chunk loading, async background chunk saving, and a C++ inventory system (hotbar + 27-slot storage) wired into block break/place with a GDScript GUI. Ships with a C++ player controller with Minecraft-accurate fixed-timestep physics.

## Architecture

- **Godot 4** — renderer, input, audio, and UI
- **C++ GDExtension** — voxel engine core (chunking, meshing, lighting, terrain gen, collision, player sim)
- **ThreadPool** — async chunk generation, mesh building, and light propagation, sized to `hardware_concurrency() - 1` workers with a high-priority queue
- **RenderingServer** — direct GPU mesh upload for zero SceneTree overhead per chunk
- **Sharded chunk map** — 64 independently-locked shards (`shared_mutex` each), so a write on one shard never blocks readers on another
- **Palette-compressed storage** — block and light data stored as 8 paletted 16³ sections per chunk instead of dense arrays, cutting per-chunk memory from ~130KB to as little as ~1–20KB on uniform terrain
- **Frustum prioritization** — camera frustum extracted each frame; visible chunks get priority for generation, meshing, retention, and LOD detail
- **Budget-capped main thread** — generation completion, mesh uploads, and light propagation are wall-clock-budgeted per frame; the nearest-to-player completed mesh uploads first

## Key Systems

| System | File(s) | Notes |
|--------|---------|-------|
| Chunk data | `src/core/chunk_data.hpp/cpp` | 32×32×32 chunks, palette-compressed blocks + light (8 × 16³ sections each) |
| Block types | `src/core/block_types.hpp/cpp` | Registry singleton loaded from `data/block_definitions.json` — the single source of truth for block properties, per-face textures, and emissive maps |
| Chunk map | `src/core/chunk_map.hpp` | 64-shard `shared_mutex` map, ordered multi-shard locking (`lock_keys`/`lock_all`), resumable bucket-cursor iteration |
| Frustum utility | `src/core/frustum.hpp` | AABB-in-frustum test, used by generation, mesh, unload, and LOD priority |
| World updater | `src/world/world_updater.hpp/cpp` | Per-frame budgeted scheduling (generate → light → mesh → upload) |
| Generation scheduler | `src/world/generation_scheduler.hpp/cpp` | Standalone generation loop |
| Mesh queue | `src/mesh/mesh_queue.hpp` | Priority queue sorted by urgent > in-frustum > distance |
| Mesh builder | `src/mesh/mesh_builder.hpp/cpp` (+ `mesh_builder_faces.cpp`, `mesh_builder_greedy.cpp`) | Greedy + standard face culling, neighbor-aware, thread-local instances, solid-block fast path, full rebuilds and incremental partial remeshes |
| Mesh manager | `src/mesh/mesh_manager.hpp/cpp` | Upload dedup, lazy RID creation, instance budget capping, multi-tier LOD (stride/detail + far-mode), nearest-first budget-capped completion |
| Lighting | `src/lighting/light_propagator.cpp` | Async block-light propagation on worker threads, sky-light columns |
| Terrain gen | `src/worldgen/chunk_generator.hpp/cpp` | Signed 3D density field over a macro heightmap (overhangs/shelves), 4×4×4 shape lattice, biome-based macro surface, chunk-level generation fast paths |
| Vegetation | `src/worldgen/vegetation_generator.hpp/cpp` | Tree placement (oak/spruce/birch) with minimum spacing, deferred cross-chunk writes |
| Collision | `src/engine/collision_resolver.cpp` | Custom binary-search AABB voxel grid query (no Godot physics nodes), step-up support |
| Day/night | `src/world/day_night_cycle.hpp` | Shader-driven sky-light intensity + color blending |
| Player sim | `src/engine/player_controller.hpp/cpp` | Minecraft-accurate fixed 20-tick/s physics: vanilla jump/sprint/sneak ordering, accumulator, smooth eye-height transitions |
| LOD | `lod_distance` / `lod_detail_level` / `lod_far_distance` / `far_detail_level` (`mesh_manager.cpp`) | Three tiers — full detail, stride/detail reduction, far-mode heightmap-only silhouette meshes merged into far regions; capped remesh-per-frame |
| Frame budgets | `src/core/frame_budgets.hpp` | Tiered budgets for generate/light/mesh/upload (idle/active/loading) |
| Performance timers | `src/core/performance_timer.hpp` | Scoped frame-by-frame profiling |
| Inventory | `src/core/inventory.hpp/cpp` | 9-slot hotbar + 27-slot main storage, 64 stack limit, add/consume/can_add logic, persisted to `user://chunks/inventory.bin` |
| Inventory UI | `inventory.gd` / `hotbar.gd` | GDScript `Control` overlays: E toggles the full inventory, mouse wheel cycles the hotbar, click-to-hold / drag-drop stack movement, pixel-color-keyed hover/selection highlights |
| Chunk persistence | `src/world/chunk_world.cpp` | Async background saves: dirty chunks are snapshotted under their shard lock, then RLE-encoded + atomically written on the thread pool; per-key generation gating guarantees the newest data reaches disk; blocking flush on quit |

## Rendering Notes

- Opaque and water are separate mesh surfaces; water uses its own shader (`shaders/voxel_shader_water.gdshader`) with edge fade, tint, shimmer, sun glint, and Beer-Lambert depth absorption.
- Blocks can carry an emissive texture (second `Texture2DArray`) for glow, driven by `data/block_definitions.json`.
- The terrain shader (`shaders/voxel_shader.gdshader`) adds slope-triplanar cliff blending (>45° blends in rock faces), procedural wind sway on foliage, a twinkling night starfield, and a non-linear AO power curve (`pow(raw_ao, 1.35)`) that hides diagonal triangulation seams.
- The directional sun light has shadows disabled — both terrain shaders are unshaded, so the shadow pass was pure overhead with no visual effect.
- Block edits trigger an incremental partial remesh (tight dirty-AABB re-emit) instead of a full 32³ rebuild.

## Build

Requires:
- Godot 4.1+ (GDExtension `compatibility_minimum`); developed against 4.7
- Python 3 + SCons
- C++17 compiler (MSVC on Windows, GCC/Clang on Linux/macOS)
- `godot-cpp` submodule (run `git submodule update --init --recursive` after cloning)

```bash
# Build the extension library
scons

# Build standalone tools
scons debug    # terrain_debug executable
scons bench    # benchmark executable (supports --check <baseline>)
scons test     # builds the doctest suite (see tests/)
scons fuzz     # libFuzzer harnesses (Linux/macOS, clang required)
```

Optional build flags: `TSAN=1` (ThreadSanitizer), `ASAN=1` (ASan+UBSan), `COVERAGE=1` (lcov) — all Linux/macOS only.

CI (`.github/workflows/build.yml`) runs on every push and pull request:
- **Build job** — 5-leg matrix: ubuntu plain, ubuntu TSan, ubuntu ASan+UBSan, macos plain, windows plain. Tests run on every leg; the benchmark regression check (`--check benchmark_baseline.txt`) runs on the non-sanitizer legs.
- **Fuzz job** — builds and runs 4 libFuzzer harnesses (`fuzz_palette`, `fuzz_chunk_load`, `fuzz_light_propagation`, `fuzz_mesh_builder`) for 60 seconds each on Linux.
- **Static-analysis job** — clang-tidy across all of `src/` with `bugprone-*`, `concurrency-*`, and `performance-*` checks; findings in project sources fail the job.
- **Coverage job** — lcov coverage report uploaded to Codecov.

## Running

Open the project root in Godot 4 and press Play. The main scene is `Main.tscn`. The C++ extension loads automatically from the platform-specific library declared in `voxel_engine.gdextension` (e.g. `bin/libgdextension.windows.template_debug.x86_64.dll` on Windows).

## Controls

| Key | Action |
|-----|--------|
| W/A/S/D | Move |
| Mouse | Look (click the window to capture the mouse) |
| Space | Jump / ascend in flight |
| Left click | Break block (collects into inventory) |
| Right click | Place block (consumes from the selected hotbar slot) |
| Shift | Sprint |
| Ctrl | Sneak / descend in flight |
| F | Toggle fly mode |
| 1–9 | Select hotbar slot |
| E | Toggle inventory |
| Mouse wheel | Cycle hotbar selection (while the inventory is closed) |
| Esc | Release the mouse / close the inventory |

Input bindings live in `project.godot` (`move_forward`, `move_back`, `move_left`, `move_right`, `jump`, `sprint`, `sneak`, `fly_toggle`, `toggle_inventory`, `mouse_click_left`, `mouse_click_right`). The C++ `PlayerController` node owns all movement, look, block interaction, and inventory state — there is no player GDScript. The hotbar/inventory screens are GDScript `Control` overlays that read/write that state.

## Performance Tuning

The `ChunkManager` node exposes these editor properties (see `src/godot_bindings/chunk_manager.cpp`):

- **seed**, **render_distance**, **player_path**, **player_position**, **auto_update**
- **editor_enabled**, **editor_render_distance**
- **sea_level**, **biome_size** — terrain shape
- **smooth_lighting** — toggle smooth vertex lighting
- **lod_distance**, **lod_detail_level** — mid-tier mesh LOD (stride/detail reduction; see `mesh_manager.cpp`)
- **lod_far_distance**, **far_detail_level** — far-mode tier: heightmap-only silhouette meshes beyond the mid tier (0 disables)
- **player_light_enabled** / **player_light_level** — player-following dynamic light
- **day_time**, **day_night_cycle_enabled**, **day_duration**, **day_sky_intensity**/**night_sky_intensity**, **day_sky_color**/**night_sky_color**
- **fog_density** — exponential fog distance
- **vegetation_enabled** — toggle tree/vegetation generation
- **move_speed_multiplier** — global player movement speed multiplier
- **debug_enabled**, **debug_print_interval** — performance report logging
- **FrameBudgets** (in `src/core/frame_budgets.hpp`) — per-frame generation/mesh/upload caps

The `PlayerController` node exposes **sensitivity** (mouse look) and **fly_speed**.

## Notes

- The player is a C++ `PlayerController` node (`src/engine/player_controller.*` + `src/godot_bindings/player_controller.*`) — fixed 20-tick/s simulation with an accumulator, vanilla-accurate jump/sprint/sneak ordering, smooth eye-height transitions, fly mode, and raycast-based block break/place. There is no player GDScript. The GUI layer (hotbar, full inventory screen, block-texture atlas) is GDScript (`hotbar.gd`, `inventory.gd`, `block_textures.gd`).
- Modified chunks are saved to `user://chunks/` as versioned RLE-compressed `.chunk` files (v3 format with a CRC32 checksum; v2/v1 legacy files load transparently). Saves are asynchronous — `WorldUpdater` snapshots dirty chunks every 5s and writes them on the thread pool, and `ChunkManager::_exit_tree()` performs a blocking flush so nothing is lost on quit. The inventory persists to `user://chunks/inventory.bin` (magic `INVE`, version 1) and is written at `_exit_tree`.
- `analyze.py` analyzes biome maps produced by the `terrain_debug` tool; it requires `Pillow`, `numpy`, and `scipy`, which aren't otherwise part of the build.
- The LOD system is per-chunk, three-tier distance-based reduction inside the greedy mesher: full detail within `lod_distance`, stride/detail reduction in the mid tier, and heightmap-only silhouette meshes in the far tier (`lod_far_distance`/`far_detail_level`).
- Frustum prioritization requires a `Camera3D` child on the player node (named `Camera3D`).
- There is no gameplay layer yet beyond movement and block break/place with an inventory — no crafting, mobs, or multiplayer.

## License

GPL-3.0 — see `LICENSE`.
