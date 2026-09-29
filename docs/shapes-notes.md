# Block shape notes

What was decided about the shape system, what each decision cost, and the traps that
were paid for once already. The shapes themselves are in
[shapes.md](shapes.md).

## The shape set

- **Non-full block shapes**: Slabs (bottom/top with auto-detecting placement and double-slab
  merging), stairs (8 orientations with auto-detecting placement), walls (4 orientations), and
  poles (fence-like collision boxes that extend 1.5 blocks high)
- **Shared shape registry**: `data/block_shapes.json` defines selection boxes and collision
  boxes for all non-full block shapes
- **Multi-box models are just shapes**: a shape entry is a LIST of AABBs, so an arbitrary block
  model is data rather than code. The **crucible** is the first model past the 2-box shapes:
  four 1/16 x 2/16 corner legs, a 1/16 floor plate across the whole footprint, and four 1/16
  walls open at the top (9 boxes; its texture is `stone` as a placeholder). A `shape` name that
  fails to resolve only logs an error and leaves the block a FULL CUBE, so a typo looks like a
  plain block rather than failing — read `get_selection_boxes(id)` back to prove the model
  landed (`.freebuff/probe_crucible.gd`, and `tests/test_mesh_face_emission.cpp` for the emitted
  geometry)
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
  worth watching when adding states: this took the registry from 143 to **175 of
  `MAX_BLOCK_TYPES` = 256**

## The art is generated from the same data

