extends SceneTree
## Which of the far field's new terms is putting a speckle on its faces?
##
## A report from the field: "there is now z fighting on every single face, I think
## caused by the biome blending". Two things arrived in the same build as that report
## -- the biome blend's second textured sample, and the detail term's repeat halved
## from 16 blocks to 8 -- and a third possibility is neither, which is what "z
## fighting" literally means: two surfaces at the same depth.
##
## So every candidate is a state of the same camera, and each one is a shot as well as
## a number:
##
##   fade      the term with its footprint fade as it ships, whose window is four
##             times too wide for the rule it claims to copy
##   rule      the same fade moved onto the world's own window
##             (shaders/block_noise.gdshaderinc)
##   adapt     no fade at all: the repeat grows with the distance, so the sampler is
##             never asked for a texel smaller than a pixel
##   flat      no detail term at all (detail_strength 0)
##
## `flat` is the control the others are read against: with no detail sample the far
## field is one mip average per cell -- a smooth gradient of biome colour -- so the
## noise left in it is the frame's own, and any state above it is a term doing it.
## The question this probe now answers is which of `rule` and `adapt` reaches `flat`'s
## SPIKE rate without reaching `flat`'s NOISE as well: the artifact gone is the first,
## a field that has gone flat is the second, and a fix that buys one with the other is
## not a fix. `rule` is the honest minimum; `adapt` is the one that keeps the far
## field textured.
##
## And the two geometries that could genuinely z-fight -- the terrain quad and the
## water quad a cell emits in two independent passes -- would still fight with BOTH
## terms off, which is why `flat` is asserted to be smooth rather than assumed to be.
## The blend was measured the same way and is not in this matrix any more: with the
## real vertex data its contribution to every metric here was 0.0000, and at full
## strength (a patch) it moved the far field's noise by an order of magnitude -- so
## the plumbing works and the weight is simply not where the speckle was.
##
## The metric is what the eye complains about: the mean difference between a pixel and
## the one two along (a pattern's own contrast) and the fraction of pixels that differ
## from BOTH neighbours while those two agree with each other (one pixel of another
## surface, which a texture at a sane scale cannot make).
##
## WINDOWED: it reads the rendered frame, and the dummy renderer has none.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_zfight.gd 600
##
## Shots go to user://lod_grid_shots/ for the eye: zfight_<state>_<yaw>.png.

const RENDER_DISTANCE := 4
## Where the camera sits above the player: high enough that the whole far field is in
## frame. The report's own view is a player on the ground, and this instrument started
## there -- but a probe runs in whatever world the machine has, and a ground view from
## the wrong spot reads a tenth of the frame as far field and the rest as the hill in
## front of it (measured: player y 96 gave 0.41 of the frame, y 192 gave 0.10, and both
## were the same probe). From 140 blocks up the loaded world is a disc a couple of
## hundred blocks below -- four chunks of it at RENDER_DISTANCE -- and everything above
## it is the far field, near to horizon, which is the whole subject: a view that shows
## the band the fade acts on and the band it must leave alone.
const EYE_HEIGHT := 140.0
const PITCH := -6.0
const YAWS := [0, 90, 180, 270]
const FOV := 70.0
## The reach is the one knob that decides whether the far field HAS a distance at all:
## the ladder's own reach ends at ~1,800 blocks, and the report came from a session
## whose reach was pushed out -- the field's own `Far Reach` row, 100 rings, a horizon
## at 27 km. Both are measured here, because a band of ground that only exists at the
## far reach cannot be looked at from the near one.
const REACHES := [100]
const WAIT_FRAMES := 4500
const WAIT_UPLOADS := 40
const SETTLE_FRAMES := 6
const SAMPLE_STEP := 2
## A pixel counts as "the far field drew here" above this summed RGB distance (0..1)
## from the mode-off frame.
const MASK_TOL := 0.03
## A pixel "spikes" when it differs from both neighbours by this much while they agree
## with each other: one pixel of a different colour, which is what a depth fight
## leaves and what a pattern wider than a pixel does not.
const SPIKE := 12.0
const SPIKE_AGREE := 6.0
## The `flat` state is the frame's own floor, and the states above it are read as
## multiples of it rather than against a number: how much a term must add before it is
## worth calling the artifact is the one thing this probe cannot derive.
const PLAIN_NOISE_MAX := 0.004
## The mean colour may not move between two states: the blend mixes toward the other
## biome's average and the detail term is a ratio against the face's own, so neither
## may relight the field, and a state that did would be measuring that instead.
const MEAN_TOL := 0.02
## The first of the two fixes, and the reason the shipped fade underdelivers: the
## world's own grain (shaders/block_noise.gdshaderinc) fades to nothing by TWO texels
## per pixel and is drawn as authored at one, while the shipped window here is written
## in repeats per pixel and reaches full strength at two texels per pixel -- which is
## where the world's rule has already reached zero. A repeat of this term is the same
## 16-texel face (DETAIL_AVERAGE_MIP is the mip at which a face is one texel), so the
## two rules are the same rule in different units and this patch puts the two
## constants where the world's own window puts them: 16 px a repeat is one texel a
## pixel, 8 px is two. Half strength at 12 px a repeat is the ramp's middle.
const RULE_WINDOW := [
	["const float DETAIL_REPEAT_MIN_PX = 2.0;", "const float DETAIL_REPEAT_MIN_PX = 8.0;"],
	["const float DETAIL_REPEAT_FULL_PX = 8.0;", "const float DETAIL_REPEAT_FULL_PX = 16.0;"],
]
## The second family, and the one the first run of this probe pointed at: hold the
## term's own sampling rate constant instead of fading it, by growing the repeat with
## the footprint. The number in each state's name is the screen pixels one texel of the
## face is drawn at, and the first run swept 1, 2 and 4: a texel at ONE pixel is where a
## noise-textured face is at its most salt-and-pepper -- that state measured WORSE than
## the shipped fade (x64 over the control against x35) -- and four pixels a texel was
## x5.5, i.e. most of the artifact gone with four fifths of the term's contrast left.
## 8 and 16 are the same question asked further out, and the higher of the two is also
## the state at which the term has to be checked for having become the flat average
## under another name.
##
## Their one difference from `rule` is what happens past the crossover: it falls back
## to the cell's flat average, these keep the same face at a coarser scale. All of them
## are identical wherever a repeat is already at or above the authored scale.
const FINE_LINE := "vec3 fine = texture(texture_array, vec3(detail_uv, layer), mipmap_bias).rgb;"
const AVERAGE_LINE := "vec3 average = textureLod(texture_array, vec3(detail_uv, layer), DETAIL_AVERAGE_MIP).rgb;"


