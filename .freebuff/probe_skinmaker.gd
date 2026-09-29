extends Node

const SHOT_DIR := "user://probe_skinmaker"

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.transparent_bg = false
	# Match skin_preview.gd exactly (MSAA 4x), but opaque magenta bg for detection.
	vp.msaa_3d = Viewport.MSAA_4X
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 0.35
	var world := World3D.new()
	world.environment = env
	vp.world_3d = world
	add_child(vp)
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.4
	sun.rotation_degrees = Vector3(-45, -35, 0)
	vp.add_child(sun)
	var fill := DirectionalLight3D.new()
	fill.light_energy = 0.5
	fill.rotation_degrees = Vector3(70, 140, 0)
	vp.add_child(fill)
	var model: Node3D = load("res://player.glb").instantiate()
	model.scale = Vector3(0.9, 0.9, 0.9)
	model.set_script(load("res://player_model.gd"))
	vp.add_child(model)
	var cam := Camera3D.new()
	cam.fov = 70.0
	vp.add_child(cam)
	var target := Vector3(0, 16, 0)
	var dist := 34.0
	var i := 0
	for pitch: float in [14.0, -5.0, 30.0, -20.0, 45.0]:
		for yaw: float in [-35.0, 10.0, 55.0, 100.0, 145.0, 190.0]:
			var pr := deg_to_rad(pitch)
			var yr := deg_to_rad(yaw)
			var dir := Vector3(
				dist * cos(pr) * sin(yr),
				dist * sin(pr),
				dist * cos(pr) * cos(yr))
			cam.global_position = target + dir
			cam.look_at(target, Vector3.UP)
			await get_tree().process_frame
			await get_tree().process_frame
			var img: Image = vp.get_texture().get_image()
			img.save_png(SHOT_DIR + "/sm_%d.png" % i)
			print("PROBE saved sm_", i)
			i += 1
	get_tree().quit()