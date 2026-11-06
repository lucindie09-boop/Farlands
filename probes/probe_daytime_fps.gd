extends SceneTree
## FPS against the time of day: which hours cost more, and what moves with them.
##
## The question is "does the frame cost change as the sky does, and if it does, what
## changes alongside it". Nothing here asserts a threshold, because an fps number
## belongs to the machine: this is a benchmark (like probe_stream_bench.gd) and it
## exits 0 unless it could not set itself up. The comparison between the BUCKETS of
## one run is the reading.
##
## It runs WINDOWED on purpose. `--headless` selects the dummy renderer, which has no
## frame rate at all, so a headless run would time this script's own awaits; the
## probe refuses to start rather than print that as a result. Vsync is disabled and
## the fps cap lifted for the same reason in reverse: with either in place an idle
## frame and a heavy one are both exactly the display's period and every hour reads
## the same number. What is left is this machine's uncapped frame cost, so compare
## buckets WITHIN one run, not one run against another at a different window size.
##
##   probes/run_probe_shot.sh probes/probe_daytime_fps.gd 300
##
## Two modes over one sampler:
##
##   sweep (default)  The day is frozen (day_duration 1e9) and day_time is stepped
##                    through STEPS positions. Each is left SETTLE seconds to
##                    relight, then sampled for SECS seconds. One camera, one
##                    world, no transitions: clean per-hour numbers.
##   LIVE=1           The day runs at DUR seconds for WATCH seconds, and every frame
##                    is bucketed by the time it was drawn at. This is the cycle as
##                    it happens, transitions included, and it lists the worst
##                    frames with the hour they fell in.
##
## Switches: STEPS=16 SECS=4 SETTLE=1.5 (sweep), LIVE=1 WATCH=60 DUR=60 (cycle).
##
## BEFORE ANY HOUR IS SAMPLED the probe waits for the world to go quiet: the first
## version measured straight after boot and its "midnight is half the fps" was the
## spawn disc still generating (3,584 chunks in that window, primitives still
## climbing every bucket). A bucket that records generation or mesh rebuilds while it
## runs is marked UNSETTLED, and the summary's best/worst is taken from settled
## buckets only, so warm-up cannot be read as the sky.
##
## THE SETTLE IS NOT DECORATION. A validation run at SETTLE=0.3 read the first bucket
## (midnight, the largest jump from the noon the world is held at while it settles) at
## 571 fps against noon's 1,110; at the default 1.5 s all sixteen buckets read
## 1,120-1,163. What the short settle measures is the second after a day_time jump (the
## sky's own relight), not the hour, so an hour reported as slow with a sub-second
## settle is worth re-running at the default before it is believed.
##
## A bucket prints frame-time percentiles, primitives and draw calls, and the
## generation/mesh-rebuild counters that moved while it was sampled -- an hour that is
## slower only where chunks were arriving is a streaming artifact, not the sky. The
## day/night cycle is left ON throughout: with it off, a time of day means nothing
## and every reading is the same day constant.
##
## The engine's own Performance.TIME_FPS and TIME_PROCESS are NOT used: both are
## rolling averages of the last second or so (measured: TIME_PROCESS held 61 ms for
## eighteen frames whose real intervals were 1-3 ms, and TIME_FPS held 100 through
## them), so at a four-second window they smear one hour into the next. Frame time is
## taken from the wall clock instead, which is also the number the player waits.

const SWEEP_STEPS := 16
const SWEEP_SECS := 4.0
const SWEEP_SETTLE := 1.5
const LIVE_WATCH := 60.0
const LIVE_DAY := 60.0
const FROZEN_DAY := 1.0e9      # long enough that a set day_time does not move
const QUIET_SECONDS := 2.0     # streaming must be still this long before measuring
const QUIET_SCAN := 10         # frames between generation-counter reads while waiting
const QUIET_MAX_SECONDS := 90.0
const STAT_EVERY := 8          # frames between generation-stat reads while sampling
const WORST_FRAMES := 8
const BOOT_SETTLE := 1.0
const LIVE_WARMUP := 3.0       # after the jump into a cycle, before live recording starts

var main: Node
var cm: Node
var player: Node
var ok := true

var steps := SWEEP_STEPS
var window_secs := SWEEP_SECS
var settle_secs := SWEEP_SETTLE
var live := false
var watch_secs := LIVE_WATCH
var live_day := LIVE_DAY

