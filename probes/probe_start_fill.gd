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
var worst_frame_ms := 0.0
var slow_frames := 0
var sampled_frames := 0
var skip_next_frame := false
var phase_ms := {}
var phase_count := {}
# The same totals as they stood when the chunk load settled, so the tail (load ->
# mesh/far done) can be read on its own instead of being diluted by the load.
var phase_ms_at_load := {}
var phase_count_at_load := {}
# The buried test's probe accounting at load settle: the tail's hit/miss ratio
# only means something next to the load's.
var probe_line_at_load := ""
var pop_at_load := {}
var inflight_sum := 0.0
var inflight_max := 0
var completed_sum := 0.0
var light_stage_sum := 0.0
var light_stage_max := 0
var light_result_sum := 0.0
var light_result_max := 0
var dirty_stage_sum := 0.0
var dirty_stage_max := 0
var inflight_samples := 0
var sweep_at_load := {}
# Dirty chunk-queue depth during the mesh tail, and at the moment the chunk
# load settled: decides whether the tail drains a backlog or waits for arrivals.
var tail_queue_sum := 0.0
var tail_queue_max := 0
var tail_queue_samples := 0
var queue_at_load := -1
var builds_at_load := 0
var rejects_at_load := 0
var rebuild_budget_at_load := 0
var stops_view_at_load := 0
var stops_backlog_at_load := 0
# The split of the first 256 frames after the load settled (the far drain), taken
# then because one average over the whole tail hides which half the wall went to.
var early_tail_report := ""
# First visible geometry, the chunk-load settle (before the probe's 1 s stable
# window), and the in-frustum mesh progress at that settle. Those are the hold's
# price; `loaded_at` alone cannot show it.
var chunk_load_at := -1.0
var first_upload_at := -1.0
var first_visible_at := -1.0
var visible_target := 0
var visible_50_at := -1.0
var visible_90_at := -1.0
# In-frustum queue depth per report sample. The 50/90 thresholds are read from
# this series at the end against its own peak: the per-instance detail counters
# exclude chunks drawn through a far region, so they cannot answer how much of
# the view has meshes.
var visible_series: Array[Dictionary] = []
var slow_events: Array[Dictionary] = []
var slow_before_chunk_load := 0
var slow_after_chunk_load := 0
var slow_worst_ms := 0.0
var slow_worst_at := -1.0
# QUIET_MATCH=a,b limits QUIET to scripts whose path contains one of the parts.
var _quiet_match: PackedStringArray = []

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

func _detail_total(report: String) -> int:
	var line := _report_line(report, "Visible chunk detail:")
	var total := 0
	for key in ["full=", "mid=", "far=", "unknown="]:
		var at := line.find(key)
		if at >= 0:
			total += int(line.substr(at + key.length()).get_slice(" ", 0))
	return total

# "Far uncovered: regions=20 ... active_uncovered=0 ..."
# This is informational, not a hole count: a far chunk whose region is active
# but does not list it among its members can still be drawn by its own chunk
# instance. The missing-draw counter is the report's "Unrendered" line.
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

# One report interval's phase totals: avg * count is the time the timer spent in
# that phase, and summing the intervals reconstructs the whole fill. The main
# phase lines print a count now, so this covers generate_chunk/build_mesh and
# the main-thread phases with the same parser.
func _accumulate_phases(report: String) -> void:
	for raw in report.split("\n"):
		var line := raw.strip_edges()
		if not line.contains("avg=") or not line.contains("count="):
			continue
		var colon := line.find(":")
		if colon <= 0:
			continue
		var label := line.substr(0, colon).strip_edges().split(" ")[0]
		var avg := float(line.get_slice("avg=", 1).get_slice("ms", 0))
		var count := int(line.get_slice("count=", 1).strip_edges().split(" ")[0])
		phase_ms[label] = float(phase_ms.get(label, 0.0)) + avg * count
		phase_count[label] = int(phase_count.get(label, 0)) + count

func _light_ms(report: String, key: String) -> float:
	if not report.contains(key):
		return 0.0
	return float(report.get_slice(key, 1).get_slice("ms", 0))