## The two lines that make a state adaptive, for a chosen number of screen pixels per
## texel of the face: one repeat is sixteen texels, so a repeat has to be
## px_per_texel * 16 pixels wide for a texel to cover px_per_texel of them.
func _adaptive_pairs(px_per_texel: float) -> Array:
	var width := "%.1f" % (px_per_texel * 16.0)
	return [
		["detail_strength * fade", "detail_strength"],
		[FINE_LINE,
			"float spread = max(detail_scale, max(max(footprint.x, footprint.y), 1.0e-4) * %s);\n        vec2 span_uv = uv / spread;\n        vec3 fine = texture(texture_array, vec3(span_uv, layer), mipmap_bias).rgb;" % width],
		[AVERAGE_LINE,
			"vec3 average = textureLod(texture_array, vec3(span_uv, layer), DETAIL_AVERAGE_MIP).rgb;"],
	]


## Every state of the same camera, in the order they are printed: the term's two
## knobs and the shader to draw with. One list, read by the runner and by the
## measurement, so a state added here appears in every reading of the run. The sweep
## is the answer's own shape: the term has to be drawn somewhere between the one pixel
## a texel that is at its most salt-and-pepper and the point at which it has been
## smoothed so far that it is the flat average under another name.
func _specs() -> Array:
	return [
		["fade", 8.0, 0.75, null],
		["rule", 8.0, 0.75, _patch("rule", RULE_WINDOW)],
		["adapt64", 8.0, 0.75, _patch("adapt64", _adaptive_pairs(4.0))],
		["adapt128", 8.0, 0.75, _patch("adapt128", _adaptive_pairs(8.0))],
		["adapt256", 8.0, 0.75, _patch("adapt256", _adaptive_pairs(16.0))],
		["flat", 8.0, 0.0, null],
	]
## The crawl is read over the WHOLE far-field mask, and it used to be read above the
## eye line alone. That band was the wrong one: from a camera 140 blocks above the
## ground the loaded world is a disc 128 blocks across seen from 47 degrees below the
## horizon, which is off the bottom of a 70 degree frame, so every ground pixel this
## camera shows is already beyond the loaded world -- and the band above the eye line
## is sky, which the mask has almost none of (measured: 0.00 to 0.04 of the frame on
## three of four headings, i.e. a crawl metric read over a sliver of its own mask).
## The slide the old band guarded against is the near world's, and the near world is
## not in this frame.
## The temporal half of the question, and the half a still frame cannot answer: the
## report is about a player who is MOVING, and a pattern sampled near the screen's own
## resolution crawls when the camera moves -- the mip the sampler picks flips from one
## pixel to the next as the footprint shifts. A camera moved this many blocks between
## two frames is a sub-pixel change at every distance this field draws (0.02 pixels at
## a kilometre), so anything a pixel does between the two frames is the texture boiling
## rather than geometry moving.
const DRIFT_BLOCKS := 0.05
const SHOT_DIR := "user://lod_grid_shots"

