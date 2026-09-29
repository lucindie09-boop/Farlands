extends SceneTree
## Does a poured source actually FLOW in the running game?
##
## The simulation is only real if the game drives it: WorldUpdater advances it, a
## player edit wakes it, and its writes land in chunks. This probe builds a flat
## floating shelf above the terrain (every write targets air, because placing into
## an occupied cell is refused — a real gameplay rule, not a probe shortcut),
## drops a water source in the middle, and watches the blocks change on their own.
##
## What it pins, in order:
##   1. A source floods a full diamond of radius 7, and the depth stored in each
##      cell is its distance from the source (the fluid state, as a block).
##   2. It settles: once the flood is done, nothing more changes.
##   3. Digging a shaft under the settled pool wakes it and fills the shaft with
##      FALLING water — the player-edit wake-up and down-beats-sideways, live.
##   4. The flood only ever wrote into air: no terrain was overwritten, and it
##      never ran into a cell it could not judge.
##
## Run it through the wrapper, which snapshots and restores the saved world — the
## shelf and the poured water are persisted edits, so a bare run would leave them
## in the world the user plays:
##   .freebuff/run_probe.sh .freebuff/probe_flow.gd

const SHELF_RADIUS := 12         # 25x25. The drop-seeking search reaches 5 blocks,
                                 # so the shelf must out-reach the radius-7 diamond
                                 # by more than that or the edge would steal the flow.
const WRITES_PER_FRAME := 40
const QUIET_MS := 700            # no block change for this long = settled (a tick
                                 # is 50 ms and a wave advances one ring per 5 ticks)
const MAX_SETTLE_MS := 60000
const SHAFT_DEPTH := 3           # the well dug into the settled pool
const SHAFT_DISTANCE := 3        # from the source, inside the 4-step search

var main: Node3D
var cm: Node
var player: Node3D
var ok := true
var depth_by_id := {}            # block id -> fluid depth (source and runoff)
var falling_ids := {}            # block ids that mean "a falling column"
var natural := {}                # fluid cells that were already there (a pond, an
                                 # ocean): not the flood's doing, so never counted
