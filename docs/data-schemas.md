# Data file schemas

Everything tunable in Farlands lives in `data/*.json`, and each file has exactly
one reader. This page is the field reference: what each key means and which
translation unit reads it. Where a key's exact type matters, the reader is the
authority and is named in each section.

Two rules apply to all of them:

- **The prose lives on this page, not in the files.** An entry whose reason is not obvious
  gets a bullet under its file below — the block registry's in "The entries whose reason is
  not obvious", the shape registry's in "What each shape is" — so what a reader opens to
  change a number is only the numbers. Two files still carry a `_comment` key
  (`recipes.json`, `minecraft_blocks.json`), because there the note is one line about one
  entry of a long table; every reader ignores the key wherever it appears, as a string or a
  list of strings.
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
| `textures` | 6 strings | Per face, a bare texture name resolved to `res://Assets/textures/blocks/<name>.png`. Empty means that face draws nothing |
| `emissive_textures` | 6 strings | Per face, the glow map, same resolution |
| `light` | 3 numbers | The colour this block emits. All zero means it emits nothing |
| `light_opacity` | number | How much light the block swallows as light passes through it |
| `hardness` | number | Break time divisor; `0` (or absent) is instant |
| `contact_damage` | number | Half-hearts a body overlapping this block takes per tick it stays inside it (cactus). `0` (or absent) is harmless |
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

### The entries whose reason is not obvious

Twenty-four of the 175 entries need a sentence beyond their fields, and this is where
those sentences live — the file itself is only data. Ids are given so a reader can
find the entry in the array without counting by hand.

- **`stone`** (id 1) — Block IDs are positional, so this entry must stay where it is; only
  ever APPEND new blocks.
- **`surface_water`** (id 5) — Generated ocean water, and deliberately NOT a fluid state:
  the flowing simulation is the dynamic water a player pours, not the sea. Left out of the
  state table, the ocean never ticks — it cannot spill runoff along its shores or drain
  through a channel — while still acting as a wall the flow pools against (see blocks_fluid
  in core/block_types.hpp). Adding "fluid": {"kind": "water", "depth": 0} here is what would
  make oceans live.
- **`water`** (id 6) — A source: the block a poured bucket places and the block a pool
  settles into.
- **`oak_wall`** (id 25) — A wall is a post with an arm per side it reaches toward, resolved
  when the chunk is meshed (core/shape_resolver.hpp): a run closes up, a wall leaning on
  stone runs its arm the full height, and the post keeps its top while something stands on
  it. The four per-material orientations all carry this one shape now — which way a wall
  runs is a property of its neighbours, not of the id you placed — and `_full` stays a plain
  cube as the way to thicken a wall into a solid block.
- **`crucible`** (id 117) — Model: the "crucible" entry in data/block_shapes.json — 4 corner
  legs at full block height (1x1px, 2px tall), a 1px floor plate across the whole footprint,
  and 4 1px walls open at the top. The stone texture is a placeholder until the crucible has
  art of its own.
- **`water_runoff_1`** (id 118) — The seven runoff states and the falling state are the
  flowing half of water: entry order is the save-format id, so they are appended and never
  moved. `depth` is how many steps this water is from the source that fed it, and `falling`
  marks a column pouring downward (full strength; its depth is not meaningful). They are
  hidden from the /give list because they are what water BECOMES, not something a player
  places — see src/fluids/ for the rules that pick between them. Each depth has its OWN
  height, which is what makes a stream slope instead of sitting flat: the surface drops by
  (depth+1)/9, so depth 1 is 0.78 tall and depth 7 is 0.11, against the source's 0.88. The
  fallen state is full height, because a column of water is only ever full strength.
- **`water_fallen`** (id 125) — A column of water pouring downward: full strength all the
  way down, which is why this is ONE state rather than eight. Nothing reads a falling cell's
  depth, so there is none to store. Full height (no shape, no offset), which is what a
  waterfall SHOULD look like: a column, not a stack of shallow blocks.
