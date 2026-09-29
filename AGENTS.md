## Goal
A Minecraft-style voxel engine (Godot 4 + C++ GDExtension) with chunked streaming, greedy
meshing, colored lighting, biome-based terrain, and frustum/LOD-prioritized rendering.

This file is the **engineering record**: the constraints that are not negotiable and the notes
that cut across systems (locking, the save format, testing and CI). For the current
description of how things work see [ARCHITECTURE.md](ARCHITECTURE.md); for anything
per-system, the description and the notes for it now live in `docs/` — shapes
([shapes.md](docs/shapes.md) / [shapes-notes.md](docs/shapes-notes.md)), streaming
([streaming.md](docs/streaming.md)), rendering ([rendering-notes.md](docs/rendering-notes.md)),
inventory and GUI ([gui-notes.md](docs/gui-notes.md)), the GDScript UI ([ui.md](docs/ui.md)),
build files ([schematic-notes.md](docs/schematic-notes.md)), terrain
([terrain-notes.md](docs/terrain-notes.md)), pathfinding ([pathfinding.md](docs/pathfinding.md)),
fluids ([fluids.md](docs/fluids.md)) and the test/CI rules ([testing.md](docs/testing.md)).
For the tasks, schemas, vocabulary and instruments, [docs/](docs/README.md).

Every heading under *Major completed work* is a summary with a link; the full notes for a
subsystem are the file the summary points at, so nothing is written twice.

