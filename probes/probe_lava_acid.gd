extends SceneTree
## Are lava and acid real, and do the liquid textures animate in the world?
##
## Two features meet here. One is two more liquids: lava and acid are blocks of
## their own, with textures, shapes and a place in the flowing simulation, and a
## bucket of either pours one. The other is LiquidAnimator, which animates the
## liquid texture layers whenever the game runs — panel or no panel.
##
## What it pins, in order:
##   1. Every lava/acid state resolves by name in the game's own registry (the
##      JSON, not the C++ defaults the unit tests see) and each depth is its own
##      height, so a lava flow slopes the way a water flow does.
##   2. The texture array has a writable layer for lava and for acid — without
##      that, a block would draw with the stone fallback.
##   3. The animator animates all three liquids, and its frames really land in the
##      world: it reports the frame it pushed, and with compression forced on it
##      refuses (and says why) instead of writing frames Godot would drop.
##   4. A lava source on a flat shelf floods a diamond of radius THREE and stops —
##      lava's own depth limit, not water's seven — storing the distance in each
##      cell, and it drains again when the source goes.
##   5. Two acid sources with one cell between them do NOT turn that cell into a
##      spring (water would): the one rule acid does not share.
##   6. A lava bucket poured through the real use path lands lava, not water.
##   7. The Lab's "Bind to world" button hands the animator a strip under the
##      liquid's own name, which is how a strip you tuned becomes the animation
##      the world plays from then on.
##
## Run it through the wrapper, which snapshots and restores the saved world — the
## shelf and the poured liquid are persisted edits, so a bare run would leave them
## in the world the user plays:
##   .freebuff/run_probe.sh .freebuff/probe_lava_acid.gd

const SHELF_RADIUS := 14         # 29x29. Acid looks five cells for a drop and its
                                 # flood reaches seven, so the rim has to sit more
                                 # than twelve blocks from the source or the edge
                                 # would steal the flow instead of it pooling.
const WRITES_PER_FRAME := 40
const QUIET_MS := 1600           # no change for this long = settled. Lava waits a
                                 # whole second between cells, so a 700 ms lull is
                                 # just the gap between rings.
const MAX_SETTLE_MS := 90000
const AIM_LANE_LENGTH := 5.0

