extends SceneTree
## Headless check for the fence / torch / pane / ladder / chain / lantern /
## carpet / plate / button / snow-layer batch.
##
## A shape name that does not resolve only logs an error and leaves the block a
## FULL CUBE, so a typo looks like a plain solid block rather than failing. That
## failure mode is why this reads the delivered geometry back by name from the
## real registry (the same boxes the mesh builder, the outline and the icon
## renderer use) instead of trusting the JSON.
##
## Three things are checked per block:
##   * it resolves by name, and a hidden orientation variant is marked hidden,
##   * its selection boxes match the 16ths model the shape file documents, and
##   * it is not a stand-in full cube.
##
## Run: Godot --headless --path <project> --script res://probes/probe_shapes.gd

# name -> expected selection boxes, in block-local units. A single box of
# [0,0,0,1,1,1] would mean the shape did not resolve.
const EXPECTED := {
	# The CANONICAL flattening (post plus the two X arms), not the union of all
	# four arms: a fence's arms are resolved against the world at mesh time, so
	# what the registry holds for a worldless consumer is the fence run. The
	# arms themselves are checked in the world further down.
	"oak_fence": [
		[0.375, 0, 0.375, 0.625, 1, 0.625],
		[0.4375, 0.75, 0.4375, 1, 0.9375, 0.5625],
		[0.4375, 0.375, 0.4375, 1, 0.5625, 0.5625],
		[0, 0.75, 0.4375, 0.5625, 0.9375, 0.5625],
		[0, 0.375, 0.4375, 0.5625, 0.5625, 0.5625],
	],
	"light_torch": [[0.4375, 0, 0.4375, 0.5625, 0.625, 0.5625]],
	"light_torch_wall_n": [[0.4375, 0.21875, 0, 0.5625, 0.84375, 0.125]],
	"light_torch_wall_e": [[0.875, 0.21875, 0.4375, 1, 0.84375, 0.5625]],
	# The pane is the third neighbour-aware family, and the first whose claim is a
	# DECLARED face rather than one read off the boxes: an arm spans the whole cell
	# height, so its geometry alone would claim up and down too, and what a pane has
	# above it has nothing to do with whether it reaches sideways. Canonical is the
	# post plus the two arms along X  -  one flat sheet across the cell, which is
	# exactly the plate this block was before it grew arms. Both ids carry it: the
	# second was the other axis of the flat plate, and an axis is no longer something
	# a block gets to decide.
	"glass_pane": [
		[0.4375, 0, 0.4375, 0.5625, 1, 0.5625],
		[0.5625, 0, 0.4375, 1, 1, 0.5625],
		[0, 0, 0.4375, 0.4375, 1, 0.5625],
	],
	"glass_pane_z": [
		[0.4375, 0, 0.4375, 0.5625, 1, 0.5625],
		[0.5625, 0, 0.4375, 1, 1, 0.5625],
		[0, 0, 0.4375, 0.4375, 1, 0.5625],
	],
	"ladder": [[0, 0, 0, 1, 1, 0.0625]],
	"ladder_s": [[0, 0, 0.9375, 1, 1, 1]],
	"chain": [[0.4375, 0, 0.4375, 0.5625, 1, 0.5625]],
	"lantern": [[0.3125, 0.375, 0.3125, 0.6875, 0.75, 0.6875]],
	"carpet": [[0, 0, 0, 1, 0.0625, 1]],
	"oak_plate": [[0, 0, 0, 1, 0.0625, 1]],
	"snow_layer": [[0, 0, 0, 1, 0.125, 1]],
	"snow_layer_8": [[0, 0, 0, 1, 1, 1]],
	"stone_button": [[0.3125, 0, 0.3125, 0.6875, 0.125, 0.6875]],
	"stone_button_n": [[0.3125, 0.375, 0, 0.6875, 0.625, 0.125]],
	# The stair is the second neighbour-aware shape: slab plus step, with the two
	# corner quarters ABSENT from the canonical list because a corner means "a
	# stair is beside me", which a worldless consumer cannot know. Pinned here so
	# the corner parts cannot quietly leak into the hand model and the outline.
	"oak_stairs": [[0, 0, 0, 1, 0.5, 1], [0, 0.5, 0, 1, 1, 0.5]],
	# ...and a hanging stair is those same two boxes mirrored about the floor, which
	# is what the canonical resolution of its (mirrored) parts has to produce.
	"oak_stairs_n_up": [[0, 0.5, 0, 1, 1, 1], [0, 0, 0, 1, 0.5, 0.5]],
	# The wall is the fourth neighbour-aware family and the only one whose hands are
	# ONE reach drawn twice: the same box at 14/16 with open sky over the cell, and at
	# the full height while the cell above spans the strip the reach stands on. Canonical
	# is the post plus the two SHORT reaches along X  -  a column with a run through it
	#  -  which is a wall held in the hand, and also the first half of the story the
	# live-world checks below finish.
	"oak_wall": [
		[0.25, 0, 0.25, 0.75, 1, 0.75],
		[0.5, 0, 0.3125, 1, 0.875, 0.6875],
		[0, 0, 0.3125, 0.5, 0.875, 0.6875],
	],
	# ...and every per-material body carries that same canonical run, not a panel of
	# its own: that is the whole point of the shape resolving from neighbours.
	"stone_wall": [
		[0.25, 0, 0.25, 0.75, 1, 0.75],
		[0.5, 0, 0.3125, 1, 0.875, 0.6875],
		[0, 0, 0.3125, 0.5, 0.875, 0.6875],
	],
	# A full cube reports the whole cell: that is the default the registry hands
	# back for a block with no shape at all, so it is worth pinning -- and this is one
	# of them, the block a wall thickens into.
	"oak_wall_full": [[0, 0, 0, 1, 1, 1]],
	"glass": [[0, 0, 0, 1, 1, 1]],
}

