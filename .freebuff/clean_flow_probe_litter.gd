extends SceneTree
## One-shot cleanup for the litter a probe run leaves in the real world when it is
## run WITHOUT run_probe.sh (which snapshots and restores the world): stone
## shelves above spawn, the fluid that poured off them, and the fluid still
## creeping underground.
##
## Two things identify that litter, and neither can touch generated terrain:
##
##   * DYNAMIC fluid — the states the simulation writes: water + water_runoff_* +
##     water_fallen, lava + lava_runoff_1..3 + lava_fallen, and acid +
##     acid_runoff_1..7 + acid_fallen. Generated water is never one of them:
##     worldgen emits surface_water for a sea, a lake and a cave pool alike (see
##     chunk_generator.cpp). Whatever holds one of those ids here was poured by a
##     probe or produced by the flow, so every one of them is litter.
##   * Floating stone above y=255 in the spawn column: the natural terrain there
##     tops out at 254 (measured by the first probe run, before any of this), and
##     nothing generated floats. Checked afterwards with `.freebuff/scan_footprint.gd`,
##     which lists every non-air block in that footprint — the shelves and the two
##     acid floods are the only things in it, and there is no hand-built block.
##
## It sweeps repeatedly: removing water wakes the simulation, and a source deeper
## in the puddle keeps feeding new runoff until it has been removed too.
##
## Run: Godot --headless --path <project> --script res://.freebuff/clean_flow_probe_litter.gd
## (Run it directly, NOT through run_probe.sh: its whole job is to persist the fix.)

# The litter is NOT centred on the spawn column: it was made by the previous
# session's runs, from wherever the player stood then. Measured with
# `.freebuff/scan_fluid_box.gd`: the dynamic water sits at x -120..-82,
# z -5..34, y 252..262, with its two sources at x=-120 and x=-97 — outside the
# ±96 box a first sweep used, which is exactly why 53 cells of it kept coming
# back after every pass. So the search starts wide and then SHRINKS to what it
# found plus a margin: a source spreads at most seven cells, so twelve is enough
# to catch whatever feeds the cells that were just removed.
const SEARCH_X0 := -200
const SEARCH_X1 := 60
const SEARCH_Z0 := -60
const SEARCH_Z1 := 90
const SEARCH_Y0 := 240
const SEARCH_Y1 := 275
const MARGIN := 12
const SHELF_X := 0             # the spawn column the shelf runs used
const SHELF_Z := 0
const SHELF_RADIUS := 14
const SHELF_BOTTOM := 255      # natural terrain there tops out at 254
const PASSES := 12
const SETTLE_BETWEEN_PASSES_MS := 2000
const SAVE_MS := 9000          # the edit maps save on a timer; wait for it

