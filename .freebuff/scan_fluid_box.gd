extends SceneTree
## Read-only: where the dynamic fluid actually is, and what could be feeding it.
## Lists every water/lava/acid state in a band around the spawn column, by name,
## with coordinates and a per-level breakdown, and checks the cells around each
## source for the pair rule. Written to answer "the cleanup removes 53 cells and
## 53 come back" — which cannot happen with no source anywhere.

const RX := 120
const RZ := 120
const Y_LOW := 236
const Y_HIGH := 268

func _initialize() -> void:
	_run()

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("BOX: main.tscn missing"); quit(1); return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	var cm: Node = main.get_node_or_null("ChunkManager")
	if cm == null:
		print("BOX: ChunkManager missing"); quit(1); return
	for i in range(150):
		await process_frame

	var dynamic := {}
	dynamic[BlockTextures.get_block_id_by_name("water")] = "water"
	for d in range(1, 8):
		dynamic[BlockTextures.get_block_id_by_name("water_runoff_%d" % d)] = "runoff_%d" % d
	dynamic[BlockTextures.get_block_id_by_name("water_fallen")] = "water_fallen"
	for substance in ["lava", "acid"]:
		dynamic[BlockTextures.get_block_id_by_name(substance)] = substance
		for d in range(1, 8):
			dynamic[BlockTextures.get_block_id_by_name("%s_runoff_%d" % [substance, d])] = "%s_runoff_%d" % [substance, d]
		dynamic[BlockTextures.get_block_id_by_name("%s_fallen" % substance)] = "%s_fallen" % substance
	dynamic.erase(0)

	var counts := {}
	var bounds := {}
	var sources: Array = []
	var x_min := 1 << 30
	var x_max := -(1 << 30)
	var z_min := 1 << 30
	var z_max := -(1 << 30)
	var y_min := 1 << 30
	var y_max := -(1 << 30)
	for y in range(Y_LOW, Y_HIGH + 1):
		for z in range(-RZ, RZ + 1):
			for x in range(-RX, RX + 1):
				var id: int = cm.get_block(x, y, z)
				if not dynamic.has(id):
					continue
				var name: String = String(dynamic[id])
				counts[name] = int(counts.get(name, 0)) + 1
				x_min = mini(x_min, x)
				x_max = maxi(x_max, x)
				z_min = mini(z_min, z)
				z_max = maxi(z_max, z)
				y_min = mini(y_min, y)
				y_max = maxi(y_max, y)
				if name == "water" and sources.size() < 12:
					sources.append(Vector3i(x, y, z))
	print("BOX: %d dynamic cells, x %d..%d, y %d..%d, z %d..%d"
		% [_total(counts), x_min, x_max, y_min, y_max, z_min, z_max])
	for name in counts:
		print("BOX:   %s x%d" % [name, counts[name]])
	for cell in sources:
		print("BOX:   source at %s; below=%s, neighbours=%s/%s/%s/%s"
			% [cell, _name(cm, cell.x, cell.y - 1, cell.z), _name(cm, cell.x + 1, cell.y, cell.z),
			   _name(cm, cell.x - 1, cell.y, cell.z), _name(cm, cell.x, cell.y, cell.z + 1),
			   _name(cm, cell.x, cell.y, cell.z - 1)])
	quit(0)

func _total(counts: Dictionary) -> int:
	var n := 0
	for key in counts:
		n += int(counts[key])
	return n

func _name(cm: Node, x: int, y: int, z: int) -> String:
	var id: int = cm.get_block(x, y, z)
	if id == 0:
		return "air"
	return cm.get_block_name(id)