**Contents** — [Goal](#goal) · [Constraints & preferences](#constraints--preferences) ·
[Major completed work](#major-completed-work): core engine, terrain generation, worldgen
config, pathfinding, block files, fluids, testing and CI - each a summary of the page it links
· [Technical details](#technical-details): locking, LOD, save format · [Notes](#notes)

The four gates this file's constraints describe are `scons sizecheck`, `scons portability`,
`scons docscheck` and clang-tidy — see [docs/README.md](docs/README.md).

## Constraints & Preferences
- Bias toward minimal, high-impact changes that reuse existing infrastructure
- **Never declare `static` (including `static inline` member) variables of godot-cpp types**
  (`PackedStringArray`, `String`, `Array`, `Dictionary`, `Image`, ...) at namespace/class scope
  in extension code: their constructors call into the gdextension interface table, which is a
  null function pointer until the engine calls `gdextension_init` — the static init runs at DLL
  load and crashes it (Godot reports "Can't open dynamic library ... Error 5: Access is denied",
  raw `LoadLibraryW` gives WinError 1114). Use std types (`std::vector<std::string>`, ...) or
  function-local statics built on first use. `Ref<T>` and `std::unordered_map`/`bool` statics
  are safe.
- Camera3D child named "Camera3D" on the player node is the frustum source
- Chunk size is 32×32×32, world height 1024 blocks (`WORLD_HEIGHT_Y` in `chunk_coords.hpp`)
- Block properties live in `data/block_definitions.json` — single source of truth
- Block IDs are positional (entry order = save-format ID): **always append** new blocks to
  `block_definitions.json`, never insert mid-list — inserting shifts every later ID and corrupts
  existing worlds
- Crafting recipes live in `data/recipes.json` — shaped (`pattern`+`key`) and shapeless entries
  resolved by block name; a `key` entry may list several acceptable ingredients (expanded per
  symbol at load)
- Block shapes live in `data/block_shapes.json` — shared shape registry for non-full blocks
  (slabs, stairs, walls, poles)
- Worldgen tuning is data-driven: `data/terrain_config.json` (macro-surface layers: base height,
  warp amps, relief fields, strength range, weirdness thresholds, climate base scales),
  `data/biomes.json` (per-biome surface materials, amplification, tree variants, preferred
  climate), `data/vegetation.json` (sparse-tree knobs)
- glTF (and .glb) has **no** Blockbench pivot metadata — a node's origin IS a part's rotation
  anchor for every importer (Godot, Blender, three.js). Blockbench 5.1.6's glTF exporter
  flattens cube pivots by anchoring nodes at bottom corners, so a part swings around the wrong
  point unless the file is fixed. `player.glb` is re-baked by `tools/rebake_player_pivots.py`
  (idempotent): it moves each node onto its true joint and shifts that mesh's vertices by the
  delta so world placement stays byte-identical. After re-baking, delete
  `.godot/imported/player.glb-*` so Godot reimports
- **No C++ source file above 500 lines** (`.cpp` or `.hpp`, in `src/`, `tests/` and `tools/`).
  `scons sizecheck` (and the CI step that runs it) fails with the offender list and warns at
  480. Split along responsibilities rather than by size: the original filename stays the entry
  point, new pieces are `<name>_<topic>.cpp` beside it, helpers two files need go in
  `<name>_internal.hpp` (and test fixtures in `tests/<name>_test_support.hpp`), and a split
  moves text verbatim before it changes any. The recipe, the per-phase history and the traps are
  in `docs/file_size_plan.md`. Two constraints that are easy to miss: a new `.cpp` is globbed by
  `SConstruct` (no edit needed) unless its file is named in `shared_sources`, the fuzz source
  lists or a tool's `Program()`, and a helper that was `static` (or in an anonymous namespace)
  has to become `inline` in a named namespace once two translation units share it — a header
  does not inherit the includer's `using namespace` either. Those fuzz source lists are also
  **godot-free by construction**: the harnesses link a pure core with
  `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`, so a source in one of them may not reach a
  registry or loader that sits behind that define — it links on no machine but the CI runner,
  and a file is in the list because a harness needs it, not because it is nearby
- **`_WIN32` is the only gate for Windows-only code, and standard attributes come first.** MSVC
  accepts `inline [[nodiscard]] bool f()` and finds `dbghelp.h`; GCC and clang reject both, and
  the Linux jobs run only in CI, so such a mistake sits in the tree looking green.
  `scons portability` (and the CI step) fails when a Windows-only include,
  `#pragma comment`/`#pragma warning`, an MSVC intrinsic or a Win32 type sits outside an
  `#ifdef _WIN32` region, or when `[[...]]` is written after a decl-specifier.
  `python tools/check_portability.py --selftest` proves the checks fire. Two more members of
  that class, worth knowing before a change that creates files: a strict second opinion is one
  command away — `clang-cl -fsyntax-only` with the flags from `compile_commands.json`, since
  clang diagnoses what MSVC waves through — and a helper local to one `.cpp` belongs in an
  anonymous namespace (or `static`), or its external linkage can collide with the same name in a
  sibling file at link time
- **The static-analysis job is the third structural gate, and the only one that reads C++
  semantics rather than shapes.** clang-tidy runs over `find src -name '*.cpp'` (112 files) with
  `bugprone-*`, `concurrency-*` and `performance-*` on `ubuntu-latest`, and any finding in
  project sources fails the job, so a split that smuggles in a new shape surfaces here one
  finding at a time. Two shapes are worth knowing before writing the next helper, and both are
  silent on Windows: a `const std::string name = value.as_string();` copy-*initialization* from
  a function returning `const std::string&` is `performance-unnecessary-copy-initialization`
  (bind the reference), and **returning a `const&` PARAMETER** is
  `bugprone-return-const-ref-from-parameter` — which is not style, it is a use-after-free:
  `amplification_for` returned its `blended` argument by reference while one caller built that
  argument as a conditional temporary, so on the blending path the reference dangled the instant
  the initializing statement ended. Returning the knobs by value (five floats) is both the fix
  and the contract. Reproducing the gate locally needs the workflow's `jq` dedupe plus a filter
  that drops `clang-diagnostic-*` findings, because a locally generated `compile_commands.json`
  carries MSVC-only flags (`/c`, `/external:anglebrackets`) that make clang report
  `unused-command-line-argument` on nearly every file — an artifact no Linux runner produces.
  Windows-only regions are outside the gate's reach either way, so their idioms (an address cast
  into `GetModuleHandleExW`, walking `CONTEXT::Rsp`) carry a scoped
  `// NOLINTNEXTLINE(check) - reason` in the house form rather than being restructured
- Block targeting must cast from the player's **eye along the look direction**
  (`PlayerController::get_aim_origin`/`get_aim_direction`), never from the `Camera3D` — the
  third-person camera sits off the eye, so camera rays diverge at close range and first/third
  person aim at different blocks. The third-person camera belongs **on the look ray** with the
  player's own look rotation (front view mirrors it), and the player head tracks the **aim**,
  not the camera, because the front-view camera is yaw-flipped 180°

## Major Completed Work

### Core Engine Foundation
- **Frustum-prioritized loading**: Camera frustum extracted each frame; visible chunks get
  priority for generation, meshing, retention, and LOD detail
- **Sharded chunk map**: 64 independently-locked shards (`shared_mutex` each), so a write on one
  shard never blocks readers on another
- **Palette-compressed storage**: Block and light data stored as 8 paletted 16³ sections per
  chunk instead of dense arrays, cutting per-chunk memory from ~130KB to ~1–20KB on uniform
  terrain
- **Budget-capped main thread**: Generation completion, mesh uploads, and light propagation are
  wall-clock-budgeted per frame; nearest-to-player completed mesh uploads first
- **Dynamic mesh budget (REMOVED — it never ran)**: the budgets were scaled by a
  `visible_chunk_ratio_` the retired frustum pass was meant to write, and nothing ever assigned
  it, so the scale was a constant 1.0 and the two lines were dead arithmetic. The ratio and its
  cursor are gone; what is left is the fixed idle/active budget split below
- **Targeted shard locking**: `lock_keys_exclusive()` locks only shards whose keys appear in
  input, reducing contention from all-64-shard locks to only the 1–54 shards actually needed
- **Work stealing thread pool**: Adaptive idle polling with work stealing for better CPU
  utilization
- **Per-worker task queues**: Round-robin task distribution replaces single global mutex for
  better throughput

### Block Shapes & Collision
- Slabs, stairs, walls, poles and the flat batch, with multi-box models as data; a
  `shape` name that fails to resolve leaves a FULL CUBE and only logs an error.
- Liquids are passable, and `BlockType::stops_bodies()` is the single predicate the
  collision resolver, the sneak edge-guard and the third-person camera all consult.
- Collision defaults to the visible model (the pole is the documented exception),
  bottom faces are emitted only where they can be seen, and the art for every shape is
  generated from its own selection boxes.

Full notes: [docs/shapes-notes.md](docs/shapes-notes.md).

### Rendering & Visual Features
- Three-tier LOD with 8x8-chunk region merging, and the two emitters that must agree
  about where a liquid cell is drawn (the mesh content hash covers both surfaces).
- A full cube hides a face only if it is also OPAQUE - the difference leaves and
  falling water expose, pinned per shape by its own test.
- Sky, fog, god rays, turbidity, emissive maps and soft curved AO.
- Animated liquid textures are generated (a cellular automaton mapped through a colour
  ramp), not drawn; the Liquid Texture Lab (O) is the tool and `liquid_animator.gd`
  animates the world's own liquids.
- The shader-effect stack: kinds and parameters from `data/shaders.json`, then Hand
  Drawn, CRT Screen, Phosphor Trail, Invert/Sepia, Camera Jitter, World Bend and the
  Horizon Curve.

Full notes, effect by effect: [docs/rendering-notes.md](docs/rendering-notes.md).

### Inventory & GUI
- The C++ inventory, crafting and item-registry cores, and the GDScript screens over
  them (hotbar, inventory, 2x2 crafting, drag operations, persistence).
- The settings menu: one size table rather than raw pixels, per-row labels, and a
  shareable code per category.
- The in-game editors (Skin Maker, Block Maker) with shared paint tools and input
  guards, block break animation and hardness, hammer crushing, and block `drops`.
- The first-person viewmodel (held item/block, swing, bob, head look, F5 camera cycle,
  eye-ray targeting) and the overlay tools around it.

Full notes: [docs/gui-notes.md](docs/gui-notes.md).

### Async Chunk Saving
- **Off-main-thread writes**: `flush_dirty_chunks` deep-copies each dirty chunk under its shard
  lock and hands the snapshot to the thread pool for RLE encode + atomic write; periodic 5s
  flush is non-blocking
- **Generation gating**: per-key save generations (`next_save_generation`) so a newer snapshot
  supersedes an older in-flight save — superseded workers abort at their gate instead of
  clobbering newer data; epoch gate drops stale saves after a world reset
- **Flush on quit**: `ChunkManager::_exit_tree()` blocks on `flush_dirty_chunks(true, 5.0)` so
  recent edits are never lost on exit
- **Cross-chunk canopy persistence**: `apply_pending_placements` marks the receiving neighbor
  chunk dirty so deferred tree-canopy writes survive reload

### Terrain Generation
- A stacked-noise macro surface with a signed 3D density field over it, a 4x4x4
  world-aligned shape lattice, and chunk-level fast paths.
- Biomes shape the terrain first and the oceans decide water last; the 3D shaping is
  gated by strength ("weirdness") zones, with shoreline handling near water.
- Data-driven: `data/biomes.json`, `data/vegetation.json` and
  `data/terrain_config.json` are what worldgen reads.

Full notes: [docs/terrain-notes.md](docs/terrain-notes.md). The fields of each file:
[docs/data-schemas.md](docs/data-schemas.md).

### Worldgen Config Data
- The three files worldgen reads are `data/biomes.json`, `data/vegetation.json` and
  `data/terrain_config.json`; what each one drives is in
  [docs/terrain-notes.md](docs/terrain-notes.md), and every field is in
  [docs/data-schemas.md](docs/data-schemas.md).
### Pathfinding (`src/pathfinding/`)
- A ground-route planner with no Godot dependencies: the goal is a cell rather than a
  column, and the heuristic is in the cost table's own units.
- `Unknown` (an unresident chunk) is never traversable, nodes are feet cells, and a
  column's result is memoised per `(x, z, hint)`.
- `NavPath::truncated` means a budget ran out, not that no route exists; every cost
  lives in `NavCosts`.

Full notes: [docs/pathfinding.md](docs/pathfinding.md).
### Block files from other tools (`src/schematic/`)
- A read-only decoder for both families of build file, with no Godot and no zlib, that
  tolerates the three quirks real writers produce.
- `data/minecraft_blocks.json` is the translation table, read by `minimal_json` rather
  than `godot::JSON`; a legacy id is not a block, and unmapped ids are dropped loudly.
- Planning a paste is engine-free and separate from writing it; the writer is
  `BlockEditor::apply_paste`, not a loop over `place_block`, and cells whose chunk is
  not resident WAIT for it rather than being skipped.

Full notes: [docs/schematic-notes.md](docs/schematic-notes.md).

### Fluid flow (`src/fluids/`)
- Poured water flows and generated water does not; lava and acid are the same rules
  with their own numbers.
- A cell's state is the whole state - there is no flowing/settled block pair - and
  every depth has its own height, which is data rather than code.
- The driver is single-threaded: a wake that comes from a worker is POSTED and drained
  on the main thread, never applied where it was raised.

Full notes: [docs/fluids.md](docs/fluids.md).
### Testing & CI
- 588 test cases / 346,456 assertions, written with `CHECK`/`CHECK_FALSE` only: this
  build disables exceptions, so `REQUIRE` is a compile error.
- Move work off the frame before making it cheaper, and prove the move with a total:
  the phases must add up to the frame.
- A probe that runs for more than a few frames is a benchmark; a probe that edits the
  world must snapshot it first and then verify the write.
- The jobs: Linux and Windows builds, concurrency tests, the soak test, the benchmark
  check, the fuzzers, coverage, and the four static gates above.

Full notes: [docs/testing.md](docs/testing.md).
### Persistence & Format
- **Save format v3**: RLE-compressed with CRC32 checksum; atomic writes with `.tmp` → `.bak` →
  target pattern
- **Legacy support**: Handles v3 (CRC32), v2 (legacy RLE), and v1 (flat) transparently
- **Corrupted file recovery**: Attempts to load from `.bak` backup on CRC mismatch
- **Async background saves**: Dirty chunks are snapshotted and written on the thread pool;
  per-key generation + epoch gating ensures the newest state reaches disk
- **Sparse edit map persistence**: Replace whole-chunk snapshots with sparse edit map
  persistence for cross-version compatibility
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1) written at
  `PlayerController::_exit_tree`

### Locking & Concurrency Fixes
- **8 deadlock classes resolved**: Self-deadlock in light propagation, shard lock ordering,
  `_locked` method contracts, and **recursive shared shard-lock acquisition** (Windows SRW locks
  block new shared acquisitions once a writer is queued on a shard, so re-acquiring a shard you
  already hold shared freezes the whole game). The recursive class was fixed with
  `queue_dirty_chunk_fast()` (dirty-queue under a caller-held lock) and `ShardLock::reset()`
  (release-then-re-acquire for periodic lock refreshes like the block raycast's all-shard lock)
- **Player light thread safety**: Fixed unlocked writes while BFS runs concurrently
- **Cross-chunk writer race**: Fixed vegetation cross-chunk block writes with proper exclusive
  locking
- **Mesh-build serialization**: `MeshBuildTask::execute` holds a shared 3×3×3 `lock_keys` over
  the center chunk + 26 neighbors for the whole data read, serializing the build against writers
  (block edits, light region recomputes) that mutate neighbor section palettes mid-build
- **Overlapping light removal**: Per-channel removal clears a channel only when the removed
  source emitted it and the cell's level is strictly below the source's; surviving channels are
  re-added so they refill the cleared region, and each (cell, channel) is cleared at most once
  so the BFS terminates with overlapping sources
- **Worker→simulation wake**: A generated chunk applies its saved edit map on the thread pool,
  which used to wake the fluid simulation directly (mutating a queue the main thread ticks) and
  surfaced later as heap corruption on a worker. `ChunkWorld` now carries a second listener for
  that path, and it posts (`FluidSim::post_block_changed`) instead of applying; the main thread
  drains the inbox at the top of `advance()`. See the fluid section for why the drain sits
  before the idle early-return

### Crash Reporting
- **Our own handler, installed before any class registers** (`debug/crash_dump.hpp`, wired in
  `register_types.cpp`): an unhandled exception writes `user://crashes/crash-<stamp>.txt` —
  exception code, read/write and address, the module and offset of the instruction pointer, the
  faulting thread's stack with a symbol per frame, the loaded modules, and our DLL's build time
  — then `crash-<stamp>.dmp` beside it, plus a `<stamp>-firstchance.txt` capped at 8 writes. The
  report is written before the dump because it is what can be read without a debugger, and the
  handler returns `EXCEPTION_CONTINUE_SEARCH` so the engine's own handler still runs. It exists
  because the process dies before the engine can print anything, so its log just stops
  mid-initialization
- **`dbghelp` and `/DEBUG:FULL` are linked on Windows** (`SConstruct`): a minidump of an
  optimized DLL with no symbols is a column of numbers
- **The proof fault is debug-build and env-armed only**: `ChunkManager::debug_crash_for_test()`
  exists under `DEBUG_ENABLED` and `crash_for_test()` answers false without
  `FARLANDS_CRASH_TEST=1`, because a synthetic crash in the player's crash folder sits beside
  the real ones. `.freebuff/run_crash_probe.sh` runs it armed and prints what landed

### Collision & Physics
- **Binary-search AABB collision**: Custom voxel collision queries directly against chunk map
- **Multi-box collision support**: Non-full block shapes (stairs, slabs, walls, poles) have
  accurate multi-box collision detection
- **Step-up fix**: Tests player's full body AABB raised by step_height (old approach always
  failed)
- **Minecraft-accurate physics**: Fixed 20-tick/s simulation with vanilla jump/sprint/sneak
  ordering, removed 0.98 input scaling, proper sprint state machine with sticky flag and
  one-tick stale airborne, sneak multiplier only on ground
- **Move speed multiplier**: Adjustable player movement speed multiplier property
- **Fall damage**: `PlayerSim` accumulates fall distance from per-tick position deltas
  (including the collision-clipped landing segment — it must be added before the landing check
  or every fall loses its final stretch), and a landing past `SAFE_FALL_DISTANCE` (3 blocks)
  queues `floor(distance − 3)` half-hearts via `consume_pending_fall_damage()`;
  `PlayerController` applies it to a clamped `health` property (0–20 half-hearts,
  `get_health`/`set_health` bindings) that `healthbar.gd` renders; ascending doesn't accumulate
  and `reset()` (teleport/fly) clears everything
- **Death & respawn**: `set_health` reaching 0 triggers `die()` — sets a `dead_` flag that
  freezes `_process`/`_input` (no movement, look, break/place, hotbar), forces the mouse cursor
  visible via `update_mouse_mode()`, and emits `died`; `respawn()` restores full health,
  teleports to the `_ready`-captured spawn point (clearing fall state), emits `respawned`.
  `death_screen.gd` shows a vanilla-style "You died!" + Respawn overlay on those signals;
  inventory is kept on death

### Code Quality & Hygiene
- **GDScript lint fixes**: Fixed extra closing parentheses in timer connections, renamed
  shadowed parameters (scale → scale_factor to avoid Control base class conflict, char → ch to
  avoid shadowing built-in function), resolved integer division warnings with explicit float
  division and int() casts, removed redundant int division casts, dropped unused variables, cast
  deserialized bindings to Key/MouseButton enums, renamed lambda locals to avoid shadowing
  Node.name
- **Documentation format**: Migrated roadmap from .txt to .md for proper markdown formatting and
  GitHub rendering
- **Single source of truth**: `data/block_definitions.json` for all block properties
- **Stable block-name storage**: `BlockRegistry::load_from_json` stores `bt.name` pointers into
  a static `std::deque<std::string>` (was `std::vector`, which dangled every previously-stored
  name on reallocation — late lookups like recipe resolution read freed memory and
  intermittently failed)
- **A 500-line cap now holds across the whole tree** (`scons sizecheck` + a CI step, warning at
  480): 36 files were over it — the largest `chunk_generator.hpp` at 888 — and each was split
  along responsibilities rather than by size, the original name keeping its subject while
  `<name>_<topic>.cpp` takes a piece, `<name>_internal.hpp` holds what two translation units
  share and `tests/<name>_test_support.hpp` holds shared fixtures. Two conventions came out of
  it that cost an afternoon each: a helper that was file-local (`static`, or in an anonymous
  namespace) has to become `inline` in a named namespace once two files use it, and a support
  header does not inherit the includer's `using namespace`, so it may need its own. The recipe,
  the phase history and every trap are in `docs/file_size_plan.md`
- **Source reorganization**: Split `mesh_manager.cpp` into worker/upload/rebuild/far/lifecycle
  TUs (shared `mesh_manager_internal.hpp`), `mesh_builder.cpp` (`mesh_builder_solid.cpp`), and
  `chunk_world.cpp` (`chunk_world_edits.cpp`, `chunk_world_persistence.cpp`). Moved
  render-facing types out of the `core/chunk_types.hpp` junk drawer:
  `ChunkRenderData`/`CachedFarChunkMesh`/`CompletedMesh` → `mesh/chunk_render_data.hpp`,
  `DirtyChunkEntry` → `mesh/mesh_queue.hpp`, `WorldRenderStats` →
  `render/world_render_stats.hpp`; relocated `texture_array_generator.hpp` to `render/`; deleted
  orphaned `.obj` artifacts
- **Removed old LOD system**: Deleted 2,435 lines of 2×2×2 group-mesh-merging code, replaced
  with per-chunk three-tier LOD with region merging
- **Removed experimental features**: Cloud layer system, lighting preset system (Main/Spooky),
  occluder boxes - all reverted or removed
- **Terrain simplification**: Removed complex biome systems (Tundra/Taiga/Savanna/StonePlateau)
  and experimental mountain/erosion systems in favor of the current 3-biome JSON-driven system
  (`data/biomes.json`: plains, hills, ocean — the 3×3 temperature×humidity grid maps land
  columns to the first two and the below-sea stage produces the third); domain warp retained for
  flowing terrain ridges
- **Dead code cleanup**: `is_occluder()` method defined but unused, legacy `mountain_scale`
  parameter in persistence (ignored), cave system disabled (`kCavesEnabled = false`)
- **Clang compatibility**: Fixed NSDMI compile error for nested structs
- **Repo cleanup**: Removed leaked `.lnk` shortcuts, orphaned `.import` files, added
  `.gitignore` rules
- **License**: Added GPL-3.0 license (repo was previously all-rights-reserved)
- **Shadow optimization**: Disabled sun shadow map (both terrain shaders are unshaded)

### Performance Optimizations
- **Vertex format optimization**: Fixed-point positions reduce quad cache memory usage,
  ARRAY_CUSTOM format for GPU compatibility
- **Chunk generation optimization**: Fill blocks for uniform chunks, chunk-level fast paths for
  chunks far from terrain surface
- **AO optimization**: Pass BlockRegistry by reference, use get_block_fast() for hot path,
  incremental AO tracking removes redundant passes
- **GDScript performance**: Gate queue_redraw() calls, throttle block outline raycast, skip
  per-frame redraw churn
- **Mesh queue optimization**: O(1) mesh-queue removes, bounded lock_keys instead of whole-map
  locks
- **Off-screen meshing starvation fix**: the dirty mesh queue was one heap ordered
  `urgent > in_frustum > dist_sq`. During streaming the queue is permanently saturated (every
  install dirties itself and its neighbours, several times what the 16/frame active budget
  drains), so an unbounded view-first tier meant a chunk that was merely off-screen never
  reached the front at all — it stayed un-meshed until the player turned round, at which point
  it jumped the queue and appeared ("forgets to mesh stuff until you look at it"). Now two heaps
  (visible / everything else), the visible pass getting `max_rebuilds - reserve`, its unspent
  share handed to the other heap, and `max_rebuilds/4` reserved for distance-ordered work every
  frame. A frame with nothing visible still spends the full budget; an entry that comes into
  view moves back into the visible heap on its next pop; with no frustum fed (benchmarks, tools)
  no reserve applies. `tests/test_mesh_queue.cpp` pins all four
- **LOD rescan walks the shell, not the box**: the transition-band rescan tested
  `max(|dx|,|dy|,|dz|) in [tier-1, tier+1]` cell by cell over the enclosing box — ~1.7M
  iterations per tier per frame to reach ~100k band cells — which was most of the
  `dirty_mesh_queue` phase's cost (avg 1-4.7 ms, max 157 ms). `for_each_shell_cell`
  (`src/mesh/lod_shell.hpp`) visits the band directly, with `tests/test_lod_shell.cpp` pinning
  the visited set against the old box filter
- **Mesh budget does NOT scale with the backlog** (tried, reverted): raising the rebuild count
  4× when the queue is deep drained the queue sooner but took `dirty_mesh_queue` from a ~1.0 ms
  to ~2.7 ms median in a live session, because each rebuild is 7 chunk lookups plus a 64-chunk
  walk per far region — all main-thread, against a map that already has a writer convoy. The
  same chunks arrive in the same order, only sooner
- **Generator buffer reuse**: Reuse generation buffers to reduce allocations
- **Shader fog gating**: Gate sun-scatter/haze math behind fog check for performance
- **LOD solid cache fix**: Fix O(stride²) solid_cache population in LOD mesh builder, stores
  BlockID instead of bool
- **Light propagation optimization**: Offload to worker threads with fast-path atomic check,
  poll results on main thread
- **Sub-chunk dirty tracking**: Fine-grained invalidation instead of full-chunk rebuilds
- **Batched edit persistence**: a paste and the fluid sink both persist a chunk's cells through
  `ChunkWorld::add_block_edits` — one edit-map lock, one chunk lookup, one dirty mark and one
  hash-table reserve for the run instead of one of each per BLOCK (the per-cell form was paying
  three mutex acquisitions and two hash lookups per block on a build with hundreds of thousands
  of cells). It deliberately does NOT notify the edit listener, so the caller picks the cells
  worth waking: `apply_paste` computes that where the chunk is already in hand
  (`paste_cell_needs_fluid_wake` — the written cell became or stopped being a fluid, or a
  face-neighbour is one, which is the simulation's own `notify_block_changed` filter answered
  from the chunk array rather than through a world read), and wakes them once every write band
  is released. Undo goes through the same `apply_paste`, so it gets both halves. **A face
  neighbour has to be read with the `_fast` accessor** (`get_block_world_fast`), not the locking
  one, because the predicate runs while the caller holds the 3×3×3 exclusive band and a
  neighbour is at most one chunk out — so its shard is one the caller already owns, and a shared
  lock on it is a deadlock, not a slow path. A paste that spans chunks always has cells on a
  face, so the locking form hangs every large paste; the debug lock-order checker now reports it

## Technical Details

### Locking Hierarchy
- 64 shards, each with `shared_mutex` + `unordered_map`
- Hash: `key % 64`
- Lock types: `ShardLock` (shared), `ExclusiveShardLock` (exclusive)
- Single-chunk accessors lock only their own shard
- Batch methods lock all relevant shards in ascending order to avoid deadlock
- `_locked` methods: Caller MUST already hold exclusive lock, uses `_fast` accessors only
- Auto-locking methods: Acquire their own shared locks — MUST NOT be called under exclusive
  lock. This is the same non-recursive rule as the bullet below but from the other side, and its
  failure mode is worse: an exclusive holder calling one never returns, so there is no stack, no
  report and no log line, just a window that stopped responding. Every exclusive lock now tells
  the checker which shards it holds (`LOCK_ORDER_ACQUIRE_EX`), and every locking read asks first
  (`LOCK_ORDER_REQUIRE_SHARED`, in `get_block_world`, `get_chunk_data`, `get_chunk_render_data`,
  `has_loaded_chunk`, `is_block_solid`, `contains`, `pin_chunk`, `unpin_chunk`, and the
  `lock_chunk`/`lock_keys`/`lock_all` factories), so a debug build names the accessor, the shard
  and the fix instead of hanging — the check has to run BEFORE the lock is taken, since after it
  there is nothing left to print with. `_fast` is the answer inside such a scope, and
  `tests/test_chunk_map.cpp` pins that it reads a neighbouring chunk correctly under the band
- Recursive shared acquisition is forbidden: never re-acquire a shard you already hold shared
  (e.g. calling `queue_dirty_chunk` inside a `lock_keys` scope, or building a fresh `lock_all()`
  while one is alive). Use `queue_dirty_chunk_fast()` / `get_chunk_render_data_fast()` and
  `ShardLock::reset()` before re-locking. Windows SRW blocks new shared acquisitions once a
  writer is queued, turning the recursion into a hard deadlock

### Targeted Shard Locking Usage
- `propagate_block_light_region()` — 27 keys (3×3×3 neighborhood)
- `place_block` — 27 keys (3×3×3 center)
- `light_propagate_add` / `light_propagate_remove` — origin 3×3×3 + each seed node's 3×3×3
  (deduplicated)
- `update_block_light_incremental` — 54 keys (origin + center 3×3×3)
- `PlayerLight::update` — vector of up to 54 keys (old+new chunk 3×3×3)
- `MeshBuildTask::execute` — 27 keys (center + 26 neighbors), **shared** `lock_keys` held for
  the whole data read
- `ChunkFluidWorld::read_window` — up to 8 keys (an 11×11×3 window spans at most 2 chunks per
  axis), passed as **array + count**
- **`lock_keys` has two forms and they are not interchangeable**: the array template locks its
  whole extent (`lock_keys(uint64_t[8])` takes all 8 shards), while `lock_keys(keys, count)`
  locks only what was filled. A caller that fills part of a fixed array — the fluid window fills
  1 to 8 slots — must pass the count: the array form reads the untouched slots (undefined
  values) and takes shards nothing asked for, which the analyzer reports as "argument is an
  uninitialised value"

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
