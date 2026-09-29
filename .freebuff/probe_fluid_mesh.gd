extends SceneTree
## What does the liquid surface pass cost IN THE GAME?
##
## The flow probe checks what the simulation DOES; this one checks that the mesh
## path runs and what it costs, because that is the half that has no other way to
## be observed from GDScript (the water mesh is a surface on a RenderingServer RID,
## not a scene node). It builds a small shelf, pours one source, lets it spread,
## and prints the `fluid_mesh` line of the performance report — which only exists
## if passive_fluid_mesh actually ran on real chunks in the real world.
##
## Not an assertion probe: it measures, and a missing `fluid_mesh` line is the
## only failure it can report. Run it through the wrapper, which snapshots and
## restores the saved world (the shelf and the poured water are persisted edits):
##   .freebuff/run_probe.sh .freebuff/probe_fluid_mesh.gd

const RADIUS := 4                 # 9x9 shelf: this one is not shaping a flood
const SETTLE_FRAMES := 150

var cm: Node
var player: Node3D
var ok := true

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	cm = main.get_node_or_null("ChunkManager")
	player = main.get_node_or_null("Player")
	if cm == null or player == null:
		print("PROBE FAIL: ChunkManager / Player missing")
		quit(1)
		return

	# Wait for the body to land: a player still falling puts the ceiling scan
	# inside the hill.
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	for i in range(10):
		await process_frame

	var stone: int = BlockTextures.get_block_id_by_name("stone")
	var water: int = BlockTextures.get_block_id_by_name("water")
	var sx := int(player.global_position.x)
	var sz := int(player.global_position.z)
	var ceiling := _ceiling(sx, sz)
	var sy := ceiling + 2
	print("probe: shelf at (%d, %d, %d), terrain ceiling %d" % [sx, sy, sz, ceiling])

	var writes := 0
	for dz in range(-RADIUS, RADIUS + 1):
		for dx in range(-RADIUS, RADIUS + 1):
			cm.set_block(sx + dx, sy - 1, sz + dz, stone)
			writes += 1
			if writes % 20 == 0:
				await process_frame
	for i in range(10):
		await process_frame

	var landed := 0
	for dz in range(-RADIUS, RADIUS + 1):
		for dx in range(-RADIUS, RADIUS + 1):
			if cm.get_block(sx + dx, sy - 1, sz + dz) == stone:
				landed += 1
	print("probe: shelf floor landed on %d/%d cells" % [landed, (2 * RADIUS + 1) * (2 * RADIUS + 1)])

	cm.set_block(sx, sy, sz, water)
	for i in range(SETTLE_FRAMES):
		await process_frame

	var wet := 0
	for dz in range(-RADIUS, RADIUS + 1):
		for dx in range(-RADIUS, RADIUS + 1):
			var id: int = cm.get_block(sx + dx, sy, sz + dz)
			if id != 0 and id != stone:
				wet += 1
	print("probe: %d liquid cells on the shelf after %d frames" % [wet, SETTLE_FRAMES])

	var saw_fluid_mesh := false
	for line in cm.get_performance_report().split("\n"):
		if line.contains("fluid_mesh") or line.contains("build_mesh") or line.contains("process_total"):
			print("probe:", line)
			if line.contains("fluid_mesh"):
				saw_fluid_mesh = true
	if not saw_fluid_mesh:
		print("PROBE FAIL: the performance report has no fluid_mesh line")
		ok = false

	if ok:
		print("PROBE PASS")
	quit(0 if ok else 1)

# The highest solid block anywhere in the shelf's footprint, so the shelf can go
# above all of it and every cell it writes into is air.
func _ceiling(sx: int, sz: int) -> int:
	var top := int(player.global_position.y) + 48
	var ceiling := 0
	for dz in range(-RADIUS, RADIUS + 1):
		for dx in range(-RADIUS, RADIUS + 1):
			var y := top
			while y > 0:
				if cm.get_block(sx + dx, y, sz + dz) != 0:
					ceiling = maxi(ceiling, y)
					break
				y -= 1
	return ceiling
