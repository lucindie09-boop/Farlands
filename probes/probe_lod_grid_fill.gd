extends SceneTree
## How long the far field takes to ARRIVE, in one number.
##
## The reach is the mode's headline setting and its fill is the mode's headline cost:
## at the top of the slider it is 45,369 tiles over some 71,000 sampled columns, and
## the first version took about forty seconds to put them up. The reason was one line
## of arithmetic -- schedule_builds topped the in-flight count up ONCE PER FRAME, so
## "12 in flight" was not a concurrency ceiling but a dispatch rate of 12 x the frame
## rate, and the pool's fifteen workers were idle for most of the fill.
##
## So this measures that arithmetic rather than the machine: tiles, frames, seconds,
## and the rate both ways. A dispatch-bound fill shows up as a constant tiles-per-frame
## (it was 6.1) that a faster machine cannot raise; a pool-bound one shows up as a
## tiles-per-frame that scales with the workers.
##
## Measured at the top of the slider, all three on this one probe (16 cores, headless):
## the old budgets gave 6 tiles a frame, 467 tiles/s and **97.2 s**; the dispatch rate
## fixed gave 31 a frame and 30.1 s; and the merge interval after that, 24 a frame,
## 2,214 tiles/s and **20.5 s** -- 4.7x, and the probe's own assertions fail on the
## first of those three, which is what they are for. The remaining limit is the sampler
## and not this arithmetic: ~73,600 columns at ~4 ms each over the pool's fifteen
## workers is ~20 s of the 20.5, and each column is walked TWICE by the two public
## entry points the build calls (find_surface_y re-derives the blended biome
## amplification that sample_column_debug already returned), so the next lever is a
## column query that answers both in one pass rather than anything here.
##
## The floors below are therefore set at the SHAPE and not at the machine: 15 tiles a
## frame is more than twice the old dispatch bound of 6, and 45 s is inside the forty
## the reach used to take on a slower one.
##
##   probes/run_probe.sh probes/probe_lod_grid_fill.gd 300
##
## run_probe.sh snapshots and restores user://chunks; a plain headless run of this
## probe generates a few chunks around the origin and does not.

const REACH_RINGS := 100          # the top of the slider: 100 * 256 blocks of horizon
const RENDER_DISTANCE := 2        # small, so what is waited for is the grid, not streaming
const MAX_FRAMES := 20000         # the old build needed 7,425 of them; this is headroom
const PROGRESS_EVERY := 120       # one line of progress, so a stall is visible
const MIN_TILES_PER_FRAME := 15.0
const MAX_SECONDS := 45.0
## A tile with no geometry at all is an honest hole (the sampler refused the column
## rather than inventing terrain) and the mode marks it settled so it is not re-asked.
## They sit at the horizon -- 24 of the 45,369 here, all at the far corners -- so the
## share is what is asserted, not zero.
const MAX_HOLE_SHARE := 0.01

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _run() -> void:
	var cm: Node = ClassDB.instantiate("ChunkManager")
	if cm == null:
		print("PROBE FAIL: ChunkManager is not registered")
		quit(1)
		return
	# In the tree, which is what gives the controller its owner: schedule_builds does
	# nothing without one, and a probe whose fill is zero tiles would be measuring the
	# absence of a scene rather than the speed of the mode.
	root.add_child(cm)
	cm.set("render_distance", RENDER_DISTANCE)
	cm.set("lod_grid_enabled", true)
	cm.set("lod_grid_outer_rings", 0)
	# The ladder's own reach first, so the fill being timed is the reach's and not the
	# first ring the mode ever built.
	var warm := 0
	while warm < 600:
		await process_frame
		warm += 1
		var s: Dictionary = cm.get_lod_grid_stats()
		if int(s.get("tiles", 0)) > 0 and int(s.get("uploads", 0)) >= int(s.get("tiles", 0)):
			break
	var ladder: Dictionary = cm.get_lod_grid_stats()
	_ok("the ladder's own reach is up before the reach is moved",
		int(ladder.get("tiles", 0)) > 0 and int(ladder.get("uploads", 0)) >= int(ladder.get("tiles", 0)),
		"%d tiles" % int(ladder.get("tiles", 0)))

	cm.set("lod_grid_outer_rings", REACH_RINGS)
	var horizon := int(cm.get_lod_grid_stats().get("outer_radius", 0))
	_ok("the far end of the slider is asked for", horizon >= 25000,
		"%d blocks (%.1f km)" % [horizon, float(horizon) / 1000.0])

	var start_ms := Time.get_ticks_msec()
	var frames := 0
	var filled := false
	var stats: Dictionary = {}
	while frames < MAX_FRAMES:
		await process_frame
		frames += 1
		stats = cm.get_lod_grid_stats()
		var live := int(stats.get("tiles", 0))
		var answered := int(stats.get("uploads", 0)) + int(stats.get("failed", 0))
		if live > 0 and answered >= live:
			filled = true
			break
		if frames % PROGRESS_EVERY == 0:
			print("probe: %5d frames  %5d / %d tiles  %.1f s" % [frames,
				answered, live, float(Time.get_ticks_msec() - start_ms) / 1000.0])
	var seconds := float(Time.get_ticks_msec() - start_ms) / 1000.0
	var tiles := int(stats.get("tiles", 0))
	var per_frame := float(tiles) / float(maxi(frames, 1))
	var per_second := float(tiles) / maxf(seconds, 0.001)

	print("probe: fill %d tiles in %d frames / %.1f s  = %.0f tiles/s, %.0f tiles/frame"
		% [tiles, frames, seconds, per_second, per_frame])
	print("probe: %d columns sampled, %d asks answered from the shared table, %d draw calls"
		% [int(stats.get("columns_sampled", 0)), int(stats.get("cache_hits", 0)),
			int(stats.get("draw_calls", 0))])

	var holes := int(stats.get("failed", -1))
	_ok("the top of the reach answers every tile it wanted", filled,
		"%s at %d tiles" % ["filled" if filled else "gave up", tiles])
	_ok("and the holes in it are the sampler's own refusals, not a pile of them",
		holes >= 0 and float(holes) <= float(tiles) * MAX_HOLE_SHARE,
		"%d of %d tiles empty (%.2f%%) -- a refused column, never invented terrain"
			% [holes, tiles, 100.0 * float(holes) / float(maxi(tiles, 1))])
	_ok("the fill is not one dispatch per frame", per_frame >= MIN_TILES_PER_FRAME,
		"%.0f tiles a frame (a per-frame dispatch was measured at 6)" % per_frame)
	_ok("and it therefore arrives in seconds, not a minute", seconds <= MAX_SECONDS,
		"%.1f s at %.0f tiles/s" % [seconds, per_second])

	print("PROBE lod grid fill: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