- **The art for those shapes is generated, not hand-drawn, because the layout is not free**: a
  box samples its own texture region, so the torch stick must sit in x 7..9 / y 6..16 and the
  chain's links in x 7..9 of their textures. The torch BLOCK texture is the exception: it is a
  verbatim copy of `textures/items/torch.png`, because the torch you hold and the torch you
  place must be one drawing and the item art is what the player sees in hand.
  `python tools/make_block_textures.py` copies that file and generates `torch_emit.png` (aligned
  to the item art's two flame rows) plus glass, ladder, chain and carpet, idempotently; new PNGs
  need Godot to import them before the game can load them (`--headless --path . --import`, and
  an open editor makes that pass fail on the extension copy while still writing the `.import`
  files)

## Neighbour-aware shapes at mesh time

- **Neighbour-aware shapes are resolved at MESH TIME, and that is a deliberate architecture
  choice** (`core/shape_resolver.hpp`). A cell here has no state beyond its block id, so the
  reference's variant-per-state scheme (fence_straight, fence_corner_nw, stair_inner_right, ...)
  would need a neighbour-update pass that does not exist in this engine — and that pass would
  have to run again after worldgen, after a paste and after a load, or the shapes would be wrong
  exactly where nobody looks. Instead a shape variant may declare `parts`: each part is a box
  list, optionally carrying a rule, and a rule-claimed part is present only while the cell faces
  its boxes reach have neighbours the rule accepts. The claim FOLLOWS THE GEOMETRY
  (`shape_box_faces` reads it off the boxes), so a claim cannot be written against a face the
  part does not actually meet — UNLESS the RULE supplies the faces, which the stair rules do
  because what they ask about is not where their boxes are: a corner quarter and a cut remnant
  reach the far cell boundary as well as the neighbour's, so a derived claim would read "east
  and back" and a stair standing behind would fill the east notch, a corner with nothing
  attached to that side. Over-narrowing is visible and over-claiming is not, so the rule wins
  where the two disagree; a part may still declare `faces` for its own documentation, and the
  loader warns when they differ (it still refuses a geometry-derived claim on a face the part
  does not reach). The stair carries five rules instead, because a stair is three questions.
  `stair_step` is the raised half in full, drawn while nothing cuts it. A stair standing in
  FRONT of the step that has been turned across it meets this cell along one of the step's
  sides, so it cuts the step back to the half it is stepping toward: that is the remnant
  (`stair_cut_left` / `stair_cut_right`), and the two steps then run into each other instead of
  into a wall of themselves. A stair BEHIND the step that points at one side leaves the region
  between them open, and the quarter (`stair_corner_left` / `stair_corner_right`) fills the
  inside of that turn. Both are switched off by a stair on THIS one's step (same way up, same
  way round) standing on the side in question — two flights meeting sideways already own that
  region, so the step between them stays whole and the quarter is not drawn. Orientation is
  therefore part of the rules and material is not, so any stair turns with any other, and all of
  it costs zero new block ids where variant-per-state needs 128. The four hanging `*_up`
  variants are those SAME five rules on geometry mirrored about the cell floor — same rules,
  same order, every box with `y` flipped, which the probe compares rule for rule and box for box
  — so a stair that hangs turns a corner and is cut back exactly as a climbing one is, in the
  lower half. The two ways up never turn with each other: the region a corner fills only exists
  between two stairs laid out the same way round. `BlockType::stair_hanging` is what makes that
  askable, because a rule can only read the block it is looking at. The OUTER corner needed no
  part that replaces another after all: because the step is itself a claimed part, a cut is the
  step switching off with a remnant switching on. Those rules are also what makes a full-block
  stair impossible: the step and a remnant cannot both be drawn, and the corner's partner is a
  single cell, so at most one corner exists and the four upper quadrants are never all covered.
  The third family is the pane, and it is the one that shows what a DECLARED claim is for: an
  arm spans the whole cell height, so its boxes reach up and down as well, and a claim read off
  them would demand a block above and below — so each arm declares the single side it points at,
  which is the only case the loader takes on the author's word (a declared claim must still be a
  face its boxes reach). A pane reaches another pane, or anything offering a whole face, which
  is deliberately not the fence's test: a rail needs an end to meet, while a sheet glues to the
  stone and the glass it is set in, so a pane sits flush against a window where a fence refuses
  one. Post plus the two arms on one axis is exactly the flat plate the block was before it grew
  arms, so one id stands in for a variant per axis — a run, a corner and a lone post are the
  same block — where the reference needs a second id and a placement rule to choose it. The
  fourth family is the wall, and it is the one that shows a piece's HEIGHT is not a question
  about its neighbour. A reach is drawn twice from the same box: at 14/16 while the cell above
  it is open, and at the full cell height while the cell above spans the strip the reach stands
  on — a wall under a block is carrying it, and a flank under a span should meet it. So a wall
  beside a whole block and a wall beside another wall draw the same thing, which is exactly what
  the family used to get wrong, and the two rules that draw them (`wall_arm` / `wall_bearing`)
  are one reach split in two, so a side that reaches can never draw both or neither. What the
  neighbour decides is only WHETHER there is a reach: another wall, a sheet it can bite into
  (the mirror of the pane rule's asymmetry — a sheet presses against a whole face and a wall's
  flank is not one, while a reach is a buttress and a sheet is something to buttress against),
  or a neighbour offering a whole face, glass included. All four per-material bodies carry this
  one shape, so placement has nothing left to orient — a wall is the block you were holding. The
  post is 8/16 wide and full height, and it is the one part in the file that reads the cell as a
  WHOLE instead of through a face: it stands unless this cell is a plain through-run, because a
  rail studded with a post at every cell stops reading as a rail. A lone wall, the ends of a
  run, a corner and a T junction carry it, while a plain run and a cross do not (four reaches
  already meet in the middle of a cross). A cell in a plain run carries one too while something
  narrow enough rests on the post's own 2/16 footprint — a question about the cell above,
  answered from that block's boxes, so a plank, a window or a fence's post does and half a cell
  of ledge does not — and a run carries NONE under a whole block, because that block spans the
  reaches as well and the two of them already meet the span. It claims no face at all, which is
  why the walk has a second kind of rule (`shape_rule_self_decided`) for the one part a per-face
  conjunction cannot express, and why a worldless (inventory) resolution draws it: the item
  model is a post with a run through it. The family is still five ids per material because ids
  are positional, but they are ONE family to the registry, and that family is what a mined wall
  drops and what a wall thickens into (`WallFamily::base`/`full`); the match is by shape, so the
  retired `wall/{n,s,e,w}` names stay understood instead of silently losing the drop. Both
  heights are DECLARED claims, which puts this family on the other side of the box/rule split
  from the stairs: a reach's boxes meet the floor, so a claim read off them would demand
  something underneath a wall built out over a drop. Consequence: a cell's geometry is a pure
  function of the cell and its six neighbours, which is the property
  `tests/test_shape_resolver.cpp` pins by meshing the same two fences once at load and once
  after an edit and comparing the vertex buffers byte for byte — that test is what replaces an
  update pass
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
- **`selection_boxes`/`collision_boxes` are DERIVED from the parts at load**, and the loader
  warns when the declared list disagrees. It matters because two readers see a shape: the world
  resolves the parts, while `block_icon_renderer.gd` and `viewmodel.gd` read
  `data/block_shapes.json` directly and have no world to resolve against. If the two ever drift,
  the item in your hand is a different model from the one you place. `fence/all` is the shipped
  case: post plus four claimed arms, so an isolated fence is a lone post and a run is
  continuous, and collision is part-wise for the same reason (a run is a 1.5-high wall, an
  isolated post is only as wide as itself). `pane/all` is the second: post plus four arms
  claimed one side each, so a lone pane is a 2/16 column you walk around and a run is a sheet
  you do not — and its collision list is deliberately left EMPTY in the JSON, because a pane's
  collision IS its sheets and the two lists therefore cannot drift apart

