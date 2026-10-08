extends SceneTree
## The far field against the loaded world, from the player's own eye.
##
## A report from the field: "z fighting on every single face", everywhere, close and far,
## on a screen that is not moving, with Far Mode on -- and of the instrument that said it
## looked fine, "when i look at ur probe it looks fine, then i run the game and it z
## fights". That instrument's camera sits 140 blocks above the ground, where the loaded
## world is below the bottom of its own frame, and it never applied the player's settings;
## it can only see the far field over open country.
##
## The far field is not supposed to be drawn over the loaded world. Its fragments inside
## the world's disc are discarded in the shader against `clip_radius`, and the radius the
## C++ pushes is cut from the world's DRAWN edge -- the render distance, the unload pass's
## retained rings, and the width of the last ring (see world_drawn_radius_blocks). It used
## to be cut from the STREAMING radius, which is two chunks and a chunk-width inside that
## edge: the far field was drawn over the world's retained rings, cell against block.
##
## The clip was the first thing measured here and it was not the answer: cutting the disc
## from the drawn edge is real (it takes the far field off the world's retained rings, 45%
## of the frame to 21%) and the pixel fight is identical at both radii, so what follows is
## the far field's own rendering. The camera is the player's eye, level, with the player's
## own render distance and the far field's own spacing, which is the configuration the
## report came from.
##
## It reports three numbers over the mask: the mask's own contrast, its single-pixel
## outliers (pixels that differ from BOTH neighbours while those two agree, which is what
## two surfaces contesting one pixel leaves), and -- because the report names them --
## **blue|green**: the share of the mask whose water tint (much more blue than green) has
## the land's green within two pixels. A coastline drawn by geometry is a contour, so a
## large share here is one pixel of each colour interleaved, which is the report.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_seam.gd 600

const RENDER_DISTANCE := 4
const SPACING := 128
const OUTER_RINGS := 100
## The player's own eye: 1.6 blocks up and level, which is the view the report came from.
## A probe camera rather than the player's own node, because the controller re-aims its
## own camera every frame from the player's look state.
const EYE_HEIGHT := 1.6
const PITCH := 0.0
const YAWS := [0, 90, 180, 270]
const FOV := 70.0
const CAMERA_FAR := 27392.0
const SETTLE_FRAMES := 12
const WAIT_FRAMES := 5400
const SAMPLE_STEP := 2
## A pixel counts as "the far field drew here" above this summed RGB distance (0..1) from
## the mode-off frame.
const MASK_TOL := 0.03
const SPIKE := 12.0
const SPIKE_AGREE := 6.0
const SHOT_DIR := "user://lod_grid_shots"
const CLIP_LINE := "distance(world_xz, clip_center) < clip_radius"
## Two chunks of and a chunk's width off the drawn edge: the radius the clip used to be
## cut from, which is the two retained rings plus the width of the last one.
const CLIP_OLD_BUILD := [CLIP_LINE, "distance(world_xz, clip_center) < clip_radius - 96.0"]
## The three candidates the report's own colours leave standing, each a state of the same
## camera and the same fill:
##
##   flat       the detail term off (`detail_strength` 0). If the interleave is the term's
##              grain, this is the state that has none of it.
##   adapt128   the term with its repeat held at eight screen pixels a texel instead of
##              faded by distance (the sweep in probe_lod_grid_zfight.gd measured that as
##              x2.7 of a no-term control against the shipped x31, keeping four fifths of
##              the term's contrast).
##   nofog      the mode's own fog mix removed. Its colour is vec3(0.70, 0.80, 0.95) --
##              light blue -- and the report is light blue against green, so this state is
##              what is left when the only blue in the shader is gone.
const WATER_LINE := "albedo = mix(albedo, water_color, is_water * water_mix);"
const WATER_OFF := "if (is_water > 0.5) discard;"
const FOG_LINE := "ALBEDO = mix(lit, fog_color, fade);"
const FOG_OFF := "ALBEDO = lit;"
const FINE_LINE := "vec3 fine = texture(texture_array, vec3(detail_uv, layer), mipmap_bias).rgb;"
const AVERAGE_LINE := "vec3 average = textureLod(texture_array, vec3(detail_uv, layer), DETAIL_AVERAGE_MIP).rgb;"
const DETAIL_STRENGTH := 0.75


## The two lines that make the term hold its own sampling rate, for a chosen number of
## screen pixels per texel of the face: one repeat is sixteen texels, so the repeat has to
## be that many times sixteen pixels wide for a texel to cover that many of them.
func _adaptive_pairs(px_per_texel: float) -> Array:
	var width := "%.1f" % (px_per_texel * 16.0)
	return [
		["detail_strength * fade", "detail_strength"],
		[FINE_LINE,
			"float spread = max(detail_scale, max(max(footprint.x, footprint.y), 1.0e-4) * %s);\n        vec2 span_uv = uv / spread;\n        vec3 fine = texture(texture_array, vec3(span_uv, layer), mipmap_bias).rgb;" % width],
		[AVERAGE_LINE,
			"vec3 average = textureLod(texture_array, vec3(span_uv, layer), DETAIL_AVERAGE_MIP).rgb;"],
	]

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
	img.save_png("%s/seam_%s.png" % [SHOT_DIR, name])
	return img.get_data()