var cm: Node
var dynamic := {}
var search_x0 := SEARCH_X0
var search_x1 := SEARCH_X1
var search_z0 := SEARCH_Z0
var search_z1 := SEARCH_Z1
var search_y0 := SEARCH_Y0
var search_y1 := SEARCH_Y1

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("CLEAN FAIL: main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)

	cm = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("CLEAN FAIL: ChunkManager missing"); quit(1); return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 60:
			break

	# Dynamic fluid only. surface_water is deliberately absent: that is what the
	# generator writes, so clearing it would carve holes in real seas and lakes.
	dynamic[BlockTextures.get_block_id_by_name("water")] = "water"
	for d in range(1, 8):
		dynamic[BlockTextures.get_block_id_by_name("water_runoff_%d" % d)] = "runoff_%d" % d
	dynamic[BlockTextures.get_block_id_by_name("water_fallen")] = "water_fallen"
	for substance in ["lava", "acid"]:
		dynamic[BlockTextures.get_block_id_by_name(substance)] = substance
		for d in range(1, 8):
			dynamic[BlockTextures.get_block_id_by_name("%s_runoff_%d" % [substance, d])] = \
				"%s_runoff_%d" % [substance, d]
		dynamic[BlockTextures.get_block_id_by_name("%s_fallen" % substance)] = "%s_fallen" % substance
	dynamic.erase(0)

	for pass_index in range(PASSES):
		# Collect first, then remove SOURCES BEFORE RUNOFF. That ordering is the
		# whole trick: the flow simulation only ticks between frames, but this loop
		# yields while it removes, so a cell the flow can reach with two surviving
		# source neighbours becomes a source itself — which is exactly how a puddle
		# of paired sources survives being swept cell by cell forever (13 sources
		# held a pool through eleven passes before this). Take the sources out
		# first and there is no pair left to make a third.
		var doomed: Array = []
		var sources := 0
		var blocks := 0
		for y in range(search_y0, search_y1 + 1):
			for dz in range(search_z0, search_z1 + 1):
				for dx in range(search_x0, search_x1 + 1):
					var id: int = cm.get_block(dx, y, dz)
					if id == 0:
						continue
					var name: String = String(dynamic.get(id, ""))
					var is_shelf: bool = name == "" && y >= SHELF_BOTTOM \
						&& absi(dx - SHELF_X) <= SHELF_RADIUS && absi(dz - SHELF_Z) <= SHELF_RADIUS
					if name == "" and not is_shelf:
						continue
					var is_source: bool = name != "" && not name.contains("runoff") && not name.contains("fallen")
					if is_source:
						sources += 1
					if is_shelf:
						blocks += 1
					doomed.append({"x": dx, "y": y, "z": dz, "source": is_source})
		# Sources to the front, then everything is removed in that order.
		doomed.sort_custom(func(a, b): return a["source"] and not b["source"])
		var removed := 0
		for cell in doomed:
			cm.set_block(cell["x"], cell["y"], cell["z"], 0)
			removed += 1
			if removed % 40 == 0:
				await process_frame
		var water: int = doomed.size() - blocks
		print("clean: pass %d removed %d dynamic fluid cells (%d of them sources, first) and %d floating blocks"
			% [pass_index, water, sources, blocks])
		if water == 0 and blocks == 0:
			break
		# Shrink the search to the neighbourhood of what was just removed.
		for cell in doomed:
			search_x0 = maxi(search_x0, cell["x"] - MARGIN)
			search_x1 = mini(search_x1, cell["x"] + MARGIN)
			search_z0 = maxi(search_z0, cell["z"] - MARGIN)
			search_z1 = mini(search_z1, cell["z"] + MARGIN)
			search_y0 = maxi(search_y0, cell["y"] - MARGIN)
			search_y1 = mini(search_y1, cell["y"] + MARGIN)
		var until := Time.get_ticks_msec() + SETTLE_BETWEEN_PASSES_MS
		while Time.get_ticks_msec() < until:
			await process_frame

	# Hold the world open so the edit-map autosave takes the removals.
	var save_until := Time.get_ticks_msec() + SAVE_MS
	while Time.get_ticks_msec() < save_until:
		await process_frame

	# The readback covers the whole ORIGINAL region, not the shrunk one: a pool
	# the shrink walked away from would otherwise read as clean.
	var left_water := 0
	var left_blocks := 0
	for y in range(SEARCH_Y0, SEARCH_Y1 + 1):
		for dz in range(SEARCH_Z0, SEARCH_Z1 + 1):
			for dx in range(SEARCH_X0, SEARCH_X1 + 1):
				var id: int = cm.get_block(dx, y, dz)
				if dynamic.has(id):
					left_water += 1
				elif id != 0 && y >= SHELF_BOTTOM && absi(dx - SHELF_X) <= SHELF_RADIUS \
						&& absi(dz - SHELF_Z) <= SHELF_RADIUS:
					left_blocks += 1
	print("clean: final readback — %d dynamic fluid cells, %d floating blocks left"
		% [left_water, left_blocks])
	if left_water > 0 or left_blocks > 0:
		print("CLEAN FAIL")
		quit(1)
		return
	print("CLEAN PASS")
	quit(0)