# The sweep's counters as they stood when the chunk load settled. The session
# totals printed below cover the load and the mesh/far tail alike; this snapshot
# is the admission window itself, and the stop table says which exit ended each
# frame's walk.
func _print_sweep_at_load(g: Dictionary, load_s: float) -> void:
	var frames := int(g.get("frames", 0))
	var checks := int(g.get("checks", 0))
	var gens := int(g.get("generations", 0))
	var chain_gens := int(g.get("chain_generations", 0))
	var urgent_gens := int(g.get("urgent_generated", 0))
	var admitted := gens + chain_gens + urgent_gens
	print("PROBE load sweep frames=%d checks=%d gens=%d chain=%d urgent=%d admitted=%d checks/frame=%.1f gens/frame=%.1f chain/frame=%.2f urgent/frame=%.2f" % [
		frames, checks, gens, chain_gens, urgent_gens, admitted,
		float(checks) / maxf(1.0, float(frames)),
		float(gens) / maxf(1.0, float(frames)),
		float(chain_gens) / maxf(1.0, float(frames)),
		float(urgent_gens) / maxf(1.0, float(frames))])
	print("PROBE load sweep stops admission=%d budget=%d band=%d spent=%d gen_cap=%d check_cap=%d dynamic_max_last=%d dynamic_max_max=%d" % [
		int(g.get("stop_admission", 0)), int(g.get("stop_budget", 0)),
		int(g.get("stop_band", 0)), int(g.get("stop_spent", 0)),
		int(g.get("stop_generation_cap", 0)), int(g.get("stop_check_cap", 0)),
		int(g.get("dynamic_max_generations", 0)), int(g.get("dynamic_max_generations_max", 0))])
	print("PROBE load sweep chain offered=%d gens=%d skipped=%d refused=%d budget_stops=%d pending_max=%d | urgent requested=%d generated=%d pending_max=%d" % [
		int(g.get("chain_offered", 0)), int(g.get("chain_generations", 0)),
		int(g.get("chain_skipped", 0)), int(g.get("chain_refused", 0)),
		int(g.get("chain_budget_stops", 0)), int(g.get("chain_pending_max", 0)),
		int(g.get("urgent_requested", 0)), int(g.get("urgent_generated", 0)),
		int(g.get("urgent_pending_max", 0))])
	print("PROBE load sweep list columns=%d candidates=%d cursor_resets=%d passes=%d sweeps=%d" % [
		int(g.get("candidate_columns", 0)), int(g.get("candidate_offsets", 0)),
		int(g.get("cursor_resets", 0)), int(g.get("sweep_passes", 0)),
		int(g.get("sweeps_completed", 0))])
	print("PROBE load sweep band reads=%d total_ms=%.1f max_ms=%.2f columns_built=%d columns_skipped=%d rejected loaded=%d inflight=%d above=%d below=%d oob=%d | window checks=%d gens=%d frames=%d" % [
		int(g.get("band_reads", 0)), float(g.get("total_band_ms", 0.0)),
		float(g.get("max_band_ms", 0.0)), int(g.get("columns_built", 0)),
		int(g.get("columns_skipped", 0)), int(g.get("reject_loaded", 0)),
		int(g.get("reject_inflight", 0)), int(g.get("reject_above", 0)),
		int(g.get("reject_below", 0)), int(g.get("reject_oob", 0)),
		int(g.get("window_checks", 0)), int(g.get("window_generations", 0)),
		int(g.get("window_frames", 0))])
	print("PROBE load sweep installs total=%d empty=%d above_top=%d above_top_empty=%d below_land=%d below_land_empty=%d in_range=%d in_range_empty=%d unknown=%d" % [
		int(g.get("installs_total", 0)), int(g.get("installs_empty", 0)),
		int(g.get("installs_above_top", 0)), int(g.get("installs_above_top_empty", 0)),
		int(g.get("installs_below_land", 0)), int(g.get("installs_below_land_empty", 0)),
		int(g.get("installs_in_range", 0)), int(g.get("installs_in_range_empty", 0)),
		int(g.get("installs_bounds_unknown", 0))])
	print("PROBE load sweep time total=%.1fms avg=%.3fms max=%.2fms | load=%.2fs" % [
		float(g.get("total_ms", 0.0)), float(g.get("avg_ms", 0.0)),
		float(g.get("max_ms", 0.0)), load_s])

func _measure_script_costs(root_node: Node) -> void:
	var rows: Array = []
	_collect_script_costs(root_node, rows)
	rows.sort_custom(func(a, b): return float(a["us"]) > float(b["us"]))
	var total := 0.0
	for row in rows:
		total += float(row["us"])
		print("PROBE script cost us=%.2f per_call %s" % [float(row["us"]), str(row["path"])])
	print("PROBE script cost total_us=%.2f scripts=%d" % [total, rows.size()])


func _collect_script_costs(node: Node, rows: Array) -> void:
	for child in node.get_children():
		var script := child.get_script() as Script
		if script != null and child.has_method("_process"):
			var start := Time.get_ticks_usec()
			for i in 500:
				child.call("_process", 0.0)
			var per := float(Time.get_ticks_usec() - start) / 500.0
			rows.append({"us": per, "path": str(script.resource_path)})
		_collect_script_costs(child, rows)


func _quiet_matches(path: String) -> bool:
	if _quiet_match.is_empty():
		return true
	for part in _quiet_match:
		if path.contains(part):
			return true
	return false

func _quiet_scene_process(node: Node) -> int:
	var disabled := 0
	for child in node.get_children():
		var script := child.get_script() as Script
		if script != null and child != player and _quiet_matches(str(script.resource_path)):
			child.set_process(false)
			child.set_physics_process(false)
			print("PROBE fill quiet script " + str(script.resource_path))
			disabled += 1
		disabled += _quiet_scene_process(child)
	return disabled


