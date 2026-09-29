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

const CHUNKS_TO_FLY  := 24
const BLOCKS_PER_STEP := 2.0
const MAX_BOOT_FRAMES := 1500
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
	var waited := 0
	while waited < MAX_BOOT_FRAMES:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	var boot_frames := waited
	# Give the sweep a further stretch of frames with the player parked, so the
	# initial pass is as far along as a still player lets it get.
	for i in range(240):
		await process_frame
		boot_frames += 1
	_print_stats("boot")

	# --- Flying: reset, then walk the player along the terrain --------------
	cm.reset_generation_stats()
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
	if holes > 0:
		_fail("%d columns along the flight had no ground: the sweep list lost chunks" % holes)
	if int(g.get("candidate_columns", 0)) <= 0:
		_fail("the sweep list is empty - nothing was ever enumerated")
	if int(g.get("checks", 0)) <= 0:
		_fail("no sweep checks recorded at all - instrumentation is not firing")
	if int(g.get("window_checks", 0)) <= 0:
		_fail("no window checks recorded")
	print("PROBE %s (boot_frames=%d flown_blocks=%d)" % ["OK" if ok else "FAILED", boot_frames, int(flown)])
	quit(0 if ok else 1)
