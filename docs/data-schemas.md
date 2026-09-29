# Data file schemas

Everything tunable in Farlands lives in `data/*.json`, and each file has exactly
one reader. This page is the field reference: what each key means and which
translation unit reads it. Where a key's exact type matters, the reader is the
authority and is named in each section.

Two rules apply to all of them:

- **A `_comment` key is documentation, not data.** Every reader ignores it, and
  several files use it heavily to explain a block of entries in place. `".."`
  (a list of strings) and `"..."` (a string) both work.
- **Names are the interface.** Entries refer to each other by name — a block's
  `shape` is a key in `block_shapes.json`, an item's `place.block` is a block
  name — and the loaders resolve them after all files are in. A name that does not
  resolve is logged, and the usual failure is silent-looking: an unresolved
  `shape` leaves the block a full cube.

## data/block_definitions.json

**A JSON array, not an object.** The block id is the entry's *position*, so the
order is the save format: **append new blocks, never insert one.** Inserting
shifts every later id and corrupts existing worlds. 175 entries today, against
`MAX_BLOCK_TYPES` = 256 (`src/core/block_types.hpp`). Read by
`src/core/block_types_load.cpp` (`parse_block_entry` and its per-group helpers),
with the cross-block references resolved in `src/core/block_types_families.cpp`.

| Key | Type | Meaning |
|---|---|---|
| `name` | string | The block's name, and the id every other file uses to refer to it |
| `properties` | list of strings | Capability flags: `Solid`, `Transparent`, `Opaque`, `Liquid`, `RenderAllFaces`, `NoOcclusion`, `Emissive` (`src/core/block_properties.hpp`). `Liquid` is what makes `stops_bodies()` false, i.e. a body walks through it |
| `visible_faces` | 6 booleans | Per face, whether it is drawn. Face order is the mesh's own (`+x, -x, +y, -y, +z, -z`) |
| `textures` | 6 strings | Per face, a bare texture name resolved to `res://textures/blocks/<name>.png`. Empty means that face draws nothing |
| `emissive_textures` | 6 strings | Per face, the glow map, same resolution |
| `light` | 3 numbers | The colour this block emits. All zero means it emits nothing |
| `light_opacity` | number | How much light the block swallows as light passes through it |
| `hardness` | number | Break time divisor; `0` (or absent) is instant |
| `preferred_tool` | string | The tool class that mines this block quickly |
| `min_tier` | number | The tool tier below which mining is not rewarded (it never *blocks* the break) |
| `top_face_offset` | number | Lowering of the drawn top face, for a block whose surface sits below the cell top |
| `slipperiness` | number | Ice-like friction; 0.6 is the default |
| `shape` | string | A key in `block_shapes.json`. A name that does not resolve leaves the block a full cube and only logs |
| `hidden` | bool | Not offered by `/give` or its autocomplete |
| `drops` | string | What mining yields — a block name, or an **item** name (that is what lets a placed torch return the torch item). A crush wins over it |
| `crush_result` | string | A hammer yields this block instead of the block itself (cobblestone → gravel) |
| `fluid` | string | This block *is* a fluid state (`water`, `water_runoff_3`, `lava_fallen`, ...). The rules in `src/fluids/` resolve these by name, never by id |
| `slab_family` / `stair_family` / `wall_family` | string | The material's shape family. Members are the ids a merge or a paste may produce; the base member is what a player holds |

## data/block_shapes.json

An object keyed by **shape key**, which is `<shape>` for a shape with no
variants and `<shape>/<variant>` for one with (`slab/bottom`, `stair/n`,
`fence/all`, `crucible`). Read by `src/core/block_types_shapes.cpp`
(`parse_shape_dict`, `parse_parts_array`).

| Key | Type | Meaning |
|---|---|---|
| `selection_boxes` | list of boxes | Raycast target and, unless `collision_boxes` is given, the collision too |
| `collision_boxes` | list of boxes | Present only when the visible model is *not* what should stop a body (the `pole`, which is 1.5 high while it looks 1 high) |
| `parts` | list of part objects | The neighbour-aware half: each part contributes its boxes to the static lists only while its rule holds |

A box is `[min_x, min_y, min_z, max_x, max_y, max_z]` in cell units (0..1, so
`0.0625` is one texel of sixteen). A part object:

