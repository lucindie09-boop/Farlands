extends SceneTree
## The far field's two surfaces against EACH OTHER, at the player's own eye.
##
## The report is a light blue over the far field's own grass and sand, and the light blue is
## the mode's own `water_color` (its linear (0.20, 0.42, 0.78) lands on the captured
## pixels' mean (122, 169, 227)), so what the report is seeing is the water sheet's
## fragments where a distant land cell's texture should be.
##
## WHAT THE MODE DOES NOW: `build_tile_mesh` draws a cell's sheet only when ALL FOUR of its
## samples are under their own water; a cell with a land sample anywhere on it is drawn as
## land, whole, and draws no water at all. A far cell is therefore either water or land, and
## the two can never be two surfaces over one pixel -- the property
## `tests/test_lod_surface_shore.cpp` pins on the geometry itself (the area of a tile's
## triangles equals the area of the cells it drew, once).
##
## A single frame cannot tell a fight from a coastline, so this probe renders the two
## surfaces one at a time (`is_water` kept and its opposite discarded) -- the same camera,
## the same fill, the same geometry, only the fragment differs. Their INTERSECTION is every
## pixel where the water is drawn and the land is also drawn at the same pixel, which is a
## coastline seen from far above as much as it is a defect, so it is the CONTEXT figure and
## not the claim.
##
## The `cover_*` states measure the same thing with the DEPTH TEST OFF, so every pixel two
## surfaces both cross shows one of them whatever the depth buffer would have picked. That
## figure is REPORTED and is not a pass/fail: a pixel where the water is in front of the
## land and a pixel where the land is in front of the water both count, and a world with a
## coast in it has both. A frame cannot tell "the same ground covered twice" from "two
## grounds on one ray", which is why the claim for that property is made on the GEOMETRY,
## in tests/test_lod_surface_shore.cpp, and this probe is the look.
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_overlap.gd 600

