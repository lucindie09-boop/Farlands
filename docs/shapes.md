# Block shapes

A shape is an arbitrary list of AABBs, so a block model is data rather than code:
slabs, stairs, walls, poles, fences and their arms, the nine-box crucible, and the
flat decorations (carpet, plates, snow layers, torches, buttons, chains, lanterns).

The engineering notes - the decisions, the costs and the traps - are in
[shapes-notes.md](shapes-notes.md).

## The shape set

- **Non-full block shapes**: Slabs, stairs, walls, and poles defined in
  `data/block_shapes.json`. A shape is an arbitrary list of AABBs, so a model is data rather
  than code: the crucible is 9 boxes (4 legs, a floor plate, 4 open-topped walls).
  `collision_boxes` is optional and defaults to the visible boxes
  ([data-schemas.md](data-schemas.md)), so the crucible's walls
  stop a body while its open top lets one stand inside on the inner floor plate; the pole is
  the counter-example that overrides it (1.5-high collision under a 1-block-tall stick). A
  `shape` name that does not resolve leaves a full cube ([data-schemas.md](data-schemas.md)), and
  per-AABB emission emits a box's Bottom face only while that box is RAISED off the cell floor,
  and not even then when another box of the shape covers that footprint from below (a
  stair's upper step over its own lower step). A wall
  and a fence share that 1.5 for the same reason the pole does: the lowest reach of a wall is
  14/16, already above what a jump clears, so 1.5 is what makes one a body cannot hop.
- **Selection and collision boxes**: Each shape variant has explicit selection boxes (for
  raycast) and collision boxes (for physics)
- **Auto-detecting placement**: Slabs, stairs, and walls automatically orient based on clicked
  face and neighboring blocks
- **Double-slab merging**: Two stacked slabs of the same type merge into a double slab; breaking
  drops 2 slabs
- **Pole collision**: Fence-like collision boxes extend 1.5 blocks high for proper player
  interaction
- **The flat shape batch** (fences, glass, panes, torches, ladders, chains, lantern, carpet,
  plate, button, snow layers) is geometry only. `fence/all` is post plus all four arms with a
  1.5-high collision, and each arm is drawn as two rails rather than one bar, because a
  single bar reads as a rail fence and two read as a fence. A 2/16 box samples its own
  region of its texture, since the emitter scales UVs by the box's extent. `oak_plate` is
  the pressure plate under this project's own name (the reference calls it
  `oak_pressure_plate`) and keeps a shape of its own so that it and the carpet can diverge
  even though both are a 1/16 plate; and `glass` wears a texture of its own rather than
  ice's, which would have been a stand-in that nothing could tell apart from ice itself.
  What the batch's ids and flags decide — the placed lights being `light_torch` and its four
  wall variants, the torch carrying no `Solid`, the hidden orientations and what they drop,
  the family variants `/give` withholds, stone's deleted solid wall, the generated art and
  the one item-to-block `place` bridge — is [shapes-notes.md](shapes-notes.md)'s; the fields
  they set are [data-schemas.md](data-schemas.md)'s.

## Neighbour-aware shapes: the resolver

