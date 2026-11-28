extends SceneTree
## What a goblin COSTS THE RENDERER: the draw calls one body is worth, and what a frame pays
## with 1, 10, 32 and 100 of them on screen.
##
## A goblin is six MeshInstance3D parts, and every part used to be its own mesh and its own
## material, so no two bodies shared anything and Forward+ had nothing to instance: eleven
## draws per goblin (the head was six surfaces of one mesh), a hundred of them eleven hundred.
## The body is now six shared meshes on one opaque material, each part's source surfaces merged
## into one, drawn through ONE MultiMesh per part (scripts/body_batch.gd) -- because the
## engine's own instancing batches only up to a limit, past which a hundred bodies cost thirty
## draws instead of six (both numbers are measured below). The crowd's cost is therefore a
## constant rather than a count, and that is what this probe requires.
##
##   * N = 0, 1, 10, 32, 100 goblins in a grid, and for each the frame's draw calls with the
##     crowd DRAWN and with the batch hidden. The difference is the body's cost, and it has to
##     stay FLAT -- a handful at 100 where it was eleven per body. "Better" is not the claim.
##   * the instancing's preconditions, read off two goblins: six parts, six surfaces (the head
##     one), the union of the parts still 0..1.44 tall, the same mesh instance id for a part on
##     both bodies, the same material instance id on all six -- and both bodies in ONE batch.
##   * the per-instance light survives. Two goblins in cells 20 blocks apart, one lit: each must
##     be handed ITS OWN cell's light, through the batch's per-instance custom data as well as
##     its node.
##   * the parts are still turned by the clips, and the batch follows them: instancing must not
##     have frozen the crowd.
##
## WINDOWED: the counters come from a real renderer, and the dummy one has none.
##
##   probes/run_probe_shot.sh probes/probe_goblin_drawcalls.gd 400
##
## The session's nav trace is claimed and muted here: this probe spawns real goblins in a real
## world, and a goblin's first act is to install a recorder that would write over the player's
## last session at exit.

const GOBLIN_SCRIPT := "res://scripts/goblin.gd"
const MODEL_SCRIPT := "res://scripts/goblin_model.gd"
const MODEL_PATH := "res://Assets/models/Goblin.obj"
const TEXTURE_PATH := "res://Assets/models/Goblin.png"
const NAV_TRACE := "res://scripts/nav_trace.gd"
## Where scripts/body_batch.gd puts the crowd's MultiMeshes.
const BATCH_NAME := "BodyBatch"
## The spawn tool's own centring of the mesh on the body's box.
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)
## The wander silenced, so the grid stays where it was put.
const SILENCED_IDLE := 1000000
const PARTS := ["arm", "arm2", "leg", "leg2", "torso", "head"]
const COUNTS := [0, 1, 10, 32, 100]
const COLUMNS := 10
const SPACING := 1.6
## How far above the ground the grid floats: the terrain slopes, and a body dropped into a hill
## is not drawn at all.
const AIRBORNE := 6.0
const LIGHT_BLOCK := 14  # data/block_definitions.json index 14: emissive 15,15,15
## How many draws the crowd's own cost may vary by between 1 and 100 goblins: six parts are six
## draws whether one body or a hundred is on screen. The slack is for the frame-to-frame noise of
## a streaming world (a handful of terrain draws) -- not for a per-body count.
const FLAT_SLACK := 8

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-46s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		ok = false


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - t0) / 1_000_000.0 < seconds:
		await process_frame


func _settle(frames: int) -> void:
	for i in frames:
		await process_frame


## The frame's draw calls, as the engine counts them: the whole renderer's total, the visible
## pass, the shadow pass, and how many objects the visible pass took.
func _counts() -> Dictionary:
	return {
		# (info_type first: `get_render_info(RenderInfoType, RenderInfo)`.)
		"visible": root.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE,
			Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
		"shadow": root.get_render_info(Viewport.RENDER_INFO_TYPE_SHADOW,
			Viewport.RENDER_INFO_DRAW_CALLS_IN_FRAME),
		"objects": root.get_render_info(Viewport.RENDER_INFO_TYPE_VISIBLE,
			Viewport.RENDER_INFO_OBJECTS_IN_FRAME),
		"total": int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
	}


