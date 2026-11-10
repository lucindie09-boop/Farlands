extends SceneTree
## Two-pass start fill for the in-RAM generation memo.
##
## Pass 1 runs with FARLANDS_GEN_MEMO=1 and fills the process-wide memo at the
## generate_chunk boundary; pass 2 re-generates the same world after
## clear_editor_chunks() and every resident chunk is a memo hit. The difference
## is what a cache would buy the fill, before any codec exists.
##
##   FARLANDS_GEN_MEMO=1 FARLANDS_GEN_WORK_PER_CHUNK_MS=1.44 \
##   FARLANDS_GEN_MAX_INFLIGHT=192 FARLANDS_WORKER_COUNT=3 \
##   probes/run_probe_shot.sh probes/probe_fill_memo.gd 120
##
## The settle rule is the start-fill probe's: at least MIN_LOADED chunks, no
## generation and nothing in the completed queue, stable for a full second.

const SAMPLE_SECS := 0.25
const LOADED_STABLE_SECS := 1.0
const MIN_LOADED := 500
const DEFAULT_RD := 32

var cm: Node
var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else int(raw)

func _value(report: String, label: String) -> int:
	for line in report.split("\n"):
		if line.contains(label):
			return int(line.get_slice(":", 1).strip_edges().split(" ")[0])
	return -1

func _wait_fill(start_us: int, timeout_s: float) -> float:
	var last_sample := -SAMPLE_SECS
	var stable_since := 0.0
	while true:
		await process_frame
		var elapsed := float(Time.get_ticks_usec() - start_us) / 1_000_000.0
		if elapsed >= timeout_s:
			return -1.0
		if elapsed - last_sample < SAMPLE_SECS:
			continue
		last_sample = elapsed
		var report: String = cm.get_performance_report()
		var loaded := _value(report, "Loaded chunks:")
		var generating := _value(report, "Generating:")
		var completed := _value(report, "Completed queue:")
		var quiet := loaded >= MIN_LOADED and generating == 0 and completed == 0
		if not quiet:
			stable_since = elapsed
			continue
		if elapsed - stable_since >= LOADED_STABLE_SECS:
			return elapsed
	return -1.0

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
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
		_fail("ChunkManager missing from main.tscn")
		quit(1)
		return

	cm.set("render_distance", _env_int("RD", DEFAULT_RD))
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	var timeout_s := float(_env_int("PROBE_TIMEOUT_SECS", 90))

	var pass1_start := Time.get_ticks_usec()
	var pass1 := await _wait_fill(pass1_start, timeout_s)
	print("PROBE memo pass1=%.2fs" % pass1)
	if pass1 < 0.0:
		_fail("pass 1 did not settle")
		quit(1)
		return
	print("PROBE memo pass1 gen=%s" % cm.get_generation_stats())

	# The process-wide memo survives this reset on purpose: the next fill is the
	# cache-hit arm, not a different world.
	cm.call("clear_editor_chunks")
	await process_frame
	await process_frame
	# Wait for the reset to empty the map before starting the clock, so the new
	# fill is not mixed with the old set's tail.
	for i in 40:
		await process_frame
		if _value(cm.get_performance_report(), "Loaded chunks:") < MIN_LOADED:
			break
	var pass2_start := Time.get_ticks_usec()
	var pass2 := await _wait_fill(pass2_start, timeout_s)
	print("PROBE memo pass2=%.2fs" % pass2)
	if pass2 < 0.0:
		_fail("pass 2 did not settle")
		quit(1)
		return
	print("PROBE memo pass2 gen=%s" % cm.get_generation_stats())
	print("PROBE memo ratio=%.3f saved=%.2fs" % [pass2 / pass1, pass1 - pass2])
	print("PROBE memo %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)
