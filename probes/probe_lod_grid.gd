extends SceneTree
## Does the seed-grid far mode do what its settings row promises?
##
## The mode (docs/lod-modes.md) is a second kind of LOD: instead of meshing the
## world more coarsely, it samples the world SEED on a grid and draws the surface
## from the samples, beyond the loaded world, with no chunk behind it. This probe
## checks the halves that can be checked without eyes:
##
##   A. the setting -- the default is OFF, the properties round-trip, and the stats
##      a caller reads describe a mode that is on with a horizon further out than
##      the loaded world.
##   B. the pipeline -- with the mode on and the manager in the tree, tiles are
##      actually sampled (columns_sampled > 0), built on the pool and uploaded
##      (quads > 0), and switching the mode off takes every tile away again.
##      That is the whole path: setting -> controller -> worker -> instance.
##   C. the seam -- the far field's inner boundary is the loaded world's DISC, so
##      the tiles that straddle its edge have to exist (straddling_tiles) and the
##      clip that keeps them from drawing over the world has to be armed (clip_radius
##      on the material), just inside the world's own radius: the world's chunk
##      staircase can end a few blocks short of the radius it streams with, and a
##      clip exactly on that radius leaves a sliver of ground that neither side
##      draws. Both were missing when the boundary was a hole in the tile set: the
##      hole was a square and the world a disc, so four wedges of ground belonged to
##      neither.
##   D. the reach -- the rings the reach adds are BUILT, not merely reported. A
##      horizon the stats moved while the ground under it was never drawn is exactly
##      what the coarser spacings did: their outer levels' tiles were refused by the
##      builder (a spacing that does not divide a tile), the mode counted them as
##      built, and `Far Reach` hauled the horizon out over nothing. The same section
##      reads the horizon and the outermost spacing back at every spacing, because
##      `Far Grid Spacing` is a detail setting: the outermost level is one quad on a
##      tile wide at every base, so it must not move how far the mode sees.
##
## What it cannot check is what the far field LOOKS like; that is a screenshot, not
## an assertion -- probes/probe_lod_grid_shot.gd measures that. What it deliberately does not check is generation: the mode must
## never change the world it is drawn beside, and the cheapest way to keep that
## true is to keep it out of its way entirely (it owns only the ring beyond
## set_inner_radius_blocks).
##
##   probes/run_probe.sh probes/probe_lod_grid.gd 300
##
## run_probe.sh snapshots and restores user://chunks; a plain headless run of this
## probe generates a few chunks around the origin and does not.

