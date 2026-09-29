extends SceneTree
## Windowed probe: does a translucent MultiMesh overlay (the wand's ghost, and by
## the same code the pathfinding route cubes) disappear as the camera ANGLE
## changes?
##
## The report is "the blue ghost vanishes at random weird angles". Two causes look
## identical from the player's seat, so this separates them by measuring every
## angle instead of trusting one:
##
##   * the overlay is not there at all (our logic hid it — `has_preview()` false,
##     or the node's `visible` false), or
##   * it is there and the RENDERER dropped it (the node says visible, has cells,
##     and the frame has no ghost pixels).
##
## The ghost is repainted pure magenta for the measurement: the real colour is a
## 26%-alpha pale blue that blends into sky, terrain and water, and a pixel count
## of "sort of blue" cannot tell the ghost from the sky.
##
## The camera is detached (`top_level`) and driven on a circle around the ghost's
## centre, looking straight at it, so "on screen" is guaranteed and the only thing
## changing is the approach angle.
##
## Run: .freebuff/run_probe_shot.sh .freebuff/probe_overlay_angle.gd

const FILE := "church.schematic"

var main: Node3D
var cm: Node
var player: Node3D
var wand: Node
var camera: Camera3D
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _surface_point(from: Vector3) -> Vector3:
	var x := int(floor(from.x))
	var z := int(floor(from.z))
	var y := int(from.y) + 64
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(x, y + 1, z)
		y -= 1
	return Vector3.INF

## Magenta pixels in a downscaled copy of the frame: a 64x36 readback is a few
## thousand pixel reads per angle instead of three quarters of a million.
func _ghost_pixels() -> int:
	var img: Image = root.get_texture().get_image()
	if img == null:
		return -1
	img.resize(64, 36, Image.INTERPOLATE_BILINEAR)
	var n := 0
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			if c.r > 0.35 and c.b > 0.35 and c.g < 0.45:
				n += 1
	return n

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	wand = main.get_node_or_null("HUD/Wand")
	camera = player.get_node_or_null("Camera3D")
	if cm == null or player == null or wand == null or camera == null:
		_fail("ChunkManager / Player / Camera3D / HUD/Wand missing")
		quit(1)
		return

	var wand_id := BlockTextures.get_block_id_by_name("wand")
	for _attempt in 60:
		player.set_hotbar_slot(0, wand_id, 1)
		player.select_hotbar_slot(0)
		await process_frame
		if int(player.get_selected_block()) == wand_id:
			break
	if not player.is_wand_held():
		_fail("the wand is not held, so the ghost would not survive a frame")

	# Let the player land so the ghost is anchored on real ground.
	var waited := 0
	while waited < 1200:
		await process_frame
		waited += 1
		if waited > 30 and player.is_on_floor():
			break

	if not wand.choose_file(FILE):
		_fail("could not read %s" % FILE)
		quit(1)
		return
	var frames := 0
	while frames < 1200 and not wand._build_ready():
		await process_frame
		frames += 1
	if not wand._build_ready():
		_fail("%s never finished being read" % FILE)
		quit(1)
		return

	var base := _surface_point(player.global_position)
	var origin := Vector3i(int(base.x) - 14, int(base.y) + 6, int(base.z) - 16)
	var info: Dictionary = wand.anchor_preview_at(origin)
	if not info.get("ok", false):
		_fail("the ghost could not be aimed: %s" % info.get("error", "?"))
		quit(1)
		return
	var lo: Vector3i = info.get("min", origin)
	var hi: Vector3i = info.get("max", origin)
	var centre := Vector3((lo.x + hi.x) * 0.5 + 0.5, (lo.y + hi.y) * 0.5 + 0.5,
		(lo.z + hi.z) * 0.5 + 0.5)
	print("probe: ghost %d cells, volume %s..%s, centre %s" % [int(info.get("planned", 0)), lo, hi, centre])

	# Paint it unmistakable for the measurement (the probe owns this node).
	var ghost: MultiMeshInstance3D = wand._ghost
	ghost.material_override.albedo_color = Color(1.0, 0.0, 1.0, 1.0)

	# The camera leaves the player for the measurement: the ghost must stay on
	# screen at every angle, so the only variable left is the approach direction.
	camera.top_level = true
	var radius := maxf(40.0, float(maxi(hi.x - lo.x, hi.z - lo.z)) * 1.6)
	var blank := 0
	var samples := 0
	var pitches := [-25.0, 0.0, 25.0]
	for pitch in pitches:
		for step in 24:
			var yaw := float(step) * 15.0
			var offset := Vector3(cos(deg_to_rad(yaw)), 0.0, sin(deg_to_rad(yaw))) * radius
			var height := sin(deg_to_rad(pitch)) * radius
			camera.global_position = centre + offset + Vector3(0, height, 0)
			camera.look_at(centre, Vector3.UP)
			await process_frame
			await RenderingServer.frame_post_draw
			var pixels := _ghost_pixels()
			samples += 1
			var present: bool = wand.has_preview() and ghost.visible
			if pixels <= 0:
				blank += 1
				if blank <= 8:
					print("probe: BLANK angle yaw=%.0f pitch=%.0f px=%d has_preview=%s visible=%s instances=%d"
						% [yaw, pitch, pixels, wand.has_preview(), ghost.visible,
						   ghost.multimesh.instance_count])
			if not present:
				_fail("the ghost was gone from the scene at yaw=%.0f pitch=%.0f (has_preview=%s visible=%s)"
					% [yaw, pitch, wand.has_preview(), ghost.visible])
				break
	print("probe: %d angles sampled, %d with no ghost pixels" % [samples, blank])
	if blank == samples:
		_fail("the ghost never rendered at any angle")
	elif blank > 0:
		_fail("%d of %d angles rendered no ghost at all" % [blank, samples])
	else:
		print("probe: the ghost rendered at every angle")
	if ok:
		print("PROBE PASS")
	quit(0 if ok else 1)
