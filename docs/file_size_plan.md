# File-size reduction plan — no C++ file above 500 lines

**Goal.** Every `.cpp` / `.hpp` under `src/`, `tests/` and `tools/` is **≤ 500 lines**, so a
file can be read in one sitting, named by what it does, and a new responsibility lands in a new
file instead of growing a mega file. Behaviour is unchanged: the 588-test suite must stay green
(588/588, 346,456 assertions) at every stage, and the existing `scons bench --check
benchmark_baseline.txt` gate stays clean.

## 1. Scope — 36 files over the cap today

| Group | Files > 500 | Lines | Where the mass is |
|---|---|---|---|
| `src/**/*.cpp` | 14 | 12,400 | Godot facades, world streaming, meshers |
| `src/**/*.hpp` | 8 | 5,073 | header-only helpers + fat class definitions |
| `tests/*.cpp` | 13 | 10,264 | one file per subject, several subjects per file |
| `tools/*.cpp` | 1 | 553 | `schematic_report.cpp` |

Definition of done:

```bash
find src tests tools \( -name '*.cpp' -o -name '*.hpp' \) | xargs wc -l | awk '$1 > 500'
# → no output
```

## 2. Execution recipe (the invariants that keep this safe)

1. **One owner per file.** A split always leaves one file under the original name (the entry
   point / orchestration), and new files are named `foo_<topic>.cpp` — the convention the repo
   already uses (`mesh_manager_worker.cpp`, `chunk_world_edits.cpp`, `schematic_reader.cpp`).
2. **No behaviour edits inside a split.** Moving a function verbatim first, tidying after (and
   only in a separate commit-sized step). Any real change is listed in §6 and verified
   separately.
3. **Header splits keep an umbrella.** `block_types.hpp` etc. keep their names and
   `#include` the extracted pieces, so **no includer changes at all**. Only the extracted header
   is new.
4. **Internals stay internal.** When two `foo_*.cpp` need the same private helper, it goes in a
   `foo_internal.hpp` (`chunk_world_install`, `mesh_manager_internal.hpp`,
   `chunk_map_internal.hpp` are the
   precedents) — never into a public header.
5. **LF line endings** for every new source; docs stay CRLF.
6. **State that both halves read cannot be file-local.** An anonymous namespace gives internal
   linkage, so anything two `foo_*.cpp` share (a handle, a resolved path, a helper) has to be a
   real
   namespace member declared in `foo_internal.hpp` — and the readers then spell it qualified
   when
   they live outside that namespace. Found the hard way in `crash_dump.cpp` (§9).
7. **Basenames are a build key.** `SConstruct` builds `shared_obj_by_src` keyed on *basename*,
   so
   every new `shared_sources` entry must have a globally unique basename. Add an assert (§5).

## 3. Build-system facts that constrain the splits

| Fact | Consequence |
|---|---|
| `sources = Glob("src/*.cpp") + Glob("src/*/*.cpp")` | New library `.cpp` files are picked up automatically. No edit. |
| `Glob("tests/*.cpp")` for `bin/run_tests` | New test files are picked up automatically. No edit. |
| `shared_sources` is an explicit list | **Every new piece of a shared file must be added**, or tools/tests fail to link. |
| `terrain_tool_objects` lists `"chunk_generator"`, `"block_types"` by basename | Splitting those two means adding the new basenames there too. |
| `schem_tool_objects` lists `"schematic_reader"`, `"mc_palette"` | Same for those two. |
| `bench` / `greedy_measure` / `memory_estimate` / `repro_stride2` link `shared_objects` wholesale | Unaffected by splits. |
| Tools name their own sources explicitly (`["tools/schematic_report.cpp"]`) | A split tool must add its new `.cpp` to that `Program()` call. |
| Godot binds classes, not files | `register_types.cpp` never changes for a `.cpp` split. |
| `chunk_data.cpp` is compiled three times (lib, test, standalone) via explicit target names | Its header split is the highest compile-time win; do not touch the target names. |

Files needing `SConstruct` edits: `block_types.cpp`, `chunk_generator.cpp`,
`schematic_reader.cpp`, `mc_palette.cpp`, `shape_resolver.cpp`, `mesh_builder_solid.cpp`,
`mesh_builder_faces.cpp`, and `tools/schematic_report.cpp`.

## 4. Staged plan

Each stage ends with: `scons -j14`, `scons test -j14`, `./bin/run_tests.exe`, and — for the
stages marked ⚙ — `scons bench` against `benchmark_baseline.txt`.

### Phase 0 — guardrails (no source churn)
- `tools/check_file_sizes.py` — walks `src/`, `tests/`, `tools/`, exits non-zero above 500 lines
  (soft-warn at 480). Wired as `scons sizecheck` **and** as a CI step so a mega file cannot
  come back. This is the "reduce the future risk of mega files" half of the goal.
- Assert in `SConstruct` that `len(shared_obj_by_src) == len(shared_sources)` — turns a silent
  basename collision (which we are about to make much more likely) into a build error.
- Record baseline timings: cold `scons -j14`, warm rebuild, `scons bench` numbers.
- (Housekeeping, optional) `git rm --cached` the stale `*.obj` files sitting in `src/` and
  `tests/` — they are gitignored now but still in the tree.

### Phase 1 — header extractions (umbrella pattern, lowest risk)
Pure code motion; no `.cpp` split, no `SConstruct` change, no caller change.
`block_types.hpp`, `chunk_data.hpp`, `mesh_builder.hpp`, `texture_array_generator.hpp`,
`liquid_texture.hpp`, `world_updater.hpp`, `chunk_map.hpp`.

### Phase 2 — library-only `.cpp` splits (no build edits)
`chunk_world.cpp`, `crash_dump.cpp`, `block_editor.cpp`, `world_updater.cpp`,
`chunk_manager.cpp`, `player_controller.cpp`, `voxel_engine_controller.cpp`.
⚙ for `world_updater` / `chunk_manager` / `player_controller` (streaming + per-frame paths).

### Phase 3 — shared `.cpp` splits (needs `SConstruct` edits)
`mesh_builder_faces.cpp`, `mesh_builder_solid.cpp`, `shape_resolver.cpp`, `block_types.cpp`,
`schematic_reader.cpp`, `mc_palette.cpp`, `chunk_generator.cpp`.
⚙ for `mesh_builder_*`, `shape_resolver`, `chunk_generator` — these are the hot paths.

### Phase 4 — `chunk_generator.hpp` (the hard one)
See §5.4 — needs a small structural extraction plus a perf comparison.

### Phase 5 — tests, then the tool
13 test files + `tools/schematic_report.cpp`.

### Phase 6 — docs and final gate
- `ARCHITECTURE.md` file map (lines ~365-450) and `AGENTS.md` updated with every new filename.
- `python`-free final check: the §1 command prints nothing; `scons sizecheck` green; suite
  green;
  bench green.

## 5. Per-file split table

### 5.1 Headers (Phase 1)

| File | Lines | New files | What moves out |
|---|---|---|---|
| `core/chunk_map.hpp` | 826 | `core/shard_lock.hpp`, `core/chunk_map_inline.hpp` | `ShardLockStats` + `shard_lock_detail::lock_*_timed` (~75), the nested `ShardLock`/`ExclusiveShardLock` guards (~113) → `shard_lock.hpp`; then the `lock_chunk/lock_column/lock_keys/lock_all/for_each_limited_resumable` bodies → `chunk_map_inline.hpp` (included at the bottom) |
| `worldgen/chunk_generator.hpp` | 887 | `worldgen/chunk_generator_lattice.hpp`, `worldgen/chunk_generator_sample.cpp` | lattice structs + `build_*_lattice` / `interp_*_lattice` (~230); cold sampling helpers → `.cpp` (see §5.4) |
| `render/liquid_texture.hpp` | 613 | `render/liquid_texture_settings.hpp`, `render/liquid_texture_generator.hpp` | `Kernel`/`Style`/`Settings`/`Strip` (~285); `Rng`/`Generator`/`render_into` (~330) |
| `render/texture_array_generator.hpp` | 610 | `render/texture_array_generator_impl.hpp` | all inline method bodies + the free helpers (~470); main keeps the class + singleton (~145) |
| `core/block_types.hpp` | 569 | `core/block_shape_types.hpp`, `core/block_ids.hpp` | `BlockProperty`/`LightEmissionPattern`/`FluidKind`/`BlockAABB`/`ShapeRule`/`ShapePart`/`BlockShape` (~200); `namespace BlockIDs` (~60) |
| `mesh/mesh_builder.hpp` | 558 | `mesh/mesh_builder_types.hpp` | `Face`, `FaceData`, `kBlockBrightness`, `SubChunkBounds`, `NeighborPtrs`, `GreedyVerticalStatsSnapshot` (~130) |
| `core/chunk_data.hpp` | 549 | `core/palette_storage.hpp` | `PalSection` + `PaletteStorage` (~330) |
| `world/world_updater.hpp` | 541 | `world/world_updater_types.hpp` | `ColumnSurfaceBounds`, `FluidSink`, the chain/band/sweep structs, config structs (~200) |

Notes:
- `shard_lock.hpp` is a **copy-with-friend** extraction: the guards keep
  `friend class ChunkMap`,
  so `sl.locks_.emplace_back(...)` / `held_stats_` call sites in `ChunkMap` are untouched. Only
  `kNumShards` moves (it becomes the shared constant the guards are sized by).
- `liquid_texture.hpp` is consumed by `tests/test_liquid_texture.cpp` **without linking a
  `.cpp`**,
  so its bodies must stay header-only. The split keeps that property (new headers, not new
  translation units).