- **`lava`** (id 126) — A source of lava: the block a poured lava bucket places. It flows
  exactly like water does, with three differences that live in src/fluids/fluid_rules.cpp
  rather than here: it stops at three cells from its source instead of seven, it waits a
  second between one cell and the next instead of a quarter of one, and it does not look for
  drops as far away. Shared with water: two side sources over solid ground DO pool into a
  third, so a wide enough pool of lava stops draining. NOT emissive yet — the light array is
  zero, so lava lights nothing; that is a deliberate follow-up rather than an oversight,
  since a liquid that lights the world needs the light engine to treat liquids as sources.
- **`lava_runoff_1`** (id 127) — Lava's flowing half. Only depths 1-3 exist, because three
  is as far as lava's traits let it spread: a depth nothing can reach needs no block to
  store it. The offsets are water's ladder (the surface drops by (depth+1)/9), so a stream
  of lava slopes exactly the way a stream of water does.
- **`acid`** (id 131) — A source of acid: the block a poured acid bucket places. It shares
  water's depth and reach but moves faster (three ticks a cell), and it is the one fluid
  that does NOT pool from a pair of side sources: two buckets do not conjure a third source,
  so a splash of acid spends itself and drains instead of becoming an endless supply.
  Everything else — the ladder of runoff depths, the falling column, the slopes — is
  water's, and the differences live in src/fluids/fluid_rules.cpp.
- **`acid_runoff_1`** (id 132) — Acid's flowing half: seven depths and a falling state,
  exactly the shape water's has, because acid's traits use water's numbers for depth and
  reach.
- **`surface_lava`** (id 140) — Lava a build file brought with it, left STILL: a paste
  places this rather than a lava source, so a pasted lake does not immediately run downhill,
  and `/paste <file> fluids` is what asks for the live one. It is lava to look at and lava
  to flow against (mesh/mesh_fluid.hpp family_of) and it declares NO fluid state, exactly
  like surface_water — that absence is the whole of what makes it still.
- **`surface_acid`** (id 141) — Acid a build file brought with it, left still — see
  surface_lava. Same substance, same shape, no fluid state.