func _indices(w: int, h: int) -> PackedInt32Array:
	var out := PackedInt32Array()
	out.resize((w / SAMPLE_STEP) * (h / SAMPLE_STEP))
	var n := 0
	for y in range(0, h, SAMPLE_STEP):
		for x in range(0, w, SAMPLE_STEP):
			out[n] = (y * w + x) * 4
			n += 1
	return out


func _mask(off: PackedByteArray, on: PackedByteArray, idx: PackedInt32Array) -> PackedInt32Array:
	var out := PackedInt32Array()
	var tol := MASK_TOL * 255.0
	for i in idx:
		var d := absf(float(off[i]) - float(on[i])) \
			+ absf(float(off[i + 1]) - float(on[i + 1])) \
			+ absf(float(off[i + 2]) - float(on[i + 2]))
		if d > tol:
			out.append(i)
	return out


## Mean difference between a pixel and the one two along, over the mask: local contrast,
## which is what a pattern adds.
func _noise(data: PackedByteArray, idx: PackedInt32Array) -> float:
	var total := 0.0
	var counted := 0
	const STEP_BYTES := SAMPLE_STEP * 4
	for i in idx:
		var j := i + STEP_BYTES
		if j + 2 >= data.size():
			continue
		total += (absf(float(data[i]) - float(data[j]))
			+ absf(float(data[i + 1]) - float(data[j + 1]))
			+ absf(float(data[i + 2]) - float(data[j + 2]))) / 3.0 / 255.0
		counted += 1
	return total / float(maxi(counted, 1))


## The share of the mask where the water's own blue and the land's own green are
## neighbours at pixel scale: a coastline drawn by geometry has ONE such boundary, a
## contour, so a large share of the mask in this measurement is two surfaces interleaved
## rather than a shore. Light blue is the sheet's tint over the land (`water_color` over
## a green albedo), so the pair is the report's own two colours.
func _mosaic(data: PackedByteArray, idx: PackedInt32Array) -> float:
	var hits := 0
	var counted := 0
	const STEP_BYTES := SAMPLE_STEP * 4
	for i in idx:
		var j := i + STEP_BYTES
		if j + 2 >= data.size():
			continue
		counted += 1
		var a := _tint(data, i)
		var b := _tint(data, j)
		if (a == 1 and b == 2) or (a == 2 and b == 1):
			hits += 1
	return float(hits) / float(maxi(counted, 1))


## 0 for anything that is neither, 1 for the water tint (much more blue than green) and 2
## for land (much more green than blue) -- the two colours the report names.
func _tint(data: PackedByteArray, i: int) -> int:
	var r := float(data[i])
	var g := float(data[i + 1])
	var b := float(data[i + 2])
	if b - g > 24.0 and b - r > 24.0:
		return 1
	if g - b > 24.0 and g - r > 8.0:
		return 2
	return 0


## The fraction of mask pixels that stand out against BOTH neighbours while those two
## agree with each other: one pixel of a different colour, which is what two surfaces
## fighting for one pixel leaves and what a pattern wider than a pixel cannot make.
func _spikes(data: PackedByteArray, idx: PackedInt32Array) -> float:
	var hits := 0
	var counted := 0
	const STEP_BYTES := SAMPLE_STEP * 4
	for i in idx:
		var l := i - STEP_BYTES
		var r := i + STEP_BYTES
		if l < 0 or r + 2 >= data.size():
			continue
		var dl := (absf(float(data[i]) - float(data[l])) + absf(float(data[i + 1]) - float(data[l + 1]))
			+ absf(float(data[i + 2]) - float(data[l + 2]))) / 3.0
		var dr := (absf(float(data[i]) - float(data[r])) + absf(float(data[i + 1]) - float(data[r + 1]))
			+ absf(float(data[i + 2]) - float(data[r + 2]))) / 3.0
		var lr := (absf(float(data[l]) - float(data[r])) + absf(float(data[l + 1]) - float(data[r + 1]))
			+ absf(float(data[l + 2]) - float(data[r + 2]))) / 3.0
		counted += 1
		if dl > SPIKE and dr > SPIKE and lr < SPIKE_AGREE:
			hits += 1
	return float(hits) / float(maxi(counted, 1))


