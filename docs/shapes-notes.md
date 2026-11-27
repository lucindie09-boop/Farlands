# Block shape notes

What was decided about the shape system, what each decision cost, and the traps that
were paid for once already. The shapes themselves are in
[shapes.md](shapes.md).

## The shape set

- **Non-full block shapes**: Slabs (bottom/top with auto-detecting placement and double-slab
  merging), stairs (8 orientations with auto-detecting placement), walls (4 orientations), and
  poles (fence-like collision boxes that extend 1.5 blocks high)
- **Shared shape registry**: every non-full shape's boxes live in `data/block_shapes.json`;
  the keys that hold them are [data-schemas.md](data-schemas.md)'s
- **Multi-box models are just shapes**: a shape entry is a LIST of AABBs, so an arbitrary block
  model is data rather than code. The **crucible** is the first model past the 2-box shapes:
  four 1/16 x 2/16 corner legs, a 1/16 floor plate across the whole footprint, and four 1/16
  walls open at the top (9 boxes; its texture is `stone` as a placeholder). A typo in `shape`
  draws a plain block and only a log line says so ([data-schemas.md](data-schemas.md)); read
  `get_selection_boxes(id)` back to prove the model
  landed (`probes/probe_crucible.gd`, and `tests/test_mesh_face_emission.cpp` for the emitted
  geometry), because a typo otherwise looks like a plain block
- **Liquids are passable, and the answer lives in one place**: `BlockType::stops_bodies()` is
  false for anything carrying the `Liquid` property, and that is what the collision resolver,
  the sneak edge-guard and the third-person camera's clear-distance all consult. Before it
  existed the collider called a water cell a solid box (the built-in defaults describe water as
  a full cube, `data/block_shapes.json` as a 14/16 shape) while the pathfinder's classification
  already called the same cell passable — the visible symptoms are a body standing on an ocean
  and a poured bucket becoming a walkable step. `chunk_map.is_block_solid()` is deliberately
  unchanged: "is a block here" is a different question from "does it stop a body"
  (`tests/test_collision_resolver.cpp` pins both)
- **Water movement**: while any part of the body is inside a liquid — two samples, foot and head
  level, so a body standing on a submerged slab counts — the tick swaps gravity for a slow sink
  (1.6 blocks/s), ground/air friction for water drag, scales horizontal acceleration to 1/5,
  adds a rise impulse while jump is held, lifts the body when it swims into a wall, and forgets
  the fall distance (falling into water never hurts). Constants are the `WATER_*` block in
  `src/engine/player_controller.hpp`. The wall lift is what makes a pool escapable: without it a
  bank at the water line is a trap, and swimming alone tops out fractionally above the surface
- **Collision defaults to the model**: `collision_boxes` is optional and falls back to
  `selection_boxes`, so a body collides with what it sees. The crucible relies on that: you can
  stand *inside* it on the inner floor plate (feet at 0.1875, the rim at 1.0), the walls still
  block you from walking in from any side (the full-footprint floor plate crosses the body's
  whole span), and a vanilla jump clears 1.1 blocks so the 13/16 walls are escapable. Declare
  `collision_boxes` only when the visible model is NOT what should stop you — the **pole** is
  the example, with a 1.5-high collision box so bodies cannot hop the 1-block-tall visible stick
  (`tests/test_collision_resolver.cpp` pins the fallback for a hollow shape)
- **Bottom faces: raised boxes yes, floor boxes no.** Per-AABB emission skips a box's
  `FaceDirection::Bottom` only while that box sits on the cell floor (`box.min[1] <= 0`),
  because a resting underside is never visible from a ground-level view and emitting it costs a
  sixth of the pass. A box RAISED off the floor emits it, which is what keeps a wall torch, a
  fence rail or a lantern from being see-through from below — UNLESS a sibling box of the same
  shape spans that footprint up to the box's base (a stair's upper step over its own lower
  step), where the face is inside the model and emitting it is a hidden quad on every stair
  block. `tests/test_mesh_face_emission.cpp` pins all three halves (a raised stick emits exactly
  one bottom quad, the same stick standing on the floor emits none, a step over a full-footprint
  sibling emits none while a step over a partial one keeps its quad, and the rest of the box is
  still drawn so the checks cannot pass on an empty mesh)