const RENDER_DISTANCE := 4
## The world the run is measured on. WITHOUT this the seed is the launch's own, so two runs
## are two different landscapes: the camera lands at a different altitude, the far field
## covers a different share of the frame, and NOTHING measured here is comparable between
## them. The eye is still wherever the spawn settles, but a pinned seed pins the spawn.
const WORLD_SEED := 1337
const SPACING := 128
const OUTER_RINGS := 100
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
const SHOT_DIR := "user://lod_grid_shots"
const WATER_LINE := "albedo = mix(albedo, water_color, is_water * water_mix);"
const WATER_KEEP := "if (is_water < 0.5) discard;\n    albedo = mix(albedo, water_color, is_water * water_mix);"
const LAND_KEEP := "if (is_water > 0.5) discard;\n    albedo = mix(albedo, water_color, is_water * water_mix);"
## After the clip the two surfaces meet along an edge and no pixel centre is inside both,
## so the intersection is empty. The allowance is for a rasterizer that keeps a pixel on
## the shared edge twice.
const OVERLAP_TOL := 0.01
## The last state of the walk paints the fragments by SURFACE instead of by texture: red
## for the sheet, green for the land, nothing else in it. A one-pixel red-and-green
## interleave in this frame is two surfaces meeting at pixel scale and nothing else can
## make it; solid red and solid green regions with a ragged boundary are a shoreline
## drawn correctly and the interleave is coming from the textures instead.
const SHADING_LINE := "ALBEDO = mix(lit, fog_color, fade);"
const SURFACE_ONLY := "ALBEDO = vec3(is_water, 1.0 - is_water, 0.0);"
## ...and the same painting with the sheet's own fragments LIFTED two centimetres, which
## is the whole of one experiment: the pixels whose colour changes between the two are
## the pixels whose answer came from two surfaces within two centimetres of each other,
## measured inside one frame with no reference frame at all. A coastline drawn correctly
## changes nothing (its land and its water are not over each other); two surfaces drawn
## over the same ground change wholesale.
const VERTEX_LINE := "world_xz = VERTEX.xz;"
const VERTEX_LIFT := "world_xz = VERTEX.xz;\n    if (mod(UV2.y, 2.0) > 0.5) VERTEX.y += 0.02;"
## The coverage pair: the same painting, with the depth test off. With it off, the fragment
## that is drawn LAST owns the pixel, so a pixel covered by both surfaces shows one of them
## and comparing that frame against each single-surface frame names the pixels both cover --
## without the depth buffer's answer entering into it, which is the difference between "the
## two overlap here" and "the two are both visible along this ray".
## The sheet's own GEOMETRY, read out of the frame: the fragment's world Y carried through a
## varying and painted into red (a step of 4 blocks), with the land's fragments discarded and
## the depth test off, so every painted pixel is a sheet fragment and its height is readable.
## It answers a question no other state can: at what height is the far field's water? 
const VARY_DECL := "varying vec2 world_xz;"
const VARY_DECL_Y := "varying vec2 world_xz;\nvarying float probe_y;"
const VARY_SET := "world_xz = VERTEX.xz;"
const VARY_SET_Y := "world_xz = VERTEX.xz;\n    probe_y = VERTEX.y;"
const PAINT_HEIGHT := "ALBEDO = vec3(fract(probe_y / 1024.0), is_water, 0.0);"
const MODE_LINE := "render_mode unshaded, cull_disabled, depth_draw_always;"
const MODE_NO_DEPTH := "render_mode unshaded, cull_disabled, depth_draw_always, depth_test_disabled;"
const COVER_SHEET_ONLY := "if (is_water < 0.5) discard;\n    ALBEDO = vec3(is_water, 1.0 - is_water, 0.0);"
const COVER_LAND_ONLY := "if (is_water > 0.5) discard;\n    ALBEDO = vec3(is_water, 1.0 - is_water, 0.0);"
## ...and the allowance: two cells that share an edge name their own pixels, so the set must
## be empty and a rasterizer that keeps a shared edge twice is the only thing allowed here.
const COVERAGE_TOL := 64
## The far mode's two surfaces painted apart WITH THE DEPTH TEST LEFT ON: the land's
## fragments red and the sheet's blue, both keeping the depth buffer they ship with (no
## discard, no render_mode change). A second surface drawn behind either of them still
## cannot appear -- only the winner of the depth test is painted -- so wherever the
## report's light blue is in the frame, this state says which of the two put it there:
## blue means the sheet won that pixel against the land, red means the land won and the
## blue came from something else in the frame.
const SURFACE_LIVE := (
	"albedo = mix(albedo, is_water > 0.5 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), 1.0);"
)
## A binary ladder: every channel is exactly 0 or 1, so no tonemap, fog or sRGB curve can
## move a bit. The fragment's own WORLD Y and the shader's own water flag, side by side:
##
##   red   = this fragment sits at y >= 200 (the world's sea level)
##   green = this fragment is water per the shader (is_water > 0.5)
##   blue  = this fragment sits at y >= 400
##
## The far field's water is one flat plane at y = 200 (src/lod/lod_debug, read off the
## tiles), and a plane at 200 with the eye at 251.6 cannot project above the frame's
## middle -- so a pixel here that is green (water) and blue (high) is a fragment the
## SHADER calls water at a hill's own height, which is the report and cannot come from
## the mesh builder's own answer.
const Y_CODE := "ALBEDO = vec3(step(200.0, probe_y), is_water, step(400.0, probe_y));"
## ...and which texture layer the fragment's own cell samples: red = the water layer (32)
## or above, green = water per the shader, blue = the SECOND layer of its pair is 32+.
## A land cell wearing the water texture and a water cell wearing a land texture are both
## visible here, and neither is visible in is_water alone.
const LAYER_CODE := (
	"ALBEDO = vec3(step(31.5, UV2.x), is_water, step(31.5, floor(UV2.y * 0.5)));"
)

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
	img.save_png("%s/overlap_%s.png" % [SHOT_DIR, name])
	return img.get_data()


func _sample_positions(w: int, h: int) -> PackedInt32Array:
	var out := PackedInt32Array()
	out.resize((w / SAMPLE_STEP) * (h / SAMPLE_STEP))
	var n := 0
	for y in range(0, h, SAMPLE_STEP):
		for x in range(0, w, SAMPLE_STEP):
			out[n] = (y * w + x) * 4
			n += 1
	return out


## A byte per sampled pixel: 1 where the frame differs from the loaded world alone, which
## is "a far-field fragment is visible here".
func _hit_map(off: PackedByteArray, on: PackedByteArray, idx: PackedInt32Array,
		w: int, h: int) -> PackedByteArray:
	var cw := w / SAMPLE_STEP
	var out := PackedByteArray()
	out.resize(cw * (h / SAMPLE_STEP))
	var tol := MASK_TOL * 255.0
	var n := 0
	for i in idx:
		var d := absf(float(off[i]) - float(on[i])) \
			+ absf(float(off[i + 1]) - float(on[i + 1])) \
			+ absf(float(off[i + 2]) - float(on[i + 2]))
		out[n] = 1 if d > tol else 0
		n += 1
	return out