- **Neighbour-aware shapes: the resolver** (`core/shape_resolver.hpp`). A variant may declare
  `parts`, each an unconditional box list or one carrying a rule; a rule-claimed part is present
  only while the cell faces its boxes reach have neighbours the rule accepts. A part may declare
  `faces` where the rule's question is not where its boxes are: the stair rules do, and
  [shapes-notes.md](shapes-notes.md) has that story — the far cell boundary a quarter and a
  remnant reach, and why the loader takes a declared claim on the author's word. `fence/all` is
  the shipped case: a post plus four arms, resolved when the chunk is meshed, so an isolated fence
  is the post alone and a run is continuous. `pane/all` is the declared one: post plus the two
  arms on one axis, which is EXACTLY the flat plate the block was before it grew arms — each arm
  stops where the post begins rather than overlapping it — so a run, a corner and a lone post are
  the same block. A pane reaches another pane, or anything offering a whole face, and that is
  deliberately not the fence's test: a rail is a bar that only needs an end to meet, while a sheet
  glues to the stone, the planks and the glass it is set in, so a pane may sit flush against a
  window where a fence refuses one. `stair/{n,s,e,w}` carries five rules, because a stair is three
  questions. `stair_step` is the raised half in full, drawn while nothing cuts it. A stair
  standing in FRONT of the step that has been turned across it meets this cell along one of the
  step's sides, so it cuts the step back to the half it is stepping toward: that is the remnant
  (`stair_cut_left` / `stair_cut_right`), and the two steps then run into each other instead of
  into a wall of themselves. A stair BEHIND the step pointing at one side leaves the region
  between them open, and the quarter (`stair_corner_left` / `stair_corner_right`) fills the inside
  of that turn. Both are switched off by a stair on THIS one's step — same way up, same way round
  — standing on the side in question, because two flights meeting sideways already own that
  region: the step between them stays whole and the quarter is not drawn. The hanging variants are
  the SAME five rules on geometry mirrored about the cell floor (the four `*_up` shapes repeat the
  upright boxes with `y` flipped), so a stair that hangs turns a corner and has its step cut back
  exactly as a climbing one does — in the lower half — and the two ways up never turn with each
  other. The OUTER corner needed no part that replaces another: because the step is itself a
  claimed part, a cut is the step switching off with a remnant switching on. Those rules are also
  why a stair can never resolve into a full block — the step and a remnant cannot both be drawn,
  and the corner's partner is a single cell, so at most one corner exists and the four upper
  quadrants are never all covered. `wall/all` is the family that shows a piece's HEIGHT is not a
  question about its neighbour. A reach is drawn twice from the same box: at 14/16 while the cell
  above it is open, and at the full cell height while the cell above spans the strip the reach
  stands on — a wall under a block is carrying it, and a flank under a span should meet it. So a
  wall beside a whole block and a wall beside another wall draw the same thing, which is what the
  two rules that draw them (`wall_arm` / `wall_bearing`, one reach split in two) exist to
  guarantee. What the neighbour decides is only WHETHER there is a reach at all: another wall, a
  sheet it can bite into — the mirror of the pane rule's asymmetry, because a sheet presses
  against a whole face and a wall's flank is not one, while a reach is a buttress and a sheet is
  something to buttress against — or a neighbour offering a whole face, glass included, since a
  whole face is a whole face whether or not it is transparent. All four per-material bodies carry
  this one shape, so placement has nothing left to orient: a wall is the block you were holding.
  The post is 8/16 wide and full height, and it is the one part in the file that reads the cell as
  a WHOLE instead of through a face: it stands unless this cell is a plain through-run — a rail
  studded with a post at every cell stops reading as a rail — so a lone wall, the ends of a run, a
  corner and a T junction carry it, while a plain run and a cross do not (four reaches already
  meet in the middle of a cross). A cell in a plain run carries one too while something narrow
  enough rests on the post's own 2/16 footprint, a question about the cell above answered from
  that block's boxes, so a plank, a window or a fence's post does and half a cell of ledge does
  not (and a wall above is inert: a wall two high stays the rail one high is). A run never carries
  one under a whole block, because that block spans the reaches as well and the two of them
  already meet the span. Its rule claims no face at all (`shape_rule_self_decided`), which is why
  an inventory resolution has to draw it: the item model is a post with a run through it. Both of
  the wall's heights are declared rather than derived. Why any of it resolves at mesh time,
  what the wall family costs and how a mined wall drops or thickens, and the consumers of the
  one rule table are [shapes-notes.md](shapes-notes.md)'s; the declared lists, their derivation
  from the parts and the drift warning are [data-schemas.md](data-schemas.md)'s.

## Probed, and what is still open

