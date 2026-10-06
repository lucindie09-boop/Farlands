extends SceneTree
## Is the player's own body lit by the WORLD's light, like the ground it stands on?
##
## The body used to be an ordinary StandardMaterial3D: the engine's sky and
## ambient, which know nothing about the cell the body is in. This scene has no
## light node at all -- only a sky ambient at 0.027 energy -- so a body left on
## the engine's lighting is nearly black while the terrain beside it is bright,
## and a body running the world's own model (shaders/item_lighting.gdshaderinc,
## through player_model.gd) matches it.
##
## Three claims, all of them about the body:
##
##   * the body's own mean luma is the mean luma of the pixels it is drawn OVER,
##     in the same frame -- an item can never disagree with the ground it is
##     standing on. Engine lighting is a factor of ten out here (0.027 ambient).
##   * the body follows the SUN down. Midnight is asked for through the same
##     day/night uniforms the terrain is fed, which is a change the engine cannot
##     produce AT ALL in this scene: there is nothing for it to move. A body on
##     the engine's lighting reads identically at noon and at midnight.
##   * the body does NOT wear the world's midday tint, and the world still does.
##     The sky half of the light is tinted by `sky_light_warmth`, which the world
##     is handed as a warm CREAM even at noon: a cast that hides in grass, dirt
##     and stone, and reads as orange on a pale skin -- the one sky-lit surface
##     with no texture to hide it behind. So the body's own material is handed the
##     sun's own colour instead, WHITE overhead, while the world keeps the cream.
##     Both ends are read off the live materials at noon, which is the hour the
##     cast is at full strength; the horizon is checked too, because the body's
##     white is a curve and not a flat colour (a flat white would take the sunset
##     out of it).
##
## The silhouette comes from hiding the body: the only thing that differs between
## a frame with it and a frame without it is whether it was drawn. The mask is
## taken ONCE and reused for every measurement, because a mask re-derived inside a
## brighter or darker world keeps only the body's pixels that still contrast with
## it -- the dark ones at noon, the bright ones at midnight -- and reads a step
## that is not there.
##
## Nothing here writes to the world: the light steps are uniforms.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_body_light.gd 500
##
## Shots go to user://body_shots/ (noon.png, midnight.png) for the eye.

const STRIDE := 4              # sample every Nth pixel; the means do not need all of them
const BODY_DELTA := 0.12       # a sampled pixel is the body above this much change
const MIN_BODY_SHARE := 0.01   # ...and the body has to be a real part of the frame
const MATCH_TOLERANCE := 0.18  # body vs the world it is drawn over, in the same frame
const NIGHT_BLEND := 0.5       # the body must be down to this share of noon, at most
const NOON := 0.5              # day_time: the sun highest (get_sun_elevation == 1)
const HORIZON_TIME := 0.25     # day_time: the sun ON the horizon (elevation == 0)
const MIDNIGHT := 0.0          # day_time: the sun lowest (elevation == -1)
const SLOW_DAY := 600.0        # the settings slider's own maximum, see _run
const WHITE_TOL := 0.02        # "white overhead" -- all three channels this close
const CREAM_TOL := 0.02        # the world's own midday cast, per channel
const WORLD_CREAM_G := 0.85    # DayNightCycle::get_sun_color() at the zenith
const WORLD_CREAM_B := 0.60

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


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


## A frame with the body drawn and the same frame with it hidden.
func _pair(model: Node3D) -> Array:
	model.visible = true
	await _seconds(0.2)
	var shown := _frame()
	model.visible = false
	await _seconds(0.2)
	var empty := _frame()
	model.visible = true
	return [shown, empty]


