extends SceneTree
## Does a goblin's offhand torch actually light the world?
##
## Three claims, all in the real world and all at night, where the sun cannot hide the
## answer:
##
##   * the GROUND under the torch is brighter than the same ground with the same goblin
##     empty-handed -- read off the rendered frame, the way probes/probe_body_light.gd
##     reads the body. The goblin's own model is hidden for both frames, so the only
##     thing that differs between them is the light it carries: no body, no torch mesh,
##     just terrain.
##   * the set the world is fed is the right one: one light, at the DRAWN torch rather
##     than at the body's feet, with the item's own tint at 14/15 -- read by putting a
##     scratch material through ChunkManager.apply_item_lighting, which hands over the
##     same cached values the terrain, water, items and bodies all receive.
##   * a torch-bearer out of range drops the count back to 0.
##
## Not checked: occlusion. The glow is a shader term and a torch behind a wall lights the
## near face; that limit is written down in docs/rendering-notes.md rather than tested,
## because the test would be of a raycast nobody wants to pay for.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_entity_light.gd 400

const GOBLIN_SCRIPT := "res://scripts/goblin.gd"
const MODEL_SCRIPT := "res://scripts/goblin_model.gd"
const MODEL_PATH := "res://Assets/models/Goblin2.obj"
const ITEMS_JSON := "res://data/items.json"
## The spawn tool's own centring of the mesh on the body's box.
const BODY_OFFSET := Vector3(0.031, 0.0, 0.094)
## Standing still: the wander is silenced so the torch stays where it was put.
const SILENCED_IDLE := 1000000
## Midnight, through the day/night uniforms the terrain is lit by.
const NIGHT_TIME := 0.0
## Sampling: every Nth pixel of a box around the projected ground point.
const STRIDE := 2
const REGION_HALF := 56
## A sampled pixel this much brighter counts as lit; the mean and the number of those
## pixels both have to move, so a stray bright texel cannot pass the check alone.
const BRIGHT_STEP := 0.03
const MIN_BRIGHT_SHARE := 0.05
const MEAN_STEP := 0.02

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - t0) / 1_000_000.0 < seconds:
		await process_frame


func _frame() -> Image:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return null
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _luma(data: PackedByteArray, o: int) -> float:
	return (0.299 * float(data[o]) + 0.587 * float(data[o + 1]) + 0.114 * float(data[o + 2])) / 255.0


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


## The torch's own light entry out of data/items.json, so this checks the registry's
## resolution against the file rather than against a second copy of the numbers.
func _torch_light() -> Dictionary:
	var file := FileAccess.open(ITEMS_JSON, FileAccess.READ)
	if file == null:
		return {}
	var json := JSON.new()
	var text := file.get_as_text()
	file.close()
	if json.parse(text) != OK or not (json.data is Dictionary):
		return {}
	for entry in (json.data as Dictionary).get("items", []):
		if String(entry.get("name", "")) == "torch":
			return entry.get("light", {})
	return {}


## What the world is currently fed, read the way probes/probe_body_light.gd reads the sky
## warmth: a scratch material through the same call every item and body material gets.
func _entity_lights(cm: Node) -> Dictionary:
	var scratch := ShaderMaterial.new()
	scratch.shader = load("res://Assets/shaders/item_shader.gdshader")
	cm.apply_item_lighting(scratch)
	return {
		"count": int(scratch.get_shader_parameter("entity_light_count")),
		"positions": scratch.get_shader_parameter("entity_light_positions"),
		"colors": scratch.get_shader_parameter("entity_light_colors"),
		"radius": float(scratch.get_shader_parameter("entity_light_radius")),
	}


