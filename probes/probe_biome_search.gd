extends SceneTree
## A real /locatebiome miss, measured in the running game.
##
## Boots Main.tscn, scans outward from the spawn for a column whose whole
## search square contains none of the three biomes, teleports there and waits
## for streaming to settle, then drives the actual chat command. It reports the
## radius the engine advertises, the radius chat asked for, the wall clock from
## command to answer, and frame times before / during / after the walk, so both
## halves of "the message must not promise a radius the search cannot reach"
## are read in one run. A miss must end in "Could not find ...", not a teleport
## link; a hit means the scan's 64-block sampling missed a narrow patch.
##
##   bash probes/run_probe_shot.sh probes/probe_biome_search.gd 180

const RADIUS_FALLBACK := 864
# The search's own 16-block ring grid: a 64-block scan misses ponds that the
# search finds (the first run of this probe did exactly that). The 256-block
# inner square is a cheap filter before the full 864-block one.
const SEARCH_STEP := 16
const INNER_RADIUS := 256
const BIOMES := ["ocean", "hills", "plains"]
const BASELINE_SEC := 1.0
const SEARCH_TIMEOUT_SEC := 8.0
const ELAPSED_MAX_SEC := 4.5      # MAX_SECONDS (4) plus one probe and one frame
const FRAME_MEAN_SLACK_MS := 1.5
const FRAME_P99_SLACK_MS := 5.0

var main: Node
var cm: Node
var chat: Node
var player: Node3D
var radius := RADIUS_FALLBACK
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

# Does the search's own ring grid find `target` in a square of half-width
# `scan_radius`? A miss here is a miss the walk will make too, because both
# sample (cx + i*16, cz + j*16) from the same centre and the same generator.
func _square_has(cx: int, cz: int, target: String, scan_radius: int) -> bool:
	var d := -scan_radius
	while d <= scan_radius:
		var e := -scan_radius
		while e <= scan_radius:
			if String(cm.call("get_biome_at", cx + e, cz + d)) == target:
				return true
			e += SEARCH_STEP
		d += SEARCH_STEP
	return false

# Nearest centre to the spawn with a target biome absent from the whole search
# square. The small square rejects most candidates cheaply; the full one is the
# proof that a real /locatebiome from there has to end in a miss.
func _find_miss(spawn: Vector3) -> Dictionary:
	var sx := int(spawn.x)
	var sz := int(spawn.z)
	for dist in range(0, 16385, 512):
		var offsets := [[0, 0], [dist, 0], [-dist, 0], [0, dist], [0, -dist]]
		for off in offsets:
			var cx := sx + int(off[0])
			var cz := sz + int(off[1])
			var here := String(cm.call("get_biome_at", cx, cz))
			for target in BIOMES:
				if target == here:
					continue
				if _square_has(cx, cz, target, INNER_RADIUS):
					continue
				if _square_has(cx, cz, target, radius):
					continue
				return {"x": cx, "z": cz, "target": target}
	return {}

func _teleport_to_surface(x: int, z: int) -> void:
	# The column is not resident yet, so a top-down scan here finds only bedrock
	# and drops the player into it. Go high, let the chunk arrive, then stand on
	# its real surface.
	player.call("teleport_to", Vector3(x, 400, z))
	await process_frame
	await process_frame
	var y := 400
	while y > 0:
		if int(cm.call("get_block", x, y, z)) != 0:
			break
		y -= 1
	player.call("teleport_to", Vector3(x, y + 1, z))

func _wait_for_quiet() -> void:
	var deadline := Time.get_ticks_usec() + 30_000_000
	var quiet := 0
	while Time.get_ticks_usec() < deadline:
		await process_frame
		if bool(cm.call("is_fill_settled")) and not bool(cm.call("has_pending_mesh_work")):
			quiet += 1
			if quiet >= 60:
				return
		else:
			quiet = 0
	print("PROBE biome warning: streaming did not settle in 30 s; frame windows include fill")

func _answer() -> String:
	if chat == null or chat.messages.is_empty():
		return ""
	var text := String(chat.messages[chat.messages.size() - 1].get("text", ""))
	if text.begins_with("Could not find") or text.begins_with("The nearest"):
		return text
	return ""

