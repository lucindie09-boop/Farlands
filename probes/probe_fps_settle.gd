extends SceneTree
## When does the frame rate settle after the world load?
##
## probe_start_fill stops at "loaded + 1 s" — the point where the chunk map is
## quiet — but the frame rate keeps paying the tail for several seconds after
## that, and the fill probe cannot see it. This one boots the same scene, then
## samples every frame for SECS and prints one line a second: the frame rate, the
## loading/idle budget edge, the pending-mesh reason mask and the queue counts,
## so the tail can be split into the mesh drain, the far-visibility walk and the
## latch's quiet window instead of read as one number.
##
##   RD=32 SECS=20 probes/run_probe_shot.sh probes/probe_fps_settle.gd 120

const DEFAULT_RENDER_DISTANCE := 32
const DEFAULT_SECS := 20.0
# "Settled" for this probe: a whole second whose mean frame is under 1.15 ms
# (about 870 fps) and whose worst frame is under 3 ms, i.e. no tail burst left.
const SETTLE_MEAN_MS := 1.15
const SETTLE_WORST_MS := 3.0

var main: Node
var cm: Node
var ok := true
var last_report := ""
var quiet_match: PackedStringArray = PackedStringArray()

func _quiet_matches(path: String) -> bool:
	if quiet_match.is_empty():
		return true
	for pattern in quiet_match:
		if path.contains(pattern):
			return true
	return false

# QUIET=1 silences every scripted node except ChunkManager (a native node) and
# the player: the arm for "is the tail's outside-updater time other scripts?".
func _quiet_scene_process(node: Node) -> int:
	var disabled := 0
	for child in node.get_children():
		var script := child.get_script() as Script
		if script != null and _quiet_matches(str(script.resource_path)):
			child.set_process(false)
			child.set_physics_process(false)
			disabled += 1
		disabled += _quiet_scene_process(child)
	return disabled

func _initialize() -> void:
	_run()

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return int(raw) if raw.is_valid_int() else fallback

func _env_float(name: String, fallback: float) -> float:
	var raw := OS.get_environment(name)
	return float(raw) if raw.is_valid_float() else fallback

func _value(report: String, label: String) -> int:
	var line_start := report.find(label)
	if line_start < 0:
		return 0
	var rest := report.substr(line_start + label.length())
	var digits := ""
	for ch in rest:
		if ch >= "0" and ch <= "9":
			digits += ch
		elif not digits.is_empty():
			break
	return int(digits) if not digits.is_empty() else 0