var main: Node3D
var cm: Node
var player: Node3D
var animator: Node
var ok := true
var depth_by_id := {}            # block id -> depth, per substance
var kind_by_id := {}             # block id -> "lava" / "acid"
var shelf := Vector3i.ZERO       # the source cell on the shelf
var original_compression := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing"); quit(1); return
	main = scene.instantiate()
	root.add_child(main)

	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	animator = root.get_node_or_null("LiquidAnimator")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing"); quit(1); return
	if animator == null:
		_fail("LiquidAnimator autoload missing — the liquid textures cannot animate")
		quit(1); return
	original_compression = bool(cm.get_compression_enabled())
	print("probe: texture compression setting at start: %s" % original_compression)

	if not _resolve_ids():
		_restore(); quit(1); return
	if not _check_shapes():
		_restore(); quit(1); return
	if not await _check_layers():
		_restore(); quit(1); return
	if not await _check_animator():
		_restore(); quit(1); return
	if not await _check_bind():
		_restore(); quit(1); return

	# Wait for the body to land: the shelf is placed relative to the terrain, and
	# a player still falling would put the ceiling scan inside the hill.
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(5):
		await process_frame

	# The bucket goes first, while the player is still standing on the terrain it
	# spawned on: the shelf below is laid through the player's own column, and a
	# body inside stone cannot aim at anything.
	if not await _check_lava_bucket():
		_restore(); quit(1); return

	var spot := player.global_position.floor()
	var sx := int(spot.x)
	var sz := int(spot.z)
	var stone := BlockTextures.get_block_id_by_name("stone")
	var ceiling := await _stable_ceiling(sx, sz)
	var sy := ceiling + 2
	if not await _build_shelf(sx, sy, sz, stone):
		_fail("the shelf never landed around (%d, %d) — the region never settled" % [sx, sz])
		_restore(); quit(1); return
	shelf = Vector3i(sx, sy, sz)
	print("probe: shelf at (%d, %d, %d), terrain ceiling %d" % [sx, sy, sz, ceiling])

	# --- lava: a diamond of radius three, then it drains ---------------------
	var lava_source := BlockTextures.get_block_id_by_name("lava")
	cm.set_block(sx, sy, sz, lava_source)
	var flood := await _settle("lava")
	if not ok:
		_restore(); quit(1); return
	var wrong := 0
	var sample := ""
	var outside := 0
	var stray := ""
	for key in flood["cells"]:
		var d: int = absi(key.x - sx) + absi(key.z - sz)
		if d > 3:
			outside += 1
			if stray == "":
				stray = "%s (%s at distance %d)" % [key, kind_by_id.get(flood["cells"][key], "?"), d]
			continue
		var depth: int = depth_by_id.get(flood["cells"][key], -1)
		if depth != d:
			wrong += 1
			if sample == "":
				sample = "%s holds depth %d, expected %d" % [key, depth, d]
	print("probe: the lava source flooded %d cells and settled" % flood["count"])
	if outside > 0:
		_fail("%d lava cells are past radius 3; first is %s" % [outside, stray])
	if wrong > 0:
		_fail("%d lava cells hold the wrong depth; first is %s" % [wrong, sample])
	if flood["count"] != 25:
		_fail("the lava flood is %d cells, expected the 25 of a radius-3 diamond" % flood["count"])
	var beyond: int = cm.get_block(sx + 4, sy, sz)
	if beyond != 0:
		_fail("lava reached 4 blocks from its source (id %d) — it may spread only three" % beyond)
	if ok:
		print("probe: lava stopped at exactly three cells (25 cells, depths = distance)")

	# Take the source away: what it fed must dry up, exactly as a water pool does.
	cm.set_block(sx, sy, sz, 0)
	await _settle("lava")
	var left := _cells_of("lava").size()
	if left != 0:
		_fail("%d lava cells stayed behind after the source was removed" % left)
	else:
		print("probe: removing the source drained all of it")

	# --- acid: two sources, one cell between them, and no spring ------------
	var acid_source := BlockTextures.get_block_id_by_name("acid")
	cm.set_block(sx - 1, sy, sz, acid_source)
	cm.set_block(sx + 1, sy, sz, acid_source)
	var acid := await _settle("acid")
	if not ok:
		_restore(); quit(1); return
	print("probe: two acid sources one cell apart flooded %d cells" % acid["count"])
	var gap: int = cm.get_block(sx, sy, sz)
	if depth_by_id.get(gap, -1) != 1 or kind_by_id.get(gap, "") != "acid":
		_fail("the cell between two acid sources holds %s (id %d), expected acid runoff at depth 1"
			% [cm.get_block_name(gap) if gap > 0 else "air", gap])
	else:
		print("probe: the gap cell is acid runoff, not a new source — acid does not pool")

	_restore()
	if not ok:
		quit(1); return
	print("PROBE PASS")
	quit(0)

# -----------------------------------------------------------------------------
# Registry, shapes, layers, animator
# -----------------------------------------------------------------------------

# Every state by NAME: ids differ between the JSON the game loads and the C++
# defaults the unit tests run on, which is why the state table resolves names.
func _resolve_ids() -> bool:
	for name in ["lava", "acid"]:
		var source: int = BlockTextures.get_block_id_by_name(name)
		if source <= 0:
			_fail("%s did not resolve by name" % name); return false
		depth_by_id[source] = 0
		kind_by_id[source] = name
	# Lava stops at depth three; acid goes all the way to seven like water.
	for depth in range(1, 8):
		for name in ["lava", "acid"]:
			if name == "lava" and depth > 3:
				if BlockTextures.get_block_id_by_name("lava_runoff_%d" % depth) > 0:
					_fail("lava_runoff_%d exists, but lava may only reach depth 3" % depth)
					return false
				continue
			var id: int = BlockTextures.get_block_id_by_name("%s_runoff_%d" % [name, depth])
			if id <= 0:
				_fail("%s_runoff_%d did not resolve by name" % [name, depth]); return false
			depth_by_id[id] = depth
			kind_by_id[id] = name
	for name in ["lava", "acid"]:
		var fallen: int = BlockTextures.get_block_id_by_name("%s_fallen" % name)
		if fallen <= 0:
			_fail("%s_fallen did not resolve by name" % name); return false
		depth_by_id[fallen] = 0
		kind_by_id[fallen] = name
	var stats: Dictionary = cm.get_fluid_stats()
	if not stats["enabled"]:
		_fail("the flow simulation is disabled — the state table found no states")
		return false
	print("probe: registry has lava (depths 0-3) and acid (0-7); %d fluid states found" % depth_by_id.size())
	return true

