extends SceneTree
## Does the far field LOOK like the world it continues -- and does the engine's own
## monitor see what it costs?
##
## Two things the mode's counters cannot answer, and one the monitor answered
## wrongly, so all three are read out of a real frame here:
##
##   1. The far field read as ONE FLAT COLOUR per biome, and the cause is
##      dimensional: a cell is 32..256 blocks wide and the texture coordinate IS the
##      world coordinate (lod/lod_surface.hpp), so the sampler legitimately lands on
##      a mip that has averaged the whole 16x16 face away. shaders/lod_grid.gdshader
##      puts the variation back at a fixed world scale -- as a RATIO against that
##      same face's own average, which is a claim with two halves. Both are measured
##      on the frame the player gets: the spread over the pixels the mode drew has to
##      rise, and the MEAN over those very pixels must not move.
##
##   2. The monitor numbers do not add up to the frame cost, so "the monitor is not
##      picking up the seed mesh" was the natural reading. It is checked instead of
##      argued: RENDER_TOTAL_PRIMITIVES_IN_FRAME with the mode off, then on, against
##      the triangle count the mode says it uploaded. A monitor that moves by the
##      mesh's own triangles has it. And a frame sitting at 60 fps is a CAP, not a
##      cost -- which is why the fps line here is read with vsync disabled, where the
##      number means something.
##
## The camera is the PLAYER's view -- their own position, a little above it, four
## headings -- because the detail term lives and dies on how big a cell is on screen,
## and getting that wrong is a measurement of nothing. Straight down from three
## kilometres, a 256-block cell is thirty pixels and the sampler's own mip for the
## detail scale is already past the face's average, so the ratio is 1 by
## construction: the first run of this probe read a spread rise of x1.00 over a frame
## that turned out to be 464 triangles of deep-ocean water quads. From the player's
## eye the near rings (32 and 64 blocks a cell) cover the frame at a scale where the
## term has something to put back, and four headings are sampled because a world can
## be an ocean one way and a forest the other.
##
## The fog and the water tint are measured OUT of the way: both paint the distance
## with a colour that carries no texture, and the question here is the texture. The
## mask is still "the pixels the mode drew" -- off versus on -- so nothing about the
## world's own geometry has to be classified.
##
##   probes/run_probe_shot.sh probes/probe_lod_grid_look.gd 600
##
## Shots go to user://lod_grid_shots/ for the eye: look_<name>.png.

const RENDER_DISTANCE := 4        # the same loaded radius probe_lod_grid_shot.gd uses
const EYE_HEIGHT := 6.0           # just above the player's head: terrain, not a floor plan
const PITCH := -6.0               # the horizon, the way a player looks at it
const YAWS := [0, 90, 180, 270]   # a world can be an ocean one way and a forest another
const FOV := 70.0
const WAIT_FRAMES := 900          # tile builds are budgeted, so the grid takes frames
const WAIT_UPLOADS := 40
const SETTLE_FRAMES := 6
## Statistics are read from every other pixel: a couple of hundred thousand samples
## is far more than the comparison needs, and the passes over each frame are the
## run's own cost.
const SAMPLE_STEP := 2
## A pixel counts as "the far field drew here" above this summed RGB distance (0..1)
## from the mode-off frame.
const MASK_TOL := 0.03
## The mean colour may not move by this much on any channel: it is the whole point of
## dividing by the face's own average instead of scaling the face.
const MEAN_TOL := 0.02
## ...and the spread has to rise by at least this factor in the best of the four
## headings, on top of beating the frame-to-frame noise by NOISE_MULT.
const RISE := 1.25
const NOISE_MULT := 4.0
const SHOT_DIR := "user://lod_grid_shots"

var _failures := 0
var _cm: Node = null
var _main: Node = null
var _player: Node3D = null
var _camera: Camera3D = null
var _material: ShaderMaterial = null
var _eye := Vector3.ZERO


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _frames(count: int) -> void:
	for i in count:
		await process_frame


## The frame, and the shot of it: a look measurement with no picture beside it cannot
## be argued with by eye, and the picture is what the player's complaint was about.
func _capture(name: String) -> PackedByteArray:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return PackedByteArray()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	img.save_png("%s/look_%s.png" % [SHOT_DIR, name])
	return img.get_data()


