extends SceneTree
## Headless end-to-end check for the hammers.
##
## Loads the real scene (so items.json, block_definitions.json and recipes.json
## are parsed by the same code the game runs), then holds a real tool and breaks
## a real block through PlayerController::break_block — the same call the
## hold-to-break loop makes — and reads the inventory delta:
##
##   gravel      to a stick  -> gravel       (only a hammer crushes)
##   stone       to anything -> cobblestone  (stone's own `drops`)
##   stone       to a hammer -> cobblestone  (the crush is on cobblestone now)
##   cobblestone to a hammer -> gravel       (the hammer's `crush_result`)
##   gravel      to a hammer -> sand
##
## It also builds the stone hammer's 3x3 crafting-table shape through the real
## match_recipe/craft_recipe, and checks that every block it touches names a
## texture that exists (a missing one silently renders as stone).
##
## Two world-setup traps this probe has to dodge, both of which read as "the
## drop rule is wrong" rather than as a setup problem:
##   1. BlockEditor only APPLIES a write when the target chunk has render data;
##      otherwise it queues a pending placement and returns. So every write here
##      is verified by reading the block back before the break.
##   2. A spawn buried in rock is exactly the case where the chunk has no render
##      data (underground mesh culling), so the player is moved to open ground
##      first. Do NOT pre-generate chunks as a shortcut: generating a chunk that
##      is already loaded leaves it without render data and queues every later
##      write to it forever.
##
## The break ray is the player's EYE ray (get_aim_origin / get_aim_direction),
## not the Camera3D node's transform — aiming the camera node would move
## nothing, which is what made the first version of this probe flaky.
##
## Run: Godot --headless --path <project> --script res://.freebuff/probe_hammer.gd

const HOTBAR_SLOTS := 9
const INVENTORY_SLOTS := 27
const RAY_STEP := 0.125
const RAY_STEPS := 80      # 10 blocks, matching the break reach
const SETTLE_ATTEMPTS := 600
const BREAK_ATTEMPTS := 3  # a write can be queued while a chunk is still meshing

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

	# --- the three hammer items resolve ------------------------------------
	var hammer_ids := {}
	for name in ["stone_hammer", "copper_hammer", "iron_hammer"]:
		var id := BlockTextures.get_block_id_by_name(name)
		hammer_ids[name] = id
		if id <= 0:
			_fail("%s did not resolve by name" % name)
			continue
		if not BlockTextures.is_item(id) or BlockTextures.is_hidden(id):
			_fail("%s resolved to id %d, item=%s hidden=%s"
				% [name, id, BlockTextures.is_item(id), BlockTextures.is_hidden(id)])
			continue
		var tex: Texture2D = BlockTextures.get_texture(id)
		if tex == null:
			_fail("%s resolved but no texture loaded (textures/items/%s.png)" % [name, name])
			continue
		print("probe: %-14s id=%-5d item=true texture=%dx%d"
			% [name, id, tex.get_width(), tex.get_height()])

	var stone_id := BlockTextures.get_block_id_by_name("stone")
	var cobble_id := BlockTextures.get_block_id_by_name("cobblestone")
	var gravel_id := BlockTextures.get_block_id_by_name("gravel")
	var sand_id := BlockTextures.get_block_id_by_name("sand")
	var stick_id := BlockTextures.get_block_id_by_name("stick")
	print("probe: stone=%d cobblestone=%d gravel=%d sand=%d"
		% [stone_id, cobble_id, gravel_id, sand_id])
	if cobble_id <= 0:
		_fail("cobblestone did not resolve by name")
	_check_textures(["stone", "cobblestone", "gravel"])

	# The world must be streamed in around the player before a raycast can hit
	# anything; the drop rules themselves do not care where the player stands.
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	print("probe: player landed after %d frames at %s" % [waited, player.global_position])

	# Stand somewhere open. A spawn buried in rock leaves the chunk without
	# render data, and writes into such a chunk are queued instead of applied.
	var spot := _find_open_spot(cm, player.global_position)
	if spot == Vector3.INF:
		_fail("no open standing spot within 8 blocks of %s" % player.global_position)
		quit(1)
		return
	if spot.distance_to(player.global_position.floor()) > 0.5:
		print("probe: moved the player from %s to open ground at %s"
			% [player.global_position, spot])
		player.teleport_to(spot)
		await process_frame
		await process_frame

	if not await _wait_for_applied_writes(cm, player, stone_id, cobble_id):
		quit(1)
		return

	# --- the drop rules, through a real break ------------------------------
	await _check_break(cm, player, stick_id, gravel_id, gravel_id, 1, "stick/gravel")
	await _check_break(cm, player, stick_id, stone_id, cobble_id, 1, "stick/stone")
	await _check_break(cm, player, int(hammer_ids["iron_hammer"]), stone_id, cobble_id, 1, "hammer/stone")
	await _check_break(cm, player, int(hammer_ids["iron_hammer"]), cobble_id, gravel_id, 1, "hammer/cobble")
	await _check_break(cm, player, int(hammer_ids["iron_hammer"]), gravel_id, sand_id, 1, "hammer/gravel")

	# --- the stone hammer is craftable from the real recipes.json ----------
	#   . f .
	#   . / f     f = cobblestone (the head material, which is what mining
	#   / . .            stone yields), / = stick
	# 3x3, so it is a crafting-table recipe and not an inventory-grid one.
	_check_craft(player, int(hammer_ids["stone_hammer"]), cobble_id, stick_id)

	if not ok:
		quit(1)
		return
	print("PROBE PASS")
	quit(0)