## 1 where this frame's far-field fragment is the sheet (painted red), 2 where it is the
## land (green), 0 where the pixel is neither -- the sky, an antialiased edge between the
## two, or the near world, which all three coverage frames show identically.
func _surface_class(data: PackedByteArray, i: int) -> int:
	var r := float(data[i])
	var g := float(data[i + 1])
	if r - g > 40.0:
		return 1
	if g - r > 40.0:
		return 2
	return 0


## The pixels BOTH surfaces cover. The depth test is off in all three frames, so `both`
## shows whichever fragment was drawn last and the pixel is covered twice exactly when the
## OTHER surface's own frame also has a fragment there -- which is the geometry, with the
## depth buffer's answer taken out of it.
func _covered_twice(sheet: PackedByteArray, land: PackedByteArray, both: PackedByteArray,
		idx: PackedInt32Array) -> int:
	var count := 0
	for i in idx:
		var b := _surface_class(both, i)
		if b == 1 and _surface_class(land, i) == 2:
			count += 1
		elif b == 2 and _surface_class(sheet, i) == 1:
			count += 1
	return count


func _tint(data: PackedByteArray, i: int) -> int:
	var r := float(data[i])
	var g := float(data[i + 1])
	var b := float(data[i + 2])
	if b - g > 24.0 and b - r > 24.0:
		return 1
	if g - b > 24.0 and g - r > 8.0:
		return 2
	return 0


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