## Every SAMPLE_STEP-th pixel of the frame, as byte offsets into RGBA8 data.
func _indices(w: int, h: int) -> PackedInt32Array:
	var out := PackedInt32Array()
	out.resize((w / SAMPLE_STEP) * (h / SAMPLE_STEP))
	var n := 0
	for y in range(0, h, SAMPLE_STEP):
		for x in range(0, w, SAMPLE_STEP):
			out[n] = (y * w + x) * 4
			n += 1
	return out


## The pixels the far mode drew, which is the measurement's whole population: a pixel
## the mode did not reach cannot say anything about what the mode looks like.
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


## Mean absolute deviation from the mean, averaged over the three channels: "how much
## variation is in these pixels", which is exactly what a flat sheet has none of.
func _spread(data: PackedByteArray, idx: PackedInt32Array, mean: Array) -> float:
	var total := 0.0
	for i in idx:
		total += (absf(float(data[i]) / 255.0 - mean[0])
			+ absf(float(data[i + 1]) / 255.0 - mean[1])
			+ absf(float(data[i + 2]) / 255.0 - mean[2])) / 3.0
	return total / float(maxi(idx.size(), 1))


## The same measure between two frames of the SAME state: the temporal jitter floor a
## real rise has to clear.
func _drift(a: PackedByteArray, b: PackedByteArray, idx: PackedInt32Array) -> float:
	var total := 0.0
	for i in idx:
		total += (absf(float(a[i]) - float(b[i]))
			+ absf(float(a[i + 1]) - float(b[i + 1]))
			+ absf(float(a[i + 2]) - float(b[i + 2]))) / 3.0 / 255.0
	return total / float(maxi(idx.size(), 1))


func _monitor() -> Dictionary:
	return {
		"draw": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		"prims": Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME),
		"objects": Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME),
		"fps": Performance.get_monitor(Performance.TIME_FPS),
	}


## Waits until the grid has the tiles the frame needs, the way the geometry probe
## does: the builds are budgeted per frame, so a capture taken too early is a
## measurement of a half-empty far field.
func _wait_for_grid() -> Dictionary:
	var stats := {}
	for i in WAIT_FRAMES:
		await process_frame
		stats = _cm.get_lod_grid_stats()
		if int(stats.get("uploads", 0)) >= WAIT_UPLOADS \
				and int(stats.get("uploads", 0)) >= int(stats.get("tiles", 0)) - 4:
			break
	return stats


func _aim(yaw_degrees: float) -> void:
	_camera.global_position = _eye
	_camera.rotation_degrees = Vector3(PITCH, yaw_degrees, 0.0)


