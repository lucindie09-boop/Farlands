extends Node3D

const SHOT_DIR := "user://probe_zoom"

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var model: Node3D = load("res://player.glb").instantiate()
	model.set_script(load("res://player_model.gd"))
	model.transform = Transform3D(
		Vector3(-0.05625, 0, 0), Vector3(0, 0.05625, 0), Vector3(0, 0, -0.05625),
		Vector3(0, 0, 0.0844))
	add_child(model)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 35, 0)
	light.light_energy = 1.6
	add_child(light)
	var cam := Camera3D.new()
	cam.fov = 28.0
	add_child(cam)
	cam.make_current()
	# Each target: (eye, look_at) — camera close to a specific cube edge, grazing.
	var targets := [
		# right leg front-right vertical edge, nearly edge-on
		[Vector3(0.30, 0.22, 0.18), Vector3(0.0, 0.22, 0.0)],
		# left leg front-left vertical edge
		[Vector3(-0.30, 0.22, 0.18), Vector3(0.0, 0.22, 0.0)],
		# right arm outer (right) vertical edge, nearly edge-on from the side
		[Vector3(0.52, 0.62, 0.10), Vector3(0.32, 0.62, 0.0)],
		# head top-front edge grazing
		[Vector3(0.14, 0.92, 0.10), Vector3(0.0, 0.78, 0.0)],
		# torso bottom edge (waist) from the front
		[Vector3(0.05, 0.42, 0.30), Vector3(0.0, 0.45, 0.0)],
		# right shoe top edge, grazing
		[Vector3(0.28, 0.10, 0.16), Vector3(0.08, 0.06, 0.0)],
		# arm pit region (shoulder junction)
		[Vector3(0.42, 0.75, -0.30), Vector3(0.30, 0.72, 0.0)],
		# head side (right) vertical edge
		[Vector3(0.30, 0.80, 0.02), Vector3(0.0, 0.80, 0.0)],
	]
	var i := 0
	for t in targets:
		cam.global_position = t[0]
		cam.look_at(t[1], Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
		await get_tree().process_frame
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(SHOT_DIR + "/zoom_%d.png" % i)
		print("PROBE saved zoom_", i)
		i += 1
	get_tree().quit()