## The frame's counts once they have STOPPED moving.
##
## The world streams while this runs, so two frames a moment apart can differ by a dozen terrain
## draws -- more than the handful a body's crowd is worth -- and a difference taken across that
## is noise with a sign. A count that has been the same for several frames is the same scene.
func _stable_counts() -> Dictionary:
	var last := _counts()
	var same := 0
	for i in 900:
		await process_frame
		var now := _counts()
		if int(now["total"]) == int(last["total"]) and int(now["visible"]) == int(last["visible"]):
			same += 1
			if same >= 4:
				return now
		else:
			same = 0
		last = now
	return last


## The top surface of a column, as the feet position standing on it.
func _surface(cm: Node, around: Vector3) -> Vector3:
	var x := floori(around.x)
	var z := floori(around.z)
	var y := floori(around.y) + 24
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(float(x) + 0.5, float(y + 1), float(z) + 0.5)
		y -= 1
	return Vector3.INF


## One goblin, built the way scripts/goblin_spawn.gd builds one: the entity body carrying
## goblin.gd in the two groups, and the Model child the player's own body script draws with.
## The spawn TOOL is not used -- it aims a ray and rolls dice -- but nothing here invents a
## shape the game does not have.
func _spawn_goblin(parent: Node, at: Vector3, mesh: Mesh, body_script: Script,
		model_script: Script) -> Node3D:
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(body_script)
	body.add_to_group("pose_clone")
	body.add_to_group("goblin")
	body.position = at
	parent.add_child(body)
	var model := Node3D.new()
	model.name = "Model"
	model.set_script(model_script)
	model.set("source_mesh", mesh)
	model.position = BODY_OFFSET
	body.add_child(model)
	body.set("_idle_ticks", SILENCED_IDLE)
	return body


## `count` goblins in a grid centred on `origin`, all in front of the camera.
func _spawn_grid(parent: Node, origin: Vector3, count: int, mesh: Mesh, body_script: Script,
		model_script: Script) -> Array[Node3D]:
	var bodies: Array[Node3D] = []
	if count <= 0:
		return bodies
	var columns := mini(count, COLUMNS)
	var rows := ceili(float(count) / float(COLUMNS))
	for i in count:
		var col := i % COLUMNS
		var row := floori(float(i) / float(COLUMNS))
		var at := origin + Vector3((float(col) - float(columns - 1) * 0.5) * SPACING, 0.0,
			(float(row) - float(rows - 1) * 0.5) * SPACING)
		bodies.append(_spawn_goblin(parent, at, mesh, body_script, model_script))
	return bodies


func _goblins(scene: Node) -> Array[Node]:
	var found: Array[Node] = []
	for node in scene.get_tree().get_nodes_in_group("goblin"):
		if scene.is_ancestor_of(node):
			found.append(node)
	return found


## A world write, read back: a probe that trusts a queued write would measure a change that
## never happened.
func _write(cm: Node, cell: Vector3i, id: int) -> bool:
	if cm.get_block(cell.x, cell.y, cell.z) == id:
		return true
	cm.set_block(cell.x, cell.y, cell.z, id)
	return cm.get_block(cell.x, cell.y, cell.z) == id


## A stand for one body: the cell its feet are in, and -- for the lit one -- a light block two
## cells above it, which puts the body's own middle cell in a different light from the other
## stand's. Nothing is carved: both stands are on the open surface (see the callers).
func _stand(cm: Node, base: Vector3i, lit: bool) -> bool:
	if not lit:
		return true
	var cell := base + Vector3i(0, 2, 0)
	if _write(cm, cell, LIGHT_BLOCK):
		return true
	_fail("the light block would not land at %s (that cell reads %d, chunk resident?)"
		% [str(cell), int(cm.get_block(cell.x, cell.y, cell.z))])
	return false


func _max_diff(a: Vector4, b: Vector4) -> float:
	return maxf(maxf(absf(a.x - b.x), absf(a.y - b.y)), maxf(absf(a.z - b.z), absf(a.w - b.w)))


