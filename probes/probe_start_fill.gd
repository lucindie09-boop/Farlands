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
	print("PROBE load sweep time total=%.1fms avg=%.3fms max=%.2fms | load=%.2fs" % [
		float(g.get("total_ms", 0.0)), float(g.get("avg_ms", 0.0)),
		float(g.get("max_ms", 0.0)), load_s])

func _print_fill_phases(wall_s: float, load_s: float, light_report: String) -> void:
	print("PROBE fill phases wall=%.2fs load=%.2fs tail=%.2fs" % [wall_s, load_s, wall_s - load_s])
	for label in ["generate_chunk", "process_completed_chunks", "build_mesh",
			"process_completed_meshes", "dirty_mesh_queue", "mesh_queue_chunks",
			"mesh_queue_far", "mesh_pop_callback", "mesh_pop_detail", "mesh_pop_far",
			"mesh_pop_lookup", "mesh_pop_construct", "mesh_pop_early", "mesh_pop_buried",
			"mesh_pop_requeue",
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
	var load_frame_sum := 0.0
	var load_frame_count := 0
	var load_worst_ms := 0.0
	var load_slow_frames := 0
	print("PROBE fill start rd=%d" % rd)

	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var frame_ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		if skip_next_frame:
			# The report call runs between frames; its cost lands in the next
			# frame interval, so do not count that interval as engine work.
			skip_next_frame = false
		else:
			sampled_frames += 1
			worst_frame_ms = maxf(worst_frame_ms, frame_ms)
			if frame_ms > 33.4:
				slow_frames += 1
			if loaded_at < 0.0:
				load_frame_sum += frame_ms
				load_frame_count += 1
				load_worst_ms = maxf(load_worst_ms, frame_ms)
				if frame_ms > 33.4:
					load_slow_frames += 1
		var elapsed := float(Time.get_ticks_usec() - start) / 1_000_000.0
		if elapsed >= TIMEOUT_SECS:
			break
		if elapsed - last_sample < SAMPLE_SECS:
			continue
		last_sample = elapsed
		samples += 1

		last_report = cm.get_performance_report()
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
		var pending_mesh := bool(cm.call("has_pending_mesh_work"))
		var queue_depth := int(cm.call("get_pending_chunk_mesh_count"))
		var queue_builds := int(cm.call("get_mesh_queue_builds"))
		var queue_rejects := int(cm.call("get_mesh_queue_rejects"))

		# Settle on the counts alone: a fast idle frame rate (this probe can run at
		# 1,000 fps headless) makes the old "loaded changed" reset fire every
		# sample, because last_loaded only advanced once the latch was set.
		var load_quiet := loaded >= 500 and generating == 0 and completed == 0
		if not load_quiet:
			stable_since = elapsed
		if loaded_at < 0.0 and load_quiet and elapsed - stable_since >= LOADED_STABLE_SECS:
			loaded_at = elapsed
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
			last_loaded = loaded

		if loaded_at >= 0.0 and mesh_at < 0.0:
			tail_queue_sum += float(queue_depth)
			tail_queue_max = maxi(tail_queue_max, queue_depth)
			tail_queue_samples += 1
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

	print("PROBE fill loaded=%.2fs mesh=%.2fs far=%.2fs end=%.2fs samples=%d frames=%d worst=%.2fms slow=%d" % [
		loaded_at, mesh_at, far_at, final_elapsed, samples, sampled_frames, worst_frame_ms, slow_frames])
	print("PROBE fill frame avg=%.2fms load_avg=%.2fms load_max=%.2fms load_slow=%d/%d worst=%.2fms slow=%d/%d" % [
		final_elapsed * 1000.0 / float(max(1, sampled_frames)),
		load_frame_sum / float(max(1, load_frame_count)), load_worst_ms,
		load_slow_frames, load_frame_count, worst_frame_ms, slow_frames, sampled_frames])
	for label in ["Loaded chunks:", "Chunk meshes:", "Far cached:",
			"Region empty:", "Far uncovered:", "Unrendered:"]:
		var line := _report_line(last_report, label)
		if not line.is_empty():
			print("PROBE fill final " + line)
	var light_report: String = cm.get_light_profile_report()
	print("PROBE light profile: %s" % light_report)
	var gen_path_report: String = cm.get_generation_path_report()
	print("PROBE gen path: %s" % gen_path_report)
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