- `texture_array_generator_impl.hpp` is included at the bottom of the main header, so the class
  is
  still complete for every includer.

### 5.2 Library-only `.cpp` (Phase 2)

| File | Lines | Split |
|---|---|---|
| `godot_bindings/player_controller.cpp` | 1517 | `player_controller.cpp` (ctor, `_bind_methods`, lifecycle, `_process`/`_input`, movement, camera, aim) · `player_controller_interact.cpp` (`break_block`, `update_break_progress`, `get_break_state`, `place_block`, `use_item`, `update_held_light`, bucket pour/fill) · `player_controller_inventory.cpp` (inventory/hotbar accessors, give/clear, match/craft recipe, texture pack, menu-open flags) → 3 files, largest ~500 |
| `godot_bindings/chunk_manager.cpp` | 1181 | `chunk_manager.cpp` (ctor, tree lifecycle, cached-node resolution, `_bind_methods`) · `chunk_manager_world_api.cpp` (`set/get_block`, raycast, selection boxes, collision, environment/LOD/lighting setters, path request/poll) · `chunk_manager_schematic.cpp` (biome, schematic inspect/preview/paste/undo, pending paste, fluid + generation stats) |
| `world/world_updater.cpp` | 1132 | `world_updater.cpp` (setters, `update`, generation, unload, mesh budgets, `flush_dirty`, clear/reset) · `world_updater_columns.cpp` (`ColumnSurfaceBounds` trio, `store_column_bounds`, `invalidate_height_cache`, `on_chunk_installed`) · `world_updater_streaming.cpp` (chain queue, column prefetch, sweep bands, `advance_sweep`, `rebuild_sweep_columns`) |
| `engine/voxel_engine_controller.cpp` | 1110 | `voxel_engine_controller.cpp` (lifecycle, thread pool, `update`, world cull, chunk fwd, property accessors) · `voxel_engine_schematic.cpp` (palette, `plan_counters`, `decode_and_plan`, inspect/preview, paste bytes, undo, pending-paste tick/finish/cancel/get/take) · `voxel_engine_config.cpp` (world configs, metadata save/load, inventory save/load, `flush_dirty_chunks`, `find_biome`, perf report) |
| `world/block_editor.cpp` | 731 | `block_editor.cpp` (ctor, `query_block`, `place_block`) · `block_editor_raycast.cpp` (`raycast_from_ray`) · `block_editor_paste.cpp` (`apply_paste`, `undo_paste`) + `block_editor_internal.hpp` if the two share helpers |
| `debug/crash_dump.cpp` | 561 | `crash_dump.cpp` (install, filters, `crash_for_test`, report text) · `crash_dump_symbols.cpp` (`describe_address`/`describe_symbol`, build report, module list, dump write) + `crash_dump_internal.hpp`. Lowest value item in the plan — Windows-only, unverifiable in the local sanitizer config |
| `world/chunk_world.cpp` | 506 | `chunk_world.cpp` (generation) · `chunk_world_install.cpp` (`process_completed_chunks` + install path) |

### 5.3 Shared `.cpp` (Phase 3, all need `SConstruct` `shared_sources` updates)

| File | Lines | Split | Extra build edits |
|---|---|---|---|
| `mesh/mesh_builder_solid.cpp` | 1077 | `..._solid.cpp` (`populate_solid_cache`, `solid_at`) · `..._solid_emit.cpp` (`emit_faces`) · `..._solid_cull.cpp` (`should_cull_aabb_face`, `resolve_neighbor_shape`, `shape_neighbor_lookup`, `append_quad`, `should_drop_quad`, `should_cull_against_neighbor`, `boundary_face_fully_occluded`) | `shared_sources` |
| `core/block_types.cpp` | 1044 | `block_types.cpp` (core registry: ctor, `get_block_id_by_name`, `resolve_pending_drops`, family accessors) · `block_types_json.cpp` (`load_from_json`, `load_shapes_from_json`, the anonymous-namespace parse helpers) · `block_types_defaults.cpp` (`initialize_default_blocks`) | `shared_sources` + `terrain_tool_objects` |
| `worldgen/chunk_generator.cpp` | 963 | `chunk_generator.cpp` (chunk fill, fast path, height range, `find_surface_y`) · `chunk_generator_noise.cpp` (`find_nearest_biome`, macro height, surface/subsurface selection) · `chunk_generator_climate.cpp` (climate/land-shape/amp lattices, `sample_column*`) | `shared_sources` + `terrain_tool_objects` |
| `schematic/schematic_reader.cpp` | 826 | `schematic_reader.cpp` (format walk / NBT binding) · `schematic_reader_states.cpp` (`BlockState`, `SchematicData`) · `schematic_reader_internal.hpp` for the ~20 file-local parse helpers and the `Data` layout struct | `shared_sources` + `schem_tool_objects` |
| `schematic/mc_palette.cpp` | 776 | `mc_palette.cpp` (`load`, `row_for`, `species_keys`, `resolve`/`resolve_named`/`apply_name_row`, `ids`) · `mc_palette_tables.cpp` (the static species/alias tables) + `mc_palette_tables.hpp` | `shared_sources` + `schem_tool_objects` |
| `core/shape_resolver.cpp` | 634 | `shape_resolver.cpp` (entry points + the canonical walk) · `shape_resolver_families.cpp` (stair + wall family predicates) + `shape_resolver_internal.hpp` | `shared_sources` |
| `mesh/mesh_builder_faces.cpp` | 610 | `mesh_builder_faces.cpp` (`add_aabb_face`, `apply_special_block_offsets`) · `mesh_builder_face_emit.cpp` (`add_face`, `add_greedy_face`) | `shared_sources` |

### 5.4 `chunk_generator.hpp` (Phase 4, the one structural change)

887 lines, and most of it is inline private helpers called **per voxel** — moving them wholesale
to a `.cpp` would risk generation throughput. Two-part approach:

1. **Extract the chunk-scoped lattice caches** (`build_climate_lattice`,
   `build_land_shape_lattice`, `build_amp_lattice`, `interp_*`, and the arrays they fill) into a
   small `ChunkGeneratorLattice` helper in `worldgen/chunk_generator_lattice.hpp`. This is a
   real
   seam — a chunk-scoped cache with build/interp — and it removes ~230 lines with no call-site
   semantics changed.
2. **Move the cold helpers** (`find_nearest_biome` neighbours, block selection, band maths) to
   `chunk_generator_sample.cpp`. The genuinely hot inlines (`density_at`, `shape_field`,
   `macro_height_at`) stay in the header.

Gate: `scons bench` against `benchmark_baseline.txt` plus a `.freebuff` streaming probe run
before/after; if the delta is not noise, revert step 2 and reach 500 by extracting the
`is_cave`/density debug accessors instead.

### 5.5 Tests (Phase 5) — mechanical, auto-globbed

Split each file by `TEST_CASE` subject group into `<name>_<topic>.cpp`; shared fixtures go to
`tests/<name>_fixtures.hpp`. Largest first:

| File | Lines | → files |
|---|---|---|
| `test_shape_resolver.cpp` | 1814 | **done**: 5 — `_walls` / `_stairs` / `_hanging` / `_reconcile` + `shape_resolver_test_support.hpp` (fence+pane stayed in the owner) |
| `test_concurrency.cpp` | 1113 | **done**: 6 — locks (owner) / `_light` / `_cross_chunk` / `_edit_map` / `_thread_pool` / `_lock_order` + `concurrency_test_support.hpp` |
| `test_player_controller.cpp` | 733 | **done**: 4 — locomotion (owner) / `_water` / `_tick` / `_airborne` + `player_controller_test_support.hpp` |
| `test_fluid_sim.cpp` | 723 | **done**: 3 — cells/writes (owner) / `_wakes` / `_flow` + `fluid_sim_test_support.hpp` |
| `test_play_session.cpp` | 644 | 2 |
| `test_mesh_culling.cpp` | 631 | 2 |
| `test_mesh_builder.cpp` | 625 | 2 |
| `test_fluid_surface.cpp` | 592 | 2 |
| `test_mesh_face_emission.cpp` | 580 | 2 |
| `test_fluid_rules.cpp` | 579 | 2 |
| `test_schematic_reader.cpp` | 578 | 2 |
| `test_biome_amplification.cpp` | 532 | 2 |
| `test_liquid_texture.cpp` | 516 | 2 |

Case count must stay **588** — a split that changes the count means a `TEST_CASE` was dropped.

### 5.6 Tools (Phase 5)

`tools/schematic_report.cpp` (553) → `schematic_report.cpp` (CLI + orchestration) +
`schematic_report_format.cpp` (printers) + `schematic_report_format.hpp`; add the new `.cpp` to
the `schem_tool_objects`/`Program` call.

## 6. Optimizations to fold in (only where the split makes them free)

**All four were done after Phase 6 closed; see §10 for what each one measured. The list
below is kept as written, because the intent is what the work was checked against.**

1. **Narrow the hottest includes.** `chunk_data.hpp` / `chunk_map.hpp` are pulled in almost
   everywhere, and `chunk_data.cpp` is compiled three times. `palette_storage.hpp` should not
   drag Godot types, and `shard_lock.hpp` should not drag `chunk_data.hpp`. Expected real
   incremental-build win; measure warm `scons -j14`.
2. **Decompose the four giant functions while their file is open** (separate, independently
   revertable step): `BlockRegistry::load_from_json` (~400), `BlockEditor::apply_paste` (~248),
   `PlayerController::place_block` (~179), `McPalette::load` (~188). These are the readability
   problems the line cap cannot see.
3. **`texture_array_generator` / `liquid_texture`**: the splits make it possible to move bodies
   into `.cpp` for a real compile-time win — but only where the consumer targets allow it
   (`liquid_texture` must stay header-only for its test; `texture_array_generator`'s only
   consumers are library-side, so it can move).
