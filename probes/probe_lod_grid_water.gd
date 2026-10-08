extends SceneTree
## WHERE the far field's water is, and what the sampler answered for a column.
##
## The report: a light blue over the far field's OWN grass and sand, which does not show
## over water because over water that colour is the water. Two things can put it there --
## the mode's water sheet at a height other than the world's sea level, or the land's own
## quads wearing the water's texture layer -- and a frame cannot separate them:
## probes/probe_lod_grid_overlap.gd answers only WHICH surface won a pixel, never at what
## height or from which sample.
##
## So this probe asks the data, through the far grid's own readouts
## (src/lod/lod_grid_debug.cpp):
##
##   * `debug_lod_grid_column(x, z)` is the sampler's answer for one world column, run
##     through the SAME generator a tile build makes -- biome, height, water level,
##     flooded or not, and the surface sample the mesh builder would receive, beside the
##     configured sea level and the level the squish maps it to;
##   * `debug_lod_grid_water()` reads the water vertices back out of the tiles that have
##     been built: how many there are, the range of world Y they sit at, a histogram of
##     that Y rounded to blocks, and the same per spacing level.
##
## The claims, in the report's own terms: every water vertex must sit at the world's own
## sea level, and a column the sampler calls dry must be handed no water at all.
##
## HEADLESS (no frame is read):
##
##   probes/run_probe.sh probes/probe_lod_grid_water.gd 600

const WORLD_SEED := 1337
const RENDER_DISTANCE := 4
const SPACING := 128
## The reach the report comes from: the user's own setting, so the readout is of the same
## far field the frames with the light blue over land are of (100 rings = 25.6 km).
const OUTER_RINGS := 100
const WAIT_FRAMES := 9000
## the columns asked by hand: the spawn's own, the four quadrants and the diagonals of
## the innermost rings, and a few at the outer level's spacing
const COLUMNS := [
	[0, 0], [128, 0], [0, 128], [128, 128],
	[2048, 0], [0, 2048], [2048, 2048], [5120, 5120],
	[-2048, 0], [0, -2048], [-5120, 5120], [12288, 0],
]

var _failures := 0
var _cm: Node = null


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-52s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