# Blocks that must NOT show up in the inventory, because a placement rule or an
# orientation rule is what creates them.
const HIDDEN := [
	"glass_pane_z", "light_torch_wall_n", "light_torch_wall_s", "light_torch_wall_e",
	"light_torch_wall_w", "ladder_s", "ladder_e", "ladder_w",
	# The four per-material wall bodies all carry the connected shape now, so the
	# three that are not the inventory variant are placement-only ids kept for the
	# positional save format, and a material's solid block (where it has one) is what
	# a merge or a pasted build writes rather than something you pick up. Stone's
	# three were NOT hidden before the connected shape and are now, because three ids
	# that draw the same connected wall are three identical inventory entries. Stone
	# has no solid wall at all any more -- it was just a stone cube, not a thing the
	# reference has -- so the MC pack converter substitutes foreign walls with `stone`.
	"oak_wall_s", "oak_wall_e", "oak_wall_w", "oak_wall_full",
	"stone_wall_s", "stone_wall_e", "stone_wall_w",
	"snow_layer_2", "snow_layer_3", "snow_layer_4", "snow_layer_5",
	"snow_layer_6", "snow_layer_7", "snow_layer_8",
	"stone_button_n", "stone_button_s", "stone_button_e", "stone_button_w",
]

# Blocks that exist as their own texture and must load one.
const TEXTURED := [
	"oak_fence", "pine_fence", "spruce_fence", "holly_fence",
	"glass", "glass_pane", "light_torch", "ladder", "chain", "lantern",
	"carpet", "oak_plate", "snow_layer", "stone_button",
]

var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return

	for name in EXPECTED.keys():
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			_fail("%s did not resolve by name" % name)
			continue
		if BlockTextures.is_item(id):
			_fail("%s resolved to an ITEM, not a block" % name)
			continue
		var expected: Array = EXPECTED[name]
		var boxes: Array = cm.get_selection_boxes(id)
		if boxes.size() != expected.size():
			_fail("%s has %d selection boxes, expected %d — a shape name that fails to resolve falls back to a single full cube"
				% [name, boxes.size(), expected.size()])
			continue
		var bad := 0
		for i in range(boxes.size()):
			var got := PackedFloat32Array(boxes[i])
			if got.size() != 6:
				bad += 1
				continue
			for f in range(6):
				if absf(got[f] - float(expected[i][f])) > 0.0001:
					bad += 1
					break
		if bad > 0:
			_fail("%s: %d of %d boxes do not match the documented model" % [name, bad, boxes.size()])
		else:
			print("probe: %-20s id=%-4d %d boxes" % [name, id, boxes.size()])

	for name in HIDDEN:
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			_fail("%s did not resolve by name" % name)
		elif not BlockTextures.is_hidden(id):
			_fail("%s is reachable in the inventory but is meant to be a derived state" % name)

	for name in TEXTURED:
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			continue
		var tex: Texture2D = BlockTextures.get_texture(id)
		if tex == null:
			_fail("%s resolved but no texture loaded" % name)

	# The torch is the one block here that must NOT stop a body, and the fence is
	# the one that must (its collision is the post plus a raised bar per arm).
	_check_shape_file(cm)
	await _check_neighbour_shapes(cm, player)

	if ok:
		print("PROBE PASS")
	quit(0 if ok else 1)

