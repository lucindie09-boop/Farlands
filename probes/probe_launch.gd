extends SceneTree
## How long a launch takes now that the game boots on `Boot.tscn`.
##
## Engine uptime (`Time.get_ticks_msec`) starts with the process, so every mark is
## a wall-clock offset from the double-click. The marks split the boot into what
## the player waits for: the engine + autoload scripts, the boot scene itself, the
## frame the title screen appears on, and the frame the world (`Main.tscn`, loaded
## on a thread) is swapped in behind it.
##
##   probes/probe_launch.gd
##   SECS=8 probes/probe_launch.gd          # watch the frames after the swap
##   EMPTY=1 probes/probe_launch.gd         # the floor: a bare node, no boot work

const DEFAULT_SECS := 6.0

var boot: Node

func _initialize() -> void:
	print("PROBE launch engine_ms=%d" % Time.get_ticks_msec())
	# KILL_AUTOLOAD=Name,... frees an autoload before the first frame, i.e. before
	# its `_ready` can run: the arm for "which autoload owns the first frame".
	for autoload_name in OS.get_environment("KILL_AUTOLOAD").split(",", false):
		var autoload := root.get_node_or_null(autoload_name)
		if autoload != null:
			autoload.free()
			print("PROBE launch freed " + autoload_name)
	_run()

func _env_float(name: String, fallback: float) -> float:
	var raw := OS.get_environment(name)
	return float(raw) if raw.is_valid_float() else fallback

func _run() -> void:
	var secs := _env_float("SECS", DEFAULT_SECS)
	var t_load := Time.get_ticks_msec()
	var packed := load("res://Boot.tscn") as PackedScene
	var t_loaded := Time.get_ticks_msec()
	# EMPTY=1 adds a bare node instead of the boot scene: the floor for "one frame
	# on this machine, in this project, with nothing in it".
	if OS.get_environment("EMPTY") == "1":
		boot = Node.new()
		boot.name = "Boot"
	elif packed != null:
		boot = packed.instantiate()
	if boot == null:
		print("PROBE launch FAIL: Boot.tscn missing")
		quit(1)
		return
	var t_instantiated := Time.get_ticks_msec()
	root.add_child(boot)
	current_scene = boot
	var t_added := Time.get_ticks_msec()
	print("PROBE launch stage boot_load_ms=%d instantiate_ms=%d add_ms=%d" % [
		t_loaded - t_load, t_instantiated - t_loaded, t_added - t_instantiated])
	var menu := root.get_node_or_null("MenuLayer")

	var start := Time.get_ticks_usec()
	var title_ms := -1.0
	var world_ms := -1.0
	var next_second := 1
	var slow: Array[String] = []
	var frames := 0
	var window: Array[float] = []
	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var frame_ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		var t := float(Time.get_ticks_usec() - start) / 1000.0
		frames += 1
		window.append(frame_ms)
		if frame_ms >= 30.0 and slow.size() < 12:
			slow.append("%.0f@%.0f" % [frame_ms, t])
		if title_ms < 0.0 and menu != null:
			var title := menu.get("_title") as CanvasItem
			if title != null and title.visible:
				title_ms = t
				print("PROBE launch title_ms=%.1f uptime_ms=%d frames=%d" % [
					t, Time.get_ticks_msec(), frames])
		if world_ms < 0.0 and current_scene != null and current_scene != boot:
			world_ms = t
			print("PROBE launch world_ms=%.1f uptime_ms=%d scene=%s" % [
				t, Time.get_ticks_msec(), current_scene.name])
		if t >= float(next_second) * 1000.0:
			var total := 0.0
			var worst := 0.0
			for value in window:
				total += value
				worst = maxf(worst, value)
			print("PROBE launch t=%d frames=%d mean=%.2fms worst=%.2fms uptime_ms=%d proc=%.1fms phys=%.1fms" % [
				next_second, window.size(), total / float(max(1, window.size())), worst,
				Time.get_ticks_msec(),
				float(Performance.get_monitor(Performance.TIME_PROCESS)) * 1000.0,
				float(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)) * 1000.0])
			window.clear()
			next_second += 1
		if t >= secs * 1000.0:
			break

	# The cloud layers are generated on a thread now: say whether they landed.
	if current_scene != null and current_scene != boot:
		var clouds := current_scene.get_node_or_null("CloudLayer") as MeshInstance3D
		if clouds != null:
			var attached := 1 if clouds.mesh != null else 0
			for child in clouds.get_children():
				var layer := child as MeshInstance3D
				if layer != null and layer.mesh != null:
					attached += 1
			print("PROBE launch clouds attached=%d of=%d" % [attached, 1 + clouds.get_child_count()])
	print("PROBE launch summary secs=%.1f frames=%d title_ms=%.1f world_ms=%.1f uptime_ms=%d slow_frames=%s" % [
		secs, frames, title_ms, world_ms, Time.get_ticks_msec(),
		"none" if slow.is_empty() else ",".join(slow)])
	quit(0)
