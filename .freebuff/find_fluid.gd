extends SceneTree
## Read-only: where is there fluid near spawn, and which block is it?
##
## Written to locate the leftovers of the first (unwrapped) probe_flow runs, after
## a bounded cleanup kept seeing water reappear. Prints a per-id, per-layer summary
## with a bounding box, so probe litter (runoff/falling at odd heights) can be told
## apart from a natural ocean or pond.
##
## Run: Godot --headless --path <project> --script res://.freebuff/find_fluid.gd

const RADIUS := 96
const Y_BOTTOM := 180
const Y_TOP := 300

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("FIND FAIL: main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)

	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("FIND FAIL: ChunkManager missing"); quit(1); return

	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 60:
			break

	var names := {}
	names[BlockTextures.get_block_id_by_name("water")] = "water(source)"
	names[BlockTextures.get_block_id_by_name("surface_water")] = "surface_water"
	for d in range(1, 8):
		names[BlockTextures.get_block_id_by_name("water_runoff_%d" % d)] = "runoff_%d" % d
	names[BlockTextures.get_block_id_by_name("water_fallen")] = "water_fallen"
	names.erase(0)

	var stats := {}          # name -> {count, min, max}
	var total := 0
	for y in range(Y_BOTTOM, Y_TOP + 1):
		for dz in range(-RADIUS, RADIUS + 1):
			for dx in range(-RADIUS, RADIUS + 1):
				var id: int = cm.get_block(dx, y, dz)
				if not names.has(id):
					continue
				var name: String = names[id]
				if not stats.has(name):
					stats[name] = { "count": 0, "min": Vector3i(dx, y, dz), "max": Vector3i(dx, y, dz) }
				var s: Dictionary = stats[name]
				s["count"] += 1
				s["min"] = Vector3i(mini(s["min"].x, dx), mini(s["min"].y, y), mini(s["min"].z, dz))
				s["max"] = Vector3i(maxi(s["max"].x, dx), maxi(s["max"].y, y), maxi(s["max"].z, dz))
				total += 1
			if total > 200000:
				print("FIND: too much fluid to summarise")
				quit(1)
				return

	print("find: player at %s; box x,z=+-%d y=%d..%d" % [player_pos(main), RADIUS, Y_BOTTOM, Y_TOP])
	for name in stats:
		var s: Dictionary = stats[name]
		print("find: %-16s %6d cells, y %d..%d, x %d..%d, z %d..%d"
			% [name, s["count"], s["min"].y, s["max"].y, s["min"].x, s["max"].x, s["min"].z, s["max"].z])
	if stats.is_empty():
		print("find: no fluid at all in the box")
	print("FIND DONE")
	quit(0)

func player_pos(main: Node3D) -> Vector3:
	var p: Node3D = main.get_node_or_null("Player")
	return p.global_position if p != null else Vector3.INF
