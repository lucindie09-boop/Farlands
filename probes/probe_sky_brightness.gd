extends SceneTree
## What does a fully sky-lit surface actually render AT, as a fraction of its own
## texture? A measurement, not a pass/fail: it is here to answer the one question
## the light model's arithmetic cannot answer on its own, because the answer runs
## through the whole pipeline -- the mesher's brightness curve, the sky uniforms,
## the face shading, the +0.15 sky bounce, the filmic curve and the framebuffer's
## own conversion. Those are not separable by reading them; the frame is what the
## player sees.
##
## The subject is the player's own body, because it is the one surface in the game
## whose ALBEDO can be set to a number rather than guessed from an atlas: its
## material is one uniform (shaders/item_shader.gdshader), so the probe hands it a
## flat grey and knows exactly what went in. What comes back out of the frame, over
## the body's own silhouette, is the pipeline's answer for that albedo -- in the
## mean across its faces and at its brightest pixel (a top face, where the face
## shading is 1.0 and nothing else is dimming it).
##
## It reports, per albedo, both the body's luma and the world's luma behind it, and
## it prints the body's own live uniforms beside them, so the number can be read
## against the light it was lit with instead of taken on faith.
##
## It asserts the model's own claim, once per albedo: a face in FULL sun renders at
## the texel it was authored with and never above it. The measured number is the
## body's MEDIAN luma over its silhouette, which sits a little below the albedo by
## construction -- the silhouette is mostly side faces, and the terrain's own side
## shading (0.6 and 0.8) is what a side is lit at -- so the claim it is held to is
## the two-sided one: never ABOVE its own texel, and not far below it either.
##
## The MAXIMUM is printed too but not asserted on: the silhouette's outer pixels are
## blends of the body and the bright world behind it, and the body is idling, so a
## few of the mask's pixels are not body at all. A median ignores them; a maximum is
## made of them.
##
## What it caught, before the fix, at noon on a fully sky-lit body: a 0.50 albedo
## came back at 0.68 (1.36x its own texel) and a 0.25 at 0.61 (2.43x). The light
## topped out at kBlockBrightness[15] = 0.415, the sky term added a flat 0.15 on
## top of it, a filmic curve lifted the mid-tones, and the albedo was never sRGB-
## decoded -- so the framebuffer's own encode lifted it again.
##
## Nothing here writes to the world: the light steps are uniforms and the albedo is
## an in-memory 1x1 texture.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_sky_brightness.gd 400
##
## Shots go to user://brightness_shots/ (one per albedo).

const STRIDE := 4              # sample every Nth pixel; the means do not need all of them
const BODY_DELTA := 0.12       # a sampled pixel is the body above this much change
const MIN_BODY_SHARE := 0.01   # ...and the body has to be a real part of the frame
const NOON := 0.5              # day_time: the sun highest (get_sun_elevation == 1)
const SLOW_DAY := 600.0        # the settings slider's own maximum, see _run
## The albedos put under the light. 1.0 is a white texel: the top of what any
## texture can ask for, and the clearest place to see an over-bright pipeline.
const ALBEDOS := [0.25, 0.5, 0.75, 1.0]
## A full-sun face may not come back above its own texel by more than this share of
## it (plus a hair for the 8-bit frame), and must come back at least this far up
## toward it: the first is the bug this probe exists for, the second is the one a
## light that never arrives would cause.
const PEAK_OVER := 1.10
const PEAK_UNDER := 0.80

var _failures := 0


func _median(values: PackedFloat32Array) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[sorted.size() / 2]


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
## else, because nothing else about the frame changed. Taken ONCE, under the skin:
## the geometry does not move when the albedo does, so every albedo is measured
## over the same pixels.
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


## The body's own mean and peak luma over the mask, and the mean luma of the world
## it is drawn over, in the same frame.
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
	var body_max := -1.0
	var samples := PackedFloat32Array()
	for o in offsets:
		var l := _luma(a, o)
		body_sum += l
		body_max = maxf(body_max, l)
		samples.append(l)
		world_sum += _luma(b, o)
	var n := float(offsets.size())
	return {"body": body_sum / n, "peak": body_max, "median": _median(samples),
		"world": world_sum / n, "samples": offsets.size()}


