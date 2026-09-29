extends SceneTree
## Does the bent world still get drawn?
##
## World Bend moves the world's vertices in a vertex shader, and a vertex shader
## runs after the engine has decided what to draw: every chunk is culled against
## a box describing where it *was*. With the bend at its defaults a chunk two
## hundred blocks out is drawn 27 blocks nearer and 43 blocks higher than its box
## says, so the effect culls away the far field it exists to show - and looking
## down, where the whole effect lives, the missing ring is obvious.
##
## probe_bend_geo.gd measures where the geometry goes. This one measures what the
## *renderer* did with it, which is a different question with a different number:
## how many primitives the frame actually drew.
##
## Runs WINDOWED through .freebuff/run_probe_shot.sh - a culled chunk is only
## missing in a real frame.
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_bend_cull.gd [timeout]
##
## Four states, one camera, one settled world:
##
##   A  bend off                                  - what the world costs to draw
##   B  bend on, engine not told (= the bug)      - the same geometry, moved
##   C  bend on, engine told (through the overlay)- the compensation
##   D  bend off again, engine still told         - is the box ever given back?
##
## and the four things they have to say:
##   - B draws about what A draws. That is the bug, stated exactly: the world
##     moved and the engine's work did not change, so the geometry it drew went
##     somewhere else and what stayed put was culled.
##   - C draws *more* than B, by a lot - which is also the proof that the overlay
##     got the knobs to the engine, since B is the same shader with only that
##     hand-off missing.
##   - C's frame differs from B's where the holes were, and the pictures are kept
##     for the report.
##   - D draws what A draws: switching the bend off gives the boxes back, so a
##     world that was bent and is not any more is not left drawing outside
##     itself, and a player who never turns the effect on pays nothing for it.

const SHOT_DIR := "user://shader_shots"
const OVERLAY_PATH := "HUD/ShaderOverlay"
const BEND_ID := "bend"
const AMOUNT := 1.0
const RADIUS := 128.0
const RISE := 1.0
const CAMERA_HEIGHT := 320.0
const PITCH := -78.0
const SAMPLES := 5