- **Still open**: a stair is four ids you pick from the menu rather than one that resolves its
  orientation on placement (its corners and cut remnants are data-only on top of that, with no
  new ids), a wall torch cannot be leaned like the reference's because a box has no rotation
  ([data-schemas.md](data-schemas.md)), nothing checks that a torch, a lantern, a ladder or a
  wall button still has the support it hangs from — break the block behind a sconce and the
  sconce stays in the air — and a wall's solid
  `_full` block (where a material still has one — stone's was deleted) has no gesture of its
  own: the placement merge only fires when the cell you are aiming into already holds a wall
  of the family, so there is no deliberate in-game way to thicken one. None of it can be
  solved with variant IDs instead: the registry is already at its budget's edge
  ([data-schemas.md](data-schemas.md)).
  The snow layers are `layer/1` … `layer/8`, one shape entry per eighth with layers 2..8
  hidden, and what is missing there is the placement rule that GROWS an existing layer instead
  of stacking a second block on top of it; the carpet has no recipe either, because there is
  no cloth material to cut it from.
- **Probed by `probes/probe_shapes.gd`** — every new shape's boxes read back by name from the
  real registry and compared against the documented 16ths model, the hidden flags, a texture per
  block, and then the shape file itself for the four things a static box list cannot show: that
  `fence/all` is part-based with the four arms claimed, that its declared box lists still match
  what the parts resolve to (the icon/viewmodel drift check), that a fence's collision is 1.5
  high, and that the wall torch is raised off the floor, which is what makes its underside
  visible. It then places a fence in the live world and reads `get_selection_boxes_at` back
  three times — alone (1 box), against stone (3 boxes with the arm reaching x=1), and after the
  neighbour is removed (1 box again) — which is the only check that sees the JSON, the rule and
  the resolver together. The hanging stairs get their own four: alone (2 boxes, the slab raised
  and the step hanging), with a hanging stair turning in behind the step (3, the quarter in the
  LOWER half), with an upright stair in that same spot (2 — the two ways up do not turn with
  each other), and a hanging stair turned across the step in front (the remnant) verses an
  upright one there (the step whole). It does the same for a pane, three states rather than one:
  alone (the post, which is what proves a declared claim is not asking about the empty cell
  above it), against stone (the sheet flush at x=1), and in a corner with a pane of its own kind
  on the other axis (two sheets meeting at the post), plus that post-plus-two-arms along either
  axis adds up to the flat plate the block replaced — measured on the file's own numbers,
  because it is the geometry that has to add up, not the machinery agreeing with itself. It also
  checks that `stair/n` is part-based with the slab plus exactly one part per rule — the step,
  two remnants and two quarters — that only a rule which supplies its own claim faces has parts
  declaring `faces` (the stair rules, where a declared list would be overruled at load), while a
  declared claim anywhere else has to be a face its own boxes reach — which is what a pane arm's
  one-side declaration is checked against, and that every rule name in the file is one the
  engine knows, since an unresolved name draws its part unconditionally. In the live world it
  places a stair and reads the boxes back four times: alone (2 boxes), with a stair turning in
  BEHIND the step (3 boxes, the quarter present), with a stair turned ACROSS it in front (2
  boxes, the whole step replaced by the half the crossing stair is stepping toward), and with
  that neighbour turned the other way (the other half survives). The wall's live checks are the
  cases the height rule and the conditional post need between them: beside another material's
  wall (the reach at 14/16), beside a whole block (the SAME 14/16 reach, which is the case the
  family used to get wrong, and the one worth re-reading after any change to the rule), carrying
  a block above (the same footprint run to the cell top), a plain run along X (two reaches and
  no post between them), that run under a whole block (both reaches to the top and still no
  post, because the pair already meets the span) and that run with a fence post above (three
  boxes, the post up on the post's own footprint) — beside the always-carried cases (alone, an
  end, a corner). `tests/test_mesh_face_emission.cpp` (with `_shape.cpp` / `_coplanar.cpp`) pins
  the raised-box Bottom rule; `tests/test_shape_resolver.cpp` (with `_walls.cpp` / `_stairs.cpp`
  / `_hanging.cpp` / `_reconcile.cpp`) pins the rules, the claim narrowing, the canonical lists,
  part-wise collision and the load-vs-edit property
