# How to change things

Each recipe is the list of files a change actually touches, in the order you
touch them, with the mistake that each step is easy to make. Field meanings are
in [data-schemas.md](data-schemas.md); the "why" is in
[ARCHITECTURE.md](../ARCHITECTURE.md).

## Add a block

1. **Append** an entry to `data/block_definitions.json`. It is an array and the
   id is the position, so appending is safe and inserting is not: inserting
   shifts every later id and corrupts existing saves. Expect the registry to sit
   around 175 of `MAX_BLOCK_TYPES` = 256.
2. Give it `properties`, `visible_faces`, `textures`, `emissive_textures` (see
   the schema page). The minimal working entry is `name` plus those.
3. Put the texture in `textures/blocks/<name>.png` (16×16). **A new PNG needs
   Godot to import it** before the game can load it: `godot --headless --path .
   --import`. An open editor makes that pass fail on the extension copy while
   still writing the `.import` files.
4. Decide what mining it yields: `drops` (a block *or* an item name),
   `crush_result` for a hammer, `hardness` / `preferred_tool` / `min_tier` for
   the break time.
5. Run `scons docscheck` — the schema page states the entry count, and the gate
   will tell you if you need to update it.

For the flat shape batch (fence, torch, ladder, chain, carpet, glass) the art is
*generated*, because a box samples its own region of the texture and the layout
is not free: `python tools/make_block_textures.py`.

## Give a block a shape

1. If the shape already exists, set `"shape": "<name>"` on the block (or
   `/<variant>`, e.g. `slab/bottom`). A name that does not resolve leaves the
   block a **full cube** and only logs an error, so read `get_selection_boxes(id)`
   back rather than trusting the log — `.freebuff/probe_crucible.gd` is the
   pattern.
2. If it is new, add `data/block_shapes.json` entry `<shape>/<variant>` with
   `selection_boxes` (a list of `[min_x, min_y, min_z, max_x, max_y, max_z]` in
   0..1 cell units). Add `collision_boxes` **only** when the visible model is not
   what should stop a body.
3. For anything neighbour-aware, do not write a variant per state. Use `parts`
   with a `rule` (the names are in data-schemas.md). The claim follows the boxes,
   so you cannot author a rule against a face the part does not reach — which is
   the point: geometry that resolves at mesh time has no update pass to get wrong.
4. If it is a **family** (several ids that merge or place into one another), the
   family tables are built in `src/core/block_types_families.cpp` and referenced
   by `slab_family` / `stair_family` / `wall_family` in the block definitions.
5. Covered by `.freebuff/probe_shapes.gd` — run it, and add your case.

## Add an item

1. Append to `data/items.json`. Entry order is the item id.
2. `texture` is a bare name resolved under `textures/items/` (items are not part
   of the block texture array).
3. Optional behaviour, all declarative: `tool` (`class`/`tier`/`speed`), `light`
   (`level` + `color`), `use` (`kind: pour` writes `block` where you are aiming;
   `kind: fill` empties the fluid source you are aiming at and swaps the item for
   that fluid's bucket), `pose` (a viewmodel resting position — read by
   `viewmodel.gd`, not the registry).
4. **An item is never placeable without `place`.** `place.block` names the block,
   and `place.wall` → `{n, s, e, w}` names the states that hug a face; placement
   resolves the clicked face and consumes the *item*. That is the only bridge
   from item space into block space, and it is what keeps tools out of voxels.
5. `python tools/make_block_textures.py` regenerates the art for the torch, which
   is the one item whose texture is shared with a block.

## Add a biome

Four places, and the enum is the identity, so the JSON key must match it:

1. `src/worldgen/biome_config.hpp` — add to `enum class BiomeType` and bump
   `Count`.
2. `src/worldgen/biome_config.cpp` — `biome_name()` must return the name the JSON
   uses.
3. `data/biomes.json` — add the `biomes.<name>` object (see the schema page), and
   tune `climate` thresholds if the new biome is a new band.
4. `src/worldgen/chunk_generator.hpp` — `kLandBiomeGrid` is the 3×3
   temperature×humidity table a land column is placed by.
5. `scons test` — `tests/test_biome_amplification.cpp` and
   `tests/test_climate_noise.cpp` are the ones that will notice.

## Add a crafting recipe

Append to `data/recipes.json`. Shaped recipes carry `pattern` (rows of
characters, `" "` for empty) and `key` (character → ingredient); shapeless carry
`ingredients`. `result` is `{block, count}`. A `key` entry may be a **list**, and
the loader expands one concrete recipe per combination — that is how a wooden
tool is one wood type throughout rather than a mix.

## Add a chat command

Two places in `chat.gd`, plus the help text:

1. `COMMANDS` — the autocomplete list.
2. The dispatch `match cmd:` blocks. There are two of them: one for execution and
   one for autocomplete argument hints, and a command added to only one works
   silently in the wrong half.
3. The `/help` output.
4. If the command needs engine state, add the binding in
   `src/godot_bindings/chunk_manager*.cpp` or
   `src/godot_bindings/player_controller_*.cpp` rather than reaching into the
   scene tree from the chat script.

## Add a C++ file

`SConstruct` builds the library from `Glob("src/*.cpp") + Glob("src/*/*.cpp")`, so a
new file needs **no build edit** — unless something else has to link it, and there are exactly
five lists
that name files explicitly: `shared_sources` (shared by the library, the tests
and the tools), the fuzz source lists, `terrain_tool_objects`,
`schem_tool_objects`, and a tool's own `Program()`. `SConstruct` raises on a
duplicate basename, so two files may not share a name.

If a `shared_sources` file is listed in a **fuzz** list too, remember those
targets are godot-free by construction: a file in one may not reach a loader or a
registry behind `FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION`. Such a target links
on no machine but the CI runner, so you cannot check it locally by accident.

Keep the file under 500 lines (`scons sizecheck`) and wrap prose at 96 columns in
any doc you touch (`scons docscheck`).

## Add a test

Put it in `tests/test_<subject>.cpp`; the glob picks it up and there is no
registration. Use `CHECK`/`CHECK_FALSE` — this build compiles doctest with
exceptions off, so `REQUIRE` becomes a `static_assert` and `INFO`/`CAPTURE`
become no-ops. If a subject needs more than one file, keep the original name for
the main subject and add `_<topic>` peers, with shared fixtures in
`tests/<subject>_test_support.hpp` in a named namespace with `inline` helpers.

## Add a screen shader effect

1. Write `shaders/<name>.gdshader`.
2. Add an entry to `data/shaders.json` with `id`, `name`, `kind`, `shader` and
   `params`. The menu builds its page from the registry, so no menu code changes.
3. If the effect needs the previous frame, declare a `previous_frame` uniform —
   that name is the contract and `shader_overlay.gd` builds the history for you.
4. If it must sample the screen at its own place in the stack, it needs its own
   `BackBufferCopy`; the overlay does that for screen and world passes, and a
   pass without one reads a frame that is missing the passes under it.

## Change the save format

Do not — not casually. Add a field and bump the version, and keep the reader able
to load the previous versions transparently (`v3` CRC32, `v2` RLE, `v1` flat).
The writer lives in `src/world/chunk_world_persistence.cpp`; the pure formats
that the tests share live in `src/core/edit_map.hpp` and
`src/core/inventory.hpp`.

## Check that any of it still works

```bash
scons -j14 && scons test -j14 && ./bin/run_tests.exe
scons sizecheck && scons portability && scons docscheck
./bin/benchmark.exe --check benchmark_baseline.txt
```

and for anything that only exists on screen, a probe: [probes.md](probes.md).
