extends SceneTree
## Is the ground beyond the loaded world actually THERE, at every radius, in every
## direction?
##
## Three versions of the far mode looked healthy by every number it reports and were
## not, and each needed a different measurement to see:
##
##   1. A ring of tiles built, uploaded and counted -- and exactly one of them
##      visible. The geometry is in world coordinates and the instance was ALSO
##      given the tile's origin, so every tile but the first was drawn at double its
##      offset. Tiles built is not tiles seen.
##
##   2. The far field's inner boundary was a square hole in the TILE SET -- whole
##      256-block tiles were skipped around the player -- while the world loads a
##      DISC, so the square's corners, from the world's radius out to 1.41x it, were
##      drawn by neither.
##
##   3. Which is only visible if the frame is read where those blocks land. A band
##      of screen rows is a band of ground distances that depends on how high the
##      camera happens to be, and aiming it by pitch is how a previous version of
##      this probe managed to measure the loaded world and call it the horizon.
##
## So the camera goes straight up and looks straight DOWN, where the mapping is
## exact: the frame's centre is the point under the camera, and a pixel at distance
## D from it is the ground at radius r = H * tan(D / f_px), with H the camera's
## height above the terrain and f_px the focal length in pixels. Every direction at
## once, no pitch to guess.
##
## The reference is the SAME PIXEL with the mode off: a pixel the mode did not
## reach is a pixel that did not change. No sky colour, no fog colour, no threshold
## on "is this terrain" -- just "was this pixel different before the mode drew its
## side of the world". A second off capture is the control, so "the mode filled it"
## cannot be confused with "the frame drifted"; the day/night cycle is frozen for
## the run, because a drifting sun moves every sky pixel by a few thousandths and an
## early version of this probe measured exactly that.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_shot.gd 840
##
## Shots go to user://lod_grid_shots/ for the eye: top_<prefix>.png.

const RENDER_DISTANCE := 4        # 128 blocks of loaded world: the far field starts there
const CAMERA_Y := 3600.0          # far above the terrain, so r = H * tan(angle) is exact
const FOV := 70.0                 # vertical, matching the camera below
## Radii the profile is read at, in blocks: across the world's edge, through the band
## the square hole used to leave empty (192-448), and out to the grid's own horizon.
const RADII := [128, 160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 1024, 1280, 1536, 1792, 2048]
## Past the grid's own horizon, where the ground must be SKY again: a measurement
## that says "changed" everywhere measures nothing. It starts at 2600 because the
## rings reach FURTHER than the radius the stats report: that radius is where the
## outermost ring begins, and its tiles are another 256 blocks wide (to 2304).
const BEYOND := [2600, 2900]
const SECTORS := 16               # directions around the circle
## A patch "changed" above this RGB distance (summed over the three channels).
## The frame is temporally jittered -- consecutive frames differ by a few
## thousandths per pixel -- so each sample is the MEAN of a small patch, which
## averages that away, and the threshold sits above what is left. Too high and a
## hole in the ground reads as unchanged: the far field of the world this was
## measured on is a flat ocean whose colour is a mere 0.19 from the sky's.
const PATCH := 3                  # half-width: a 7x7 patch per sample
## The bottom of the frame is the debug overlay, and it prints counts that change
## when the mode is switched on -- so a patch that lands there measures the HUD, not
## the ground. Those directions are dropped rather than counted either way, which is
## what "the ground past the horizon is sky" was failing on.
const HUD_SAFE_Y := 0.84
const CHANGE_TOL := 0.08
const MIN_MEAN_CHANGED := 0.9     # ...and this share of the directions must have changed
const MIN_WORST_CHANGED := 0.75   # ...and no single direction may be worse than this
const MAX_DRIFT := 0.1            # ...while the off-versus-off control must not
## The tile lattice: boundaries at +-256 and +-512 blocks (the far field's tiles are
## anchored at the world origin), sampled along each line. CRACK_TOL is how much
## brighter than the ground to either side a boundary patch may be before it counts
## as a line of sky rather than ground.
const CRACK_LINES := [256, -256, 512, -512]
const CRACK_ALONG := [-320, -224, -96, 0, 96, 224, 320]
const CRACK_TOL := 0.06
const WAIT_FRAMES := 600          # tile builds are budgeted, so the grid takes frames
const WAIT_UPLOADS := 30          # enough tiles to reach the inner rings at least
const SETTLE_FRAMES := 8