var dug := {}                    # cells the probe cuts out of the shelf itself

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing"); quit(1); return
	main = scene.instantiate()
	root.add_child(main)

	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing"); quit(1); return

	if not _resolve_fluid_ids():
		quit(1); return
	if not _check_water_shapes():
		quit(1); return

	# Wait for the body to land: the shelf is placed relative to the terrain, and
	# a player still falling would put the ceiling scan inside the hill.
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
	var sz := int(spot.z)
	var stone := BlockTextures.get_block_id_by_name("stone")
	var source := BlockTextures.get_block_id_by_name("water")

	# The shelf floats one block above the tallest terrain in the region: every
	# cell it writes into is air, and nothing natural is within the flood's reach.
	# Both halves of that need the region to be RESIDENT first — an unloaded chunk
	# reads as air, so a ceiling scan run too early picks a shelf height inside the
	# hill, and a write into the chunk that has not streamed in yet is refused. So:
	# measure, lay, and if anything did not land, the region was still streaming —
	# measure again and lay again.
	var ceiling := 0
	var sy := 0
	var laid := false
	var attempts := 0
	for attempt in range(10):
		attempts = attempt + 1
		ceiling = await _stable_ceiling(sx, sz)
		sy = ceiling + 2
		if await _build_shelf(sx, sy, sz, stone):
			laid = true
			break
	if not laid:
		_fail("the shelf never landed around (%d, %d) — the region never settled" % [sx, sz])
		quit(1); return
	print("probe: shelf at (%d, %d, %d) — terrain ceiling %d; source=%d runoff_1=%d fallen=%d (attempt %d)"
		% [sx, sy, sz, ceiling, source, BlockTextures.get_block_id_by_name("water_runoff_1"),
		   BlockTextures.get_block_id_by_name("water_fallen"), attempts])

	# What every cell in reach held before any water: the check that a flood never
	# writes into anything but air.
	var before := _snapshot(sx, sy, sz)
	for key in before:
		if depth_by_id.has(before[key]) or falling_ids.has(before[key]):
			natural[key] = before[key]

	# --- 1. pour and watch it spread ---------------------------------------
	cm.set_block(sx, sy, sz, source)
	var flood := await _settle(sx, sy, sz)
	var count: int = flood["count"]
	var cells: Dictionary = flood["cells"]
	print("probe: the source flooded %d cells and settled (%d ms of quiet)"
		% [count, flood["quiet"]])
	if not ok:
		quit(1); return

	# The diamond: 113 cells, and every cell's stored depth is its distance from
	# the source. This is the simulation writing a STATE, not just a block.
	var wrong := 0
	var sample := ""
	var outside := 0
	var stray := ""
	for key in cells:
		var d: int = absi(key.x - sx) + absi(key.z - sz)
		if d > 7:
			outside += 1
			if stray == "":
				stray = "%s (depth %d, distance %d)" % [key, depth_by_id.get(cells[key], -1), d]
			continue
		var depth: int = depth_by_id.get(cells[key], -1)
		if depth != d:
			wrong += 1
			if sample == "":
				sample = "%s holds depth %d, expected %d" % [key, depth, d]
	if outside > 0:
		_fail("%d water cells are outside the radius-7 layer-0 diamond; first is %s" % [outside, stray])
	if wrong > 0:
		_fail("%d cells hold the wrong depth; first is %s" % [wrong, sample])
	if count != 113:
		_fail("the flood is %d cells, expected the 113 of a radius-7 diamond" % count)
	var edge: int = cm.get_block(sx + 8, sy, sz)
	if edge != 0:
		_fail("water reached 8 blocks from the source (id %d), past the runoff limit" % edge)

	# The rim is five blocks past the diamond's reach, so a wet cell on it means
	# the flood ran off the platform instead of pooling on it. This is the check
	# that the shelf layer scan above gives up: water that leaves goes down or
	# outward, and either way it has to cross the rim first.
	var rim_wet := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			if absi(dx) != SHELF_RADIUS and absi(dz) != SHELF_RADIUS:
				continue
			var rim_id: int = cm.get_block(sx + dx, sy, sz + dz)
			if depth_by_id.has(rim_id) or falling_ids.has(rim_id):
				rim_wet += 1
	if rim_wet > 0:
		_fail("%d cells of the shelf rim are wet — the flood left the platform" % rim_wet)

	# --- 2. it settles: nothing changes while we watch ----------------------
	var idle := await _settle(sx, sy, sz)
	if idle["count"] != count:
		_fail("the settled pool changed on its own: %d cells, was %d" % [idle["count"], count])

	# What the simulation reports about its own ticking: a settled flood must be
	# idle (nothing queued), and the numbers say what an active one costs.
	var stats: Dictionary = cm.get_fluid_stats()
	print("probe: the simulation ran %d ticks over %d cells; its last tick touched %d and wrote %d in %.2f ms; %d queued"
		% [stats["ticks"], stats["cells_ticked"], stats["last_tick_cells"], stats["last_tick_writes"],
		   stats["last_tick_ms"], stats["pending"]])
	if not stats["enabled"]:
		_fail("the flow simulation is disabled — the state table found no water states")
	if int(stats["pending"]) != 0:
		_fail("the flood never drained its work list: %d cells still queued" % stats["pending"])

	# --- 3. dig a shaft; the pool must wake and pour into it ----------------
	# Built right now, under already-flowing water, so this is the real wake-up
	# path (a player edit) and not just the initial pour.
	var wx := sx + SHAFT_DISTANCE
	for i in range(2, SHAFT_DEPTH + 1):
		# Walls one cell out at each level below the mouth...
		for side in [Vector3i(1, 0, 0), Vector3i(-1, 0, 0), Vector3i(0, 0, 1), Vector3i(0, 0, -1)]:
			cm.set_block(wx + side.x, sy - i, sz + side.z, stone)
		await process_frame
	# ...a floor under the shaft...
	cm.set_block(wx, sy - SHAFT_DEPTH - 1, sz, stone)
	await process_frame
	# ...and only then the mouth, cut out of the shelf, so the water above it
	# finds the drop. Cell (wx, sy) held runoff above a solid shelf before this.
	cm.set_block(wx, sy - 1, sz, 0)
	dug[Vector3i(wx, sy - 1, sz)] = true
	var poured := await _settle(sx, sy, sz)
	print("probe: the shaft woke the pool (%d cells now, was %d)" % [poured["count"], count])

	# The top layer settles before the water it poured down the shaft has finished
	# falling — the shaft is below the shelf the settle check watches — so the
	# shaft gets its own bounded wait.
	var falling := await _wait_for_shaft(wx, sy, sz)
	if falling == SHAFT_DEPTH:
		print("probe: the shaft filled with %d falling cells (down beats sideways)" % falling)
	else:
		for i in range(1, SHAFT_DEPTH + 1):
			var id: int = cm.get_block(wx, sy - i, sz)
			if not falling_ids.has(id):
				_fail("the shaft never filled: y-%d holds block %d (%s), not a falling state"
					% [i, id, cm.get_block_name(id) if id > 0 else "air"])
	# The pool plus whatever poured into the shaft: opening the shaft moves water
	# down, not out of the world, so the total must not shrink.
	var settled_cells := _water_cells(sx, sy, sz)
	var total: int = settled_cells.size() + falling
	if total < count:
		_fail("the pool lost water when the shaft opened: %d cells plus %d in the shaft, was %d"
			% [settled_cells.size(), falling, count])

	# --- 4. the flood only ever wrote into air ------------------------------
	var violations := 0
	var violation := ""
	var water_here := 0
	for y in range(sy - SHAFT_DEPTH - 2, sy + 2):
		for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
				var key := Vector3i(sx + dx, y, sz + dz)
				var id: int = cm.get_block(key.x, key.y, key.z)
				if not depth_by_id.has(id) and not falling_ids.has(id):
					continue
				water_here += 1
				var was: int = before.get(key, -2)
				if was == 0 or was == -2 or natural.has(key) or dug.has(key):
					continue   # air before, outside the shelf, or its own water / our own cut
				violations += 1
				if violation == "":
					violation = "%s holds water but held %d (%s)" % [key, was, cm.get_block_name(was)]
	if violations > 0:
		_fail("%d cells hold water they replaced (%s)" % [violations, violation])
	else:
		print("probe: all %d water cells sit in cells that were air beforehand (%d checked too)"
			% [water_here, before.size()])

	if not ok:
		quit(1); return
	print("PROBE PASS")
	quit(0)