- **`oak_fence`** (id 142) — A fence is a post with arms, not a pole: 4/16 thick where the
  pole is 2/16, and its collision is the post plus a raised bar per arm (1.5 high, the
  pole's precedent) so a body cannot hop it and the gap between rails is not a doorway.
  Until the neighbour resolver lands, `fence/all` draws all four arms -- correct along a run
  and at corners, wrong for a lone post, which is the trade the flat shape makes.
- **`glass`** (id 146) — A transparent FULL cube, like leaves: the terrain pass draws no
  alpha (ALPHA is 1.0 in voxel_shader.gdshader), so this reads as pale glass rather than
  true transparency. Its own texture rather than ice's, which would have been a stand-in
  that can never be told apart from ice itself.
- **`glass_pane`** (id 147) — A sheet that reaches toward another pane or toward anything
  offering a whole face, resolved when the chunk is meshed (core/shape_resolver.hpp). One
  id, no axis and no placement rule: which way the sheet runs is a property of its
  neighbours, so a run, a corner and a lone post are the same block. Drops itself from
  either orientation.
- **`glass_pane_z`** (id 148) — Kept only because block ids are positional. It was the other
  axis of the flat plate, chosen at placement; now that the sheet reads its own neighbours
  there is nothing for a second id to say, so this one carries the same shape and behaves
  identically wherever an old world still holds one.
- **`light_torch`** (id 149) — No `Solid` on purpose: stops_bodies() then answers false and
  the collision resolver skips the cell, which is what a torch does -- you walk through it.
  Named apart from the `torch` ITEM (data/items.json), which is the held light; a block
  called `torch` would have collided with it by name. The 15 is the light level the
  propagator reads. `drops` names the ITEM, so breaking a placed torch hands back the thing
  you can place again (the drop resolves after items load, see
  BlockRegistry::resolve_pending_drops).
- **`ladder`** (id 154) — Decorative: there is no climbing, the same reason the block-file
  importer skips minecraft:ladder. The other three orientations are hidden and drop this
  one.
- **`chain`** (id 158) — A 2/16 column, the shape a hanging chain reads as when it is one
  block. Its links live in the x 7..9 band of the texture, which is the region the geometry
  samples (see tools/make_block_textures.py).
- **`lantern`** (id 159) — Reuses the light block's art and its emissive map rather than
  inventing a second lamp texture: it is the same light in a smaller box.
- **`carpet`** (id 160) — A 1/16 plate. No recipe yet: there is no cloth material to cut it
  from, so this is the shape and the art waiting for one.
- **`oak_plate`** (id 161) — The pressure plate, named our way (the reference calls it
  oak_pressure_plate). Same 1/16 plate as the carpet; it is its own shape so the two can
  diverge.
- **`snow_layer`** (id 162) — Layer 1 of 8. The taller layers exist as blocks 2..8 (hidden)
  so the shape family is complete and reachable with /give; what is missing is the placement
  rule that GROWS an existing layer instead of stacking a second block beside it.

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

### What each shape is

The prose that used to sit inside `block_shapes.json`, one entry at a time, in file
order. A family's note covers its variants unless a variant has one of its own.

- **`stair/n`** — A stair whose step (the raised half) sits against its -Z face. "left" and
  "right" in the stair rules are from the point of view of standing on the stair facing the
  way its step points — here the step points -Z, so left is -X and right is +X. The step is
  neighbour-dependent in three ways. A stair turned ACROSS it, standing in front of the
  step, cuts it back to one of the two `stair_cut_*` remnants, and the half that survives is
  the half that stair is stepping toward, so the two steps run into each other instead of
  into a wall of themselves. A `stair_corner_*` quarter fills the inside of a turn when the
  stair BEHIND the step points at one side. And a stair on THIS one's step — same way up,
  same way round — standing on the side either of those is about switches it off, because
  two flights meeting sideways already own that region: the step between them stays whole
  and the quarter is not drawn. A cell therefore never covers more than three of its four
  upper quadrants, so a stair cannot resolve into a full block. The canonical flattening is
  the slab plus the whole step, so a lone stair — and the icon, the hand model and the
  outline — is the plain step.
- **`stair/n_up`** — The hanging half of the family: the same five rules as stair/n on
  geometry mirrored about the cell floor, so the slab is the raised half and the step hangs.
  `selection_boxes` is the canonical flattening (slab then step), which is what the loader
  derives from the parts and what the icon draws. A hanging stair turns only with another
  hanging one and an upright only with an upright — the two are mirrored, so a quarter or a
  remnant between them would sit in a step-shaped gap that is not there.
- **`wall`** (the family) — A wall is a POST plus a REACH per side it meets, resolved when
  the chunk is meshed (core/shape_resolver.hpp). Three rules, because the family asks three
  different questions. A reach is drawn twice, at two heights, and WHICH height is not a
  property of what is next door: a reach stops 2/16 short of the top (14/16) while the cell
  above it is open, and runs the full height while the cell above spans the strip the reach
  stands on -- a wall under a block is carrying it, and a flank under a span should meet it.
  So a wall beside a whole block and a wall beside another wall draw the same thing, which
  is the point of the rule: what a reach butts against says nothing about how tall it should
  be. What the neighbour decides is whether there is a reach at all: another wall, a sheet a
  wall can bite into, or a neighbour offering a whole face. And the post itself, which is up
  unless this cell is a plain THROUGH-RUN -- a rail studded with posts at every cell stops
  reading as a rail -- so it is the ends of a run, the corners, the T junctions and a wall
  standing on its own that carry it -- and the layouts that do not are the plain run, the
  cross (four reaches already meet in the middle), and a run under a whole block, whose two
  reaches are full height and already meet the span above. A cell in a plain run does carry
  one while something rests on the post's own 2/16 footprint. That last rule is the only one
  in the file that reads the cell AS A WHOLE rather than one face at a time, which is why it
  claims no faces at all. Which of the two heights a reach ends up drawn at is a property of
  the cell above, so ONE shape serves all four directions and the four per-material wall
  blocks that used to differ by orientation now differ only in the id they kept.
- **`wall/all`** — `selection_boxes` and `collision_boxes` are the canonical flattening
  (post plus the two short reaches along X), which the loader derives from the parts and
  warns about if they drift. Collision is 1.5 high, the pole's and the fence's precedent and
  the height this family is measured at: the model is 14/16 at its lowest, already above
  what a jump clears, and 1.5 is what makes a wall one a body cannot hop.
- **`crucible`** — No collision_boxes on purpose: the VISIBLE model is the collision, so a
  body can stand inside the vessel on the inner floor plate (0.1875) instead of on the rim.
  Legs/floor/walls are all solid, so a body outside it is still blocked by the walls.
- **`the file as a whole`** — Everything below the shapes above is flat, non-connecting
  geometry: a fence does not yet know which of its four arms to draw, a pane is a plate
  rather than a post with arms, and a torch is axis-aligned. `fence` is deliberately split
  into `post` and `arm_*` parts so that when the neighbour resolver lands it composes `post`
  with only the arms a run needs, and `all` (the model the blocks use today) disappears. The
  neighbour resolver has since landed for `fence` and `pane`, whose `all` entries carry
  their rules now; the rest of this note still describes the split.
- **`fence/all`** — Post plus the four arms, each arm carrying "rule": "fence". A claimed
  part is present only while the cell face its boxes reach has a neighbour the rule accepts,
  so an isolated fence is the post alone and a run is a continuous rail: two rails per arm,
  not one, because a single bar reads as a rail fence and two read as a fence. The claim is
  resolved when the chunk is meshed (core/shape_resolver.hpp) because nothing in this engine
  updates a cell when its neighbour changes. Collision is part-wise for the same reason: an
  unconnected post is only as wide as itself, while a run is 1.5 high and a body cannot hop
  it (the pole's precedent).
- **`torch_wall`** — The same stick, moved to hug one face of the cell and raised so it
  hangs off the floor. The reference leans a wall torch 22.5 degrees, which this schema
  cannot express yet (a rotated box needs a `rotate` field), so these are axis-aligned on
  purpose.
- **`pane`** (the family) — A 2/16 sheet: a centre post plus one arm per direction it can
  seal against (core/shape_resolver.cpp). The arms stop where the post begins instead of
  overlapping it, so a two-way run reproduces the old single-plane block EXACTLY, in either
  axis — which is what makes one block id enough here where the reference needs a separate
  id per axis and a placement rule to pick between them. A lone pane is the post alone, a
  2/16 column, and the reference draws one the same way. Each arm declares ONE claim face on
  purpose: its box spans the full cell height, so the geometry on its own would read as a
  claim on up and down too, and a sheet is not something that needs a block above and below
  it.
- **`pane/all`** — Post plus the four arms, each carrying "rule": "pane". `selection_boxes`
  is the canonical flattening — post plus the two arms along X, i.e. one flat sheet across
  the cell — which is what the inventory icon and the held model draw; the loader derives it
  from the parts and warns if the two ever disagree.
- **`ladder`** — A 1/16 plate against one face. Decorative: there is no climbing, the same
  reason the block-file importer currently skips ladders.
- **`layer`** (the family) — Stacked heights for snow layers, one entry per layer: layer N
  is N eighths tall. The blocks for 2..8 exist and are hidden; what is missing is the
  placement rule that grows an existing layer instead of stacking a second block on top of
  it.

## data/items.json

`{"items": [...]}`, 33 entries. Read by `src/core/item_registry.cpp`; the
`texture` name resolves under `res://Assets/textures/items/`, because an item is not
part of the block texture array.

| Key | Type | Meaning |
|---|---|---|
| `name` | string | The item's name. Entry order is the item id, so append |
| `texture` | string | Bare name under `Assets/textures/items/` |
| `display` | string | What a player reads instead of the entry's `name` — for an item whose STATE is a second id (the rods' thrown version), so both states read as one item. Read by `item_names.gd`, not the registry |
| `tool` | object | `class`, `tier`, `speed` — what the item is as a tool |
| `pose` | string | A resting pose id the viewmodel knows (`item1`, `item2`, ...). Read by `viewmodel.gd`, not the registry |
| `light` | object | `level` and `color` ([r,g,b]). While the item is selected it *owns* the player's dynamic light |
| `use` | object | `kind` plus its argument: `pour` writes `block` into the aimed cell, `fill` empties the fluid source aimed at and swaps the item for that fluid's bucket, `dummy` puts a combat dummy on the aimed block (`dummy_egg`), `goblin` puts a goblin there instead (`goblin_egg`), `swap` replaces the held item with `item` keeping the count (the rods' cast/retract pair) |
| `place` | object | The one bridge from item space into block space: `block`, plus `wall` → `{n, s, e, w}` for the states that hug a face. Without it an item is never placeable |