func _measure(secs: float, stop_on_answer: bool) -> Dictionary:
	var start := Time.get_ticks_usec()
	var samples: Array[float] = []
	var worst := 0.0
	var worst_at := 0.0
	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		samples.append(ms)
		if ms > worst:
			worst = ms
			worst_at = float(Time.get_ticks_usec() - start) / 1_000_000.0
		if stop_on_answer and not _answer().is_empty():
			break
		if float(Time.get_ticks_usec() - start) / 1_000_000.0 >= secs:
			break
	samples.sort()
	var total := 0.0
	for s in samples:
		total += s
	var n := maxi(samples.size(), 1)
	return {
		"frames": samples.size(),
		"mean": total / float(n),
		"p95": samples[clampi(int(float(n) * 0.95), 0, n - 1)],
		"p99": samples[clampi(int(float(n) * 0.99), 0, n - 1)],
		"worst": worst,
		"worst_at": worst_at,
	}

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
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
	# The player's own _process computes the spawn at (0,0) once its chunks are
	# resident and resets global_position then. Let it finish before teleporting.
	for i in range(30):
		await process_frame
	cm = main.get_node_or_null("ChunkManager")
	chat = main.get_node_or_null("HUD/Chat")
	player = main.get_node_or_null("Player")
	if cm == null or chat == null or player == null:
		_fail("ChunkManager / HUD/Chat / Player missing")
		quit(1)
		return
	if cm.has_method("get_biome_search_radius"):
		radius = int(cm.call("get_biome_search_radius"))
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	cm.set("render_distance", 8)  # a small world for a quiet frame-time floor

	var miss := _find_miss(player.global_position)
	if miss.is_empty():
		_fail("no column with a whole biome absent within %d blocks" % radius)
		quit(1)
		return
	await _teleport_to_surface(int(miss["x"]), int(miss["z"]))
	await _wait_for_quiet()
	print("PROBE biome engine_radius=%d target=%s centre=(%d,%d)" % [
		radius, miss["target"], miss["x"], miss["z"]])

	print("PROBE biome player after teleport=%s" % player.global_position)
	chat.messages.clear()
	var before: Dictionary = await _measure(BASELINE_SEC, false)
	print("PROBE biome player at command=%s (chat sees %s)" % [
		player.global_position, chat.get("player_controller").global_position])
	var issued := Time.get_ticks_usec()
	chat.call("_run_command", "/locatebiome %s" % miss["target"])
	var requested := int(chat.get("_search_radius"))
	var during: Dictionary = await _measure(SEARCH_TIMEOUT_SEC, true)
	var elapsed := float(Time.get_ticks_usec() - issued) / 1_000_000.0
	var answer := _answer()
	var after: Dictionary = await _measure(BASELINE_SEC, false)

	print("PROBE biome radius engine=%d chat=%d agree=%d" % [
		radius, requested, 1 if requested == radius else 0])
	print("PROBE biome baseline frames=%d mean=%.3fms p95=%.3fms p99=%.3fms worst=%.3fms" % [
		before["frames"], before["mean"], before["p95"], before["p99"], before["worst"]])
	print("PROBE biome search frames=%d mean=%.3fms p95=%.3fms p99=%.3fms worst=%.3fms@%.3fs elapsed=%.3fs" % [
		during["frames"], during["mean"], during["p95"], during["p99"],
		during["worst"], during["worst_at"], elapsed])
	print("PROBE biome after   frames=%d mean=%.3fms p95=%.3fms p99=%.3fms worst=%.3fms" % [
		after["frames"], after["mean"], after["p95"], after["p99"], after["worst"]])
	print("PROBE biome answer=\"%s\"" % answer)

	var elapsed_ok := elapsed <= ELAPSED_MAX_SEC
	# Mean and p99 are the stable signal: single worst frames on this machine
	# come in 13-17 ms spikes with no search running, so they are reported but
	# not used as the gate.
	var frames_ok := float(during["mean"]) <= float(before["mean"]) + FRAME_MEAN_SLACK_MS \
			and float(during["p99"]) <= maxf(float(before["p99"]) + FRAME_P99_SLACK_MS, 3.0)
	var miss_ok := answer.begins_with("Could not find")
	if answer.begins_with("Could not find %s nearby - the search gave up after " % miss["target"]):
		print("PROBE biome note: the walk hit the time cap at or before the advertised %d blocks (expected for a full-radius miss)" % radius)
	elif not answer.begins_with("Could not find %s within %d blocks" % [miss["target"], radius]):
		print("PROBE biome note: answer was not a miss message (a hit or a different format)")
	if requested != radius:
		_fail("chat asked for %d, the engine advertises %d" % [requested, radius])
	if not miss_ok:
		_fail("the search found a %s column inside the scanned miss square" % miss["target"])
	if not elapsed_ok:
		_fail("search took %.2fs, over the %.1fs bound" % [elapsed, ELAPSED_MAX_SEC])
	if not frames_ok:
		_fail("frame times moved: before %.3f/%.3fms, during %.3f/%.3fms" % [
			before["mean"], before["worst"], during["mean"], during["worst"]])
	print("PROBE biome verdict elapsed_ok=%d frames_ok=%d radius_ok=%d miss_ok=%d" % [
		1 if elapsed_ok else 0, 1 if frames_ok else 0,
		1 if requested == radius else 0, 1 if miss_ok else 0])
	quit(0 if ok else 1)
