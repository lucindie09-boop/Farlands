extends SceneTree
## Headless check for the torch, the empty bucket and the iron ingot.
##
## Three new items, three new behaviours, each driven through the real game path
## rather than asserted on data alone:
##   torch      - holding it OWNS the player's dynamic light (level and colour
##                come from items.json), and putting it away hands the light back
##                to whatever the scene's own toggle said before it took over.
##   bucket     - right-clicking a fluid SOURCE empties the cell and swaps the
##                empty bucket for that fluid's filled one.
##   iron_ingot - the iron hammer and pickaxe recipes match a crafting grid.
##
## The bucket aim is built from the real aim ray: the probe carves the cells the
## ray passes through, puts a source in the one straight ahead of the eye, and
## then uses the item. Nothing about the geometry is assumed — if the crosshair
## stops somewhere else the probe says so instead of passing by accident.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_torch_bucket.gd

const SETTLE_ATTEMPTS := 600
const LANE_LENGTH := 3.0

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
	# The data files (items and recipes) are loaded on the engine's first frames,
	# so nothing may be queried before them.
	await process_frame
	await process_frame

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return

	var torch_id := BlockTextures.get_block_id_by_name("torch")
	var bucket_id := BlockTextures.get_block_id_by_name("bucket")
	var ingot_id := BlockTextures.get_block_id_by_name("iron_ingot")
	var water_bucket_id := BlockTextures.get_block_id_by_name("water_bucket")
	var lava_bucket_id := BlockTextures.get_block_id_by_name("lava_bucket")
	var stick_id := BlockTextures.get_block_id_by_name("stick")
	var water_id := BlockTextures.get_block_id_by_name("water")
	var lava_id := BlockTextures.get_block_id_by_name("lava")
	var stone_id := BlockTextures.get_block_id_by_name("stone")
	var hammer_id := BlockTextures.get_block_id_by_name("iron_hammer")
	var pickaxe_id := BlockTextures.get_block_id_by_name("iron_pickaxe")
	var cobble_id := BlockTextures.get_block_id_by_name("cobblestone")
	for pair in [["torch", torch_id], ["bucket", bucket_id], ["iron_ingot", ingot_id],
			["iron_hammer", hammer_id], ["iron_pickaxe", pickaxe_id]]:
		if int(pair[1]) <= 0:
			_fail("%s did not resolve by name" % pair[0])
	if not ok:
		quit(1)
		return
	print("probe: torch=%d bucket=%d iron_ingot=%d" % [torch_id, bucket_id, ingot_id])

	_check_iron_recipes(player, ingot_id, stick_id, hammer_id, pickaxe_id, cobble_id)

	# --- the torch owns the dynamic light -----------------------------------
	# The scene ships with player_light_enabled = false, so this begins from the
	# state a fresh game is in.
	cm.player_light_enabled = false
	await process_frame
	player.set_hotbar_slot(0, torch_id, 1)
	player.select_hotbar_slot(0)
	await process_frame
	await process_frame
	if player.get_selected_block() != torch_id:
		_fail("could not select the torch (slot holds %d)" % player.get_selected_block())
	elif not cm.player_light_enabled:
		_fail("holding the torch left the dynamic light off")
	elif cm.player_light_level != 14:
		_fail("the torch set the light level to %d, expected 14" % cm.player_light_level)
	elif not cm.player_light_color.is_equal_approx(Color(1.0, 0.83, 0.6)):
		_fail("the torch set the light colour to %s, expected (1, 0.83, 0.6)" % cm.player_light_color)
	else:
		print("probe: the torch set the light on, level %d, colour %s"
			% [cm.player_light_level, cm.player_light_color])

	# Putting it away hands the light back to the scene's own toggle (off).
	player.set_hotbar_slot(0, stick_id, 1)
	await process_frame
	await process_frame
	if player.get_selected_block() != stick_id:
		_fail("could not select the stick (slot holds %d)" % player.get_selected_block())
	elif cm.player_light_enabled:
		_fail("putting the torch away left the dynamic light on")
	elif cm.player_light_level != 12 or not cm.player_light_color.is_equal_approx(Color(1.0, 0.9, 0.7)):
		_fail("putting the torch away left the light at level %d / colour %s, expected 12 / (1, 0.9, 0.7)"
			% [cm.player_light_level, cm.player_light_color])
	else:
		print("probe: putting the torch away restored level %d, colour %s"
			% [cm.player_light_level, cm.player_light_color])

	# ...and a toggle the user set themselves must survive a torch in between.
	cm.player_light_enabled = true
	player.set_hotbar_slot(0, torch_id, 1)
	await process_frame
	await process_frame
	player.set_hotbar_slot(0, stick_id, 1)
	await process_frame
	await process_frame
	if not cm.player_light_enabled:
		_fail("a torch in between wiped the manual player_light_enabled = true")
	else:
		print("probe: a manual light toggle survives holding the torch")
	cm.player_light_enabled = false

	# --- the bucket ---------------------------------------------------------
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
	if not await _wait_for_applied_writes(cm, player, stone_id):
		quit(1)
		return

	var feet := player.global_position.floor()
	var origin: Vector3 = player.get_aim_origin()
	var dir: Vector3 = player.get_aim_direction()
	print("probe: aim from %s along %s" % [origin, dir])
	var lane: Array = []
	for entry in _lane_cells(player, origin, dir):
		var c: Vector3i = entry
		lane.append({"cell": c, "was": cm.get_block(c.x, c.y, c.z)})
		cm.set_block(c.x, c.y, c.z, 0)
		await process_frame
	if lane.size() < 2:
		_fail("aim ray gave only %d cells to work with" % lane.size())
		quit(1)
		return

	# The source goes in the cell the ray runs through, one cell above the floor,
	# so the ray is inside its box and the crosshair is on the fluid itself.
	var pool: Vector3i = lane[1]["cell"]
	var support := Vector3i(pool.x, int(feet.y), pool.z)
	var support_was: int = cm.get_block(support.x, support.y, support.z)
	cm.set_block(support.x, support.y, support.z, stone_id)
	cm.set_block(pool.x, pool.y, pool.z, water_id)
	await process_frame
	if cm.get_block(pool.x, pool.y, pool.z) != water_id:
		_fail("could not place a water source at %s" % pool)
		quit(1)
		return

	player.set_hotbar_slot(1, bucket_id, 1)
	player.select_hotbar_slot(1)
	await process_frame
	if player.get_selected_block() != bucket_id:
		_fail("could not select the empty bucket (slot holds %d)" % player.get_selected_block())
		quit(1)
		return
	print("probe: holding the empty bucket with the source at %s under the crosshair" % pool)

	player.use_item()
	await process_frame
	var pool_after: int = cm.get_block(pool.x, pool.y, pool.z)
	var slot_after: int = player.get_hotbar_slot_block_id(1)
	if pool_after != 0:
		_fail("the bucket left block %d in the source cell at %s, expected air" % [pool_after, pool])
	elif slot_after != water_bucket_id:
		_fail("the bucket did not become a water bucket (slot holds %d, water_bucket is %d)"
			% [slot_after, water_bucket_id])
	else:
		print("probe: the bucket picked the source up and became a water bucket")

	# --- a different fluid becomes a different bucket -----------------------
	cm.set_block(pool.x, pool.y, pool.z, lava_id)
	await process_frame
	if cm.get_block(pool.x, pool.y, pool.z) != lava_id:
		_fail("could not set up a lava source at %s" % pool)
		quit(1)
		return
	player.set_hotbar_slot(1, bucket_id, 1)
	player.select_hotbar_slot(1)
	await process_frame
	player.use_item()
	await process_frame
	if cm.get_block(pool.x, pool.y, pool.z) != 0:
		_fail("the bucket left block %d in the lava source cell"
			% cm.get_block(pool.x, pool.y, pool.z))
	elif player.get_hotbar_slot_block_id(1) != lava_bucket_id:
		_fail("lava did not become a lava bucket (slot holds %d, lava_bucket is %d)"
			% [player.get_hotbar_slot_block_id(1), lava_bucket_id])
	else:
		print("probe: lava fills the bucket into a lava bucket")

	# --- a plain item is not a container ------------------------------------
	# The negative control: the stick has no use action, so it must neither empty
	# the source nor be consumed.
	cm.set_block(pool.x, pool.y, pool.z, water_id)
	await process_frame
	player.set_hotbar_slot(1, stick_id, 1)
	player.select_hotbar_slot(1)
	await process_frame
	player.use_item()
	await process_frame
	if cm.get_block(pool.x, pool.y, pool.z) != water_id:
		_fail("using a stick emptied the water at %s" % pool)
	elif player.get_hotbar_slot_count(1) != 1:
		_fail("using a stick consumed it (%d left)" % player.get_hotbar_slot_count(1))
	else:
		print("probe: a stick neither picks up fluid nor is consumed")

	# --- put the world back -------------------------------------------------
	cm.set_block(pool.x, pool.y, pool.z, 0)
	cm.set_block(support.x, support.y, support.z, support_was)
	for entry in lane:
		var c: Vector3i = entry["cell"]
		if c == pool:
			continue
		cm.set_block(c.x, c.y, c.z, entry["was"])
	await process_frame

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