The three newest entries are `pebble`, `silver_coin` and `goblin_ear`: a name, a
`texture` and nothing else. They exist to be **dropped** (`data/loot_tables.json`) rather
than used, held for a reason or placed, which is why none of them carries a `use`, a
`tool` or a `place` — an ear in the inventory is just a thing in the inventory.

## data/loot_tables.json

`{"tables": {...}}`, keyed by **table name**: a body names the table it drops
(`scripts/goblin.gd::_drop_loot` asks for `goblin`). Parsed and rolled by
`src/core/loot_table.cpp`, which has no Godot in it so the test binary can pin the dice;
the file read and the name → id resolution are the `LootTables` binding (`load` once at
startup, `roll` per body). Every `item` is a name in `data/items.json`; a name that is not
an item is warned about and that drop skipped, rather than silently leaving nothing.

| Key | Type | Meaning |
|---|---|---|
| `tables.<name>.pools` | list | The table. Each pool is rolled on its own, so two independent drops are two pools |
| `pools[].rolls` | number or `[min, max]` | How many times this pool is rolled; absent means once |
| `pools[].entries` | list | What a roll may pick. One entry is picked per roll |
| `entries[].item` | string | The item to drop, by `items.json` name |
| `entries[].weight` | number | This entry's share of its own pool's picks, default 1. It is a share and not a percentage: one entry in a pool makes it irrelevant |
| `entries[].chance` | number, 0 to 1 | Rolled AFTER the pick, so the entry drops only this often (default 1) |
| `entries[].count` | number or `[min, max]` | How many, default 1. `[0, n]` is a real outcome — the pick happens and gives nothing |

