extends SceneTree
## Does the frame rate drop at a chunk border?
##
## Run this WINDOWED; `--headless` has no frame rate worth measuring:
##
##   probes/run_probe_shot.sh probes/probe_border_fps.gd 240
##
## The probe boots the real scene, waits for the world to go quiet, and records
## every frame's wall interval in two phases: a stationary BASELINE, then a
## straight +X traverse at a constant SPEED across BORDERS x-boundaries. The
## player is flown at just above the surface, with a periodic vertical pin so it
## keeps a ground-level view; pinned frames are tagged and excluded from the
## class statistics because the teleport is the probe's own work.
##
## The reading is WITHIN one run: frames near a border (<= NEAR_BLOCKS), the
## frame in the new chunk and the 1 s after it, against frames far from a border
## (>= INTERIOR_BLOCKS). Vsyc is off and the fps cap lifted so a hitch is not
## rounded up to 16.7 ms. No fps threshold is asserted; the machine owns that
## number. The comparison, the per-crossing table and the streaming counters in
## each 1 s window are what the theory has to explain.
##
## Switches: BORDERS=12 SPEED=5.6 (blocks/s, sprint speed)

const CHUNK := 32.0
const BASELINE_SECS := 4.0
const QUIET_SECS := 1.5
const QUIET_MAX_SECS := 30.0
const QUIET_SCAN := 8
const PIN_INTERVAL_SECS := 0.5
const PIN_ABOVE_SURFACE := 3.0
const PIN_TOLERANCE := 0.75
const NEAR_BLOCKS := 2.0
const CROSS_BLOCKS := 0.5
const INTERIOR_BLOCKS := 8.0
const WORST_FRAMES := 8

var main: Node
var cm: Node
var player: Node
var ok := true

var borders := 12
var speed := 5.6
var walk := false
var dump_ms := 10.0

# One entry per frame. Parallel arrays rather than dictionaries: the traversal
# runs at several hundred fps and allocating a dictionary per frame would
# measure the allocator.
var _ms: Array[float] = []
var _t: Array[float] = []        # seconds from the start of the traverse (baseline is negative)
var _x: Array[float] = []
var _near: Array[float] = []
var _phase: Array[int] = []      # 0 baseline, 1 traverse
var _pin: Array[bool] = []
var _prims: Array[float] = []
var _draws: Array[float] = []
var _gpu: Array[float] = []
var _rcpu: Array[float] = []
var _setup: Array[float] = []
var _objs: Array[float] = []

var _events: Array[Dictionary] = []
var _active: Dictionary = {}
var _pins := 0
var _render_state_printed := false
var _slow_dumps: Array[String] = []
var _slow_dump_pending := false
var _boot_region_empty_max := 0
var _boot_region_empty_samples := 0

var was_cap := 0
var was_vsync := 0
var was_cycle := false
var was_time := 0.5
var was_duration := 1.0
var was_fly := false

func _initialize() -> void:
	if DisplayServer.get_name() == "headless":
		print("PROBE FAIL: --headless selects the dummy renderer, which has no frame rate; run windowed:")
		print("PROBE FAIL: probes/run_probe_shot.sh probes/probe_border_fps.gd")
		quit(1)
		return
	borders = clampi(_env_int("BORDERS", borders), 1, 64)
	speed = clampf(_env_float("SPEED", speed), 1.0, 50.0)
	walk = OS.get_environment("WALK") == "1"
	dump_ms = clampf(_env_float("DUMP_MS", dump_ms), 1.0, 100.0)
	_run()

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else int(raw)

func _env_float(name: String, fallback: float) -> float:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else float(raw)

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _frame() -> void:
	await process_frame

func _seconds(s: float) -> void:
	var end := Time.get_ticks_usec() + int(s * 1_000_000.0)
	while Time.get_ticks_usec() < end:
		await process_frame

func _stream_counts() -> Vector2i:
	var s: Dictionary = cm.get_generation_stats()
	return Vector2i(int(s.get("generations", 0)), int(s.get("rebuilds", 0)))