var _failures := 0
var _cm: Node = null
var _main: Node = null
var _camera: Camera3D = null
var _player: Node3D = null


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _frames(count: int) -> void:
	for i in count:
		await process_frame


func _frame() -> PackedByteArray:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return PackedByteArray()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	DirAccess.make_dir_recursive_absolute("user://lod_grid_shots")
	img.save_png("user://lod_grid_shots/top_%d%s.png" % [_captures, "_on" if _on_capture else ""])
	return img.get_data()


var _captures := 0
var _on_capture := false


## The mean colour of a 7x7 patch of frame around the ground point dx, dz blocks
## from the camera's own column, through the same pinhole projection: the frame's
## centre is the point under the camera.
func _patch_xz(data: PackedByteArray, size: Vector2i, dx: float, dz: float,
		height: float) -> Color:
	var half: float = float(size.y) * 0.5
	var f_px: float = half / tan(deg_to_rad(FOV * 0.5))
	var cx: int = clampi(int(round(float(size.x) * 0.5 + f_px * dx / maxf(height, 1.0))),
		PATCH, size.x - PATCH - 1)
	var cy: int = clampi(int(round(half - f_px * dz / maxf(height, 1.0))),
		PATCH, size.y - PATCH - 1)
	var sum := Color(0.0, 0.0, 0.0)
	var count := 0.0
	for dy in range(-PATCH, PATCH + 1):
		var row: int = (cy + dy) * size.x
		for dxi in range(-PATCH, PATCH + 1):
			var o: int = (row + cx + dxi) * 4
			sum += Color8(data[o], data[o + 1], data[o + 2])
			count += 1.0
	return sum / count


func _patch(data: PackedByteArray, size: Vector2i, radius_blocks: float, height: float,
		sector: int) -> Color:
	var phi: float = TAU * float(sector) / float(SECTORS)
	return _patch_xz(data, size, radius_blocks * sin(phi), -radius_blocks * cos(phi), height)


## Patch brightness in the frame, which is what a crack shows up as: a line of sky
## where there should be ground.
func _luma(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b


## Counts cracks along the tile lattice. Tiles are 256 blocks a side anchored at the
## world origin, and where two of them meet they agree at their shared nodes and
## nowhere else unless something makes them: two spacings meeting at a level
## boundary, or a tile that has not arrived. Either one opens a thin line of sky
## along the boundary, so each boundary point is compared with the ground 24 blocks
## to either side of it, in the same direction.
func _cracks(live: PackedByteArray, size: Vector2i, height: float) -> int:
	var found := 0
	for line in CRACK_LINES:
		for along in CRACK_ALONG:
			var cx := float(line)
			var cz := float(along)
			var centre: float = _luma(_patch_xz(live, size, cx, cz, height))
			var side_a: float = _luma(_patch_xz(live, size, cx - 24.0, cz, height))
			var side_b: float = _luma(_patch_xz(live, size, cx + 24.0, cz, height))
			if centre > maxf(side_a, side_b) + CRACK_TOL:
				found += 1
				print("probe: crack at line %d along %d  centre %.3f sides %.3f/%.3f" %
					[int(line), int(along), centre, side_a, side_b])
	return found


func _changed(before: PackedByteArray, after: PackedByteArray, size: Vector2i, radius: float,
		height: float, sector: int) -> bool:
	if before.size() == 0 or before.size() != after.size():
		return false
	var a: Color = _patch(before, size, radius, height, sector)
	var b: Color = _patch(after, size, radius, height, sector)
	return absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b) > CHANGE_TOL