var _failures := 0
var _cm: Node = null
var _player: Node3D = null
var _camera: Camera3D = null
var _material: ShaderMaterial = null
var _shader: Shader = null
var _patched: Dictionary = {}
var _params: Dictionary = {}
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
	img.save_png("%s/zfight_%s.png" % [SHOT_DIR, name])
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


## Mean difference between a pixel and the one two along, over the mask: local
## contrast, which is what a pattern adds and what a flat average has none of.
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


## The fraction of mask pixels that stand out against BOTH neighbours while those two
## agree with each other -- one pixel of a different colour. A pattern wider than a
## pixel cannot make this number move the way a depth fight does, and neither can the
## face average the other states fall back to.
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


func _mean(data: PackedByteArray, idx: PackedInt32Array) -> Array:
	var sr := 0.0
	var sg := 0.0
	var sb := 0.0
	for i in idx:
		sr += float(data[i])
		sg += float(data[i + 1])
		sb += float(data[i + 2])
	var n := float(maxi(idx.size(), 1))
	return [sr / n / 255.0, sg / n / 255.0, sb / n / 255.0]


func _shift(a: Array, b: Array) -> float:
	return maxf(maxf(absf(a[0] - b[0]), absf(a[1] - b[1])), absf(a[2] - b[2]))


## One state's spikes as a multiple of the flat control's: the artifact is gone when
## this reads 1, and a fix that leaves the field no noisier than a mip average has
## bought the artifact away with the picture. Floored so a silent floor reads as a
## multiple rather than as a division by zero.
func _spike_ratio(spikes: float, control: float) -> float:
	return spikes / maxf(control, 1.0e-5)


## Whether two states of the same heading rendered different frames, which is the only
## evidence that a patched shader reached the material at all.
func _differs(f: Dictionary, a: String, b: String) -> bool:
	return absf(f[a] - f[b]) > 1.0e-5 or absf(f["%s_spikes" % a] - f["%s_spikes" % b]) > 1.0e-5


## A patch of the shader's own text, as one or more find/replace pairs applied in
## order -- two states of a program rather than two programs. A frozen string matching
## nothing is reported and the state is dropped rather than silently measuring the
## unpatched shader, which is the failure this instrument is most likely to have: the
## shader it patches is edited by the same hand that edits the probe.
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
			print("probe: the line to patch for '%s' was not found in the shader: %s" % [name, from])
			_patched[name] = null
			return null
		body = next
	var shader := Shader.new()
	shader.code = body
	_patched[name] = shader
	return shader


## Every uniform the material carries, so a swapped shader can be handed the same
## values and the only difference between two states is the line above.
func _snapshot() -> Dictionary:
	var out := {}
	if _material == null or _material.shader == null:
		return out
	for u in _material.shader.get_shader_uniform_list():
		out[String(u["name"])] = _material.get_shader_parameter(String(u["name"]))
	return out


## One state: the detail term's own two knobs, and the shader to draw with.
func _state(scale: float, strength: float, shader: Shader) -> void:
	_material.shader = shader if shader != null else _shader
	for k in _params:
		_material.set_shader_parameter(k, _params[k])
	_material.set_shader_parameter("detail_scale", scale)
	_material.set_shader_parameter("detail_strength", strength)
	await _frames(SETTLE_FRAMES)


## The frame again after the camera has moved DRIFT_BLOCKS to the RIGHT of where it
## is looking: a sub-pixel move at the far field's own distances, so the difference
## between the two frames is what the sampler makes of a footprint that shifted rather
## than what the geometry did. Saved as a shot of the offset frame, which is the state
## a person has to look at to believe the number.
func _drift_frame(name: String) -> PackedByteArray:
	var basis := _camera.global_transform.basis
	_camera.global_position = _eye + basis.x * DRIFT_BLOCKS
	await _frames(3)
	var data := _capture("%s_shift" % name)
	_aim(_camera.rotation_degrees.y)
	await _frames(2)
	return data