# Each depth is its own height or a flow renders as one flat sheet of tiles —
# read from the selection boxes the GAME registry built from the JSON, which the
# unit tests cannot see.
func _check_shapes() -> bool:
	var ladder := [["lava", 0.88], ["lava_runoff_1", 0.78], ["lava_runoff_2", 0.67],
		["lava_runoff_3", 0.56], ["lava_fallen", 1.0],
		["acid", 0.88], ["acid_runoff_1", 0.78], ["acid_runoff_2", 0.67],
		["acid_runoff_3", 0.56], ["acid_runoff_4", 0.44], ["acid_runoff_5", 0.33],
		["acid_runoff_6", 0.22], ["acid_runoff_7", 0.11], ["acid_fallen", 1.0]]
	var report: Array[String] = []
	for pair in ladder:
		var id: int = BlockTextures.get_block_id_by_name(pair[0])
		var boxes: Array = cm.get_selection_boxes(id)
		if boxes.size() != 1:
			_fail("%s has %d selection boxes, expected one" % [pair[0], boxes.size()])
			return false
		var height: float = float(boxes[0][4])
		report.append("%s=%.2f" % [pair[0], height])
		if absf(height - float(pair[1])) > 0.001:
			_fail("%s is %.3f tall, expected %.2f" % [pair[0], height, pair[1]])
			return false
	print("probe: lava/acid heights by depth: %s" % ", ".join(report))
	return true

func _check_layers() -> bool:
	var names := ["water", "lava", "acid"]
	var indices := {}
	for name in names:
		var info: Dictionary = cm.get_texture_layer_info(name)
		if not bool(info.get("found", false)):
			_fail("the texture array has no layer named \"%s\" — textures/blocks/%s.png is missing" % [name, name])
			return false
		indices[name] = int(info.get("index", -1))
		print("probe: layer %s -> %d of %d (%dx%d, %s)"
			% [name, indices[name], int(info.get("layers", 0)), int(info.get("width", 0)),
			   int(info.get("height", 0)), "writable" if bool(info.get("writable", false)) else "COMPRESSED"])
	if indices["lava"] == indices["water"] or indices["acid"] == indices["water"]:
		_fail("lava/acid share water's layer (%s) — they would draw as water" % indices)
		return false
	return true