# One entry per sampled frame, in sampling order. Five parallel arrays rather than
# an array of dictionaries: a live run at a few hundred fps is tens of thousands of
# frames, and a dictionary per frame would hand the allocator work inside the very
# loop that is measuring the frame.
var _ms: Array[float] = []
var _prims: Array[float] = []
var _draws: Array[float] = []
var _t: Array[float] = []
var _bucket: Array[int] = []
var _gens_by_bucket := {}
var _rebuilds_by_bucket := {}
var _stat_frame := 0
var _gens_seen := 0
var _rebuilds_seen := 0

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _env_int(name: String, fallback: int) -> int:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else int(raw)

func _env_float(name: String, fallback: float) -> float:
	var raw := OS.get_environment(name)
	return fallback if raw.is_empty() else float(raw)

func _initialize() -> void:
	# The dummy renderer has no frame rate: refuse instead of measuring nothing.
	if DisplayServer.get_name() == "headless":
		print("PROBE FAIL: this measures the renderer's frame rate, and --headless selects the")
		print("PROBE FAIL: dummy one -- run it windowed: probes/run_probe_shot.sh probes/probe_daytime_fps.gd")
		quit(1)
		return
	steps = clampi(_env_int("STEPS", SWEEP_STEPS), 1, 64)
	window_secs = maxf(_env_float("SECS", SWEEP_SECS), 0.2)
	settle_secs = maxf(_env_float("SETTLE", SWEEP_SETTLE), 0.0)
	live = _env_int("LIVE", 0) != 0
	watch_secs = maxf(_env_float("WATCH", LIVE_WATCH), 1.0)
	live_day = maxf(_env_float("DUR", LIVE_DAY), 1.0)
	_run()

# The frame interval the player actually waits: read around `process_frame`, not off
# Performance.TIME_FPS (see the header).
func _frame() -> void:
	await process_frame

func _seconds(s: float) -> void:
	var end := Time.get_ticks_usec() + int(s * 1_000_000.0)
	while Time.get_ticks_usec() < end:
		await process_frame

func _stream_counters() -> Vector2i:
	var s: Dictionary = cm.get_generation_stats()
	return Vector2i(int(s.get("generations", 0)), int(s.get("rebuilds", 0)))

# day_time 0 = midnight, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset: the sun's own
# elevation is that cosine, which is what the sky shader is handed.
func _elevation(t: float) -> float:
	return cos(TAU * (t - 0.5))

func _phase(t: float) -> String:
	var e := _elevation(t)
	if e <= -0.85:
		return "midnight"
	if e < -0.15:
		return "night"
	if e < 0.15:
		return "horizon"
	if e < 0.75:
		return "low sun"
	return "noon"

# Wait until neither generations nor mesh rebuilds have moved for QUIET_SECONDS.
# Reads every QUIET_SCAN frames rather than every frame: get_generation_stats builds
# a dictionary, and this loop is supposed to sit still.
func _wait_quiet() -> void:
	var last := _stream_counters()
	var quiet_since := -1
	var begin := Time.get_ticks_usec()
	var deadline := begin + int(QUIET_MAX_SECONDS * 1_000_000.0)
	var tick := 0
	var quiet := false
	while Time.get_ticks_usec() < deadline:
		await _frame()
		tick += 1
		if tick % QUIET_SCAN != 0:
			continue
		var now := _stream_counters()
		if now != last:
			last = now
			quiet_since = -1
		elif quiet_since < 0:
			quiet_since = Time.get_ticks_usec()
		elif float(Time.get_ticks_usec() - quiet_since) / 1_000_000.0 >= QUIET_SECONDS:
			quiet = true
			break
	var s := _stream_counters()
	if quiet:
		print("PROBE quiet after %.1fs (generations=%d rebuilds=%d)" % [
			float(Time.get_ticks_usec() - begin) / 1_000_000.0, s.x, s.y])
	else:
		print("PROBE WARNING: still streaming after %.0fs (generations=%d rebuilds=%d) -- buckets that moved will say UNSETTLED" % [
			QUIET_MAX_SECONDS, s.x, s.y])

