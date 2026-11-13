extends SceneTree
## Is the post-boot frame-time plateau Farlands' or the engine's?
##
## Boots no Main.tscn: a camera, a directional light and 64 boxes. If this shows
## the same multi-second 4.5 ms plateau the fill probe sees, the plateau is the
## host/engine's warm-up, not streaming.
##
##   SECS=8 probes/run_probe_shot.sh probes/probe_engine_warmup.gd 60

func _initialize() -> void:
	_run()

func _run() -> void:
	var world := Node3D.new()
	root.add_child(world)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 2.0, 6.0)
	world.add_child(cam)
	var mesh := BoxMesh.new()
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.7, 0.6, 0.4)
	for i in range(64):
		var mi := MeshInstance3D.new()
		mi.mesh = mesh
		mi.material_override = mat
		mi.position = Vector3(float(i % 8) * 1.5 - 6.0, 0.0, float(i / 8) * 1.5 - 6.0)
		world.add_child(mi)
	var sun := DirectionalLight3D.new()
	sun.rotation_degrees = Vector3(-50.0, 30.0, 0.0)
	world.add_child(sun)
	# NO_ENV=1 drops the project's default environment (sky, fog, glow): the arm
	# for "is the project's own environment what warms up?".
	if OS.get_environment("NO_ENV") == "1":
		world.environment = Environment.new()
		print("PROBE warm no_env=1")
	# KILL_AUTOLOADS=1 frees every project autoload before the measurement;
	# KEEP_AUTOLOAD=name,... keeps just those, one run per autoload, to bisect.
	var keep := OS.get_environment("KEEP_AUTOLOAD").split(",", false)
	if OS.get_environment("KILL_AUTOLOADS") == "1" or not keep.is_empty():
		for autoload_name in ["UIScale", "SkinManager", "BlockManager", "BlockIconRenderer",
				"PoseCloneDebug", "LiquidAnimator", "LiquidTextureLab"]:
			if keep.has(autoload_name):
				continue
			var autoload := root.get_node_or_null(autoload_name)
			if autoload != null:
				autoload.free()
		print("PROBE warm kept=" + ",".join(keep))
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0

	var start := Time.get_ticks_usec()
	var window: Array[float] = []
	var next_second := 1
	var secs := 8.0
	var raw := OS.get_environment("SECS")
	if raw.is_valid_float():
		secs = float(raw)
	while true:
		var s := Time.get_ticks_usec()
		await process_frame
		var ms := float(Time.get_ticks_usec() - s) / 1000.0
		window.append(ms)
		var t := float(Time.get_ticks_usec() - start) / 1_000_000.0
		if t >= float(next_second):
			var total := 0.0
			var worst := 0.0
			for value in window:
				total += float(value)
				worst = maxf(worst, float(value))
			print("PROBE warm t=%d frames=%d mean=%.2fms worst=%.2fms cap=%d res=%d nodes=%d mem=%.0fMB" % [
				next_second, window.size(), total / float(max(1, window.size())), worst, Engine.max_fps,
				int(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT)),
				int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
				float(Performance.get_monitor(Performance.MEMORY_STATIC)) / 1048576.0])
			window.clear()
			next_second += 1
		if t >= secs:
			break
	quit(0)