## The checks that have to write blocks: a fence's arms and a stair's inner corner
## are resolved against the world when the chunk is meshed, so only a live
## neighbour can prove the parts, the rule and the resolver agree. The world is
## snapshotted and restored around this probe, and every cell is put back below.
func _check_neighbour_shapes(cm: Node, player: Node3D) -> void:
	var fence_id := BlockTextures.get_block_id_by_name("oak_fence")
	var stone_id := BlockTextures.get_block_id_by_name("stone")
	# `oak_stairs` is `stair/n`, so its step (the raised half) is at its -Z face and
	# the cell at -Z is the one IN FRONT of the step. The two turns that matter are
	# the hidden orientations: `oak_stairs_e` (stepping toward +X) and `oak_stairs_w`
	# (stepping toward -X), which are the two halves a step can be cut back to.
	var stair_id := BlockTextures.get_block_id_by_name("oak_stairs")
	var stair_e_id := BlockTextures.get_block_id_by_name("oak_stairs_e")
	var stair_w_id := BlockTextures.get_block_id_by_name("oak_stairs_w")
	if fence_id <= 0 or stone_id <= 0 or stair_id <= 0 or stair_e_id <= 0 or stair_w_id <= 0:
		_fail("oak_fence / stone / oak_stairs(_e/_w) did not resolve, so the neighbour checks cannot run")
		return

	# Writes to a chunk with no render data are queued rather than applied, so this
	# waits for the ground the player is standing on before looking for a cell —
	# the same reason the crucible probe waits for is_on_floor().
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break

	# The cells are cleared rather than searched for: a headless spawn can sit
	# INSIDE terrain (this world is solid stone to y=252 at the origin), so natural
	# air is not guaranteed — and an isolated post needs all four lateral
	# neighbours empty or it would be armed by the mountain around it. The world is
	# snapshotted and restored around this probe, and every cell is put back below.
	var p := player.global_position
	var cell := Vector3i(int(floor(p.x)), int(floor(p.y)) + 1, int(floor(p.z)))
	var east := cell + Vector3i(1, 0, 0)
	# The cell above is cleared too, because a wall's post is drawn partly from what
	# stands on it: a headless spawn can sit under terrain, and a wall with a mountain
	# resting on its top would put the post up in the "alone" check.
	var above := cell + Vector3i(0, 1, 0)
	var empties := [cell, east, cell + Vector3i(-1, 0, 0), cell + Vector3i(0, 0, -1),
		cell + Vector3i(0, 0, 1), above]
	var restore: Array = []
	for c in empties:
		restore.append(cm.get_block(c.x, c.y, c.z))
		if not await _await_write(cm, c, 0):
			_fail("could not clear %s for the fence arm check" % c)
			return

	if not await _await_write(cm, cell, fence_id):
		_fail("the fence write at %s never landed" % cell)
		return

	var isolated: Array = cm.get_selection_boxes_at(fence_id, cell.x, cell.y, cell.z)
	if isolated.size() != 1:
		_fail("an isolated fence post resolves to %d boxes, expected 1 — the arms are not being claimed away" % isolated.size())
	else:
		print("probe: fence alone -> 1 box")

	var east_existing: int = 0
	if not await _await_write(cm, east, stone_id):
		_fail("the neighbour write at %s never landed" % east)
		return

	var connected: Array = cm.get_selection_boxes_at(fence_id, cell.x, cell.y, cell.z)
	if connected.size() != 3:
		_fail("a fence against stone resolves to %d boxes, expected 3 (post + one arm of two rails)" % connected.size())
	else:
		var reaches := false
		for b in connected:
			if float(b[3]) >= 1.0:
				reaches = true
		if not reaches:
			_fail("the claimed arm does not reach the cell boundary it was claimed on")
		else:
			print("probe: fence against stone -> 3 boxes, arm reaches x=1")

	# ...and it goes away again. Geometry here is a function of the world, not
	# something an edit leaves behind for the next remesh to trip over.
	if not await _await_write(cm, east, east_existing):
		_fail("the neighbour could not be put back at %s" % east)
		return
	var after: Array = cm.get_selection_boxes_at(fence_id, cell.x, cell.y, cell.z)
	if after.size() != 1:
		_fail("the arm stayed after its neighbour was removed (%d boxes)" % after.size())

	# The pane, on the same cleared cells. Its arms are claimed on a DECLARED face,
	# so this is also what proves the declaration reaches the world: a lone sheet is
	# the post alone even though its boxes run the full cell height, which they would
	# not be if the claim had been read off the geometry with the block above and
	# below empty.
	var pane_id := BlockTextures.get_block_id_by_name("glass_pane")
	if pane_id <= 0:
		_fail("glass_pane did not resolve, so the pane checks cannot run")
		return
	if not await _await_write(cm, cell, 0):
		_fail("the fence could not be cleared from %s before the pane check" % cell)
		return
	if not await _await_write(cm, cell, pane_id):
		_fail("the pane write at %s never landed" % cell)
		return
	var lone_pane: Array = cm.get_selection_boxes_at(pane_id, cell.x, cell.y, cell.z)
	if lone_pane.size() != 1:
		_fail("a lone pane resolves to %d boxes, expected the post alone" % lone_pane.size())
	elif not _box_is(lone_pane[0], 0.4375, 0, 0.4375, 0.5625, 1, 0.5625):
		_fail("a lone pane is not the 2/16 post: %s" % str(lone_pane[0]))
	else:
		print("probe: pane alone -> 1 box (the post)")

	# A stone neighbour gives the sheet a whole face to press against.
	if not await _await_write(cm, east, 0) or not await _await_write(cm, east, stone_id):
		_fail("the stone neighbour write at %s never landed" % east)
		return
	var reached: Array = cm.get_selection_boxes_at(pane_id, cell.x, cell.y, cell.z)
	if reached.size() != 2:
		_fail("a pane against stone resolves to %d boxes, expected the post plus one sheet" % reached.size())
	elif not _box_is(reached[1], 0.5625, 0, 0.4375, 1, 1, 0.5625):
		_fail("the sheet is not flush against the stone: %s" % str(reached[1]))
	else:
		print("probe: pane against stone -> 2 boxes, sheet reaches x=1")

	# ...and a pane reaches a pane, which is what makes a run and a corner the same
	# block rather than two more variants. -Z is the north arm.
	if not await _await_write(cm, east, 0) or not await _await_write(cm, east, pane_id):
		_fail("the pane neighbour write at %s never landed" % east)
		return
	var north := cell + Vector3i(0, 0, -1)
	if not await _await_write(cm, north, pane_id):
		_fail("the pane corner write at %s never landed" % north)
		return
	var corner_pane: Array = cm.get_selection_boxes_at(pane_id, cell.x, cell.y, cell.z)
	if corner_pane.size() != 3:
		_fail("a pane in a corner resolves to %d boxes, expected the post plus two sheets" % corner_pane.size())
	elif not _box_is(_hull(corner_pane), 0.4375, 0, 0, 1, 1, 0.5625):
		_fail("the corner sheets do not add up to an L from the post: %s" % str(_hull(corner_pane)))
	else:
		print("probe: pane corner -> 3 boxes, two sheets meeting at the post")

	# Put these cells back to air, because set_block refuses to lay one non-air
	# block over another: the stair check below writes into -Z too.
	for c in [east, north]:
		if not await _await_write(cm, c, 0):
			_fail("the pane check could not clear %s again" % c)
			return

	# The stair's rules, on the same cleared cells (nothing but air around them, so
	# no leftover arm from the fence check can flatter this). Whatever block is in
	# the cell has to go first: set_block refuses to overwrite a non-air block with
	# another non-air block unless it is a documented merge (slab+slab into the full
	# block), so a stale fence or pane would silently swallow the stair write.
	if not await _await_write(cm, cell, 0):
		_fail("the block could not be cleared from %s before the stair check" % cell)
		return
	if not await _await_write(cm, cell, stair_id):
		_fail("the stair write at %s never landed" % cell)
		return
	var solo: Array = cm.get_selection_boxes_at(stair_id, cell.x, cell.y, cell.z)
	if solo.size() != 2:
		_fail("a lone stair resolves to %d boxes, expected the slab and its step" % solo.size())
	else:
		print("probe: stair alone -> 2 boxes")

	# -Z is in FRONT of this stair's step and +Z is behind it. A stair turning in
	# BEHIND the step points back into the cell along one of its sides, and the gap
	# the two of them leave between them is what the quarter fills  -  here on the
	# +X side, because `oak_stairs_e` points at +X.
	var front := cell + Vector3i(0, 0, -1)
	var behind := cell + Vector3i(0, 0, 1)
	if not await _await_write(cm, behind, stair_e_id):
		_fail("the stair neighbour write at %s never landed" % behind)
		return
	var corner: Array = cm.get_selection_boxes_at(stair_id, cell.x, cell.y, cell.z)
	if corner.size() != 3:
		_fail("a stair with one turning in behind the step resolves to %d boxes, expected 3 (step + the quarter)" % corner.size())
	else:
		var fills := false
		for b in corner:
			if absf(float(b[0]) - 0.5) < 0.0001 and absf(float(b[1]) - 0.5) < 0.0001 					and absf(float(b[2]) - 0.5) < 0.0001 and absf(float(b[3]) - 1.0) < 0.0001:
				fills = true
		if not fills:
			_fail("the third box is not the quarter behind the step on the +X side (0.5,0.5,0.5)-(1,1,1)")
		else:
			print("probe: stair with one turning in behind the step -> 3 boxes, quarter present")

	# The cell in FRONT of the step behaves the other way round: a stair there that
	# has turned ACROSS the step cuts it back to the half it is stepping toward, so
	# the whole step is gone and a remnant stands in its place.
	if not await _await_write(cm, behind, 0):
		_fail("the turn behind the step could not be cleared at %s" % behind)
		return
	if not await _await_write(cm, front, stair_w_id):
		_fail("the crossing stair write at %s never landed" % front)
		return
	var cut: Array = cm.get_selection_boxes_at(stair_id, cell.x, cell.y, cell.z)
	if cut.size() != 2:
		_fail("a stair turned across in front of the step resolves to %d boxes, expected 2 (slab + the remnant)" % cut.size())
	else:
		var remnant := false
		var whole_step := false
		for b in cut:
			if absf(float(b[0]) - 0.0) < 0.0001 and absf(float(b[1]) - 0.5) < 0.0001 					and absf(float(b[3]) - 0.5) < 0.0001 and absf(float(b[4]) - 1.0) < 0.0001:
				remnant = true
			if absf(float(b[1]) - 0.5) < 0.0001 and absf(float(b[3]) - 1.0) < 0.0001 					and absf(float(b[5]) - 0.5) < 0.0001:
				whole_step = true
		if not remnant or whole_step:
			_fail("the cut step is not the half the crossing stair is stepping toward")
		else:
			print("probe: stair turned across in front -> 2 boxes, step cut to one half")

	# ...and turning the crossing stair the other way keeps the OTHER half, so what
	# survives really follows the stair in front rather than the hole it left. It has
	# to go to air first for the same reason as above: one non-air id over another is
	# only allowed for a documented merge.
	if not await _await_write(cm, front, 0):
		_fail("the crossing stair could not be cleared at %s" % front)
		return
	if not await _await_write(cm, front, stair_e_id):
		_fail("the crossing stair could not be turned the other way at %s" % front)
		return
	var cut_other: Array = cm.get_selection_boxes_at(stair_id, cell.x, cell.y, cell.z)
	var other_half := false
	for b in cut_other:
		if absf(float(b[0]) - 0.5) < 0.0001 and absf(float(b[2]) - 0.0) < 0.0001 				and absf(float(b[3]) - 1.0) < 0.0001 and absf(float(b[5]) - 0.5) < 0.0001:
			other_half = true
	if cut_other.size() != 2 or not other_half:
		_fail("turning the crossing stair the other way did not keep the other half of the step")
	else:
		print("probe: the half that survives follows the stair in front")

	# The hanging stairs, in the same cleared cells, and the two things that make them
	# work: they turn with each other in the LOWER half, and a stair of the other kind
	# is inert to them in either role.
	var down_id := BlockTextures.get_block_id_by_name("oak_stairs_n_up")
	var down_e_id := BlockTextures.get_block_id_by_name("oak_stairs_e_up")
	if down_id <= 0 or down_e_id <= 0:
		_fail("oak_stairs_n_up / oak_stairs_e_up did not resolve, so the hanging checks cannot run")
		return
	for c2 in [cell, front, behind]:
		if not await _await_write(cm, c2, 0):
			_fail("could not clear %s for the hanging stair check" % c2)
			return
	if not await _await_write(cm, cell, down_id):
		_fail("the hanging stair write at %s never landed" % cell)
		return
	var hanging_alone: Array = cm.get_selection_boxes_at(down_id, cell.x, cell.y, cell.z)
	if hanging_alone.size() != 2 or not _box_is(hanging_alone[0], 0, 0.5, 0, 1, 1, 1) \
			or not _box_is(hanging_alone[1], 0, 0, 0, 1, 0.5, 0.5):
		_fail("a hanging stair is not the mirrored slab and step: %s" % str(hanging_alone))
	else:
		print("probe: hanging stair alone -> 2 boxes, slab raised and step hanging")

	# +Z is behind this stair's step, and the turning stair points at +X: the quarter
	# fills the region behind the step in the LOWER half.
	if not await _await_write(cm, behind, down_e_id):
		_fail("the hanging turn write at %s never landed" % behind)
		return
	var hanging_corner: Array = cm.get_selection_boxes_at(down_id, cell.x, cell.y, cell.z)
	if hanging_corner.size() != 3:
		_fail("a hanging stair with one turning in behind the step resolves to %d boxes, expected 3" % hanging_corner.size())
	else:
		var lower_quarter := false
		for b in hanging_corner:
			if _box_is(b, 0.5, 0.0, 0.5, 1.0, 0.5, 1.0):
				lower_quarter = true
		if not lower_quarter:
			_fail("the hanging quarter is not in the lower half behind the step: %s" % str(hanging_corner))
		else:
			print("probe: hanging stair corner -> 3 boxes, quarter in the lower half")

	# A stair of the other kind behind the step fills no corner at all, even though it
	# points at the same side: the two ways up are mirrored, so the region the corner
	# would fill is not open between them.
	if not await _await_write(cm, behind, 0):
		_fail("the hanging turn could not be cleared at %s" % behind)
		return
	if not await _await_write(cm, behind, stair_e_id):
		_fail("the upright turn write at %s never landed" % behind)
		return
	var hanging_vs_upright: Array = cm.get_selection_boxes_at(down_id, cell.x, cell.y, cell.z)
	if hanging_vs_upright.size() != 2:
		_fail("an upright stair turned a hanging stair's corner (%d boxes, expected 2)" % hanging_vs_upright.size())
	else:
		print("probe: the two ways up never turn with each other")

	# In FRONT of the hanging step, a hanging stair turned across it cuts it back  -  and
	# the same upright stair in that spot does not, which is the guard the other way
	# round.
	if not await _await_write(cm, behind, 0):
		_fail("the upright turn could not be cleared at %s" % behind)
		return
	if not await _await_write(cm, front, down_e_id):
		_fail("the hanging crossing write at %s never landed" % front)
		return
	var hanging_cut: Array = cm.get_selection_boxes_at(down_id, cell.x, cell.y, cell.z)
	var hanging_remnant := false
	var hanging_step_whole := false
	for b in hanging_cut:
		if _box_is(b, 0.5, 0.0, 0.0, 1.0, 0.5, 0.5):
			hanging_remnant = true
		if _box_is(b, 0.0, 0.0, 0.0, 1.0, 0.5, 0.5):
			hanging_step_whole = true
	if hanging_cut.size() != 2 or not hanging_remnant or hanging_step_whole:
		_fail("a hanging stair turned across in front did not cut the hanging step back: %s" % str(hanging_cut))
	else:
		print("probe: hanging stair turned across in front -> 2 boxes, step cut to one half")

	if not await _await_write(cm, front, 0):
		_fail("the hanging crossing stair could not be cleared at %s" % front)
		return
	if not await _await_write(cm, front, stair_e_id):
		_fail("the upright crossing write at %s never landed" % front)
		return
	var hanging_uncut: Array = cm.get_selection_boxes_at(down_id, cell.x, cell.y, cell.z)
	var still_whole := false
	for b in hanging_uncut:
		if _box_is(b, 0.0, 0.0, 0.0, 1.0, 0.5, 0.5):
			still_whole = true
	if hanging_uncut.size() != 2 or not still_whole:
		_fail("an upright stair turned across a hanging step cut it back (%d boxes)" % hanging_uncut.size())
	else:
		print("probe: an upright stair leaves a hanging step whole")

	# The wall, in the same cleared cells. A reach is one piece drawn at two heights AND
	# a post that is not simply always there, so what is checked is which height the world
	# picks and when the column stands up: a wall on its own, a wall beside another
	# material's wall, a wall beside a whole block (the SAME height, which is the case this
	# family used to get wrong), a wall carrying a block above it, and then a plain
	# through-run with and without something on it.
	var wall_id := BlockTextures.get_block_id_by_name("oak_wall")
	var pine_wall_id := BlockTextures.get_block_id_by_name("pine_wall")
	if wall_id <= 0 or pine_wall_id <= 0:
		_fail("oak_wall / pine_wall did not resolve, so the wall checks cannot run")
		return
	var west := cell + Vector3i(-1, 0, 0)
	for c3 in [cell, east, west, above]:
		if not await _await_write(cm, c3, 0):
			_fail("could not clear %s for the wall check" % c3)
			return
	if not await _await_write(cm, cell, wall_id):
		_fail("the wall write at %s never landed" % cell)
		return
	var wall_alone: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	if wall_alone.size() != 1 or not _box_is(wall_alone[0], 0.25, 0, 0.25, 0.75, 1, 0.75):
		_fail("a lone wall is not its post, 8/16 wide and full height: %s" % str(wall_alone))
	else:
		print("probe: wall alone -> 1 box, the post full height")

	# Another material is still a wall, and a bridging hand stops short of the top.
	if not await _await_write(cm, east, pine_wall_id):
		_fail("the wall neighbour write at %s never landed" % east)
		return
	var wall_run: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var run_arm := false
	for b in wall_run:
		if absf(float(b[3]) - 1.0) < 0.0001 and absf(float(b[4]) - 0.875) < 0.0001:
			run_arm = true
	if wall_run.size() != 2 or not run_arm:
		_fail("a wall beside another wall is not post + a 14/16 arm: %s" % str(wall_run))
	else:
		print("probe: wall beside another wall -> 2 boxes, the arm short of the top")

	# A whole face next door draws the SAME reach at the same height, because the height
	# is not a question about the neighbour at all. (The wall has to be cleared first:
	# set_block refuses non-air over non-air unless the change is a documented merge, so
	# stone over the pine wall is refused.)
	if not await _await_write(cm, east, 0):
		_fail("the wall neighbour could not be cleared at %s" % east)
		return
	if not await _await_write(cm, east, stone_id):
		_fail("the whole face neighbour write at %s never landed" % east)
		return
	var wall_beside_stone: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var stone_reach_short := false
	for b in wall_beside_stone:
		if absf(float(b[3]) - 1.0) < 0.0001 and absf(float(b[4]) - 0.875) < 0.0001:
			stone_reach_short = true
	if wall_beside_stone.size() != 2 or not stone_reach_short:
		_fail("a wall beside a whole block is not post + the same 14/16 reach: %s"
			% str(wall_beside_stone))
	else:
		print("probe: wall beside stone -> 2 boxes, the reach the same height a run draws")

	# ...and the cell ABOVE is what makes a reach full height: with a block resting on the
	# cell, the same footprint runs to the cell top instead. The post stays up here, because
	# a wall reached on one side only is the end of a run and carries a column either way.
	if not await _await_write(cm, above, stone_id):
		_fail("the carried block write at %s never landed" % above)
		return
	var wall_carrying: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var reach_to_top := false
	for b in wall_carrying:
		if absf(float(b[3]) - 1.0) < 0.0001 and absf(float(b[4]) - 1.0) < 0.0001:
			reach_to_top = true
	if wall_carrying.size() != 2 or not reach_to_top:
		_fail("a wall carrying a block above it is not post + a full-height reach: %s"
			% str(wall_carrying))
	else:
		print("probe: wall carrying a block above -> 2 boxes, the reach run to the top")
	if not await _await_write(cm, above, 0):
		_fail("the carried block could not be cleared at %s" % above)
		return

	# A plain through-run along X is the layout with NO post: the two arms are all
	# there is, and the rail is 6/16 wide for its whole height instead of growing a
	# column at every cell. (The whole-face neighbour has to go first: `set_block`
	# refuses non-air over non-air unless the change is a documented merge.)
	if not await _await_write(cm, east, 0):
		_fail("the whole face neighbour could not be cleared at %s" % east)
		return
	if not await _await_write(cm, east, wall_id):
		_fail("the run neighbour write at %s never landed" % east)
		return
	if not await _await_write(cm, west, wall_id):
		_fail("the run neighbour write at %s never landed" % west)
		return
	var wall_run_plain: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var run_has_post := false
	for b in wall_run_plain:
		if _box_is(b, 0.25, 0, 0.25, 0.75, 1, 0.75):
			run_has_post = true
	if wall_run_plain.size() != 2 or run_has_post:
		_fail("a plain wall run is not two arms with no post: %s" % str(wall_run_plain))
	else:
		print("probe: a plain wall run -> 2 boxes, the arms with no post between them")

	# ...and the two ways the cell above can answer are opposites. A whole block spans the
	# reaches, so a run under one draws both of them at the full height and keeps no post:
	# the column between them is already there.
	if not await _await_write(cm, above, stone_id):
		_fail("the span write at %s never landed" % above)
		return
	var wall_run_under_block: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var run_under_has_post := false
	var run_under_all_tall := true
	for b in wall_run_under_block:
		if _box_is(b, 0.25, 0, 0.25, 0.75, 1, 0.75):
			run_under_has_post = true
		if absf(float(b[4]) - 1.0) > 0.0001:
			run_under_all_tall = false
	if wall_run_under_block.size() != 2 or run_under_has_post or not run_under_all_tall:
		_fail("a wall run under a whole block is not two reaches run to the top: %s"
			% str(wall_run_under_block))
	else:
		print("probe: a wall run under a whole block -> 2 boxes, both reaches to the top")
	if not await _await_write(cm, above, 0):
		_fail("the span above could not be cleared at %s" % above)
		return

	# ...while something narrow enough to rest on the post's own 2/16 footprint brings the
	# column up to meet it instead of running the reaches to the top.
	if not await _await_write(cm, above, fence_id):
		_fail("the fence post above write at %s never landed" % above)
		return
	var wall_run_resting: Array = cm.get_selection_boxes_at(wall_id, cell.x, cell.y, cell.z)
	var resting_has_post := false
	for b in wall_run_resting:
		if _box_is(b, 0.25, 0, 0.25, 0.75, 1, 0.75):
			resting_has_post = true
	if wall_run_resting.size() != 3 or not resting_has_post:
		_fail("a wall run with a fence post above did not put its post up: %s"
			% str(wall_run_resting))
	else:
		print("probe: ...with a fence post above -> 3 boxes, the post up to meet it")

	# Put the world back exactly as it was found.
	for i in range(empties.size()):
		await _await_write(cm, empties[i], int(restore[i]))