- **The fence / torch / pane batch, and what in it is deliberately flat.** `oak_fence` and its
  three siblings (`fence/all` — post plus four claimed arms, 1.5-high collision so a body cannot
  hop it and the gap between rails is not a doorway; the arms are the first neighbour-aware
  shape, see the resolver bullet above), `glass` (a `Transparent` full cube) and `glass_pane`
  (one connected id, resolved at mesh time — see the resolver bullet; the old hidden z variant
  is kept only because ids are positional and now carries the same shape), `light_torch` plus
  four hidden `light_torch_wall_{n,e,s,w}`, `ladder` (+ 3 hidden orientations, decorative —
  there is no climbing, which is why the block-file importer skips ladders), `chain`, `lantern`
  (reuses the light block's art and emissive map), `carpet`, `oak_plate`, `snow_layer` (+ hidden
  layers 2..8) and `stone_button` (+ 4 hidden wall variants). Three naming/behaviour decisions
  worth not re-litigating: the placed blocks are **`light_torch` / `light_torch_wall_*`, not
  `torch`**, because a `torch` ITEM already exists in `data/items.json` as the held light and a
  second thing by that name would be ambiguous; the torch carries **no `Solid` property**, so
  `stops_bodies()` is false and the collision resolver walks through it without any new flag;
  and every derived orientation is `hidden` and declares `drops` naming what a player can
  actually hold (the `drops` field is the whole mechanism — `mining.hpp` already consults it): a
  sconce or a wall button yields its visible block, while the torch yields the **ITEM**, not the
  block, so breaking a placed torch hands back the thing you can place again. A `drops` may name
  an item, which is why it cannot all be resolved at block load:
  `BlockRegistry::resolve_pending_drops` finishes item-named drops once `items.json` has loaded
  (blocks load first), and an unknown name there is logged. Separately from the flag,
  `BlockRegistry::is_family_variant` treats a shape family's non-base members as hidden too — a
  stair's seven other orientations and a slab's top/full — so `/give` and its autocomplete offer
  only the id a player places, never the orientations placement derives; walls are deliberately
  excluded from that rule because a wall's `_full` is a real solid block a merge or a paste
  writes, not an orientation. Stone's `_full` is gone entirely: it was a plain stone cube (no
  such block exists in the reference), so stone now substitutes to `stone` in the MC converter
  and its family simply has no solid to thicken into. No alpha: the terrain pass draws
  `ALPHA = 1.0`, so glass reads as pale glass rather than true transparency, and art must not
  rely on an authored alpha channel. `chunk_generations`-style knobs aside, the ID budget is
  worth watching when adding states: this took the registry from 143 to 175, and the cap it
  spends is [data-schemas.md](data-schemas.md)'s.

## The art is generated from the same data