## A 1x1 opaque grey. RGBA8 stores the components raw (no sRGB decode on this
## sampler -- the include declares a plain `sampler2D`, not `hint_albedo`), so what
## the shader samples is the number handed in, to a byte.
func _flat_albedo(grey: float) -> ImageTexture:
	var img := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	img.fill(Color(grey, grey, grey, 1.0))
	return ImageTexture.create_from_image(img)


func _shot(img: Image, name: String) -> void:
	if img == null:
		return
	DirAccess.make_dir_recursive_absolute("user://brightness_shots")
	img.save_png("user://brightness_shots/%s.png" % name)


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

	# The sun at the top of the day, where a sky-lit surface is lit the most. The
	# cycle has to be ON (while it is off every value here is its own day constant)
	# and SLOWED (a full day is ten seconds by default, and this takes longer), and
	# both go back at the end of the run.
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
	var body_material: ShaderMaterial = model.get("_body_material")
	if body_material == null:
		print("PROBE FAIL: the body has no world material to hand an albedo to")
		quit(1)
		return

	var mask: PackedInt32Array = await _body_mask(model)
	var total := 0
	var img := _frame()
	if img != null:
		total = int(ceil(float(img.get_width()) / STRIDE)) * int(ceil(float(img.get_height()) / STRIDE))
	var share := float(mask.size()) / maxf(float(total), 1.0)
	if mask.size() < 64:
		_ok("body: found in the frame", false,
			"the body covers too few pixels to measure (%d) -- is anything drawn at all?" % mask.size())
		print("PROBE sky brightness: %d failure(s)" % _failures)
		quit(1)
		return
	_ok("body: found in the frame", share >= MIN_BODY_SHARE, "%.1f%% of the frame" % (100.0 * share))

	# The light it is about to be lit with, read off its own material.
	var warmth: Color = body_material.get_shader_parameter("sky_light_warmth")
	print("PROBE light: sky intensity %s, sky colour %s, warmth %s" % [
		str(body_material.get_shader_parameter("sky_light_intensity")),
		str(body_material.get_shader_parameter("sky_light_color")), str(warmth)])
	var sample := _body_sample_light(model)
	print("PROBE light: the body's own cell light is %s (rgb block light, a sky light)" % str(sample))

	var skin: Texture2D = body_material.get_shader_parameter("albedo_texture")
	var reported := 0
	for grey in ALBEDOS:
		# A number under the light, not a texture.
		body_material.set_shader_parameter("albedo_texture", _flat_albedo(grey))
		await _seconds(0.25)
		var m: Dictionary = await _measure(model, mask)
		_shot(_frame(), "albedo_%03d" % int(grey * 100.0))
		if m.is_empty():
			continue
		reported += 1
		print("PROBE albedo %.2f -> body %.3f (peak %.3f), the world behind it %.3f  [x%.2f mean, x%.2f peak]" % [
			grey, float(m["body"]), float(m["peak"]), float(m["world"]),
			float(m["body"]) / grey, float(m["peak"]) / grey])
		var median := float(m["median"])
		_ok("albedo %.2f: never brighter than its own texel" % grey,
			median <= grey * PEAK_OVER + 0.01,
			"median %.3f over a %.2f albedo (x%.2f)" % [median, grey, median / grey])
		_ok("albedo %.2f: the light reaches its texel" % grey,
			median >= grey * PEAK_UNDER,
			"median %.3f over a %.2f albedo (x%.2f)" % [median, grey, median / grey])
	# The body back on its skin, and the light back where it was found.
	body_material.set_shader_parameter("albedo_texture", skin)
	cm.set("day_time", was_time)
	cm.set("day_duration", was_duration)
	cm.set("day_night_cycle_enabled", was_enabled)

	_ok("measured every albedo", reported == ALBEDOS.size(),
		"%d of %d" % [reported, ALBEDOS.size()])
	print("PROBE sky brightness: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


## The cell light the body's own mesh instances are handed, which is the other half
## of what it is lit with (the uniforms above are the sky half).
func _body_sample_light(model: Node3D) -> Vector4:
	var meshes: Array = model.get("_body_meshes")
	if meshes.is_empty():
		return Vector4.ZERO
	var mi := meshes[0] as MeshInstance3D
	if mi == null:
		return Vector4.ZERO
	var v: Variant = mi.get_instance_shader_parameter("item_light")
	return v if v is Vector4 else Vector4.ZERO
