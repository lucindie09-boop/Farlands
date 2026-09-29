extends SceneTree
## Natural (generated) water, checked where it actually lives: the ocean.
##
## The report from the user was water — most visibly the sea — that is there to
## collide with and outline but not to look at, until an edit happens to dirty the
## chunk. Every explanation for that is invisible from GDScript, so this probe
## puts the player over an ocean, lets streaming settle, and reads the renderer's
## own invariants out of the performance report:
##
##   Liquid missing:  chunks with terrain geometry on the GPU and liquid in their
##                    data but no liquid geometry (0 means water is not being
##                    dropped between the data and the mesh)
##   Water-only skips: uploads the content hash skipped that carried a different
##                    water mesh — the dedup eating a water change
##   Unrendered:      chunks with geometry in range, not covered by a far region,
##                    but with no instance drawing them
##
## All must be 0. It also samples the sea surface itself, so "no water at all
## here" cannot be mistaken for "water renders fine".
##
## It writes nothing to the world (no blocks, no fluid), so unlike the pouring
## probes it does not need the snapshot wrapper — but running it through
## `.freebuff/run_probe.sh` is harmless and consistent:
##   .freebuff/run_probe.sh .freebuff/probe_ocean_water.gd

const TELEPORT_MARGIN := 6       # blocks above the ocean surface to hover at
const SETTLE_FRAMES := 400

var cm: Node
var player: Node3D
var ok := true

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

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(10):
		await process_frame

	var here := player.global_position
	var ocean: Dictionary = cm.find_biome("ocean", int(here.x), int(here.z), 3000)
	if not ocean.get("found", false):
		_fail("no ocean within 3000 blocks of spawn — nothing to check")
		quit(1); return
	print("probe: nearest ocean at (%d, %d, %d)" % [ocean["x"], ocean["y"], ocean["z"]])

	# Hover over it. The water surface is the top of the sea, so flood.filled
	# columns are the surface_water cells right below that height.
	var surface_y: int = int(ocean["y"]) + TELEPORT_MARGIN
	player.global_position = Vector3(ocean["x"] + 0.5, surface_y + 20.0, ocean["z"] + 0.5)
	for i in range(SETTLE_FRAMES):
		await process_frame

	# Is there really a sea under us? Sample a grid at the reported height.
	var sampled := 0
	var wet := 0
	for dz in range(-24, 25, 8):
		for dx in range(-24, 25, 8):
			var id: int = cm.get_block(int(ocean["x"]) + dx, int(ocean["y"]), int(ocean["z"]) + dz)
			sampled += 1
			if cm.get_block_name(id) == "surface_water":
				wet += 1
	print("probe: %d/%d sampled columns at the ocean surface hold surface_water" % [wet, sampled])
	if wet == 0:
		_fail("no sea under the reported ocean position — the probe is not looking at water")
		quit(1); return

	var missing := -1
	var swallowed := -1
	var unrendered := -1
	var report: String = cm.get_performance_report()
	for raw_line in report.split("\n"):
		var line: String = raw_line
		if line.contains("Liquid missing:"):
			print("probe:", line.strip_edges())
			var numbers: PackedStringArray = line.replace("Liquid missing:", "") \
				.replace("Liquid repairs:", " ").strip_edges().split(" ", false)
			missing = int(numbers[0])
		elif line.contains("Water-only skips:"):
			var counts: PackedStringArray = line.replace("Mesh uploads:", "") \
				.replace("Dedup skips:", " ").replace("Water-only skips:", " ") \
				.strip_edges().split(" ", false)
			swallowed = int(counts[2])
		elif line.contains("Unrendered:"):
			print("probe:", line.strip_edges())
			unrendered = int(line.replace("Unrendered:", "").strip_edges())

	if missing < 0 or swallowed < 0 or unrendered < 0:
		_fail("the performance report is missing one of the water counters")
		quit(1); return
	if missing != 0:
		_fail("%d ocean chunk(s) hold liquid with no liquid geometry on the GPU" % missing)
	if swallowed != 0:
		_fail("%d upload(s) were skipped while the water mesh had changed" % swallowed)
	if unrendered != 0:
		_fail("%d chunk(s) with geometry in range have no instance drawing them" % unrendered)
	if ok:
		print("probe: ocean water is complete — no dropped liquid, no swallowed uploads, nothing unrendered")

	if not ok:
		quit(1); return
	print("PROBE PASS")
	quit(0)
