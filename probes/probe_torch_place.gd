extends SceneTree
## Headless check that the torch ITEM places the torch BLOCK through the real
## right-click path.
##
## The item and the block are deliberately different ids (the item is the held
## light, the block is the thing in the world), and the only bridge between them
## is an item's declared "place". This drives that bridge end to end: give the
## item, select it, aim at a real face, use_item(), and read back the cell the
## aim points into. It also checks the click CONSUMES the item and that what
## landed is the torch model rather than a full cube.
##
## The variant is not assumed: the probe reads the same raycast the game does and
## computes which torch a click on that face should produce, so a wall click and a
## top click are both handled by one run.
##
## Run (snapshots and restores the world):
##   .freebuff/run_probe.sh .freebuff/probe_torch_place.gd

const SETTLE_ATTEMPTS := 600
const LANE_LENGTH := 3.0

# The documented 16ths models, matching data/block_shapes.json: the standing
# torch, then the four wall variants keyed by the face they hug (n = -Z,
# s = +Z, e = +X, w = -X).
const STANDING_BOX := [0.4375, 0.0, 0.4375, 0.5625, 0.625, 0.5625]
const WALL_BOXES := [
	[0.4375, 0.21875, 0.0, 0.5625, 0.84375, 0.125],    # n
	[0.4375, 0.21875, 0.875, 0.5625, 0.84375, 1.0],    # s
	[0.875, 0.21875, 0.4375, 1.0, 0.84375, 0.5625],    # e
	[0.0, 0.21875, 0.4375, 0.125, 0.84375, 0.5625],    # w
]