## Which cell faces a box list reaches, read the way core/shape_resolver.cpp reads
## it: a face counts only when a box touches that boundary more or less exactly
## (kFaceTouchEpsilon, 1e-4), never when it merely comes near it  -  a fence rail
## 15/16 tall reaches the top of its cell in every sense except the one that
## matters. Only used to check a DECLARED claim face, because that is the one case
## the loader takes on the author's word.
func _faces_reached(boxes: Array) -> Array:
	var out := []
	var e := 0.0001
	for b in boxes:
		if float(b[1]) <= e and not out.has("down"): out.append("down")
		if float(b[4]) >= 1.0 - e and not out.has("up"): out.append("up")
		if float(b[0]) <= e and not out.has("w"): out.append("w")
		if float(b[3]) >= 1.0 - e and not out.has("e"): out.append("e")
		if float(b[2]) <= e and not out.has("n"): out.append("n")
		if float(b[5]) >= 1.0 - e and not out.has("s"): out.append("s")
	return out

func _hull(boxes: Array) -> Array:
	var out := [INF, INF, INF, -INF, -INF, -INF]
	for b in boxes:
		for k in range(3):
			out[k] = minf(out[k], float(b[k]))
			out[k + 3] = maxf(out[k + 3], float(b[k + 3]))
	return out

