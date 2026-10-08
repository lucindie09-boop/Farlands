extends SceneTree
## Is the far field being drawn over the loaded world?
##
## A report from the field: "z fighting on every single face", everywhere, close and far,
## on a still screen, with Far Mode on -- and, of the instrument that said it looked fine,
## "when I look at ur probe it looks fine, then i run the game and it z fights". That
## instrument's camera sits 140 blocks above the ground, where the loaded world is below
## the bottom of its own frame; it can only see the far field.
##
## The far field is not supposed to be drawn over the loaded world at all. Its fragments
## inside the world's disc are discarded in the shader against `clip_radius`, a uniform
## the C++ pushes every frame from `inner_radius_blocks` (the render distance in blocks).
## The two numbers do not have to agree: the clip is `inner_radius_blocks - 16` (16 blocks
## INSIDE the world's radius, deliberately, so the two sides overlap rather than leave a
## sliver), while the loaded world's own coverage is the render distance plus the two
## chunks its unload pass keeps (`unload_hrd = render_distance + 2`, i.e. 192 blocks of
## Chebyshev radius at a render distance of 4, and up to 271 at the diagonals). If the
## far field's clip is the smaller of the two, the far field is drawn over a ring of the
## loaded world, cell against block.
##
## So this probe does not measure noise: it PAINTS the far field's own fragments magenta
## and counts them, from the player's own eye. The states are all the same camera and the
## same fill, and the last one exists to prove the swap reaches the GPU at all:
##
##   off        the far mode disabled: the reference frame
##   paint      the far mode on, its fragments painted flat magenta, depth test as shipped
##   paint4     the same with the clip test's radius multiplied by four: a clip that is
##              doing its job has to show LESS, and a radius of zero cannot show anything
##   paint_all  the same paint with the depth test disabled, which shows the far field's
##              whole PROJECTED area rather than only the pixels it wins: the geometry's
##              extent, with the loaded world not allowed to hide it
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_clip.gd 600

const RENDER_DISTANCE := 4
const SPACING := 128
## The ladder's own reach rather than the setting's far end: this probe asks where the far
## field is near the PLAYER, and those are the innermost tiles at every reach.
const OUTER_RINGS := 0
const EYE_HEIGHT := 1.6
const PITCH := 0.0
const YAWS := [0, 90, 180, 270]
const FOV := 70.0
const CAMERA_FAR := 27392.0
const SETTLE_FRAMES := 12
const WAIT_FRAMES := 3600
const SAMPLE_STEP := 2
## A pixel is magenta when it is much more red+blue than green: the paint is flat, so
## nothing else in the world -- sky, terrain, water -- can be mistaken for it.
const MAGENTA_MARGIN := 40.0
const SHOT_DIR := "user://lod_grid_shots"
const ALBEDO_LINE := "ALBEDO = mix(lit, fog_color, fade);"
const PAINT_LINE := "ALBEDO = vec3(1.0, 0.0, 1.0);"
const CLIP_LINE := "distance(world_xz, clip_center) < clip_radius"
const CLIP_WIDE := "distance(world_xz, clip_center) < clip_radius * 4.0"
const MODE_LINE := "render_mode unshaded, cull_disabled, depth_draw_always;"
const MODE_ALWAYS := "render_mode unshaded, cull_disabled, depth_draw_always, depth_test_disabled;"

var _failures := 0
var _cm: Node = null
var _player: Node3D = null
var _camera: Camera3D = null
var _material: ShaderMaterial = null
var _shader: Shader = null
var _patched := {}
var _eye := Vector3.ZERO


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _frames(count: int) -> void:
	for _i in count:
		await process_frame


func _capture(name: String) -> PackedByteArray:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return PackedByteArray()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	img.save_png("%s/clip_%s.png" % [SHOT_DIR, name])
	return img.get_data()


## The magenta share of the sampled pixels of the whole frame, which is the number a
## fragment the world should be hiding cannot move.
func _paint(image_data: PackedByteArray, width: int, height: int) -> float:
	var hits := 0
	var total := 0
	for y in range(0, height, SAMPLE_STEP):
		for x in range(0, width, SAMPLE_STEP):
			var i := (y * width + x) * 4
			if i + 2 >= image_data.size():
				continue
			total += 1
			if float(image_data[i]) > float(image_data[i + 1]) + MAGENTA_MARGIN \
					and float(image_data[i + 2]) > float(image_data[i + 1]) + MAGENTA_MARGIN:
				hits += 1
	return float(hits) / float(maxi(total, 1))


## A patch of the shader's own text, as find/replace pairs applied in order. A frozen
## string matching nothing is reported rather than silently measuring the unpatched
## shader.
func _patch(name: String, pairs: Array) -> Shader:
	if _patched.has(name):
		return _patched[name]
	var src := FileAccess.get_file_as_string("res://shaders/lod_grid.gdshader")
	if src.is_empty():
		return null
	var body := src
	for pair in pairs:
		var from := String(pair[0])
		var next := body.replace(from, String(pair[1]))
		if next == body:
			print("probe: the line to patch for '%s' was not found: %s" % [name, from])
			_patched[name] = null
			return null
		body = next
	var shader := Shader.new()
	shader.code = body
	_patched[name] = shader
	return shader


