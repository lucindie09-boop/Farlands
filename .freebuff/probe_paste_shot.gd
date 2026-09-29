extends SceneTree
## Windowed probe: does a pasted build actually RENDER?
##
## The headless probe (probe_paste.gd) proves the blocks land in the chunk data.
## This one proves a player can see them: it drops a real .schematic on the
## ground along the direction the player is already looking, lets the mesh queue
## drain, and screenshots what the camera sees.
##
## The build is placed along the player's EXISTING aim (aim direction, not a
## camera cheat) because the look is driven by internal pitch/yaw that GDScript
## cannot set directly.
##
## Runs WINDOWED (the headless driver has no viewport texture), so run it through
## .freebuff/run_probe_shot.sh, which snapshots and restores user:// around it:
##
##   .freebuff/run_probe_shot.sh .freebuff/probe_paste_shot.gd
##
## Then read .freebuff/paste_shots/*.png.

const FILE := "res://church.schematic"
const SHOT_DIR := "user://paste_shots"

var main: Node3D
var cm: Node
var player: Node3D
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _surface_y(x: int, z: int, from_y: int) -> int:
	var y := int(from_y) + 80
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return y
		y -= 1
	return -1

func _shoot(name: String) -> void:
	var img: Image = root.get_texture().get_image()
	if img == null:
		_fail("no viewport image (running headless?)")
		return
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var path := "%s/%s.png" % [SHOT_DIR, name]
	img.save_png(path)
	print("probe: shot %s" % path)

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
	if cm == null or player == null:
		_fail("ChunkManager/Player missing")
		quit(1)
		return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	print("probe: player at %s after %d frames" % [player.global_position, waited])

	var bytes := FileAccess.get_file_as_bytes(FILE)
	var info: Dictionary = cm.inspect_schematic(bytes)
	if not info.get("ok", false):
		_fail("inspect refused: %s" % info.get("error", "?"))
		quit(1)
		return
	var w := int(info["file_width"])
	var h := int(info["file_height"])
	var l := int(info["file_length"])
	print("probe: %s is %dx%dx%d, %d cells would land (%d stand-ins)"
		% [FILE, w, h, l, int(info.get("planned", 0)), int(info.get("substituted", 0))])

	# Stand off along the direction the player is already facing and put the
	# build's south-west corner on the ground there.
	var flat := Vector3(player.get_aim_direction().x, 0.0, player.get_aim_direction().z)
	if flat.length() < 0.01:
		flat = Vector3(0, 0, -1)
	flat = flat.normalized()
	var target := player.global_position + flat * 34.0
	var origin := Vector3i(int(target.x) - w / 2, 0, int(target.z) - l / 2)
	var ground := _surface_y(origin.x + w / 2, origin.z + l / 2, int(target.y))
	if ground < 0:
		_fail("no ground under the target spot")
		quit(1)
		return
	origin.y = ground + 1
	print("probe: pasting at %s (ground y=%d)" % [origin, ground])

	var result: Dictionary = cm.paste_schematic(bytes, origin.x, origin.y, origin.z, {})
	if not result.get("ok", false):
		_fail("paste refused: %s" % result.get("error", "?"))
		quit(1)
		return
	print("probe: pasted %d of %d cells in %d chunks (box %s..%s)"
		% [int(result.get("cells", 0)), int(result.get("planned", 0)),
		   int(result.get("chunks", 0)), result.get("min", Vector3i.ZERO),
		   result.get("max", Vector3i.ZERO)])
	if int(result.get("cells", 0)) <= 0:
		_fail("nothing was written")
	if int(result.get("chunks", 0)) <= 0:
		_fail("no chunk was queued for a remesh")

	# Let the meshes build and upload, then look at it from where we stand.
	for i in range(180):
		await process_frame
	_shoot("pasted_from_34_blocks")

	# And from a few blocks closer, the way someone walks up to a building.
	var inward := -flat * 18.0
	player.global_position = player.global_position + inward
	for i in range(90):
		await process_frame
	_shoot("pasted_from_16_blocks")

	print("PROBE %s" % ("PASS" if ok else "FAIL"))
	quit(0 if ok else 1)