func _box_is(b: Array, x0: float, y0: float, z0: float, x1: float, y1: float, z1: float) -> bool:
	var want := [x0, y0, z0, x1, y1, z1]
	for k in range(6):
		if absf(float(b[k]) - want[k]) > 0.0001:
			return false
	return true

## Sets a cell and waits until the write is actually readable, because BlockEditor
## queues writes while the target chunk has no render data.
func _await_write(cm: Node, cell: Vector3i, id: int) -> bool:
	cm.set_block(cell.x, cell.y, cell.z, id)
	for attempt in range(600):
		await process_frame
		if cm.get_block(cell.x, cell.y, cell.z) == id:
			return true
	return false

## The shape file is the source of truth for collision too, and neither the
## torch's missing collision nor the fence's raised override is visible through
## get_selection_boxes (which returns the MODEL). Read the file.
func _check_shape_file(cm: Node) -> void:
	var f := FileAccess.open("res://data/block_shapes.json", FileAccess.READ)
	if f == null:
		_fail("data/block_shapes.json is not readable")
		return
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	if typeof(data) != TYPE_DICTIONARY:
		_fail("data/block_shapes.json did not parse")
		return
	var shapes: Dictionary = data

	var fence: Dictionary = shapes.get("fence", {}).get("all", {})
	if not fence.has("parts"):
		_fail("fence/all has no \"parts\": its arms are not neighbour-dependent, so every fence would arm in all four directions")
	else:
		var claimed := 0
		for part in fence["parts"]:
			if part.get("rule", "") == "fence":
				claimed += 1
		if claimed != 4:
			_fail("fence/all marks %d parts with the fence rule, expected the four arms" % claimed)

	# The collision is part-wise too: the canonical set is the post tower plus the
	# two X arms, all 1.5 high, which is what makes a RUN a wall while a lone post
	# is only as wide as itself.
	var collision: Array = fence.get("collision_boxes", [])
	if collision.size() != 3:
		_fail("fence/all declares %d canonical collision boxes, expected 3 (post + the two X arms)" % collision.size())
	else:
		for b in collision:
			if absf(float(b[4]) - 1.5) > 0.0001:
				_fail("a fence collision box is %s high, expected 1.5 so a body cannot hop it" % b[4])

	# The pane: one arm per direction, four in all, each declaring exactly the face
	# it points at, and post plus the two arms on one axis adding up to the flat
	# plate this block used to be. That last part is what lets ONE id stand in for a
	# variant per axis, so it is checked on the file's own numbers rather than
	# assumed from the machinery agreeing with itself.
	var pane: Dictionary = shapes.get("pane", {}).get("all", {})
	if not pane.has("parts"):
		_fail("pane/all has no \"parts\": every pane would be a sheet in one fixed direction")
	else:
		var claimed := 0
		var arms := {}
		for part in pane["parts"]:
			if String(part.get("rule", "")) != "pane":
				continue
			claimed += 1
			var declared: Array = part.get("faces", [])
			if declared.size() != 1:
				_fail("a pane arm declares %d claim faces, expected exactly the one it points at" % declared.size())
				continue
			arms[String(declared[0])] = part["boxes"][0]
		if claimed != 4:
			_fail("pane/all marks %d parts with the pane rule, expected one per direction" % claimed)
		if arms.size() != 4:
			_fail("the pane arms claim %d distinct directions, expected four" % arms.size())
		else:
			var post_box: Array = pane["parts"][0]["boxes"][0]
			if not _box_is(_hull([arms["n"], post_box, arms["s"]]), 0.4375, 0, 0, 0.5625, 1, 1):
				_fail("post plus the two arms along Z is not the 2/16 plate the pane replaced")
			elif not _box_is(_hull([arms["w"], post_box, arms["e"]]), 0, 0, 0.4375, 1, 1, 0.5625):
				_fail("post plus the two arms along X is not the 2/16 plate the pane replaced")
			else:
				print("probe: a two-way pane run is exactly the flat plate it replaced")
		var canonical: Array = pane.get("selection_boxes", [])
		if canonical.size() != 3:
			_fail("pane/all declares %d canonical boxes, expected the post and the two arms along X" % canonical.size())

	# The file is what the inventory icon and the viewmodel read, while the world
	# resolves the PARTS. If the two ever disagree, the item in the hand would be a
	# different model from the one placed, so they are compared here  -  for every
	# neighbour-aware family, because the drift is per family.
	for pair in [["fence", "all", "oak_fence"], ["stair", "n", "oak_stairs"],
			["pane", "all", "glass_pane"]]:
		var box: Dictionary = shapes.get(String(pair[0]), {}).get(String(pair[1]), {})
		var block_id := BlockTextures.get_block_id_by_name(String(pair[2]))
		if block_id <= 0:
			_fail("%s did not resolve, so its shape file drift cannot be checked" % pair[2])
			continue
		var declared: Array = box.get("selection_boxes", [])
		var live: Array = cm.get_selection_boxes(block_id)
		if declared.size() != live.size():
			_fail("%s/%s declares %d selection boxes but the parts resolve to %d"
				% [pair[0], pair[1], declared.size(), live.size()])
		else:
			for i in range(declared.size()):
				var got := PackedFloat32Array(live[i])
				for k in range(6):
					if absf(float(declared[i][k]) - got[k]) > 0.0001:
						_fail("%s/%s box %d differs between the file and the resolved parts" % [pair[0], pair[1], i])
						break

	# The stair is the family whose claims cannot be derived from the geometry: a
	# corner quarter and a cut remnant reach the FAR cell boundary as well as the
	# neighbour's (z: 0.5..1 on a stair whose step is at the other end), so a derived
	# claim would read "east and back" and a stair standing BEHIND this one would fill
	# the east notch  -  a corner with nothing attached to that side. The five stair
	# rules supply their own claim faces, and no part may declare them: a declared list
	# that disagreed with its rule would be overruled at load, so this is where the
	# file and the rules stay in step.
	var known_rules := ["fence", "pane", "stair_step", "stair_cut_left", "stair_cut_right",
		"stair_corner_left", "stair_corner_right", "wall_arm", "wall_bearing", "wall_post"]
	# ...and the rules that supply their own claim faces, which are the only ones a
	# part may not declare. A rule that reads its claim off the geometry may narrow
	# it (a pane arm reaching up and down but claiming only the side it points at),
	# but one that asks about the step face and a guard side cannot have a part
	# answering for it.
	var rule_supplies_faces := ["stair_step", "stair_cut_left", "stair_cut_right",
		"stair_corner_left", "stair_corner_right"]
	for family in shapes.keys():
		# Untyped on purpose: this dictionary carries a `_comment` string beside the
		# families, and a typed local throws on the assignment before the check below
		# can reject it.
		var variants = shapes.get(family)
		if typeof(variants) != TYPE_DICTIONARY or String(family).begins_with("_"):
			continue
		for variant in variants.keys():
			var boxes = variants.get(variant)
			if typeof(boxes) != TYPE_DICTIONARY:
				continue
			for part in boxes.get("parts", []):
				var rule := String(part.get("rule", ""))
				if rule != "" and not known_rules.has(rule):
					_fail("shape %s/%s carries the unknown rule \"%s\"  -  an unresolved name leaves the part unconditional" % [family, variant, rule])
				if rule != "" and part.has("faces") and rule_supplies_faces.has(rule):
					_fail("shape %s/%s declares claim faces on a %s part; the rule supplies those" % [family, variant, rule])
				if part.has("faces"):
					# A declared claim on a face the part does not even reach is dead
					# geometry: the resolver would never draw the part.
					var reached := _faces_reached(part.get("boxes", []))
					for face_name in part["faces"]:
						if not reached.has(String(face_name)):
							_fail("shape %s/%s claims %s, which its own boxes do not reach (reaches %s)"
								% [family, variant, face_name, str(reached)])

	# The wall is the family that mixes the two kinds of claim: ONE part answered
	# without a neighbour's help at all (the post, which reads the four sides at once
	# and so claims no face), and eight reaches each declaring the single side it points
	# at (a reach's boxes also meet the floor, so a derived claim would ask for
	# something underneath a wall built out over a drop). Both heights must sit on all
	# four sides, or a wall would draw the tall reach on one side and the short one on
	# another whenever the cell above changed.
	var wall_parts: Array = shapes.get("wall", {}).get("all", {}).get("parts", [])
	if wall_parts.size() != 9:
		_fail("wall/all has %d parts, expected 9 (post, four arms, four bearing reaches)" % wall_parts.size())
	var posts := 0
	for part in wall_parts:
		if String(part.get("rule", "")) != "wall_post":
			continue
		posts += 1
		# The post is the one part whose boxes meet no cell boundary, so a declared
		# claim could only be a face it does not reach, which the loader refuses: what
		# the rule reads is not one neighbour at all.
		if part.has("faces"):
			_fail("wall/all declares claim faces on the post, which claims none: %s" % str(part["faces"]))
	if posts != 1:
		_fail("wall/all has %d wall_post parts, expected exactly 1" % posts)
	var hands := {}
	for part in wall_parts:
		var hand_rule := String(part.get("rule", ""))
		if hand_rule != "wall_arm" and hand_rule != "wall_bearing":
			continue
		var declared: Array = part.get("faces", [])
		if declared.size() != 1:
			_fail("wall/all %s part declares %d faces, expected exactly 1" % [hand_rule, declared.size()])
			continue
		hands[hand_rule + "/" + String(declared[0])] = true
	for side in ["n", "s", "e", "w"]:
		if not hands.has("wall_arm/" + side) or not hands.has("wall_bearing/" + side):
			_fail("wall/all is missing the %s hand of wall_arm or wall_bearing" % side)
	if hands.size() != 8:
		_fail("wall/all has %d distinct hands, expected 8" % hands.size())
	# ...and the two heights must be the SAME box, differing only in where the top is.
	# A file that let them drift would draw a reach that changed footprint as a block
	# came down on it, which reads as the wall moving rather than as it bearing a load.
	for side in ["n", "s", "e", "w"]:
		var short_box: Array = []
		var tall_box: Array = []
		for part in wall_parts:
			var hand_rule := String(part.get("rule", ""))
			var declared: Array = part.get("faces", [])
			if declared.size() != 1 or String(declared[0]) != side:
				continue
			if hand_rule == "wall_arm":
				short_box = part.get("boxes", [])
			elif hand_rule == "wall_bearing":
				tall_box = part.get("boxes", [])
		if short_box.size() != 1 or tall_box.size() != 1:
			_fail("wall/all %s has %d short and %d tall reach boxes, expected 1 each"
				% [side, short_box.size(), tall_box.size()])
			continue
		var flat_short: Array = short_box[0]
		var flat_tall: Array = tall_box[0]
		for k in range(6):
			if k == 4:
				continue
			if absf(float(flat_short[k]) - float(flat_tall[k])) > 0.0001:
				_fail("wall/all %s reaches differ outside their height: %s vs %s"
					% [side, str(flat_short), str(flat_tall)])
		if not (absf(float(flat_short[4]) - 0.875) < 0.0001 and absf(float(flat_tall[4]) - 1.0) < 0.0001):
			_fail("wall/all %s reaches are %s and %s tall, expected 14/16 and the cell top"
				% [side, str(flat_short[4]), str(flat_tall[4])])

	var stair_parts: Array = shapes.get("stair", {}).get("n", {}).get("parts", [])
	var rules := {}
	var unconditional := 0
	for part in stair_parts:
		var rule := String(part.get("rule", ""))
		if rule == "":
			unconditional += 1
		else:
			rules[rule] = int(rules.get(rule, 0)) + 1
	if unconditional != 1:
		_fail("stair/n has %d unconditional parts, expected 1 (the slab)" % unconditional)
	for rule in ["stair_step", "stair_cut_left", "stair_cut_right", "stair_corner_left", "stair_corner_right"]:
		if int(rules.get(rule, 0)) != 1:
			_fail("stair/n has %d parts carrying the %s rule, expected 1" % [int(rules.get(rule, 0)), rule])
	if rules.size() != 5:
		_fail("stair/n carries %d distinct rules, expected the step, two remnants and two quarters" % rules.size())

	# The two ways up are the same rules on mirrored geometry, so the halves have to
	# stay in step: same rules, same order, and every box flipped about the cell floor.
	# Nothing else would catch a drift here  -  the rules would simply draw an upright
	# step inside a hanging stair, which looks like a slightly wrong stair rather than
	# like a bug.
	for variant in ["n", "s", "e", "w"]:
		var up_parts: Array = shapes.get("stair", {}).get(variant, {}).get("parts", [])
		var down_parts: Array = shapes.get("stair", {}).get(variant + "_up", {}).get("parts", [])
		if up_parts.size() != down_parts.size():
			_fail("stair/%s has %d parts but stair/%s_up has %d" % [variant, up_parts.size(), variant, down_parts.size()])
			continue
		for i in range(up_parts.size()):
			var up_rule := String(up_parts[i].get("rule", ""))
			var down_rule := String(down_parts[i].get("rule", ""))
			if up_rule != down_rule:
				_fail("stair/%s_up part %d carries rule %s where stair/%s carries %s" % [variant, i, down_rule, variant, up_rule])
				continue
			for j in range(up_parts[i]["boxes"].size()):
				var a: Array = up_parts[i]["boxes"][j]
				var b: Array = down_parts[i]["boxes"][j]
				if not _box_is(b, float(a[0]), 1.0 - float(a[4]), float(a[2]), float(a[3]), 1.0 - float(a[1]), float(a[5])):
					_fail("stair/%s_up part %d box %d is not stair/%s mirrored about the floor: %s vs %s" % [variant, i, j, variant, str(a), str(b)])
	print("probe: the hanging stairs are the upright ones mirrored, rule for rule")

	# The wall torch hangs off the floor, which is what makes its underside
	# visible: the emitter draws a box's bottom face only when the box is raised.
	for v in ["n", "s", "e", "w"]:
		var box: Dictionary = shapes.get("torch_wall", {}).get(v, {})
		var boxes: Array = box.get("selection_boxes", [])
		if boxes.is_empty() or float(boxes[0][1]) <= 0.0:
			_fail("torch_wall/%s is not raised off the cell floor, so its underside would be invisible" % v)

	# A layer must be strictly taller than the one below it or the ladder of
	# heights is not a ladder.
	var prev := 0.0
	for i in range(1, 9):
		var boxes: Array = shapes.get("layer", {}).get(str(i), {}).get("selection_boxes", [])
		if boxes.is_empty():
			_fail("layer/%d is missing" % i)
			continue
		var h := float(boxes[0][4])
		if h <= prev:
			_fail("layer/%d is %.4f tall, not taller than layer %d (%.4f)" % [i, h, i - 1, prev])
		prev = h

	# The wall family spans two files, and the C++ side reads it by DECLARATION ORDER:
	# the first entry carrying the connected shape becomes the body a mined wall drops,
	# and the entry with no shape at all becomes the solid block a wall thickens into.
	# Reordering the family would silently make a wall drop nothing (the drop would be
	# air), which no box list can show, so the contract is checked against the file.
	var defs_file := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if defs_file == null:
		_fail("data/block_definitions.json is not readable")
		return
	var defs: Variant = JSON.parse_string(defs_file.get_as_text())
	defs_file.close()
	if typeof(defs) != TYPE_ARRAY:
		_fail("data/block_definitions.json did not parse to a block list")
		return
	var wall_families := {}
	for b in defs:
		if not b.has("wall_family"):
			continue
		var fam := String(b["wall_family"])
		if not wall_families.has(fam):
			wall_families[fam] = []
		wall_families[fam].append(b)
	if wall_families.is_empty():
		_fail("no wall family in block_definitions.json  -  the wall checks cannot be pinning anything")
	for fam in wall_families.keys():
		var entries: Array = wall_families[fam]
		var first: Dictionary = entries[0]
		if String(first.get("shape", "")) != "wall/all":
			_fail("wall family %s is declared first as \"%s\"; the family's body has to be the first entry, because that is the id a mined wall drops" % [fam, str(first.get("shape", ""))])
		if bool(first.get("hidden", false)):
			_fail("wall family %s declares a HIDDEN body first; a mined wall would drop a block the inventory does not offer" % fam)
		var solid := 0
		for b in entries:
			if not b.has("shape"):
				solid += 1
		# At most one: a family MAY have a shapeless solid block a wall can thicken
		# into, but it need not -- the stone family has none, and a solid wall is not
		# a thing the reference has either.
		if solid > 1:
			_fail("wall family %s has %d entries with no shape; at most one (the solid block a wall may thicken into) is expected" % [fam, solid])
	print("probe: %d wall families declare their body first and at most one solid block" % wall_families.size())
