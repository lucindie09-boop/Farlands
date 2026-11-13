extends SceneTree
## Engine-only control: an empty project (no assets, no autoloads, no scripts).
## If the 4.5 ms/frame plateau is still here, it is the engine/host, not the
## project's files. Run with --path probes/empty_project --script res://warm.gd

func _initialize() -> void:
	print("PROBE warm engine_ms=%d" % Time.get_ticks_msec())
	_run()

func _run() -> void:
	var world := Node3D.new()
	root.add_child(world)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 6.0)
	world.add_child(cam)
	var mesh := BoxMesh.new()
	var mat := StandardMaterial3D.new()
	for i in range(64):
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.material_override = mat
		mi.position = Vector3(float(i % 8) * 1.5 - 6.0, 0.0, float(i / 8) * 1.5 - 6.0)
		world.add_child(mi)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	world.add_child(sun)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	var start := Time.get_ticks_usec()
	var window: Array[float] = []
	var next_second := 1
	while true:
		var s := Time.get_ticks_usec()
		await process_frame
		window.append(float(Time.get_ticks_usec() - s) / 1000.0)
		var t := float(Time.get_ticks_usec() - start) / 1_000_000.0
		if t >= float(next_second):
			var total := 0.0
			var worst := 0.0
			for value in window:
				total += float(value)
				worst = maxf(worst, float(value))
			print("PROBE warm t=%d frames=%d mean=%.2fms worst=%.2fms cap=%d res=%d mem=%.0fMB" % [
				next_second, window.size(), total / float(max(1, window.size())), worst,
				Engine.max_fps,
				int(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT)),
				float(Performance.get_monitor(Performance.MEMORY_STATIC)) / 1048576.0])
			window.clear()
			next_second += 1
		if t >= 9.0:
			break
	quit(0)