The three steps are separate because they answer three questions — which item
(`weight`), whether at all (`chance`), how many (`count`) — and one percentage cannot say
all three. The pools are separate because `weight` is a share of a pool: weighing a coin
against a pebble would make one of them crowd out the other instead of both dropping.

`goblin` is the only table: `silver_coin` `[0, 2]`, `pebble` `[1, 4]`, and `goblin_ear`
count 1 at `chance` 0.33. Pinned by `tests/test_loot_table.cpp`, which parses the shipped
file and holds every name in it against `data/items.json`.

## data/mobs.json

`{"mobs": {...}}`, keyed by **mob name**: a body names its own entry
(`scripts/goblin.gd::MOB_TYPE` asks for `goblin`). Parsed by
`src/core/mob_attributes.cpp`, which has no Godot in it so the test binary reads an entry out
of a string; the file read and the struct → `Dictionary` translation are the `Mobs` binding
(`load` once at startup, `attributes` per body). `_apply_attributes` in the script fills the
body's own fields from it in `_ready`, and the script's constants of the same names are the
fallback — so a missing file is a goblin with the numbers it was written against, not a body
with none.

| Key | Type | Unit | Meaning |
|---|---|---|---|
| `health` | number | half-hearts | 12 is six hearts; must be above zero |
| `damage` | number | half-hearts | what one of its blows costs |
| `speed` | number | Minecraft's movement value | per-tick acceleration; `× 9.52` is blocks/s, so 0.30 is 2.86 |
| `follow_range` | number | blocks | past this a chase forgets its target |
| `alert_box` | number | blocks, on every axis | how far the shout that wakes a pack carries |
| `anger_window` | `[min, max]` | ticks (20 a second) | how long the anger lasts |
| `step_height` | number | blocks | the ledge it walks up without jumping; 0.6 is a slab, 1.0 a full block |
| `safe_fall_distance` | number | blocks | how far it may drop before a landing hurts |
| `fall_damage_multiplier` | number | a share of one half-heart per block | scales what a landing past that distance costs |
| `armor` | number | defense points, as a chestplate's | the armor stage of the receiving chain, applied to every source; the chain's own 20-point cap applies |
| `knockback_resistance` | number, 0 to 1 | a share | how much of a hit's HORIZONTAL push it shrugs off (1.0 is none of it); the vertical hop is never reduced |