var main: Node3D
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	# The data files are loaded on the engine's first frames.
	await process_frame
	await process_frame

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return

	var torch_item := BlockTextures.get_block_id_by_name("torch")
	var ground := BlockTextures.get_block_id_by_name("light_torch")
	var wall := [
		BlockTextures.get_block_id_by_name("light_torch_wall_n"),
		BlockTextures.get_block_id_by_name("light_torch_wall_s"),
		BlockTextures.get_block_id_by_name("light_torch_wall_e"),
		BlockTextures.get_block_id_by_name("light_torch_wall_w"),
	]
	var stone := BlockTextures.get_block_id_by_name("stone")
	for pair in [["torch", torch_item], ["light_torch", ground], ["stone", stone]]:
		if int(pair[1]) <= 0:
			_fail("%s did not resolve by name" % pair[0])
	if not ok:
		quit(1)
		return
	for i in range(4):
		if wall[i] <= 0:
			_fail("wall torch variant %d did not resolve" % i)
	if not ok:
		quit(1)
		return
	if not BlockTextures.is_item(torch_item):
		_fail("torch resolved to a block, so there is no item to bridge from")
		quit(1)
		return
	print("probe: torch item=%d, ground block=%d, wall=%s" % [torch_item, ground, wall])

	# The recipe must hand back the ITEM, not the block: the item is what places,
	# so a recipe that yields light_torch would put a thing in hand that cannot be
	# right-clicked the way the recipe implies. Pattern is ["C", "S"], so in a 2x2
	# grid the coal sits above the stick in the left column.
	var coal := BlockTextures.get_block_id_by_name("coal_ore")
	var stick := BlockTextures.get_block_id_by_name("stick")
	if coal <= 0 or stick <= 0:
		_fail("coal_ore or stick did not resolve, so the torch recipe cannot be checked")
	else:
		var match: Dictionary = player.match_recipe(
			PackedInt32Array([coal, 0, stick, 0]), PackedInt32Array([1, 0, 1, 0]))
		if not match.get("ok", false) or int(match.get("block_id", 0)) != torch_item:
			_fail("the torch recipe returned %s, expected the torch item (%d)"
				% [match, torch_item])
		else:
			print("probe: the coal-over-stick recipe yields the torch item")

	# Shape variants must not be offerable: the base block is what a player picks
	# up and the orientation is derived on placement, so /give and its autocomplete
	# must not list them. Stone's stairs are the family that used to leak (the other
	# woods' variants were flagged in data; stone's were not), and hiding is now
	# derived from family membership as well as the flag.
	var giveable: PackedStringArray = BlockTextures.get_block_names()
	for name in ["stone_stairs", "oak_stairs", "stone_wall"]:
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			_fail("%s did not resolve" % name)
		elif BlockTextures.is_hidden(id):
			_fail("%s is hidden but must stay offerable" % name)
	for name in ["stone_stairs_s", "stone_stairs_e", "stone_stairs_w",
			"stone_stairs_n_up", "stone_stairs_s_up", "stone_stairs_e_up",
			"stone_stairs_w_up"]:
		var id := BlockTextures.get_block_id_by_name(name)
		if id <= 0:
			_fail("%s did not resolve" % name)
		elif not BlockTextures.is_hidden(id):
			_fail("%s is offerable but is a placement variant" % name)
		elif giveable.has(name):
			_fail("%s is hidden yet still listed for /give" % name)
	if not ok:
		quit(1)
		return
	print("probe: stone stair variants are hidden; stone_stairs and stone_wall stay offerable")

	# Settle on the floor so the aim ray is a stable, sane one.
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	var spot := _find_open_spot(cm, player.global_position, 4)
	if spot == Vector3.INF:
		_fail("no standing spot near %s" % player.global_position)
		quit(1)
		return
	player.teleport_to(spot)
	await process_frame
	await process_frame
	if not await _wait_for_applied_writes(cm, player, stone):
		quit(1)
		return

	# Carve the cells the aim ray runs through, then stand a stone in the lane so
	# the ray has a face to hit. The cell between the player and that stone is
	# where the placement will land.
	var origin: Vector3 = player.get_aim_origin()
	var dir: Vector3 = player.get_aim_direction()
	print("probe: aim from %s along %s" % [origin, dir])
	var lane: Array = []
	for entry in _lane_cells(player, origin, dir):
		var c: Vector3i = entry
		lane.append({"cell": c, "was": cm.get_block(c.x, c.y, c.z)})
		cm.set_block(c.x, c.y, c.z, 0)
		await process_frame
	if lane.size() < 3:
		_fail("aim ray gave only %d cells to work with" % lane.size())
		quit(1)
		return
	var blocker: Vector3i = lane[2]["cell"]
	cm.set_block(blocker.x, blocker.y, blocker.z, stone)
	await process_frame
	if cm.get_block(blocker.x, blocker.y, blocker.z) != stone:
		_fail("the aim blocker never landed at %s" % blocker)
		quit(1)
		return

	# Ask the SAME ray the game will: where does the crosshair point, and on what
	# face. That decides which torch this click should make.
	var hit: Dictionary = cm.raycast_from_camera(10.0)
	if not hit.get("success", false):
		_fail("the aim ray hit nothing after the blocker landed")
		quit(1)
		return
	var place_pos: Vector3 = hit["place_position"]
	var target := Vector3i(int(floor(place_pos.x)), int(floor(place_pos.y)), int(floor(place_pos.z)))
	var n: Vector3 = hit["hit_normal"]
	var expected := _expected_torch(n, ground, wall)
	print("probe: aim normal %s -> target %s, expecting block %d" % [n, target, expected])
	if cm.get_block(target.x, target.y, target.z) != 0:
		_fail("the target cell %s is not air before placing" % target)
		quit(1)
		return

	# The real path: item in the hotbar, selected, then use_item() (right-click).
	player.set_hotbar_slot(0, torch_item, 3)
	player.select_hotbar_slot(0)
	await process_frame
	await process_frame
	if player.get_selected_block() != torch_item:
		_fail("could not select the torch item (slot holds %d)" % player.get_selected_block())
		quit(1)
		return
	var before: int = player.get_hotbar_slot_count(0)

	player.use_item()
	await process_frame
	await process_frame

	var placed: int = cm.get_block(target.x, target.y, target.z)
	if placed != expected:
		_fail("use_item left block %d at %s, expected %d (ground=%d wall=[%d,%d,%d,%d], normal %s)"
			% [placed, target, expected, ground, wall[0], wall[1], wall[2], wall[3], n])
	else:
		print("probe: the torch item placed block %d at %s" % [placed, target])

	var after: int = player.get_hotbar_slot_count(0)
	if after != before - 1:
		_fail("placing consumed %d torches, expected exactly 1" % (before - after))
	else:
		print("probe: placing consumed one torch item (%d -> %d)" % [before, after])

	# What landed must be the torch MODEL, not a full cube: that is the difference
	# between the mapping reaching a shaped block and reaching a shapeless one.
	if placed > 0:
		var boxes: Array = cm.get_selection_boxes_at(placed, target.x, target.y, target.z)
		var want := STANDING_BOX if placed == ground else _wall_box(placed, wall)
		if boxes.size() != 1:
			_fail("the placed torch resolves to %d boxes, expected the 1-box model" % boxes.size())
		elif want.is_empty() or not _box_is(boxes[0], want):
			_fail("the placed torch box %s is not the documented model %s" % [str(boxes[0]), str(want)])
		else:
			print("probe: the placed torch is the %s model"
				% ("standing" if placed == ground else "wall"))

	# The placed torch must WEAR the item's art, not a second drawing: the block
	# texture and the item texture have to be the same image.
	if placed > 0:
		var block_tex: Texture2D = BlockTextures.get_texture(placed)
		var item_tex: Texture2D = BlockTextures.get_texture(torch_item)
		if block_tex == null or item_tex == null:
			_fail("the placed torch or the torch item has no texture")
		elif not _images_equal(block_tex.get_image(), item_tex.get_image()):
			_fail("the placed torch texture differs from the torch item texture")
		else:
			print("probe: the placed torch wears the item's torch texture")

	# Breaking a placed torch must hand back the torch ITEM, not the block: the
	# craft -> place -> break loop has to close, or a player slowly accumulates a
	# block they can only get rid of through /give.
	if placed > 0:
		# Line the ray up with the torch's own 2/16 column: the spawn aim sits on a
		# cell edge and passes beside it -- which is why the placement test reads
		# place_position rather than relying on the ray striking the torch.
		var stand := player.global_position
		player.teleport_to(Vector3(stand.x + 0.5, stand.y, stand.z + 0.5))
		await process_frame
		await process_frame
		var hit2: Dictionary = cm.raycast_from_camera(10.0)
		var hpos: Vector3 = hit2.get("position", Vector3.INF)
		var hcell := Vector3i(int(floor(hpos.x)), int(floor(hpos.y)), int(floor(hpos.z)))
		if hcell != target:
			_fail("the break ray points at %s, not the torch at %s" % [hcell, target])
		var held_before: int = player.get_hotbar_slot_count(0)
		player.break_block()
		await process_frame
		await process_frame
		var held_after: int = player.get_hotbar_slot_count(0)
		var gone: int = cm.get_block(target.x, target.y, target.z)
		if gone != 0:
			_fail("the torch at %s survived the break (%d left)" % [target, gone])
		elif held_after != held_before + 1:
			_fail("breaking a torch moved the torch item count %d -> %d, expected +1"
				% [held_before, held_after])
		else:
			print("probe: breaking the placed torch returned the torch item")

	# Put the world back the way it was.
	cm.set_block(target.x, target.y, target.z, 0)
	await process_frame
	cm.set_block(blocker.x, blocker.y, blocker.z, 0)
	for entry in lane:
		var c: Vector3i = entry["cell"]
		cm.set_block(c.x, c.y, c.z, entry["was"])
	await process_frame

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