| Key | Type | Meaning |
|---|---|---|
| `boxes` | list of boxes | The geometry this part draws |
| `collision_boxes` | list of boxes | Optional separate collision for this part |
| `rule` | string | When the part is present, from `shape_rule_from_name` (`src/core/shape_resolver.cpp`): `fence`, `pane`, `wall_arm`, `wall_bearing`, `wall_post`, `stair_step`, `stair_cut_left`, `stair_cut_right`, `stair_corner_left`, `stair_corner_right`, and `null`/absent for unconditional |
| `faces` | list of strings | Which neighbour cells the rule asks about: `n`, `s`, `e`, `w`, `up`, `down`. Optional — by default the claim is **derived from the boxes**, so a rule cannot be written against a face its boxes do not reach. Only the stair rules must declare it, because what they ask about is not where their boxes are |

An unknown `rule` name is logged and the part stays unconditional, which draws
too much rather than too little. A declared `faces` list that disagrees with the
boxes is overruled and warned about at load.

## data/items.json

`{"items": [...]}`, 17 entries. Read by `src/core/item_registry.cpp`; the
`texture` name resolves under `res://textures/items/`, because an item is not
part of the block texture array.

| Key | Type | Meaning |
|---|---|---|
| `name` | string | The item's name. Entry order is the item id, so append |
| `texture` | string | Bare name under `textures/items/` |
| `tool` | object | `class`, `tier`, `speed` — what the item is as a tool |
| `pose` | string | A resting pose id the viewmodel knows (`item1`, `item2`, ...). Read by `viewmodel.gd`, not the registry |
| `light` | object | `level` and `color` ([r,g,b]). While the item is selected it *owns* the player's dynamic light |
| `use` | object | `kind` plus its argument: `pour` writes `block` into the aimed cell, `fill` empties the fluid source aimed at and swaps the item for that fluid's bucket |
| `place` | object | The one bridge from item space into block space: `block`, plus `wall` → `{n, s, e, w}` for the states that hug a face. Without it an item is never placeable |

## data/biomes.json

Read by `src/worldgen/biome_config.cpp`.

| Key | Type | Meaning |
|---|---|---|
| `underwater_surface` | string | The block used for a surface that ended up under water |
| `climate.temp_cold_max` / `temp_hot_min` / `hum_dry_max` / `hum_humid_min` | number | The thresholds of the 3×3 temperature×humidity land grid: below cold and above hot are the outer bands, between them is the temperate one |
| `biomes.<name>` | object | One entry per biome; `<name>` is the enum's name (`plains`, `hills`, `ocean`) |

Each biome object:

| Key | Type | Meaning |
|---|---|---|
| `surface` / `subsurface` | string | The blocks laid on the top two layers |
| `near_water_surface` / `near_water_subsurface` | string | Swapped in where the surface is close to water level |
| `height_amplification` | number | Multiplies the macro height's distance from sea level (1.0 neutral) |
| `weirdness_amplification` | number | Scales the weirdness mask that gates the 3D density field |
| `min_weirdness_amplification` | number | The mask floor, expressed as an offset above neutral (1.0 = no floor) |
| `weirdness_size_amplification` | number | Scales the whole 3D-shape envelope — strength *and* surface band, and so `density_margin()` |
| `preferred_continentalness` / `preferred_temperature` / `preferred_humidity` | number | Reference data for profile-based biome selection. Sampled and carried today, not yet consulted by selection |
| `tree_density` | number | How likely a qualifying column gets vegetation |
| `tree_variants` | object | Variant name → weight (`{"oak": 1.0, "spruce": 0.0}`) |

All four amplification knobs are blended across biome borders over
`climate_blend_radius_nodes` (see `terrain_config.json`); radius 0 makes every
column use its own biome's values exactly.

## data/vegetation.json

Read by `src/worldgen/vegetation_config.cpp`.

| Key | Type | Meaning |
|---|---|---|
| `tree_trunk_height` | number | Trunk length in blocks |
| `hills.chunk_chance_pct` | number | Percent of qualifying chunks that get a tree at all |
| `hills.spacing_radius` | number | Minimum spacing between trees, in chunks |

## data/terrain_config.json