# The animator, and the two ways it can fail: no layer to write, or a compressed
# array that cannot take a raw frame. The second is forced here rather than
# hoped for, so the refusal path is exercised too.
func _check_animator() -> bool:
	if not bool(animator.call("is_enabled")):
		_fail("the animator reports itself disabled at startup")
		return false
	# It prepares each liquid on its first tick, so give it a few frames before
	# asking: a node that has not run yet is not a node that failed.
	var deadline0 := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline0 and not _all_prepared():
		await process_frame
	var status: Dictionary = animator.call("get_status")
	for name in ["water", "lava", "acid"]:
		var entry: Dictionary = status.get(name, {})
		if entry.is_empty():
			_fail("the animator says nothing about %s" % name)
			return false
		if int(entry["frames"]) < 2:
			_fail("%s has %d frames — an animation needs at least two" % [name, int(entry["frames"])])
			return false
		if not bool(entry["animating"]):
			_fail("%s is not animating: %s" % [name, String(entry["note"])])
			return false
		print("probe: animator plays %s from %s — %d frames, %d holds of %d ticks"
			% [name, String(entry["source"]), int(entry["frames"]), int(entry["frame"]) + 1, int(entry["hold"])])

	# Does a frame actually reach the world? `pushed` is the frame the animator
	# last wrote into the texture array; it has to keep up with the playing frame.
	var start_frame := _frame_of("lava")
	var start_pushed := _pushed_of("lava")
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline:
		await process_frame
		if _frame_of("lava") != start_frame and _pushed_of("lava") == _frame_of("lava"):
			break
	if _pushed_of("lava") < 0:
		_fail("the animator never pushed a frame into the world")
		return false
	if _pushed_of("lava") != _frame_of("lava"):
		_fail("the animator is on frame %d but the world last got frame %d"
			% [_frame_of("lava"), _pushed_of("lava")])
		return false
	if _frame_of("lava") == start_frame and _pushed_of("lava") == start_pushed:
		_fail("the animator's frame never advanced in five seconds")
		return false
	print("probe: the animator's frames land in the world (lava now on frame %d, pushed %d)"
		% [_frame_of("lava"), _pushed_of("lava")])

	# Now the refusal: compression on, no permission to flip it, so the layer is
	# read-only and the animator must say so rather than push frames Godot drops.
	animator.set("allow_compression_flip", false)
	animator.call("set_enabled", false)
	cm.set_compression_enabled(true)
	animator.call("set_enabled", true)
	await process_frame
	await process_frame
	var blocked: Dictionary = (animator.call("get_status") as Dictionary).get("lava", {})
	if bool(blocked.get("animating", false)) or int(blocked.get("pushed", 0)) > 0:
		_fail("the animator claims to be animating with a compressed array (pushed %d)"
			% int(blocked.get("pushed", -1)))
		return false
	if not String(blocked.get("note", "")).contains("compress"):
		_fail("a compressed layer should be reported as the reason; got \"%s\"" % String(blocked.get("note", "")))
		return false
	print("probe: a compressed array is refused, and named: \"%s\"" % String(blocked.get("note", "")))

	# And back, with permission: compression off, layer writable, frames landing.
	animator.set("allow_compression_flip", true)
	animator.call("set_enabled", false)
	animator.call("set_enabled", true)
	var deadline2 := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline2 and _pushed_of("lava") < 0:
		await process_frame
	if _pushed_of("lava") < 0:
		_fail("the animator never recovered after compression was turned back off")
		return false
	print("probe: with compression off again it animates (lava pushed frame %d)" % _pushed_of("lava"))
	return true

# The Lab hands a strip to the world by binding it under the liquid's own name;
# the animator is what reads that name. This drives the real button on the real
# panel, so the handoff is tested where it is wired rather than re-implemented.
func _check_bind() -> bool:
	var lab := root.get_node_or_null("LiquidTextureLab")
	if lab == null:
		_fail("the LiquidTextureLab autoload is missing")
		return false
	if not lab.has_method("_bind_to_world"):
		_fail("the lab has no _bind_to_world — a tuned strip cannot reach the world")
		return false
	# The probe must not leave a binding behind that was not there before, so note
	# whether acid was already bound (a bind writes acid.json and acid.png).
	var had_binding := FileAccess.file_exists("user://liquids/acid.json")
	lab.call("_show_lab")
	# The panel generates its strip on the tick after it opens, and Bind saves that
	# strip: wait for it rather than racing the first regeneration.
	var deadline := Time.get_ticks_msec() + 3000
	while Time.get_ticks_msec() < deadline and lab.get("_strip") == null:
		await process_frame
	if lab.get("_strip") == null:
		_fail("the lab never generated a strip to bind")
		lab.call("_hide_lab")
		return false
	var pick: OptionButton = lab.get("_target_pick")
	if pick == null or pick.item_count == 0:
		_fail("the lab has no live target picker")
		return false
	for index in pick.item_count:
		if pick.get_item_text(index) == "acid":
			pick.selected = index
	lab.call("_bind_to_world")
	await process_frame
	await process_frame
	var bound := String(animator.call("bound_name", "acid"))
	var source := String((animator.call("get_status") as Dictionary).get("acid", {}).get("source", ""))
	var bound_ok := bound == "acid.json" and source == "acid.json"
	if bound_ok:
		print("probe: the lab's Bind handed the world a strip the animator now plays (%s)" % source)
	else:
		_fail("after binding, the animator's acid source is \"%s\" (bound name \"%s\"), expected acid.json"
			% [source, bound])
	lab.call("_hide_lab")
	if not had_binding:
		DirAccess.remove_absolute(ProjectSettings.globalize_path("user://liquids/acid.json"))
		DirAccess.remove_absolute(ProjectSettings.globalize_path("user://liquids/acid.png"))
	return bound_ok

