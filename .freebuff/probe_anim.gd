extends Node3D

# Renders the player model WITH the Idle animation playing, at grazing angles
# and several animation times, against a magenta background. Enclosed magenta
# runs = see-through cracks (terrain showing between body parts).

const SHOT_DIR := "user://probe_anim"
const ANIM_TIMES: Array[float] = [0.0, 0.9, 2.07, 3.4]  # rest, mid-swing, swing peak, return

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)

	var model: Node3D = load("res://player.glb").instantiate()
	model.set_script(load("res://scripts/player_model.gd"))
	var anim_player := AnimationPlayer.new()
	anim_player.name = "AnimationPlayer"
	model.add_child(anim_player)
	model.transform = Transform3D(
		Vector3(-0.05625, 0, 0), Vector3(0, 0.05625, 0), Vector3(0, 0, -0.05625),
		Vector3(0, 0, 0.0844))
	add_child(model)

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 35, 0)
	light.light_energy = 1.6
	add_child(light)
	var ambient := DirectionalLight3D.new()
	ambient.rotation_degrees = Vector3(30, -20, 0)
	ambient.light_energy = 0.5
	add_child(ambient)

	var cam := Camera3D.new()
	cam.fov = 50.0
	add_child(cam)
	cam.make_current()

	await get_tree().process_frame
	await get_tree().process_frame

	# Grazing views: low elevations (looking up at the junctions), front-side
	# azimuths where the arm-torso and leg-torso seams sit edge-on, plus the
	# user's behind-left hip-height view.
	var azimuths: Array[float] = [45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]
	var elevations: Array[float] = [0.12, 0.30, 0.62]
	var shots := 0
	for t in ANIM_TIMES:
		anim_player.seek(t, true)
		await get_tree().process_frame
		await get_tree().process_frame
		for elev in elevations:
			for az in azimuths:
				var rad := deg_to_rad(az)
				var eye := Vector3(sin(rad) * 1.35, elev, cos(rad) * 1.35)
				cam.global_position = eye
				cam.look_at(Vector3(0, 0.45, 0), Vector3.UP)
				await get_tree().process_frame
				await get_tree().process_frame
				var img: Image = get_viewport().get_texture().get_image()
				img.save_png(SHOT_DIR + "/a%02d_e%d_az%d.png" % [int(t * 100.0), int(elev * 100.0), int(az)])
				shots += 1
	# Tight close-ups at the swing peak on the junctions the user reported.
	anim_player.seek(2.07, true)
	await get_tree().process_frame
	await get_tree().process_frame
	var close_ups := [
		[Vector3(-0.42, 0.35, -0.35), Vector3(-0.18, 0.42, -0.05)],  # left arm-torso, low
		[Vector3(0.42, 0.35, -0.35), Vector3(0.18, 0.42, -0.05)],   # right arm-torso, low
		[Vector3(-0.38, 0.62, -0.30), Vector3(-0.18, 0.60, -0.05)], # left shoulder
		[Vector3(0.38, 0.62, -0.30), Vector3(0.18, 0.60, -0.05)],   # right shoulder
		[Vector3(-0.30, 0.18, -0.30), Vector3(-0.10, 0.30, -0.05)], # left hip (leg|torso|arm)
		[Vector3(0.30, 0.18, -0.30), Vector3(0.10, 0.30, -0.05)],   # right hip
	]
	for i in range(close_ups.size()):
		var cu: Array = close_ups[i]
		cam.global_position = cu[0]
		cam.look_at(cu[1], Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(SHOT_DIR + "/cu%d.png" % i)
		shots += 1
	print("PROBE_ANIM saved ", shots, " shots to ", SHOT_DIR)
	get_tree().quit()