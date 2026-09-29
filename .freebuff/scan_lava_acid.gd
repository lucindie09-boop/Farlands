extends SceneTree
## Where is the lava/acid litter, if any? READ-ONLY: prints every lava or acid
## cell in the column of space above the player, plus a per-height census of the
## floating stone a leaked probe run leaves behind, so a cleanup can be exact
## instead of a guess.
##
## Run it directly (`godot --headless --path . --script res://.freebuff/scan_lava_acid.gd`)
## — NOT through run_probe.sh, which would restore the world this is reading.

const RADIUS := 20
const UP := 48

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("SCAN: main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		print("SCAN: ChunkManager / Player missing"); quit(1); return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited >= 30 and player.is_on_floor():
			break
	var spot := player.global_position.floor()
	var base_y := int(spot.y)
	print("SCAN: player at %s" % spot)

	var fluid := {}
	var fluid_first := {}
	var per_y := {}
	for y in range(base_y - 12, base_y + UP + 1):
		var counts := {}
		for dz in range(-RADIUS, RADIUS + 1):
			for dx in range(-RADIUS, RADIUS + 1):
				var x := int(spot.x) + dx
				var z := int(spot.z) + dz
				var id: int = cm.get_block(x, y, z)
				if id <= 0:
					continue
				var name: String = cm.get_block_name(id)
				counts[name] = int(counts.get(name, 0)) + 1
				if name.contains("lava") or name.contains("acid"):
					fluid[name] = int(fluid.get(name, 0)) + 1
					if not fluid_first.has(name):
						fluid_first[name] = Vector3i(x, y, z)
		if not counts.is_empty():
			per_y[y] = counts
	print("SCAN: %d lava/acid cells" % fluid.size() if not fluid.is_empty() else "SCAN: no lava/acid cells")
	for name in fluid:
		print("SCAN:   %s x%d, first at %s" % [name, fluid[name], fluid_first[name]])
	for y in per_y:
		var counts: Dictionary = per_y[y]
		var parts: Array = []
		for name in counts:
			parts.append("%s x%d" % [name, counts[name]])
		print("SCAN: y=%d  %s" % [y, ", ".join(parts)])
	quit(0)