func _reason_names(mask: int) -> String:
	const NAMES := ["queue", "immediate", "far_vis", "far_region", "mesh", "region"]
	var parts: Array[String] = []
	for bit in range(NAMES.size()):
		if mask & (1 << bit):
			parts.append(NAMES[bit])
	return "none" if parts.is_empty() else ",".join(parts)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE fps FAIL: main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	# Same boot as probe_start_fill: the title is presentation, so hide it and
	# release the gate by hand; the measurement starts at the release.
	current_scene = main
	await process_frame
	await process_frame
	var menu := root.get_node_or_null("MenuLayer")
	if menu != null:
		var title := menu.get("_title") as CanvasItem
		var curtain := menu.get("_curtain") as CanvasItem
		if title != null:
			title.hide()
		if curtain != null:
			curtain.hide()
		if bool(menu.get("_booted")):
			menu.call("_release")
			menu.set_process(false)

	cm = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("PROBE fps FAIL: ChunkManager missing from main.tscn")
		quit(1)
		return
	if OS.get_environment("QUIET") == "1":
		quiet_match = OS.get_environment("QUIET_MATCH").split(",", false)
		print("PROBE fps quiet scripts=%d" % _quiet_scene_process(main))
	# KILL_AUTOLOAD=name,... frees those autoloads outright, which also stops
	# work they started in _ready (a coroutine or a thread, not just _process).
	for autoload_name in OS.get_environment("KILL_AUTOLOAD").split(",", false):
		var autoload := root.get_node_or_null(autoload_name)
		if autoload != null:
			autoload.free()
			print("PROBE fps freed " + autoload_name)
	# QUIET only walks main; the autoloads hang off root, so a baking autoload
	# (liquid textures, icon rendering) survives it. This arm stops them by name.
	var autoload_off := OS.get_environment("AUTOLOAD_OFF")
	if not autoload_off.is_empty():
		for autoload_name in autoload_off.split(",", false):
			var autoload := root.get_node_or_null(autoload_name)
			if autoload != null:
				autoload.set_process(false)
				autoload.set_physics_process(false)
				print("PROBE fps autoload_off " + autoload_name)

	var rd := _env_int("RD", DEFAULT_RENDER_DISTANCE)
	var secs := _env_float("SECS", DEFAULT_SECS)
	cm.set("render_distance", rd)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	# NO_RENDER=1 stops the render loop: a tail frame that still costs what it did
	# is main-loop work, and one that drops is the renderer's own side.
	if OS.get_environment("NO_RENDER") == "1":
		RenderingServer.render_loop_enabled = false
		print("PROBE fps no_render=1")
	print("PROBE fps start rd=%d secs=%.1f workers=%d" % [rd, secs, int(cm.get_worker_count())])

	var start := Time.get_ticks_usec()
	var window: Array[float] = []
	var reason_frames := [0, 0, 0, 0, 0, 0]
	var budget_on_at := -1.0
	var budget_off_at := -1.0
	var settle_at := -1.0
	var frames := 0
	var worst := 0.0
	var next_second := 1
	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		var t := float(Time.get_ticks_usec() - start) / 1_000_000.0
		frames += 1
		worst = maxf(worst, ms)
		window.append(ms)
		var budget := int(cm.call("get_mesh_rebuild_budget"))
		if budget >= 100:
			if budget_on_at < 0.0:
				budget_on_at = t
		elif budget_on_at >= 0.0 and budget_off_at < 0.0:
			budget_off_at = t
		var reasons := int(cm.call("get_pending_mesh_reasons"))
		for bit in range(6):
			if reasons & (1 << bit):
				reason_frames[bit] += 1
		if t >= float(next_second):
			var total := 0.0
			var second_worst := 0.0
			for value in window:
				total += float(value)
				second_worst = maxf(second_worst, float(value))
			var mean := total / float(max(1, window.size()))
			if settle_at < 0.0 and mean < SETTLE_MEAN_MS and second_worst < SETTLE_WORST_MS:
				settle_at = t
			last_report = cm.get_performance_report()
			var loaded := _value(last_report, "Loaded chunks:")
			# TIME_PROCESS is the scene's own _process (scripts + our node); the
			# rest of a frame is the engine's main loop and the RenderingServer.
			print("PROBE fps t=%d frames=%d mean=%.2fms worst=%.2fms cap=%d res=%d mem=%.0fMB proc=%.2fms phys=%.2fms budget=%d reasons=%d(%s) gen=%d completed=%d pending_mesh=%d loaded=%d settled=%d" % [
				next_second, window.size(), mean, second_worst, Engine.max_fps,
				int(Performance.get_monitor(Performance.OBJECT_RESOURCE_COUNT)),
				float(Performance.get_monitor(Performance.MEMORY_STATIC)) / 1048576.0,
				float(Performance.get_monitor(Performance.TIME_PROCESS)) * 1000.0,
				float(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)) * 1000.0,
				budget, reasons, _reason_names(reasons),
				_value(last_report, "Generating:"), _value(last_report, "Completed queue:"),
				1 if bool(cm.call("has_pending_mesh_work")) else 0, loaded,
				1 if bool(cm.call("is_fill_settled")) else 0])
			window.clear()
			next_second += 1
		if t >= secs:
			break

	print("PROBE fps summary secs=%.1f frames=%d worst=%.2fms settle_1s=%.2fs budget=%s->%s" % [
		secs, frames, worst, settle_at,
		("%.2fs" % budget_on_at) if budget_on_at >= 0.0 else "never",
		("%.2fs" % budget_off_at) if budget_off_at >= 0.0 else "never"])
	print("PROBE fps reasons frames queue=%d immediate=%d far_vis=%d far_region=%d mesh=%d region=%d total_frames=%d" % [
		reason_frames[0], reason_frames[1], reason_frames[2], reason_frames[3],
		reason_frames[4], reason_frames[5], frames])
	print("PROBE fps queue depth=%d visible_pending=%d reasons_now=%d(%s)" % [
		int(cm.call("get_pending_chunk_mesh_count")),
		int(cm.call("get_visible_pending_chunk_mesh_count")),
		int(cm.call("get_pending_mesh_reasons")),
		_reason_names(int(cm.call("get_pending_mesh_reasons")))])
	quit(0 if ok else 1)