## The first block of the column at (x, z), read from the loaded world. The world is
## deterministic here (the seed is pinned), so this is the same number every run.
func _ground_y(x: int, z: int) -> int:
	for y in range(1023, 0, -1):
		if int(_cm.get_block(x, y, z)) != 0:
			return y + 1
	return 0


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

	_cm.set("seed", WORLD_SEED)
	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)
	_cm.set("day_night_cycle_enabled", false)
	_cm.set("lod_grid_enabled", false)
	_cm.set("lod_grid_spacing", SPACING)
	_cm.set("lod_grid_outer_rings", OUTER_RINGS)
	await _frames(40)
	await _settle_player()
	# The EYE IS PLACED, not read off the player. The spawn is a height that depends on which
	# chunks had loaded when it was chosen -- two runs of this probe read 128 and 192 blocks
	# over ground whose surface is at 249 -- so a camera taken from the player is a different
	# camera every run and nothing measured here could be compared with anything. The ground
	# under the pinned seed is the same every run, so the eye is the ground plus a player's
	# own height at a fixed point.
	_eye = Vector3(0.0, float(_ground_y(0, 0)) + EYE_HEIGHT, 0.0)

	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	_ok("the frame is readable", size.x > 0, "%dx%d" % [size.x, size.y])
	if size.x == 0:
		_finish()
		return
	var idx := _sample_positions(size.x, size.y)

	_material = load("res://materials/lod_grid_material.tres")
	_ok("the far mode's material loads", _material != null and _material.shader != null)
	if _material == null or _material.shader == null:
		_finish()
		return
	_shader = _material.shader
	var water_only: Shader = _patch("water_only", [[WATER_LINE, WATER_KEEP]])
	var land_only: Shader = _patch("land_only", [[WATER_LINE, LAND_KEEP]])
	var surface_only: Shader = _patch("surface_only", [[SHADING_LINE, SURFACE_ONLY]])
	var surface_lift: Shader = _patch("surface_lift",
		[[SHADING_LINE, SURFACE_ONLY], [VERTEX_LINE, VERTEX_LIFT]])
	var sheet_height: Shader = _patch("sheet_height",
		[[VARY_DECL, VARY_DECL_Y], [VARY_SET, VARY_SET_Y], [MODE_LINE, MODE_NO_DEPTH],
		[WATER_LINE, WATER_KEEP], [SHADING_LINE, PAINT_HEIGHT]])
	var cover_both: Shader = _patch("cover_both",
		[[SHADING_LINE, SURFACE_ONLY], [MODE_LINE, MODE_NO_DEPTH]])
	var cover_sheet: Shader = _patch("cover_sheet",
		[[SHADING_LINE, SURFACE_ONLY], [MODE_LINE, MODE_NO_DEPTH],
		[SURFACE_ONLY, COVER_SHEET_ONLY]])
	var cover_land: Shader = _patch("cover_land",
		[[SHADING_LINE, SURFACE_ONLY], [MODE_LINE, MODE_NO_DEPTH],
		[SURFACE_ONLY, COVER_LAND_ONLY]])
	var surface_live: Shader = _patch("surface_live", [[WATER_LINE, SURFACE_LIVE]])
	var y_code: Shader = _patch("y_code",
		[[VARY_DECL, VARY_DECL_Y], [VARY_SET, VARY_SET_Y], [SHADING_LINE, Y_CODE]])
	var layer_code: Shader = _patch("layer_code", [[SHADING_LINE, LAYER_CODE]])
	_ok("the two surfaces can be rendered apart as patches of the shader's own text",
		water_only != null and land_only != null and surface_only != null
		and surface_lift != null and cover_both != null and cover_sheet != null
		and cover_land != null and sheet_height != null and surface_live != null)
	if water_only == null or land_only == null or surface_only == null or surface_lift == null \
			or cover_both == null or cover_sheet == null or cover_land == null \
			or sheet_height == null:
		_finish()
		return

	# The reference: the loaded world alone, from the same camera and the same fill.
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
	_ok("the far field is up, so there are two surfaces to compare",
		int(stats.get("uploads", 0)) >= 40,
		"uploads=%d of %d tiles, %d quads, spacing %d blocks, clip %d blocks" % [
			int(stats.get("uploads", 0)), int(stats.get("tiles", 0)),
			int(stats.get("quads", 0)), int(stats.get("spacing", 0)),
			int(stats.get("clip_radius", 0))])

	var cw := size.x / SAMPLE_STEP
	var rows := size.y / SAMPLE_STEP
	var worst_overlap := 0.0
	var worst_yaw := -1
	var covered_worst := 0
	var worst_sheet := 0.0
	var worst_land := 0.0
	var blue_on_world_land := 0
	var by_yaw := {}
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		var reference := _reference(int(yaw))
		if reference.size() == 0:
			_ok("the world-only reference at yaw %d loads" % int(yaw), false)
			continue
		# Both surfaces out of the SAME fill: only the fragment differs, so a pixel in both
		# maps is a pixel both surfaces are drawn to.
		_material.shader = water_only
		await _frames(SETTLE_FRAMES)
		var water_data := _capture("water_%d" % int(yaw))
		var water := _hit_map(reference, water_data, idx, size.x, size.y)
		_material.shader = land_only
		await _frames(SETTLE_FRAMES)
		var land_data := _capture("land_%d" % int(yaw))
		var land := _hit_map(reference, land_data, idx, size.x, size.y)
		_material.shader = surface_only
		await _frames(SETTLE_FRAMES)
		_capture("surface_%d" % int(yaw))
		_material.shader = surface_lift
		await _frames(SETTLE_FRAMES)
		_capture("surfacelift_%d" % int(yaw))
		# The claim: what the two surfaces COVER, with the depth buffer taken out of the
		# question (see the header).
		_material.shader = cover_both
		await _frames(SETTLE_FRAMES)
		var cover_both_data := _capture("cover_both_%d" % int(yaw))
		_material.shader = cover_sheet
		await _frames(SETTLE_FRAMES)
		var cover_sheet_data := _capture("cover_sheet_%d" % int(yaw))
		_material.shader = cover_land
		await _frames(SETTLE_FRAMES)
		var cover_land_data := _capture("cover_land_%d" % int(yaw))
		_material.shader = sheet_height
		await _frames(SETTLE_FRAMES)
		_capture("sheet_y_%d" % int(yaw))
		_material.shader = _shader
		await _frames(SETTLE_FRAMES)
		var ship_data := _capture("ship_%d" % int(yaw))
		# ...and the same frame with the two surfaces painted apart, the depth test kept: which
		# of them owns each pixel the report's light blue lands on.
		_material.shader = surface_live
		await _frames(SETTLE_FRAMES)
		_capture("surflive_%d" % int(yaw))
		_material.shader = y_code
		await _frames(SETTLE_FRAMES)
		_capture("ycode_%d" % int(yaw))
		_material.shader = layer_code
		await _frames(SETTLE_FRAMES)
		_capture("layercode_%d" % int(yaw))
		# The report as a switch. The far mode paints its light blue with ONE uniform
		# (`albedo = mix(albedo, water_color, is_water * water_mix)`, materials/
		# lod_grid_material.tres), so turning that uniform off shows what the tint is
		# doing today: the sea keeps its own texture from the water layer either way, and
		# every pixel that changes is a pixel something ELSE was wearing the tint on.
		_material.shader = _shader
		await _frames(SETTLE_FRAMES)
		_material.set_shader_parameter("water_mix", 0.0)
		await _frames(SETTLE_FRAMES)
		_capture("nomix_%d" % int(yaw))
		_material.set_shader_parameter("water_mix", 0.55)
		await _frames(SETTLE_FRAMES)
		# The report in the user's own terms: the sheet's light blue over the loaded world's
		# grass and sand.
		for i in idx:
			if _tint(ship_data, i) == 1 and _tint(reference, i) == 2:
				blue_on_world_land += 1

		var n_water := 0
		var n_land := 0
		var n_both := 0
		var n_union := 0
		var band := PackedInt32Array()
		band.resize(rows)
		for n in water.size():
			var w_hit := water[n] == 1
			var l_hit := land[n] == 1
			if w_hit:
				n_water += 1
			if l_hit:
				n_land += 1
			if w_hit or l_hit:
				n_union += 1
			if w_hit and l_hit:
				n_both += 1
				band[n / cw] += 1
		var share := float(n_both) / float(maxi(n_union, 1))
		worst_sheet = maxf(worst_sheet, float(n_water) / float(water.size()))
		worst_land = maxf(worst_land, float(n_land) / float(water.size()))
		by_yaw[int(yaw)] = share
		worst_overlap = maxf(worst_overlap, share)
		if share >= worst_overlap:
			worst_yaw = int(yaw)
		print("probe: yaw %3d  water %.3f of frame, land %.3f, union %.3f, BOTH %.3f of union"
			% [int(yaw), float(n_water) / float(water.size()),
				float(n_land) / float(water.size()),
				float(n_union) / float(water.size()), share])
		# Where in the frame the contested pixels are, as a sixth-resolution profile: a
		# coastline is a band with one peak, a sheet over the land fills the band.
		var profile := ""
		for r in rows:
			var step := rows / 24
			if r % step == 0:
				var v := 0.0
				for k in range(r, mini(r + step, rows)):
					v += float(band[k])
				profile += " .:-=+*#%@"[mini(int(8.0 * v / float(maxi(n_both, 1)) * 24.0 / float(step)), 9)]
		print("probe:        the contested pixels down the frame: [%s]" % profile)
		var covered_twice := _covered_twice(cover_sheet_data, cover_land_data, cover_both_data, idx)
		covered_worst = maxi(covered_worst, covered_twice)
		print("probe:        covered by BOTH of the far field's surfaces: %d px" % covered_twice)

	# The instrument's own control: the world-only reference again, at the END of the walk.
	# A reference that has moved between the two captures makes every mask above a
	# measurement of the difference between two runs rather than of the far field.
	_material.shader = _shader
	_cm.set("lod_grid_enabled", false)
	await _frames(40)
	_aim(90.0)
	await _frames(SETTLE_FRAMES)
	_capture("off2_90")
	print("probe: eye %s, seed %d, spacing %d blocks, %d tiles, %d quads" % [
		str(_eye), WORLD_SEED, SPACING, int(stats.get("tiles", 0)),
		int(stats.get("quads", 0))])
	print("probe: overlap per heading %s" % str(by_yaw))
	print("probe: both surfaces cross %d pixel(s) at the worst heading (occlusion counts here)"
		% covered_worst)
	# The instrument's own control: if either surface were missing, every figure above would
	# be measuring half a mode.
	_ok("both of the far field's surfaces are on screen",
		worst_sheet > 0.02 and worst_land > 0.02,
		"the sheet on %.3f of the frame, the land on %.3f" % [worst_sheet, worst_land])
	# ...and the claim this probe CAN make from a frame, in the words of the report: the
	# sheet's light blue must not be over the loaded world's grass and sand. (That the wide,
	# horizon part of it is not over the FAR field's own land is the same property one cell
	# out, and it is geometry: tests/test_lod_surface_shore.cpp.)
	_ok("the far field's light blue is not over the loaded world's land",
		blue_on_world_land <= COVERAGE_TOL,
		"%d pixel(s) of the world's land painted blue, allowance %d" % [
			blue_on_world_land, COVERAGE_TOL])

	_cm.set("lod_grid_enabled", false)
	print("PROBE lod grid overlap: %d failures" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)


## The loaded world alone at this heading; the far mode is on, so it is read back from the
## file the walk wrote at the top of the run.
func _reference(yaw: int) -> PackedByteArray:
	var img := Image.new()
	if img.load("%s/overlap_off_%d.png" % [SHOT_DIR, yaw]) != OK:
		return PackedByteArray()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img.get_data()
