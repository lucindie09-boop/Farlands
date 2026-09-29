extends SceneTree
## Can you actually pour water out of a bucket?
##
## Right-clicking with an item used to do nothing at all: items carried tool stats
## and a resting pose and nothing that could act on the world, so the buckets added
## earlier were inert ("Items are non-placeable and have no in-world use action").
## This drives the real use path — bucket into a hotbar slot, select it, aim down a
## cleared lane at a placed block, call use_item() — and asserts water lands in the
## cell the crosshair is against. A stick is the negative control: same call, no use
## action, nothing may change.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_bucket.gd

const SETTLE_ATTEMPTS := 600
const LANE_LENGTH := 5.0

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

	var stone_id := BlockTextures.get_block_id_by_name("stone")
	var water_id := BlockTextures.get_block_id_by_name("water")
	var bucket_id := BlockTextures.get_block_id_by_name("water_bucket")
	var stick_id := BlockTextures.get_block_id_by_name("stick")
	if bucket_id <= 0 or stick_id <= 0 or water_id <= 0:
		_fail("item or block lookup failed (bucket=%d stick=%d water=%d)"
			% [bucket_id, stick_id, water_id])
		quit(1)
		return
	if not await _wait_for_applied_writes(cm, player, stone_id):
		quit(1)
		return

	# --- carve a lane along the aim ray and cap it with a block -------------
	# The pour target is derived from the ray, so the probe has to know exactly
	# where the ray stops. Clearing the lane and putting one block at its end makes
	# the crosshair hit deterministic instead of depending on the terrain.
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
	var wall: Vector3i = lane[lane.size() - 1]["cell"]
	cm.set_block(wall.x, wall.y, wall.z, stone_id)
	await process_frame
	if cm.get_block(wall.x, wall.y, wall.z) != stone_id:
		_fail("could not place the block the crosshair is meant to hit at %s" % wall)
		quit(1)
		return

	var march := _march(cm, origin, dir)
	print("probe: aim ray from %s stops at %s; the pour cell is %s (air: %s)"
		% [origin, march["hit"], march["target"], cm.get_block(march["target"].x, march["target"].y, march["target"].z) == 0])
	if march["hit"] == wall:
		print("probe: the crosshair hits the placed block, as intended")
	else:
		_fail("the crosshair stopped at %s instead of the placed block at %s" % [march["hit"], wall])
	var target: Vector3i = march["target"]

	# --- the bucket ---------------------------------------------------------
	player.set_hotbar_slot(0, bucket_id, 1)
	player.select_hotbar_slot(0)
	await process_frame
	print("probe: holding id=%d (bucket is %d), slot count %d"
		% [player.get_selected_block(), bucket_id, player.get_hotbar_slot_count(0)])

	player.use_item()
	await process_frame
	var after: int = cm.get_block(target.x, target.y, target.z)
	if after != water_id:
		_fail("using the water bucket left block %d at %s, expected water (%d)"
			% [after, target, water_id])
	else:
		print("probe: the bucket poured water into %s" % target)

	# Putting a block in that spot must still be refused by the same guards.
	player.use_item()
	await process_frame
	if cm.get_block(target.x, target.y, target.z) != water_id:
		_fail("a second pour changed the already-filled cell at %s" % target)
	else:
		print("probe: pouring into a filled cell is refused (no overwrite)")

	if player.get_hotbar_slot_count(0) != 1:
		_fail("the bucket was consumed (%d left); v1 keeps it because there is no empty bucket item yet"
			% player.get_hotbar_slot_count(0))
	else:
		print("probe: the bucket is not consumed (still %d in the slot)" % player.get_hotbar_slot_count(0))

	# --- negative control: a stick has no use action ------------------------
	var fence: Vector3i = lane[1]["cell"]
	var fence_was: int = cm.get_block(fence.x, fence.y, fence.z)
	player.set_hotbar_slot(1, stick_id, 1)
	player.select_hotbar_slot(1)
	await process_frame
	if player.get_selected_block() != stick_id:
		_fail("could not select the stick (slot holds %d)" % player.get_selected_block())
	player.use_item()
	await process_frame
	if cm.get_block(fence.x, fence.y, fence.z) != fence_was:
		_fail("using a stick changed %s from %d to %d — items must not be placeable and a plain item has no use action"
			% [fence, fence_was, cm.get_block(fence.x, fence.y, fence.z)])
	else:
		print("probe: a stick does nothing on use (block at %s unchanged)" % fence)

	# --- put the world back -------------------------------------------------
	cm.set_block(target.x, target.y, target.z, 0)
	for entry in lane:
		var c: Vector3i = entry["cell"]
		if c == target:
			continue
		cm.set_block(c.x, c.y, c.z, entry["was"])
	await process_frame

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

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

# First non-air cell along the ray and the last air cell before it (the pour cell).
func _march(cm: Node, origin: Vector3, dir: Vector3) -> Dictionary:
	var hit := Vector3i(-9999, -9999, -9999)
	var target := Vector3i(-9999, -9999, -9999)
	var t := 0.0
	while t <= 10.0:
		var p := origin + dir * t
		var c := Vector3i(int(floor(p.x)), int(floor(p.y)), int(floor(p.z)))
		if c != target:
			if cm.get_block(c.x, c.y, c.z) != 0:
				hit = c
				break
			# air: remember it, so when the ray stops the previous air cell is the
			# one a block would be placed in (and the one a pour lands in)
			target = c
		t += 0.1
	return {"hit": hit, "target": target}

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
	_fail("a write near the player never landed, so no probe could be placed")
	return false