## A patch of the shader's own text, reported rather than silently ignored when the line
## it was written against has moved.
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


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	_cm = main.get_node_or_null("ChunkManager")
	_player = main.get_node_or_null("Player")
	if _cm == null or _player == null:
		_ok("ChunkManager and Player exist", false)
		_finish()
		return
	_camera = Camera3D.new()
	_camera.fov = FOV
	_camera.far = CAMERA_FAR
	main.add_child(_camera)
	_camera.make_current()

	# The player's own configuration: the render distance the clip is cut from, the far
	# field's spacing, and its reach.
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
	var idx := _indices(size.x, size.y)

	_material = load("res://materials/lod_grid_material.tres")
	_ok("the far mode's material loads", _material != null and _material.shader != null)
	if _material == null or _material.shader == null:
		_finish()
		return
	_shader = _material.shader
	var adapt128: Shader = _patch("adapt128", _adaptive_pairs(8.0))
	var no_fog: Shader = _patch("nofog", [[FOG_LINE, FOG_OFF]])
	_ok("the states can be reached as patches of the shader's own text",
		adapt128 != null and no_fog != null)
	if adapt128 == null or no_fog == null:
		_finish()
		return

	# The reference: the loaded world alone.
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		_capture("off_%d" % int(yaw))

	_cm.set("lod_grid_enabled", true)
	var stats := {}
	for _i in WAIT_FRAMES:
		await process_frame
		stats = _cm.get_lod_grid_stats()
		if int(stats.get("uploads", 0)) >= 40 \
				and int(stats.get("uploads", 0)) >= int(stats.get("tiles", 0)) - 8:
			break
	_ok("the far field is up, so there is something to race the world with",
		int(stats.get("uploads", 0)) >= 40,
		"uploads=%d of %d tiles, %d draw calls, %d quads, horizon %d blocks, clip %d" % [
			int(stats.get("uploads", 0)), int(stats.get("tiles", 0)),
			int(stats.get("draw_calls", 0)), int(stats.get("quads", 0)),
			int(stats.get("outer_radius", 0)), int(stats.get("clip_radius", 0))])
	print("probe: the far field's cells are %d blocks; the clip disc is %d blocks; the world" % [
		int(stats.get("spacing", 0)), int(stats.get("clip_radius", 0))])
	print("probe: is streamed to %d blocks and drawn to %d of them plus its last chunk" % [
		RENDER_DISTANCE * 32, (RENDER_DISTANCE + 2) * 32])

	var worst := {"ship": 0.0, "flat": 0.0, "adapt128": 0.0, "nofog": 0.0}
	var drawn := {"ship": 0.0, "flat": 0.0, "adapt128": 0.0, "nofog": 0.0}
	var mosaic := {"ship": 0.0, "flat": 0.0, "adapt128": 0.0, "nofog": 0.0}
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		for state in [["ship", null], ["flat", null], ["adapt128", adapt128], ["nofog", no_fog]]:
			var name := String(state[0])
			_material.shader = state[1] if state[1] != null else _shader
			_material.set_shader_parameter("detail_strength",
				0.0 if name == "flat" else DETAIL_STRENGTH)
			await _frames(SETTLE_FRAMES)
			var data := _capture("%s_%d" % [name, int(yaw)])
			var mask := _mask(_reference(int(yaw), size), data, idx)
			var coverage := float(mask.size()) / float(maxi(idx.size(), 1))
			var spikes := _spikes(data, mask)
			var mix_share := _mosaic(data, mask)
			print("probe: yaw %3d  %-8s far field %.2f of frame, noise %.4f, spikes %.4f, blue|green %.4f" % [
				int(yaw), name, coverage, _noise(data, mask), spikes, mix_share])
			worst[name] = maxf(worst[name], spikes)
			drawn[name] += coverage
			mosaic[name] = maxf(mosaic[name], mix_share)
		_material.shader = _shader
		_material.set_shader_parameter("detail_strength", DETAIL_STRENGTH)

	print("probe: worst heading per state -- spikes: ship %.4f, term off %.4f, sampling held %.4f," % [
		worst["ship"], worst["flat"], worst["adapt128"]])
	print("probe: fog off %.4f; blue|green: ship %.4f, term off %.4f, sampling held %.4f, fog off %.4f" % [
		worst["nofog"], mosaic["ship"], mosaic["flat"], mosaic["adapt128"], mosaic["nofog"]])

	_cm.set("lod_grid_enabled", false)
	print("PROBE lod grid seam: %d failures" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)


## The loaded world alone at this heading, from the file the walk wrote: re-capturing it
## with the far mode off would tear the grid down and pay for its next fill.
func _reference(yaw: int, _size: Vector2i) -> PackedByteArray:
	var img := Image.new()
	if img.load("%s/seam_off_%d.png" % [SHOT_DIR, yaw]) != OK:
		return PackedByteArray()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img.get_data()