func _all_prepared() -> bool:
	var status: Dictionary = animator.call("get_status")
	for name in ["water", "lava", "acid"]:
		if int((status.get(name, {}) as Dictionary).get("frames", 0)) < 2:
			return false
	return true

func _frame_of(liquid: String) -> int:
	return int((animator.call("get_status") as Dictionary).get(liquid, {}).get("frame", -1))

func _pushed_of(liquid: String) -> int:
	return int((animator.call("get_status") as Dictionary).get(liquid, {}).get("pushed", -1))

# -----------------------------------------------------------------------------
# Pouring through the real bucket path
# -----------------------------------------------------------------------------

# The same lane the water-bucket probe clears: block the aim ray with a stone so
# the crosshair target is known, then use the bucket and read the cell back.
func _check_lava_bucket() -> bool:
	# Stand somewhere known first. The body spawns at a fixed height above terrain
	# that may not be resident yet in a headless run (it falls until something
	# stops it, which can be well inside the hill), and aiming from inside rock
	# gives the ray no target at all.
	var spot := _find_open_spot(cm, player.global_position, 4)
	if spot == Vector3.INF:
		_fail("no standing spot near %s to pour from" % player.global_position)
		return false
	player.teleport_to(spot)
	await process_frame
	await process_frame

	var bucket: int = BlockTextures.get_block_id_by_name("lava_bucket")
	var stone: int = BlockTextures.get_block_id_by_name("stone")
	var lava: int = BlockTextures.get_block_id_by_name("lava")
	if bucket <= 0:
		_fail("lava_bucket did not resolve by name"); return false

	var origin: Vector3 = player.get_aim_origin()
	var dir: Vector3 = player.get_aim_direction()
	var lane: Array = []
	for entry in _lane_cells(player, origin, dir):
		var c: Vector3i = entry
		lane.append({"cell": c, "was": cm.get_block(c.x, c.y, c.z)})
		cm.set_block(c.x, c.y, c.z, 0)
		await process_frame
	if lane.size() < 3:
		_fail("aim ray gave only %d cells to work with" % lane.size()); return false
	var wall: Vector3i = lane[lane.size() - 1]["cell"]
	cm.set_block(wall.x, wall.y, wall.z, stone)
	await process_frame

	var march := _march(cm, origin, dir)
	var target: Vector3i = march["target"]
	print("probe: aim from %s along %s; %d lane cells, far end %s; the ray hits %s and the pour cell is %s"
		% [origin, dir, lane.size(), wall, march["hit"], target])
	if march["hit"] == Vector3i(-9999, -9999, -9999):
		_fail("the aim ray crossed ten blocks of air, so there is no pour target")
		return false
	player.set_hotbar_slot(0, bucket, 1)
	player.select_hotbar_slot(0)
	await process_frame
	player.use_item()
	await process_frame
	var after: int = cm.get_block(target.x, target.y, target.z)
	var poured := after == lava
	if poured:
		print("probe: the lava bucket poured lava into %s" % target)
	else:
		_fail("using the lava bucket left id %d at %s, expected lava (%d)" % [after, target, lava])

	# Put the lane back so the shelf work above is not disturbed by the crater.
	cm.set_block(target.x, target.y, target.z, 0)
	for entry in lane:
		var c: Vector3i = entry["cell"]
		if c != target:
			cm.set_block(c.x, c.y, c.z, entry["was"])
	await process_frame
	return poured

# Cells the aim ray passes through, skipping the player's own body and anything
# below the feet (carving under the probe would make it fall while it works).
func _lane_cells(p: Node3D, origin: Vector3, dir: Vector3) -> Array:
	var out: Array = []
	var py := int(p.global_position.floor().y)
	var t := 0.0
	while t <= AIM_LANE_LENGTH:
		var point := origin + dir * t
		var c := Vector3i(int(floor(point.x)), int(floor(point.y)), int(floor(point.z)))
		if c.y >= py and not _is_body_cell(p, c) and not out.has(c):
			out.append(c)
		t += 0.1
	return out