- **The art for those shapes is generated, not hand-drawn, because the layout is not free**: a
  box samples its own texture region, so the torch stick must sit in x 7..9 / y 6..16 and the
  chain's links in x 7..9 of their textures. The torch BLOCK texture is the exception: it is a
  verbatim copy of `Assets/textures/items/torch.png`, because the torch you hold and the torch you
  place must be one drawing and the item art is what the player sees in hand.
  `python tools/make_block_textures.py` copies that file and generates `torch_emit.png` (aligned
  to the item art's two flame rows) plus glass, ladder, chain and carpet, idempotently; new PNGs
  need Godot to import them before the game can load them (`--headless --path . --import`, and
  an open editor makes that pass fail on the extension copy while still writing the `.import`
  files)

## Neighbour-aware shapes at mesh time

- **Resolved at MESH time is a deliberate architecture choice** (`core/shape_resolver.hpp`), and
  what each family is and what its rules draw is [shapes.md](shapes.md)'s — this page keeps the
  reasons and what they cost. A cell here has no state beyond its block id, so the reference's
  variant-per-state scheme (fence_straight, fence_corner_nw, stair_inner_right, ...) would need a
  neighbour-update pass this engine does not have — and that pass would have to run again after
  worldgen, after a paste and after a load, or the shapes would be wrong exactly where nobody
  looks
- **Over-claiming is invisible and over-narrowing is not**, which is why the rule's own faces win
  where a part's declared `faces` disagree with its boxes: a corner quarter and a cut remnant
  reach the far cell boundary as well as the neighbour's, so a claim read off the geometry alone
  would say "east and back" and a stair standing behind would fill the east notch, a corner with
  nothing attached to that side. A declared claim is still checked against the part's own boxes,
  and the loader warns where the two differ
- **The pane is what a DECLARED claim exists for**: an arm spans the whole cell height, so its
  boxes reach up and down as well, and a claim read off them would demand a block above and below
  — so each arm declares the single side it points at, the only case the loader takes on the
  author's word. The stair sits on the other side of that split, and the wall on a third: a
  reach's boxes meet the floor, so a derived claim would demand something underneath a wall built
  out over a drop, which is why both of the wall's heights are declared
- **A family's shape is one id, and the cost is the reason**: orientation is part of the rules and
  material is not, so any stair turns with any other, and the stair's five rules cost **zero new
  block ids** where a variant-per-state scheme needs 128. The pane's post plus arms needs no second
  id: one id stands in for a variant per axis, where the reference needs a second id and a
  placement rule to choose it. The wall's five ids per
  material are positional-id history rather than state — ONE family to the registry, which is what
  a mined wall drops and what a wall thickens into (`WallFamily::base`/`full`), matched by shape so
  the retired `wall/{n,s,e,w}` names stay understood instead of silently losing the drop
- **The hanging stair is the same rules mirrored, and an upright stair never turns with one**:
  the region a corner fills only exists between two stairs laid out the same way round, and
  `BlockType::stair_hanging` is what makes that askable, because a rule can only read the block it
  is looking at. The probe compares the four `*_up` shapes rule for rule and box for box, which is
  the guard against the two drifting apart
- **The post is the one part that reads the cell as a WHOLE**, which is why the walk has a second
  kind of rule (`shape_rule_self_decided`) for the single part a per-face conjunction cannot
  express, and why a worldless (inventory) resolution draws it. The two rules that draw a wall's
  reach (`wall_arm` / `wall_bearing`) are one reach split in two, so a side that reaches can
  never draw both or neither; the family's old bug was a wall beside another wall drawing
  differently from a wall beside a block, which is the first thing to re-check after any change
  to the rule
- **The consequence is the property that replaces an update pass**: a cell's geometry is a pure
  function of the cell and its six neighbours, which `tests/test_shape_resolver.cpp` pins by
  meshing the same two fences once at load and once after an edit and comparing the vertex buffers
  byte for byte
- **One rule table, five consumers.** `shape_rule_connects` (which neighbours satisfy a rule,
  taking the caller's own neighbour lookups rather than one id because a stair rule asks about
  cells other than the face it was called on) and `shape_rule_canonical` (what a resolution with
  no world assumes connected) live in `core/shape_resolver.cpp` alone, and the same walk serves
  the mesher (its own `ChunkNeighborAccessor`), collision and the raycast (the chunk map under
  the lock they already hold — collision pads its key set by a block, the raycast grows its lock
  box by one so a +-1 neighbour is inside it), the outline (one locked lookup per face, and it
  rebuilds only when the resolved set actually changes, because an arm can appear without the
  block id changing), and icons, which have no world and take the canonical set. Pathfinding is
  the deliberate exception: `block_class.hpp` classifies by id only and has no position, and
  counting every fence as a full-height obstacle is the conservative answer for a planner
- **The declared lists are the worldless reader's model**: how they are derived from the parts,
  why a drift is warned about and which readers need them are all
  [data-schemas.md](data-schemas.md)'s. What matters here is which entries ARE that model and
  how their geometry behaves. `fence/all` is the shipped case: post plus four claimed arms, so
  an isolated fence is a lone post and a run is continuous, and collision is part-wise for the
  same reason (a run is a 1.5-high wall, an isolated post is only as wide as itself).
  `pane/all` is the second: post plus four arms claimed one side each, so a lone pane is a
  2/16 column you walk around and a run is a sheet you do not — and its collision list is
  deliberately left EMPTY in the JSON, because a pane's collision IS its sheets and the two
  lists therefore cannot drift apart

## Probed

- **Probed by `probes/probe_shapes.gd`**, whose case list is
  [shapes.md](shapes.md)'s and [probes.md](probes.md)'s. Three things about running it are worth
  knowing: the fence has to be cleared to air before the stair goes in, because `set_block`
  refuses to overwrite one non-air block with another unless it is a documented merge, so a stale
  neighbour silently swallows the write; cells are cleared and put back rather than searched for,
  because a headless spawn can sit inside solid terrain and an isolated post needs all four
  lateral neighbours empty. What it is worth is the overlap: it is the one place a shape's JSON,
  its rules and the resolver are compared in a single run, so a shape that resolves but draws the
  wrong boxes cannot pass unnoticed

## Earlier work: placement, collision and AO/UV

- **Auto-detecting placement**: Slabs and stairs orient from the face clicked and the player's
  direction. A wall no longer has anything to orient: its four bodies carry the connected shape,
  so which way it runs comes from its neighbours and the placement is the block you were
  holding. What is left of the wall's placement logic is the merge — a wall aimed into a cell
  that already holds a wall of the same family becomes the family's solid block, where the
  family has one (stone does not, so aiming a stone wall into stone just refuses) — and that is
  the only path that produces one, since `_full` is hidden from the inventory. The torch is the
  one item that places a block: `items.json` gives the `torch` ITEM a `place` object naming
  `light_torch` plus its four wall variants, and `place_block` resolves the clicked face into
  the right one (a top face the standing torch, a vertical face the variant that hugs that wall)
  and consumes the ITEM. That is the only declared bridge from the item space into the block
  space — without it an item is never placeable, which is what keeps tools out of voxels. A wall
  button still has none (reachable only by `/give`), and a snow layer does not grow into layer 2
  when you place a second one. The resolver makes the rest cheap to add later: a stair id that
  resolves its orientation on placement. The pane needed neither in the end — with its arms
  claimed off the neighbours it has no axis to choose, so there is one id to place and nothing
  to decide. `/give` follows the same rule: its list and its refusal both key off
  `BlockTextures.is_hidden`, which now also reports a family's non-base variants, so
  `stone_stairs_s` can no longer be handed out while `stone_stairs` can (and a variant a player
  somehow holds still breaks down to its base on mining). The stair's corners and cut remnants
  have since landed (see the resolver bullet), also with no new ids
- **Proper collision and raycast**: Multi-box shapes (stairs, slabs, walls, poles) have accurate
  collision detection and raycast selection
- **Fixed AO and UV mapping**: Ambient occlusion and texture mapping properly handle partial
  blocks with their irregular geometry