4. **`PlayerController::_bind_methods`** (~90 lines of `ClassDB` calls) could become a table +
   loop. Optional; do it only if it stays readable.

## 7. Risks and non-goals

- **Lock core is the one place a mistake is expensive.** `chunk_map.hpp` is split by
  copy-with-friend extraction only; the debug `lock_order_checker` assertions plus
  `test_chunk_map.cpp` / `test_concurrency.cpp` are the gate, and Phase 1 lands that change
  alone.
- **Hot-path helpers:** `chunk_generator.hpp`, `mesh_builder_faces/solid`, `shape_resolver`
  bodies are per-voxel. Verbatim motion only, benchmark-gated.
- **Sanitizers are non-Windows-gated** in `SConstruct`, so ASan/TSan verification happens in CI
  only — which is also why the local gate is the full unit suite plus the benchmark baseline.
- **Non-goals:** changing any public header's name, changing class layouts, renaming Godot
  bindings, splitting a class *definition* across files (only nested types and out-of-class
  bodies move), and touching the shader/GDScript side.
- **Local changes are uncommitted work in a shared checkout** — one stage per commit-sized unit,
  never a blanket `git add -A`.

## 8. Expected result

`src/` goes from 22 files over the cap to ~55 files averaging ~150 lines; the largest file in
the
tree becomes a test (≤500). No file over 500, a CI guard that keeps it that way, and the
per-subject naming that makes "where does this live?" answerable from the filename.

## 9. Status — first execution pass

Over the cap: **36 → 0 files.** `python tools/check_file_sizes.py` now prints
`file sizes OK: 354 C++ files, none above 500 lines`, and the plan's own definition of done
(`find src tests tools \( -name '*.cpp' -o -name '*.hpp' \) | xargs wc -l | awk '$1 > 500'`)
prints
nothing. **Phases 0–5 are complete; Phase 6 (docs) is done too** — every new filename is in
`ARCHITECTURE.md`'s key-file list, the test counts there and in `AGENTS.md` are current (90
`.cpp` files in
`tests/`, 588 cases), and `AGENTS.md`'s constraints now state the 500-line rule and the
splitting recipe.
(Was 36 when the plan was written.) Suite green at every checkpoint: `scons -j14`,
`scons test -j14`,
`./bin/run_tests.exe` → **588/588 cases, 346,456 assertions**, and the library links after each
split.
`bin/benchmark --check benchmark_baseline.txt` → all six metrics within baseline after every
hot-path split (`chunk_map.hpp`, `mesh_builder_solid.cpp`, `block_types.cpp`,
`shape_resolver.cpp`).
Run the benchmark **after** the parallel build has finished, not in the same chain as it: the
first
`--check` straight after a 14-way compile has reported `build_mesh` at 3.97 ms against a 3.80 ms
baseline while four runs on an idle machine gave 3.22–3.54 ms.

**Phases 0–5 are complete.** Every library-only `.cpp` has been split: seven
  header extractions, then `chunk_world`, `crash_dump`, `block_editor`, `world_updater`,
  `chunk_manager`, `player_controller` and `voxel_engine_controller`; Phase 3 then split the
  eight
  `shared_sources` files, and Phase 4 took `chunk_generator.hpp` from 888 to 486. Phase 5 has
  taken
  all 13 test files over the cap (`test_shape_resolver.cpp` at 1814 down to
  `test_liquid_texture.cpp`
  at 516) and the one tool, `tools/schematic_report.cpp`.

One **pre-existing flake was observed** during the Phase 2 work: a single `./bin/run_tests.exe`
run
immediately after a parallel build reported `1 failed` (346,455 of 346,456 assertions), and
seven
runs since have been clean. The test program is built from `tests/*.cpp` + `shared_sources`, so
the
file being split (`block_editor.cpp`, `crash_dump.cpp`) is not even in it — the failure cannot
come
from a split, and the repeating that same binary proves it. Worth chasing on its own, not as
part of
the size work.

Count the offenders from the **summary line** of `python tools/check_file_sizes.py`
("file size check FAILED: N file(s) above 500 lines") — the `--list` form prints only the top
25,
so `--list | grep -c OVER` silently pins at 25 and will hide real progress.

### Done

| Phase | Item | Result |
|---|---|---|
| 0 | `tools/check_file_sizes.py` + `scons sizecheck` + CI step | Guard live; failed loudly on all 36, now reports 0 |
| 0 | `SConstruct` basename-collision assert | `raise Exception` on duplicate `shared_sources` basenames |
| 1 | `chunk_data.hpp` | 549 → **253** + `palette_storage.hpp` (312) |
| 1 | `mesh_builder.hpp` | 559 → **465** + `mesh_builder_types.hpp` (119) |
| 1 | `world_updater.hpp` | 542 → **403** + `world_updater_types.hpp` (158) |
| 1 | `block_types.hpp` | 570 → **460** + `block_properties.hpp` (65) + `block_shape_types.hpp` (75) |
| 1 | `texture_array_generator.hpp` | 610 → **484** + `texture_array_generator_build.hpp` (167) |
| 1 | `liquid_texture.hpp` | 613 → **391** + `liquid_texture_settings.hpp` (240) |
| 1 | `chunk_map.hpp` | 826 → **386** + `shard_lock.hpp` (222) + `chunk_map_inline.hpp` (335) |
| 2 | `chunk_world.cpp` | 506 → **421** + `chunk_world_generate.cpp` (103) |
| 2 | `debug/crash_dump.cpp` | 561 → **225** + `crash_dump_report.cpp` (365) + `crash_dump_internal.hpp` (48) |
| 2 | `world/block_editor.cpp` | 731 → **152** + `block_editor_raycast.cpp` (249) + `block_editor_paste.cpp` (361) |
| 2 | `world/world_updater.cpp` | 1132 → **348** + `world_updater_generation.cpp` (253) + `world_updater_columns.cpp` (176) + `world_updater_streaming.cpp` (404) |
| 2 | `godot_bindings/chunk_manager.cpp` | 1181 → **343** + `chunk_manager_properties.cpp` (160) + `chunk_manager_world_api.cpp` (308) + `chunk_manager_schematic.cpp` (197) + `chunk_manager_render.cpp` (239) |
| 2 | `godot_bindings/player_controller.cpp` | 1517 → **285** + `_input.cpp` (237) + `_camera.cpp` (155) + `_interact.cpp` (252) + `_place.cpp` (326) + `_inventory.cpp` (293) + `_internal.hpp` (85) |
| 2 | `engine/voxel_engine_controller.cpp` | 1110 → **289** + `voxel_engine_properties.cpp` (128) + `voxel_engine_config.cpp` (110) + `voxel_engine_schematic.cpp` (284) + `voxel_engine_paste.cpp` (359) |
| 3 | `mesh/mesh_builder_solid.cpp` | 1077 → **271** + `_faces.cpp` (314) + `_cull.cpp` (152) + `_emit.cpp` (374) + `_internal.hpp` (61); `SConstruct` +3 lines in `shared_sources` and +3 in `fuzz_mesh_sources` |
| 3 | `core/block_types.cpp` | 1044 → **70** + `block_types_shapes.cpp` (300) + `block_types_load.cpp` (448) + `block_types_defaults.cpp` (289) + `block_types_internal.hpp` (24); four `SConstruct` edits |
| 3 | `core/shape_resolver.cpp` | 634 → **246** + `shape_resolver_rules.cpp` (404) + `shape_resolver_internal.hpp` (22); `shared_sources` + `fuzz_mesh_sources` |
| 3 | `mesh/mesh_builder_faces.cpp` | 610 → **385** + `mesh_builder_faces_aabb.cpp` (243); `shared_sources` + `fuzz_mesh_sources` |
| 3 | `worldgen/chunk_generator.cpp` | 963 → **472** + `chunk_generator_columns.cpp` (304) + `chunk_generator_terrain.cpp` (221); `shared_sources` + `terrain_tool_objects` |
| 3 | `schematic/schematic_reader.cpp` | 826 → **415** + `schematic_reader_classic.cpp` (171) + `schematic_reader_palette.cpp` (303) + `schematic_reader_internal.hpp` (39); `shared_sources` + `schem_tool_objects` |
| 3 | `schematic/mc_palette.cpp` | 776 → **465** + `mc_palette_resolve.cpp` (350) + `mc_palette_internal.hpp` (22); `shared_sources` + `schem_tool_objects` |
| 5 | `tools/schematic_report.cpp` | 553 → **479** + `schematic_report_format.cpp` (93) + `.hpp` (42): the gaps table and the timed paste plan out of `main`, with `elapsed_ms` and `FailedPastRow` moved to the header; the two new names added to the `Program()` at SConstruct:207. Verified by diffing the report itself on `schematics/13776.schematic --all --plan --repeat 3` before and after: 204 lines, 0 differences once the timings are normalised |
| 4 | `worldgen/chunk_generator.hpp` | 888 → **486**: sampling layer out to `chunk_generator_sampling.cpp` (312), `chunk_generator_debug.cpp` (73), `chunk_generator_config.cpp` (68); lattice cache into `chunk_generator_lattice.hpp` (72) + `chunk_generator_lattice.cpp` (211); `chunk_generator_columns.cpp` 304 → 136 |
| 5 | `tests/test_shape_resolver.cpp` | 1814 → **313** + `_walls.cpp` (424) + `_stairs.cpp` (285) + `_hanging.cpp` (229) + `_reconcile.cpp` (173) + `shape_resolver_test_support.hpp` (450); no `SConstruct` edit (tests are globbed) |
| 5 | `tests/test_concurrency.cpp` | 1113 → **444** + `_light.cpp` (96) + `_cross_chunk.cpp` (95) + `_edit_map.cpp` (90) + `_thread_pool.cpp` (55) + `_lock_order.cpp` (141) + `concurrency_test_support.hpp` (274); 27 cases in, 27 cases out |
| 5 | `tests/test_player_controller.cpp` | 733 → **190** + `_water.cpp` (248) + `_tick.cpp` (140) + `_airborne.cpp` (141) + `player_controller_test_support.hpp` (96); 21 cases in, 21 cases out |
| 5 | `tests/test_fluid_sim.cpp` | 723 → **319** + `_wakes.cpp` (168) + `_flow.cpp` (178) + `fluid_sim_test_support.hpp` (144); 19 cases in, 19 cases out |
| 5 | `tests/test_play_session.cpp` | 644 → **257** + `_persistence.cpp` (376) + `play_session_test_support.hpp` (68); 7 cases in, 7 cases out |
| 5 | `tests/test_mesh_culling.cpp` | 631 → **227** + `_lod.cpp` (412); 21 cases in, 21 cases out — no support header, the one shared helper stayed with its callers |
| 5 | `tests/test_mesh_builder.cpp` | 625 → **156** + `_partial.cpp` (287) + `_regressions.cpp` (85) + `mesh_builder_test_support.hpp` (165); 17 cases in, 17 cases out |
| 5 | `tests/test_fluid_surface.cpp` | 592 → **179** + `_mesh.cpp` (364) + `fluid_surface_test_support.hpp` (103); 18 cases in, 18 cases out |
| 5 | `tests/test_mesh_face_emission.cpp` | 580 → **86** + `_shape.cpp` (217) + `_coplanar.cpp` (270) + `mesh_face_emission_test_support.hpp` (67); 11 cases in, 11 cases out |
| 5 | `tests/test_fluid_rules.cpp` | 579 → **204** + `_scenes.cpp` (286) + `fluid_rules_test_support.hpp` (152); 16 cases in, 16 cases out |
| 5 | `tests/test_schematic_reader.cpp` | 578 → **356** + `_palette.cpp` (248) + `schematic_reader_test_support.hpp` (55); 19 cases in, 19 cases out |
| 5 | `tests/test_biome_amplification.cpp` | 532 (no trailing newline) → **376** + `_envelope.cpp` (131) + `biome_amplification_test_support.hpp` (70); 10 cases in, 10 cases out |
| 5 | `tests/test_liquid_texture.cpp` | 516 → **282** + `_style.cpp` (212) + `liquid_texture_test_support.hpp` (74); 17 cases in, 17 cases out |

