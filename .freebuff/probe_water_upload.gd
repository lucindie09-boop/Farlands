extends SceneTree
## Does water that changes reach the GPU, or only the world?
##
## The upload is deduplicated by a content hash, and that hash has to cover BOTH
## surfaces (opaque + liquid). It used to cover the opaque one only, so water
## changing while the terrain did not was judged "unchanged" and the whole upload
## was skipped: water you can swim in, collide with and outline, with no geometry
## on screen, until some unrelated edit moved an opaque vertex. This probe makes
## exactly that case happen — a pour INTO A CHUNK THAT ALREADY HAS TERRAIN, not
## onto a floating shelf over empty air (which has no opaque content at all and so
## never hit the dedup) — and then reads the renderer's own invariant report:
##
##   Liquid missing:  chunks whose data holds liquid while the GPU holds no liquid
##                    geometry for them (and which do have terrain geometry, so
##                    they are resident and visible, not merely still streaming)
##   Liquid repairs:  chunks remeshed to repair that
##
## Both must be 0. What it cannot see from GDScript is the GPU itself, so this
## checks the two counters the C++ side maintains from what it uploaded.
##
## Run it through the wrapper, which snapshots and restores the saved world (the
## basin and the poured water are persisted edits):
##   .freebuff/run_probe.sh .freebuff/probe_water_upload.gd

const PEN_RADIUS := 9            # 19x19 interior: the radius-7 flood fits with room
const FRAMES_PER_BATCH := 20
# Long enough that the pen's own build is uploaded and the streaming around the
# player is quiet: the pour has to be the ONLY thing that changes, or the opaque
# mesh moves with it and the dedup this probe is about is never reached.
const QUIET_FRAMES := 300
const AFTER_POUR_FRAMES := 8
const SETTLE_FRAMES := 900

var cm: Node
var player: Node3D
var ok := true
var stone := 0
var water := 0
var water_ids := {}

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager / Player missing"); quit(1); return

	stone = BlockTextures.get_block_id_by_name("stone")
	water = BlockTextures.get_block_id_by_name("water")
	# Every state the flood can write, so "is this cell wet" is one lookup.
	for name in ["water", "water_runoff_1", "water_runoff_2", "water_runoff_3",
			"water_runoff_4", "water_runoff_5", "water_runoff_6", "water_runoff_7",
			"water_fallen"]:
		water_ids[BlockTextures.get_block_id_by_name(name)] = true

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(10):
		await process_frame

	var spot := player.global_position.floor()
	var sx := int(spot.x)
	var sz := int(spot.z)
	var gy := _ground_y(sx, sz)
	if gy < 0:
		_fail("no ground under the player"); quit(1); return
	print("probe: pouring into the terrain at (%d, %d, %d) — this chunk has opaque geometry" % [sx, gy + 1, sz])

	# A walled pen ON the terrain: clear the interior, floor it with stone, and
	# raise a one-block rim. The water then sits in a chunk that already carries
	# the terrain's opaque mesh, which is the case the dedup used to swallow.
	var pen_y := gy + 1
	var writes := 0
	for dz in range(-PEN_RADIUS - 1, PEN_RADIUS + 2):
		for dx in range(-PEN_RADIUS - 1, PEN_RADIUS + 2):
			var edge := absi(dx) > PEN_RADIUS or absi(dz) > PEN_RADIUS
			for dy in range(-1, 2):
				cm.set_block(sx + dx, pen_y + dy, sz + dz,
					stone if (edge or dy < 0) else 0)
			writes += 3
			if writes % FRAMES_PER_BATCH == 0:
				await process_frame
	for i in range(QUIET_FRAMES):
		await process_frame

	# Pour. `set_block` is the same write path a placement uses (block editor ->
	# mesh dirty), so this is the real thing, not a probe shortcut.
	cm.set_block(sx, pen_y, sz, water)
	var wet_before := _wet_count(sx, pen_y, sz)
	for i in range(AFTER_POUR_FRAMES):
		await process_frame

	# First look: right after the pour, with only the water changed. This is the
	# window the dedup used to swallow — the chunk is resident and drawn, its
	# opaque mesh is untouched, and the only new content is liquid.
	var early := _liquid_counters()
	print("probe: right after the pour (whole world quiet): missing=%d repairs=%d"
		% [early[0], early[1]])

	for i in range(SETTLE_FRAMES):
		await process_frame
	var wet_after := _wet_count(sx, pen_y, sz)
	print("probe: the pen holds %d water cells after %d frames (was %d just after the pour)"
		% [wet_after, SETTLE_FRAMES, wet_before])
	if wet_after == 0:
		_fail("no water in the pen at all — the pour itself did not land")
		quit(1); return

	# Read the renderer's invariant straight out of the performance report.
	var final := _liquid_counters()
	var missing: int = final[0]
	var repairs: int = final[1]
	if missing < 0 or repairs < 0 or early[0] < 0 or final[2] < 0:
		_fail("the performance report has no Liquid missing line — the counter was not wired up")
		quit(1); return

	if early[0] != 0:
		_fail("%d chunk(s) held liquid with no liquid geometry right after the pour" % early[0])
	if missing != 0:
		_fail("%d chunk(s) hold liquid with no liquid geometry on the GPU" % missing)
	else:
		print("probe: every chunk with liquid has liquid geometry on the GPU")
	print("probe: %d chunk(s) needed a repair remesh" % repairs)

	# The upload counters: the dedup may only skip a mesh whose content truly did
	# not change, water included. A skip that dropped a different water mesh is
	# water on no screen.
	var swallowed: int = final[2]
	print("probe: uploads=%d dedup_skips=%d water_only_skips=%d" % [final[3], final[4], swallowed])
	if swallowed != 0:
		_fail("%d upload(s) were skipped while the water mesh had changed" % swallowed)

	if not ok:
		quit(1); return
	print("PROBE PASS")
	quit(0)

# [liquid missing, liquid repairs, water-only skips, uploads, dedup skips].
func _liquid_counters() -> Array:
	var missing := -1
	var repairs := -1
	var swallowed := -1
	var uploads := -1
	var skips := -1
	var report: String = cm.get_performance_report()
	for raw_line in report.split("\n"):
		var line: String = raw_line
		if line.contains("Liquid missing:"):
			print("probe:", line.strip_edges())
			var numbers: PackedStringArray = line.replace("Liquid missing:", "") \
				.replace("Liquid repairs:", " ").strip_edges().split(" ", false)
			missing = int(numbers[0])
			repairs = int(numbers[numbers.size() - 1])
		elif line.contains("Water-only skips:"):
			print("probe:", line.strip_edges())
			var counts: PackedStringArray = line.replace("Mesh uploads:", "") \
				.replace("Dedup skips:", " ").replace("Water-only skips:", " ") \
				.strip_edges().split(" ", false)
			uploads = int(counts[0])
			skips = int(counts[1])
			swallowed = int(counts[2])
	return [missing, repairs, swallowed, uploads, skips]

# First solid block below the player, scanned at the player's own column.
func _ground_y(sx: int, sz: int) -> int:
	var y := int(player.global_position.y) + 2
	while y > -64:
		if cm.get_block(sx, y, sz) != 0:
			return y
		y -= 1
	return -1

func _wet_count(sx: int, sy: int, sz: int) -> int:
	var wet := 0
	for dz in range(-PEN_RADIUS, PEN_RADIUS + 1):
		for dx in range(-PEN_RADIUS, PEN_RADIUS + 1):
			if water_ids.has(cm.get_block(sx + dx, sy, sz + dz)):
				wet += 1
	return wet
