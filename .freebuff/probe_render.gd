extends Node3D

const SHOT_DIR := "user://probe_shots2"

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	# Magenta background so the model silhouette and any cracks pop.
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	var we := WorldEnvironment.new()
	we.environment = env
	add_child(we)
	var model: Node3D = load("res://player.glb").instantiate()
	model.set_script(load("res://scripts/player_model.gd"))
	# Match Main.tscn's instance transform: glb is in px (16px tall), 0.05625 m/px.
	model.transform = Transform3D(
		Vector3(-0.05625, 0, 0), Vector3(0, 0.05625, 0), Vector3(0, 0, -0.05625),
		Vector3(0, 0, 0.0844))
	add_child(model)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 35, 0)
	light.light_energy = 1.4
	add_child(light)
	var ambient := DirectionalLight3D.new()
	ambient.rotation_degrees = Vector3(30, -20, 0)
	ambient.light_energy = 0.5
	add_child(ambient)
	var cam := Camera3D.new()
	cam.fov = 50.0
	add_child(cam)
	cam.make_current()
	var i := 0
	# Orbit at waist height (0.45) â€” near-edge-on angles included.
	for deg in range(0, 360, 45):
		var rad := deg * PI / 180.0
		var eye := Vector3(sin(rad) * 1.25, 0.45, cos(rad) * 1.25)
		cam.global_position = eye
		cam.look_at(Vector3(0, 0.45, 0), Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(SHOT_DIR + "/shot_%d.png" % i)
		print("PROBE saved shot_", i)
		i += 1
	# Low grazing across the legs.
	for eye in [Vector3(1.15, 0.08, -0.55), Vector3(-1.15, 0.08, 0.55),
			Vector3(0.55, 0.08, -1.15), Vector3(-0.55, 0.08, 1.15)]:
		cam.global_position = eye
		cam.look_at(Vector3(0, 0.35, 0), Vector3.UP)
		await get_tree().process_frame
		await get_tree().process_frame
		var img2: Image = get_viewport().get_texture().get_image()
		img2.save_png(SHOT_DIR + "/shot_%d.png" % i)
		print("PROBE saved shot_", i)
		i += 1
	get_tree().quit()