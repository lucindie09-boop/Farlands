extends SceneTree
## How long does the initial stream take to fill the view?
##
## The per-frame report is printed every 2 s, which is too coarse to tell
## "chunks are loaded" from "the world is visually filled". This probe releases
## the real title, pins the render distance, and samples the engine's own report:
##
##   loaded  — the loaded chunk count has been stable for 1 s, with no
##             generation or completed-chunk work in flight
##   mesh    — has_pending_mesh_work() is false
##   far     — no active far-region gaps and no empty active region
##
## The timestamps all start at the title release, not at process launch, because
## that is when the player's world starts. Run windowed:
##
##   probes/run_probe_shot.sh probes/probe_start_fill.gd 120
##
## RD is the render distance to force (default 32).

const DEFAULT_RENDER_DISTANCE := 32
const SAMPLE_SECS := 0.25
const LOADED_STABLE_SECS := 1.0
const TIMEOUT_SECS := 90.0

var main: Node
var cm: Node
var player: Node
var ok := true
var last_report := ""

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else int(raw)

# "Loaded chunks: 20236" / "Generating: 0 chunks"
func _value(report: String, label: String) -> int:
	for line in report.split("\n"):
		if line.contains(label):
			return int(line.get_slice(":", 1).strip_edges().split(" ")[0])
	return -1

# "Far uncovered: regions=20 ... active_uncovered=0 ..."
func _far_uncovered(report: String) -> int:
	for line in report.split("\n"):
		if line.contains("Far uncovered:"):
			for field in line.split(" ", false):
				if field.begins_with("active_uncovered="):
					return int(field.get_slice("=", 1))
	return -1

func _report_line(report: String, label: String) -> String:
	for line in report.split("\n"):
		if line.contains(label):
			return line.strip_edges()
	return ""

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)

	# The title is presentation, not the state being measured: boot the real
	# scene far enough for the autoloads, hide it, and release the gate by hand
	# (same shape as probe_border_fps.gd's REAL_BOOT path).
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
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing from main.tscn")
		quit(1)
		return

	var rd := _env_int("RD", DEFAULT_RENDER_DISTANCE)
	cm.set("render_distance", rd)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0

	var start := Time.get_ticks_usec()
	var last_sample := -SAMPLE_SECS
	var last_loaded := -1
	var stable_since := 0.0
	var loaded_at := -1.0
	var mesh_at := -1.0
	var far_at := -1.0
	var samples := 0
	print("PROBE fill start rd=%d" % rd)

	while true:
		await process_frame
		var elapsed := float(Time.get_ticks_usec() - start) / 1_000_000.0
		if elapsed >= TIMEOUT_SECS:
			break
		if elapsed - last_sample < SAMPLE_SECS:
			continue
		last_sample = elapsed
		samples += 1

		last_report = cm.get_performance_report()
		var loaded := _value(last_report, "Loaded chunks:")
		var generating := _value(last_report, "Generating:")
		var completed := _value(last_report, "Completed queue:")
		var region_empty := _value(last_report, "Region empty:")
		var active_uncovered := _far_uncovered(last_report)
		var pending_mesh := bool(cm.call("has_pending_mesh_work"))

		if loaded != last_loaded or generating > 0 or completed > 0:
			stable_since = elapsed
		if loaded_at < 0.0 and loaded >= 500 and generating == 0 and completed == 0 \
				and elapsed - stable_since >= LOADED_STABLE_SECS:
			loaded_at = elapsed
		last_loaded = loaded

		if mesh_at < 0.0 and loaded_at >= 0.0 and not pending_mesh:
			mesh_at = elapsed
		if far_at < 0.0 and loaded_at >= 0.0 and not pending_mesh \
				and region_empty == 0 and active_uncovered == 0:
			far_at = elapsed
		if mesh_at >= 0.0 and far_at >= 0.0:
			break

	var final_elapsed := float(Time.get_ticks_usec() - start) / 1_000_000.0
	if loaded_at < 0.0:
		_fail("loaded chunks did not settle within %.0f s" % TIMEOUT_SECS)
	if mesh_at < 0.0:
		_fail("pending mesh work did not drain (loaded at %.2f s)" % loaded_at)
	if far_at < 0.0:
		_fail("active far gaps did not close (loaded at %.2f s, mesh at %.2f s)" % [loaded_at, mesh_at])

	print("PROBE fill loaded=%.2fs mesh=%.2fs far=%.2fs end=%.2fs samples=%d" % [
		loaded_at, mesh_at, far_at, final_elapsed, samples])
	for label in ["Loaded chunks:", "Chunk meshes:", "Far cached:",
			"Region empty:", "Far uncovered:", "Unrendered:"]:
		var line := _report_line(last_report, label)
		if not line.is_empty():
			print("PROBE fill final " + line)
	print("PROBE fill %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)