### Deviations from the plan (deliberate)

- **`texture_array_generator.hpp`**: the plan said move the bodies into a `.cpp`. The bodies
  stayed
  header-inline (four of the five consumers are in the same library, so the header is the only
  delivery channel anyway) and only the free image helpers plus the two build entry points
  moved.
  It lands at 484 — inside the cap but only by 16 lines, so the `.cpp` move is still worth doing
  later: it would take the header to ~150 and cut compile time in four TUs.
- **`liquid_texture_settings.hpp`** is a new header rather than a new translation unit, for the
  reason the plan gives: `tests/test_liquid_texture.cpp` links no `.cpp` for it.
- **`chunk_map.hpp` needed four `using` aliases rather than the two the plan drew**, plus a
  declaration block: a member's *body* can move out of class, but its *declaration* cannot, so
  the
  class keeps the signatures (and their `[[nodiscard]]`s) and `chunk_map_inline.hpp` holds only
  the
  definitions. Two extra pieces of the plan's recipe proved wrong in practice and are worth
  remembering for every later `.cpp`-style split: (a) out-of-class definitions of ordinary
  member
  functions are **not** implicitly `inline`, so each one needs the keyword explicitly or the
  link
  fails with `LNK2005` in every TU that includes the header; (b) function *templates* are exempt
  from that and must not be given the keyword redundantly. The shard count moved to
  `kShardCount` in `shard_lock.hpp` with a `static_assert(kShardCount <= 64)` tying it to the
  lock-order checker's 64-bit held-shard bitset, and `ChunkMap::kNumShards` aliases it.
- **`crash_dump.cpp`**: the plan called the second file `crash_dump_symbols.cpp`, but the seam
  that
  fell out naturally is *hook lifecycle* vs *report text* — the symbol/module/dump helpers are
  the
  report half, not a peer of it — so it is `crash_dump_report.cpp`. This split broke the plan's
  "verbatim motion" invariant in exactly one place, and for a reason worth remembering: the two
  halves
  share state that is resolved once at install time (`g_stderr`, `g_main_thread`,
  `is_heap_corruption`,
  `write_dump`, `write_report`), and state cannot be shared from an anonymous namespace, so
  those five
  are now declared in `crash_dump_internal.hpp` and `install_crash_dump_handler` reads the two
  globals
  as `crash_detail::g_stderr` / `crash_detail::g_main_thread` — five qualified lines, no other
  change.
  The file-local paths stayed in `debug`'s anonymous namespace, where the nested `crash_detail`
  functions still see them unqualified, which kept `install`'s `_snwprintf_s` block
  byte-identical.
  Verified mechanically (`git show HEAD:` the old file, assert every non-blank line of each half
  appears in its new home): the report half is 100% verbatim, the hook half differs by those 5
  lines
  and nothing else.
- **`block_editor.cpp`** split cleanly along the seams the plan predicted (single edit /
  selection ray
  / bulk paste) with **no shared helper and no new header**: each of the three pieces had its
  own
  anonymous-namespace helpers, so nothing had to be promoted. It is also where the
  narrow-include
  rule paid off most: the owner TU drops `shape_resolver.hpp`, `<algorithm>`, `<array>`,
  `<map>`,
  `<vector>` and `<cmath>` and needs five headers where the parent carried eleven, while the
  raycast
  TU adds only `shape_resolver.hpp` and the paste TU only the mesh/light headers it really
  calls.
- **`world_updater.cpp` needed FOUR files where the plan drew three.** The plan's owner kept
  `update_generation`, which is 234 lines of the file's 539 — with it the owner landed at ~585
  and
  was still over the cap, so `world_updater_generation.cpp` carries it instead and the owner
  holds
  the frame's order (`update`), the unload half, the mesh budgets, the small generator queries
  and
  `clear`/`reset` (348). That is also where the mechanical-extraction trick earned its keep: the
  four
  pieces are contiguous line ranges, so they were cut with a throwaway script by line number and
  then
  proven verbatim per range, rather than retyped. All four TUs compile with their own narrow
  include
  list, and `world_updater.hpp` already pulls `chunk_world.hpp`, `sweep_band.hpp`,
  `column_prefetch.hpp`
  and `world_updater_types.hpp`, so the new files add only `chunk_generator.hpp`,
  `thread_pool.hpp`,
  `mesh_manager.hpp`, `material_manager.hpp` and the std headers they actually call — the old
  `engine.hpp` include turned out to be unused and is gone.
- **Phase 4 took `chunk_generator.hpp` from 888 to 486 in two steps, and the header was 60%
  comments and bodies** — 39 inline definitions accounted for ~500 of its lines, so the lattice
  extraction alone could never have reached the cap. Step one moved the *cold* bodies out by
  responsibility: the sampling layer (20 functions, `chunk_generator_sampling.cpp`), the debug
  accessors (`chunk_generator_debug.cpp`) and construction/configuration
  (`chunk_generator_config.cpp`).
  What stayed inline is exactly the per-cell hot path — `trilinear_interp`,
  `density_from_shape`,
  `sample_shape_3d`, `sample_shape_3d_interp` — because those must still inline into the
  generation
  loop. Step two extracted `ChunkGeneratorLattice`, which owns the four node arrays and the
  builders
  that fill them; `generate_chunk` holds one object instead of four raw arrays threaded through
  five
  signatures, and the reads lost their `wx0/wz0` parameters because the lattice remembers its
  origin.
  The builders moved into the lattice as methods taking `const ChunkGenerator&`, which is why
  `ChunkGenerator` now declares `friend class ChunkGeneratorLattice` — the samplers are private,
  and a
  lattice filled from anywhere else could drift from them.
- **Three new failure modes for mechanical body extraction, all of which cost a build:**
  (a) a constructor's declaration is not its signature — the member-initializer list follows the
  `)`,
  so the declaration has to be taken up to the balanced closing paren; (b) a *nested* return
  type must
  be qualified in the out-of-class definition
  (`ChunkGenerator::ColumnSample ChunkGenerator::x(...)`),
  because the return type is parsed before the definition's `ChunkGenerator::` is seen; (c) the
  bodies
  are indented for class scope and need de-indenting, and a brace that sat alone on its own line
  has
  to stay that way rather than be glued to the signature. All three are in the generator
  script's
  checks now, which also pins each moved body line to its original text (117 lines verified).
- **The bit-identity property is what the tests already guard.** `test_climate_noise.cpp`
  compares the
  cached lattice path against the per-call samplers (and `blend_amplification_lattice_debug`
  against
  `blend_amplification_debug`), so the 588-case suite is the real gate on this refactor — the
  benchmark only checks that taking the bodies out of line did not cost generation time (it did
  not:
  `generate_chunk_avg_ms` 0.232–0.257 against 0.283–0.212 measured before, all runs well inside
  1.4).