## The body's silhouette: the sampled byte offsets that are the body and nothing
## else, because nothing else about the frame changed.
func _body_mask(model: Node3D) -> PackedInt32Array:
	var offsets := PackedInt32Array()
	var pair: Array = await _pair(model)
	var shown: Image = pair[0]
	var empty: Image = pair[1]
	if shown == null or empty == null:
		return offsets
	var w := shown.get_width()
	var h := shown.get_height()
	var a := shown.get_data()
	var b := empty.get_data()
	var threshold := BODY_DELTA * 255.0
	for y in range(0, h, STRIDE):
		for x in range(0, w, STRIDE):
			var o := (y * w + x) * 4
			var d := maxf(maxf(absf(float(a[o]) - float(b[o])), absf(float(a[o + 1]) - float(b[o + 1]))),
				absf(float(a[o + 2]) - float(b[o + 2])))
			if d > threshold:
				offsets.append(o)
	return offsets


## The body's own mean luma, and the mean luma of the world it is drawn over, over
## a mask that is the same for every measurement.
func _measure(model: Node3D, offsets: PackedInt32Array) -> Dictionary:
	if offsets.is_empty():
		return {}
	var pair: Array = await _pair(model)
	var shown: Image = pair[0]
	var empty: Image = pair[1]
	if shown == null or empty == null:
		return {}
	var a := shown.get_data()
	var b := empty.get_data()
	var body_sum := 0.0
	var world_sum := 0.0
	for o in offsets:
		body_sum += _luma(a, o)
		world_sum += _luma(b, o)
	var n := float(offsets.size())
	return {"body": body_sum / n, "world": world_sum / n, "samples": offsets.size()}


## What the WORLD's materials are handed as `sky_light_warmth`, read off the call
## they are fed through -- a scratch material, so the probe cannot disturb a real
## one. apply_item_lighting is the terrain material's own feed, from the same
## cached value, so this is the number the ground is lit with.
func _world_warmth(cm: Node) -> Color:
	var scratch := ShaderMaterial.new()
	scratch.shader = load("res://shaders/item_shader.gdshader")
	cm.apply_item_lighting(scratch)
	return scratch.get_shader_parameter("sky_light_warmth")


