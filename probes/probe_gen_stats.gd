extends SceneTree
## Headless probe: what the generation sweep spends its per-frame check budget on.
##
## Prints the engine's own counters (WorldUpdater::GenerationStats via
## chunk_manager.get_generation_stats) in the two situations that differ most:
##   boot   — the candidate list is huge and almost everything is rejected,
##   flying — the list is mostly already loaded and the cursor resets on every
##            chunk crossing, so the same budget is spent walking known chunks.
##
## It decides nothing; it prints numbers to be read.
##
##   .freebuff/run_probe.sh .freebuff/probe_gen_stats.gd 400
##
## Environment switches for the squish A/B (worldgen/terrain_squish.hpp):
##   RD=<n>     render distance to boot at (default: whatever the game's
##              settings hold). The win is radius-dependent, so measure more
##              than one.
##   SQUISH=1   compress the terrain into one chunk slice before booting, so the
##              vertical half of generation is gone and the same flight can be
##              diffed against the normal world.
##   VEG=0      vegetation off. Use it for BOTH sides of the comparison: trees
##              are vertical content the squish deliberately removes.
## Run the pair as:  VEG=0 ...     and   SQUISH=1 VEG=0 ...

const CHUNKS_TO_FLY  := 24
const BLOCKS_PER_STEP := 2.0
const MAX_FILL_FRAMES := 12000
const QUIET_FRAMES    := 30
const SNAPSHOT_EVERY  := 20

var main: Node
var cm: Node
var player: Node
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _surface_y(x: int, z: int, hint: int) -> int:
	# Start from the previous column's height and walk down to the ground: the
	# terrain under a 2-block step is close to the previous one, so scanning from
	# the top every step would spend most of the probe in get_block.
	var y: int = min(320, hint + 24)
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return y + 1
		y -= 1
	return 0

# Columns along the flight that had NO ground under them. The sweep list is a
# per-column slice RANGE now, so a range that came out one slice short would stop
# generating the chunk that holds the surface — terrain missing along the path
# while everything still renders and reports as fine. That is the failure this
# counter exists to catch: it must be 0.
var holes := 0