- **The last two files of Phase 3 are one subsystem, and they arrived at the same new idea
  twice:
  a `*_detail` namespace.** `schematic_reader.cpp` was an anonymous namespace of parse helpers
  (644
  lines) wrapped around a public API (172), so no two-way split exists — the helpers have to be
  visible to whichever TU holds the parser that calls them. The seam that works is **format**:
  the owner keeps the key scan, the format classification, the shared field reads and the public
  API, `_classic.cpp` takes the numeric-id family, `_palette.cpp` the named-state families.
  Eight
  definitions cross (`fail`, `fail_from_reader`, four field reads, both parsers), so they moved
  out
  of the anonymous namespace into `schematic_detail`, declared in `_internal.hpp`. The owner
  keeps
  calling them unqualified through a `using namespace schematic_detail;` instead of qualifying
  ~30
  call sites.
- **`mc_palette.cpp` needed exactly two names across its seam** — the `{wood}` placeholder and
  `has_placeholder`, both used by the name rows (loader) and the species substitution (resolver)
  —
  so the loader stays the owner and `mc_palette_resolve.cpp` takes the resolution half, the
  shape
  family expansion and the three accessors the name list is built from (`row_for`, `ids`,
  `species_keys`, all used only there). The constant lives in `mc_palette_internal.hpp`.
- **Both new internal headers must not share a namespace.** They would both declare
  `bool fail(std::string*, const std::string&)`; any TU that included both would see two
  declarations
  of one symbol with two definitions in two objects, i.e. a link failure. Hence
  `schematic_detail`
  for the reader and `mc_palette_detail` for the palette — worth remembering before the next
  pair of
  internal headers in one subsystem.
- **`chunk_generator.cpp` needed three files, and the third was forced by one function:**
  `generate_chunk` alone is 453 lines, so \"everything else\" landed at ~510 and was still over
  the cap.
  The two halves that fell out around it are `chunk_generator_columns.cpp` (289) — the
  nearest-biome
  search, the per-column samplers, the chunk-scoped climate/amplification lattices and the
  surface
  material rules — and `chunk_generator_terrain.cpp` (207) — the height range, the
  surface/subsurface
  queries, `generate_fast_path` and `chunk_would_be_fully_solid`. **Nothing crosses:** every
  moved
  definition is a member declared in `chunk_generator.hpp`, so there is no internal header, and
  the
  only `SConstruct` work was two names in `shared_sources` **plus the same two in
  `terrain_tool_objects`** (that list is what standalone tools link, and it names
  `chunk_generator` by
  basename). Each new TU lists only the std headers it actually calls —
  `<algorithm>`/`<limits>`/
  `<vector>` for columns, `<algorithm>`/`<cmath>` for terrain — where the parent carried all
  three.
  This is the file the **Phase 4** `ChunkGeneratorLattice` extraction will touch, so the owner
  being
  `generate_chunk` plus its `perf_timer` is deliberate: that is where the lattice members are
  read.
- **The parent file ends without a trailing newline** (`} // namespace VoxelEngine` is the last
  byte),
  and the rewrite preserves that — `git diff` reports no `\\ No newline at end of file` change,
  which
  is the check to use rather than looking at the file. The two new TUs do end with one, like
  every
  other source in the tree.
- **`mesh_builder_faces.cpp` was the cheapest split in Phase 3** and the only one with **nothing
  crossing**: its anonymous-namespace corner table (`compute_aabb_face_corners`) is called only
  by
  `add_aabb_face`, so the AABB half moved out whole (243 lines) and the owner kept `add_face`,
  `add_greedy_face` and `apply_special_block_offsets` (385). No internal header, no `static`
  promotion, no include changes beyond grouping `<cmath>` with the std headers in both prologues
  —
  each half genuinely calls `mesh_fluid::pack_flow`, `is_fluid_drawn`, `compute_smooth_light`
  and
  `std::lroundf`. A leftover empty `// Side face emission helper` banner sits above
  `apply_special_block_offsets`; it was left exactly where it was rather than tidied mid-split.
- **The parent file's working copy was CRLF while `.gitattributes` says `* text=auto eol=lf`**
  (`git ls-files --eol` reports `w/crlf`, `i/lf` — drift from a checkout older than the
  attribute, and
  it is the only such file in the tree). The rewrite left the parent as it was and wrote the new
  TU
  LF, so neither shows in a diff: the attribute normalizes both.
- **`shape_resolver.cpp` came out as the plan's two files**, but not along the plan's line:
  `shape_resolver_families.cpp` assumed the walk and the family predicates could be separated,
  and they
  cannot — the two neighbour questions (`above_covers_reach`, `wall_reaches`) are asked *by* the
  connector rules and read the cross-section constants the public utilities also read. The seam
  that
  works is **the walk plus the public surface** (constants, the two questions, `resolve_impl`,
  the
  rule-name table, `shape_box_faces`/`shape_face_from_name`, the three `resolve_*` entry points)
  against
  **every rule implementation** (`step_face_of`, the stair geometry, `shape_rule_faces_for`,
  `is_connector`, `self_decided`, `canonical`, `connects`, the post helpers, `present`). Two
  function
  bodies crossing that seam (75 lines) is what needs `shape_resolver_internal.hpp`, which holds
  only
  their two declarations.
- **The cost of that header is real and worth watching:** the two questions were file-local
  statics
  inside the same TU as their only caller, so a compiler could inline them into the per-face
  connector
  path; they are now external symbols in a different TU. The first `--check` after this split
  reported
  `build_mesh` at 3.974 ms (baseline 3.8) and the next four runs gave 3.22–3.54 ms, so the
  failure is
  most likely build contention rather than codegen — but if a future split of this file is
  undertaken,
  moving those two bodies into the internal header as `inline` definitions is the change that
  would
  keep the inlining opportunity.
- **`block_types.cpp` needed four files, not the plan's three**, and its JSON half is the
  largest new
  file in the phase (448) because `load_from_json` is one 398-line function — decomposing it is
  §6 work,
  not placement. The seam that exists is `block_shapes.json` vs `block_definitions.json`: the
  shape
  reader (`_shapes.cpp`) owns the part/AABB vocabulary and the derivation from parts to box
  lists, the
  block reader (`_load.cpp`) owns the definitions and the drop-chain resolution that follows
  them.
  Exactly one function crosses (`apply_shape_to_block`) and it needed a real move, not just a
  declaration: it lived in the anonymous namespace, and a file-local definition cannot satisfy
  an
  external declaration, so the anonymous namespace now closes before it while the other four
  parse
  helpers stay internal. All four `SConstruct` lists name this file — `shared_sources`,
  `terrain_tool_objects`, `fuzz_sources_common` and `fuzz_edit_map_sources` — and the terrain
  one wants
  only what a tool actually calls (`block_types_defaults`, for `path_cost`), not the JSON
  readers.
- **A pre-existing breakage surfaced through that list.** With the two JSON units correctly left
  out of
  `terrain_tool_objects`, the only unresolved symbol left in `bin/terrain_debug` was
  `ItemRegistry::get_instance`, referenced from `inventory.cpp` — a file this phase never
  touched. The
  terrain aliases had simply stopped linking at some earlier point (`bin/terrain_debug.exe` was
  absent,
  and no CI job builds them). Adding `item_registry` to that list fixed it: all eleven
  standalone tool
  targets now build, so the split left the tools better than it found them.
- **`mesh_builder_solid.cpp` needed four files, not the plan's three**, and the first
  `SConstruct` edit of the phase. The arithmetic again: `emit_faces` is 282 lines and needs
  ~320 lines of helpers around it (`box_underside_covered`, `skip_bottom_face`, the
  face-rectangle
  machinery, `drop_neighbor_coverage`), so "emit + its helpers" is ~605 and the helpers had to
  become
  a file of their own (`_faces.cpp`) — the owner keeps the LOD representatives and the solid
  cache,
  `_cull.cpp` keeps the neighbour-chunk and carry-forward-quad logic. That left a real shared
  surface
  for the first time in the phase: two types (`FaceAxes`, `FaceRect`), two constants and four
  functions, all now in `_internal.hpp`, with the three definitions that crossed a TU boundary
  losing
  their `static` (a `static` *declaration* in a header would give each includer its own copy and
  the
  emit TU would link against a definition it cannot see).
- **The build wiring, for the rest of Phase 3:** `shared_sources` (line ~74) and the matching
  `fuzz_mesh_sources` list (~line 285) are the two places that name the file; the fuzz block is
  `if sys.platform != "win32"`, so a missed entry there is invisible locally and only fails on
  Linux
  CI. After the edit `scons test` relinks (`bin/run_tests.exe` links `shared_objects`) and
  `scons bench`
  relinks too — both rebuilt clean, and the six baseline metrics are unchanged.
- **`chunk_manager.cpp` needed five files, not the plan's three.** The owner (lifecycle + the
  whole
  154-line `_bind_methods`) is 343 on its own, and the property surface — the ~170 lines of
  one-line
  `controller->x()` delegations — is a responsibility in its own right, so it got its own file
  rather
  than riding along in `chunk_manager_world_api.cpp` and pushing it to ~455. That leaves four
  files of
  160–343 with real headroom. Two things to remember from this one: (a) the file defines its
  members at
  **global scope** with `using namespace VoxelEngine;` (not inside `namespace VoxelEngine`), so
  every
  extracted TU needs that using-declaration or the compiler says `'ChunkManager': is not a class
  or
  namespace name` ~100 times; (b) the owner's out-of-line `~ChunkManager()` deletes a
  `std::unique_ptr<nav::PathService>` member, so it needs `pathfinding/path_service.hpp` even
  though it
  never names the type — the first include error of the whole phase, and the reason the
  narrow-include
  rule is "cut, then compile, then widen" rather than "cut and hope".