## Probed

- **Probed by `.freebuff/probe_shapes.gd`**, which reads every new shape's boxes back from the
  real registry and compares them to the documented 16ths model, checks the hidden flags,
  asserts a texture loaded per block, and then reads `data/block_shapes.json` itself for the
  things a static box list cannot show: that `fence/all` and `stair/n` are part-based with their
  arms, corners and remnants claimed, that only a rule which supplies its own claim faces has
  parts declaring `faces` (the stair rules, where a declared list that disagreed is overruled at
  load) while a declared claim anywhere else must be a face its own boxes reach, which is what a
  pane arm's one-side declaration is checked against, that every rule name in the file is one
  the engine knows (an unresolved name draws its part unconditionally rather than failing
  loudly), that both families' declared box lists still match what their parts resolve to (the
  icon/viewmodel drift check), that a fence's collision is 1.5 high, that `wall/all` is nine
  parts — one post plus a reach on all four sides at BOTH heights, the two being the SAME box
  with only its top moved — with every reach declaring exactly one face and the post declaring
  none, and that the wall torch is raised off the floor, which is what makes its underside
  visible. It then places a fence, a pane and a stair in the live world and reads
  `get_selection_boxes_at` back — fence alone 1 box, against stone 3 boxes with the arm reaching
  x=1, then 1 again once the neighbour goes; pane alone 1 box (the post, which is what shows a
  declared claim is not quietly asking about the empty cell above it), against stone 2 boxes
  with the sheet flush at x=1, and in a corner with a pane of its own kind on the other axis 3
  boxes; stair alone 2 boxes, 3 with a stair turning in BEHIND the step (the quarter present), 2
  with a stair turned ACROSS it in front (the whole step replaced by the half the crossing stair
  is stepping toward), and the other half once that neighbour is turned the other way. A hanging
  stair gets the same treatment: alone (2 boxes, slab raised and step hanging), 3 with a hanging
  stair turning in BEHIND the step (the quarter in the LOWER half), 2 with an upright one in
  that spot instead (the two ways up do not turn with each other), 2 with a hanging stair turned
  across in front (the remnant) and the hanging step left whole when it is an upright one there.
  The wall gets the same treatment: alone 1 box (the post, 8/16 wide and full height), 2 with
  another material's wall beside it (the arm at 14/16), 2 with stone beside it (the SAME 14/16
  reach, which is the case the family used to get wrong and the first thing to re-check after
  any change to the rule), 2 carrying a block above (that same footprint run to the cell top),
  then a plain run along X (2 boxes, the two reaches and NO post between them), that run under a
  whole block (2, both reaches to the top and still no post — the pair already meets the span)
  and that run with a fence post above (3, the post up on its own footprint) — the cases that
  show a reach's height comes from the cell above and the post is conditional rather than always
  drawn. It is the only check that sees the JSON, the rules and the resolver together. Note the
  fence has to be cleared to air before the stair goes in: `set_block` refuses to overwrite one
  non-air block with another unless it is a documented merge, so a stale neighbour silently
  swallows the write. Cells are cleared and put back rather than searched for: a headless spawn
  can sit inside solid terrain, and an isolated post needs all four lateral neighbours empty. A
  shape name that fails to resolve silently leaves a FULL CUBE, so this probe is the difference
  between "the model landed" and "it looks like a plain block"

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