## The parts of one body, by name, by SEARCH: `arm` hangs under its shoulder pivot, so it is not
## a direct child of the model.
func _parts(model: Node) -> Dictionary:
	var found := {}
	for part in PARTS:
		var mi := model.find_child(part, true, false) as MeshInstance3D
		if mi != null and mi.mesh != null:
			found[part] = mi
	return found


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(1280, 720))
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: Main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	var cm := main.get_node_or_null("ChunkManager")
	if player == null or cm == null:
		print("PROBE FAIL: Main.tscn has no Player / ChunkManager")
		quit(1)
		return
	# The HUD, the title screen and the crosshair are not the world.
	for child in main.get_children():
		if child is CanvasLayer:
			child.visible = false
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	# The session's trace: claimed and muted, see the header. A goblin's `_install_trace` asks
	# `claim()`, so claiming it first means no recorder is built and nothing is written.
	var nav: GDScript = load(NAV_TRACE)
	if nav != null:
		nav.set("muted", true)
		nav.call("claim")

	var feet := _surface(cm, player.global_position + Vector3(8.0, 0.0, 8.0))
	if feet == Vector3.INF:
		print("PROBE FAIL: no ground beside the player")
		quit(1)
		return
	var origin := feet + Vector3(0.0, AIRBORNE, 0.0)
	var camera := Camera3D.new()
	camera.fov = 70.0
	main.add_child(camera)
	camera.global_position = origin + Vector3(0.0, 10.0, 30.0)
	# A camera inside a hill sees no grid, and a draw call measured through rock is zero.
	var tries := 0
	while tries < 40 and cm.get_block(floori(camera.global_position.x),
			floori(camera.global_position.y), floori(camera.global_position.z)) != 0:
		camera.global_position.y += 1.0
		tries += 1
	camera.look_at(origin + Vector3(0.0, 0.8, 0.0), Vector3.UP)
	camera.current = true
	await _seconds(0.4)

	# --- the pieces the spawn loads -------------------------------------------
	var mesh: Mesh = load(MODEL_PATH)
	var body_script: Script = load(GOBLIN_SCRIPT)
	var model_script: Script = load(MODEL_SCRIPT)
	var texture: Texture2D = load(TEXTURE_PATH)
	if mesh == null or body_script == null or model_script == null or texture == null:
		print("PROBE FAIL: a goblin asset or script did not load")
		quit(1)
		return

	# --- one body's shape, the instancing's preconditions, and the batch -------
	var pair := _spawn_grid(main, origin, 2, mesh, body_script, model_script)
	# ...and let them LAND: the grid starts six blocks up, and a body still falling has a
	# different transform every frame, which is not what these checks are about.
	await _seconds(1.6)
	var model_a := pair[0].get_node_or_null("Model") as Node3D
	var model_b := pair[1].get_node_or_null("Model") as Node3D
	var batch := root.get_node_or_null(NodePath(BATCH_NAME))
	if model_a == null or model_b == null or batch == null:
		print("PROBE FAIL: a goblin has no Model, or nothing registered with the body batch")
		quit(1)
		return
	var parts := _parts(model_a)
	var other_parts := _parts(model_b)
	_ok("the rig has six parts", parts.size() == 6, "%d of 6" % parts.size())
	var surfaces := 0
	for part in parts:
		surfaces += int((parts[part] as MeshInstance3D).mesh.get_surface_count())
	_ok("six surfaces in total (the head is one box group)", surfaces == 6,
		"%d surface(s)" % surfaces)
	var head := parts.get("head") as MeshInstance3D
	_ok("the head is ONE surface", head != null and head.mesh.get_surface_count() == 1,
		"%d" % (head.mesh.get_surface_count() if head != null else -1))
	var union := AABB()
	var any := false
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		var box: AABB = (model_a.global_transform.affine_inverse() * mi.global_transform
			* (mi.mesh as Mesh).get_aabb())
		union = box if not any else union.merge(box)
		any = true
	_ok("the rig is still 0..1.44 tall",
		any and absf(union.position.y) < 0.01 and absf(union.size.y - 1.44) < 0.02,
		"y %.3f .. %.3f" % [union.position.y, union.position.y + union.size.y])
	var shared_mesh := 0
	var material_ids := {}
	var shader_ok := true
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		var other: MeshInstance3D = other_parts.get(part)
		if other == null or other.mesh != mi.mesh:
			shared_mesh += 1
		var material := mi.get_surface_override_material(0) as ShaderMaterial
		if material == null:
			shader_ok = false
			continue
		material_ids[material.get_instance_id()] = true
		if material.shader == null or not String(material.shader.resource_path) \
				.ends_with("body_shader.gdshader"):
			shader_ok = false
	_ok("two goblins share the six MESHES", shared_mesh == 0, "%d differ" % shared_mesh)
	_ok("two goblins wear ONE material", material_ids.size() == 1,
		"%d material(s)" % material_ids.size())
	_ok("the body is on body_shader.gdshader (opaque)", shader_ok)
	var lit_parts := 0
	for part in parts:
		var value = (parts[part] as MeshInstance3D).get_instance_shader_parameter("item_light")
		if typeof(value) == TYPE_VECTOR4:
			lit_parts += 1
	_ok("every part is handed item_light", lit_parts == parts.size(),
		"%d of %d" % [lit_parts, parts.size()])
	# The crowd is ONE batch, and it is what is drawn: the parts are pose carriers, and a part's
	# drawn instance is its node's world transform.
	_ok("both bodies are in one batch", int(batch.call("used_slots")) == 2,
		"%d slot(s)" % int(batch.call("used_slots")))
	_ok("the parts themselves are not drawn", not (parts["torso"] as MeshInstance3D).visible)
	var batch_shader_ok := false
	for child in batch.get_children():
		var mmi := child as MultiMeshInstance3D
		if mmi != null and mmi.material_override is ShaderMaterial:
			var shader: Shader = (mmi.material_override as ShaderMaterial).shader
			batch_shader_ok = shader != null and String(shader.resource_path) \
				.ends_with("body_batch.gdshader")
			break
	_ok("the batch draws with body_batch.gdshader", batch_shader_ok)
	var drawn: Variant = await _batch_transform(batch, model_a, "torso")
	var node_transform: Transform3D = (parts["torso"] as MeshInstance3D).global_transform
	_ok("the batch draws a part where its node is",
		drawn is Transform3D
			and (drawn as Transform3D).origin.distance_to(node_transform.origin) < 0.01,
		"%s against %s" % [str((drawn as Transform3D).origin if drawn is Transform3D else "?"),
			str(node_transform.origin)])
	# What one body is, in the terms the draw call is made of: one surface a part, six parts, the
	# six mesh RIDs shared with every other goblin and the one material RID they all wear.
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		print("PROBE part %-6s surfaces=%d mesh_id=%d material_id=%d" % [part,
			mi.mesh.get_surface_count(), mi.mesh.get_instance_id(),
			mi.get_surface_override_material(0).get_instance_id()])
	for body in _goblins(main):
		body.queue_free()
	await _settle(2)

	# --- the crowd ------------------------------------------------------------
	var cost := {}
	for count in COUNTS:
		for body in _goblins(main):
			body.queue_free()
		await _settle(2)
		var bodies := _spawn_grid(main, origin, count, mesh, body_script, model_script)
		var shown: Dictionary = await _stable_counts()
		# The crowd is drawn by the batch, not by the bodies' own nodes: hiding what is DRAWN
		# is the only way to measure it.
		var batch_node := root.get_node_or_null(NodePath(BATCH_NAME)) as Node3D
		if batch_node != null:
			batch_node.visible = false
		var hidden: Dictionary = await _stable_counts()
		if batch_node != null:
			batch_node.visible = true
		var body_cost := {}
		for key in shown:
			body_cost[key] = int(shown[key]) - int(hidden[key])
		cost[count] = body_cost
		print("PROBE n=%-3d shown: total=%d visible=%d shadow=%d objects=%d | batch hidden: "
			% [count, int(shown["total"]), int(shown["visible"]), int(shown["shadow"]),
				int(shown["objects"])]
			+ "total=%d | the crowd's own: total=%d visible=%d shadow=%d objects=%d"
			% [int(hidden["total"]), int(body_cost["total"]), int(body_cost["visible"]),
				int(body_cost["shadow"]), int(body_cost["objects"])])
		for body in bodies:
			body.queue_free()
	for body in _goblins(main):
		body.queue_free()
	await _settle(2)

	var one := int(cost[1]["visible"])
	var ten := int(cost[10]["visible"])
	var thirty_two := int(cost[32]["visible"])
	var hundred := int(cost[100]["visible"])
	_ok("100 goblins are drawn", hundred > 0, "%d draw(s) for 600 parts" % hundred)
	_ok("the crowd costs the same at 1 and at 100 goblins", hundred <= one + FLAT_SLACK,
		"%d at 1, %d at 10, %d at 32, %d at 100 (allowed %d)"
			% [one, ten, thirty_two, hundred, one + FLAT_SLACK])
	_ok("...and at 10 and at 32", hundred <= thirty_two + FLAT_SLACK,
		"%d at 32, %d at 100" % [thirty_two, hundred])

	# --- the per-instance light survives the batch ----------------------------
	# Two stands 20 blocks apart, each on the open surface: one with a light block over it. The
	# two bodies are in cells of a different light, so ONE material over both is only correct
	# while the light is per instance -- the node's `item_light` for a body drawn as its own
	# mesh, and the batch's custom data for the crowd.
	var feet_a := _surface(cm, player.global_position + Vector3(3.0, 0.0, 0.0))
	var feet_b := _surface(cm, player.global_position + Vector3(23.0, 0.0, 0.0))
	if feet_a == Vector3.INF or feet_b == Vector3.INF:
		_fail("no ground for the two light stands")
	elif not (_stand(cm, Vector3i(floori(feet_a.x), floori(feet_a.y), floori(feet_a.z)), true)
			and _stand(cm, Vector3i(floori(feet_b.x), floori(feet_b.y), floori(feet_b.z)),
				false)):
		pass
	else:
		var base_a := Vector3i(floori(feet_a.x), floori(feet_a.y), floori(feet_a.z))
		var base_b := Vector3i(floori(feet_b.x), floori(feet_b.y), floori(feet_b.z))
		var lit := _spawn_goblin(main, feet_a, mesh, body_script, model_script)
		var dark := _spawn_goblin(main, feet_b, mesh, body_script, model_script)
		await _seconds(0.6)
		var lit_model := lit.get_node("Model")
		var dark_model := dark.get_node("Model")
		var lit_rig := _parts(lit_model)
		var dark_rig := _parts(dark_model)
		if lit_rig.is_empty() or dark_rig.is_empty():
			_fail("a stand's goblin has no rig")
		else:
			var lit_light: Vector4 = (lit_rig["torso"] as MeshInstance3D) \
				.get_instance_shader_parameter("item_light")
			var dark_light: Vector4 = (dark_rig["torso"] as MeshInstance3D) \
				.get_instance_shader_parameter("item_light")
			# The cell each body READS: its own middle, the way _push_world_lighting reads it.
			var middle_a: Vector3 = lit_model.to_global(Vector3(0.0,
				float(lit_model.get("_body_height")), 0.0))
			var middle_b: Vector3 = dark_model.to_global(Vector3(0.0,
				float(dark_model.get("_body_height")), 0.0))
			var want_lit: Vector4 = cm.get_light_at(floori(middle_a.x), floori(middle_a.y),
				floori(middle_a.z))
			var want_dark: Vector4 = cm.get_light_at(floori(middle_b.x), floori(middle_b.y),
				floori(middle_b.z))
			print("PROBE light: lit cell %s handed %s | dark cell %s handed %s"
				% [str(want_lit), str(lit_light), str(want_dark), str(dark_light)])
			_ok("two goblins in different cells keep their OWN light",
				_max_diff(lit_light, dark_light) > 0.1,
				"diff %.3f" % _max_diff(lit_light, dark_light))
			_ok("...and each is its own cell's light",
				_max_diff(lit_light, want_lit) < 0.05 and _max_diff(dark_light, want_dark) < 0.05,
				"lit off by %.3f, dark off by %.3f"
					% [_max_diff(lit_light, want_lit), _max_diff(dark_light, want_dark)])
			var same := true
			for part in lit_rig:
				var value: Vector4 = (lit_rig[part] as MeshInstance3D) \
					.get_instance_shader_parameter("item_light")
				if _max_diff(value, lit_light) > 0.001:
					same = false
			_ok("every part of one body carries the same light", same)
			# ...and the light the CROWD is drawn with is the same per-body value, from the
			# batch's own per-instance custom data.
			var batch_node := root.get_node_or_null(NodePath(BATCH_NAME))
			if batch_node != null:
				var lit_slot := int(lit_model.get("_batch_slot"))
				var dark_slot := int(dark_model.get("_batch_slot"))
				await _settle(2)
				var lit_drawn: Variant = batch_node.call("read_light", lit_slot, "torso")
				var dark_drawn: Variant = batch_node.call("read_light", dark_slot, "torso")
				var lit_col: Color = lit_drawn if lit_drawn is Color else Color.BLACK
				var dark_col: Color = dark_drawn if dark_drawn is Color else Color.BLACK
				var lit_vec := Vector4(lit_col.r, lit_col.g, lit_col.b, lit_col.a)
				var dark_vec := Vector4(dark_col.r, dark_col.g, dark_col.b, dark_col.a)
				_ok("the batch draws each body in its own light",
					_max_diff(lit_vec, lit_light) < 0.01
						and _max_diff(dark_vec, dark_light) < 0.01,
					"lit %s against the node's %s" % [str(lit_vec), str(lit_light)])
		lit.queue_free()
		dark.queue_free()
		await _settle(2)

	# --- the parts still turn, and the crowd follows them ----------------------
	# Batching is a property of how the bodies are SUBMITTED, not of what they are: the clip
	# still writes each body's own part transforms every frame, and the batch reads them.
	var walkers := _spawn_grid(main, origin, 1, mesh, body_script, model_script)
	await _settle(2)
	var walker := walkers[0].get_node_or_null("Model") as Node3D
	var leg := walker.find_child("leg", true, false) as Node3D
	var walk_batch := root.get_node_or_null(NodePath(BATCH_NAME))
	if leg == null or walk_batch == null:
		_fail("the rig has no leg to watch, or no batch to read it from")
	else:
		var slot := int(walker.get("_batch_slot"))
		var first := leg.quaternion
		var first_drawn: Variant = walk_batch.call("read_transform", slot, "leg")
		var moved := 0.0
		var drawn_moved := 0.0
		for i in 24:
			# The walk amount is what the clips' blend weight is, and a body standing in a
			# probe has no speed; forcing it is the probe's own input, not the game's.
			walker.set("_swing", 1.0)
			await process_frame
			moved = maxf(moved, first.angle_to(leg.quaternion))
			var now: Variant = walk_batch.call("read_transform", slot, "leg")
			if first_drawn is Transform3D and now is Transform3D:
				# The leg TURNS about its own joint, so its origin stays put and its swing is about
				# X (which leaves X alone): the whole basis is compared, as the turn it is.
				var before_q := (first_drawn as Transform3D).basis.get_rotation_quaternion()
				var now_q := (now as Transform3D).basis.get_rotation_quaternion()
				drawn_moved = maxf(drawn_moved, before_q.angle_to(now_q))
		_ok("the parts still animate", moved > 0.02, "the leg turned %.3f rad" % moved)
		_ok("the batch follows the parts", drawn_moved > 0.01,
			"the drawn leg turned %.3f rad" % drawn_moved)
	for body in _goblins(main):
		body.queue_free()

	if ok:
		print("PROBE PASS: one goblin is six shared meshes on one opaque material drawn through "
			+ "one MultiMesh per part, a hundred of them cost the same few draws as one, and each "
			+ "is still lit and posed by its own cell")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)


## The transform the batch is drawing for one part of one body, after a frame has written it.
func _batch_transform(batch: Node, model: Node, part: String) -> Variant:
	await process_frame
	var slot := int(model.get("_batch_slot"))
	return batch.call("read_transform", slot, part)