func _print_fill_phases(wall_s: float, load_s: float, light_report: String) -> void:
	print("PROBE fill phases wall=%.2fs load=%.2fs tail=%.2fs" % [wall_s, load_s, wall_s - load_s])
	# What the last frames cost besides the updater. The report's window average
	# splits the frame into our callback, the engine's renderer measure, and the
	# rest (other nodes' _process, physics, idle); the per-frame renderer measure
	# is reliable, unlike the rolling TIME_PROCESS.
	if not early_tail_report.is_empty():
		for early_line in early_tail_report.split("\n"):
			if not early_line.is_empty():
				print("PROBE fill earlytail " + early_line)
	var tail_report: String = cm.call("get_slow_frame_report", 256, 0.0)
	for tail_line in tail_report.split("\n"):
		if not tail_line.is_empty():
			print("PROBE fill tail " + tail_line)
	for label in ["generate_chunk", "process_completed_chunks", "build_mesh",
			"process_completed_meshes", "dirty_mesh_queue", "mesh_queue_chunks",
			"mesh_queue_far", "mesh_pop_callback", "mesh_pop_detail", "mesh_pop_far",
			"mesh_pop_lookup", "mesh_pop_construct", "mesh_pop_early", "mesh_pop_buried",
			"mesh_pop_buried_probes", "mesh_pop_buried_frees", "mesh_pop_buried_farmark",
			"mesh_pop_buried_rest",
			"mesh_pop_requeue",
			"solid_cache", "greedy_h", "greedy_v", "worker_task",
			"chunk_load_unload",
			"far_region_visibility", "mesh_reprioritize", "process_total"]:
		var total_ms := float(phase_ms.get(label, 0.0))
		var count := int(phase_count.get(label, 0))
		var each := total_ms / float(count) if count > 0 else 0.0
		if label == "generate_chunk" or label == "build_mesh":
			var workers := total_ms / (wall_s * 1000.0) if wall_s > 0.0 else 0.0
			var workers_load := total_ms / (load_s * 1000.0) if load_s > 0.0 else 0.0
			print("PROBE fill phase %s total=%.2fs count=%d avg=%.2fms workers=%.2f workers_load=%.2f" % [
				label, total_ms / 1000.0, count, each, workers, workers_load])
		else:
			var share := 100.0 * total_ms / (wall_s * 1000.0) if wall_s > 0.0 else 0.0
			print("PROBE fill phase %s total=%.2fs count=%d avg=%.2fms share=%.1f%%" % [
				label, total_ms / 1000.0, count, each, share])
		var tail_total := total_ms - float(phase_ms_at_load.get(label, 0.0))
		var tail_count := count - int(phase_count_at_load.get(label, 0))
		var tail_each := tail_total / float(tail_count) if tail_count > 0 else 0.0
		var tail_share := 100.0 * tail_total / ((wall_s - load_s) * 1000.0) if wall_s > load_s else 0.0
		print("PROBE fill tail phase %s total=%.2fs count=%d avg=%.2fms share=%.1f%%" % [
			label, tail_total / 1000.0, tail_count, tail_each, tail_share])
		# The load window: the same counters snapshotted when the chunk set
		# settled, divided by the chunks installed in it. This is the main thread's
		# per-install cost, which the whole-run total hides behind the tail.
		var load_total := float(phase_ms_at_load.get(label, 0.0))
		var load_count := int(phase_count_at_load.get(label, 0))
		var load_each := load_total / float(load_count) if load_count > 0 else 0.0
		var load_installs := float(maxi(1, int(sweep_at_load.get("installs_total", 0))))
		var per_install := load_total / load_installs * 1000.0
		print("PROBE load phase %s total=%.2fs count=%d avg=%.2fms per_install=%.1fus" % [
			label, load_total / 1000.0, load_count, load_each, per_install])
	print("PROBE fill phase light hold=%.2fs wait=%.2fs" % [
		_light_ms(light_report, "hold total=") / 1000.0,
		_light_ms(light_report, "wait total=") / 1000.0])

func _unpack_stops(packed: int) -> Array:
	return [(packed >> 48) & 0xFFF, (packed >> 36) & 0xFFF, (packed >> 24) & 0xFFF,
		(packed >> 12) & 0xFFF, packed & 0xFFF]