# The rule the placement path implements, spelled out from the raycast's own
# normal so the expectation is derived, not assumed: a horizontal face places the
# standing torch, a vertical face the variant that hugs the wall it was clicked on.
func _expected_torch(n: Vector3, ground: int, wall: Array) -> int:
	if absf(n.y) > 0.5:
		return ground
	if absf(n.z) >= absf(n.x):
		return wall[0] if n.z > 0.0 else wall[1]
	return wall[3] if n.x > 0.0 else wall[2]

func _wall_box(placed: int, wall: Array) -> Array:
	for i in range(4):
		if wall[i] == placed:
			return WALL_BOXES[i]
	return []

func _images_equal(a: Image, b: Image) -> bool:
	if a == null or b == null:
		return false
	if a.get_width() != b.get_width() or a.get_height() != b.get_height():
		return false
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			if not a.get_pixel(x, y).is_equal_approx(b.get_pixel(x, y)):
				return false
	return true

func _box_is(b: Array, want: Array) -> bool:
	if b.size() != 6 or want.size() != 6:
		return false
	for k in range(6):
		if absf(float(b[k]) - float(want[k])) > 0.0001:
			return false
	return true

# Cells the aim ray passes through, in order, skipping the player's own body cells
# and anything below the feet (carving the floor out from under the probe would
# make it fall while it works).
func _lane_cells(player: Node3D, origin: Vector3, dir: Vector3) -> Array:
	var out: Array = []
	var feet := player.global_position.floor()
	var py := int(feet.y)
	var t := 0.0
	while t <= LANE_LENGTH:
		var p := origin + dir * t
		var c := Vector3i(int(floor(p.x)), int(floor(p.y)), int(floor(p.z)))
		if c.y >= py and not _is_body_cell(player, c) and not out.has(c):
			out.append(c)
		t += 0.1
	return out

func _is_body_cell(player: Node3D, c: Vector3i) -> bool:
	var feet := player.global_position.floor()
	return c.x == int(feet.x) and c.z == int(feet.z) \
		and (c.y == int(feet.y) or c.y == int(feet.y) + 1)

func _find_open_spot(cm: Node, near: Vector3, headroom: int) -> Vector3:
	var base_x := int(floor(near.x))
	var base_z := int(floor(near.z))
	var start_y := int(floor(near.y)) + 64
	for radius in range(0, 9):
		for dx in range(-radius, radius + 1):
			for dz in range(-radius, radius + 1):
				if absi(dx) != radius and absi(dz) != radius:
					continue
				var x := base_x + dx
				var z := base_z + dz
				var y := start_y
				while y > 0:
					if cm.get_block(x, y, z) != 0:
						var clear := true
						for up in range(1, headroom + 1):
							if cm.get_block(x, y + up, z) != 0:
								clear = false
								break
						if clear:
							return Vector3(x, y + 1, z)
						break
					y -= 1
	return Vector3.INF

func _wait_for_applied_writes(cm: Node, player: Node3D, stone_id: int) -> bool:
	var base := player.global_position.floor()
	var cell := Vector3i(int(base.x), int(base.y) + 2, int(base.z))
	var existing: int = cm.get_block(cell.x, cell.y, cell.z)
	for attempt in range(SETTLE_ATTEMPTS):
		cm.set_block(cell.x, cell.y, cell.z, stone_id)
		await process_frame
		if cm.get_block(cell.x, cell.y, cell.z) == stone_id:
			cm.set_block(cell.x, cell.y, cell.z, existing)
			await process_frame
			return true
	_fail("block edits never applied (chunks not loaded?)")
	quit(1)
	return false