# The first spot in a ring around `near` with `headroom` clear cells above it,
# walking down from high above so the search finds the surface rather than
# stopping at the first block under the body.
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

func _is_body_cell(p: Node3D, c: Vector3i) -> bool:
	var feet := p.global_position.floor()
	return c.x == int(feet.x) and c.z == int(feet.z) \
		and (c.y == int(feet.y) or c.y == int(feet.y) + 1)

func _march(node: Node, origin: Vector3, dir: Vector3) -> Dictionary:
	var hit := Vector3i(-9999, -9999, -9999)
	var target := Vector3i(-9999, -9999, -9999)
	var t := 0.0
	while t <= 10.0:
		var point := origin + dir * t
		var c := Vector3i(int(floor(point.x)), int(floor(point.y)), int(floor(point.z)))
		if c != target:
			if node.get_block(c.x, c.y, c.z) != 0:
				hit = c
				break
			target = c
		t += 0.1
	return {"hit": hit, "target": target}

# -----------------------------------------------------------------------------
# The shelf and the floods
# -----------------------------------------------------------------------------

func _build_shelf(sx: int, sy: int, sz: int, stone: int) -> bool:
	var writes := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			cm.set_block(sx + dx, sy - 1, sz + dz, stone)
			cm.set_block(sx + dx, sy, sz + dz, 0)
			writes += 2
			if writes % WRITES_PER_FRAME == 0:
				await process_frame
	for i in range(15):
		await process_frame
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			if cm.get_block(sx + dx, sy, sz + dz) != 0:
				return false
			if cm.get_block(sx + dx, sy - 1, sz + dz) != stone:
				return false
		await process_frame
	return true

func _stable_ceiling(sx: int, sz: int) -> int:
	var previous := -1
	for attempt in range(8):
		var ceiling := _terrain_ceiling(sx, sz)
		if ceiling == previous:
			return ceiling
		previous = ceiling
		var until := Time.get_ticks_msec() + 1000
		while Time.get_ticks_msec() < until:
			await process_frame
	return previous

func _terrain_ceiling(sx: int, sz: int) -> int:
	var top := int(player.global_position.y) + 48
	var ceiling := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			var y := top
			while y > 0:
				if cm.get_block(sx + dx, y, sz + dz) != 0:
					ceiling = maxi(ceiling, y)
					break
				y -= 1
	return ceiling

# Liquid cells ON the shelf layer, inside its footprint, as {Vector3i: block_id}.
func _cells_of(kind: String) -> Dictionary:
	var out := {}
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			var key := Vector3i(shelf.x + dx, shelf.y, shelf.z + dz)
			var id: int = cm.get_block(key.x, key.y, key.z)
			if kind_by_id.get(id, "") == kind:
				out[key] = id
	return out

# Watch until the liquid stops changing. Wall-clock, because the simulation ticks
# on elapsed time and lava's own clock is a full second per cell.
func _settle(kind: String) -> Dictionary:
	var previous := {}
	var cells := {}
	var quiet_since := Time.get_ticks_msec()
	var start := quiet_since
	while Time.get_ticks_msec() - start < MAX_SETTLE_MS:
		await process_frame
		cells = _cells_of(kind)
		var same := cells.size() == previous.size()
		if same:
			for key in cells:
				if previous.get(key, -1) != cells[key]:
					same = false
					break
		if not same:
			quiet_since = Time.get_ticks_msec()
		previous = cells
		if Time.get_ticks_msec() - quiet_since >= QUIET_MS:
			break
	return {"count": cells.size(), "cells": cells,
		"quiet": Time.get_ticks_msec() - quiet_since}

# Leave the user's settings exactly as they were found.
func _restore() -> void:
	if animator != null:
		animator.set("allow_compression_flip", true)
		animator.call("set_enabled", true)
	if cm != null and bool(cm.get_compression_enabled()) != original_compression:
		cm.set_compression_enabled(original_compression)