# The iron recipes, matched on a real 3x3 grid through the same call the crafting
# UI uses. The cobblestone hammer is the control: the new entries must not have
# displaced it.
func _check_iron_recipes(player: Node3D, ingot: int, stick: int, hammer: int,
		pickaxe: int, cobble: int) -> void:
	var grid: PackedInt32Array
	var counts: PackedInt32Array

	grid = PackedInt32Array([0, ingot, 0, 0, stick, ingot, stick, 0, 0])
	counts = PackedInt32Array([0, 1, 0, 0, 1, 1, 1, 0, 0])
	var hammer_match: Dictionary = player.match_recipe(grid, counts)
	if not hammer_match.get("ok", false) or int(hammer_match.get("block_id", 0)) != hammer:
		_fail("the iron hammer pattern matched %s, expected iron_hammer (%d)"
			% [hammer_match, hammer])
	else:
		print("probe: the iron hammer pattern matches (\" f \", \" /f\", \"/  \")")

	grid = PackedInt32Array([ingot, ingot, ingot, 0, stick, 0, 0, stick, 0])
	counts = PackedInt32Array([1, 1, 1, 0, 1, 0, 0, 1, 0])
	var pickaxe_match: Dictionary = player.match_recipe(grid, counts)
	if not pickaxe_match.get("ok", false) or int(pickaxe_match.get("block_id", 0)) != pickaxe:
		_fail("the iron pickaxe pattern matched %s, expected iron_pickaxe (%d)"
			% [pickaxe_match, pickaxe])
	else:
		print("probe: the iron pickaxe pattern matches (\"III\", \" S \", \" S \")")

	grid = PackedInt32Array([0, cobble, 0, 0, stick, cobble, stick, 0, 0])
	counts = PackedInt32Array([0, 1, 0, 0, 1, 1, 1, 0, 0])
	var stone_match: Dictionary = player.match_recipe(grid, counts)
	var stone_hammer: int = BlockTextures.get_block_id_by_name("stone_hammer")
	if not stone_match.get("ok", false) or int(stone_match.get("block_id", 0)) != stone_hammer:
		_fail("the stone hammer pattern matched %s, expected stone_hammer (%d)"
			% [stone_match, stone_hammer])
	else:
		print("probe: the stone hammer pattern still matches")

	# The empty bucket, so the fill/pour loop is reachable without /give.
	grid = PackedInt32Array([ingot, 0, ingot, 0, ingot, 0, 0, 0, 0])
	counts = PackedInt32Array([1, 0, 1, 0, 1, 0, 0, 0, 0])
	var bucket_match: Dictionary = player.match_recipe(grid, counts)
	var bucket_id: int = BlockTextures.get_block_id_by_name("bucket")
	if not bucket_match.get("ok", false) or int(bucket_match.get("block_id", 0)) != bucket_id:
		_fail("the bucket pattern matched %s, expected bucket (%d)"
			% [bucket_match, bucket_id])
	else:
		print("probe: the empty bucket pattern matches (\"I I\", \" I \")")

	# An iron head must NOT accept cobblestone: a recipe key names one material,
	# so a mixed grid has to fall through rather than craft.
	grid = PackedInt32Array([0, cobble, 0, 0, stick, ingot, stick, 0, 0])
	counts = PackedInt32Array([0, 1, 0, 0, 1, 1, 1, 0, 0])
	var mixed: Dictionary = player.match_recipe(grid, counts)
	if int(mixed.get("block_id", 0)) == hammer:
		_fail("an iron-and-cobblestone grid matched the iron hammer")
	else:
		print("probe: a mixed-material hammer grid matches nothing")

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
