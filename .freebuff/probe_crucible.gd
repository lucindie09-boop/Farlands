extends SceneTree
## Headless check for the crucible block and its model.
##
## The crucible is the first block whose model is not a plain box, and its shape
## lives in data/block_shapes.json. Three ways that can silently go wrong:
##   * a block whose `shape` name does not resolve falls back to a FULL CUBE, so
##     the crucible would look like a block of stone rather than a pot,
##   * a collision override that is a full cube makes a body stand on the RIM
##     instead of inside the vessel, and
##   * a collision that lost the walls would let a body walk straight through it.
## So this checks the delivered geometry by name (from the real registry, which
## is what the mesh builder and the block outline read) and the collision by
## sweeping a player-sized box into a placed crucible.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_crucible.gd

const SHAPE_NAME := "crucible"
const SETTLE_ATTEMPTS := 600
const HEADROOM := 6      # air cells above the standing spot, for the drop test

# data/block_shapes.json "crucible", in block-local 16ths:
# four 1x1px corner legs 2px tall, a 1px floor plate across the whole footprint,
# then four 1px walls open at the top.
const EXPECTED_BOXES := [
	[0.0, 0.0, 0.0, 0.0625, 0.125, 0.0625],
	[0.9375, 0.0, 0.0, 1.0, 0.125, 0.0625],
	[0.0, 0.0, 0.9375, 0.0625, 0.125, 1.0],
	[0.9375, 0.0, 0.9375, 1.0, 0.125, 1.0],
	[0.0, 0.125, 0.0, 1.0, 0.1875, 1.0],
	[0.0, 0.1875, 0.0, 0.0625, 1.0, 1.0],
	[0.9375, 0.1875, 0.0, 1.0, 1.0, 1.0],
	[0.0625, 0.1875, 0.0, 0.9375, 1.0, 0.0625],
	[0.0625, 0.1875, 0.9375, 0.9375, 1.0, 1.0],
]

var main: Node3D
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
	main = scene.instantiate()
	root.add_child(main)

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return

	# --- the block resolves, with a texture --------------------------------
	var id := BlockTextures.get_block_id_by_name(SHAPE_NAME)
	if id <= 0:
		_fail("crucible did not resolve by name")
		quit(1)
		return
	if BlockTextures.is_item(id) or BlockTextures.is_hidden(id):
		_fail("crucible resolved to id %d, item=%s hidden=%s (it must be a placeable block)"
			% [id, BlockTextures.is_item(id), BlockTextures.is_hidden(id)])
	var tex: Texture2D = BlockTextures.get_texture(id)
	if tex == null:
		_fail("crucible resolved but no texture loaded")
	else:
		print("probe: crucible id=%d texture=%dx%d" % [id, tex.get_width(), tex.get_height()])

	# --- the model the engine will mesh ------------------------------------
	var boxes: Array = cm.get_selection_boxes(id)
	if boxes.size() != EXPECTED_BOXES.size():
		_fail("crucible has %d selection boxes, expected %d — a block whose shape name does not resolve falls back to a single full cube"
			% [boxes.size(), EXPECTED_BOXES.size()])
	else:
		var mismatches := 0
		for i in range(boxes.size()):
			var got := PackedFloat32Array(boxes[i])
			if got.size() != 6:
				mismatches += 1
				continue
			for f in range(6):
				if absf(got[f] - float(EXPECTED_BOXES[i][f])) > 0.0001:
					mismatches += 1
					break
		if mismatches > 0:
			_fail("%d of %d crucible boxes do not match the documented 16ths model" % [mismatches, boxes.size()])
		else:
			print("probe: model = %d boxes, legs 0..0.125, floor 0.125..0.1875, walls 0.1875..1.0" % boxes.size())

	# --- the shape file also decides the COLLISION -------------------------
	# get_selection_boxes is what the mesh/outline use; the collision comes from
	# the same entry, so check the file declares NO override (collision then
	# follows the model, which is what lets a body stand inside the vessel).
	_check_shape_file()

	# --- behaviour: a body ends up INSIDE the pot, on its floor ------------
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	var spot := _find_open_spot(cm, player.global_position, HEADROOM)
	if spot == Vector3.INF:
		_fail("no standing spot with %d cells of headroom near %s" % [HEADROOM, player.global_position])
		quit(1)
		return
	if spot.distance_to(player.global_position.floor()) > 0.5:
		print("probe: moved the player from %s to open ground at %s" % [player.global_position, spot])
		player.teleport_to(spot)
		await process_frame
		await process_frame

	var cell := Vector3i(int(spot.x), int(spot.y) + 3, int(spot.z))
	var stone_id := BlockTextures.get_block_id_by_name("stone")
	var cobble_id := BlockTextures.get_block_id_by_name("cobblestone")
	if not await _wait_for_applied_writes(cm, player, stone_id, cobble_id):
		quit(1)
		return
	cm.set_block(cell.x, cell.y, cell.z, id)
	await process_frame
	if cm.get_block(cell.x, cell.y, cell.z) != id:
		_fail("placing the crucible at %s did not stick" % cell)
		quit(1)
		return

	# Drop the player's own collision box through the crucible's mouth. It must
	# come to rest on the INNER floor plate (3/16 above the cell base), not on the
	# full-block rim — that is the whole point of the model being the collision.
	var box_size := Vector3(0.6, 1.8, 0.6)
	var from := Vector3(float(cell.x) + 0.5, float(cell.y) + 4.0, float(cell.z) + 0.5)
	var hit: Dictionary = cm.resolve_voxel_collision(from, Vector3(0.0, -4.0, 0.0), box_size)
	var landed: Vector3 = hit["position"]
	var floor_surface := float(cell.y) + 0.1875
	var rim := float(cell.y) + 1.0
	if absf(landed.y - floor_surface) > 0.01:
		_fail("a %s box dropped into the crucible at %s landed at y=%.4f, expected the inner floor at %.4f (a solid-cube collision would stop it at the rim, %.4f)"
			% [box_size, cell, landed.y, floor_surface, rim])
	elif not hit.get("on_floor", false):
		_fail("the box rested at y=%.4f but on_floor is false" % landed.y)
	elif absf(landed.x - (float(cell.x) + 0.5)) > 0.01 or absf(landed.z - (float(cell.z) + 0.5)) > 0.01:
		_fail("the box landed at %s, off-centre from the crucible at %s" % [landed, cell])
	else:
		print("probe: a player-sized box dropped into it rests INSIDE on the floor plate at y=%.4f (rim would be %.4f, on_floor=%s)"
			% [landed.y, rim, hit.get("on_floor", false)])

	# The side of the vessel stopping a body on foot (legs are only 1/16 corner
	# columns, so it is the walls that do it) is pinned deterministically in
	# tests/test_collision_resolver.cpp on a flat synthetic world instead of here:
	# a lane scan around real terrain would make this probe flaky, and a missing
	# wall is already caught by the box-by-box model check above.

	# Put the world back (we overwrote whatever was at that air cell).
	cm.set_block(cell.x, cell.y, cell.z, 0)

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