## One heading: flat, then with the detail term on -- and the numbers the two claims
## are read off. `off` is the same heading with the mode off, taken before it was ever
## switched on: toggling the mode between headings would drop the tiles every time and
## every capture after the first would be of a half-built far field.
func _measure(yaw: float, off: PackedByteArray, idx: PackedInt32Array) -> Dictionary:
	_aim(yaw)
	await _frames(SETTLE_FRAMES)
	_material.set_shader_parameter("detail_strength", 0.0)
	await _frames(SETTLE_FRAMES)
	var flat := _capture("flat_%d" % int(yaw))
	_material.set_shader_parameter("detail_strength", 0.75)
	await _frames(SETTLE_FRAMES)
	var detail := _capture("detail_%d" % int(yaw))

	var mask := _mask(off, flat, idx)
	var mean_flat := _mean(flat, mask)
	var mean_detail := _mean(detail, mask)
	var spread_flat := _spread(flat, mask, mean_flat)
	var spread_detail := _spread(detail, mask, mean_detail)
	var shift := maxf(maxf(absf(mean_flat[0] - mean_detail[0]),
		absf(mean_flat[1] - mean_detail[1])), absf(mean_flat[2] - mean_detail[2]))
	var out := {
		"yaw": yaw,
		"coverage": float(mask.size()) / float(maxi(idx.size(), 1)),
		"shift": shift,
		"spread_flat": spread_flat,
		"spread_detail": spread_detail,
		"rise": spread_detail / maxf(spread_flat, 1.0e-6),
		"mask": mask,
		"detail": detail,
	}
	print("probe: yaw %3d  far field %.2f of frame  spread %.4f -> %.4f (x%.2f)  mean shift %.4f"
		% [int(yaw), out["coverage"], spread_flat, spread_detail, out["rise"], shift])
	return out


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

	# A camera of this probe's own: the player's is rewritten every frame, and this one
	# has to be parkable and aimable.
	_camera = Camera3D.new()
	_camera.fov = FOV
	_camera.far = 12000.0
	_main.add_child(_camera)
	_camera.make_current()

	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)
	_cm.set("day_night_cycle_enabled", false)   # a drifting sun re-lights every pixel
	_cm.set("lod_grid_outer_rings", 0)          # the ladder's own reach: the standard look
	_cm.set("lod_grid_enabled", false)
	await _frames(40)

	# The player's own spot, a little above their head: the far field starts where the
	# loaded world ends, and looking out from here is the view the complaint was about.
	_aim(0.0)
	_eye = _player.global_position + Vector3(0.0, EYE_HEIGHT, 0.0)
	_aim(0.0)
	await _frames(20)
	_ok("the camera is at the player's eye, looking at the horizon",
		absf(_camera.global_position.y - _player.global_position.y - EYE_HEIGHT) < 1.0,
		"player y %.1f, camera y %.1f" % [_player.global_position.y, _camera.global_position.y])

	# Every heading's own "before": the mode has never been on at this point, so the
	# far field is absent by construction and the mask below is exactly its footprint.
	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	_ok("the frame is readable", size.x > 0, "%dx%d" % [size.x, size.y])
	if size.x == 0:
		_finish()
		return
	var before := {}
	for yaw in YAWS:
		_aim(float(yaw))
		await _frames(SETTLE_FRAMES)
		before[int(yaw)] = _capture("off_%d" % int(yaw))
	var idx := _indices(size.x, size.y)

	_cm.set("lod_grid_enabled", true)
	var stats: Dictionary = await _wait_for_grid()
	await _frames(SETTLE_FRAMES)
	stats = _cm.get_lod_grid_stats()
	_ok("the far field is up, so there is something to look at",
		int(stats.get("uploads", 0)) >= WAIT_UPLOADS,
		"uploads=%d of %d tiles live, %d draw calls, %d quads" % [int(stats.get("uploads", 0)),
			int(stats.get("tiles", 0)), int(stats.get("draw_calls", 0)),
			int(stats.get("quads", 0))])

	# The fog and the water tint are colour, and neither carries a texture: left on,
	# both answer for the texture term and the measurement below would be of them.
	_material = load("res://materials/lod_grid_material.tres")
	if _material == null:
		_ok("the far mode's material loads", false)
		_finish()
		return
	_material.set_shader_parameter("fog_begin", 0.0)
	_material.set_shader_parameter("fog_end", 100000.0)
	_material.set_shader_parameter("water_mix", 0.0)
	_material.set_shader_parameter("water_color", Vector3(1.0, 1.0, 1.0))
	await _frames(2)
	# Read back, because the engine pushes this mode's own fog every frame it changes:
	# a silent revert would turn this into a measurement of the fog.
	_ok("measurement: the far field is drawn without its fog or its water tint",
		float(_material.get_shader_parameter("fog_end")) > 1000.0
			and float(_material.get_shader_parameter("water_mix")) == 0.0,
		"fog_end %s water_mix %s" % [str(_material.get_shader_parameter("fog_end")),
			str(_material.get_shader_parameter("water_mix"))])

	var frames := []
	for yaw in YAWS:
		frames.append(await _measure(float(yaw), before[int(yaw)], idx))

	# The claim is "the term puts the variation back where the surface has variation",
	# so the heading that shows it is the measurement; a heading whose far field happens
	# to be one flat body of water is a sample of a surface with nothing to put back.
	var best: Dictionary = frames[0]
	for f in frames:
		if f["rise"] > best["rise"]:
			best = f
	_ok("the mask is the far field, not a corner of it", best["coverage"] > 0.02,
		"best heading covers %.2f of the frame" % best["coverage"])
	_ok("the detail term cannot move the mean colour", best["shift"] < MEAN_TOL,
		"worst channel %.4f at yaw %d" % [best["shift"], int(best["yaw"])])

	# The noise floor is a second capture of the SAME state, in the same heading.
	_material.set_shader_parameter("detail_strength", 0.75)
	_aim(best["yaw"])
	await _frames(SETTLE_FRAMES)
	var again := _capture("detail_%d_b" % int(best["yaw"]))
	var noise := _drift(best["detail"], again, best["mask"])
	print("probe: frame-to-frame noise over the far field, same state: %.4f" % noise)
	_ok("the detail term puts the texture's variation back",
		best["rise"] > RISE and best["spread_detail"] - best["spread_flat"] > noise * NOISE_MULT,
		"x%.2f spread at yaw %d, %.4f above a %.4f noise floor" % [best["rise"],
			int(best["yaw"]), best["spread_detail"] - best["spread_flat"], noise])

	# The material's `mipmap_bias` blurs the detail sample too, while the average it is
	# divided by is taken at an explicit mip: a bias therefore eats the very separation
	# the term lives on. Measured rather than argued -- the number decides the shipped
	# value, and the shot beside it shows what the difference looks like.
	_material.set_shader_parameter("mipmap_bias", 0.0)
	await _frames(SETTLE_FRAMES)
	var unbiased := _capture("detail_bias0")
	print("probe: mipmap_bias 1.0 spread %.4f, 0.0 spread %.4f" % [best["spread_detail"],
		_spread(unbiased, best["mask"], _mean(unbiased, best["mask"]))])
	_material.set_shader_parameter("mipmap_bias", 1.0)

	# --- does the engine's own monitor see the far mesh? -------------------
	# With vsync on, a frame that costs nothing and a frame that costs a little are both
	# "60 fps", which is the reading that started this: a cap is not a cost.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_cm.set("lod_grid_enabled", false)
	await _frames(30)
	var mon_off := _monitor()
	_cm.set("lod_grid_enabled", true)
	stats = await _wait_for_grid()
	await _frames(30)
	var mon_on := _monitor()
	stats = _cm.get_lod_grid_stats()
	var triangles := float(stats.get("vertices", 0)) / 3.0
	print("probe: monitor off  draw %d  primitives %d  objects %d  fps %.0f" % [
		mon_off["draw"], mon_off["prims"], mon_off["objects"], mon_off["fps"]])
	print("probe: monitor on   draw %d  primitives %d  objects %d  fps %.0f" % [
		mon_on["draw"], mon_on["prims"], mon_on["objects"], mon_on["fps"]])
	print("probe: the mode reports %d triangles over %d draw calls" % [
		int(triangles), int(stats.get("draw_calls", 0))])
	_ok("the far mesh is in the engine's own monitor, not missing from it",
		float(mon_on["prims"]) - float(mon_off["prims"]) > triangles * 0.5,
		"primitives %.0f -> %.0f for %d triangles" % [mon_off["prims"], mon_on["prims"],
			int(triangles)])
	_ok("and the far field's reach costs nothing per tile: one draw call per level",
		int(stats.get("draw_calls", 0)) <= 4 and int(stats.get("draw_calls", 0)) > 0,
		"%d draw calls for %d tiles" % [int(stats.get("draw_calls", 0)),
			int(stats.get("tiles", 0))])
	_ok("a settle, not a stall: the merge is over by the time the tiles are up",
		float(stats.get("merge_ms", 0.0)) < 2.0,
		"merge %.3f ms/frame, %d columns sampled, %d asks answered from the table" % [
			float(stats.get("merge_ms", 0.0)), int(stats.get("columns_sampled", 0)),
			int(stats.get("cache_hits", 0))])
	print("probe: fps off %.0f vs on %.0f with vsync disabled, %d primitives to %d" % [
		mon_off["fps"], mon_on["fps"], mon_off["prims"], mon_on["prims"]])

	print("PROBE lod grid look: %d failure(s)" % _failures)
	_finish()


func _finish() -> void:
	quit(1 if _failures > 0 else 0)