A **flat object of 25 numbers** — no nesting. Read by
`src/core/terrain_params.cpp` into `TerrainParams`. The layer names and what the
stack does with them are diagrammed in
[ARCHITECTURE.md](../ARCHITECTURE.md#terrain-generation); the values are at the
top of the file and every one of them is a dial for the macro surface.

| Group | Keys |
|---|---|
| Base height | `height_base_y` |
| Macro domain warp | `macro_warp_amp_x1`, `macro_warp_amp_z1`, `macro_warp_amp_x2`, `macro_warp_amp_z2` |
| Shape (3D) domain warp | `shape_warp_amp_x`, `shape_warp_amp_z` |
| Mid relief field | `mid_lattice_spacing`, `mid_frequency`, `mid_amplitude` |
| Small relief field | `small_lattice_spacing`, `small_frequency`, `small_amplitude` |
| Shape strength | `shape_strength_min`, `shape_strength_max` |
| Weirdness mask | `weirdness_scale`, `weirdness_low`, `weirdness_high` |
| Climate fields | `climate_temp_base_scale`, `climate_humidity_base_scale` |
| Climate domain warp | `climate_warp_amp_x1`, `climate_warp_amp_z1`, `climate_warp_amp_x2`, `climate_warp_amp_z2` |
| Biome blending | `climate_blend_radius_nodes` |

## data/recipes.json

Read by `src/core/crafting.cpp` into `RecipeBook`. `grid_size` is the largest
grid the shaped patterns are written for.

| Key | Type | Meaning |
|---|---|---|
| `type` | `"shaped"` or `"shapeless"` | Shaped patterns may be trimmed, mirrored and offset before matching; shapeless is a multiset |
| `pattern` | list of strings | Shaped only: rows, one character per cell, `" "` for empty |
| `key` | object | Shaped only: character → ingredient name. **An entry may be a list**, meaning any of those fills the cell; the loader expands one concrete recipe per combination, so all cells of a symbol are still the same wood |
| `ingredients` | list of strings | Shapeless only: the required items, order-insensitive |
| `result` | object | `block` and `count` |

There are 43 recipes. A key entry that expands to a list multiplies the recipe
count at load, which is intentional: matching stays exact rather than
approximate.

## data/shaders.json

Read by `shader_overlay.gd`, which builds one effect per entry, and by
`settings_menu.gd`, which persists each param. The file declares both the kinds
that exist and what the menu calls each one, so a new kind is a registry entry
rather than a page of menu code.

| Key | Type | Meaning |
|---|---|---|
| `kinds` | list of objects | `kind` (the id used by entries) and `name` (the menu heading). Five today: `screen`, `vertex`, `world`, `filter`, `camera` |
| `shaders` | list of objects | One per effect; registry order is the order the passes are drawn in |

Each effect:

| Key | Type | Meaning |
|---|---|---|
| `id` | string | Used for the settings keys (`<id>_enabled`, `<id>_<param key>`) |
| `name` | string | What the menu calls it |
| `enabled` | bool | Default state |
| `kind` | string | One of `kinds` |
| `shader` | string | Path to the `.gdshader`, for the kinds that draw (`screen`, `world`, `filter`) |
| `materials` / `enable_key` | list / string | Vertex effects: which world materials are switched, and the uniform the switch is pushed to |
| `params` | list of objects | `key` (the shader uniform's name), `label`, `type` (`float`/`bool`), `default`, and for a float `min`, `max`, `step`, optional `suffix` |

A param whose `key` is declared by none of its target's materials is warned
about at load, because it is a slider attached to nothing.

## data/minecraft_blocks.json

The translation table for imported build files. Read by
`src/schematic/mc_palette.cpp` — through `src/schematic/minimal_json.hpp`, not
`godot::JSON`, because the same code runs in `bin/schematic_report` and in the
tests, neither of which has an engine runtime.

| Key | Type | Meaning |
|---|---|---|
| `variant_sets` | object | Named legacy data-value layouts, shared by every material so a shape is written once. `{family}` is substituted with the row's material |
| `species` | object | Which of our four woods stands in for another game's species. A row with `"substitute": true` is a stand-in, recorded on the mapping rather than inferred |
| `names` | object | State name (or a pattern with one `*`) → our block. An exact key wins over a pattern, and of two patterns the first that fits wins, so specific rows must come before general ones |
| `blocks` | list of objects | The numbered table: `id` plus what it becomes |

A row in `blocks`:

| Key | Type | Meaning |
|---|---|---|
| `id` | number | The legacy numeric id |
| `note` | string | Why this row is what it is. Kept, and printed by the report |
| `block` | string / object | The block to place, or `{block, substitute}` when it is only a stand-in |
| `skip` | bool | A deliberate no — a state we would rather leave a gap than guess at |
| `substitute` | bool | The mapping is approximate |
| `variants` | object or string | Data value → block, or the name of a `variant_sets` entry |
| `family` | string | The material substituted into a variant set's `{family}` |
| `fluid` / `still` | bool / string | The row is a liquid, and the block a paste places for it by default (the same substance with no fluid state, so a pasted lake stays put) |

## Where the numbers are

Nothing here is validated against the code by hand — `scons docscheck`
(`tools/check_docs.py`) recounts the block registry, the biome-independent
counts and the test totals, and fails when this page or any other disagrees with
the tree.