## Mean per-pixel difference between two frames of the same state, over the mask: the
## far field's crawl. A surface that is merely textured moves by the sub-pixel shift
## and reads near zero here; one that is sampled finer than the screen can carry
## repaints its pixels instead.
func _drift(a: PackedByteArray, b: PackedByteArray, idx: PackedInt32Array) -> float:
	var total := 0.0
	for i in idx:
		total += (absf(float(a[i]) - float(b[i]))
			+ absf(float(a[i + 1]) - float(b[i + 1]))
			+ absf(float(a[i + 2]) - float(b[i + 2]))) / 3.0 / 255.0
	return total / float(maxi(idx.size(), 1))


## The player spawns in the air and FALLS, so a probe that reads the eye position after
## a fixed number of FRAMES reads a different altitude on every run: at 20 fps the same
## 60 frames are three seconds of falling. Measured the hard way, across runs of this very
## probe: player y 96, 160 and 192, and the high ones were standing in terrain. The eye
## is taken once the fall is over and the height has stopped moving.
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


func _aim(yaw_degrees: float) -> void:
	_camera.global_position = _eye
	_camera.rotation_degrees = Vector3(PITCH, yaw_degrees, 0.0)


func _measure(yaw: float, idx: PackedInt32Array, off: PackedByteArray, reach: int) -> Dictionary:
	_aim(yaw)
	await _frames(SETTLE_FRAMES)
	var specs := _specs()

	var out := {"yaw": yaw, "reach": reach}
	var names := []
	for spec in specs:
		names.append(String(spec[0]))
	out["names"] = names
	var data := {}
	var shifted := {}
	for spec in specs:
		await _state(float(spec[1]), float(spec[2]), spec[3])
		var name := String(spec[0])
		var tag := "%s_r%d_%d" % [name, reach, int(yaw)]
		data[name] = _capture(tag)
		shifted[name] = await _drift_frame(tag)
	await _state(8.0, 0.75, null)
	var mask := _mask(off, data["fade"], idx)
	out["mask"] = mask
	out["coverage"] = float(mask.size()) / float(maxi(idx.size(), 1))
	for name in data:
		out[name] = _noise(data[name], mask)
		out["%s_spikes" % name] = _spikes(data[name], mask)
		out["%s_mean" % name] = _mean(data[name], mask)
		out["%s_drift" % name] = _drift(data[name], shifted[name], mask)
	print("probe: reach %3d yaw %3d  far field %.2f of frame"
		% [reach, int(yaw), out["coverage"]])
	print("probe:        noise  %s" % _line(names, out, ""))
	print("probe:        spikes %s" % _line(names, out, "_spikes"))
	print("probe:        crawl  %s" % _line(names, out, "_drift"))
	return out


## One metric of every state on one line, so a state added to _specs() is in the
## reading without an edit here.
func _line(names: Array, out: Dictionary, suffix: String) -> String:
	var text := ""
	for name in names:
		text += "  %s %.4f" % [name, out["%s%s" % [name, suffix]]]
	return text