# -----------------------------------------------------------------------------

# Every depth must be its OWN height, or a stream renders as one flat sheet of
# tiles. Reads the selection boxes the GAME registry built from
# block_definitions.json (the tests can only see the C++ defaults), so this is
# the assertion that the shape names in the JSON resolve to the right heights.
func _check_water_shapes() -> bool:
	# surface 1 - offset, from the reference's height for a cell at depth d.
	# The ladder: source, then one step lower per depth. The falling column is
	# checked separately — it is meant to be FULL height, not part of the slope.
	var expected := [["water", 0.88], ["water_runoff_1", 0.78], ["water_runoff_2", 0.67],
		["water_runoff_3", 0.56], ["water_runoff_4", 0.44], ["water_runoff_5", 0.33],
		["water_runoff_6", 0.22], ["water_runoff_7", 0.11], ["water_fallen", 1.0]]
	var previous := 2.0
	var report: Array[String] = []
	for pair in expected:
		var descending: bool = pair[0] != "water_fallen"
		var id: int = BlockTextures.get_block_id_by_name(pair[0])
		if id <= 0:
			_fail("%s did not resolve by name" % pair[0])
			return false
		var boxes: Array = cm.get_selection_boxes(id)
		if boxes.size() != 1:
			_fail("%s has %d selection boxes, expected one" % [pair[0], boxes.size()])
			return false
		var height: float = float(boxes[0][4])
		report.append("%s=%.2f" % [pair[0], height])
		if absf(height - float(pair[1])) > 0.001:
			_fail("%s is %.3f tall, expected %.2f" % [pair[0], height, pair[1]])
			return false
		if descending:
			if height > previous:
				_fail("%s (%.2f) is taller than the depth feeding it (%.2f)" % [pair[0], height, previous])
				return false
			previous = height
	print("probe: water heights by depth: %s" % ", ".join(report))
	return true

# Lay the shelf: stone at sy-1, air at sy, over a square that out-reaches the
# flood and its drop search. Every write is a real player-style edit. False means
# a cell did not land — a chunk that has not streamed in yet refuses the write —
# so the caller re-measures and lays again rather than measuring nothing.
func _build_shelf(sx: int, sy: int, sz: int, stone: int) -> bool:
	var writes := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			cm.set_block(sx + dx, sy - 1, sz + dz, stone)
			cm.set_block(sx + dx, sy, sz + dz, 0)
			writes += 2
			if writes % WRITES_PER_FRAME == 0:
				await process_frame
	for i in range(15):
		await process_frame
	# Read back: a queued write that never landed would make every count below a
	# lie, so fail loudly instead of quietly measuring nothing.
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			# Report the first cell that did not land: "the shelf never landed" with
			# no cell named is the least useful message this probe can print.
			var above: int = cm.get_block(sx + dx, sy, sz + dz)
			if above != 0:
				print("probe: shelf cell (%d, %d, %d) is still %d (%s), wanted air"
					% [sx + dx, sy, sz + dz, above, cm.get_block_name(above)])
				return false
			var floor_id: int = cm.get_block(sx + dx, sy - 1, sz + dz)
			if floor_id != stone:
				print("probe: shelf floor (%d, %d, %d) is %d (%s), wanted stone"
					% [sx + dx, sy - 1, sz + dz, floor_id,
					   cm.get_block_name(floor_id) if floor_id > 0 else "air"])
				return false
		await process_frame
	return true

