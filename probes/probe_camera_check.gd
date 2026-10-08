extends SceneTree
## Which camera draws the frames, and where does the sea plane land on screen?
##
## Every row figure measured off a shot in this mode rests on two assumptions: that the
## camera the probe made current is still the one drawing (the player's own camera takes
## "current" on ready and may take it again later), and that a level camera at the placed
## eye puts a plane at y=200 below the middle of the frame. A plane BELOW the eye cannot
## paint above the horizon, so if a shot shows the far field's sheet up there, one of the
## two is false -- and this prints both, from the placed eye, in the same window size the
## shot probes use.
##
## WINDOWED (the projection is asked of the real viewport):
##
##   probes/run_probe_shot.sh probes/probe_camera_check.gd 240

const WORLD_SEED := 1337
const RENDER_DISTANCE := 4
const EYE_HEIGHT := 1.6
const FOV := 70.0
const SEA_LEVEL := 200.0

var _failures := 0
var _cm: Node = null
var _cam: Camera3D = null
var _player: Node = null


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _ground_y(x: int, z: int) -> int:
	for y in range(1023, 0, -1):
		if int(_cm.call("get_block", x, y, z)) != 0:
			return y + 1
	return 0


func _who(label: String) -> void:
	var current := root.get_camera_3d()
	var mine := current == _cam
	print("probe: %-22s current=%s (%s) at %s rot %s" % [label,
		str(current.get_path()) if current else "none",
		"the probe's" if mine else "NOT the probe's",
		str(current.global_position) if current else "-",
		str(current.rotation_degrees) if current else "-"])
	if _player != null:
		var pcam: Node = _player.get_node("Camera3D")
		if pcam != null and pcam == current:
			print("probe:    ...and that is the PLAYER's camera, at %s rot %s"
				% [str(pcam.global_position), str(pcam.rotation_degrees)])
	_ok("the probe's camera is the one drawing the frame (%s)" % label, mine,
		"current is %s" % (str(current.get_path()) if current else "none"))


func _rows(tag: String) -> void:
	var size := Vector2i(root.get_texture().get_width(), root.get_texture().get_height())
	var fwd := -_cam.global_transform.basis.z
	var level_line := _cam.global_position - _cam.global_transform.basis.z * 0.0
	for reach: float in [300.0, 500.0, 1000.0, 5000.0, 20000.0]:
		var sea: Vector3 = Vector3(0.0, SEA_LEVEL, 0.0) + fwd * reach
		print("probe: %s sea plane %6.0f blocks out -> row %6.1f" % [tag, reach,
			_cam.unproject_position(sea).y])
	var far_line: Vector3 = level_line + fwd * 20000.0
	print("probe: %s the level line at 20 km -> row %6.1f (frame %dx%d, middle %.1f)"
		% [tag, _cam.unproject_position(far_line).y, size.x, size.y, float(size.y) * 0.5])
	print("probe: %s the eye %s, fov %.1f, pitch %s" % [tag, str(_cam.global_position),
		_cam.fov, str(_cam.rotation_degrees.x)])


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	_cm = main.get_node_or_null("ChunkManager")
	_player = main.get_node_or_null("Player")
	if _cm == null:
		_ok("ChunkManager exists", false)
		quit(1)
		return
	_cm.set("seed", WORLD_SEED)
	_cm.set("render_distance", RENDER_DISTANCE)
	_cam = Camera3D.new()
	_cam.fov = FOV
	_cam.far = 27392.0
	main.add_child(_cam)
	_cam.make_current()
	await _frames(5)
	# The player spawns in the air and falls; the probe places its eye where the ground is.
	var stood := 0
	var last: float = _player.global_position.y if _player else 0.0
	for _i in 900:
		await process_frame
		var now: float = _player.global_position.y if _player else 0.0
		if absf(now - last) < 0.01:
			stood += 1
			if stood >= 10:
				break
		else:
			stood = 0
		last = now
	var ground := _ground_y(0, 0)
	_ok("the ground under the pinned seed is found", ground > 0, "the first air above y %d" % ground)
	_cam.global_position = Vector3(0.0, float(ground) + EYE_HEIGHT, 0.0)
	_cam.rotation_degrees = Vector3(0.0, 90.0, 0.0)
	await _frames(10)
	_who("after the settle")
	_rows("placed")

	# ...and again with the far field running, which is when the shots are taken.
	_cm.set("lod_grid_enabled", true)
	_cm.set("lod_grid_spacing", 128)
	_cm.set("lod_grid_outer_rings", 6)
	for _i in 600:
		await process_frame
	_who("with the far field on")
	_rows("far field on")

	print("PROBE camera check: %d failures" % _failures)
	quit(1 if _failures > 0 else 0)


func _frames(count: int) -> void:
	for _i in count:
		await process_frame