- Both `mesh_builder_types.hpp` and `world_updater_types.hpp` keep the nested spellings
  (`MeshBuilder::SubChunkBounds`, `WorldUpdater::GenerationStats`) alive with a `using` alias
  inside
  the class, so **no call site changed** for either move.
- **`test_shape_resolver.cpp` (1814) needed FIVE files, and the plan's four-file sketch could
  not
  work.** Case count is a hard constraint here (588, and the assertion count 346,456), so the
  split
  is by subject group: fence + pane stay in the owner, then a file each for the wall family (8
  cases,
  424 lines), the stair family (8 cases incl. the canonical-stair check, 285), the hanging
  family
  (4 cases — they own the `StairSweep` struct and its exhaustive sweep, 229), and the two
  chunk-mesh agreement pairs (3 cases, 172). The 24 file-local fixture builders became
  `tests/shape_resolver_test_support.hpp` (450), in a **named** namespace with `inline` on each
  helper — the same shape `tests/schematic_test_data.hpp` already uses — because a header in an
  anonymous namespace would silently give each TU its own copy and a warning-prone one at that.
  The one thing a header does **not** inherit is the includer's `using namespace VoxelEngine;`:
  the first build of the support header failed with a hundred lines of `'BlockAABB': undeclared
  identifier` / `'ShapeRule': is not a class or namespace name`, and the header needed its own
  using-directive before any of it compiled. Worth remembering for every later `*_support.hpp`.
- **`test_concurrency.cpp` (1113) is 21 numbered sections and needed SIX files.** Three of those
  sections were fixtures rather than tests, so they went to `tests/concurrency_test_support.hpp`
  (274) in a named namespace: `TestShardMap` (the 64-shard map that mirrors `ChunkMap` without
  `godot::RID`), `CrossChunkWriter` (the `chunk_world.cpp` queue/drain pattern) and
  `PendingLightRemovals` (the `light_propagator.cpp` insert/fixup-erase pattern). The remaining
  18
  sections grouped by subject: locks (owner, 11 cases), light propagation and the
  pending-removal
  stress (`_light`, 3), the two cross-chunk writer races (`_cross_chunk`, 2), edit-map
  encode/decode
  and CRC32 (`_edit_map`, 3), thread-pool work stealing (`_thread_pool`, 1) and the seven
  lock-order-checker cases (`_lock_order`). The plan drew three files for this one; the fixtures
  are
  what made six the natural answer, and each of the six is a subject that can be found by name.
  Unlike the shape-resolver fixtures, these are *classes*, so no helper needed `inline` (an
  in-class
  body already is), but the header still needed its own `using namespace VoxelEngine;` for
  `ChunkData` — the same lesson as the split before it, now confirmed twice.
