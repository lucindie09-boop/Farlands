extends SceneTree
## What the god-ray overlay costs with the sun on screen, per tap count.
##
## Run this WINDOWED; a dummy renderer has no frame worth measuring:
##
##   "C:/.../Godot_v4.7.1-stable_win64_console.exe" --path <project> \
##       --script res://probes/probe_godrays.gd
##
## The probe boots the real scene, pins the day to a 45-degree sun (a view a
## player actually has), sets the window to 1080p, disables the player so the
## camera can be aimed, and measures the same frame at several ray-march sample
## counts. It owns `num_samples` itself (the overlay's `_process` is switched
## off after it has set its uniforms), so the counts are the shader's input and
## not whatever the script would have pushed.
##
## The reading is the MEDIAN frame interval over MEASURE_FRAMES after a warmup:
## this machine runs an editor in the background, and a mean chases its
## spirious 60-120 ms hitches. `facing` aims the camera at the sun (the march at
## its longest); `away` points it at the opposite sky, where both visibilities
## are zero and the shader early-outs, which is the fixed cost of the pass.
##
## Versus is off and the fps cap lifted. Nothing is asserted; the numbers are
## printed for a before and after.

const SUN_TIME := 0.375          # day_time at which the sun sits 45 degrees up, +X
const WARMUP_FRAMES := 30
const MEASURE_FRAMES := 240
const TAP_COUNTS := [32, 24, 16, 8]
const WINDOW_SIZE := Vector2i(1920, 1080)

var main: Node3D
var cm: Node
var player: Node3D
var camera: Camera3D
var overlay: ColorRect
var material: ShaderMaterial
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
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	overlay = main.get_node_or_null("HUD/GodRaysOverlay")
	if cm == null or player == null or overlay == null:
		_fail("ChunkManager / Player / GodRaysOverlay missing")
		quit(1)
		return
	camera = player.get_node_or_null("Camera3D")
	if camera == null:
		_fail("Camera3D missing")
		quit(1)
		return

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(WINDOW_SIZE)
	Engine.max_fps = 0

	var waited := 0
	while waited < 1200:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(120):
		await process_frame

	if "day_night_cycle_enabled" in cm:
		cm.day_night_cycle_enabled = false
	cm.day_time = SUN_TIME
	var sun: Vector3 = cm.get_sun_direction()
	if sun.length_squared() < 0.5:
		_fail("sun direction is degenerate")
		quit(1)
		return
	sun = sun.normalized()

	player.set_process(false)
	player.set_physics_process(false)
	camera.global_transform = Transform3D(Basis.looking_at(sun, Vector3.UP), camera.global_position)

	var viewport_size: Vector2 = get_root().get_visible_rect().size
	print("PROBE godrays viewport=%dx%d" % [int(viewport_size.x), int(viewport_size.y)])

	material = overlay.material as ShaderMaterial
	if material == null:
		_fail("the overlay has no ShaderMaterial")
		quit(1)
		return
	# Do the rays still reach the frame? The pass writes light additively, so a
	# broken one (an empty scene texture) is cheap AND black. Capture the sun
	# view with and without the overlay and compare luminance: the center is
	# where the sun sits, and the whole frame is where the shafts reach.
	overlay.visible = true
	# The pass reads the previous frame's texture, so it needs a couple of
	# frames after being switched on before its output belongs to this view.
	for i in range(12):
		await process_frame
	var with_rays := await _capture()
	overlay.visible = false
	for i in range(8):
		await process_frame
	var without_rays := await _capture()
	overlay.visible = true
	for i in range(8):
		await process_frame
	var center_with := _luma(with_rays, Rect2i(viewport_size.x / 2 - 120, viewport_size.y / 2 - 120, 240, 240))
	var center_without := _luma(without_rays, Rect2i(viewport_size.x / 2 - 120, viewport_size.y / 2 - 120, 240, 240))
	var all_with := _luma(with_rays, Rect2i(0, 0, int(viewport_size.x), int(viewport_size.y)))
	var all_without := _luma(without_rays, Rect2i(0, 0, int(viewport_size.x), int(viewport_size.y)))
	print("PROBE godrays add_center=%.4f add_frame=%.4f" % [
		center_with - center_without, all_with - all_without])
	if all_with <= all_without + 0.0005:
		_fail("the rays added no light to the frame")

	# Settings -> Sky -> God Ray Quality is a percent of the resolution-derived
	# taps; at 1080p that is 4 / 16 / 32 for 25% / 100% / 200%.
	for q in [25, 100, 200]:
		overlay.quality = q
		for i in range(3):
			await process_frame
		print("PROBE godrays quality=%d taps=%d" % [
			q, int(material.get_shader_parameter("num_samples"))])
	overlay.quality = 100

	# Take the tap count over for the sweep below (the slider sets it every
	# frame otherwise).
	overlay.samples_override = 16

	# Two rounds, the second reported: the first frames after a resize (and the
	# first draw of a shader) are compilation and re-upload, not the march. The
	# second round walks the counts in the opposite order, so a drift in the
	# machine shows up as a curve that does not match the first.
	for i in range(TAP_COUNTS.size()):
		overlay.samples_override = TAP_COUNTS[i]
		await _measure("round1 taps=%d" % TAP_COUNTS[i], sun)
	for i in range(TAP_COUNTS.size() - 1, -1, -1):
		overlay.samples_override = TAP_COUNTS[i]
		await _measure("taps=%d" % TAP_COUNTS[i], sun)
	await _measure("hidden", sun, false)
	await _measure("away taps=8", -sun, true, 8)

	quit(0 if ok else 1)


func _capture() -> Image:
	await RenderingServer.frame_post_draw
	return get_root().get_texture().get_image()


# Mean luminance of a rect, sampled every 4th pixel: the image is 1080p and a
# per-pixel loop in GDScript would be slower than the pass being measured.
func _luma(image: Image, rect: Rect2i) -> float:
	var step := 4
	var total := 0.0
	var count := 0
	for y in range(rect.position.y, rect.position.y + rect.size.y, step):
		for x in range(rect.position.x, rect.position.x + rect.size.x, step):
			var c := image.get_pixel(x, y)
			total += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			count += 1
	return total / maxf(float(count), 1.0)


func _measure(label: String, look: Vector3, visible := true, taps := -1) -> void:
	camera.global_transform = Transform3D(Basis.looking_at(look, Vector3.UP), camera.global_position)
	overlay.visible = visible
	if taps > 0:
		overlay.samples_override = taps
	for i in range(WARMUP_FRAMES):
		await process_frame
	var ms: Array[float] = []
	ms.resize(MEASURE_FRAMES)
	var last := Time.get_ticks_usec()
	for i in range(MEASURE_FRAMES):
		await process_frame
		var now := Time.get_ticks_usec()
		ms[i] = float(now - last) / 1000.0
		last = now
	ms.sort()
	var median := ms[MEASURE_FRAMES / 2]
	var p90 := ms[int(MEASURE_FRAMES * 9 / 10)]
	print("PROBE godrays %s median_ms=%.3f fps=%.1f p90_ms=%.3f" % [
		label, median, 1000.0 / median, p90])