# The same measurement twice, a second apart: the honest test that streaming has
# caught up and the region below is the region we will actually edit.
func _stable_ceiling(sx: int, sz: int) -> int:
	var previous := -1
	for attempt in range(8):
		var ceiling := _terrain_ceiling(sx, sz)
		if ceiling == previous:
			return ceiling
		previous = ceiling
		var until := Time.get_ticks_msec() + 1000
		while Time.get_ticks_msec() < until:
			await process_frame
	return previous

# The highest solid block anywhere in the shelf's footprint, so the shelf can go
# above all of it and every cell it writes into is air.
func _terrain_ceiling(sx: int, sz: int) -> int:
	# Well above the local terrain whatever it does, so the walk down finds the
	# real ceiling rather than stopping at the first block under the player.
	var top := int(player.global_position.y) + 48
	var ceiling := 0
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			var y := top
			while y > 0:
				if cm.get_block(sx + dx, y, sz + dz) != 0:
					ceiling = maxi(ceiling, y)
					break
				y -= 1
	return ceiling

# Wait (bounded) for the shaft to hold a falling cell at every level, and report
# how many it holds. Three seconds at a 50 ms tick is sixty ticks for a three
# block drop — far past the travel time, so a short count means the water is not
# coming, not that it is slow.
func _wait_for_shaft(wx: int, sy: int, sz: int) -> int:
	var deadline := Time.get_ticks_msec() + 3000
	var wet := 0
	while wet < SHAFT_DEPTH and Time.get_ticks_msec() < deadline:
		wet = 0
		for i in range(1, SHAFT_DEPTH + 1):
			if falling_ids.has(cm.get_block(wx, sy - i, sz)):
				wet += 1
		if wet < SHAFT_DEPTH:
			await process_frame
	return wet

# The pour's own water: water-state blocks ON the shelf layer, inside its
# footprint, as {Vector3i: block_id}.
#
# The shelf floor is read back as solid stone across the whole footprint before
# the pour, and the flood reaches at most the diamond, so water below the shelf
# or past its rim cannot be the pour's doing. It can still be there: the world
# around a probe spot holds water of its own (a pond, or runoff an earlier run
# left behind), and an unloaded chunk reads as air — so a cell the pre-pour
# snapshot called empty can turn out to be water once its chunk streams in.
# A wider scan reads that as an escaped flood, which is what used to make this
# probe fail run to run while the flood itself was exactly a diamond.
func _water_cells(sx: int, sy: int, sz: int) -> Dictionary:
	var out := {}
	for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
		for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			var key := Vector3i(sx + dx, sy, sz + dz)
			var id: int = cm.get_block(key.x, key.y, key.z)
			if depth_by_id.has(id) or falling_ids.has(id):
				if not natural.has(key):
					out[key] = id
	return out

# Watch until the water stops changing. Reports the final set, how many cells are
# wet, and how long (ms) nothing changed. Wall-clock, because the simulation
# ticks on elapsed time: counting frames would call a lull between waves "done".
func _settle(sx: int, sy: int, sz: int) -> Dictionary:
	var previous := {}
	var cells := {}
	var quiet_since := Time.get_ticks_msec()
	var start := quiet_since
	while Time.get_ticks_msec() - start < MAX_SETTLE_MS:
		await process_frame
		cells = _water_cells(sx, sy, sz)
		var same := cells.size() == previous.size()
		if same:
			for key in cells:
				if previous.get(key, -1) != cells[key]:
					same = false
					break
		if not same:
			quiet_since = Time.get_ticks_msec()
		previous = cells
		if Time.get_ticks_msec() - quiet_since >= QUIET_MS:
			break
	return { "count": cells.size(), "cells": cells, "quiet": Time.get_ticks_msec() - quiet_since }

func _snapshot(sx: int, sy: int, sz: int) -> Dictionary:
	var out := {}
	for y in range(sy - SHAFT_DEPTH - 2, sy + 2):
		for dz in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
			for dx in range(-SHELF_RADIUS, SHELF_RADIUS + 1):
				out[Vector3i(sx + dx, y, sz + dz)] = cm.get_block(sx + dx, y, sz + dz)
	return out

# Resolve every state the simulation can write, by NAME — the ids differ between
# the JSON the game loads and the C++ defaults the tests run on, which is the
# whole reason the state table resolves names instead of numbering them.
func _resolve_fluid_ids() -> bool:
	for name in ["water", "surface_water"]:
		var src: int = BlockTextures.get_block_id_by_name(name)
		if src > 0:
			depth_by_id[src] = 0
	for d in range(1, 8):
		var id: int = BlockTextures.get_block_id_by_name("water_runoff_%d" % d)
		if id <= 0:
			_fail("water_runoff_%d did not resolve by name" % d); return false
		depth_by_id[id] = d
	var fallen: int = BlockTextures.get_block_id_by_name("water_fallen")
	if fallen <= 0:
		_fail("water_fallen did not resolve by name"); return false
	falling_ids[fallen] = true
	return true