- **`test_player_controller.cpp` (733) and `test_fluid_sim.cpp` (723) were the same shape
  twice:**
  a block of file-local fixtures at the top, then cases. Both fixtures blocks went to a
  `*_test_support.hpp` in a named namespace, and both needed the two lessons the earlier splits
  had
  already taught: the header must carry its own `using namespace` lines (these also need
  `using namespace godot;`, because the controller's fixtures name `Vector3` and `PlayerInput`),
  and
  the four `static` helpers became `inline` — the third flavour of the same problem, after a
  `static`
  free function (here) and an `inline`-required out-of-class member (`chunk_map.hpp`). Plain
  `struct`
  fixtures like `SimFixture` and `RecordingSink` needed nothing: an in-class body is already
  inline.
  The player file also shows the value of a *narrow* prologue: the water cases keep
  `register_test_half_block` beside their only user instead of promoting it, since nothing else
  in the
  file calls it.
- **The one non-verbatim edit, and why it is safe to make.** The section banners were numbered
  1–20
  as a table of contents for the whole file; a file whose first banner reads "11." reads as a
  fragment, so each banner kept its text and lost its "N. " prefix (20 lines, the only changed
  text
  anywhere in the move). The script asserts the deltas line by line — every other line in the
  1086
  moved is identical to `HEAD`, and the only lines it may not recognise are a prologue
  `#include`
  that existed in the original prologue or a `using`.
- **Two scripting traps from the same split.** Windows' Python default codec is cp1252, so prose
  written with `open(path, "w")` turns each em dash (U+2014, and the house style uses them) into
  a
  lone 0x97 byte — write source with `encoding="utf-8"` explicitly. And a re-run of the cut
  script
  on an already-split file is a file-eating operation unless it asserts its anchors first: this
  one
  opens by pinning `namespace {` / `} // namespace` / the first `TEST_CASE` line, so the second
  run
  aborts on the anchor instead of re-slicing the owner (the original is recoverable with
  `git show HEAD:tests/<file> > tests/<file>` regardless, which is how the first attempt was
  undone).

### The staged list, closed out

**Nothing is left in Phases 0–6** — all 36 original offenders are under the cap and the guard is
green. The four §6 optimizations have since been done too (§10), along with a
`mc_palette.cpp` / `texture_array_generator.hpp` follow-up they left behind.
**No file under `src/` is over the cap.** The guard warns about exactly one file:
`worldgen/chunk_generator.hpp` (489 — 486 after Phase 4, plus the three lines §13's
`amplification_for`
note added; it has no further planned work). Watched but quiet:
`tests/schematic_test_data.hpp` (479) and `tests/shape_resolver_test_support.hpp` (450), both
under the
480 warning line.
`render/texture_array_generator.hpp` left the watch list entirely (484 → 113) and
`schematic/mc_palette.cpp` needed a second split to get back under it (499 → 373).

1. ~~**Phase 3** — shared `.cpp` splits.~~ **Done**, all eight files: `mesh_builder_solid.cpp`
   (4 files), `block_types.cpp` (4 files + header), `shape_resolver.cpp` (2 files + header),
   `mesh_builder_faces.cpp` (2 files), `chunk_generator.cpp` (3 files), `schematic_reader.cpp`
   (3 files + header), `mc_palette.cpp` (2 files + header). Each needed its names added to
   `shared_sources`; the ones the fuzz targets name (`block_types.cpp`, `shape_resolver.cpp`,
   `mesh_builder_faces.cpp`, `mesh_builder_solid.cpp`) also needed their fuzz list, and the ones
   the
   standalone tools link (`chunk_generator.cpp`, the two schematic files) their tool list.
2. ~~**Phase 4** — `chunk_generator.hpp` (888).~~ **Done**: 888 → 486, benchmark-gated, in two
   steps — the cold bodies out to three `.cpp` files, then the lattice cache into
   `ChunkGeneratorLattice`.
3. ~~**Phase 5** — the 13 test files and `tools/schematic_report.cpp`.~~ **Done**, all 13 plus
   the
   tool: every suite keeps its owner and gains `_<topic>` peers and, where a helper is shared, a
   `*_test_support.hpp`; case counts are unchanged file-by-file (35, 27, 21, 19, 7, 21, 17, 18,
   11,
   16, 19, 10, 17). The tool was verified by diffing its own report on
   `schematics/13776.schematic --all --plan --repeat 3` before and after: 204 lines, 0
   differences.
4. ~~**Phase 6** — `ARCHITECTURE.md` file map + `AGENTS.md` for every new filename.~~ **Done**:
   the
   key-file list names each new file beside its owner, `AGENTS.md` carries the 500-line rule and
   the
   recipe, and the final gate is green (`scons sizecheck`, the `find` above, `scons -j14`,
   `scons test -j14`, 588/588).

### A caution about the mechanical extraction

Two bugs in the generated pieces — one range off by a line (`face_axes` is 78..89, not 78..88)
and one
declaration that kept its opening `{` — produced **hundreds of misleading errors**:
`'ShapeBoxes': is
not a member of 'VoxelEngine::VoxelEngine'`, `symbol cannot be defined within namespace
'VoxelEngine'`,
and `C1075` only at the very end. An unbalanced brace in a header silently nests every following
namespace, so the message to trust is the *first* one and the fix is to check brace balance in
the
generated file before believing anything else. The verbatim check is what caught the range
error; the
other one is why every generated file should be compiled before the next step is built on it.

### Phase 2's mechanics, for Phase 3

- `Glob("src/*/*.cpp")` picks a new file up with **no `SConstruct` edit**, and a new `.cpp`
  starts
  with a *narrow* include list rather than a copy of its parent's — the split then costs no
  extra
  compile time (the totals have gone up ~15% across 20 new TUs, all of it the small fixed
  prologue).
  Cut, compile, widen: the whole phase needed six include fixes total.
- **Prefer mechanical extraction over retyping.** From `world_updater.cpp` on, the pieces were
  cut by
  line number with a throwaway script and each range proven line-for-line against the old file
  before
  building, which removes transcription risk entirely. A split that cannot be expressed as
  contiguous
  ranges is a sign the seam is wrong.
- Two things do not survive a split verbatim and must be handled deliberately: a name in an
  anonymous
  namespace that two halves share (`crash_dump.cpp`'s install-time state,
  `player_controller.cpp`'s
  `kPunchInterval`), and a `using namespace VoxelEngine;` at file scope in a file that defines
  its
  members outside the namespace (`chunk_manager.cpp`, `player_controller.cpp`).
- **Basenames are a build key, and the repo already has collisions**:
  `src/engine/player_controller.cpp`
  sits beside `src/godot_bindings/player_controller.cpp`. The owner must keep the original name,
  and a
  new file may not repeat a basename that is already in `shared_sources`.
## 10. The §6 optimization pass (done)

One commit-sized unit at a time, each gated on `scons -j14`, `scons test -j14`,
`./bin/run_tests.exe` (588/588 cases, 346,456 assertions), `python tools/check_file_sizes.py`,
all 16 aliases linking, and — for anything on a path the benchmark covers —
`./bin/benchmark.exe --check benchmark_baseline.txt` on an idle machine. Where a unit had no
unit-test coverage it was verified against a probe written for the purpose, run before and
after and diffed.

### The four giant functions (§6.2) and `_bind_methods` (§6.4)

| File | Function | Before | After |
|---|---|---|---|
| `src/core/block_types_load.cpp` | `BlockRegistry::load_from_json` | 398 | **40** |
| `src/schematic/mc_palette.cpp` | `McPalette::load` | 211 | **34** |
| `src/world/block_editor_paste.cpp` | `BlockEditor::apply_paste` | 247 | **40** |
| `src/godot_bindings/player_controller_place.cpp` | `PlayerController::place_block` | 178 | **47** |
| `src/godot_bindings/player_controller.cpp` | `PlayerController::_bind_methods` | 89 | **12** (7 area functions) |

- **`load_from_json`** became a frame over six field-group helpers (`parse_render_tables`,
  `parse_light`, `parse_physics`, `parse_tool`, `parse_fluid_state`, `parse_geometry`) plus
  `parse_block_entry`; the post-parse passes moved to the new
  `core/block_types_families.cpp` (**181**) as four private members declared in the header
  (460 → 468 lines, +10). The two identical selection/collision box loops collapsed into one
  `read_boxes`. The old owner was 448 lines and is now 367.
  **Verified against the registry itself**: a new probe (`.freebuff/probe_registry_digest.gd`)
  dumps every GDScript-visible field of all 175 entries — id, hidden flag, side texture,
  resolved selection boxes, registry name, inventory visibility — and the before/after files
  are byte-identical.
- **`McPalette::load`** became a frame over `load_species` / `load_variant_sets` /
  `load_name_rows` / `load_classic_row` (+ `parse_row_variants`). The classic (numeric id)
  rows then moved whole to the new `schematic/mc_palette_classic_rows.cpp` (**157**), because
  the owner had landed one line under the cap (499). That second split needed five readers
  promoted into `mc_palette_detail` — `fail`, `fail_row`, `fail_name`, `is_comment_key`,
  `parse_variants`, plus `kMaxLegacyId` / `kFamilyPlaceholder` — i.e. exactly the
  `foo_internal.hpp` rule the plan prescribes; `mc_palette_internal.hpp` went 22 → 50.
  Owner 466 → **373**. **Verified**: `schematic_report schematics/13776.schematic --all --plan
  --repeat 3` prints the same numbers as before the change (1058270 cells, placed 848892,
  substituted 209062, stilled 316, skipped 35573, unknown 2, air ignored 3393195).
- **`apply_paste`** (361-line file → 404) became a frame over `group_paste_cells` /
  `write_paste_group` / `write_paste_cell` / `persist_paste_edits` / `publish_paste_writes`,
  with one `PasteWrite` accumulator struct replacing ten locals. The per-cell guards turned
  from `continue` to `return`, and the body guard it shared with the pour path became
  `cell_hits_player_body`. This function had **no unit-test coverage at all**, so it was
  verified with `.freebuff/probe_paste.gd`: the probe pastes four real builds, checks the
  full-size case cell by cell, re-pastes with gaps, undoes, and then pastes two huge
  cross-chunk files. Its stable lines (planned/written/restored counts, substitution stats,
  "box holds exactly 3189 written cells", and the settled totals 17005 cells / 27 chunks and
  504938 / 113) are identical before and after; only the streamed-chunk counters differ, and
  those differ between two runs of the *same* binary as well (checked by running it twice
  unchanged).
- **`place_block`** (326-line file → 371) became a frame over `resolve_held_block`,
  `merge_slab_placement`, `rotate_stair_placement` and `merge_wall_placement` — the three
  family rules that were inline in it — all file-local, so the header did not grow.
  **Verified** with `.freebuff/probe_torch_place.gd` (item → block placement end to end: the
  item places block 150, consumes the item, wears the item's texture, and breaking it returns
  the item) and `.freebuff/probe_bucket.gd` (the pour path whose guard it now shares).
- **`_bind_methods`** was split into seven static member functions (`bind_actions`,
  `bind_inventory_api`, `bind_ui_state`, `bind_tuning`, `bind_state_and_view`, `add_signals`,
  `add_properties`), every bind line verbatim, methods first and then signals then properties
  as before. **Verified** by a new probe (`.freebuff/probe_bindings.gd`) that walks the live
  `ClassDB` list: all 58 methods, 7 signals and 4 properties are registered, and one live
  call per group answers.

### `texture_array_generator` bodies out of the header (§6.3)

`render/texture_array_generator.hpp` 484 → **113** lines. The bodies moved to
`texture_array_generator.cpp` (**416**) and `texture_array_generator_emissive.cpp` (**158**),
with the four image helpers both need in a new `texture_array_generator_internal.hpp` (**33**).
The old `texture_array_generator_build.hpp` (167) is gone. **Verified** by a new probe
(`.freebuff/probe_texture_array.gd`): the albedo array comes out with 36 layers — exactly the
number of distinct texture names in `block_definitions.json` — at 16×16, RGBA8, mipmapped and
writable, and every declared texture either owns a layer or is the documented `stone.png`
fallback (`wood_top` is the one, and it has no texture file). `liquid_texture.hpp` deliberately
did **not** follow: its test links no `.cpp`.

### The warm-build measurement (§6.1)

Idle machine, `-j14`, SCons' content-signature decider, one line appended to a header to force
the rebuild (**`touch` alone changes nothing**, so a measurement has to change the bytes).
Touching a header costs:

| Touched header | TUs rebuilt | Wall time |
|---|---|---|
| `core/block_types.hpp` | 83 | 29.4 s |
| `core/chunk_data.hpp` | 65 | 26.5 s |
| `mesh/mesh_builder.hpp` | 42 | 22.7 s |
| `render/texture_array_generator.hpp` | 6 | 13.0 s |

A no-op `scons -j14` is 11 s, so ~11 s of every incremental run is fixed cost, and each TU is
roughly 0.2–0.3 s of the rest at this parallelism. Two conclusions worth keeping:

- **Editing an implementation now rebuilds one TU.** Appending a line to
  `texture_array_generator.cpp` costs 1 TU / 11.7 s, against the four TUs that included the
  same code when it was inline in the header. That is the whole point of the §6.3 move.
- **The remaining build win is `block_types.hpp`.** At 83 TUs it is the most expensive header
  in the tree, and it is big because it holds the whole `BlockType` struct *and* the
  `BlockRegistry` class with its inline helpers. Splitting `BlockType` out into its own header
  (which most of those 83 TUs only need) is the next measurable win; it is a header-shape
  change rather than a line-count one, so it was left out of this pass.

### What the pass left behind

- The tree is now **354 C++ files, none above 500 lines**; the guard warns about one file
  (`chunk_generator.hpp`, 486).
- New reusable probes live in `.freebuff/`: `probe_registry_digest.gd` (the block registry as
  a diffable fingerprint), `probe_bindings.gd` (every bound method/signal/property),
  `probe_texture_array.gd` (the generated albedo array against the block data).
- Two files are now editable without a rebuild cascade: the texture-array build (1 TU) and
  everything that moved into the new `.cpp` files.

## 11. The portability pass (done)

CI builds this tree with GCC and clang on ubuntu as well as with MSVC on Windows, but the
Windows compiler is the one in front of the developer, so the other two only ever see a
mistake after it is committed. Two shapes of mistake got through that gap, both introduced
by the file-size work above and both green on MSVC:

| Symptom on GCC/clang | Where | Cause |
| --- | --- | --- |
| `fatal error: dbghelp.h: No such file or directory` | `src/debug/crash_dump_report.cpp:18` | the split moved the Win32 includes above the `#ifdef _WIN32` line, leaving `dbghelp.h`/`tlhelp32.h` in the non-Windows path |
| `standard attributes in middle of decl-specifiers` | `tests/fluid_sim_test_support.hpp:39` | the extraction wrote `inline [[nodiscard]]`, which MSVC parses and no other compiler does |

Both are fixed: the report's Win32 includes sit inside its guard again (the file is empty on
non-Windows platforms by construction), and the attribute comes first. The same class was
then swept for deliberately:

- **A strict second opinion, locally.** MSVC is the weakest of the three compilers, and
  `clang-cl -fsyntax-only` with the flags from the generated `compile_commands.json` is a
  drop-in, much stricter check on any translation unit — no build, no link. All 18
  translation units this pass created or rewrote were put through it: **0 failures**. That is
  now the pre-commit check for a file that is new or newly split.
- **File-local helpers in a named namespace** carry external linkage. `crash_dump_report.cpp`
  had seven of them sitting in `crash_detail` beside the functions `crash_dump.cpp` calls, so
  a future same-named helper in either file would have collided at link time rather than
  compile time. They now sit in two anonymous-namespace blocks; only the seven declarations
  in `crash_dump_internal.hpp` keep the shared linkage. (This is the same trap the recipe
  already records for `static` helpers that two files end up sharing — here it is the
  mirror image.)
- **Missing standard headers.** A `std::function` without `<functional>`, a
  `std::vector<std::pair<...>>` without `<utility>`, a `std::string` without `<string>`:
  all four reach GCC only through a transitive include today, and all four now name what they
  use (`block_types_load.cpp`, `mc_palette.hpp`, `block_editor_paste.cpp`,
  `player_controller_place.cpp`, `schematic_report.cpp`).

### The guard, so it cannot happen twice

