extends SceneTree
## Streaming benchmark: how fast does terrain become real?
##
## Every counter in /genstats is per-CHECK and cannot answer that. This measures
## two things a player would recognise, at a fixed 60 fps so the engine's time
## budgets behave as they do in the game:
##
##   A. cold start  — jump 8192 blocks into genuinely ungenerated terrain and time
##                    how long until the 3x3 columns around the landing spot have
##                    ground. That is "terrain appears after a teleport".
##   B. flight      — move at 100 blocks/s for 40 s across cold terrain and sample
##                    every 10 frames how many columns in a 5x5 square around the
##                    player still have no ground. That is "does streaming keep up",
##                    which is the number the user actually feels.
##
## Earlier attempts at this measured nothing, for two reasons worth not repeating:
## 128-block hops land inside the boot disc (RD 32 = 1024 blocks), so the region is
## already generated before the player arrives; and a player left to fall drags the
## streaming origin under the surface, where the surface chunks get evicted and the
## region can never be "ready". So hops here are far, and the player is pinned to
## the surface of whichever column they are standing in.
##
##   .freebuff/run_probe.sh .freebuff/probe_stream_bench.gd 600
##
## The harness restores the world afterwards, so generation at far locations leaves
## no litter — but the disk save must be identical between two compared runs.

const COLD_STARTS := 3
const COLD_STRIDE := 2048.0   # blocks between cold starts, past RD 32 = 1024
const COLD_MAX_FRAMES := 900
const COLD_RADIUS := 1        # 3x3 columns must have ground
const FLY_SPEED := 100.0      # blocks/s, the user's `/fly 100`
const FLY_SECONDS := 40.0
const STATIC_SECONDS := 10.0  # standing still: the frustum pass should cost ~nothing
const FLY_RADIUS := 2         # 5x5 columns sampled for holes
const SAMPLE_EVERY := 10      # frames between hole samples
const TARGET_FPS := 60

const RESCAN_EVERY := 5   # frames between re-scans of columns still without ground

var main: Node
var cm: Node
var player: Node
var ok := true
var worst_frame_ms := 0.0
var frame_no := 0

# Column -> the topmost solid cell found in it (x<<16 ^ z). One readiness check for
# a known column is a single lookup; a 384-step scan per column per frame would cost
# more than the generation being measured.
#
# Only FOUND columns are cached. An empty column must be re-scanned, or a column
# that is cold on the first sample reads as empty forever — which is exactly how the
# first version of this probe reported every cold start as "never settled" while
# generation was in fact filling the area. Re-scans are throttled because 384
# lookups per missing column would eat into the frame the benchmark is timing.
var solid := {}
var scanned_at := {}   # column -> frame it was last scanned (per-column, see below)

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	Engine.max_fps = TARGET_FPS
	_run()

func _key(x: int, z: int) -> int:
	return (x << 16) ^ (z & 0xFFFF)

# Topmost solid cell in a column, -1 when the column has no ground at all (which is
# what an ungenerated chunk looks like through get_block).
func _find_solid(x: int, z: int) -> int:
	var y := 383
	while y >= 0:
		if cm.get_block(x, y, z) != 0:
			return y
		y -= 1
	return -1

func _column_has_ground(x: int, z: int) -> bool:
	var k := _key(x, z)
	var y: int = solid.get(k, -1)
	if y >= 0 and cm.get_block(x, y, z) != 0:
		return true
	# Throttled PER COLUMN, not per frame: a frame-global throttle lets only the
	# first missing column of a sample be scanned, so every other column reports
	# the previous "empty" answer without looking — which is how an earlier
	# version of this probe claimed 25 of 25 columns were missing while flying.
	if frame_no - int(scanned_at.get(k, -1000000)) < RESCAN_EVERY:
		return y >= 0
	scanned_at[k] = frame_no
	y = _find_solid(x, z)
	if y >= 0:
		solid[k] = y
	else:
		solid.erase(k)   # stay uncached so it is re-tested once it generates
	return y >= 0

# Columns in the square of the given radius around a block position without ground.
func _holes(bx: float, bz: float, radius: int) -> int:
	var pcx := int(floor(bx / 32.0))
	var pcz := int(floor(bz / 32.0))
	var missing := 0
	for dx in range(-radius, radius + 1):
		for dz in range(-radius, radius + 1):
			if not _column_has_ground(pcx * 32 + 16 + dx * 32, pcz * 32 + 16 + dz * 32):
				missing += 1
	return missing