## Mean luma difference over a box of the frame, and how many sampled pixels are clearly
## brighter in `a` -- the lit frame -- than in `b`.
func _compare(a: Image, b: Image, centre: Vector2i) -> Dictionary:
	if a == null or b == null:
		return {}
	var w := a.get_width()
	var h := a.get_height()
	var da := a.get_data()
	var db := b.get_data()
	var total := 0.0
	var bright := 0
	var samples := 0
	var x0 := maxi(centre.x - REGION_HALF, 0)
	var x1 := mini(centre.x + REGION_HALF, w)
	var y0 := maxi(centre.y - REGION_HALF, 0)
	var y1 := mini(centre.y + REGION_HALF, h)
	for y in range(y0, y1, STRIDE):
		for x in range(x0, x1, STRIDE):
			var o := (y * w + x) * 4
			var delta := _luma(da, o) - _luma(db, o)
			total += delta
			if delta > BRIGHT_STEP:
				bright += 1
			samples += 1
	if samples == 0:
		return {}
	return {"mean": total / float(samples), "bright": bright, "samples": samples}


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
	# The HUD, the title screen and the crosshair are not the world: hiding them keeps a
	# scrim or a panel out of the pixels this measures.
	for child in main.get_children():
		if child is CanvasLayer:
			child.visible = false
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	var torch := BlockTextures.get_block_id_by_name("torch")
	var torch_light := _torch_light()
	if torch <= 0 or torch_light.is_empty():
		print("PROBE FAIL: `torch` does not resolve to a lit item")
		quit(1)
		return

	# Night, pinned through the engine's own default length so a settings change cannot
	# shorten it under the measurements.
	cm.set("day_night_cycle_enabled", true)
	cm.set("day_duration", cm.get_default_day_duration())
	cm.set("day_time", NIGHT_TIME)
	await _seconds(0.5)

	# A goblin on the ground a few blocks from the player, with a camera of our own so the
	# ground under it is on screen whatever the player happened to be looking at.
	var feet := _surface(cm, player.global_position + Vector3(4.0, 0.0, 0.0))
	if feet == Vector3.INF:
		print("PROBE FAIL: no ground beside the player")
		quit(1)
		return
	var cam_at := feet + Vector3(-5.0, 2.5, 0.0)
	var tries := 0
	while tries < 24 and cm.get_block(floori(cam_at.x), floori(cam_at.y), floori(cam_at.z)) != 0:
		cam_at.y += 1.0
		tries += 1
	var camera := Camera3D.new()
	camera.fov = 70.0
	main.add_child(camera)
	camera.global_position = cam_at
	camera.look_at(feet + Vector3(0.0, 0.9, 0.0), Vector3.UP)
	camera.current = true
	await _seconds(0.3)

	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(GOBLIN_SCRIPT))
	body.add_to_group("goblin")
	body.position = feet
	main.add_child(body)
	body.set("_idle_ticks", SILENCED_IDLE)
	var model := Node3D.new()
	model.name = "Model"
	model.set_script(load(MODEL_SCRIPT))
	model.set("source_mesh", load(MODEL_PATH))
	model.position = BODY_OFFSET
	body.add_child(model)
	body.set("offhand_item", torch)
	await _seconds(0.4)

	# The hand it is drawn in, proved against the mesh rather than against the seat's own
	# numbers: the goblin looks down its own -Z and `arm` is the -x side, so in the MODEL's
	# space the torch has to be in FRONT (z < 0) and on the LEFT (x < 0), with the card's up
	# pointing where the goblin faces. A seat copied straight from the player's +Z-facing rig
	# lands behind the hand with the sprite turned round -- player_model.gd's HELD_MIRROR_YAW
	# is the mirror -- and this is the one probe with a real torch to check it on.
	var drawn: Vector3 = body.call("get_offhand_position")
	if int(body.call("get_offhand_item")) != torch:
		_fail("the body reports %d as its offhand, not the torch %d"
			% [int(body.call("get_offhand_item")), torch])
	var offhand := model.find_child("OffhandItem", true, false) as MeshInstance3D
	if offhand == null or not offhand.visible:
		_fail("a goblin handed a torch draws no OffhandItem under its left arm")
	else:
		var local := model.global_transform.affine_inverse() * offhand.global_position
		if local.z >= 0.0:
			_fail("the torch sits at model z=%.3f: behind the goblin, not in front" % local.z)
		if local.x >= 0.0:
			_fail("the torch sits at model x=%.3f: on the right hand, not the left" % local.x)
		if drawn.distance_to(offhand.global_position) > 0.001:
			_fail("the body reports the torch at %s, drawn at %s" % [drawn, offhand.global_position])
		var facing := -model.global_transform.basis.z.normalized()
		if offhand.global_transform.basis.y.dot(facing) <= 0.0:
			_fail("the torch is drawn upside down (its top points behind the goblin)")
		if offhand.material_override == null or not String(
				(offhand.material_override as ShaderMaterial).shader.resource_path).ends_with("item_shader.gdshader"):
			_fail("the torch is not on the world's item shader, so the world's light misses it")
	var fed: Dictionary = _entity_lights(cm)
	print("PROBE fed: count=%d positions=%s colors=%s radius=%.2f"
		% [int(fed.get("count", -1)), str(fed.get("positions", [])),
			str(fed.get("colors", [])), float(fed.get("radius", 0.0))])
	if int(fed.get("count", 0)) != 1:
		_fail("the world is fed %d entity lights for one torch-bearer" % int(fed.get("count", 0)))
	else:
		var positions: PackedVector3Array = fed["positions"]
		var colors: PackedVector3Array = fed["colors"]
		if positions[0].distance_to(drawn) > 0.01:
			_fail("the light is at %s but the torch is drawn at %s" % [positions[0], drawn])
		var level := float(torch_light.get("level", 0)) / 15.0
		var tint: Array = torch_light.get("color", [1.0, 1.0, 1.0])
		var want := Vector3(float(tint[0]), float(tint[1]), float(tint[2])) * level
		if not colors[0].is_equal_approx(want):
			_fail("the light's colour is %s, the item's own %s * %d/15 is %s"
				% [colors[0], str(tint), int(torch_light.get("level", 0)), want])
		# Half the torch's level, which is also what the player's glow takes half of: the same
		# item must light the same pool in either hand.
		if absf(float(fed.get("radius", 0.0)) - 7.0) > 0.01:
			_fail("the light's radius is %.2f, expected the shaders' 7.0" % float(fed["radius"]))

	# The frames. The model is hidden in both, so the only difference is the light itself:
	# no body, no torch mesh, just the ground it is carried over.
	var ground := feet + Vector3(0.0, 0.15, 0.0)
	var centre := Vector2i(camera.unproject_position(ground))
	model.visible = false
	await _seconds(0.4)
	var armed_frame := _frame()
	body.set("offhand_item", 0)
	await _seconds(0.5)
	var empty_frame := _frame()
	model.visible = true
	if armed_frame == null or empty_frame == null:
		_fail("no framebuffer to read (run this windowed: probes/run_probe_shot.sh)")
	else:
		var cmp: Dictionary = _compare(armed_frame, empty_frame, centre)
		if cmp.is_empty():
			_fail("the measured region is off the frame")
		else:
			var share := float(cmp["bright"]) / maxf(float(cmp["samples"]), 1.0)
			print("PROBE ground under the torch: mean +%.4f, %d of %d samples clearly "
				% [float(cmp["mean"]), int(cmp["bright"]), int(cmp["samples"])]
				+ "brighter with the torch (%.1f%%)" % (100.0 * share))
			if float(cmp["mean"]) < MEAN_STEP or share < MIN_BRIGHT_SHARE:
				_fail("the ground does not get brighter with the torch (mean +%.4f, %.1f%% of samples)"
					% [float(cmp["mean"]), 100.0 * share])
		# With the hand empty the set has to be empty again -- and nothing drawn.
		if offhand != null and offhand.visible:
			_fail("an empty-handed goblin still draws its torch")
		var after: Dictionary = _entity_lights(cm)
		if int(after.get("count", -1)) != 0:
			_fail("an empty-handed goblin still feeds %d lights" % int(after.get("count", -1)))

	# ...and DISTANCE is not the cull: a torch 25 blocks down the view is still lighting ground
	# the camera can see, which is the bug that had a third-person player watch the light go
	# out five blocks away (the gather used to drop anything further than its radius from the
	# CAMERA, and the camera sits four or five blocks behind the player). The cull is the
	# frustum -- the torch's own 7-block sphere against it.
	body.set("offhand_item", torch)
	await _seconds(0.3)
	if int(_entity_lights(cm).get("count", 0)) != 1:
		_fail("the torch did not come back when it was handed over again")
	body.call("teleport", feet + Vector3(25.0, 0.0, 0.0))
	await _seconds(0.3)
	if int(_entity_lights(cm).get("count", -1)) != 1:
		_fail("a torch 25 blocks down the view feeds no light: the cull is a distance again")
	# Behind the camera it reaches nothing on screen, so it is not gathered at all.
	body.call("teleport", feet + Vector3(-40.0, 0.0, 0.0))
	await _seconds(0.3)
	if int(_entity_lights(cm).get("count", -1)) != 0:
		_fail("a torch 40 blocks BEHIND the camera still feeds a light")

	# Put the day back: this probe runs windowed against the player's own world.
	cm.set("day_time", 0.5)
	print("PROBE PASS: the torch lights the ground it is carried over, its light is the "
		+ "drawn torch's own colour, and the set follows the body in and out of range"
		if ok else "PROBE FAILED")
	quit(0 if ok else 1)
