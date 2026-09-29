extends SceneTree
## Headless probe: a big paste must not stall a frame, and must still account for
## every cell.
##
## The paste is written a batch at a time now (kPasteCellsPerBatch), so this
## measures the two things that change with that: the longest frame the MAIN
## thread sees from the first write to the last, and the accounting —
## written + unchanged + covered + still queued == planned — which is what proves
## the budget deferred work rather than dropping it.
##
##   .freebuff/run_probe.sh .freebuff/probe_paste_budget.gd

const FILE := "10179.schematic"

var main: Node
var cm: Node
var player: Node
var ok := true

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _initialize() -> void:
	_run()

func _surface_point(from: Vector3) -> Vector3:
	var x := int(floor(from.x))
	var z := int(floor(from.z))
	var y := int(from.y) + 64
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(x, y + 1, z)
		y -= 1
	return Vector3.INF

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

	var waited := 0
	while waited < 1200:
		await process_frame
		waited += 1
		if waited > 30 and player.is_on_floor():
			break

	var base := _surface_point(player.global_position)
	var anchor := Vector3i(int(base.x) - 20, int(base.y) + 4, int(base.z) - 20)
	var resolved: String = (preload("res://schematic_files.gd") as GDScript).resolve(FILE)
	if resolved.is_empty():
		_fail("%s is not in the schematics folders" % FILE)
		quit(1)
		return
	var bytes := FileAccess.get_file_as_bytes(resolved)
	if bytes.is_empty():
		_fail("could not read %s" % resolved)
		quit(1)
		return

	# The decode and the plan are off-thread (see build_store), so start them and
	# keep the frame loop turning until they land.
	var worst_read := 0.0
	var t := Time.get_ticks_usec()
	var read_state: Dictionary = cm.prepare_schematic(bytes, {})
	while not read_state.get("ready", false) and Time.get_ticks_usec() - t < 30_000_000:
		var f0 := Time.get_ticks_usec()
		await process_frame
		worst_read = maxf(worst_read, float(Time.get_ticks_usec() - f0) / 1000.0)
		read_state = cm.prepare_schematic(bytes, {})
	if not read_state.get("ready", false):
		_fail("%s never finished decoding (%s)" % [FILE, read_state.get("error", "?")])
		quit(1)
		return
	print("probe: %s decoded + planned off-thread; worst frame while reading: %.1f ms"
		% [FILE, worst_read])

	var t0 := Time.get_ticks_usec()
	var placed: Dictionary = cm.paste_schematic(bytes, anchor.x, anchor.y, anchor.z, {})
	var first_ms := float(Time.get_ticks_usec() - t0) / 1000.0
	if not placed.get("ok", false):
		_fail("the paste failed: %s" % placed.get("error", "?"))
		quit(1)
		return
	print("probe: first batch (the call itself) took %.1f ms, %d cells written, %d outstanding"
		% [first_ms, int(placed.get("cells", 0)), int(placed.get("pending_cells", 0))])

	var worst := 0.0
	var frames := 0
	var t1 := Time.get_ticks_usec()
	var pending: Dictionary = placed
	while Time.get_ticks_usec() - t1 < 60_000_000:
		var f0 := Time.get_ticks_usec()
		await process_frame
		worst = maxf(worst, float(Time.get_ticks_usec() - f0) / 1000.0)
		frames += 1
		pending = cm.get_pending_paste()
		var finished: Dictionary = cm.take_paste_completion()
		if not finished.is_empty():
			print("probe: finished in %d frames; worst frame during the wait: %.1f ms"
				% [frames, worst])
			if bool(finished.get("abandoned", false)):
				_fail("the paste gave up: %d cells left after %.1fs"
					% [int(finished.get("leftover_cells", 0)),
					   float(finished.get("waiting_ms", 0.0)) / 1000.0])
			break
		if pending.is_empty() and frames > 2:
			break

	# The accounting is the point of the budget: deferred work must still be
	# planned work.
	var planned := int(placed.get("planned", 0))
	var accounted := int(placed.get("cells", 0)) + int(placed.get("unchanged", 0)) \
		+ int(placed.get("covered", 0)) + int(pending.get("remaining_cells", 0))
	print("probe: planned %d, accounted %d (%d written, %d unchanged, %d covered, %d still queued)"
		% [planned, accounted, int(placed.get("cells", 0)), int(placed.get("unchanged", 0)),
		   int(placed.get("covered", 0)), int(pending.get("remaining_cells", 0))])
	if planned > 0 and accounted != planned:
		_fail("cells went missing: %d accounted of %d planned" % [accounted, planned])

	# A budget that halves the frame's work still has to be under a frame: the
	# whole point is that no single frame carries the paste.
	if worst >= 100.0:
		_fail("a frame during the paste took %.1f ms" % worst)
	elif first_ms >= 100.0:
		_fail("the paste call itself took %.1f ms" % first_ms)
	else:
		print("probe: no frame over 100 ms (call %.1f ms, worst %.1f ms)" % [first_ms, worst])

	cm.undo_paste()
	if ok:
		print("PROBE PASS")
	quit(0 if ok else 1)