func _aim(yaw_degrees: float) -> void:
	_camera.global_position = _eye
	_camera.rotation_degrees = Vector3(PITCH, yaw_degrees, 0.0)


## The player spawns in the air and falls: a fixed frame count reads a different altitude
## on every run.
func _settle_player() -> void:
	var stood := 0
	var last: float = _player.global_position.y
	for _i in 900:
		await process_frame
		var now: float = _player.global_position.y
		if absf(now - last) < 0.01:
			stood += 1
			if stood >= 10:
				return
		else:
			stood = 0
		last = now


func _use(shader: Shader) -> void:
	_material.shader = shader if shader != null else _shader
	await _frames(SETTLE_FRAMES)


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	_cm = main.get_node_or_null("ChunkManager")
	_player = main.get_node_or_null("Player")
	_camera = Camera3D.new()
	_camera.fov = FOV
	_camera.far = CAMERA_FAR
	main.add_child(_camera)
	_camera.make_current()

	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)
	_cm.set("day_night_cycle_enabled", false)
	_cm.set("lod_grid_enabled", false)
	_cm.set("lod_grid_spacing", SPACING)
	_cm.set("lod_grid_outer_rings", OUTER_RINGS)
	await _frames(40)
	await _settle_player()
	_eye = _player.global_position + Vector3(0.0, EYE_HEIGHT, 0.0)

	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	_ok("the frame is readable", size.x > 0, "%dx%d" % [size.x, size.y])
	if size.x == 0:
		_finish()
		return

	_material = load("res://materials/lod_grid_material.tres")
	_ok("the far mode's material loads", _material != null and _material.shader != null)
	if _material == null or _material.shader == null:
		_finish()
		return
	_shader = _material.shader

	var paint: Shader = _patch("paint", [[ALBEDO_LINE, PAINT_LINE]])
	var paint_wide: Shader = _patch("paint_wide", [[ALBEDO_LINE, PAINT_LINE], [CLIP_LINE, CLIP_WIDE]])
	var paint_all: Shader = _patch("paint_all", [[ALBEDO_LINE, PAINT_LINE], [MODE_LINE, MODE_ALWAYS],
		[CLIP_LINE, CLIP_WIDE]])
	_ok("the paint states all patch the shader's own text",
		paint != null and paint_wide != null and paint_all != null)
	if paint == null or paint_wide == null or paint_all == null:
		_finish()
		return

	# The reference: the loaded world alone, before the far field exists.
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		_capture("off_%d" % int(yaw))

	# One fill for the whole run: only the shader changes between states, so the geometry
	# is the same in every reading and the states are one program apart.
	_cm.set("lod_grid_enabled", true)
	await _use(paint)
	var stats := {}
	for _i in WAIT_FRAMES:
		await process_frame
		stats = _cm.get_lod_grid_stats()
		if int(stats.get("uploads", 0)) >= 24 \
				and int(stats.get("uploads", 0)) >= int(stats.get("tiles", 0)) - 8:
			break
	_ok("the far field is up, so there is something to paint",
		int(stats.get("uploads", 0)) >= 24,
		"uploads=%d of %d tiles, %d draw calls, %d quads, horizon %d blocks" % [
			int(stats.get("uploads", 0)), int(stats.get("tiles", 0)),
			int(stats.get("draw_calls", 0)), int(stats.get("quads", 0)),
			int(stats.get("outer_radius", 0))])

	print("probe: render distance %d blocks; the loaded world keeps %d chunks, so it draws" % [
		RENDER_DISTANCE * 32, RENDER_DISTANCE + 2])
	print("probe: to %d blocks of Chebyshev radius (%d at the diagonals)" % [
		(RENDER_DISTANCE + 2) * 32, int(float((RENDER_DISTANCE + 2) * 32) * 1.4143)])

	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		var uniform: Variant = _material.get_shader_parameter("clip_radius")
		var center: Variant = _material.get_shader_parameter("clip_center")
		var shipped := _paint(_capture("paint_%d" % int(yaw)), size.x, size.y)
		await _use(paint_wide)
		var wider := _paint(_capture("paint4_%d" % int(yaw)), size.x, size.y)
		await _use(paint_all)
		var widest := _paint(_capture("paintall_%d" % int(yaw)), size.x, size.y)
		await _use(paint)
		print("probe: yaw %3d  clip_radius %s, clip_center %s" % [
			int(yaw), str(uniform), str(center)])
		print("probe:           painted magenta: as shipped %.2f, at 4x the disc %.2f, whole area %.2f" % [
			shipped, wider, widest])
		_ok("the clip is a live uniform, and widening it removes far field", uniform != null
			and float(uniform) > 0.5 and wider < shipped,
			"%.3f at the shipped radius against %.3f at four times it" % [shipped, wider])

	_cm.set("lod_grid_enabled", false)
	print("PROBE lod grid clip: %d failure(s)" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)