func _surface_y(x: int, z: int, hint: int) -> int:
	var y: int = mini(383, hint + 24)
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return y
		y -= 1
	return 0

func _near_border(x: float) -> float:
	var local := x - floorf(x / CHUNK) * CHUNK
	return minf(local, CHUNK - local)

func _record(phase: int, t: float, pinned: bool) -> void:
	var a := Time.get_ticks_usec()
	await process_frame
	var now := Time.get_ticks_usec()
	var p: Vector3 = player.global_position
	_ms.append(float(now - a) / 1000.0)
	_t.append(t)
	_x.append(p.x)
	_near.append(_near_border(p.x))
	_phase.append(phase)
	_pin.append(pinned)
	_prims.append(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
	_draws.append(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
	var vp_rid: RID = player.get_viewport().get_viewport_rid()
	_gpu.append(RenderingServer.viewport_get_measured_render_time_gpu(vp_rid))
	_rcpu.append(RenderingServer.viewport_get_measured_render_time_cpu(vp_rid))
	_setup.append(RenderingServer.get_frame_setup_time_cpu())
	_objs.append(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME))
	if phase == 1 and not pinned and _ms[_ms.size() - 1] >= dump_ms and cm != null:
		if not _slow_dump_pending:
			_slow_dump_pending = true
			call_deferred("_capture_slow_dump", _ms[_ms.size() - 1], p.x)
	if not _render_state_printed and phase == 1 and _near[_near.size() - 1] <= NEAR_BLOCKS \
			and int(_draws[_draws.size() - 1]) > 300 and cm != null:
		_render_state_printed = true
		print("PROBE RENDERSTATE at x=%.1f draws=%d" % [p.x, int(_draws[_draws.size() - 1])])
		var report: String = cm.get_performance_report()
		for line in report.split("\n"):
			if line.contains("Visible instances") or line.contains("Chunk instances") \
					or line.contains("Region instances") or line.contains("Region members") \
					or line.contains("Visible chunk detail") or line.contains("Actual vs logical") \
					or line.contains("Region member detail") or line.contains("Far uncovered"):
				print("PROBE RENDERSTATE " + line.strip_edges())

func _wait_quiet() -> void:
	var last := _stream_counts()
	var quiet_since := -1
	var begin := Time.get_ticks_usec()
	var deadline := begin + int(QUIET_MAX_SECS * 1_000_000.0)
	var tick := 0
	var quiet := false
	while Time.get_ticks_usec() < deadline:
		await process_frame
		tick += 1
		if tick % QUIET_SCAN != 0:
			continue
		# Initial streaming is where a region published before its upload
		# completes used to hide its members: an active region with no surface
		# is the visual hole, so sample it until the world goes quiet.
		var boot_report: String = cm.get_performance_report()
		for boot_line in boot_report.split("\n"):
			if boot_line.contains("Region empty:"):
				var empty_count := int(boot_line.get_slice(":", 1).strip_edges())
				_boot_region_empty_max = maxi(_boot_region_empty_max, empty_count)
				_boot_region_empty_samples += 1
		var now := _stream_counts()
		if now != last:
			last = now
			quiet_since = -1
		elif quiet_since < 0:
			quiet_since = Time.get_ticks_usec()
		elif float(Time.get_ticks_usec() - quiet_since) / 1_000_000.0 >= QUIET_SECS:
			quiet = true
			break
	var after := _stream_counts()
	if quiet:
		print("PROBE quiet after %.1fs (generations=%d rebuilds=%d)" % [
			float(Time.get_ticks_usec() - begin) / 1_000_000.0, after.x, after.y])
	else:
		print("PROBE WARNING: still streaming after %.0fs (generations=%d rebuilds=%d); the baseline will show it" % [
			QUIET_MAX_SECS, after.x, after.y])

func _restore() -> void:
	Input.action_release("move_forward")
	Input.action_release("sprint")
	Input.action_release("jump")
	if player != null and is_instance_valid(player):
		player.set_fly_mode(was_fly)
	if cm != null and is_instance_valid(cm):
		cm.set("day_time", was_time)
		cm.set("day_duration", was_duration)
		cm.set("day_night_cycle_enabled", was_cycle)
	Engine.max_fps = was_cap
	DisplayServer.window_set_vsync_mode(was_vsync)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	if OS.get_environment("REAL_BOOT") == "1":
		# Reproduce the normal launch far enough to arm the autoloads. MenuLayer only
		# boots when current_scene is the scene, so a hand-instantiated fixture never
		# sees the title path the player actually gets. Hide the title and release the
		# gate by hand: the transition is presentation, not the state being measured.
		current_scene = main
		await process_frame
		await process_frame
		var menu := root.get_node_or_null("MenuLayer")
		print("PROBE real-boot current_scene=%s menu=%s" % [str(current_scene != null), str(menu != null)])
		if menu != null:
			var booted := bool(menu.get("_booted"))
			var title := menu.get("_title") as CanvasItem
			var curtain := menu.get("_curtain") as CanvasItem
			print("PROBE real-boot booted=%s open=%s title=%s" % [str(booted), str(menu.get("_open")), str(title != null)])
			if title != null:
				title.hide()
			if curtain != null:
				curtain.hide()
			if booted:
				menu.call("_release")
				menu.set_process(false)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if OS.get_environment("NO_FPS") == "1":
		var fps_node := main.get_node_or_null("HUD/FPSCounter")
		print("PROBE FPS node=%s process=%s visible=%s" % [str(fps_node != null), str(fps_node.is_processing() if fps_node != null else false), str(fps_node.visible if fps_node != null else false)])
		if fps_node != null:
			fps_node.set_process(false)
			fps_node.visible = false
			print("PROBE FPS after process=%s visible=%s" % [str(fps_node.is_processing()), str(fps_node.visible)])
	if OS.get_environment("NO_HUD") == "1":
		var hud_node := main.get_node_or_null("HUD")
		if hud_node != null:
			hud_node.queue_free()
	if OS.get_environment("HUD_NO_PROCESS") == "1":
		var hud_node := main.get_node_or_null("HUD")
		if hud_node != null:
			_disable_process_recursive(hud_node)
	if OS.get_environment("NO_WIDGETS") == "1":
		for path in ["HUD/Hotbar", "HUD/Healthbar", "HUD/Airbar", "HUD/Inventory",
				"HUD/CraftingTableMenu", "HUD/Chat", "HUD/Wand", "HUD/SettingsMenu",
				"HUD/Compass", "HUD/DeathScreen", "HUD/FPSCounter"]:
			var widget := main.get_node_or_null(path)
			if widget != null:
				widget.queue_free()
	var removed := OS.get_environment("HUD_REMOVE")
	if not removed.is_empty():
		for name in removed.split(","):
			var widget := main.get_node_or_null("HUD/" + name.strip_edges())
			if widget != null:
				widget.queue_free()
	if OS.get_environment("NO_SHADER") == "1":
		var shader_node := main.get_node_or_null("HUD/ShaderOverlay")
		if shader_node != null:
			shader_node.queue_free()
	if OS.get_environment("NO_GODRAYS") == "1":
		var godrays_node := main.get_node_or_null("HUD/GodRaysOverlay")
		if godrays_node != null:
			godrays_node.queue_free()
	if OS.get_environment("NO_HURT") == "1":
		var hurt_node := main.get_node_or_null("HUD/HurtOverlay")
		if hurt_node != null:
			hurt_node.queue_free()
	if OS.get_environment("NO_CROSSHAIR") == "1":
		var crosshair_node := main.get_node_or_null("HUD/Crosshair")
		if crosshair_node != null:
			crosshair_node.queue_free()
	if cm == null or player == null:
		_fail("ChunkManager / Player missing from main.tscn")
		quit(1)
		return

	was_cap = Engine.max_fps
	was_vsync = DisplayServer.window_get_vsync_mode()
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0

	var waited := 0
	while waited < 900:
		await _frame()
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	await _seconds(1.0)

	was_cycle = bool(cm.get("day_night_cycle_enabled"))
	was_time = float(cm.get("day_time"))
	was_duration = float(cm.get("day_duration"))
	cm.set("day_night_cycle_enabled", false)
	was_fly = bool(player.get_fly_mode())

	# The scene boots under the title layer; none of these overlays should own
	# movement while the measurement runs.
	player.set_inventory_open(false)
	player.set_table_menu_open(false)
	player.set_chat_open(false)
	player.set_settings_open(false)
	player.set_wand_menu_open(false)
	player.set_fly_mode(not walk)
	player.set("fly_speed", speed)

	# Start in the middle of a chunk in both axes, so z is at its maximum
	# distance from a border for the whole run and every boundary met is an x one.
	var p0: Vector3 = player.global_position
	var cx0 := floori(p0.x / CHUNK)
	var cz0 := floori(p0.z / CHUNK)
	var sx := (float(cx0) + 0.5) * CHUNK
	var sz := (float(cz0) + 0.5) * CHUNK
	var sy := _surface_y(int(floorf(sx)), int(floorf(sz)), int(p0.y))
	if sy <= 0:
		_fail("no ground at the start column x=%.0f z=%.0f" % [sx, sz])
		_restore()
		quit(1)
		return
	player.teleport_to(Vector3(sx, float(sy) + (1.0 if walk else PIN_ABOVE_SURFACE), sz))
	player.set_rotation(Vector3(0.0, -PI / 2.0, 0.0))   # forward = +X
	player.set_fly_mode(not walk)
	await _seconds(0.5)

	# The whole reading depends on synthetic input actually moving the player;
	# a UI flag or a menu swallowing it would otherwise look like a still world.
	Input.action_press("move_forward")
	await _seconds(0.6)
	Input.action_release("move_forward")
	var moved: float = player.global_position.x - sx
	player.teleport_to(Vector3(sx, float(sy) + PIN_ABOVE_SURFACE, sz))
	player.set_rotation(Vector3(0.0, -PI / 2.0, 0.0))
	if moved < speed * 0.3:
		_fail("the player moved %.2f blocks in 0.6 s under forward input; movement is blocked" % moved)
		_restore()
		quit(1)
		return
	print("PROBE setup borders=%d speed=%.1f blocks/s baseline=%.1fs window=%s vsync=%d cap=%d" % [
		borders, speed, BASELINE_SECS, str(root.size), DisplayServer.window_get_vsync_mode(), was_cap])

	await _wait_quiet()
	print("PROBE boot region-empty max=%d samples=%d" % [_boot_region_empty_max, _boot_region_empty_samples])
	if _boot_region_empty_max > 0:
		_fail("an active far region had no surface during initial loading (%d)" % _boot_region_empty_max)
	await _seconds(0.5)

	# --- baseline: same camera, same world, no movement ---------------------
	var base_start := Time.get_ticks_usec()
	while float(Time.get_ticks_usec() - base_start) / 1_000_000.0 < BASELINE_SECS:
		await _record(0, -float(Time.get_ticks_usec() - base_start) / 1_000_000.0, false)

	# --- traverse -----------------------------------------------------------
	# Reset the engine's phase maxima too, so the report printed after the
	# traverse describes the moving frames rather than the boot fill. The report
	# is taken outside a measured frame.
	cm.get_performance_report()
	cm.reset_generation_stats()
	var t0 := Time.get_ticks_usec()
	var last_cx := floori(player.global_position.x / CHUNK)
	var last_pin := Time.get_ticks_usec()
	var last_cross_t := -1000.0
	var last_jump := -1000.0
	var start_x: float = player.global_position.x
	var max_seconds := (float(borders) + 1.0) * CHUNK / speed + 30.0
	var done := false
	Input.action_press("move_forward")
	if walk:
		Input.action_press("sprint")

	while not done:
		var now_us := Time.get_ticks_usec()
		var t := float(now_us - t0) / 1_000_000.0
		if t > max_seconds:
			break

		# Keep a ground-level view without moving the camera every frame: a
		# vertical pin only when the terrain has carried it too far from the
		# surface since the last look. The scan happens outside the timed frame.
		var pin := false
		if walk and t - last_jump >= 0.8:
			last_jump = t
			Input.action_press("jump")
		elif walk and t - last_jump >= 0.95:
			Input.action_release("jump")
		if not walk and float(now_us - last_pin) / 1_000_000.0 >= PIN_INTERVAL_SECS:
			last_pin = now_us
			var live: Vector3 = player.global_position
			var surf := _surface_y(int(floorf(live.x)), int(floorf(live.z)), int(live.y))
			var want := float(surf) + PIN_ABOVE_SURFACE
			if surf > 0 and absf(live.y - want) > PIN_TOLERANCE:
				player.teleport_to(Vector3(live.x, want, live.z))
				pin = true
				_pins += 1

		await _record(1, t, pin)
		var now_x: float = player.global_position.x
		var cx := floori(now_x / CHUNK)
		if cx != last_cx:
			last_cx = cx
			last_cross_t = t
			_finish_active(t)
			var g := _stream_counts()
			var ev := {
				"index": _ms.size() - 1,
				"x": now_x,
				"t": t,
				"g0": g.x,
				"r0": g.y,
				"g1": g.x,
				"r1": g.y,
				"first_ms": _ms[_ms.size() - 1],
				"max_ms": _ms[_ms.size() - 1],
				"max_at": 0.0,
				"frames": 1,
				"final": false,
			}
			_events.append(ev)
			# Capture the crossing frame itself: the later 3 ms frames are not the
			# 7 ms first frame, and the difference is what the report is for.
			if cm != null and OS.get_environment("CROSS_DUMP") == "1":
				var cross_dump := "PROBE CROSSDUMP %.2fms x=%.1f\n" % [_ms[_ms.size() - 1], now_x]
				cross_dump += cm.get_slow_frame_report(1, 0.0)
				for cross_line in cross_dump.split("\n"):
					if not cross_line.strip_edges().is_empty():
						print("PROBE " + cross_line)
			_active = ev
		elif not _active.is_empty() and not bool(_active.get("final", false)):
			var age := t - float(_active["t"])
			var idx := _ms.size() - 1
			if not pin and _ms[idx] > float(_active["max_ms"]):
				_active["max_ms"] = _ms[idx]
				_active["max_at"] = age
			_active["frames"] = int(_active["frames"]) + 1
			if age >= 1.0:
				_finish_active(t)

		if _events.size() >= borders and t - last_cross_t >= 1.5:
			done = true

	Input.action_release("move_forward")
	Input.action_release("sprint")
	Input.action_release("jump")
	_finish_active(float(Time.get_ticks_usec() - t0) / 1_000_000.0)

	# The phase maxima since the baseline reset. Prefix every line so the probe
	# log stays greppable and a CI run can compare one phase at a time.
	for slow_dump in _slow_dumps:
		for slow_line in slow_dump.split("\n"):
			if not slow_line.strip_edges().is_empty():
				print("PROBE " + slow_line)

	var perf: String = cm.get_performance_report()
	for perf_line in perf.split("\n"):
		if not perf_line.strip_edges().is_empty():
			print("PROBE PERF " + perf_line)

	var end_x: float = player.global_position.x
	var elapsed := float(Time.get_ticks_usec() - t0) / 1_000_000.0
	var gstats: Dictionary = cm.get_generation_stats()
	print("PROBE sweep cost: update_generation max=%.2fms avg=%.2fms | list rebuild max=%.2fms | band max=%.2fms over %d columns, worst column %.2fms | cold bounds max=%.2fms | column lock max=%.2fms" % [
		float(gstats.get("max_ms", 0.0)), float(gstats.get("avg_ms", 0.0)),
		float(gstats.get("max_rebuild_ms", 0.0)), float(gstats.get("max_band_ms", 0.0)),
		int(gstats.get("max_band_columns", 0)), float(gstats.get("max_band_column_ms", 0.0)),
		float(gstats.get("max_cold_bounds_ms", 0.0)), float(gstats.get("max_contains_ms", 0.0))])
	print("PROBE traverse %.1fs, x %.1f -> %.1f (%.1f blocks at %.1f blocks/s), crossings=%d pins=%d" % [
		elapsed, start_x, end_x, end_x - start_x, (end_x - start_x) / maxf(elapsed, 0.001),
		_events.size(), _pins])
	if _events.size() < borders:
		_fail("only %d of %d chunk borders were crossed in %.0f s" % [_events.size(), borders, elapsed])

	_report()
	_restore()
	print("PROBE %s" % ("OK" if ok else "FAILED"))
	quit(0 if ok else 1)

func _disable_process_recursive(node: Node) -> void:
	node.set_process(false)
	node.set_physics_process(false)
	for child in node.get_children():
		_disable_process_recursive(child)


func _capture_slow_dump(frame_ms: float, world_x: float) -> void:
	_slow_dump_pending = false
	if cm == null:
		return
	var dump := "PROBE SLOWDUMP %.2fms x=%.1f\n" % [frame_ms, world_x]
	dump += cm.get_slow_frame_report(8, 0.0)
	_slow_dumps.append(dump)
	if _slow_dumps.size() > 12:
		_slow_dumps.pop_front()

func _finish_active(t: float) -> void:
	if _active.is_empty() or bool(_active.get("final", false)):
		return
	var g := _stream_counts()
	_active["g1"] = g.x
	_active["r1"] = g.y
	_active["final"] = true
	_active["window"] = t - float(_active["t"])
	_active = {}

func _stats(indices: Array, gate: float) -> Dictionary:
	if indices.is_empty():
		return {
			"frames": 0, "ms_med": 0.0, "ms_mean": 0.0, "ms_p95": 0.0, "ms_max": 0.0,
			"fps_med": 0.0, "fps_p5": 0.0, "fps_min": 0.0, "stutter": 0, "prims": 0, "draws": 0,
		}
	var vals: Array[float] = []
	var prims := 0.0
	var draws := 0.0
	for raw in indices:
		var i := int(raw)
		vals.append(_ms[i])
		prims += _prims[i]
		draws += _draws[i]
	vals.sort()
	var n := vals.size()
	var total := 0.0
	for v in vals:
		total += v
	var median := vals[int(n / 2)]
	var p95 := vals[clampi(int(floor(0.95 * float(n - 1))), 0, n - 1)]
	var stutter := 0
	for v in vals:
		if v > gate:
			stutter += 1
	return {
		"frames": n,
		"ms_med": median,
		"ms_mean": total / float(n),
		"ms_p95": p95,
		"ms_max": vals[n - 1],
		"fps_med": 1000.0 / maxf(median, 0.001),
		"fps_p5": 1000.0 / maxf(p95, 0.001),
		"fps_min": 1000.0 / maxf(vals[n - 1], 0.001),
		"stutter": stutter,
		"prims": int(prims / float(n)),
		"draws": int(draws / float(n)),
	}

func _line(label: String, w: Dictionary, gate: float) -> void:
	var frames := int(w["frames"])
	if frames == 0:
		print("PROBE %-9s frames=0" % label)
		return
	print("PROBE %-9s frames=%d (%5.1f%%) fps med=%.0f mean=%.0f p5=%.0f min=%.0f | frame med=%.2fms mean=%.2fms p95=%.2fms max=%.2fms | stutter=%d (%.1f%%) | prims=%d draws=%d" % [
		label, frames, 100.0 * float(frames) / maxf(1.0, _traverse_count()),
		float(w["fps_med"]), 1000.0 / maxf(float(w["ms_mean"]), 0.001),
		float(w["fps_p5"]), float(w["fps_min"]),
		float(w["ms_med"]), float(w["ms_mean"]), float(w["ms_p95"]), float(w["ms_max"]),
		int(w["stutter"]), 100.0 * float(w["stutter"]) / float(frames),
		int(w["prims"]), int(w["draws"])])

func _traverse_count() -> int:
	var n := 0
	for i in range(_phase.size()):
		if _phase[i] == 1 and not _pin[i]:
			n += 1
	return n

func _report() -> void:
	if _ms.is_empty():
		_fail("no frames were recorded")
		return

	var base_idx: Array = []
	var all_idx: Array = []
	for i in range(_ms.size()):
		if _phase[i] == 0:
			base_idx.append(i)
		elif not _pin[i]:
			all_idx.append(i)
	if all_idx.is_empty():
		_fail("the traverse recorded no unpinned frames")
		return

	# The run's own stutter gate: twice the median AND at least 4 ms above it,
	# so a quiet 1 ms frame is not a "stutter" just because it is next to 0.5 ms.
	var all_sorted: Array[float] = []
	for i in all_idx:
		all_sorted.append(_ms[int(i)])
	all_sorted.sort()
	var med := all_sorted[int(all_sorted.size() / 2)]
	var gate := maxf(2.0 * med, med + 4.0)
	var base_gate_sorted: Array[float] = []
	for i in base_idx:
		base_gate_sorted.append(_ms[int(i)])
	base_gate_sorted.sort()
	var base_gate := gate
	if not base_gate_sorted.is_empty():
		var bmed := base_gate_sorted[int(base_gate_sorted.size() / 2)]
		base_gate = maxf(2.0 * bmed, bmed + 4.0)

	_line("baseline", _stats(base_idx, base_gate), base_gate)
	_line("traverse", _stats(all_idx, gate), gate)

	var cross_idx: Array = []
	var near_idx: Array = []
	var interior_idx: Array = []
	for i in all_idx:
		var d := _near[int(i)]
		if d <= CROSS_BLOCKS:
			cross_idx.append(i)
		if d <= NEAR_BLOCKS:
			near_idx.append(i)
		if d >= INTERIOR_BLOCKS:
			interior_idx.append(i)
	var cross_stats := _stats(cross_idx, gate)
	var near_stats := _stats(near_idx, gate)
	var interior_stats := _stats(interior_idx, gate)
	_line("cross", cross_stats, gate)
	_line("near<=2", near_stats, gate)
	_line("interior", interior_stats, gate)
	print("PROBE penalty near-vs-interior: median %+.2fms (%.1f vs %.1f fps), p95 %+.2fms, stutter %d vs %d" % [
		float(near_stats["ms_med"]) - float(interior_stats["ms_med"]),
		float(near_stats["fps_med"]), float(interior_stats["fps_med"]),
		float(near_stats["ms_p95"]) - float(interior_stats["ms_p95"]),
		int(near_stats["stutter"]), int(interior_stats["stutter"])])

	# The 1 s either side of every crossing, pooled. A transient that starts at
	# the boundary shows up here even when the pooled "near" band is diluted by
	# frames that are merely close to it.
	var pre_idx: Array = []
	var post_idx: Array = []
	for ev in _events:
		var et := float(ev["t"])
		for i in range(_ms.size()):
			if _phase[i] != 1 or _pin[i]:
				continue
			var dt: float = _t[i] - et
			if dt >= -1.0 and dt < 0.0:
				pre_idx.append(i)
			elif dt >= 0.0 and dt <= 1.0:
				post_idx.append(i)
	var pre_stats := _stats(pre_idx, gate)
	var post_stats := _stats(post_idx, gate)
	_line("pre-1s", pre_stats, gate)
	_line("post-1s", post_stats, gate)
	print("PROBE crossing window: post-vs-pre median %+.2fms, p95 %+.2fms, max %.2f vs %.2f ms" % [
		float(post_stats["ms_med"]) - float(pre_stats["ms_med"]),
		float(post_stats["ms_p95"]) - float(pre_stats["ms_p95"]),
		float(post_stats["ms_max"]), float(pre_stats["ms_max"])])

	# Per crossing, so one bad boundary cannot hide in an aggregate.
	for k in range(_events.size()):
		var ev: Dictionary = _events[k]
		var idx := int(ev["index"])
		var t := float(ev["t"])
		var pre_max := 0.0
		var post_frames := 0
		var stutters := 0
		for i in range(_ms.size()):
			if _phase[i] != 1 or _pin[i]:
				continue
			var dt: float = _t[i] - t
			if dt >= -1.0 and dt < 0.0:
				pre_max = maxf(pre_max, _ms[i])
			elif dt >= 0.0 and dt <= 1.0:
				post_frames += 1
				if _ms[i] > gate:
					stutters += 1
		var window_label := "window=open"
		if bool(ev.get("final", false)):
			window_label = "window=%.1fs" % float(ev["window"])
		var gd := int(ev["g1"]) - int(ev["g0"])
		var rd := int(ev["r1"]) - int(ev["r0"])
		print("PROBE cross %02d x=%.1f chunk=%d t=%.2fs first=%.2fms max_1s=%.2fms at+%.2fs pre_max=%.2fms frames=%d stutters=%d gens=%+d rebuilds=%+d %s" % [
			k + 1, float(ev["x"]), floori(float(ev["x"]) / CHUNK), t, _ms[idx],
			float(ev["max_ms"]), float(ev["max_at"]), pre_max, post_frames, stutters,
			gd, rd, window_label])

	# The individual worst frames, each with its distance to the border: a spike
	# at 9.3 ms two blocks past a boundary is a different story from one in the
	# middle of a chunk.
	var ranked: Array = []
	for i in all_idx:
		ranked.append([_ms[int(i)], i])
	ranked.sort_custom(func(a, b): return float(a[0]) > float(b[0]))
	for k in range(mini(WORST_FRAMES, ranked.size())):
		var entry: Array = ranked[k]
		var i := int(entry[1])
		print("PROBE worst %d: %.2fms (%.0f fps) x=%.1f chunk=%d border_dist=%.1f prims=%d" % [
			k + 1, float(entry[0]), 1000.0 / maxf(float(entry[0]), 0.001), _x[i],
			floori(_x[i] / CHUNK), _near[i], int(_prims[i])])

	# Renderer counters are sampled at process_frame, so each sample describes
	# the frame whose interval `_ms` just recorded. Print the neighbouring GPU
	# samples too: the viewport measurement itself can lag by one more frame.
	var max_gpu := -1.0
	var max_gpu_i := 0
	var max_draws := -1.0
	var max_draws_i := 0
	for raw in all_idx:
		var i := int(raw)
		if _gpu[i] > max_gpu:
			max_gpu = _gpu[i]
			max_gpu_i = i
		if _draws[i] > max_draws:
			max_draws = _draws[i]
			max_draws_i = i
	print("PROBE render max gpu=%.3fms at x=%.1f border_dist=%.1f delta=%.2fms cpu=%.3fms setup=%.3fms draws=%d objs=%d prims=%d | draws max=%.0f at x=%.1f border_dist=%.1f delta=%.2fms" % [
		max_gpu, _x[max_gpu_i], _near[max_gpu_i], _ms[max_gpu_i], _rcpu[max_gpu_i],
		_setup[max_gpu_i], int(_draws[max_gpu_i]), int(_objs[max_gpu_i]), int(_prims[max_gpu_i]),
		max_draws, _x[max_draws_i], _near[max_draws_i], _ms[max_draws_i]])
	for k in range(mini(WORST_FRAMES, ranked.size())):
		var i := int(ranked[k][1])
		var ip := maxi(i - 1, 0)
		var inx := mini(i + 1, _ms.size() - 1)
		print("PROBE render %d: delta=%.2fms x=%.1f border_dist=%.1f gpu_prev=%.3f gpu=%.3f gpu_next=%.3f cpu=%.3f setup=%.3f draws=%d objs=%d prims=%d" % [
			k + 1, _ms[i], _x[i], _near[i], _gpu[ip], _gpu[i], _gpu[inx],
			_rcpu[i], _setup[i], int(_draws[i]), int(_objs[i]), int(_prims[i])])

	print("PROBE SUMMARY baseline_frames=%d traverse_frames=%d crossings=%d pins=%d gate=%.2fms traverse_med=%.2fms" % [
		base_idx.size(), all_idx.size(), _events.size(), _pins, gate, med])

	if _events.is_empty():
		_fail("no chunk border crossings were observed")