# A standing spot near `near` with solid ground and two cells of headroom, or
# Vector3.INF when the 8-block neighbourhood has none.
func _find_open_spot(cm: Node, near: Vector3) -> Vector3:
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
						var feet := Vector3(x, y + 1, z)
						if cm.get_block(x, y + 1, z) == 0 and cm.get_block(x, y + 2, z) == 0:
							return feet
						break
					y -= 1
	return Vector3.INF

# Waits until a write actually lands, which is what proves the player's chunk is
# streamed in far enough that BlockEditor applies edits instead of queueing
# them. Writes a block that differs from whatever is there, so this cannot pass
# by accident on terrain that already happens to be that block.
func _wait_for_applied_writes(cm: Node, player: Node3D, stone_id: int, cobble_id: int) -> bool:
	var base := player.global_position.floor()
	var cell := Vector3i(int(base.x), int(base.y) + 3, int(base.z))
	var existing: int = cm.get_block(cell.x, cell.y, cell.z)
	var readback := existing
	for attempt in range(SETTLE_ATTEMPTS):
		var probe_id := stone_id if readback != stone_id else cobble_id
		cm.set_block(cell.x, cell.y, cell.z, probe_id)
		await process_frame
		readback = cm.get_block(cell.x, cell.y, cell.z)
		if readback == probe_id:
			cm.set_block(cell.x, cell.y, cell.z, existing)
			print("probe: writes are applied (settled after %d attempts)" % (attempt + 1))
			return true
	print("probe: writes are never applied. cell=%s existing=%d readback=%d player=%s floor=%s"
		% [cell, existing, readback, player.global_position, player.is_on_floor()])
	print("probe: aim origin=%s dir=%s" % [player.get_aim_origin(), player.get_aim_direction()])
	_fail("a write near the player never landed, so no break could be trusted")
	return false

# Holds `held_id`, breaks one `source_id` block, and asserts the inventory gained
# `want_count` of `want_id` and nothing of the source. Retried, because a write
# can be queued rather than applied while a chunk is still being meshed.
func _check_break(cm: Node, player: Node3D,
		held_id: int, source_id: int, want_id: int, want_count: int, label: String) -> void:
	var reason := "not attempted"
	for attempt in range(BREAK_ATTEMPTS):
		player.clear_inventory()
		player.set_hotbar_slot(0, held_id, 1)
		player.select_hotbar_slot(0)
		if player.get_selected_block() != held_id:
			reason = "the hotbar slot did not take id %d (got %d)" % [held_id, player.get_selected_block()]
			continue

		var aim := _place_on_ray(cm, player, source_id)
		if not aim.get("ok", false):
			reason = "could not get a block %d onto the break ray" % source_id
			await process_frame
			continue
		var cell: Vector3 = aim["cell"]
		var previous: int = aim["previous"]

		# The edit must really be in the world before we trust the drop that
		# comes out of it: a queued write leaves the terrain that was there.
		var actual: int = cm.get_block(int(cell.x), int(cell.y), int(cell.z))
		if actual != source_id:
			reason = "the block at %s reads back as %d, not %d (the write was queued, not applied)" % [cell, actual, source_id]
			await process_frame
			continue

		var before_want := _count(player, want_id)
		var before_source := _count(player, source_id)
		# No frame in between: the aim confirmed above is still the aim break_block sees.
		player.break_block()
		var gained_want := _count(player, want_id) - before_want
		var gained_source := _count(player, source_id) - before_source
		var broken: int = cm.get_block(int(cell.x), int(cell.y), int(cell.z))
		# Undo the world edit (break_block left air; put the terrain back).
		cm.set_block(int(cell.x), int(cell.y), int(cell.z), previous)

		if broken != 0:
			reason = "the block at %s was not broken (id %d)" % [cell, broken]
			continue
		if source_id == want_id:
			# Control: the drop is the block itself, so gained_want IS the
			# source count — there is no second, independent measurement.
			if gained_want != want_count:
				reason = "breaking block %d gave %d, expected %d" % [source_id, gained_want, want_count]
				continue
			print("probe: %-13s block %-3d -> %d x itself (only a hammer crushes)"
				% [label, source_id, gained_want])
			return
		if gained_want != want_count or gained_source != 0:
			reason = "breaking block %d gave %d x block %d and %d x block %d, expected %d x block %d and no block %d" % [source_id, gained_want, want_id, gained_source, source_id, want_count, want_id, source_id]
			continue
		print("probe: %-13s block %-3d -> %d x block %-3d (no block %d)"
			% [label, source_id, gained_want, want_id, source_id])
		return
	_fail("%s: %s" % [label, reason])