func _shot(img: Image, name: String) -> void:
	if img == null:
		return
	DirAccess.make_dir_recursive_absolute("user://body_shots")
	img.save_png("user://body_shots/%s.png" % name)


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(1280, 720))
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)

	var player: Node3D = main.get_node_or_null("Player")
	var cm: Node = main.get_node_or_null("ChunkManager")
	if player == null or cm == null:
		print("PROBE FAIL: main.tscn is missing Player / ChunkManager")
		quit(1)
		return

	for i in range(1800):
		await process_frame
		if player.has_method("is_on_floor") and player.is_on_floor():
			break
	await _seconds(1.0)

	# The body is only drawn in the third-person views; 1 is the one behind the
	# player, which is what F5 shows first.
	player.call("set_third_person_view", 1)
	await _seconds(1.0)

	# Put the sun where this can measure from. The cycle has to be ON: while it is
	# off every one of these answers is its own day constant (white, full
	# strength), which is the one state in which the sun has no colour to get
	# wrong -- so a probe that turned it off would be measuring the fix it is here
	# to hold. And it has to be SLOWED: a full day is ten seconds by default, and
	# the measurements below take a couple of seconds, so the sun would set
	# underneath them. 600 is the settings slider's own maximum, not an invented
	# number. All three go back at the end of the run.
	var was_enabled: bool = cm.get("day_night_cycle_enabled")
	var was_time: float = cm.get("day_time")
	var was_duration: float = cm.get("day_duration")
	cm.set("day_night_cycle_enabled", true)
	cm.set("day_duration", SLOW_DAY)
	cm.set("day_time", NOON)
	await _seconds(0.4)

	var model: Node3D = main.get_node_or_null("Player/ModelPivot/PlayerModel")
	if model == null:
		print("PROBE FAIL: the player has no ModelPivot/PlayerModel to measure")
		quit(1)
		return
	if not model.visible:
		_ok("body: drawn in third person", false, "the body is hidden in view 1")

	var mask: PackedInt32Array = await _body_mask(model)
	var total := 0
	var img := _frame()
	if img != null:
		total = int(ceil(float(img.get_width()) / STRIDE)) * int(ceil(float(img.get_height()) / STRIDE))
	var share := float(mask.size()) / maxf(float(total), 1.0)
	if mask.size() < 64:
		_ok("body: found in the frame", false,
			"the body covers too few pixels to measure (%d) -- is anything drawn at all?" % mask.size())
	else:
		_ok("body: found in the frame", share >= MIN_BODY_SHARE, "%.1f%% of the frame" % (100.0 * share))

	# The tint, at the top of the day -- the two halves of it, because the claim is
	# about the pair: the body's is white, the world's is still its cream. The
	# world's is read by putting a scratch material through the call the terrain's
	# own materials are fed through, from the same cached value, so it cannot be a
	# different number from the one the terrain gets.
	var body_material: ShaderMaterial = model.get("_body_material")
	if body_material == null:
		_ok("sky light: the body has a world material", false, "no _body_material")
	else:
		var overhead: Color = body_material.get_shader_parameter("sky_light_warmth")
		_ok("sky light: white overhead on the body",
			absf(overhead.r - 1.0) <= WHITE_TOL and absf(overhead.g - 1.0) <= WHITE_TOL
				and absf(overhead.b - 1.0) <= WHITE_TOL,
			"warmth handed to the body's material at noon is %s" % str(overhead))
		var world: Color = _world_warmth(cm)
		_ok("sky light: the world keeps its midday cream",
			world.r >= 0.98 and world.g > world.b
				and absf(world.g - WORLD_CREAM_G) <= CREAM_TOL
				and absf(world.b - WORLD_CREAM_B) <= CREAM_TOL,
			"warmth handed to the world at noon is %s" % str(world))

	var noon: Dictionary = await _measure(model, mask)
	if not noon.is_empty():
		# The claim: an item -- or a body -- can never disagree with the ground it
		# is standing on. The engine's own ambient is 0.027 of the sky, so a body
		# still on it reads near zero here and this is a factor of ten out.
		_ok("body: as bright as the world it covers",
			absf(float(noon["body"]) - float(noon["world"])) <= MATCH_TOLERANCE,
			"body %.3f vs the world behind it %.3f" % [float(noon["body"]), float(noon["world"])])
	await _seconds(0.2)
	_shot(_frame(), "noon")

	# The sun: midnight through the day/night uniforms the terrain is fed. The
	# cycle has to be ON for a time to mean anything (get_blend() answers "day"
	# while it is off), so the switch and the time are put back afterwards.
	cm.set("day_night_cycle_enabled", true)
	cm.set("day_time", MIDNIGHT)
	await _seconds(0.6)
	var night: Dictionary = await _measure(model, mask)
	await _seconds(0.2)
	_shot(_frame(), "midnight")
	# ...and the other end of it: the body's own value at the horizon must still be
	# warm, or "white overhead" would have been bought by flattening its sunset.
	# Given a few frames rather than the tick after the assignment, because it is
	# the body's own _process that writes it.
	if body_material != null:
		cm.set("day_time", HORIZON_TIME)
		await _seconds(0.2)
		var horizon: Color = body_material.get_shader_parameter("sky_light_warmth")
		_ok("sky light: the body is still warm at the horizon",
			horizon.g < horizon.r and horizon.b < 0.6 * horizon.r,
			"warmth at the horizon is %s" % str(horizon))

	cm.set("day_time", was_time)
	cm.set("day_duration", was_duration)
	cm.set("day_night_cycle_enabled", was_enabled)
	if not noon.is_empty() and not night.is_empty():
		_ok("body: follows the sun down",
			float(night["body"]) <= NIGHT_BLEND * float(noon["body"]),
			"luma %.3f at noon -> %.3f at midnight (the world behind it %.3f -> %.3f)"
			% [float(noon["body"]), float(night["body"]), float(noon["world"]), float(night["world"])])

	print("PROBE body light: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