func _stops_line(tag: String, now: int, at_load: int) -> void:
	var a: Array = _unpack_stops(now)
	var b: Array = _unpack_stops(at_load)
	print("PROBE fill stops %s budget=%d done=%d popcap=%d empty=%d zero=%d tail_budget=%d tail_done=%d tail_popcap=%d tail_empty=%d tail_zero=%d" % [
		tag, a[0], a[1], a[2], a[3], a[4],
		int(a[0]) - int(b[0]), int(a[1]) - int(b[1]), int(a[2]) - int(b[2]),
		int(a[3]) - int(b[3]), int(a[4]) - int(b[4])])

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

	# QUIET=1 silences every scripted node in the world scene except the player
	# controller (the camera streaming follows): the fill A/B for "is the tail's
	# outside-updater time the rest of the scene's _process?". Streaming and the
	# renderer are untouched.
	_quiet_match = OS.get_environment("QUIET_MATCH").split(",", false)
	if OS.get_environment("QUIET") == "1":
		print("PROBE fill quiet scripts=%d" % _quiet_scene_process(main))
	# SCRIPT_COST=1: time each node's _process body directly. The tail windows
	# mix drain and idle frames, so a bisection there measures the phase, not the
	# script; 500 calls of the body is the steady per-frame cost the fill pays.
	if OS.get_environment("SCRIPT_COST") == "1":
		_measure_script_costs(main)
	# NO_RENDER=1 stops the render loop: the world is not drawn, so a tail frame
	# that still costs what it did is main-loop work, and one that drops is the
	# renderer's own side (submission, uploads, GPU sync).
	if OS.get_environment("NO_RENDER") == "1":
		RenderingServer.render_loop_enabled = false
		print("PROBE fill no_render=1")

	var rd := _env_int("RD", DEFAULT_RENDER_DISTANCE)
	var probe_timeout_secs := float(_env_int("PROBE_TIMEOUT_SECS", int(TIMEOUT_SECS)))
	var latch_flight_secs := _env_int("LATCH_FLIGHT", 0)
	cm.set("render_distance", rd)
	# The constrained-hardware arm keeps the display's vsync; every other probe
	# disables it so a frame measures the engine, not the refresh rate. The
	# project setting is off, so this is the switch that turns it on.
	if OS.get_environment("VSYNC_ON") == "1":
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_ENABLED)
	else:
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	print("PROBE fill vsync=%d" % DisplayServer.window_get_vsync_mode())
	Engine.max_fps = 0

	var start := Time.get_ticks_usec()
	var last_sample := -SAMPLE_SECS
	var last_loaded := -1
	var stable_since := 0.0
	var loaded_at := -1.0
	var mesh_at := -1.0
	var far_at := -1.0
	var samples := 0
	# Per-frame count of which pending reason held the tail open. The chunk heaps
	# are only one of six; while the tail was 680 frames with an empty backlog, this
	# is the only way to name what it was waiting for.
	var tail_reason_frames := [0, 0, 0, 0, 0, 0]
	# The same, restricted to frames whose chunk heaps (dirty + backlog) are empty:
	# that is the state the question is about, and in the running tail the queue is
	# pending too, so the unrestricted counts cannot separate the reasons.
	var tail_reason_empty_frames := [0, 0, 0, 0, 0, 0]
	var tail_empty_queue_frames := 0
	var tail_reason_last := 0
	var load_frame_sum := 0.0
	var load_frame_count := 0
	var load_worst_ms := 0.0
	var load_slow_frames := 0
	var frame_ms_sum := 0.0
	var report_usec := 0
	var report_calls := 0
	var frames_at_load := 0
	var tail_all_frame_ms := 0.0
	var tail_frames := 0
	var fill_trace := OS.get_environment("FILL_TRACE") == "1"
	var next_fill_trace := 0.0
	var fill_settled_at := -1.0
	print("PROBE fill start rd=%d workers=%d" % [rd, cm.get_worker_count()])

	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var frame_ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		# Every interval after the load settled, skipped report frames included:
		# the tail wall minus this sum is time the loop spent outside a frame.
		if loaded_at >= 0.0:
			tail_all_frame_ms += frame_ms
			tail_frames += 1
		if skip_next_frame:
			# The report call runs between frames; its cost lands in the next
			# frame interval, so do not count that interval as engine work.
			skip_next_frame = false
		else:
			sampled_frames += 1
			frame_ms_sum += frame_ms
			worst_frame_ms = maxf(worst_frame_ms, frame_ms)
			if frame_ms > 33.4:
				slow_frames += 1
			if frame_ms >= 30.0:
				var slow_at := float(Time.get_ticks_usec() - start) / 1_000_000.0
				if slow_at > slow_worst_at:
					slow_worst_at = slow_at
					slow_worst_ms = frame_ms
				if chunk_load_at < 0.0:
					slow_before_chunk_load += 1
				else:
					slow_after_chunk_load += 1
				# Keep the WORST frames, not the first ones: startup frames are
				# often slow too, so a first-24 buffer fills before the release
				# burst and never records the fill's worst frame.
				var slot := -1
				if slow_events.size() < 24:
					slot = slow_events.size()
				else:
					var lightest := 0
					for i in range(1, slow_events.size()):
						if float(slow_events[i]["ms"]) < float(slow_events[lightest]["ms"]):
							lightest = i
					if frame_ms > float(slow_events[lightest]["ms"]):
						slot = lightest
				if slot >= 0:
					var event := {"at": slow_at, "ms": frame_ms,
						"report": cm.get_slow_frame_report(1, 30.0)}
					if slot < slow_events.size():
						slow_events[slot] = event
					else:
						slow_events.append(event)
					# The capture runs between frames, so its cost lands in the NEXT
					# interval. Without this the report can make an ordinary frame
					# look slow, and that fake event then captures another report.
					skip_next_frame = true
			if loaded_at < 0.0:
				load_frame_sum += frame_ms
				load_frame_count += 1
				load_worst_ms = maxf(load_worst_ms, frame_ms)
				if frame_ms > 33.4:
					load_slow_frames += 1
		if loaded_at >= 0.0 and mesh_at < 0.0:
			var reasons := int(cm.call("get_pending_mesh_reasons"))
			var queue_now := int(cm.call("get_pending_chunk_mesh_count"))
			tail_reason_last = reasons
			if queue_now == 0:
				tail_empty_queue_frames += 1
			for bit in range(6):
				if reasons & (1 << bit):
					tail_reason_frames[bit] += 1
					if queue_now == 0:
						tail_reason_empty_frames[bit] += 1
		var elapsed := float(Time.get_ticks_usec() - start) / 1_000_000.0
		# Latch validation: the loading budgets are live only between the chunk load
		# settling and the 1 s-quiet/30 s-cap exit. Start moving the moment the
		# budget jumps to the loading value, so the flight keeps mesh work pending
		# through the cap instead of measuring a world that already went quiet.
		if latch_flight_secs > 0 and int(cm.call("get_mesh_rebuild_budget")) >= 100:
			await _latch_flight(float(latch_flight_secs), elapsed)
			quit(0 if ok else 1)
			return
		if elapsed >= probe_timeout_secs:
			break
		if elapsed - last_sample < SAMPLE_SECS:
			continue
		last_sample = elapsed
		samples += 1

		var report_start := Time.get_ticks_usec()
		last_report = cm.get_performance_report()
		report_usec += Time.get_ticks_usec() - report_start
		report_calls += 1
		_accumulate_phases(last_report)
		skip_next_frame = true
		var loaded := _value(last_report, "Loaded chunks:")
		var generating := _value(last_report, "Generating:")
		var completed := _value(last_report, "Completed queue:")
		inflight_sum += float(generating)
		inflight_max = max(inflight_max, generating)
		completed_sum += float(completed)
		var light_stages := _value(last_report, "Light stages:")
		var light_results := _value(last_report, "Light results:")
		var dirty_stages := _value(last_report, "Dirty stages:")
		light_stage_sum += float(light_stages)
		light_stage_max = maxi(light_stage_max, light_stages)
		light_result_sum += float(light_results)
		light_result_max = maxi(light_result_max, light_results)
		dirty_stage_sum += float(dirty_stages)
		dirty_stage_max = maxi(dirty_stage_max, dirty_stages)
		inflight_samples += 1
		var region_empty := _value(last_report, "Region empty:")
		var active_uncovered := _far_uncovered(last_report)
		var unrendered := _value(last_report, "Unrendered:")
		var pending_mesh := bool(cm.call("has_pending_mesh_work"))
		var queue_depth := int(cm.call("get_pending_chunk_mesh_count"))
		var queue_builds := int(cm.call("get_mesh_queue_builds"))
		var queue_rejects := int(cm.call("get_mesh_queue_rejects"))
		var visible_pending := int(cm.call("get_visible_pending_chunk_mesh_count"))
		var visible_drawn := _detail_total(last_report)
		var visible_total_now := visible_drawn + visible_pending
		if fill_trace and elapsed >= next_fill_trace:
			next_fill_trace = elapsed + 1.0
			var settled_now := bool(cm.call("is_fill_settled"))
			if settled_now and fill_settled_at < 0.0:
				fill_settled_at = elapsed
				print("PROBE fill settle t=%.2fs" % elapsed)
			var far_geometry: Dictionary = cm.call("get_pending_far_geometry")
			print("PROBE fill trace t=%.2fs loaded=%d generating=%d completed=%d pending=%d budget=%d settled=%d region_empty=%d unrendered=%d uncovered=%d far_total=%d far_no_chunk=%d far_empty=%d" % [
				elapsed, loaded, generating, completed, queue_depth,
				int(cm.call("get_mesh_rebuild_budget")), int(settled_now), region_empty,
				unrendered, active_uncovered, int(far_geometry.get("total", 0)),
				int(far_geometry.get("no_chunk", 0)), int(far_geometry.get("empty", 0))])
		if first_upload_at < 0.0 and _value(last_report, "Mesh uploads:") > 0:
			first_upload_at = elapsed
		if first_visible_at < 0.0 and (_value(last_report, "Visible instances:") > 0 or visible_drawn > 0):
			first_visible_at = elapsed

		# Settle on the counts alone: a fast idle frame rate (this probe can run at
		# 1,000 fps headless) makes the old "loaded changed" reset fire every
		# sample, because last_loaded only advanced once the latch was set.
		var load_quiet := loaded >= 500 and generating == 0 and completed == 0
		if not load_quiet:
			stable_since = elapsed
		if chunk_load_at < 0.0 and load_quiet:
			chunk_load_at = elapsed
			visible_target = maxi(visible_total_now, visible_drawn)
		if loaded_at < 0.0 and load_quiet and elapsed - stable_since >= LOADED_STABLE_SECS:
			loaded_at = elapsed
			frames_at_load = sampled_frames
			# The counters at settle are the load window; the session totals at
			# the end also include the mesh/far tail and the idle frames.
			sweep_at_load = cm.get_generation_stats()
			phase_ms_at_load = phase_ms.duplicate(true)
			phase_count_at_load = phase_count.duplicate(true)
			queue_at_load = queue_depth
			builds_at_load = queue_builds
			rejects_at_load = queue_rejects
			rebuild_budget_at_load = int(cm.call("get_mesh_rebuild_budget"))
			stops_view_at_load = int(cm.call("get_mesh_queue_stop_counts_view"))
			stops_backlog_at_load = int(cm.call("get_mesh_queue_stop_counts_backlog"))
			probe_line_at_load = _report_line(last_report, "Solid probes:")
			pop_at_load = cm.call("get_mesh_pop_outcomes")
			last_loaded = loaded

		if loaded_at >= 0.0 and mesh_at < 0.0:
			tail_queue_sum += float(queue_depth)
			tail_queue_max = maxi(tail_queue_max, queue_depth)
			tail_queue_samples += 1
		if chunk_load_at >= 0.0:
			visible_series.append({"at": elapsed, "pending": visible_pending})
		if mesh_at < 0.0 and loaded_at >= 0.0 and not pending_mesh:
			mesh_at = elapsed
		# The gate is missing geometry, not region membership. `active_uncovered`
		# counts visible far chunks the active region does not list; if their own
		# chunk instance is visible, they are drawn and there is no hole. The 3-worker
		# RD32 run sat at 92 uncovered for 300 s with Unrendered=0 and no pending work.
		if far_at < 0.0 and loaded_at >= 0.0 and not pending_mesh \
				and region_empty == 0 and unrendered == 0:
			far_at = elapsed
		# The first 256 frames after the load settles are the far drain; the capture
		# keeps their split separately from the idle frames at the end, because one
		# average over the whole tail hides which half the wall time went to.
		if loaded_at >= 0.0 and early_tail_report.is_empty() \
				and sampled_frames - frames_at_load >= 256:
			early_tail_report = cm.call("get_slow_frame_report", 256, 0.0)
		if mesh_at >= 0.0 and far_at >= 0.0:
			break

	var final_elapsed := float(Time.get_ticks_usec() - start) / 1_000_000.0
	if loaded_at < 0.0:
		_fail("loaded chunks did not settle within %.0f s" % probe_timeout_secs)
	if mesh_at < 0.0:
		_fail("pending mesh work did not drain (loaded at %.2f s)" % loaded_at)
	if far_at < 0.0:
		_fail("unrendered far chunks did not close (loaded at %.2f s, mesh at %.2f s)" % [loaded_at, mesh_at])

	print("PROBE fill loaded=%.2fs mesh=%.2fs far=%.2fs end=%.2fs samples=%d frames=%d worst=%.2fms slow=%d" % [
		loaded_at, mesh_at, far_at, final_elapsed, samples, sampled_frames, worst_frame_ms, slow_frames])
	# Read the in-frustum drain against its own peak: the held arm starts with the
	# whole visible set queued, the overlap arm peaks somewhere inside the load.
	if not visible_series.is_empty():
		var peak := 0
		var peak_at := 0.0
		for sample in visible_series:
			if int(sample["pending"]) > peak:
				peak = int(sample["pending"])
				peak_at = float(sample["at"])
		if peak > 0:
			visible_target = peak
			for sample in visible_series:
				if float(sample["at"]) < peak_at:
					continue
				var pending := int(sample["pending"])
				if visible_50_at < 0.0 and pending * 2 <= peak:
					visible_50_at = float(sample["at"])
				if visible_90_at < 0.0 and pending * 10 <= peak:
					visible_90_at = float(sample["at"])
	print("PROBE fill first chunk_load=%.3fs upload=%.3fs visible=%.3fs visible50=%.3fs visible90=%.3fs target=%d" % [
		chunk_load_at, first_upload_at, first_visible_at, visible_50_at, visible_90_at, visible_target])
	print("PROBE fill slowcount before_chunk_load=%d after_chunk_load=%d slow_worst_at=%.3fs slow_worst_ms=%.2f" % [
		slow_before_chunk_load, slow_after_chunk_load, slow_worst_at, slow_worst_ms])
	slow_events.sort_custom(func(a, b): return float(a["at"]) < float(b["at"]))
	for event in slow_events:
		print("PROBE fill slow at=%.3fs frame=%.2fms chunk_load=%.3fs loaded=%.3fs report=%s" % [
			float(event["at"]), float(event["ms"]), chunk_load_at, loaded_at,
			String(event["report"]).replace("\n", " | ")])
	print("PROBE fill frame avg=%.2fms load_avg=%.2fms load_max=%.2fms load_slow=%d/%d worst=%.2fms slow=%d/%d" % [
		final_elapsed * 1000.0 / float(max(1, sampled_frames)),
		load_frame_sum / float(max(1, load_frame_count)), load_worst_ms,
		load_slow_frames, load_frame_count, worst_frame_ms, slow_frames, sampled_frames])
	# The first number above divides the wall by the counted frames, so the probe's
	# own report calls (skipped frames, invisible to the mean) inflate it. This is
	# the mean of the measured intervals, and the report's price on its own.
	print("PROBE fill frame measured avg=%.2fms report calls=%d total=%.2fs avg=%.1fms" % [
		frame_ms_sum / float(max(1, sampled_frames)), report_calls,
		report_usec / 1_000_000.0, report_usec / 1000.0 / float(max(1, report_calls))])
	var tail_wall_ms := (final_elapsed - loaded_at) * 1000.0 if loaded_at >= 0.0 else 0.0
	print("PROBE fill tail accounting wall=%.0fms frames=%.0fms gaps=%.0fms frames=%d" % [
		tail_wall_ms, tail_all_frame_ms, tail_wall_ms - tail_all_frame_ms, tail_frames])
	for label in ["Loaded chunks:", "Chunk meshes:", "Visible chunk detail:", "Far cached:",
			"Region empty:", "Region uploads:", "Far uncovered:", "Unrendered:"]:
		var line := _report_line(last_report, label)
		if not line.is_empty():
			print("PROBE fill final " + line)
	var storage: Dictionary = cm.call("get_chunk_storage_histogram")
	print("PROBE fill storage all_air=%d fully_solid=%d mixed=%d uniform_sections=%d block_bytes=%d total_bytes=%d" % [
		int(storage.get("all_air", 0)), int(storage.get("fully_solid", 0)),
		int(storage.get("mixed", 0)), int(storage.get("uniform_sections", 0)),
		int(storage.get("block_bytes", 0)), int(storage.get("total_bytes", 0))])
	var pop_end: Dictionary = cm.call("get_mesh_pop_outcomes")
	if not pop_at_load.is_empty():
		var names := ["callbacks", "total", "missing", "no_pipeline", "distance", "clean",
			"air", "buried", "in_flight", "built"]
		var parts: Array[String] = []
		for key in names:
			parts.append("%s=%d" % [key, int(pop_end.get(key, 0)) - int(pop_at_load.get(key, 0))])
		print("PROBE fill mesh pops " + " ".join(parts))
	var spike: Dictionary = cm.call("get_region_spike_stats")
	if int(spike.get("regions_seen", 0)) > 0:
		print("PROBE fill region spike seen=%d max_members=%d regions=%d chunks=%d bytes=%d snapshot_ms=%.1f wait_total_ms=%.1f wait_max_ms=%.2f hold_total_ms=%.1f hold_max_ms=%.2f mesh_ms=%.1f mesh_max_ms=%.2f vertices=%d" % [
			int(spike.get("regions_seen", 0)), int(spike.get("max_members", 0)),
			int(spike.get("regions", 0)), int(spike.get("chunks", 0)), int(spike.get("bytes", 0)),
			float(spike.get("snapshot_ms", 0.0)), float(spike.get("wait_total_ms", 0.0)),
			float(spike.get("wait_max_ms", 0.0)), float(spike.get("hold_total_ms", 0.0)),
			float(spike.get("hold_max_ms", 0.0)), float(spike.get("mesh_ms", 0.0)),
			float(spike.get("mesh_max_ms", 0.0)), int(spike.get("vertices", 0))])
	if not probe_line_at_load.is_empty():
		print("PROBE fill probes at_load " + probe_line_at_load)
	var probes_end := _report_line(last_report, "Solid probes:")
	if not probes_end.is_empty():
		print("PROBE fill probes at_end " + probes_end)
	var light_report: String = cm.get_light_profile_report()
	print("PROBE light profile: %s" % light_report)
	var gen_path_report: String = cm.get_generation_path_report()
	print("PROBE gen path: %s" % gen_path_report)
	var install_report: String = cm.get_install_profile_report()
	print("PROBE install profile: %s" % install_report)
	if not sweep_at_load.is_empty():
		_print_sweep_at_load(sweep_at_load, maxf(0.0, loaded_at - LOADED_STABLE_SECS))
	_print_fill_phases(far_at, maxf(0.0, loaded_at - LOADED_STABLE_SECS), light_report)
	var q_ms := float(phase_ms.get("mesh_queue_chunks", 0.0))
	var cb_ms := float(phase_ms.get("mesh_pop_callback", 0.0))
	print("PROBE fill pop split bookkeeping=%.2fs callback=%.2fs early=%.2fs lookup=%.2fs buried=%.2fs requeue=%.2fs construct=%.2fs" % [
		(q_ms - cb_ms) / 1000.0, cb_ms / 1000.0,
		float(phase_ms.get("mesh_pop_early", 0.0)) / 1000.0,
		float(phase_ms.get("mesh_pop_lookup", 0.0)) / 1000.0,
		float(phase_ms.get("mesh_pop_buried", 0.0)) / 1000.0,
		float(phase_ms.get("mesh_pop_requeue", 0.0)) / 1000.0,
		float(phase_ms.get("mesh_pop_construct", 0.0)) / 1000.0])
	if tail_queue_samples > 0:
		print("PROBE fill tail queue depth avg=%.1f max=%d at_load=%d samples=%d" % [
			tail_queue_sum / float(tail_queue_samples), tail_queue_max, queue_at_load, tail_queue_samples])
	print("PROBE fill tail pending reason frames queue=%d immediate=%d far_vis=%d far_region=%d completed_mesh=%d completed_region=%d" % [
		tail_reason_frames[0], tail_reason_frames[1], tail_reason_frames[2],
		tail_reason_frames[3], tail_reason_frames[4], tail_reason_frames[5]])
	print("PROBE fill tail pending reason empty_queue frames=%d queue=%d immediate=%d far_vis=%d far_region=%d completed_mesh=%d completed_region=%d last_mask=%d" % [
		tail_empty_queue_frames, tail_reason_empty_frames[0], tail_reason_empty_frames[1],
		tail_reason_empty_frames[2], tail_reason_empty_frames[3], tail_reason_empty_frames[4],
		tail_reason_empty_frames[5], tail_reason_last])
	_stops_line("view", int(cm.call("get_mesh_queue_stop_counts_view")), stops_view_at_load)
	_stops_line("backlog", int(cm.call("get_mesh_queue_stop_counts_backlog")), stops_backlog_at_load)
	var builds_end := int(cm.call("get_mesh_queue_builds"))
	var rejects_end := int(cm.call("get_mesh_queue_rejects"))
	print("PROBE fill queue drain builds=%d rejects=%d tail_builds=%d tail_rejects=%d rebuild_budget_at_load=%d rebuild_budget_end=%d" % [
		builds_end, rejects_end, builds_end - builds_at_load, rejects_end - rejects_at_load,
		rebuild_budget_at_load, int(cm.call("get_mesh_rebuild_budget"))])
	var g: Dictionary = cm.get_generation_stats()
	var gen_frames := int(g.get("frames", 0))
	var gen_checks := int(g.get("checks", 0))
	var gen_count := int(g.get("generations", 0))
	print("PROBE fill gen frames=%d checks=%d gens=%d checks/gen=%.1f checks/frame=%.1f gens/frame=%.1f sweeps=%d rejected loaded=%d above=%d below=%d oob=%d inflight=%d refused=%d" % [
		gen_frames, gen_checks, gen_count,
		float(gen_checks) / max(1.0, float(gen_count)),
		float(gen_checks) / max(1.0, float(gen_frames)),
		float(gen_count) / max(1.0, float(gen_frames)),
		int(g.get("sweeps_completed", 0)),
		int(g.get("reject_loaded", 0)), int(g.get("reject_above", 0)),
		int(g.get("reject_below", 0)), int(g.get("reject_oob", 0)),
		int(g.get("reject_inflight", 0)), int(g.get("generate_refused", 0))])
	if inflight_samples > 0:
		print("PROBE fill gen inflight avg=%.1f max=%d completed avg=%.1f samples=%d" % [
			inflight_sum / float(inflight_samples), inflight_max,
			completed_sum / float(inflight_samples), inflight_samples])
		print("PROBE fill drain light_stages avg=%.1f max=%d light_results avg=%.1f max=%d dirty_stages avg=%.1f max=%d" % [
			light_stage_sum / float(inflight_samples), light_stage_max,
			light_result_sum / float(inflight_samples), light_result_max,
			dirty_stage_sum / float(inflight_samples), dirty_stage_max])
	print("PROBE fill bands reads=%d total_ms=%.1f max_ms=%.2f max_cols=%d rebuilds=%d rebuild_ms=%.1f built=%d skipped=%d chain=%d" % [
		int(g.get("band_reads", 0)), float(g.get("total_band_ms", 0.0)),
		float(g.get("max_band_ms", 0.0)), int(g.get("max_band_columns", 0)),
		int(g.get("rebuilds", 0)), float(g.get("total_rebuild_ms", 0.0)),
		int(g.get("columns_built", 0)), int(g.get("columns_skipped", 0)),
		int(g.get("chain_generations", 0))])
	print("PROBE fill %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)

func _p99(values: Array) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	return float(sorted[int(floor(float(sorted.size() - 1) * 0.99))])

# One fast flight while the initial-loading latch is up. The frame-time split is
# the point: the last six seconds are after the 30 s cap in the A arm and still
# inside the quiet-only latch in the B arm. The budget transitions name which
# latch actually ended the mode, rather than inferring it from a timestamp.
func _latch_flight(seconds: float, started_at: float) -> void:
	var start_pos: Vector3 = player.global_position
	var start_us := Time.get_ticks_usec()
	var frame_times: Array = []
	var late_times: Array = []
	var slow := 0
	var late_slow := 0
	var worst := 0.0
	var late_worst := 0.0
	var budget_start := int(cm.call("get_mesh_rebuild_budget"))
	var budget_end := budget_start
	var latch_live := budget_start >= 100
	var on_s := 0.0
	var end_s := -1.0
	var gen_start: Dictionary = cm.get_generation_stats()
	print("PROBE latch start=%.2fs flight=%.1fs rd=%d max_sec=%s quiet_sec=%s" % [
		started_at, seconds, int(cm.get("render_distance")),
		OS.get_environment("FARLANDS_LOADING_LATCH_MAX_SEC"),
		OS.get_environment("FARLANDS_LOADING_LATCH_QUIET_SEC")])
	while true:
		var t := float(Time.get_ticks_usec() - start_us) / 1_000_000.0
		if t >= seconds:
			break
		# A straight 100-block/s line. The y is held at the spawn height; this is a
		# streaming and frame-time load, not a terrain-following test.
		player.teleport_to(Vector3(start_pos.x + 100.0 * t, start_pos.y, start_pos.z))
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		var now := float(Time.get_ticks_usec() - start_us) / 1_000_000.0
		frame_times.append(ms)
		if now >= seconds - 6.0:
			late_times.append(ms)
			if ms > 33.4:
				late_slow += 1
			late_worst = maxf(late_worst, ms)
		if ms > 33.4:
			slow += 1
		worst = maxf(worst, ms)
		var budget := int(cm.call("get_mesh_rebuild_budget"))
		budget_end = budget
		if budget >= 100:
			if latch_live:
				on_s = now
		elif latch_live:
			latch_live = false
			end_s = now
			on_s = now
	if latch_live and end_s < 0.0:
		on_s = seconds
	# What the flight left behind: the dirty/backlog queue is the work still
	# waiting for a rebuild, and the reason mask says which of the six pending
	# kinds are still open (the queue is only one of them).
	var backlog_end := int(cm.call("get_pending_chunk_mesh_count"))
	var reasons_end := int(cm.call("get_pending_mesh_reasons"))
	var visible_end := int(cm.call("get_visible_pending_chunk_mesh_count"))
	var split: Dictionary = cm.call("get_visible_pending_lod_split")
	var tiers: Array = [int(split.get("full", 0)), int(split.get("mid", 0)), int(split.get("far", 0))]
	var bands: Array = split.get("bands", [0, 0, 0, 0, 0, 0])
	var geometry: Dictionary = cm.call("get_pending_far_geometry")
	var view_camera: Camera3D = root.get_camera_3d()
	var holes: Dictionary = {}
	if view_camera != null:
		holes = cm.call("get_visible_far_holes_screen", view_camera.global_transform,
			view_camera.get_camera_projection(), root.get_visible_rect().size)
	var mean := 0.0
	for value in frame_times:
		mean += float(value)
	mean /= float(max(1, frame_times.size()))
	var late_mean := 0.0
	for value in late_times:
		late_mean += float(value)
	late_mean /= float(max(1, late_times.size()))
	var gen_end: Dictionary = cm.get_generation_stats()
	var gens := int(gen_end.get("generations", 0)) - int(gen_start.get("generations", 0))
	print("PROBE latch start=%.2fs flight=%.2fs frames=%d mean=%.2fms p99=%.2fms worst=%.2fms slow=%d on=%.2fs end=%.2fs gens=%d late_frames=%d late_mean=%.2fms late_p99=%.2fms late_worst=%.2fms late_slow=%d budget=%d->%d backlog=%d reasons=%d visible=%d visible_tiers=%d/%d/%d visible_bands=%d/%d/%d/%d/%d/%d far_geometry=%d/%d/%d/%d/%d/%d/%d/%d/%d far_visible=%d/%d/%d far_screen=%d/%d/%.4f" % [
		started_at, seconds, frame_times.size(), mean, _p99(frame_times), worst, slow,
		on_s, end_s, gens, late_times.size(), late_mean, _p99(late_times),
		late_worst, late_slow, budget_start, budget_end, backlog_end, reasons_end, visible_end,
		tiers[0], tiers[1], tiers[2], bands[0], bands[1], bands[2], bands[3], bands[4], bands[5],
		int(geometry.get("total", 0)), int(geometry.get("no_chunk", 0)),
		int(geometry.get("empty", 0)), int(geometry.get("no_geometry", 0)),
		int(geometry.get("solid", 0)), int(geometry.get("enclosed", 0)),
		int(geometry.get("hidden", 0)), int(geometry.get("drawn_chunk", 0)),
		int(geometry.get("drawn_region", 0)), int(holes.get("total", 0)),
		int(holes.get("visible", 0)), int(holes.get("occluded", 0)),
		int(holes.get("screen_visible", 0)), int(holes.get("screen_pixels", 0)),
		float(holes.get("screen_coverage", 0.0))])
	print("PROBE %s" % ["OK" if ok else "FAILED"])
