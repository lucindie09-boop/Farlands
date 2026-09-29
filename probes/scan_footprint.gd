extends SceneTree
## Read-only: every non-air block inside the footprint a leaked-probe cleanup
## would touch — x,z in [-14, 14] of the spawn column, y from 248 to 280. Used to
## check that a cleanup would remove probe litter and nothing of the user's.

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("FOOT: main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("FOOT: ChunkManager missing"); quit(1); return
	for i in range(120):
		await process_frame
	var by_level := {}
	for y in range(248, 281):
		var cells: Array = []
		for dz in range(-14, 15):
			for dx in range(-14, 15):
				var id: int = cm.get_block(dx, y, dz)
				if id == 0:
					continue
				cells.append({"name": cm.get_block_name(id), "x": dx, "z": dz})
		if cells.is_empty():
			continue
		var names := {}
		for cell in cells:
			var key: String = cell["name"]
			names[key] = int(names.get(key, 0)) + 1
		var parts: Array = []
		for key in names:
			parts.append("%s x%d" % [key, names[key]])
		by_level[y] = parts
		print("FOOT: y=%d  %s" % [y, ", ".join(parts)])
	# Anything that is not plain stone/air inside the footprint is interesting:
	# the leaked shelves are stone, the leaked floods are liquid states, and
	# anything else there belongs to the player.
	for y in by_level:
		var parts: Array = by_level[y]
		var suspicious: Array = []
		for part in parts:
			var name: String = String(part).split(" x")[0]
			if name != "stone":
				suspicious.append(part)
		if suspicious.is_empty():
			continue
		print("FOOT: y=%d NON-STONE  %s" % [y, ", ".join(suspicious)])
	quit(0)