# Keep the player standing on the surface of the column they are in. Without this a
# teleported player falls, the streaming origin drops under the terrain, and the
# surface chunks get evicted — which makes "ready" unreachable for reasons that have
# nothing to do with generation speed.
func _pin_to_surface() -> void:
	var p: Vector3 = player.global_position
	var k := _key(int(floor(p.x)), int(floor(p.z)))
	if _column_has_ground(int(floor(p.x)), int(floor(p.z))) and solid.has(k):
		var y: int = solid[k]
		if absf(p.y - float(y)) > 1.5:
			player.teleport_to(Vector3(p.x, float(y) + 1.0, p.z))

func _frame() -> void:
	var before := Time.get_ticks_usec()
	await process_frame
	var ms := float(Time.get_ticks_usec() - before) / 1000.0
	if ms > worst_frame_ms:
		worst_frame_ms = ms
	frame_no += 1
	_pin_to_surface()

func _stats() -> Dictionary:
	return cm.get_generation_stats()

# Thousands separator, matching how /genstats prints.
func _fmt(n: int) -> String:
	var s := str(absi(n))
	var out := ""
	while s.length() > 3:
		out = "," + s.substr(s.length() - 3) + out
		s = s.substr(0, s.length() - 3)
	return ("-" if n < 0 else "") + s + out

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
	print("probe: fps=%d cold_starts=%d stride=%.0f fly=%.0f blocks/s for %.0f s" % [
		TARGET_FPS, COLD_STARTS, COLD_STRIDE, FLY_SPEED, FLY_SECONDS])

	# Boot: land, then let the spawn area settle so every run starts from a
	# comparable state (same save, same frame count).
	var waited := 0
	while waited < 900:
		await _frame()
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	for i in range(180):
		await _frame()
	var home: Vector3 = player.global_position
	print("probe: boot after %d frames, player=(%.0f,%.0f,%.0f), worst frame %.1f ms" % [
		waited + 180, home.x, home.y, home.z, worst_frame_ms])

	# --- A. cold start ------------------------------------------------------
	cm.reset_generation_stats()
	var cold_ms := 0.0
	for i in range(COLD_STARTS):
		var dir := 1.0 if i % 2 == 0 else -1.0
		var target_x := home.x + dir * COLD_STRIDE * float(i + 1)
		solid.clear()
		scanned_at.clear()
		player.teleport_to(Vector3(target_x, home.y, home.z))
		var begin := Time.get_ticks_usec()
		var frames := 0
		var missing := 999
		var first_frame_ms := 0.0
		while frames < COLD_MAX_FRAMES:
			var fbefore := Time.get_ticks_usec()
			await _frame()
			frames += 1
			if frames == 1:
				first_frame_ms = float(Time.get_ticks_usec() - fbefore) / 1000.0
			missing = _holes(target_x, home.z, COLD_RADIUS)
			if missing == 0:
				break
		var ms := float(Time.get_ticks_usec() - begin) / 1000.0
		cold_ms += ms
		var after: Dictionary = _stats()
		var live: Vector3 = player.global_position
		print("probe: cold %d at x=%.0f player=(%.0f,%.0f,%.0f) -> %s in %d frames / %.1f ms (first frame %.1f ms, holes left %d)" % [
			i, target_x, live.x, live.y, live.z, "ready" if missing == 0 else "NOT READY",
			frames, ms, first_frame_ms, missing])
		print("probe: cold %d stats: gens=%d refused=%d checks=%d cand=%d cols=%d frustum=%d/%d" % [
			i, int(after["generations"]), int(after["generate_refused"]), int(after["checks"]),
			int(after["candidate_offsets"]), int(after["candidate_columns"]),
			int(after["frustum_checks"]), int(after["frustum_visible"])])
		if missing != 0:
			_fail("cold start %d never settled" % i)

	# --- B. flight ---------------------------------------------------------
	# Back to a settled home first, so the flight starts from known ground and the
	# numbers describe streaming rather than the tail of the cold start.
	solid.clear()
	scanned_at.clear()
	player.teleport_to(home)
	for i in range(240):
		await _frame()
	cm.reset_generation_stats()
	var stats_begin: Dictionary = _stats()
	var fly_start := Time.get_ticks_usec()
	var samples := 0
	var holes_sum := 0
	var holes_max := 0
	var holes_nonzero := 0
	var frames := 0
	while true:
		var elapsed := float(Time.get_ticks_usec() - fly_start) / 1_000_000.0
		if elapsed >= FLY_SECONDS:
			break
		var x := home.x + FLY_SPEED * elapsed
		player.teleport_to(Vector3(x, player.global_position.y, home.z))
		await _frame()
		frames += 1
		if frames % SAMPLE_EVERY == 0:
			var h := _holes(x, home.z, FLY_RADIUS)
			holes_sum += h
			holes_max = maxi(holes_max, h)
			if h > 0:
				holes_nonzero += 1
			samples += 1
	var fly_ms := float(Time.get_ticks_usec() - fly_start) / 1000.0
	var stats_end: Dictionary = _stats()
	var travelled := FLY_SPEED * (fly_ms / 1000.0)
	var gens := int(stats_end["generations"]) - int(stats_begin["generations"])
	var checks := int(stats_end["checks"]) - int(stats_begin["checks"])
	var loaded := int(stats_end["reject_loaded"]) - int(stats_begin["reject_loaded"])
	var fly_s := fly_ms / 1000.0
	print("probe: flight %.0f blocks in %.1f ms over %d frames -> travel %.1f chunks/s, generated %.1f chunks/s, %d gens (%.0f%% of %d checks)" % [
		travelled, fly_ms, frames, travelled / 32.0 / fly_s, float(gens) / fly_s, gens,
		100.0 * float(gens) / maxf(1.0, float(checks)), checks])
	print("probe: flight holes: mean %.2f of 25 columns, max %d, %d/%d samples had a gap (%.0f%%)" % [
		float(holes_sum) / maxf(1.0, float(samples)), holes_max, holes_nonzero, samples,
		100.0 * float(holes_nonzero) / maxf(1.0, float(samples))])

	# --- C. standing still --------------------------------------------------
	# The camera feeds the frustum every frame, and the pass used to be re-armed by
	# that alone, so a settled world still paid its checks forever. Nothing here
	# moves, so this is the case where the view-change gate should cost ~zero.
	var still_begin: Dictionary = _stats()
	var still_start := Time.get_ticks_usec()
	var still_frames := 0
	while float(Time.get_ticks_usec() - still_start) / 1_000_000.0 < STATIC_SECONDS:
		await _frame()
		still_frames += 1
	var still_end: Dictionary = _stats()
	var still_ms := float(Time.get_ticks_usec() - still_start) / 1000.0
	print("probe: standing still %.1f s (%d frames): sweep %s checks, %s generated, %s frustum checks, %.1f ms" % [
		STATIC_SECONDS, still_frames, _fmt(int(still_end["checks"]) - int(still_begin["checks"])),
		_fmt(int(still_end["generations"]) - int(still_begin["generations"])),
		_fmt(int(still_end["frustum_checks"]) - int(still_begin["frustum_checks"])),
		float(still_end["total_ms"]) - float(still_begin["total_ms"])])

	var cold_mean := cold_ms / float(COLD_STARTS)
	print("probe: SUMMARY cold_start_mean=%.1f ms worst_frame=%.1f ms" % [cold_mean, worst_frame_ms])
	print("probe: SUMMARY sweep_max_frame=%.1f ms band_max_frame=%.1f ms (over %d columns, slowest column %.2f ms) rebuild_max=%.1f ms rebuilds=%d" % [
		float(stats_end["max_ms"]), float(stats_end["max_band_ms"]),
		int(stats_end["max_band_columns"]), float(stats_end["max_band_column_ms"]),
		float(stats_end["max_rebuild_ms"]), int(stats_end["rebuilds"])])
	# Where the worst band frame went, when it was not the budget's fault: the
	# budget is checked per column, so a frame can only overshoot by one column.
	print("probe: SUMMARY max_cold_bounds_ms=%.2f (worst single cold bounds read)" % [
		float(stats_end["max_cold_bounds_ms"])])
	# The stall split, when the worst band frame was one slow column: bounds vs
	# resident lookups vs the rest (which is descheduling, not code).
	print("probe: SUMMARY worst_column split: bounds=%.2f resident=%.2f whole=%.2f contains=%.2f" % [
		float(stats_end["max_band_bounds_ms"]), float(stats_end["max_band_resident_ms"]),
		float(stats_end["max_band_column_ms"]), float(stats_end["max_contains_ms"])])
	print("probe: SUMMARY fly_gen_chunks_per_s=%.1f mean_holes=%.2f smooth_samples=%.0f%% sweep_ms=%.1f" % [
		float(gens) / fly_s, float(holes_sum) / maxf(1.0, float(samples)),
		100.0 * (1.0 - float(holes_nonzero) / maxf(1.0, float(samples))),
		float(stats_end["total_ms"]) - float(stats_begin["total_ms"])])
	# Where the sweep's time actually went, so "checks fell but ms did not" is not a
	# mystery: the walk is per-CANDIDATE, the band frontier and the rebuild are
	# per-COLUMN and do not shrink when the walk gets cheaper.
	print("probe: SUMMARY sweep_split_ms: walk=%.1f bands=%.1f rebuilds=%.1f over %.0f s (band_reads=%d)" % [
		float(stats_end["total_ms"]) - float(stats_begin["total_ms"]) -
			(float(stats_end["total_band_ms"]) - float(stats_begin["total_band_ms"])),
		float(stats_end["total_band_ms"]) - float(stats_begin["total_band_ms"]),
		float(stats_end["total_rebuild_ms"]) - float(stats_begin["total_rebuild_ms"]), fly_s,
		int(stats_end["band_reads"]) - int(stats_begin["band_reads"])])
	# Whether the band frontier is claiming worker answers or still deriving them
	# here. `cold` is the number this whole change exists to drive to zero.
	print("probe: SUMMARY column_bounds: taken=%d cold=%d requested=%d published=%d stale=%d dropped=%d outstanding=%d" % [
		int(stats_end["prefetch_taken"]) - int(stats_begin["prefetch_taken"]),
		int(stats_end["cold_bounds"]) - int(stats_begin["cold_bounds"]),
		int(stats_end["prefetch_requested"]) - int(stats_begin["prefetch_requested"]),
		int(stats_end["prefetch_published"]) - int(stats_begin["prefetch_published"]),
		int(stats_end["prefetch_stale"]) - int(stats_begin["prefetch_stale"]),
		int(stats_end["prefetch_dropped"]) - int(stats_begin["prefetch_dropped"]),
		int(stats_end["prefetch_outstanding"])])
	print("probe: SUMMARY columns_built=%d columns_skipped=%d" % [
		int(stats_end["columns_built"]), int(stats_end["columns_skipped"])])
	print("probe: SUMMARY fly_frustum_checks=%d (of %d sweep checks), frustum_refused=%d walk_refused=%d" % [
		int(stats_end["frustum_checks"]) - int(stats_begin["frustum_checks"]), int(checks),
		int(stats_end["frustum_refused"]) - int(stats_begin["frustum_refused"]),
		int(stats_end["generate_refused"]) - int(stats_begin["generate_refused"])])
	# The chain queue: how much of the flight's generation came through it, and
	# how many of its offers were already satisfied when drained.
	print("probe: SUMMARY chain: offered=%d generated=%d skipped=%d refused=%d seeds=%d vertical_offered=%d vertical_generated=%d" % [
		int(stats_end["chain_offered"]) - int(stats_begin["chain_offered"]),
		int(stats_end["chain_generations"]) - int(stats_begin["chain_generations"]),
		int(stats_end["chain_skipped"]) - int(stats_begin["chain_skipped"]),
		int(stats_end["chain_refused"]) - int(stats_begin["chain_refused"]),
		int(stats_end["chain_seeds"]) - int(stats_begin["chain_seeds"]),
		int(stats_end["chain_vertical_offered"]) - int(stats_begin["chain_vertical_offered"]),
		int(stats_end["chain_vertical_generated"]) - int(stats_begin["chain_vertical_generated"])])
	# The counters this change is judged by: walk skips that cost no check, and the
	# locked refusals they replace.
	print("probe: SUMMARY in_flight_skipped walk=%d frustum=%d" % [
		int(stats_end["reject_inflight"]) - int(stats_begin["reject_inflight"]),
		int(stats_end["frustum_inflight"]) - int(stats_begin["frustum_inflight"])])
	print("PROBE %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)