Every field is optional (the struct's default applies), and a file that cannot be a body is
refused with the field named: no health, a negative blow, a zero speed, an `anger_window` that
runs backwards, a negative step height, fall distance, fall multiplier or armor, or a
`knockback_resistance` outside 0 to 1. `goblin` is the only entry — 12, 2, 0.30, 35, 32,
`[400, 799]`, 0.6, 3.0, 1.0, 0 and 0 — pinned by `tests/test_mob_attributes.cpp`, and
`probes/probe_mob_attributes.gd` shows a body spawned in the world running with those numbers,
and with a reloaded file's.

The numbers are the reference mobs' own, which is what `scripts/goblin.gd` carried as
constants: a spider's hunting pace, a zombie's 35-block follow range, a pigman's 32-block
shout on every axis, and twenty to forty seconds of anger.

Where each of the last five lands is why they are worth knowing separately. Step height, both
fall numbers and knockback resistance are the movement sim's own knobs (`PlayerSim`), so
`_apply_attributes` hands them to the `MobBody` it was made with and the number exists once for
the player and the mob alike; their defaults in `MobAttributes` are the reference's numbers for
a plain mob, so an entry that states none of them behaves exactly as it did before they
existed. `armor` is not a sim number: it is a stage of the receiving chain (`apply_armor` in
`src/engine/player_damage.hpp`), run by `MobBody::mitigate`, and `_take_hurt` in the script is
the single door every source of half-hearts goes through — a punch, a landing, a drowned breath,
a cactus — so one armor rule covers all of them rather than the punch having its own.

Two names differ from the reference's attribute list on purpose: its `max_health` and
`movement_speed` are this file's `health` and `speed` (the script's constants and the HUD's
half-hearts were named first), and the rest keep the reference's own names so a reader comparing
the two finds `step_height` where they expect it.

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
| `kinds` | list of objects | `kind` (the id used by entries) and `name` (the menu heading). Six today: `screen`, `vertex`, `texture`, `world`, `filter`, `camera` |
| `shaders` | list of objects | One per effect; registry order is the order the passes are drawn in |

Each effect:

| Key | Type | Meaning |
|---|---|---|
| `id` | string | Used for the settings keys (`<id>_enabled`, `<id>_<param key>`) |
| `name` | string | What the menu calls it |
| `enabled` | bool | Default state |
| `kind` | string | One of `kinds` |
| `shader` | string | Path to the `.gdshader`, for the kinds that draw (`screen`, `world`, `filter`) |
| `materials` / `enable_key` | list / string | Vertex and texture effects: the world materials the effect drives, and the uniform its switch is pushed to (a vertex effect also hands its knobs to the culling code; a texture effect has nothing to hand over) |
| `params` | list of objects | `key` (the shader uniform's name), `label`, `type` (`float`/`bool`), `default`, and for a float `min`, `max`, `step`, optional `suffix` |

A param whose `key` is declared by none of its target's materials is warned
about at load, because it is a slider attached to nothing.

The two kinds that drive materials differ in subject, not in wiring: a **vertex**
effect moves the world's own vertices, while a **texture** effect changes what
those materials *sample* — the block textures themselves, grained or recoloured
for every block at once. Neither gets a layer, because neither is a picture of
the world: it is the world.

## data/changelog.json

Read by `title_screen.gd` for the update-log card at the bottom of the title
screen's right-hand rail. Read-only, once per open; a missing or broken file leaves the card off and
logs an error rather than taking the screen down with it.

| Key | Type | Meaning |
|---|---|---|
| `version` | string | The build these entries belong to. A probe checks it equals `application/config/version`, so the card cannot advertise another release's changelog |
| `entries` | list of strings | One line each, newest first. The card **clips** rather than wraps, so an entry has to fit its width (~46 characters at 6 units); `probe_menu.gd` measures every line against the card |

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
