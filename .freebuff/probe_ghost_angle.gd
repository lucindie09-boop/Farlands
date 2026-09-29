extends SceneTree
## Rendered probe: does the wand's ghost stay visible from every camera pose?
##
## The complaint is that the overlay "becomes invisible at random weird angles".
## Two earlier theories (instance bounds, back faces in the blend) were tested and
## rejected, so this measures the thing itself: build the real ghost through the
## real wand, then put a detached camera at a grid of poses — outside far, outside
## near, on the shell, and INSIDE the volume — and count pixels that only the
## ghost can have painted (the same pose rendered with the ghost hidden is the
## baseline, so terrain and sky cannot be mistaken for it).
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_ghost_angle.gd

const FILE := "10179.schematic"

var main: Node
var cm: Node
var player: Node3D
var cam: Camera3D
var wand: Node
var ghost: MultiMeshInstance3D
var outline: MeshInstance3D
var worst_pose := ""
var worst := 1 << 30

func _initialize() -> void:
	_run()

func _count_bluish() -> int:
	var img := root.get_texture().get_image()
	var n := 0
	for y in range(0, img.get_height(), 3):
		for x in range(0, img.get_width(), 3):
			var c := img.get_pixel(x, y)
			if c.b > c.r + 0.05 and c.g > c.r + 0.02:
				n += 1
	return n

func _sample(label: String, pos: Vector3, look: Vector3) -> void:
	cam.global_position = pos
	cam.look_at(look, Vector3.UP)
	await process_frame
	await process_frame
	await RenderingServer.frame_post_draw
	var on := _count_bluish()
	ghost.visible = false
	outline.visible = false
	await process_frame
	await RenderingServer.frame_post_draw
	var off := _count_bluish()
	ghost.visible = true
	outline.visible = true
	var ghost_px := on - off
	if ghost_px < worst:
		worst = ghost_px
		worst_pose = label
	print("PROBE pose %-26s cam=(%d,%d,%d) ghost_px=%d (frame=%d, hidden=%d)"
		% [label, int(pos.x), int(pos.y), int(pos.z), ghost_px, on, off])

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player") as Node3D
	wand = main.get_node_or_null("HUD/Wand")
	if cm == null or player == null or wand == null:
		print("PROBE FAIL: scene nodes missing (cm=%s player=%s wand=%s)" % [cm, player, wand])
		quit(1)
		return
	cam = player.get_node_or_null("Camera3D") as Camera3D
	ghost = main.find_child("Cells", true, false) as MultiMeshInstance3D
	outline = main.find_child("Volume", true, false) as MeshInstance3D
	print("probe: scene root=%s cam=%s ghost=%s outline=%s wand_ghost=%s"
		% [main.name, cam != null, ghost != null, outline != null, wand.get("_ghost") != null])
	if ghost == null:
		# The ghost hangs off the 3D scene root; find it there whatever it is named.
		for child in main.get_children():
			if child is MultiMeshInstance3D:
				ghost = child
			elif child is Node3D:
				var found := child.find_child("Cells", true, false) as MultiMeshInstance3D
				if found != null:
					ghost = found
					outline = child.find_child("Volume", true, false) as MeshInstance3D
	print("probe: cam=%s ghost=%s outline=%s" % [cam != null, ghost != null, outline != null])
	if cam == null or ghost == null or outline == null:
		print("PROBE FAIL: camera/ghost/outline missing")
		quit(1)
		return

	var waited := 0
	while waited < 1200:
		await process_frame
		waited += 1
		if waited > 30 and player.is_on_floor():
			break

	var fil: GDScript = preload("res://schematic_files.gd")
	var resolved: String = fil.resolve(FILE)
	var bytes := FileAccess.get_file_as_bytes(resolved)
	print("probe: %s = %d bytes" % [FILE, bytes.size()])
	# The wand owns the choice; drive it through its own public path.
	wand.choose_file(FILE)
	var t := Time.get_ticks_usec()
	while not wand._build_ready() and Time.get_ticks_usec() - t < 30_000_000:
		await process_frame
	if not wand._build_ready():
		print("PROBE FAIL: the build never decoded")
		quit(1)
		return

	var base := player.global_position
	var anchor := Vector3i(int(floor(base.x)) - 40, int(floor(base.y)) + 4, int(floor(base.z)) - 40)
	var info: Dictionary = wand.anchor_preview_at(anchor)
	if not info.get("ok", false):
		print("PROBE FAIL: anchor_preview_at -> %s" % str(info))
		quit(1)
		return
	var lo: Vector3i = info.get("min", anchor)
	var hi: Vector3i = info.get("max", anchor)
	var shown: int = ghost.multimesh.instance_count
	print("probe: ghost volume %s..%s, %d instances shown" % [str(lo), str(hi), shown])
	if shown <= 0:
		print("PROBE FAIL: the preview drew no cells at all")
		quit(1)
		return

	# The camera goes free: detached from the player so a pose can be set exactly
	# (the controller owns the player's own transform and would fight it).
	player.set_physics_process(false)
	player.set_process(false)
	cam.reparent(main, true)
	var lo_f := Vector3(float(lo.x), float(lo.y), float(lo.z))
	var hi_f := Vector3(float(hi.x) + 1.0, float(hi.y) + 1.0, float(hi.z) + 1.0)
	var centre := (lo_f + hi_f) * 0.5
	var diag := (hi_f - lo_f).length()
	print("probe: centre=%s diag=%.1f" % [str(centre.round()), diag])

	# Poses: [label, eye offset from the centre as a fraction of the diagonal,
	# look offset]. Fractions keep this file's size independent of the fixture.
	var fractions := [
		["outside far", 1.20], ["outside 0.7", 0.70], ["outside 0.35", 0.35],
		["on the shell", 0.16], ["just outside", 0.09], ["inside near wall", 0.04],
		["inside 1/4", 0.001],
	]
	var directions := [
		Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, -1),
		Vector3(0, 1, 0.35), Vector3(0, 1, -0.35), Vector3(0, 1, 1),
	]
	for entry in fractions:
		var label: String = entry[0]
		var frac: float = entry[1]
		for dir in directions:
			var unit: Vector3 = (dir as Vector3).normalized()
			var eye: Vector3 = centre + unit * (diag * frac)
			# Look at a point past the centre, so a camera sitting ON the centre
			# still has a valid aim.
			var look: Vector3 = centre - unit * 12.0
			await _sample("%s %s" % [label, str(unit.snappedf(0.01))], eye, look)

	# Inside the volume, sweeping yaw only: this is the pose a player is in while
	# standing in a large build, which is the reported case.
	var inside := centre + Vector3(0, 0.0, 0)
	for yaw_deg in [0, 45, 90, 135, 180, 225, 270, 315]:
		var yaw := deg_to_rad(float(yaw_deg))
		var look: Vector3 = inside + Vector3(sin(yaw), 0.0, cos(yaw)) * 40.0
		await _sample("inside yaw %d" % yaw_deg, inside, look)

	print("PROBE worst pose: %s (%d px)" % [worst_pose, worst])
	quit(0)