## Is there frame to read at this ground point? The bottom strip is the overlay.
func _in_frame(size: Vector2i, dx: float, dz: float, height: float) -> bool:
	var half: float = float(size.y) * 0.5
	var f_px: float = half / tan(deg_to_rad(FOV * 0.5))
	var cx: float = float(size.x) * 0.5 + f_px * dx / maxf(height, 1.0)
	var cy: float = half - f_px * dz / maxf(height, 1.0)
	return cx >= float(PATCH) and cx <= float(size.x - PATCH - 1) \
		and cy >= float(PATCH) and cy <= float(size.y) * HUD_SAFE_Y


## The share of DIRECTIONS THE FRAME CAN ANSWER whose ground at this radius the mode
## changed.
func _row(before: PackedByteArray, after: PackedByteArray, size: Vector2i, radius: float,
		height: float) -> float:
	var hit := 0
	var asked := 0
	for s in SECTORS:
		var phi: float = TAU * float(s) / float(SECTORS)
		var dx: float = radius * sin(phi)
		var dz: float = -radius * cos(phi)
		if not _in_frame(size, dx, dz, height):
			continue
		asked += 1
		if _changed(before, after, size, radius, height, s):
			hit += 1
	return float(hit) / float(maxi(asked, 1))


## Takes the far mode's fog out of the measurement. The grid's fog runs from the
## loaded world's edge to its outer ring, and the camera here is a plan view from
## three kilometres up: every pixel of the far field is then beyond the fog's end,
## so the grid draws the fog colour -- which is the sky colour -- and a correct grid
## and a missing one differ by a couple of hundredths. The engine re-pushes these
## only when its own values change, so the override sticks; the readback is printed
## so a silent revert cannot be mistaken for a hole. What this measures is geometry,
## which is the thing under test; the fog's own tuning is not.
func _neutralise_fog() -> String:
	var material = load("res://materials/lod_grid_material.tres")
	if material == null or not (material is ShaderMaterial):
		return "no material"
	material.set_shader_parameter("fog_begin", 0.0)
	material.set_shader_parameter("fog_end", 100000.0)
	return "fog %s..%s" % [str(material.get_shader_parameter("fog_begin")),
		str(material.get_shader_parameter("fog_end"))]