# Lays the hammer shape into a 3x3 crafting grid and checks the real recipe
# matches it and consumes every ingredient.
func _check_craft(player: Node3D, hammer_id: int, material_id: int, handle_id: int) -> void:
	var cells := PackedInt32Array([
		0, material_id, 0,
		0, handle_id, material_id,
		handle_id, 0, 0])
	var counts := PackedInt32Array([
		0, 1, 0,
		0, 1, 1,
		1, 0, 0])

	var match: Dictionary = player.match_recipe(cells, counts)
	if not match.get("ok", false):
		_fail("the hammer layout matched no recipe at all")
		return
	if int(match.get("block_id", 0)) != hammer_id or int(match.get("count", 0)) != 1:
		_fail("the hammer layout gave %d x block %d, expected 1 x %d"
			% [int(match.get("count", 0)), int(match.get("block_id", 0)), hammer_id])
		return
	print("probe: hammer layout -> 1 x %d (stone_hammer)" % hammer_id)

	var crafted: Dictionary = player.craft_recipe(cells, counts)
	if not crafted.get("ok", false):
		_fail("craft_recipe refused a layout that match_recipe accepted")
		return
	var left: PackedInt32Array = crafted.get("new_counts", PackedInt32Array())
	var left_total := 0
	for c in left:
		left_total += c
	if left_total != 0:
		_fail("crafting the hammer left %d ingredients behind in the grid" % left_total)
		return
	print("probe: crafting consumed every ingredient (grid empty afterwards)")

# Every face texture a block names must exist under res://textures/blocks/ —
# otherwise the texture array falls back to stone.png and the block silently
# looks wrong in game rather than failing anywhere.
func _check_textures(block_names: Array) -> void:
	var file := FileAccess.open("res://data/block_definitions.json", FileAccess.READ)
	if file == null:
		_fail("could not open data/block_definitions.json")
		return
	var parsed = JSON.parse_string(file.get_as_text())
	file.close()
	if not (parsed is Array):
		_fail("block_definitions.json did not parse to an array")
		return
	for entry in parsed:
		var name := String(entry.get("name", ""))
		if not block_names.has(name):
			continue
		# Six faces normally name the same texture; report each name once.
		var seen := {}
		for texture_name in entry.get("textures", []):
			var texture := String(texture_name)
			if texture.is_empty() or seen.has(texture):
				continue
			seen[texture] = true
			var path := "res://textures/blocks/%s.png" % texture
			if not ResourceLoader.exists(path):
				_fail("block '%s' names texture '%s', which has no %s" % [name, texture, path])
				return
			print("probe: %-11s texture '%s' -> %s" % [name, texture, path])

# Puts a `source_id` block onto the break ray and returns {ok, cell, previous}.
# A block already on the ray is retyped; if the ray reaches nothing, one is
# marched out along it until it becomes the ray's first hit.
func _place_on_ray(cm: Node, player: Node3D, block_id: int) -> Dictionary:
	var miss := {"ok": false, "cell": Vector3.INF, "previous": 0}

	var hit: Dictionary = cm.raycast_from_camera(10.0)
	if hit.get("success", false):
		var cell := Vector3(hit["position"]).floor()
		var x := int(cell.x)
		var y := int(cell.y)
		var z := int(cell.z)
		var previous: int = cm.get_block(x, y, z)
		cm.set_block(x, y, z, block_id)
		var again: Dictionary = cm.raycast_from_camera(10.0)
		if again.get("success", false) and Vector3(again["position"]).floor() == cell:
			return {"ok": true, "cell": cell, "previous": previous}
		cm.set_block(x, y, z, previous)
		return miss

	# Nothing within reach on the ray: walk it out and drop the block into the
	# first empty cell, which then IS the ray's first hit.
	var origin: Vector3 = player.get_aim_origin()
	var dir: Vector3 = player.get_aim_direction()
	if dir.length() < 0.001:
		return miss
	dir = dir.normalized()
	var eye_cell: Vector3 = origin.floor()
	for step in range(1, RAY_STEPS):
		var cell: Vector3 = (origin + dir * (float(step) * RAY_STEP)).floor()
		if cell == eye_cell:
			continue
		var x := int(cell.x)
		var y := int(cell.y)
		var z := int(cell.z)
		cm.set_block(x, y, z, block_id)
		var placed: Dictionary = cm.raycast_from_camera(10.0)
		if placed.get("success", false) and Vector3(placed["position"]).floor() == cell:
			return {"ok": true, "cell": cell, "previous": 0}
		cm.set_block(x, y, z, 0)
	return miss

func _count(player: Node, block_id: int) -> int:
	var total := 0
	for slot in range(HOTBAR_SLOTS):
		if player.get_hotbar_slot_block_id(slot) == block_id:
			total += player.get_hotbar_slot_count(slot)
	for slot in range(INVENTORY_SLOTS):
		if player.get_inventory_slot_block_id(slot) == block_id:
			total += player.get_inventory_slot_count(slot)
	return total