# The shape entry must carry the visible model and NO collision override, so the
# collision follows the model (removing an override that used to be a solid cube
# is exactly the fix that lets a body stand inside the vessel).
func _check_shape_file() -> void:
	var file := FileAccess.open("res://data/block_shapes.json", FileAccess.READ)
	if file == null:
		_fail("could not open data/block_shapes.json")
		return
	var parsed = JSON.parse_string(file.get_as_text())
	file.close()
	if not (parsed is Dictionary) or not parsed.has(SHAPE_NAME):
		_fail("data/block_shapes.json has no \"%s\" entry" % SHAPE_NAME)
		return
	var entry: Dictionary = parsed[SHAPE_NAME]
	var boxes: Array = entry.get("selection_boxes", [])
	if boxes.size() != EXPECTED_BOXES.size():
		_fail("the \"%s\" entry has %d selection boxes, expected %d" % [SHAPE_NAME, boxes.size(), EXPECTED_BOXES.size()])
	var collision: Array = entry.get("collision_boxes", [])
	if collision.size() != 0:
		_fail("the \"%s\" entry declares %d collision boxes; it must declare NONE so collision falls back to the %d-box model (a full-cube override puts a body on the rim, not inside)"
			% [SHAPE_NAME, collision.size(), EXPECTED_BOXES.size()])
		return
	print("probe: shape file: %d model boxes, no collision override (collision = the model)" % boxes.size())

# A standing spot near `near` with solid ground and `headroom` cells of air
# above it, or Vector3.INF.
func _find_open_spot(cm: Node, near: Vector3, headroom: int) -> Vector3:
	var base_x := int(floor(near.x))
	var base_z := int(floor(near.z))
	var start_y := int(floor(near.y)) + 64
	for radius in range(0, 9):
		for dx in range(-radius, radius + 1):
			for dz in range(-radius, radius + 1):
				if absi(dx) != radius and absi(dz) != radius:
					continue  # already covered by a smaller radius
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

# Waits until a write actually lands: BlockEditor queues a write instead of
# applying it while the target chunk has no render data.
func _wait_for_applied_writes(cm: Node, player: Node3D, stone_id: int, cobble_id: int) -> bool:
	var base := player.global_position.floor()
	var cell := Vector3i(int(base.x), int(base.y) + 2, int(base.z))
	var existing: int = cm.get_block(cell.x, cell.y, cell.z)
	var readback := existing
	for attempt in range(SETTLE_ATTEMPTS):
		var probe_id := stone_id if readback != stone_id else cobble_id
		cm.set_block(cell.x, cell.y, cell.z, probe_id)
		await process_frame
		readback = cm.get_block(cell.x, cell.y, cell.z)
		if readback == probe_id:
			cm.set_block(cell.x, cell.y, cell.z, existing)
			return true
	_fail("a write near the player never landed, so the crucible could not be placed")
	return false