var main: Node3D
var cm: Node
var player: Node3D
var camera: Camera3D
var overlay: Control
var failures := 0
var shots := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(1280, 720))
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)

	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		_fail("Main.tscn missing")
		_finish()
		return
	main = scene.instantiate()
	root.add_child(main)
	await process_frame
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	overlay = main.get_node_or_null(OVERLAY_PATH)
	if cm == null or player == null or overlay == null:
		_fail("ChunkManager / Player / %s missing" % OVERLAY_PATH)
		_finish()
		return
	if player.get_node_or_null("Camera3D") == null:
		_fail("Player/Camera3D missing")
		_finish()
		return

	_hide_hud()
	# The player's own camera is no use here: the controller writes its eye height
	# and its look direction every frame, so it cannot be parked. A camera of this
	# probe's own is nothing else's business, and being current makes it the one
	# the engine renders - and the one the chunk manager streams around, so the
	# world below it is loaded rather than the column the player is standing in.
	camera = Camera3D.new()
	camera.fov = 70.0
	camera.far = 4000.0
	main.add_child(camera)
	camera.make_current()

	# High enough that the ground fills the frame out to two hundred blocks and
	# more, and looking straight down at it: the ring of ground the bend pulls in
	# is then the frame's outer band, which is exactly where the frustum's edge is.
	await _settle_world()
	camera.global_position = player.global_position + Vector3(0.0, CAMERA_HEIGHT, 0.0)
	camera.rotation_degrees = Vector3(PITCH, 0.0, 0.0)
	# The world follows the camera, so let it arrive there before measuring.
	await _settle_world()
	await _settle()

	# And the frame has to have a world in it. A frame of sky is a gradient with a
	# little noise in it; terrain is detail at every scale, which is the one thing
	# that separates them without knowing what the world looks like. Checked here
	# rather than trusted, because every number below is a comparison between two
	# of these frames and two frames of sky compare perfectly.
	var detail := _detail(await _capture())
	print("probe: camera at %s looking %.0f down, frame detail %.4f"
		% [camera.global_position, PITCH, detail])
	if detail < 0.01:
		_fail("the frame is sky: the camera is not over the world (detail %.4f)" % detail)

	# A: the world as it is drawn with the effect switched off.
	await _off()
	var base := await _measure("bend_cull_off")

	# B: the shader bends and the engine is not told. This is the state the bug
	# lives in, and the state the effect was in before any of this existed.
	# The take-back comes *after* the drain, because the overlay queues its
	# hand-off: undoing it before the queue has run would leave the queued push
	# to undo the undo, and B would quietly become C.
	await _bend_on()
	cm.call("set_world_bend", false, AMOUNT, RADIUS, RISE)
	var bug := await _measure("bend_cull_bug")

	# C: the same shader, with the overlay's own hand-off to the engine intact.
	await _off()
	await _bend_on()
	var fixed := await _measure("bend_cull_fixed")

	# D: off again, and the boxes have to come back.
	await _off()
	var after := await _measure("bend_cull_after")

	print("probe: primitives drawn  A bend off %d | B bend on, engine blind %d | C compensated %d | D off again %d"
		% [base["primitives"], bug["primitives"], fixed["primitives"], after["primitives"]])
	print("probe: draw calls         A %d | B %d | C %d | D %d"
		% [base["calls"], bug["calls"], fixed["calls"], after["calls"]])

	# The bug's signature: the world moved and the renderer's work did not.
	var moved_share := float(bug["primitives"]) / maxf(float(base["primitives"]), 1.0)
	print("probe: with the world bent and the engine blind the renderer draws %.1f%% of what it did"
		% (moved_share * 100.0))
	if moved_share < 0.9 or moved_share > 1.1:
		_fail("the engine's work changed by %.0f%% when only the shader moved the world"
			% ((moved_share - 1.0) * 100.0))

	# The fix: more of the world is drawn, because more of it is genuinely in view.
	var gained := float(fixed["primitives"]) / maxf(float(bug["primitives"]), 1.0)
	print("probe: compensating draws %.1f%% more geometry (%d -> %d primitives)"
		% [(gained - 1.0) * 100.0, bug["primitives"], fixed["primitives"]])
	if gained < 1.05:
		_fail("the compensation draws only %.1f%% more of the world: the boxes are not reaching the chunks"
			% ((gained - 1.0) * 100.0))

	# And the holes really are filled: the two frames are the same world, so what
	# differs between them is ground that one of them did not draw. This is the
	# *corroborating* measurement and it is a lower bound on the effect, not the
	# effect: how much of the recovered ring is visible depends on where the world
	# happens to have spawned the player. Measured over a mountain at 252 blocks
	# with the fog starting at 896, the recovered far field is nearly all fog and a
	# percent of the frame changes; measured over water with a view to the horizon,
	# the same code fills in 8.3% of it. The load-bearing numbers are the
	# primitives and the draw calls above, which do not depend on the view; this one
	# only has to show that the compensation is not drawing everything off screen.
	var holes := _mean_diff(bug["image"], fixed["image"])
	var share := _changed_share(bug["image"], fixed["image"])
	print("probe: the compensated frame differs from the blind one by %.4f mean, %.1f%% of pixels"
		% [holes, 100.0 * share])
	if holes < 0.0005:
		_fail("the compensation changes the frame by only %.4f: nothing it drew was on screen" % holes)

	# Off has to mean off: the same geometry as A, so a player who never switches
	# the bend on is not paying for its boxes, and one who switches it off gets
	# the world's own culling back.
	var released := float(after["primitives"]) / maxf(float(base["primitives"]), 1.0)
	print("probe: switching the bend off leaves the renderer drawing %.1f%% of what it started with"
		% (released * 100.0))
	if absf(released - 1.0) > 0.03:
		_fail("switching the bend off left the renderer drawing %.1f%% of its own geometry"
			% (released * 100.0))

	_finish()


# --- The states ----------------------------------------------------------------

func _off() -> void:
	overlay.call("set_enabled", BEND_ID, false)
	# The overlay's hand-off to the engine is deferred, so what it pushes has not
	# happened yet on the frame this returns in.
	for i in range(3):
		await process_frame