const FRAMES := 300
## Small enough that the loaded world is a couple of chunks, so what the probe is
## waiting for is the grid and not the streaming.
const RENDER_DISTANCE := 2
const TOL := 0.001

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

	# --- A. the setting, before any world exists ---------------------------
	_ok("setting: the far mode is off out of the box", not bool(cm.get("lod_grid_enabled")),
		"default is %s" % str(cm.get("lod_grid_enabled")))
	_ok("setting: the default spacing is 32 blocks", int(cm.get("lod_grid_spacing")) == 32,
		"%d" % int(cm.get("lod_grid_spacing")))
	_ok("setting: two rings per level by default", int(cm.get("lod_grid_rings")) == 2,
		"%d" % int(cm.get("lod_grid_rings")))

	var off_stats: Dictionary = cm.get_lod_grid_stats()
	_ok("off: nothing is live and nothing was built",
		int(off_stats.get("tiles", -1)) == 0 and int(off_stats.get("built", -1)) == 0,
		"tiles=%s built=%s" % [str(off_stats.get("tiles")), str(off_stats.get("built"))])
	_ok("off: the stats dictionary has every key a caller reads",
		off_stats.has("enabled") and off_stats.has("quads") and off_stats.has("columns_sampled")
			and off_stats.has("outer_radius") and off_stats.has("spacing")
			and off_stats.has("outer_spacing")
			and off_stats.has("straddling_tiles") and off_stats.has("clip_radius"),
		str(off_stats.keys().size()) + " keys")

	# The world's own material, which the mode's fog range has to move as well: the
	# world's fog is tuned to the loaded radius, so with the mode on the loaded
	# chunks would fade out at their border while the field continuing them did not.
	var world_material = load("res://materials/voxel_material.tres")
	_ok("fog: the world's terrain material loads", world_material != null,
		"res://materials/voxel_material.tres")
	var fog_off: float = float(world_material.get_shader_parameter("fog_end")) if world_material else -1.0

	cm.set("lod_grid_enabled", true)
	_ok("setting: it flips on", bool(cm.get("lod_grid_enabled")))

	var on_stats: Dictionary = cm.get_lod_grid_stats()
	_ok("on: the stats agree the mode is enabled", bool(on_stats.get("enabled")))
	_ok("on: the horizon is beyond the loaded world",
		int(on_stats.get("outer_radius", 0)) > int(cm.get("render_distance")) * 32,
		"outer=%d blocks vs loaded %d" % [int(on_stats.get("outer_radius", 0)),
			int(cm.get("render_distance")) * 32])
	_ok("on: the stats report the spacing the sampler will use",
		int(on_stats.get("spacing", 0)) == int(cm.get("lod_grid_spacing")),
		"%s" % str(on_stats.get("spacing")))

	cm.set("lod_grid_spacing", 64)
	_ok("setting: spacing round-trips", int(cm.get("lod_grid_spacing")) == 64)
	cm.set("lod_grid_spacing", 32)

	# The reach: 0 means "the level ladder's own", and every ring past it is one
	# more ring of the OUTERMOST level -- a tile there is a single quad at 256-block
	# spacing, which is what makes this the knob the horizon can be pushed with
	# rather than the spacing. Its size is the horizon the stats report, so the
	# setting and the geometry are checked against each other, not against a table.
	_ok("setting: the reach starts at the ladder's own", int(cm.get("lod_grid_outer_rings")) == 0,
		"%d" % int(cm.get("lod_grid_outer_rings")))
	var ladder_reach := int(cm.get_lod_grid_stats().get("outer_radius", 0))
	cm.set("lod_grid_outer_rings", 5)
	var long_reach := int(cm.get_lod_grid_stats().get("outer_radius", 0))
	_ok("setting: the reach round-trips", int(cm.get("lod_grid_outer_rings")) == 5)
	# Five rings of the outer level against the two the ladder gives: three more
	# tiles of 256 blocks, and not one block of it at a finer spacing.
	_ok("reach: three extra rings push the horizon out by three tiles",
		long_reach - ladder_reach == 3 * 256 and int(cm.get("lod_grid_spacing")) == 32,
		"%d -> %d blocks" % [ladder_reach, long_reach])
	cm.set("lod_grid_outer_rings", 0)
	_ok("reach: and the ladder is where it was",
		int(cm.get_lod_grid_stats().get("outer_radius", 0)) == ladder_reach)
	# The far end of the slider: 100 rings of the outermost level is 25 km of
	# horizon, and the property has to accept it rather than clamping to a number
	# nobody asked for.
	cm.set("lod_grid_outer_rings", 100)
	var deep_reach := int(cm.get_lod_grid_stats().get("outer_radius", 0))
	_ok("reach: the top of the slider is past 25 km",
		int(cm.get("lod_grid_outer_rings")) == 100 and deep_reach >= 25000,
		"%d blocks (%.1f km)" % [deep_reach, float(deep_reach) / 1000.0])
	cm.set("lod_grid_outer_rings", 0)

	# The spacing only builds at values that DIVIDE a 256-block tile. The rest make the
	# builder refuse the tile -- silently: it comes back empty, the mode counts it as
	# built, and that level's ground is simply not drawn, which is why this row used to
	# look like it had four working values. A probe is the only place that shows up, so
	# every slider step is asked for and read back.
	for pair in [[24, 16], [40, 32], [96, 64], [100, 128], [7, 8], [8, 8], [128, 128]]:
		cm.set("lod_grid_spacing", int(pair[0]))
		_ok("spacing: a request of %d blocks builds at %d" % [int(pair[0]), int(pair[1])],
			int(cm.get("lod_grid_spacing")) == int(pair[1]),
			"set %d -> %d" % [int(pair[0]), int(cm.get("lod_grid_spacing"))])
	cm.set("lod_grid_spacing", 32)
	_ok("spacing: and the default is one of them", int(cm.get("lod_grid_spacing")) == 32)

	# ...and the spacing must NOT move the horizon. A level's spacing is the base
	# doubled, capped at the 256-block tile, and the OUTERMOST level is the tile size
	# whatever the base -- so every setting sees the same distance and the reach row
	# is the one that changes it. Before the cap the outer levels of a base of 64 or
	# more were refused by the builder (an empty tile, counted as built), the reach
	# slider moved nothing there, and 32 was the only setting that saw far. Both
	# numbers are read off the mode rather than from a table.
	var spacing_baseline := int(cm.get_lod_grid_stats().get("outer_radius", -1))
	for spacing in [8, 16, 32, 64, 128]:
		cm.set("lod_grid_spacing", spacing)
		var spacing_stats: Dictionary = cm.get_lod_grid_stats()
		_ok("spacing: base %d still puts one quad on a tile at the horizon" % spacing,
			int(spacing_stats.get("outer_spacing", 0)) == 256,
			"outermost spacing %s blocks" % str(spacing_stats.get("outer_spacing")))
		_ok("spacing: base %d reaches the same horizon as the default" % spacing,
			int(spacing_stats.get("outer_radius", -1)) == spacing_baseline,
			"%s vs %d blocks" % [str(spacing_stats.get("outer_radius")), spacing_baseline])
	cm.set("lod_grid_spacing", 32)

	# The material and shader the tiles are drawn with, loaded the way the engine
	# loads them: a missing one would leave every tile invisible and no assertion
	# above would notice.
	var material = load("res://materials/lod_grid_material.tres")
	_ok("material: the far grid's material loads", material != null and material is ShaderMaterial)
	if material is ShaderMaterial:
		_ok("material: it has the shader the tiles are lit by", material.shader != null,
			str(material.shader.resource_path) if material.shader else "no shader")
		# The fog range matters more than it looks: the world's fog is tuned to the
		# loaded radius, so a far tile drawn through it would be fogged out at the
		# near edge and the whole mode would be invisible. "Unset" must therefore
		# still be a range, not zero.
		var fog_begin: float = float(material.get_shader_parameter("fog_begin"))
		var sky: float = float(material.get_shader_parameter("sky_light_intensity"))
		_ok("material: the sky uniforms the controller pushes are present",
			sky >= 0.0 and sky <= 1.0 and fog_begin > 0.0,
			"sky=%s fog_begin=%s" % [str(sky), str(fog_begin)])

	# --- B. the pipeline, with the manager in the tree ---------------------
	root.add_child(cm)
	cm.set("render_distance", RENDER_DISTANCE)
	cm.set("auto_update", true)
	cm.set("player_position", Vector3(0.0, 320.0, 0.0))
	cm.set("lod_grid_enabled", true)

	var built := 0
	var uploads := 0
	var columns := 0
	var quads := 0
	var outer := 0
	for i in FRAMES:
		await process_frame
		var stats: Dictionary = cm.get_lod_grid_stats()
		built = int(stats.get("built", 0))
		uploads = int(stats.get("uploads", 0))
		columns = int(stats.get("columns_sampled", 0))
		quads = int(stats.get("quads", 0))
		outer = int(stats.get("outer_radius", 0))
		# A RING, not a tile. The mode drew exactly one visible mesh once -- every
		# tile but the first was placed at double its offset -- and a probe that
		# settles for four uploads would have called that a pass. The innermost ring
		# alone is eight tiles, so wait past it.
		if uploads >= 12:
			break

	_ok("pipeline: a ring of tiles was built and uploaded, not one mesh",
		uploads >= 8 and built >= 8, "uploads=%d built=%d" % [uploads, built])
	_ok("pipeline: the sampler ran, and the count is the cost model's unit", columns > 0,
		"%d columns sampled" % columns)
	_ok("pipeline: geometry came out of it", quads > 0, "%d quads" % quads)
	# Draw calls are the number this mode is judged on in this engine, and they are
	# a property of how the tiles are BATCHED, not of how many there are: one call
	# per spacing level that has geometry, whatever the tile count.
	var batch: Dictionary = cm.get_lod_grid_stats()
	var calls := int(batch.get("draw_calls", -1))
	var verts := int(batch.get("vertices", -1))
	_ok("pipeline: the far field costs one draw call per level, not one per tile",
		calls >= 1 and calls <= 4 and calls * 8 < int(batch.get("tiles", 1)),
		"%d draw calls, %d vertices, %s tiles" % [calls, verts, str(batch.get("tiles"))])
	_ok("pipeline: the vertices are the ones the tiles hold",
		verts > 0 and verts == int(batch.get("quads", 0)) * 6,
		"%d vertices for %s quads" % [verts, str(batch.get("quads"))])
	_ok("pipeline: the horizon it covers is the one it reports", outer > RENDER_DISTANCE * 32,
		"%d blocks" % outer)

	# --- C. the seam with the loaded world ---------------------------------
	# The far field starts where the world's DISC ends while the tiles are a square
	# lattice, so the tiles that straddle the disc's edge are the only geometry that
	# can close the corners of the lattice the disc cannot reach. When the boundary
	# was a hole in the tile set instead, this count was zero and four wedges of
	# ground -- out to 1.41x the world's radius -- were drawn by neither side.
	var seam: Dictionary = cm.get_lod_grid_stats()
	_ok("seam: the tiles straddling the loaded world's edge exist",
		int(seam.get("straddling_tiles", 0)) >= 1,
		"%s straddling of %s tiles" % [str(seam.get("straddling_tiles")), str(seam.get("tiles"))])
	_ok("seam: the far field is clipped at the loaded world's own radius",
		int(seam.get("clip_radius", -1)) == RENDER_DISTANCE * 32,
		"clip %s blocks vs loaded %d" % [str(seam.get("clip_radius")), RENDER_DISTANCE * 32])
	var armed: float = float(material.get_shader_parameter("clip_radius")) if material else -1.0
	_ok("seam: the material carries the clip, inside the world's edge rather than on it",
		int(armed) > 0 and int(armed) <= RENDER_DISTANCE * 32 and
		int(armed) >= RENDER_DISTANCE * 32 - 32,
		"material clip_radius=%s (world %d)" % [str(armed), RENDER_DISTANCE * 32])

	# And the world wears the far mode's fog range, so the two halves of the same
	# terrain cannot disagree about distance at the one place they meet.
	var fog_on: float = float(world_material.get_shader_parameter("fog_end")) if world_material else -1.0
	_ok("fog: the world's terrain is fogged out to the far field's own horizon",
		int(fog_on) == int(seam.get("outer_radius", -1)) and fog_on > fog_off,
		"world fog_end %s -> %s, far field reaches %s" %
			[str(fog_off), str(fog_on), str(seam.get("outer_radius"))])

	# Every wanted tile, not just a ring of them. A tile whose build comes back with
	# no geometry never uploads and is retried forever, so the counts that look
	# healthy -- tiles live, a ring up, columns sampled -- stay healthy while that
	# piece of the ground is simply not drawn. "There should be a mesh every four
	# areas and there is one" is what this measures.
	var live := 0
	var uploaded := 0
	var failed := 0
	for i in 900:
		await process_frame
		var s: Dictionary = cm.get_lod_grid_stats()
		live = int(s.get("tiles", 0))
		uploaded = int(s.get("uploads", 0))
		failed = int(s.get("failed", 0))
		# A trace, because the failure mode here is a number that stops rising: the
		# wanted set, what has come back, what was dropped, and what the sampler saw.
		if i % 150 == 0:
			print("probe: frame %3d  live=%d uploads=%d failed=%d dropped=%d columns=%s" % [
				i, live, uploaded, failed, int(s.get("dropped", 0)),
				str(s.get("columns_sampled"))])
		if uploaded + failed >= live:
			break
	_ok("pipeline: every wanted tile is accounted for, not just a ring of them",
		uploaded + failed >= live, "uploads=%d failed=%d live=%d" % [uploaded, failed, live])
	_ok("pipeline: no tile came back without geometry, which would be a hole in the ground",
		failed == 0, "%d of %d tiles failed" % [failed, live])

	# --- D. the reach BUILDS, not just reports ------------------------------
	# A reach that only moves a number is exactly the bug this was: with the old
	# uncapped ladder the coarser settings' outer levels came back empty (a refused
	# tile, counted as built), so `Far Reach` hauled the horizon past ground nobody
	# had drawn. So push the reach out and wait for the WANTED SET, then read the
	# tile count against the ladder's own: the extra band has to exist, and its
	# horizon has to be the one the ring count implies.
	cm.set("lod_grid_outer_rings", 0)
	await process_frame
	var ladder_now := int(cm.get_lod_grid_stats().get("outer_radius", 0))
	cm.set("lod_grid_outer_rings", 6)
	var reach_live := 0
	var reach_answered := 0
	for i in 900:
		await process_frame
		var r: Dictionary = cm.get_lod_grid_stats()
		reach_live = int(r.get("tiles", 0))
		reach_answered = int(r.get("uploads", 0)) + int(r.get("failed", 0))
		if reach_answered >= reach_live and i > 4:
			break
	_ok("reach: the extra rings are built, not just reported",
		reach_live > live and reach_answered >= reach_live,
		"%d tiles (ladder %d), %d answered" % [reach_live, live, reach_answered])
	_ok("reach: and the horizon moved by exactly the rings asked for",
		int(cm.get_lod_grid_stats().get("outer_radius", 0)) == ladder_now + 4 * 256,
		"%d -> %s blocks" % [ladder_now, str(cm.get_lod_grid_stats().get("outer_radius"))])
	cm.set("lod_grid_outer_rings", 0)

	# Off means gone: the setting is not a fade or a hide, it is "no tile exists".
	cm.set("lod_grid_enabled", false)
	await process_frame
	await process_frame
	var after: Dictionary = cm.get_lod_grid_stats()
	_ok("off again: every tile is gone", int(after.get("tiles", -1)) == 0,
		"%s tiles remain" % str(after.get("tiles")))
	_ok("off again: the mode says so", not bool(after.get("enabled")))

	# A world reset must take the tiles with it, or they belong to the old seed.
	cm.set("lod_grid_enabled", true)
	await process_frame
	await process_frame
	cm.set("lod_grid_enabled", false)
	_ok("reset: dropping the mode mid-build leaves nothing in flight",
		int(cm.get_lod_grid_stats().get("tiles", -1)) == 0)

	print("PROBE lod grid: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
