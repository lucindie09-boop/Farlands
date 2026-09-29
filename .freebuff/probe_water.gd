extends SceneTree
## Does a body swim through water, or stand on it?
##
## Water used to be a solid 0.88-high box: a body dropped onto a water cell came to
## rest at cell + 0.88 with on_floor true. The fluid work makes liquids passable and
## gives the body basic water movement, so this drives the REAL player through a
## placed water column and compares it against the same fall in air. The air run is
## the control: a body that still stops on the water surface fails both halves, and
## a body that sinks at full speed fails the rate half.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_water.gd

const SETTLE_ATTEMPTS := 600
const DROP_HEIGHT := 10         # cells above the standing spot the drop starts from
const WATER_CELLS := 8          # water from the spot up, so it is 2.1 cells below the drop
const TIMEOUT_MS := 20000

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

	# --- settle, then find open ground -------------------------------------
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

	# Clear this column for the height of the drop and remember what was in it, so
	# how deep the water test can be does not depend on the shape of the terrain.
	var column_original := []
	for i in range(1, DROP_HEIGHT + 2):
		var c := Vector3i(int(spot.x), int(spot.y) + i, int(spot.z))
		column_original.append(cm.get_block(c.x, c.y, c.z))
		cm.set_block(c.x, c.y, c.z, 0)
		await process_frame

	var stone_id := BlockTextures.get_block_id_by_name("stone")
	var water_id := BlockTextures.get_block_id_by_name("water")
	if water_id <= 0:
		_fail("water did not resolve by name")
		quit(1)
		return
	if not await _wait_for_applied_writes(cm, player, stone_id, water_id):
		quit(1)
		return
	print("probe: standing spot %s (water id=%d)" % [spot, water_id])

	# --- control: the same drop in air -------------------------------------
	var air := await _drop_and_time(player, spot)
	print("probe: air    drop from +%d landed at y=%.4f (spot.y=%.1f) after %d ms"
		% [DROP_HEIGHT, air["y"], spot.y, air["ms"]])

	# --- the water run -----------------------------------------------------
	# The column starts AT the standing cell, so the body ends up standing in it.
	var placed := 0
	for i in range(0, WATER_CELLS):
		cm.set_block(int(spot.x), int(spot.y) + i, int(spot.z), water_id)
		await process_frame
	for i in range(0, WATER_CELLS):
		if cm.get_block(int(spot.x), int(spot.y) + i, int(spot.z)) == water_id:
			placed += 1
	if placed != WATER_CELLS:
		_fail("only %d of %d water cells could be placed at %s" % [placed, WATER_CELLS, spot])
		quit(1)
		return
	var water_surface := spot.y + float(WATER_CELLS - 1) + 0.88

	var water := await _drop_and_time(player, spot)
	print("probe: water  drop from +%d landed at y=%.4f (its surface is %.2f) after %d ms"
		% [DROP_HEIGHT, water["y"], water_surface, water["ms"]])
	print("probe: it was inside a liquid cell during the descent: %s; it is standing in water now: %s"
		% [water["in_water"], player.is_in_water()])

	# --- what the checks mean ----------------------------------------------
	# A body resting ON the water would stop at the surface; one that swims through
	# reaches the ground under it.
	if water["y"] > spot.y + 0.5:
		_fail("the body came to rest at y=%.4f, i.e. on the water surface (%.2f) instead of the ground (%.1f)"
			% [water["y"], water_surface, spot.y])
	if not water["in_water"]:
		_fail("the body never reported being inside a liquid cell on the way down")
	if not player.is_in_water():
		_fail("standing in the water column, is_in_water() is false")
	# Sinking is slow, so the same drop must take clearly longer than in air.
	if water["ms"] < int(air["ms"] * 2):
		_fail("the drop through water took %d ms against %d ms in air — near air speed, so the liquid is barely slowing the body"
			% [water["ms"], air["ms"]])
	else:
		print("probe: the water drop took %d ms against %d ms in air (%.2fx)"
			% [water["ms"], air["ms"], float(water["ms"]) / maxf(float(air["ms"]), 1.0)])

	# --- collision-level check: a body dropped onto the water column --------
	var box := Vector3(0.6, 1.8, 0.6)
	var from := Vector3(float(spot.x) + 0.5, float(spot.y) + float(WATER_CELLS) + 3.0, float(spot.z) + 0.5)
	var hit: Dictionary = cm.resolve_voxel_collision(from, Vector3(0.0, -12.0, 0.0), box)
	var landed: Vector3 = hit["position"]
	if absf(landed.y - spot.y) > 0.05:
		_fail("a body swept down the water column rests at y=%.4f, expected the ground at %.4f (the surface is %.2f)"
			% [landed.y, spot.y, water_surface])
	else:
		print("probe: a swept body passes the %.2f surface and lands on the ground at y=%.4f (on_floor=%s)"
			% [water_surface, landed.y, hit.get("on_floor", false)])

	# --- put the world back -------------------------------------------------
	for i in range(0, WATER_CELLS):
		cm.set_block(int(spot.x), int(spot.y) + i, int(spot.z), 0)
	for i in range(column_original.size()):
		cm.set_block(int(spot.x), int(spot.y) + 1 + i, int(spot.z), column_original[i])
	await process_frame

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

# Teleports the player `DROP_HEIGHT` above the spot and waits for it to land,
# reporting the resting height, how long the fall took, and whether the body was
# ever inside a liquid cell on the way down.
func _drop_and_time(player: Node3D, spot: Vector3) -> Dictionary:
	var start := Vector3(spot.x, spot.y + DROP_HEIGHT, spot.z)
	player.teleport_to(start)
	await process_frame
	var in_water := false
	var t0 := Time.get_ticks_msec()
	var landed_y := start.y
	while Time.get_ticks_msec() - t0 < TIMEOUT_MS:
		await process_frame
		if player.is_in_water():
			in_water = true
		landed_y = player.global_position.y
		if player.is_on_floor() and Time.get_ticks_msec() - t0 > 60:
			break
	# Let it settle for a couple of frames so the reported height is the resting one.
	for i in range(5):
		await process_frame
	landed_y = player.global_position.y
	return {
		"y": landed_y,
		"ms": Time.get_ticks_msec() - t0,
		"in_water": in_water,
	}

# A standing spot near `near` with solid ground and `headroom` cells of air above.
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

# Waits until a write actually lands: BlockEditor queues a write instead of
# applying it while the target chunk has no render data.
func _wait_for_applied_writes(cm: Node, player: Node3D, stone_id: int, water_id: int) -> bool:
	var base := player.global_position.floor()
	var cell := Vector3i(int(base.x), int(base.y) + 1, int(base.z))
	var existing: int = cm.get_block(cell.x, cell.y, cell.z)
	var readback := existing
	for attempt in range(SETTLE_ATTEMPTS):
		var probe_id := stone_id if readback != stone_id else water_id
		cm.set_block(cell.x, cell.y, cell.z, probe_id)
		await process_frame
		readback = cm.get_block(cell.x, cell.y, cell.z)
		if readback == probe_id:
			cm.set_block(cell.x, cell.y, cell.z, existing)
			await process_frame
			return true
	_fail("a write near the player never landed, so no probe could be placed")
	return false
