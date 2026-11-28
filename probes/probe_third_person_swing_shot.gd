extends SceneTree
## The third-person punch, PHOTOGRAPHED: the body on its own, the stroke's clock set by hand at
## points along it, and the camera where third person puts it -- behind the body, at eye height.
##
## Why a window rather than `--headless`: the dummy renderer has no textures or framebuffer, so a
## capture there is blank, and this probe exists to be LOOKED at. The numbers are the other probe's
## job (probes/probe_third_person_swing.gd); a path can be measured and still be the wrong shape.
##
##   probes/run_probe_shot.sh probes/probe_third_person_swing_shot.gd
##
## Writes .freebuff/shots/swing_pNN.png for each stop and touches nothing else -- no world, no
## user:// (there is no ChunkManager here, so the body keeps its own glTF material and the held item
## is absent: this is the arm and the rig, which is what is being judged).

const STOPS := [0.0, 0.06, 0.12, 0.20, 0.35, 0.50, 0.70, 0.88, 1.0]


class Stub extends Node3D:
	var block_id := 0
	var velocity := Vector3.ZERO
	# Where the body is aiming, which is what the stroke's head-pitch term reads. Level unless
	# `FARLANDS_SWING_AIM` says otherwise, so a run of this probe is a level-look run by default.
	var aim := Vector3(0.0, 0.0, 1.0)

	func get_selected_hotbar_slot() -> int:
		return 0

	func get_hotbar_slot_block_id(_slot: int) -> int:
		return block_id

	func get_aim_direction() -> Vector3:
		return aim

	func get_velocity() -> Vector3:
		return velocity


func _initialize() -> void:
	_run()


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(520, 720))
	DirAccess.make_dir_recursive_absolute("res://.freebuff/shots")

	var stage := Node3D.new()
	root.add_child(stage)

	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.62, 0.74, 0.86)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.78, 0.82, 0.88)
	env.ambient_light_energy = 1.1
	var we := WorldEnvironment.new()
	we.environment = env
	stage.add_child(we)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-48.0, 38.0, 0.0)
	sun.light_energy = 1.1
	stage.add_child(sun)

	var glb: PackedScene = load("res://assets/models/player.glb")
	if glb == null:
		print("PROBE FAIL: player.glb missing")
		quit(1)
		return
	var host := Stub.new()
	host.name = "ShotHost"
	# The blow is aimed, so the frames can be taken at a pitched look as well as a level one:
	# `FARLANDS_SWING_AIM=down` (or `up`) pitches the aim 45 degrees and retitles the files.
	var aim_name := OS.get_environment("FARLANDS_SWING_AIM")
	if aim_name == "down":
		host.aim = Vector3(0.0, -1.0, 1.0).normalized()
	elif aim_name == "up":
		host.aim = Vector3(0.0, 1.0, 1.0).normalized()
	else:
		aim_name = "level"
	stage.add_child(host)
	var body: Node3D = glb.instantiate()
	body.set_script(load("res://scripts/player_model.gd"))
	body.set("skip_head_look", true)
	host.add_child(body)

	for i in 4:
		await process_frame
	# The camera where third person puts it: behind the body's own +Z face, at about eye height.
	# Placed AFTER the tree is live (a global transform read before the first frame is the identity,
	# which put the camera at the origin facing away from the body and photographed the sky), and
	# with the from-position form, which does not need the node to be in the tree already.
	var mid: Vector3 = body.to_global(Vector3(0.0, 20.0, 0.0))
	var camera := Camera3D.new()
	camera.fov = 46.0
	stage.add_child(camera)
	camera.look_at_from_position(body.to_global(Vector3(0.0, 22.0, -46.0)), mid, Vector3.UP)
	camera.current = true
	await process_frame
	# The held-item pose: a body in the world has it (the hand carries something), and it is the
	# base the stroke leaves from. Called directly because this body has no ChunkManager, which is
	# what normally decides a hand is full.
	body.call("_pose_held_arm", true)

	# The stroke's clock STOPS here: `_process` is what advances it, and a stop that keeps ticking
	# while two frames are rendered lands somewhere else than the number it was set to (the first
	# version of this probe photographed 0.15 while claiming 0.00). Set, write, render, read.
	body.set_process(false)

	var index := 0
	for p in STOPS:
		body.set("_hand_swing_strength", 1.0)
		body.set("_hand_swing", 1.0 - float(p))
		body.call("_apply_hand_swing", 0.0)
		for i in 2:
			await process_frame
		await RenderingServer.frame_post_draw
		var image: Image = root.get_texture().get_image()
		var path := "res://.freebuff/shots/swing_%s_p%02d.png" % [aim_name, index]
		var err := image.save_png(path)
		print("PROBE stop %.2f -> %s (%s)" % [float(p), path, error_string(err)])
		index += 1

	body.set("_hand_swing", 0.0)
	body.call("_apply_hand_swing", 0.0)
	print("PROBE swing shots written")
	quit(0)