func _print_stats(phase: String) -> void:
	var g: Dictionary = cm.get_generation_stats()
	# The runner filters stdout down to lines mentioning PROBE or "probe:", so the
	# counters must wear one of those prefixes to survive the pipe.
	print("probe: STATS phase=%s rd=%d frames=%d list=%d cols=%d resets=%d sweeps=%d checks=%d gens=%d band=%d refused=%d r_loaded=%d r_above=%d r_below=%d r_oob=%d fchecks=%d fvis=%d fband=%d fgens=%d urgent=%d urgent_gen=%d ms_total=%.1f ms_avg=%.4f ms_max=%.3f rb=%d rb_total=%.1f rb_max=%.1f bands=%d band_ms=%.1f band_max=%.1f win_frames=%d win_checks=%d win_gens=%d" % [
		phase,
		int(g.get("render_distance", 0)),
		int(g.get("frames", 0)),
		int(g.get("candidate_offsets", 0)),
		int(g.get("candidate_columns", 0)),
		int(g.get("cursor_resets", 0)),
		int(g.get("sweeps_completed", 0)),
		int(g.get("checks", 0)),
		int(g.get("generations", 0)),
		int(g.get("band_pass", 0)),
		int(g.get("generate_refused", 0)),
		int(g.get("reject_loaded", 0)),
		int(g.get("reject_above", 0)),
		int(g.get("reject_below", 0)),
		int(g.get("reject_oob", 0)),
		int(g.get("frustum_checks", 0)),
		int(g.get("frustum_visible", 0)),
		int(g.get("frustum_band_pass", 0)),
		int(g.get("frustum_generations", 0)),
		int(g.get("urgent_requested", 0)),
		int(g.get("urgent_generated", 0)),
		float(g.get("total_ms", 0.0)),
		float(g.get("avg_ms", 0.0)),
		float(g.get("max_ms", 0.0)),
		int(g.get("rebuilds", 0)),
		float(g.get("total_rebuild_ms", 0.0)),
		float(g.get("max_rebuild_ms", 0.0)),
		int(g.get("band_reads", 0)),
		float(g.get("total_band_ms", 0.0)),
		float(g.get("max_band_ms", 0.0)),
		int(g.get("window_frames", 0)),
		int(g.get("window_checks", 0)),
		int(g.get("window_generations", 0)),
	])

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return

	# Render distance, before the fill below. It cannot be set and trusted
	# immediately: the settings menu applies the saved config during its first
	# frames, so an early set is silently overwritten (the stats would then say
	# rd=32 in a run that asked for 64). Wait for that to land, apply the radius,
	# wipe, and measure the fill from there.
	var rd_env := OS.get_environment("RD")
	if rd_env != "":
		for i in range(5):
			await process_frame
		cm.set_render_distance(int(rd_env))
		cm.clear_editor_chunks()
		print("PROBE render distance %d" % cm.get_render_distance())

	# The squish A/B: set the toggle and regenerate before anything is measured,
	# so the boot below is already the squished world's boot.
	var squish_env := OS.get_environment("SQUISH")
	if squish_env != "":
		cm.set_squish_enabled(squish_env == "1" or squish_env.to_lower() == "on")
		cm.clear_editor_chunks()
		print("PROBE squish=%s slice=%d" % [str(cm.get_squish_enabled()), int(cm.get_squish_slice())])
	if OS.get_environment("VEG") == "0":
		cm.set_vegetation_enabled(false)
		print("PROBE vegetation disabled")

	# The frustum pass is one of the things being measured, and it is only fed when
	# the camera is current. A windowed run gets that for free (Godot assigns the
	# only Camera3D), a headless one does not, so say so explicitly.
	var cam: Camera3D = player.get_node_or_null("Camera3D")
	if cam != null:
		cam.make_current()
		print("PROBE camera current=%s pos=%s" % [str(root.get_camera_3d() == cam), str(cam.global_position)])
	else:
		print("PROBE no Camera3D under the player")

	# --- Boot: let the initial load run itself out -------------------------
	# "Run itself out" is measured, not assumed: the sweep's own completed pass
	# (its walk found nothing left to do) plus a quiet window on generations,
	# installs and rebuilds, which trail the sweep that won them. The old fixed
	# 300-frame window compared two worlds over equal FRAMES, not equal work —
	# the normal world is still mid-fill at the frame the squished one is done,
	# and that gap is exactly what the FILL line below exists to price.
	# Clean slate for the fill: the frames above (settings load, RD and toggle
	# application) generated a little terrain at whatever radius was live first,
	# and leaving that in the counters would mix two radii into one number.
	cm.reset_generation_stats()
	var boot_started := Time.get_ticks_msec()
	var waited := 0
	while waited < 60:
		await process_frame
		waited += 1
		if player.is_on_floor():
			break
	var sweep_frames := 0
	var sweep_ms := 0
	var work_frames := 0
	var work_ms := 0
	var last_work := -1
	var settled := false
	while waited < MAX_FILL_FRAMES:
		await process_frame
		waited += 1
		var stats: Dictionary = cm.get_generation_stats()
		var work := int(stats.get("generations", 0)) + int(stats.get("installs_total", 0)) + int(stats.get("rebuilds", 0))
		if work != last_work:
			last_work = work
			work_frames = waited
			work_ms = Time.get_ticks_msec() - boot_started
		if sweep_frames == 0 and int(stats.get("sweeps_completed", 0)) > 0:
			sweep_frames = waited
			sweep_ms = Time.get_ticks_msec() - boot_started
		if sweep_frames > 0 and waited - work_frames >= QUIET_FRAMES:
			settled = true
			break
	var boot_frames := waited
	_print_stats("boot")
	var boot_ms := Time.get_ticks_msec() - boot_started
	# `work_ms` is when the last chunk install or mesh rebuild happened — the
	# moment terrain stopped appearing, which is the fill a player watching the
	# disc sees. `sweep_ms` is when the walk itself reported the list spent.
	# Frames are printed beside ms because the probe's frames are CPU-bound
	# (~10 ms) while the game's are vsync-bound, so FRAMES are what translate
	# into the wall clock the user reports.
	print("probe: FILL rd=%d settled=%s fill_frames=%d fill_ms=%d sweep_frames=%d sweep_ms=%d work_frames=%d work_ms=%d" % [
		int(cm.get_render_distance()), str(settled), boot_frames, boot_ms,
		sweep_frames, sweep_ms, work_frames, work_ms])

	# --- Flying: reset, then walk the player along the terrain --------------
	cm.reset_generation_stats()
	# Drain the engine's per-interval timers, so the report printed after the
	# flight describes the flight alone rather than the boot too.
	cm.get_performance_report()
	var fly_started := Time.get_ticks_msec()
	var start: Vector3 = player.global_position
	var y := _surface_y(int(floor(start.x)), int(floor(start.z)), int(start.y))
	# teleport_to, not global_position: the controller owns the player's position
	# (it re-derives it from its own simulation every frame), so writing the node
	# directly is overwritten and the streaming origin never moves.
	player.teleport_to(Vector3(start.x, float(y) + 1.0, start.z))
	for i in range(30):
		await process_frame

	var flown := 0.0
	var target := float(CHUNKS_TO_FLY) * 32.0
	var step := 0
	while flown < target and step < 20000:
		var pos: Vector3 = player.global_position
		var nx := pos.x + BLOCKS_PER_STEP
		var nz := pos.z
		var ny := _surface_y(int(floor(nx)), int(floor(nz)), int(pos.y))
		if ny <= 0:
			holes += 1
			print("probe: HOLE at x=%d z=%d (no ground found)" % [int(nx), int(nz)])
		player.teleport_to(Vector3(nx, float(ny) + 1.0, float(nz)))
		await process_frame
		flown += BLOCKS_PER_STEP
		step += 1
		if step % SNAPSHOT_EVERY == 0:
			_print_stats("flying_%d" % int(flown / 32.0))
			var node_pos: Vector3 = player.global_position
			print("probe: POS step=%d node=%.1f,%.1f,%.1f target=%.1f,%.1f,%.1f" % [step, node_pos.x, node_pos.y, node_pos.z, nx, float(ny) + 1.0, nz])
	_print_stats("flying")

	var g: Dictionary = cm.get_generation_stats()
	# The throughput number the squish A/B is actually about: chunks generated per
	# second of wall clock for the identical flight, and the wall time each phase
	# took. Boot includes the initial disc; the flight is the 24-chunk walk, whose
	# generation count is from the reset above.
	var fly_ms := Time.get_ticks_msec() - fly_started
	var fly_gens := int(g.get("generations", 0))
	print("probe: TIMING squish=%s boot_ms=%d boot_frames=%d fly_ms=%d fly_gens=%d fly_gen_per_s=%.1f fly_checks=%d" % [
		str(cm.get_squish_enabled()), boot_ms, boot_frames, fly_ms, fly_gens,
		(float(fly_gens) * 1000.0 / float(max(1, fly_ms))), int(g.get("checks", 0))])
	# The per-unit cost behind that throughput: a squished column is ONE chunk
	# (full generation, and a mesh that has geometry), where a normal column's band
	# is mostly fast-path fills and sky with no mesh at all. The counts say which
	# kind of work the flight was made of; the averages say what each cost.
	for line in cm.get_performance_report().split("\n"):
		if line.contains("generate_chunk") or line.contains("build_mesh"):
			print("probe: PERF " + line.strip_edges())

	# --- Band shape and what the pad bought ---------------------------------
	# The counters this probe was extended for. Printed as one line each so a run
	# can be diffed; the FAILs below only fire on internally inconsistent data,
	# not on "the pad is expensive" — that is a number to read, not a verdict.
	var hist: PackedInt32Array = g.get("band_size_hist", PackedInt32Array())
	var hist_sum := 0
	for i in range(hist.size()):
		hist_sum += int(hist[i])
	var fill_cols := int(g.get("fill_columns_read", 0))
	var fills := int(g.get("fill_band_slices", 0))
	var hist_parts := PackedStringArray()
	for i in range(hist.size()):
		hist_parts.append(str(hist[i]))
	print("probe: BANDS nonfill_cols=%d fill_cols=%d hist=[%s] max_slices=%d mean_all=%.2f fill_mean=%.2f" % [
		hist_sum, fill_cols, ",".join(hist_parts),
		int(g.get("band_max_slices", 0)),
		# candidate_offsets/candidate_columns is the mean over ALL columns, fill included;
		# the histogram above is where the non-fill shape is read, not this.
		(float(int(g.get("candidate_offsets", 0))) / float(max(1, int(g.get("candidate_columns", 0))))),
		(float(fills) / float(max(1, fill_cols)))])

	var inst := int(g.get("installs_total", 0))
	var i_above := int(g.get("installs_above_top", 0))
	var i_below := int(g.get("installs_below_land", 0))
	var i_in := int(g.get("installs_in_range", 0))
	var i_unknown := int(g.get("installs_bounds_unknown", 0))
	print("probe: INSTALLS total=%d empty=%d above=%d above_empty=%d below=%d below_empty=%d inrange=%d inrange_empty=%d unknown=%d" % [
		inst, int(g.get("installs_empty", 0)),
		i_above, int(g.get("installs_above_top_empty", 0)),
		i_below, int(g.get("installs_below_land_empty", 0)),
		i_in, int(g.get("installs_in_range_empty", 0)), i_unknown])

	if holes > 0:
		_fail("%d columns along the flight had no ground: the sweep list lost chunks" % holes)
	if hist_sum > 0 and hist_sum != int(g.get("band_reads", 0)) - fill_cols:
		_fail("band histogram (%d) does not account for band_reads-fill (%d)" % [hist_sum, int(g.get("band_reads", 0)) - fill_cols])
	if inst > 0 and i_above + i_below + i_in + i_unknown != inst:
		_fail("install classification (%d) does not sum to installs_total (%d)" % [i_above + i_below + i_in + i_unknown, inst])
	if int(g.get("installs_above_top_empty", 0)) > i_above or int(g.get("installs_below_land_empty", 0)) > i_below or int(g.get("installs_in_range_empty", 0)) > i_in:
		_fail("an *_empty count exceeds its class total")
	if int(g.get("candidate_columns", 0)) <= 0:
		_fail("the sweep list is empty - nothing was ever enumerated")
	if int(g.get("checks", 0)) <= 0:
		_fail("no sweep checks recorded at all - instrumentation is not firing")
	if int(g.get("window_checks", 0)) <= 0:
		_fail("no window checks recorded")
	print("PROBE %s (boot_frames=%d flown_blocks=%d)" % ["OK" if ok else "FAILED", boot_frames, int(flown)])
	quit(0 if ok else 1)
