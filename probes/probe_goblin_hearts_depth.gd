extends SceneTree
## The goblin's heart bar is a thing in the world, not a HUD mark: it is DEPTH-TESTED.
##
## `scripts/goblin.gd` builds six `Sprite3D` hearts over the body. Drawn with
## `no_depth_test` they are six red dots floating over the terrain from any angle, which
## is what a bar over a body must not be. The claim is about pixels, so this runs
## windowed, counts the heart art's own red (#ff0000) in the frame and holds the three
## cases that matter:
##
##   front      open sky, from the front    -- the six hearts are drawn
##   occluded   the same view with an opaque box between the camera and the goblin
##              -- no heart pixel survives, the bar goes behind cover with the body
##   side       from above and to one side  -- still six hearts, billboarded, not a
##              sliver, because a row that only faces the camera head-on is not a bar
##
## The box is a node of the probe's own rather than stone placed in the world: an
## occluder in the depth buffer is all this measures, and a tracked probe must not leave
## edits in the player's world. Shots go to `user://goblin_shots/`; run it with
## `probes/run_probe_shot.sh probes/probe_goblin_hearts_depth.gd` (windowed — a
## screenshot from the dummy renderer is blank).
##
## The rock version of the same check was done in the world (three stone blocks in front
## of a goblin, 0 heart pixels through them, blocks cleared afterwards); this is the
## repeatable form.

const HEART_RED := 0.94  # the art is #ff0000; nothing else in these frames is
const SHOT_DIR := "user://goblin_shots"

var ok := true


func _initialize() -> void:
	_run()


func _check(condition: bool, message: String) -> void:
	if not condition:
		ok = false
	print("PROBE %s: %s" % ["ok" if condition else "FAIL", message])


## dummy.gd drives its node from its own tick position and velocity, so a probe moves a
## body by putting it where the next tick starts (both ends of its interpolation pair)
## and putting down the fall it had built up — otherwise it is a metre lower by the time
## the frame is read.
func _teleport(body: Node3D, pos: Vector3) -> void:
	body.set("_pos", pos)
	body.set("_prev_pos", pos)
	body.set("_motion", Vector3.ZERO)
	body.global_position = pos


## Pixels of the heart art's own red, sampled every second pixel: the lava in these
## frames is orange, the goblin's texture green and brown, the sky blue.
func _heart_pixels(image: Image) -> int:
	var n := 0
	for y in range(0, image.get_height(), 2):
		for x in range(0, image.get_width(), 2):
			var c := image.get_pixel(x, y)
			if c.r > HEART_RED and c.g < 0.06 and c.b < 0.06:
				n += 1
	return n


func _shoot(cam: Camera3D, at: Vector3, look_at: Vector3, name: String) -> int:
	cam.global_position = at
	cam.look_at(look_at)
	for i in 2:
		await process_frame
	await RenderingServer.frame_post_draw
	var image := root.get_texture().get_image()
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	image.save_png("%s/%s.png" % [SHOT_DIR, name])
	return _heart_pixels(image)


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	for i in 150:
		await process_frame
	var player := main.get_node_or_null("Player") as Node3D
	if player == null:
		_check(false, "the world has a player")
		quit(1)
		return
	# The title screen is up over the live world and holds the input gate; `_release` is
	# the call CONTINUE makes. Both overlay layers are hidden so only the world is read.
	for child in root.get_children():
		if child is CanvasLayer and String(child.name) == "MenuLayer":
			child.call("_release")
			child.visible = false
	var hud := main.get_node_or_null("HUD") as CanvasLayer
	if hud != null:
		hud.visible = false

	var tool: Node = root.get_node("ToolLoader").call("open_tool", "goblin")
	tool.call("spawn")
	await process_frame
	var bodies: Array = []
	for node in main.get_tree().get_nodes_in_group("goblin"):
		if main.is_ancestor_of(node):
			bodies.append(node)
	if bodies.is_empty():
		_check(false, "the goblin tool spawned a goblin")
		quit(1)
		return
	var goblin: Node3D = bodies[0]

	# Open sky, so nothing but the probe's own box is in the way. The column over the
	# player can be a water column, so climb until the cell and the two above it are air.
	var cm := main.get_node("ChunkManager")
	var cx := floori(player.global_position.x)
	var cz := floori(player.global_position.z)
	var cy := floori(player.global_position.y) + 8
	while cy < floori(player.global_position.y) + 40 and \
			(int(cm.call("get_block", cx, cy, cz)) != 0 or \
			int(cm.call("get_block", cx, cy + 1, cz)) != 0 or \
			int(cm.call("get_block", cx, cy + 2, cz)) != 0):
		cy += 1
	var spot := Vector3(float(cx) + 0.5, float(cy), float(cz) + 0.5)
	_teleport(goblin, spot)
	var cam := Camera3D.new()
	main.add_child(cam)
	cam.fov = 60.0
	cam.current = true
	print("PROBE goblin at %s" % goblin.global_position)

	var front := await _shoot(cam, spot + Vector3(0.0, 1.2, 3.0), spot + Vector3(0.0, 1.0, 0.0), "front")
	print("PROBE front: %d heart pixels" % front)

	# An opaque box of the probe's own, three blocks across and two deep, between the
	# camera and the goblin: an opaque surface in front of the bar is all a depth test
	# has to answer.
	var cover := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(3.0, 3.0, 1.0)
	cover.mesh = box
	var cover_material := StandardMaterial3D.new()
	cover_material.albedo_color = Color(0.35, 0.35, 0.38)
	cover.material_override = cover_material
	main.add_child(cover)
	# Halfway between the camera (+3 in z) and the goblin, not behind it: an occluder on
	# the far side of the body is an occluder of nothing.
	cover.global_position = spot + Vector3(0.0, 1.4, 2.0)
	_teleport(goblin, spot)
	var occluded := await _shoot(cam, spot + Vector3(0.0, 1.2, 3.0), spot + Vector3(0.0, 1.0, 0.0), "occluded")
	print("PROBE occluded: %d heart pixels" % occluded)
	cover.queue_free()

	# From above and to the side, with the cover gone: a billboard must still read as six
	# hearts from an angle.
	for i in 2:
		await process_frame
	_teleport(goblin, spot)
	var side := await _shoot(cam, goblin.global_position + Vector3(1.8, 3.2, 1.8),
		goblin.global_position + Vector3(0.0, 1.0, 0.0), "side")
	print("PROBE from above and to the side: %d heart pixels" % side)

	_check(front > 20, "the six hearts are drawn in the open (%d heart pixels)" % front)
	_check(occluded == 0, "no heart pixel comes through cover (%d)" % occluded)
	_check(side > 20, "the hearts read from above and to the side (%d heart pixels)" % side)

	if ok:
		print("PROBE PASS: the heart bar is depth-tested like the world it stands in")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)