## How high the terrain is under the camera, found by walking a column down from the
## camera until a block appears. Every radius in the profile is scaled by the height
## above it, so this is what makes r = H * tan(angle) the ground and not a guess.
func _surface_y() -> float:
	var start: int = int(CAMERA_Y) - 64
	for y in range(start, 0, -4):
		if int(_cm.call("get_block", 0, y, 0)) != 0:
			return float(y)
	return 0.0


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_ok("Main.tscn loads", false)
		_finish()
		return
	_main = scene.instantiate()
	if _main == null:
		_ok("Main.tscn instantiates", false)
		_finish()
		return
	root.add_child(_main)
	await process_frame
	_cm = _main.get_node_or_null("ChunkManager")
	_player = _main.get_node_or_null("Player")
	if _cm == null or _player == null:
		_ok("ChunkManager and Player exist", false)
		_finish()
		return

	# A camera of this probe's own: the player's is rewritten every frame by the
	# controller, and this one has to be parkable.
	_camera = Camera3D.new()
	_camera.fov = FOV
	_camera.far = 12000.0
	_main.add_child(_camera)
	_camera.make_current()

	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)     # noon: the clearest separation of world and sky
	# Frozen: a drifting sun re-lights every sky pixel a little, which is noise in
	# this measurement, and a probe that does not freeze it measures the drift.
	_cm.set("day_night_cycle_enabled", false)
	_cm.set("lod_grid_enabled", false)
	# The reach is pinned to the ladder's own, because two of this probe's radii are
	# statements ABOUT the ladder: the beyond-horizon check expects sky past 2304
	# blocks, which is where the default reach ends. The settings menu node in the
	# scene loads the PLAYER's settings.cfg, so a reach somebody moved is a reach this
	# measurement would otherwise inherit and fail on -- which it did, once.
	_cm.set("lod_grid_outer_rings", 0)
	# Straight down, over the player: the centre of the frame is the point the
	# world's DISC is centred on, so the profile is concentric with the clip.
	_camera.global_position = Vector3(0.0, CAMERA_Y, 0.0)
	_camera.rotation_degrees = Vector3(-90.0, 0.0, 0.0)
	await _frames(40)             # let the world load under the camera

	# How high the camera is above the terrain: the terrain is a heightfield, so the
	# only way to know is to ask the world. Every radius in the profile is scaled by
	# it, and being wrong by the height of a hill moves the profile by that much.
	var ground: float = _surface_y()
	var height: float = CAMERA_Y - ground
	_ok("the camera is far above the terrain, so the profile is a plan view",
		height > 1000.0, "camera y=%.0f ground y=%.0f height=%.0f" % [CAMERA_Y, ground, height])

	var off_a := await _frame()
	_captures += 1
	await _frames(SETTLE_FRAMES)
	var off_b := await _frame()
	_captures += 1
	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	_ok("the frame is readable", off_a.size() > 0, "%dx%d" % [size.x, size.y])

	var control := 0.0
	for r in RADII:
		control = maxf(control, _row(off_a, off_b, size, float(r), height))
	_ok("control: two frames with the mode off are the same frame", control < MAX_DRIFT,
		"worst drift %.2f over %d radii" % [control, RADII.size()])

	# The mode on, and the wait is for enough tiles that the inner rings exist --
	# the outer ones cost frames, and are reported rather than asserted.
	_cm.set("lod_grid_enabled", true)
	var uploads := 0
	var live := 0
	for i in WAIT_FRAMES:
		await process_frame
		var stats: Dictionary = _cm.get_lod_grid_stats()
		uploads = int(stats.get("uploads", 0))
		live = int(stats.get("tiles", 0))
		if uploads >= WAIT_UPLOADS and uploads >= live - 4:
			break
	await _frames(SETTLE_FRAMES)
	var fog := _neutralise_fog()
	_on_capture = true
	var on := await _frame()
	_captures += 1
	_ok("on: the grid is up", uploads >= WAIT_UPLOADS,
		"uploads=%d of %d tiles live" % [uploads, live])
	_ok("measurement: the far field is drawn without its fog", fog.begins_with("fog 0"), fog)

	# The fog readback is taken after the capture, so a revert mid-frame shows up.
	_ok("measurement: the fog did not come back during the capture",
		float(load("res://materials/lod_grid_material.tres").get_shader_parameter("fog_end")) > 1000.0,
		str(load("res://materials/lod_grid_material.tres").get_shader_parameter("fog_end")))

	var profile := []
	var inner_worst := 1.0
	var inner_mean := 0.0
	var inner_count := 0
	for r in RADII:
		var share: float = _row(off_a, on, size, float(r), height)
		profile.append(share)
		print("probe: radius %5d blocks  changed %.2f of %d directions" % [int(r), share, SECTORS])
		# The radii the square hole used to leave empty: from the world's edge out
		# to where the old inner square ended (a whole 256-block tile, so twice the
		# world's radius in the diagonals).
		if r >= 192 and r <= 448:
			inner_worst = minf(inner_worst, share)
			inner_mean += share
			inner_count += 1
	inner_mean /= float(maxi(inner_count, 1))
	_ok("the ground just past the loaded world is there in EVERY direction",
		inner_worst >= MIN_WORST_CHANGED and inner_mean >= MIN_MEAN_CHANGED,
		"mean %.2f worst %.2f over the radii 192..448 (%s)" % [inner_mean, inner_worst, str(profile)])
	# And the rest of the annulus, mean over the directions: the outer rings are
	# built last and the far ones are fogged toward the sky, so this is a mean.
	var outer_worst := 1.0
	for i in RADII.size():
		if RADII[i] >= 512 and RADII[i] <= 1024:
			outer_worst = minf(outer_worst, profile[i])
	_ok("the rest of the far field is there too", outer_worst >= MIN_MEAN_CHANGED,
		"worst of the radii 512..1024 changed %.2f" % outer_worst)
	# Past the grid's own horizon, the ground has to be sky again: a measurement
	# that reports "changed" out there too is measuring the fog or a lighting drift
	# rather than the mode, and would pass whatever the mode did.
	var beyond := 0.0
	for r in BEYOND:
		beyond = maxf(beyond, _row(off_a, on, size, float(r), height))
	_ok("past the grid's horizon the ground is sky again", beyond < 0.5,
		"worst of %s changed %.2f" % [str(BEYOND), beyond])

	# Cracks: a boundary between two tiles reads as a line of sky when the two
	# surfaces do not meet -- either because they were sampled at different spacings
	# and fanned apart between their shared nodes, or because one of the two tiles is
	# simply not there yet. A tile still in flight is the benign version, so the count
	# is allowed to be explained by the tiles that have not arrived.
	var cracks := _cracks(on, size, height)
	var outstanding := maxi(live - uploads, 0)
	_ok("no tile boundary is a line of sky beyond what a missing tile explains",
		cracks <= outstanding * 4, "%d cracks, %d tiles still building" % [cracks, outstanding])

	_cm.set("lod_grid_enabled", false)
	await _frames(4)
	_ok("off again: the tiles are gone", int(_cm.get_lod_grid_stats().get("tiles", -1)) == 0)

	# --- the camera has to be able to SEE the horizon ----------------------
	# Godot's own far plane is 4000 blocks, so a reach past that is geometry the mode
	# pays for and nobody can see: the outer rings are clipped away and the slider's
	# top half does nothing. The mode raises the camera's plane to its own horizon and
	# gives the old value back when it goes off -- the last part matters, because it is
	# the player's camera and a plane left out at a far field's horizon is a change to
	# the whole world's rendering made by a setting that is switched off.
	# Godot's own default, which is what the player's camera carries: this probe's own
	# camera is set much further out so its plan view is never clipped.
	_camera.far = 4000.0
	var far_before: float = _camera.far
	_cm.set("lod_grid_outer_rings", 10)   # 10 * 256 blocks past the ladder
	_cm.set("lod_grid_enabled", true)
	await _frames(30)
	var horizon := int(_cm.get_lod_grid_stats().get("outer_radius", 0))
	_ok("far plane: the camera reaches past the horizon the mode reports",
		horizon > int(far_before) and _camera.far >= float(horizon) + 128.0,
		"camera far %.0f -> %.0f, horizon %d" % [far_before, _camera.far, horizon])
	_cm.set("lod_grid_enabled", false)
	_cm.set("lod_grid_outer_rings", 0)
	await _frames(4)
	_ok("far plane: switching the mode off gives the player's camera back",
		absf(_camera.far - far_before) < 1.0, "camera far %.0f" % _camera.far)

	# --- the reach has to change what you can SEE --------------------------
	# The reach setting's whole promise is more distance, and a count of tiles cannot
	# show it: the frame has to grow ground at radii the level ladder never reaches.
	# The ladder's own horizon is 2304 blocks here, so these radii are sky with the mode
	# off and ground with it on -- the same off-versus-on patch comparison the rest of
	# this probe uses, aimed where the reach is the only thing that could fill it.
	var reach_off := await _frame()
	_cm.set("lod_grid_outer_rings", 20)     # 20 * 256 blocks of outer rings
	_cm.set("lod_grid_enabled", true)
	var waited := 0
	while waited < 2400:
		await process_frame
		waited += 1
		var st: Dictionary = _cm.get_lod_grid_stats()
		if int(st.get("tiles", 0)) - int(st.get("uploads", 0)) <= 8:
			break
	await _frames(SETTLE_FRAMES)
	_on_capture = true          # so the frame it saves is labelled as an on one
	var reach_on := await _frame()
	_captures += 1
	_on_capture = false
	var deep: Array = [3000, 5000]
	var deep_changed := 0.0
	for r in deep:
		deep_changed = maxf(deep_changed, _row(reach_off, reach_on, size, float(r), height))
	_ok("reach: pushing the reach out puts ground where the ladder had sky",
		deep_changed > 0.5, "worst of %s changed %.2f" % [str(deep), deep_changed])
	_cm.set("lod_grid_enabled", false)
	_cm.set("lod_grid_outer_rings", 0)
	await _frames(4)
	_ok("far plane: and it comes back from a raised reach too",
		absf(_camera.far - 4000.0) < 1.0, "camera far %.0f" % _camera.far)

	print("PROBE lod grid shot: %d failure(s)" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)
