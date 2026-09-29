extends Node

const SHOT_DIR := "user://probe_game"

func _ready() -> void:
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)
	var main: Node = load("res://Main.tscn").instantiate()
	get_tree().root.add_child.call_deferred(main)
	await get_tree().process_frame
	await get_tree().process_frame
	# Magenta sky so cracks pop against a uniform background.
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(1.0, 0.0, 1.0)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(1, 1, 1)
	env.ambient_light_energy = 1.0
	var we := get_tree().root.get_node_or_null("Main/WorldEnvironment")
	if we != null:
		(we as WorldEnvironment).environment = env
	# Wait for chunks + spawn.
	var player: Node = get_tree().root.get_node_or_null("Main/Player")
	for i in range(600):
		await get_tree().process_frame
		if player != null:
			var p := (player as Node3D).get_global_position()
			if p.y > 200.0:
				break
	print("PROBE spawn y = ", (player as Node3D).get_global_position().y)
	# Hide terrain + HUD so only the model renders against magenta.
	var cm := get_tree().root.get_node_or_null("Main/ChunkManager")
	if cm != null:
		(cm as Node3D).visible = false
	var hud := get_tree().root.get_node_or_null("Main/HUD")
	if hud != null:
		(hud as CanvasLayer).visible = false
	var hotbar := get_tree().root.get_node_or_null("Main/Hotbar#HotbarTexture")
	if hotbar != null:
		(hotbar as CanvasItem).visible = false
	var outline := get_tree().root.get_node_or_null("Main/BlockOutline")
	if outline != null:
		(outline as Node3D).visible = false
	# Fly high (irrelevant now, terrain is hidden) and switch to third person.
	player.call("set_fly_mode", true)
	player.call("set_third_person_view", 1)
	await get_tree().process_frame
	await get_tree().process_frame
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	var sens := 0.003
	var shots := 0
	for deg: float in [0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0]:
		var rad := deg * PI / 180.0
		var ev := InputEventMouseMotion.new()
		ev.relative = Vector2(-rad / sens, 0)
		Input.parse_input_event(ev)
		await get_tree().process_frame
		await get_tree().process_frame
		var img: Image = get_viewport().get_texture().get_image()
		img.save_png(SHOT_DIR + "/g_%d.png" % shots)
		print("PROBE saved g_", shots)
		shots += 1
	for pitch_deg: float in [20.0, -20.0, 35.0, -35.0]:
		var ev2 := InputEventMouseMotion.new()
		ev2.relative = Vector2(0, -pitch_deg * PI / 180.0 / sens)
		Input.parse_input_event(ev2)
		await get_tree().process_frame
		await get_tree().process_frame
		var img2: Image = get_viewport().get_texture().get_image()
		img2.save_png(SHOT_DIR + "/g_%d.png" % shots)
		print("PROBE saved g_", shots)
		shots += 1
	get_tree().quit()