`tools/check_portability.py` — dependency-free, `--selftest` proves each check fires — fails
when a Windows-only include, `#pragma comment`/`#pragma warning`, an MSVC intrinsic or a
Win32 type sits outside an `#ifdef _WIN32` (or `defined(_WIN32) || defined(_MSC_VER)`) region,
or when `[[...]]` is written after a decl-specifier. It tracks the preprocessor guard stack
properly, so `#else`/`#elif`/nesting are handled and `#ifndef _WIN32` counts as the *other*
platform's branch. Wired as `scons portability` **and** as a CI step beside the size check,
because both are the same kind of cheap, mechanical, pre-build rule.

It reports **0 findings** on the current tree.

### Gate after this pass

`scons -j14` + all 16 aliases link; `./bin/run_tests.exe` **588/588 cases, 346,456
assertions**; `python tools/check_file_sizes.py` → **354 files, none above 500 lines**;
`scons portability` → clean; `benchmark.exe --check` → all six metrics within baseline (the
first run after a parallel build showed `incremental_mesh_avg_ms` 0.261 against a 0.250
baseline; three idle re-runs gave 0.238 / 0.227 / 0.182, so that was build noise, which is
what the baseline's 2× headroom is for).

## 12. The fuzz link (done)

Third member of the same family as §11: coverage that exists only in CI, on a code path the
local compiler never touches. The libFuzzer targets are built under
`if sys.platform != "win32"`, so no Windows loop ever links `bin/fuzz_*` — and the fuzz
source lists are explicit lists of translation units, which is exactly where a split can
leave a hole.

| Symptom on CI | Cause |
| --- | --- |
| `/bin/ld: undefined reference to VoxelEngine::ItemRegistry::get_instance()` while linking `bin/fuzz_palette` | `src/core/inventory.cpp` is in `fuzz_sources_common`, and its save decoder range-checks an id against `ItemRegistry` (added with the CRC/size commit), whose loader is godot-only |

The diagnosis, and why the obvious fix is wrong:

- **Adding `core/item_registry.cpp` to the list is not open.** Under
  `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION` that file cannot compile: it includes
  `core/json_config.hpp`, whose loader declares `std::optional<godot::Dictionary>` with
  `godot::Dictionary` only forward-declared (MSVC's STL rejects that outright, and it is not
  a shape to rely on elsewhere), and its own `report_unresolved_name` calls `ERR_PRINT`,
  which exists only in the godot headers. Confirmed by compiling it with the fuzz define.
- **No harness uses `Inventory` at all.** Every one of the five includes `chunk_data.hpp` /
  `block_types.hpp` / `edit_map.hpp` / `mesh_builder.hpp` and nothing else. It was dead
  weight in three targets, carried since before the item registry existed.
- So the source came **out** of `fuzz_sources_common`, with a comment saying why and what a
  future harness has to bring if it wants the save decoder. The validation rule stays single:
  fuzz builds validate saves exactly as the engine does, because nothing in a fuzz list calls
  a godot-only path at all.

### Verified without a Linux box

`clang` is installed on the Windows side, and the fuzz flags are plain clang flags, so the
whole target set can be compiled with the fuzz define and linked against a stub `main` with
the source lists read **out of `SConstruct`** (so the check cannot drift from the real
targets). It reproduced the CI failure exactly — `fuzz_palette`, `fuzz_light`,
`fuzz_mesh_builder` failing on `ItemRegistry::get_instance`, `fuzz_chunk_load` and
`fuzz_chunk_recovery` linking clean — and after the change prints **5 of 5 targets linking**.
That reproduction is the proof the fix addresses the real link set and not a guess at it.

### Gate after this pass

`scons -j14` + all 16 aliases link; `./bin/run_tests.exe` 588/588 cases, 346,456 assertions;
`scons sizecheck` → 354 files, none above 500 lines; `scons portability` → clean; the fuzz
target set links (**5 of 5**); `benchmark.exe --check` → all six metrics within baseline.

## 13. The clang-tidy pass (done)

Fourth member of the same family as §11 and §12: a gate that only runs on CI. A split can
smuggle in a shape the Windows build accepts without a murmur, and the static-analysis job
is where it surfaces — one finding at a time, over days.

| Symptom on CI | Cause |
| --- | --- |
| `warning: the const qualified variable 'name' is copy-constructed from a const reference` in `schematic/mc_palette_classic_rows.cpp` | `minimal_json.hpp`'s `as_string()` returns `const std::string&`; the moved-out reader kept a `const std::string name = value.as_string();` |
| `warning: returning a constant reference parameter may cause use-after-free … [bugprone-return-const-ref-from-parameter]` in `worldgen/chunk_generator_sampling.cpp` | `ChunkGenerator::amplification_for` returned its `const BiomeAmplification& blended` **parameter** by reference, and `quick_height_estimate` passes a temporary into it |

The second is a **real latent use-after-free**, not a style nit, and it is the reason this
pass is worth the machinery: with `climate_blend_radius_nodes > 0` the returned reference
pointed into the argument temporary, which died at the end of the initializing statement, so
the very next line read freed stack. It survived because (a) blending is off in the default
config, and (b) the memory is usually still readable. The three other call sites all pass a
named local, which is what kept the bug invisible.

Both fixes are at the root rather than at the shape:

- **`amplification_for` now returns `BiomeAmplification` by value** (five floats) with a comment
  saying why, and all five call sites hold a value instead of a `const&`. A `const&` return
  is the wrong contract for a function whose argument is routinely a temporary.
- **`const std::string& name = value.as_string();`** in the classic-row reader — a reference to
  a reference, used only for `std::find_if` capture and comparisons.
- The two remaining findings are **Windows-only and therefore invisible to this gate**: CI's
  static-analysis job is `runs-on: ubuntu-latest`, and `crash_dump_report.cpp` is
  `#ifdef _WIN32`
  from line 16 to 386, so Linux sees an empty translation unit. Both are
  `performance-no-int-to-ptr` on casts that Win32 requires (`GetModuleHandleExW`'s
  from-address argument, and walking the captured `CONTEXT::Rsp`). They are suppressed with
  the house `// NOLINTNEXTLINE(check) - reason` form so the same command gives the same answer
  on Windows as on Linux.

### Reproducing the gate locally

A throwaway sweep ran **CI's exact command line** over the **exact file list** —
`find src -name '*.cpp'` is 112 files, and 112 is what a fresh `compile_commands.json`
carries, so there is no shortcut to take there. Two details make it faithful:

- Dedupe the database by lowercased normalized `.file` first, exactly as the workflow's `jq
  'unique_by(.file)'` does; SCons emits one entry per build target.
- **Drop `clang-diagnostic-*` findings.** The local database is full of MSVC-only flags
  (`/c`, `/experimental:external`, `/external:anglebrackets`), and clang reports
  `clang-diagnostic-unused-command-line-argument` on essentially every file because of them —
  340 findings that Linux CI can never print. Filtering the diagnostic namespace, not the
  individual check, is what separates the 2 real findings from the 342 artifacts.

### Gate after this pass

`python tools/check_tidy.py` → **0 findings in 0 files** (112 sources, CI's checks);
`scons -j14` + all 16 aliases link; `./bin/run_tests.exe` → 588/588 cases, 346,456 assertions;
`scons sizecheck` → 354 files, none above 500 (`chunk_generator.hpp` 486 → 489, still 11 under);
`scons portability` → clean; `benchmark.exe --check` → all six metrics within baseline
(`generate_chunk` 0.225 ms against 0.265 ms before the by-value change — the copy is not
measurable at one call per column).

## 14. The docs pass (done)

The code got a cap and three guards; the documents had no cap and no guard, so they rot the
same way and nobody notices. This pass gave them the same treatment.

- **A gate.** [../tools/check_docs.py](../tools/check_docs.py) fails on a cited path that does not
exist (path, glob, bare basename or `block_types.hpp/cpp` shorthand), on a link into a heading
that does not exist, on a count in prose that no longer matches the tree (test cases, file
counts, `MAX_BLOCK_TYPES`, the size guard's own total) and on an unreadable line or file; it
warns on a section over 3,000 words and on a table cell over 400 characters, because markdown
gives you no way to wrap either. `scons docscheck`, plus a CI step, plus `--selftest`.
- **The citations resolve.** 47 references pointed into `.freebuff/`, which is untracked: a
clone had none of the probes the docs named as their proof. The scripts are un-ignored now
(`!.freebuff/*.gd|sh|py`), the 231 MB of probe output is ignored instead, and
`tools/check_tidy.py` (the local clang-tidy run) writes to `build/` rather than in there. The
gate flags a citation that git IGNORES, since a clone can never have one; the 34 scripts named
by the docs must therefore be added when this work is committed, or CI's docs check fails on a
clean checkout.
- **Seven sections moved out.** Every section over the warning budget became a file under
`docs/` — shapes, streaming and the GDScript UI from ARCHITECTURE.md; shapes, rendering,
inventory/GUI and build files from AGENTS.md — with a short summary and a link left behind. The
text was moved by script and checked word-for-word, not retyped: 30,957 words across seven
files. The rendering section was the hard case, since 5,400 of those words were a SINGLE bullet
about the shader stack; it is now one subsection per effect (Hand Drawn, CRT Screen, Phosphor
Trail, World Bend, Horizon Curve), cut at sentence starts.
- **README stopped being a second copy.** Its Key Systems table had cells up to 5,531
characters — an essay in a cell cannot be reviewed either. It is now one short row per system
pointing at the document that owns the detail.

The gate then caught two things in my own work: `check_docs.py` originally ran the test binary
once per document (13 runs of a 20-second suite; now once, cached, `CHECK_DOCS_NO_SUITE=1` to
skip), and a prose glob (`mesh_manager*.cpp`) that names no file. Result: 20 markdown files,
0 problems and 0 warnings; AGENTS.md 2,286 → 863 lines, ARCHITECTURE.md 1,883 → 1,110.
