extends SceneTree
# One-off: why does the compensation change nothing? Prints the distances the
# world is streamed at, the camera's own numbers, the shader's state (does the
# frame change when the bend goes on at all?), and the primitives drawn frame by
# frame across a toggle of the engine's compensation alone.

var main: Node3D
var cm: Node
var player: Node3D
var camera: Camera3D
var overlay: Control


func _initialize() -> void:
	_run()


func _run() -> void:
	DisplayServer.window_set_size(Vector2i(1280, 720))
	var scene: PackedScene = load("res://Main.tscn")
	main = scene.instantiate()
	root.add_child(main)
	await process_frame
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	overlay = main.get_node_or_null("HUD/ShaderOverlay")
	if cm == null or player == null or overlay == null:
		print("DIAG FAIL: nodes missing")
		quit(1)
		return

	for key in ["render_distance", "lod_distance", "lod_detail_level", "far_lod_distance",
			"far_lod_detail_level", "player_position"]:
		print("DIAG %s = %s" % [key, str(cm.get(key))])
	for child in overlay.get_parent().get_children():
		if child is CanvasItem and child != overlay and child.name != "SettingsMenu":
			(child as CanvasItem).visible = false

	var landed := 0
	while landed < 2400 and not player.is_on_floor():
		await process_frame
		landed += 1
	print("DIAG player landed at %s after %d frames" % [player.global_position, landed])

	camera = Camera3D.new()
	camera.fov = 70.0
	camera.far = 4000.0
	main.add_child(camera)
	camera.make_current()
	await _quiet()
	camera.global_position = player.global_position + Vector3(0.0, 320.0, 0.0)
	camera.rotation_degrees = Vector3(-78.0, 0.0, 0.0)
	await _quiet()
	print("DIAG camera at %s rotation %s" % [camera.global_position, camera.rotation_degrees])

	# Does the shader bend at all here? Two frames, one state apart.
	overlay.call("set_enabled", "bend", false)
	await _frames(6)
	var off := _frame()
	var off_prims := _prims()
	overlay.call("set_enabled", "bend", true)
	overlay.call("set_value", "bend", "world_bend", 1.0)
	overlay.call("set_value", "bend", "world_bend_radius", 128.0)
	overlay.call("set_value", "bend", "world_bend_rise", 1.0)
	await _frames(6)
	var on := _frame()
	var on_prims := _prims()
	print("DIAG shader bend off %d prims, on %d prims, frame diff %.4f"
		% [off_prims, on_prims, _diff(off, on)])

	# And the engine's own half, toggled under a bend that is definitely on.
	print("DIAG --- engine compensation off (the bug) ---")
	cm.call("set_world_bend", false, 1.0, 128.0, 1.0)
	for i in range(6):
		await process_frame
		print("DIAG prims %d calls %d" % [_prims(), _calls()])
	print("DIAG --- engine compensation on ---")
	cm.call("set_world_bend", true, 1.0, 128.0, 1.0)
	for i in range(6):
		await process_frame
		print("DIAG prims %d calls %d" % [_prims(), _calls()])

	overlay.call("set_enabled", "bend", false)
	quit()


func _quiet() -> void:
	var waited := 0
	var quiet := 0
	while waited < 3000 and quiet < 20:
		await process_frame
		waited += 1
		quiet = 0 if bool(cm.call("has_pending_mesh_work")) else quiet + 1
	print("DIAG quiet after %d frames, %d prims" % [waited, _prims()])
	var report: String = cm.call("get_performance_report")
	for line in report.split("\n"):
		if line.contains("instances") or line.contains("meshes") or line.contains("Loaded") \
				or line.contains("eligible") or line.contains("Far cached") or line.contains("members"):
			print("DIAG RENDER %s" % line.strip_edges())


func _frames(n: int) -> void:
	for i in range(n):
		await process_frame


func _prims() -> int:
	return int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))


func _calls() -> int:
	return int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))


func _frame() -> Image:
	var img: Image = root.get_texture().get_image()
	if img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _diff(a: Image, b: Image) -> float:
	var pa := a.get_data()
	var pb := b.get_data()
	if pa.size() != pb.size():
		return -1.0
	var total := 0.0
	var count := 0
	for i in range(0, pa.size() - 3, 16):
		total += absf(int(pa[i]) - int(pb[i])) + absf(int(pa[i + 1]) - int(pb[i + 1])) \
			+ absf(int(pa[i + 2]) - int(pb[i + 2]))
		count += 3
	return total / (255.0 * maxf(float(count), 1.0))