# One frame into the grand total, tagged with the bucket it was drawn for. The
# generation counters are read every STAT_EVERY frames and the delta since the last
# read is charged to the current bucket, so "this hour was slower" can be checked
# against "this hour was streaming" in the same table.
func _sample(bucket: int) -> void:
	var a := Time.get_ticks_usec()
	await process_frame
	var now := Time.get_ticks_usec()
	_ms.append(float(now - a) / 1000.0)
	_prims.append(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME))
	_draws.append(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME))
	_t.append(float(cm.get("day_time")))
	_bucket.append(bucket)
	_stat_frame += 1
	if _stat_frame % STAT_EVERY == 0:
		var s := _stream_counters()
		_gens_by_bucket[bucket] = int(_gens_by_bucket.get(bucket, 0)) + maxi(0, s.x - _gens_seen)
		_rebuilds_by_bucket[bucket] = int(_rebuilds_by_bucket.get(bucket, 0)) + maxi(0, s.y - _rebuilds_seen)
		_gens_seen = s.x
		_rebuilds_seen = s.y

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing from main.tscn")
		quit(1)
		return

	# Measurement conditions, set before anything is timed and printed rather than
	# assumed. The settings menu has already applied the player's own fps cap by now.
	var was_cap := Engine.max_fps
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	var was_enabled: bool = cm.get("day_night_cycle_enabled")
	var was_time: float = cm.get("day_time")
	var was_duration: float = cm.get("day_duration")
	print("PROBE setup %s steps=%d window=%.1fs settle=%.1fs watch=%.0fs day=%.0fs cap=%d->0" % [
		"live" if live else "sweep", steps, window_secs, settle_secs, watch_secs, live_day, was_cap])

	# Boot: land, then let the world go quiet. The settle is in SECONDS, not frames,
	# because every frame here runs uncapped.
	var waited := 0
	while waited < 900:
		await _frame()
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	await _seconds(BOOT_SETTLE)

	# The cycle has to be ON for a time to mean anything (get_blend answers "day"
	# while it is off). Hold it at noon while the world settles, so the gate's own
	# streaming is not charged to whichever hour happens to be measured first.
	cm.set("day_night_cycle_enabled", true)
	cm.set("day_duration", FROZEN_DAY)
	cm.set("day_time", 0.5)
	await _wait_quiet()
	# The window reaches the size the player's settings ask for a frame or two in (the
	# scene applies it during its own _ready), so it is reported here rather than with
	# the setup line: the 100x100 default is not the size anything was measured at.
	print("PROBE measuring at %s, vsync=%d" % [str(root.size), DisplayServer.window_get_vsync_mode()])
	_gens_seen = _stream_counters().x
	_rebuilds_seen = _stream_counters().y

	if live:
		await _run_live()
	else:
		await _run_sweep()

	cm.set("day_time", was_time)
	cm.set("day_duration", was_duration)
	cm.set("day_night_cycle_enabled", was_enabled)
	Engine.max_fps = was_cap
	_report()
	print("PROBE %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)

func _run_sweep() -> void:
	for i in steps:
		cm.set("day_time", float(i) / float(steps))
		await _seconds(settle_secs)
		var end := Time.get_ticks_usec() + int(window_secs * 1_000_000.0)
		while Time.get_ticks_usec() < end:
			await _sample(i)

func _run_live() -> void:
	cm.set("day_duration", live_day)
	cm.set("day_time", 0.0)
	# The jump into this hour is the same relight transient the sweep's settle exists
	# for, and it is longer than a bucket at a short day length: a validation run that
	# waited 0.5 s read the first buckets' p5 at 388-397 fps against 766-775 elsewhere,
	# and one that waited 1.5 s still read 531 in the bucket it started recording in
	# (a 10 s day, so 1.25 s a bucket). 3 s clears it measured. The cycle keeps running
	# through the wait, so recording begins part-way round rather than at t=0 -- over
	# WATCH seconds every bucket is still sampled, which is all the buckets claim.
	await _seconds(LIVE_WARMUP)
	var end := Time.get_ticks_usec() + int(watch_secs * 1_000_000.0)
	while Time.get_ticks_usec() < end:
		var t := float(cm.get("day_time"))
		await _sample(int(t * float(steps)) % steps)

# Statistics for the frames of one bucket. The stutter gate is twice the bucket's own
# median AND at least 4 ms longer: without the second half a quiet midnight bucket
# calls a 1.1 ms frame a stutter because it is next to 0.5 ms ones, and the share
# stops meaning anything.
func _window_stats(idx: Array) -> Dictionary:
	var n := idx.size()
	var ms: Array[float] = []
	var prims := 0.0
	var draws := 0.0
	var t_sum := 0.0
	for raw in idx:
		var i := int(raw)
		ms.append(_ms[i])
		prims += _prims[i]
		draws += _draws[i]
		t_sum += _t[i]
	var sorted: Array[float] = ms.duplicate()
	sorted.sort()
	var total := 0.0
	for v in sorted:
		total += v
	var mean := total / float(n)
	var median := sorted[int(n / 2)]
	var p95 := sorted[clampi(int(floor(0.95 * float(n - 1))), 0, n - 1)]
	var gate := maxf(2.0 * median, median + 4.0)
	var stutter := 0
	for v in sorted:
		if v > gate:
			stutter += 1
	return {
		"t": t_sum / float(n),
		"frames": n,
		"fps_med": 1000.0 / maxf(median, 0.001),
		"fps_mean": 1000.0 / maxf(mean, 0.001),
		"fps_p5": 1000.0 / maxf(p95, 0.001),
		"fps_min": 1000.0 / maxf(sorted[n - 1], 0.001),
		"ms_med": median,
		"ms_max": sorted[n - 1],
		"stutter": stutter,
		"prims": int(prims / float(n)),
		"draws": int(draws / float(n)),
	}

func _report() -> void:
	if _ms.is_empty():
		_fail("no frames were sampled")
		return
	var by_bucket := {}
	for i in _ms.size():
		var b := _bucket[i]
		if not by_bucket.has(b):
			by_bucket[b] = []
		(by_bucket[b] as Array).append(i)
	var order: Array = by_bucket.keys()
	order.sort()
	var settled: Array = []
	var everything: Array = []
	var stutters := 0
	for b in order:
		var w := _window_stats(by_bucket[b])
		var t := float(w["t"])
		var moved := int(_gens_by_bucket.get(int(b), 0)) + int(_rebuilds_by_bucket.get(int(b), 0))
		stutters += int(w["stutter"])
		everything.append(w)
		if moved == 0:
			settled.append(w)
		print("PROBE bucket %02d/%d t=%.3f elev=%+.2f %-8s frames=%d fps med=%.0f mean=%.0f p5=%.0f min=%.0f | frame med=%.2fms longest=%.2fms stutter=%d (%.1f%%) | prims=%d draws=%d | gens=%d rebuilds=%d%s" % [
			int(b), steps, t, _elevation(t), _phase(t), int(w["frames"]),
			float(w["fps_med"]), float(w["fps_mean"]), float(w["fps_p5"]), float(w["fps_min"]),
			float(w["ms_med"]), float(w["ms_max"]), int(w["stutter"]),
			100.0 * float(w["stutter"]) / maxf(1.0, float(w["frames"])),
			int(w["prims"]), int(w["draws"]),
			int(_gens_by_bucket.get(int(b), 0)), int(_rebuilds_by_bucket.get(int(b), 0)),
			" UNSETTLED" if moved > 0 else ""])

	var seconds := 0.0
	for v in _ms:
		seconds += v
	seconds /= 1000.0
	print("PROBE SUMMARY buckets=%d settled=%d frames=%d sampled=%.1fs stutters=%d (%.2f%%)" % [
		order.size(), settled.size(), _ms.size(), seconds, stutters,
		100.0 * float(stutters) / maxf(1.0, float(_ms.size()))])

	# Best and worst over SETTLED buckets when there are two to compare: a bucket
	# that was still streaming is not evidence about its hour.
	var pool: Array = settled if settled.size() >= 2 else everything
	var best: Dictionary = pool[0]
	var worst: Dictionary = pool[0]
	for w in pool:
		if float(w["fps_med"]) > float(best["fps_med"]):
			best = w
		if float(w["fps_med"]) < float(worst["fps_med"]):
			worst = w
	print("PROBE SUMMARY best t=%.3f elev=%+.2f fps_med=%.0f | worst t=%.3f elev=%+.2f fps_med=%.0f | drop=%.1f%%" % [
		float(best["t"]), _elevation(float(best["t"])), float(best["fps_med"]),
		float(worst["t"]), _elevation(float(worst["t"])), float(worst["fps_med"]),
		100.0 * (1.0 - float(worst["fps_med"]) / maxf(1.0, float(best["fps_med"])))])

	# The individual worst frames, each with the hour it happened at: a single 40 ms
	# frame in one hour is a different story from that hour being uniformly slower.
	var ranked: Array = []
	for i in _ms.size():
		ranked.append([_ms[i], i])
	ranked.sort_custom(func(a, b): return float(a[0]) > float(b[0]))
	for k in mini(WORST_FRAMES, ranked.size()):
		var entry: Array = ranked[k]
		var i := int(entry[1])
		print("PROBE worst frame %d: %.1fms (%.0f fps) at t=%.3f %s prims=%d" % [
			k + 1, float(entry[0]), 1000.0 / maxf(float(entry[0]), 0.001), _t[i],
			_phase(_t[i]), int(_prims[i])])