## One configuration's whole measurement: the mode off for each heading's own
## reference, then the grid filled at this reach before any state is read.
func _scenario(outer_rings: int, idx: PackedInt32Array) -> Array:
	_cm.set("lod_grid_outer_rings", outer_rings)
	_cm.set("lod_grid_enabled", false)
	var before := {}
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		before[int(yaw)] = _capture("off_r%d_%d" % [outer_rings, int(yaw)])

	_cm.set("lod_grid_enabled", true)
	var stats := {}
	for i in WAIT_FRAMES:
		await process_frame
		stats = _cm.get_lod_grid_stats()
		if int(stats.get("uploads", 0)) >= WAIT_UPLOADS \
				and int(stats.get("uploads", 0)) >= int(stats.get("tiles", 0)) - 32:
			break
	stats = _cm.get_lod_grid_stats()
	_ok("the far field is up at reach %d, so there is something to look at" % outer_rings,
		int(stats.get("uploads", 0)) >= WAIT_UPLOADS,
		"uploads=%d of %d tiles live, %d draw calls, %d quads, horizon %d blocks" % [
			int(stats.get("uploads", 0)), int(stats.get("tiles", 0)),
			int(stats.get("draw_calls", 0)), int(stats.get("quads", 0)),
			int(stats.get("outer_radius", 0))])
	var frames := []
	for yaw in YAWS:
		frames.append(await _measure(float(yaw), idx, before[int(yaw)], outer_rings))
	return frames


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_ok("Main.tscn loads", false)
		_finish()
		return
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
	_camera.far = 12000.0
	main.add_child(_camera)
	_camera.make_current()

	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)
	_cm.set("day_night_cycle_enabled", false)
	_cm.set("lod_grid_enabled", false)
	await _frames(40)

	await _settle_player()
	_aim(0.0)
	_eye = _player.global_position + Vector3(0.0, EYE_HEIGHT, 0.0)
	_aim(0.0)
	await _frames(20)
	_ok("the camera is at the player's eye, on the ground",
		_camera.global_position.y > _player.global_position.y,
		"player y %.1f, camera y %.1f" % [_player.global_position.y, _camera.global_position.y])

	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	_ok("the frame is readable", size.x > 0, "%dx%d" % [size.x, size.y])
	if size.x == 0:
		_finish()
		return
	var idx := _indices(size.x, size.y)

	_material = load("res://materials/lod_grid_material.tres")
	_ok("the far mode's material loads", _material != null)
	if _material == null:
		_finish()
		return
	_shader = _material.shader
	var patched := 0
	var wanted := 0
	for spec in _specs():
		var name := String(spec[0])
		if spec[3] != null:
			patched += 1
		if name != "fade" and name != "flat":
			wanted += 1
	_ok("every patched state reached the shader's own text", patched == wanted,
		"%d patched of %d patched states" % [patched, wanted])
	if patched < wanted or _shader == null:
		_finish()
		return
	# The fog and the water tint are colour and carry no texture, so both are out of
	# the way: the question is the pattern, and neither of them has one.
	_material.set_shader_parameter("fog_begin", 0.0)
	_material.set_shader_parameter("fog_end", 100000.0)
	_material.set_shader_parameter("water_mix", 0.0)
	await _frames(2)
	_params = _snapshot()
	_ok("every uniform the material carries is captured for the swap",
		_params.size() > 8, "%d uniforms" % _params.size())

	# The two reaches, and every heading of each: the report came from the far one, and
	# a term that is invisible from the near one is exactly what has to be looked at.
	var frames := []
	for reach in REACHES:
		frames.append_array(await _scenario(int(reach), idx))
	var best: Dictionary = frames[0]
	var far: Dictionary = frames[0]
	for f in frames:
		if f["coverage"] > best["coverage"]:
			best = f
		if int(f["reach"]) == int(REACHES[-1]) and f["fade_spikes"] > far["fade_spikes"]:
			far = f
	# A view the far field barely reaches is a measurement of the world in front of it, so
	# the floor here is what the probe needs to be worth reading rather than what the mode
	# needs to exist -- the run at player y 192 read a tenth of the frame and its numbers
	# were the valley's.
	_ok("the mask is the far field, not a corner of it", best["coverage"] > 0.15,
		"best heading covers %.2f of the frame at yaw %d" % [best["coverage"], int(best["yaw"])])

	# The controls, taken on the heading that shows the most far field. The first is the
	# control every state is read against: with no detail sample the far field is one face
	# average per cell, so anything above this floor is a term rather than the frame. The
	# second is the instrument's own -- a patched shader that never reached the material
	# would make every reading about the filtering meaningless.
	_ok("control: with no detail term the far field is flat",
		best["flat"] <= PLAIN_NOISE_MAX,
		"noise %.4f with %.4f spike pixels" % [best["flat"], best["flat_spikes"]])
	_ok("control: every patched state renders a frame the shipped term does not",
		_differs(best, "fade", "rule") and _differs(best, "fade", "adapt64")
			and _differs(best, "fade", "adapt128") and _differs(best, "fade", "adapt256"),
		"%s against fade %.4f/%.4f" % [_line(best["names"], best, "_spikes"), best["fade"],
			best["fade_spikes"]])
	var worst_mean := 0.0
	var worst_name := ""
	for name in best["names"]:
		var shift: float = _shift(best["%s_mean" % name], best["flat_mean"])
		if shift > worst_mean:
			worst_mean = shift
			worst_name = name
	_ok("no state of the detail term may relight the far field", worst_mean < MEAN_TOL,
		"the worst is %s at %.4f against the control" % [worst_name, worst_mean])

	# The reading, per reach and heading: what the eye complains about is the speckle and
	# the crawl, and the control says what the frame's own is.
	for f in frames:
		print("probe: reach %3d yaw %3d %s" % [int(f["reach"]), int(f["yaw"]),
			_line(f["names"], f, "_spikes")])
	var ratios := ""
	for name in far["names"]:
		ratios += "  %s x%.1f" % [name, _spike_ratio(far["%s_spikes" % name], far["flat_spikes"])]
	print("probe: the far reach's worst heading (yaw %d): spikes over the control:%s"
		% [int(far["yaw"]), ratios])

	print("PROBE lod grid zfight: %d failure(s)" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)