func _frames(count: int) -> void:
	for _i in count:
		await process_frame


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	_cm = main.get_node_or_null("ChunkManager")
	if _cm == null:
		_ok("ChunkManager exists", false)
		_finish()
		return

	_cm.set("seed", WORLD_SEED)
	_cm.set("render_distance", RENDER_DISTANCE)
	_cm.set("day_time", 0.5)
	_cm.set("day_night_cycle_enabled", false)
	_cm.set("lod_grid_enabled", false)
	_cm.set("lod_grid_spacing", SPACING)
	_cm.set("lod_grid_outer_rings", OUTER_RINGS)
	await _frames(60)

	# The sampler's own answers, before any tile exists: the water level a column is
	# given, and whether that column is flooded at all.
	var samples := {}
	for column in COLUMNS:
		var info: Dictionary = _cm.call("debug_lod_grid_column", int(column[0]), int(column[1]))
		samples[int(column[0]) * 100000 + int(column[1])] = info
		print("probe: column (%6d,%6d)  biome %d  height %8.1f  water %8.1f  flooded %s "
			% [int(column[0]), int(column[1]), int(info["biome"]),
				float(info["column_height"]), float(info["water_level"]),
				str(info["flooded"])])
		print("probe:                 surface_y %5d  sample height %7.1f water %8.1f layer %3d"
			% [int(info["surface_y"]), float(info["sample_height"]),
				float(info["sample_water"]), int(info["sample_layer"])])

	var sea := float(samples[0]["effective_sea_level"])
	var configured := float(samples[0]["sea_level"])
	print("probe: seed %d  sea_level %.1f  effective %.1f  squish %s (slice %d span %d)"
		% [WORLD_SEED, configured, sea, str(samples[0]["squish_enabled"]),
			int(samples[0]["squish_slice"]), int(samples[0]["squish_span"])])

	# The world's own water, for comparison: the sampler's level must be the level the
	# loaded world fills to, or the far field's sea is not the world's sea.
	var world_level := _world_water_top()
	print("probe: the loaded world's own water surface at (0,0): %d" % world_level)

	_cm.set("lod_grid_enabled", true)
	var stats := {}
	for _i in WAIT_FRAMES:
		await process_frame
		stats = _cm.get_lod_grid_stats()
		var live := int(stats.get("tiles", 0))
		if live > 0 and int(stats.get("uploads", 0)) >= live:
			break
	_ok("the far field built its tiles", int(stats.get("uploads", 0)) > 0,
		"%d of %d tiles, %d quads" % [int(stats.get("uploads", 0)),
			int(stats.get("tiles", 0)), int(stats.get("quads", 0))])

	var water: Dictionary = _cm.call("debug_lod_grid_water")
	print("probe: tiles with geometry %d, with water %d, water vertices %d, terrain vertices %d"
		% [int(water["tiles_with_geometry"]), int(water["tiles_with_water"]),
			int(water["water_vertices"]), int(water["terrain_vertices"])])
	print("probe: water Y   min %.1f  max %.1f      terrain Y  min %.1f  max %.1f"
		% [float(water["water_min_y"]), float(water["water_max_y"]),
			float(water["terrain_min_y"]), float(water["terrain_max_y"])])
	print("probe: eye Y %.1f, clip %.0f blocks, reach %.0f blocks, layers water %d underwater %d"
		% [float(water["player_y"]), float(water["inner_radius_blocks"]),
			float(water["outer_radius_blocks"]), int(water["water_layer"]),
			int(water["underwater_layer"])])
	var histogram: Dictionary = water["histogram"]
	var heights := histogram.keys()
	heights.sort()
	print("probe: the water vertices' own heights, rounded to blocks (Y: count):")
	for y in heights:
		print("probe:    y %6d  %7d vertex/vertices" % [int(y), int(histogram[y])])
	var levels: Dictionary = water["levels"]
	for level in levels.keys():
		var entry: Dictionary = levels[level]
		print("probe:    level %d  %6d quads  tile Y %6.1f .. %6.1f"
			% [int(level), int(entry["water_quads"]), float(entry["min_y"]),
				float(entry["max_y"])])

	# The claim, in the report's own terms: the mode's water is at the world's sea level
	# and nowhere else. A sheet that has taken the terrain's own heights is the light blue
	# over far grass and sand, and it shows up here as a range rather than one number.
	var n_water := int(water["water_vertices"])
	_ok("every water vertex sits at the world's own sea level",
		n_water > 0 and float(water["water_min_y"]) == sea and float(water["water_max_y"]) == sea,
		"%d vertices, Y %.1f .. %.1f, sea level %.1f" % [n_water,
			float(water["water_min_y"]), float(water["water_max_y"]), sea])
	# ...and the sampler's own level is the loaded world's, not a second one. The world's
	# side of the comparison is a scan of the LOADED chunks (the helper above), so a run
	# whose loaded area has no ocean in it has nothing to compare with: it says so rather
	# than reporting a check it did not make.
	if world_level < 0:
		print("probe: NOTE no water block in the scanned columns, so the sampler's level was")
		print("probe:      compared with the configured one only (%.1f)" % sea)
	else:
		_ok("the sampler's water level is the loaded world's water level",
			sea == float(world_level),
			"sampler %.1f, the world's own water surface %d" % [sea, world_level])
	# A dry column must be handed no water, which is what keeps a sheet off the land.
	var wet_columns := 0
	var dry_with_water := 0
	for key in samples.keys():
		var info: Dictionary = samples[key]
		if bool(info["flooded"]):
			wet_columns += 1
		elif float(info["sample_water"]) > -1.0e8:
			dry_with_water += 1
	_ok("a column the sampler calls dry is handed no water",
		dry_with_water == 0,
		"%d of %d asked columns dry and wet, %d dry with a water level" % [
			len(samples) - wet_columns, wet_columns, dry_with_water])

	_cm.set("lod_grid_enabled", false)
	print("PROBE lod grid water: %d failures" % _failures)
	_finish()


## The top of the loaded world's own water at (0, z): the highest y whose block is a
## liquid, read through the world rather than through the far mode's sampler -- the two
## must agree on where the sea is.
## The topmost water block in a column, read out of the loaded world. `get_block` takes
## the world coordinates and answers a block ID, which is then resolved by NAME through
## the registry's own id -> name call -- `get_block_name` takes a block ID and NOT a
## position, so passing it (x, y, z) is an error the engine reports as a failed call, and
## the 0 it answers with reads as a water level of zero rather than as a broken helper.
## Widened from three far-apart columns to the loaded square: at this seed's spawn the
## first version scanned three columns and found no water at all, which made the check
## below pass without comparing anything. The scan is over the chunks the world has (about
## 224 blocks here), 32 blocks apart.
func _world_water_top() -> int:
	var found := {}
	for x in range(-128, 129, 32):
		for z in range(-128, 129, 32):
			for y in range(600, 0, -1):
				var block_id := int(_cm.call("get_block", x, y, z))
				if block_id <= 0:
					continue
				if String(_cm.call("get_block_name", block_id)).contains("water"):
					found[y] = int(found.get(y, 0)) + 1
					break
	if found.is_empty():
		return -1
	var best := -1
	for y in found.keys():
		if best < 0 or int(found[y]) > int(found[best]):
			best = int(y)
	return best


func _finish() -> void:
	quit(1 if _failures > 0 else 0)