## The effect switched on entirely through the overlay - the same uniforms the
## live game sets - and the overlay's queued hand-off to the engine drained, so
## that by the time this returns the engine has been told about the bend. B is
## the one that then takes that back; C keeps it.
func _bend_on() -> void:
	overlay.call("set_enabled", BEND_ID, true)
	overlay.call("set_value", BEND_ID, "world_bend", AMOUNT)
	overlay.call("set_value", BEND_ID, "world_bend_radius", RADIUS)
	overlay.call("set_value", BEND_ID, "world_bend_rise", RISE)
	for i in range(4):
		await process_frame


func _finish() -> void:
	if overlay != null and is_instance_valid(overlay):
		overlay.call("set_enabled", BEND_ID, false)
	print("probe: %d shots, %d failures" % [shots, failures])
	quit(1 if failures > 0 else 0)


# --- Reading the renderer ------------------------------------------------------

## What one state of the world costs to draw: the frame's own primitives and draw
## calls, and a picture of it. The monitors are per frame and the world is not
## perfectly still, so each is the largest of a few frames - the most the state
## was ever drawing, which is the number that is about the culling rather than
## about a frame that happened to be mid-stream.
func _measure(name: String) -> Dictionary:
	await _settle()
	var calls := 0
	var primitives := 0
	for i in range(SAMPLES):
		calls = maxi(calls, int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)))
		primitives = maxi(primitives,
			int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)))
		await process_frame
	var image := _capture()
	_save(image, name)
	return {"calls": calls, "primitives": primitives, "image": image}


func _hide_hud() -> void:
	# The world is the measurement; the HUD is not part of it. The overlay and the
	# menu stay (the menu is closed, and hiding the node whose page the overlay's
	# knobs are read from would be inventing work).
	for child in overlay.get_parent().get_children():
		if child == overlay or not child is CanvasItem or child.name == "SettingsMenu":
			continue
		(child as CanvasItem).visible = false


## The world has to be there before any of this means anything. Two waits, for
## two different reasons: the player has to have landed, or the camera hangs over
## a column that is still being generated and the frame is sky; and the streaming
## has to have stopped, or the primitives drawn are mostly the streaming rather
## than the culling.
func _settle_world() -> void:
	var landed := 0
	while landed < 2400 and not player.is_on_floor():
		await process_frame
		landed += 1

	var waited := 0
	var quiet := 0
	while waited < 4000 and quiet < 30:
		await process_frame
		waited += 1
		quiet = 0 if bool(cm.call("has_pending_mesh_work")) else quiet + 1
	print("probe: the player landed after %d frame(s) at %s; the world was quiet after %d more"
		% [landed, player.global_position, waited])


func _settle() -> void:
	for i in range(4):
		await process_frame


## How much there is in a frame, as the mean step between neighbouring pixels: a
## gradient is smooth and a world is not.
func _detail(img: Image) -> float:
	var data := img.get_data()
	var w := img.get_width()
	var h := img.get_height()
	var total := 0.0
	var count := 0
	for y in range(h / 4, h / 4 * 3, 2):
		for x in range(w / 4, w / 4 * 3, 2):
			total += absf(_lum(data, w, x, y) - _lum(data, w, x + 2, y))
			total += absf(_lum(data, w, x, y) - _lum(data, w, x, y + 2))
			count += 2
	return total / maxf(float(count), 1.0)


func _lum(data: PackedByteArray, w: int, x: int, y: int) -> float:
	var i := (y * w + x) * 4
	return (0.2126 * float(data[i]) + 0.7152 * float(data[i + 1]) + 0.0722 * float(data[i + 2])) / 255.0


func _capture() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _mean_diff(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var total := 0.0
	var count := 0
	for i in range(0, pa.size() - 3, 16):
		total += absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
			+ absf(int(pa[i + 2]) - int(pb[i + 2]))
		count += 3
	return total / (255.0 * maxf(float(count), 1.0))


func _changed_share(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return 0.0
	var changed := 0
	var count := 0
	for i in range(0, pa.size() - 3, 16):
		if absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
				+ absf(int(pa[i + 2]) - int(pb[i + 2])) > 12:
			changed += 1
		count += 1
	return float(changed) / maxf(float(count), 1.0)


func _save(img: Image, name: String) -> void:
	img.save_png("%s/%s_%dx%d.png" % [SHOT_DIR, name, img.get_width(), img.get_height()])
	shots += 1


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)
