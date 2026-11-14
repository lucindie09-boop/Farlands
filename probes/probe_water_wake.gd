extends SceneTree
## What the bounded water wake costs at a shoreline.
##
## Builds a floating stone shelf, fills a 7x7 pond with `surface_water` (the static
## block worldgen emits for a sea), then places one stone block against the pond's
## edge. That edit is adjacent to generated water, so `BlockEditor::place_block`
## converts the generated cells within `FARLANDS_WATER_WAKE_RADIUS` (2 by default)
## into live `water` and wakes them; this probe reports how many, how much fluid
## work follows, and what the frame times do while it settles.
##
## It also times 100 edits with no water within reach, which is the cost the wake
## adds to ordinary building (the seven-cell pre-check and two name lookups).
##
## Run it through the wrapper, which snapshots and restores the saved world:
##   probes/run_probe.sh res://probes/probe_water_wake.gd
## Compare radii by running it again with FARLANDS_WATER_WAKE_RADIUS=0 or 1.

const SHELF_RADIUS := 5
const POND_RADIUS := 3
const WRITES_PER_FRAME := 40
const QUIET_MS := 700
const MAX_SETTLE_MS := 20000
const DRY_EDITS := 100

var main: Node3D
var cm: Node
var player: Node3D
var ok := true
var stone := 0
var generated := 0
var live := 0


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing")
		quit(1)
		return
	stone = BlockTextures.get_block_id_by_name("stone")
	generated = BlockTextures.get_block_id_by_name("surface_water")
	live = BlockTextures.get_block_id_by_name("water")
	if stone == 0 or generated == 0 or live == 0:
		_fail("a block id is missing (stone=%d surface_water=%d water=%d)" % [stone, generated, live])
		quit(1)
		return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(5):
		await process_frame

	var spot := player.global_position.floor()
	var sx := int(spot.x)
	var sy := int(spot.y) + 2
	var sz := int(spot.z)

	# Ordinary building, with the wake off and on: the cost it adds to an edit that
	# touches no water is two name lookups and the seven-cell pre-check.
	cm.set_water_wake_radius(0)
	var dry_start := Time.get_ticks_usec()
	for i in range(DRY_EDITS):
		cm.set_block(sx - SHELF_RADIUS + (i % SHELF_RADIUS), sy + 4, sz - SHELF_RADIUS + (i / SHELF_RADIUS), stone)
	var dry_base_ms := float(Time.get_ticks_usec() - dry_start) / 1000.0
	cm.set_water_wake_radius(2)
	dry_start = Time.get_ticks_usec()
	for i in range(DRY_EDITS):
		cm.set_block(sx - SHELF_RADIUS + (i % SHELF_RADIUS), sy + 6, sz - SHELF_RADIUS + (i / SHELF_RADIUS), stone)
	var dry_wake_ms := float(Time.get_ticks_usec() - dry_start) / 1000.0
	print("PROBE wake dry_edits=%d off_us=%.1f on_us=%.1f added_us=%.1f" % [
		DRY_EDITS, dry_base_ms * 1000.0 / float(DRY_EDITS),
		dry_wake_ms * 1000.0 / float(DRY_EDITS),
		(dry_wake_ms - dry_base_ms) * 1000.0 / float(DRY_EDITS)])
	# Fill the pond with the wake off, so it is a body of GENERATED water like the
	# worldgen sea; switching it on for the trigger is what the prototype does.
	cm.set_water_wake_radius(0)

	# Shelf, then the pond it holds.
	var writes := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			cm.set_block(sx + dx, sy - 1, sz + dz, stone)
			writes += 1
			if writes % WRITES_PER_FRAME == 0:
				await process_frame
	for dz in range(-POND_RADIUS, POND_RADIUS + 1):
		for dx in range(-POND_RADIUS, POND_RADIUS + 1):
			cm.set_block(sx + dx, sy, sz + dz, generated)
			writes += 1
			if writes % WRITES_PER_FRAME == 0:
				await process_frame
	for i in range(10):
		await process_frame

	var pond := _count_pond(sx, sy, sz)
	print("PROBE wake pond surface_water=%d water=%d" % [pond.x, pond.y])
	if pond.x < (2 * POND_RADIUS + 1) * (2 * POND_RADIUS + 1):
		_fail("the pond did not fill: %d of %d cells are surface_water" % [
			pond.x, (2 * POND_RADIUS + 1) * (2 * POND_RADIUS + 1)])

	# The trigger: one stone block against the pond's east edge, with the wake on.
	cm.set_water_wake_radius(2)
	var trigger_ms := 0.0
	var started := Time.get_ticks_usec()
	cm.set_block(sx + POND_RADIUS + 1, sy, sz, stone)
	trigger_ms = float(Time.get_ticks_usec() - started) / 1000.0

	# Settle: no block change in the pond window for QUIET_MS.
	var frames := 0
	var worst := 0.0
	var total := 0.0
	var settle_ms := -1.0
	var last_change := Time.get_ticks_usec()
	var previous := _count_pond(sx, sy, sz)
	var settle_start := Time.get_ticks_usec()
	while true:
		var frame_start := Time.get_ticks_usec()
		await process_frame
		var frame_ms := float(Time.get_ticks_usec() - frame_start) / 1000.0
		frames += 1
		worst = maxf(worst, frame_ms)
		total += frame_ms
		var now := _count_pond(sx, sy, sz)
		if now != previous:
			previous = now
			last_change = Time.get_ticks_usec()
		var elapsed := float(Time.get_ticks_usec() - settle_start) / 1000.0
		if float(Time.get_ticks_usec() - last_change) / 1000.0 >= QUIET_MS:
			settle_ms = elapsed
			break
		if elapsed >= MAX_SETTLE_MS:
			break

	var after := _count_pond(sx, sy, sz)
	var stats: Dictionary = cm.get_fluid_stats()
	# Cells within the wake radius (2) of the trigger that were surface_water and
	# are now any live water block: the conversion the prototype owns.
	var converted := 0
	for dz in range(-2, 3):
		for dy in range(-2, 3):
			for dx in range(-2, 3):
				var id: int = cm.get_block(sx + POND_RADIUS + 1 + dx, sy + dy, sz + dz)
				if id != generated and id != 0 and id != stone:
					converted += 1
	print("PROBE wake trigger_ms=%.2f converted=%d surface_left=%d water=%d settle_ms=%.0f frames=%d mean=%.2fms worst=%.2fms pending=%d ticks=%d" % [
		trigger_ms, converted, after.x, after.y, settle_ms, frames,
		total / float(max(1, frames)), worst, int(stats.get("pending", -1)), int(stats.get("ticks", -1))])
	if converted == 0:
		_fail("the edit did not convert any generated water")
	if after.y < pond.y:
		_fail("live water shrank: %d -> %d" % [pond.y, after.y])
	print("PROBE wake %s" % ["OK" if ok else "FAILED"])
	quit(0 if ok else 1)


## surface_water and live water around the pond, as (generated, live).
func _count_pond(sx: int, sy: int, sz: int) -> Vector2i:
	var static_cells := 0
	var live_cells := 0
	for dz in range(-POND_RADIUS - 2, POND_RADIUS + 3):
		for dx in range(-POND_RADIUS - 2, POND_RADIUS + 3):
			var id: int = cm.get_block(sx + dx, sy, sz + dz)
			if id == generated:
				static_cells += 1
			elif id != 0 and id != stone:
				live_cells += 1
	return Vector2i(static_cells, live_cells)
