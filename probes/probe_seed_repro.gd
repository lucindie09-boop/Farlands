extends SceneTree
## Scratch: is a probe run reproducible when the seed is pinned? Prints the engine's own
## seed and the ground under the origin, twice in one process (a reset and a re-read).
##
##   probes/run_probe.sh probes/probe_seed_repro.gd 120

const WORLD_SEED := 1337


func _initialize() -> void:
	_run()


func _ground(x: int, z: int) -> int:
	for y in range(1024, 0, -1):
		if int(_cm.get_block(x, y, z)) != 0:
			return y
	return 0


var _cm: Node = null
var _player: Node3D = null


func _run() -> void:
	var scene: PackedScene = load("res://Main.tscn")
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	_cm = main.get_node_or_null("ChunkManager")
	_player = main.get_node_or_null("Player")
	print("PROBE engine seed at load: %s" % str(_cm.get("seed")))
	_cm.set("seed", WORLD_SEED)
	print("PROBE engine seed after set: %s" % str(_cm.get("seed")))
	_cm.set("render_distance", 4)
	await _frames(240)
	print("PROBE player %s, ground at (0,0) = %d, at (64,0) = %d, at (0,64) = %d" % [
		str(_player.global_position), _ground(0, 0), _ground(64, 0), _ground(0, 64)])
	for key in ["sea_level", "squish_enabled", "squish_slice", "squish_span", "seed",
			"distance", "lod_grid_enabled", "lod_grid_spacing"]:
		print("PROBE %s = %s" % [key, str(_cm.get(key))])
	print("PROBE the first water block down the column at (0,0): %d, and at (5000,0): %d" % [
		_water(0, 0), _water(5000, 0)])
	quit(0)


## The highest water block in a column, or 0.
func _water(x: int, z: int) -> int:
	for y in range(1024, 0, -1):
		var name := str(_cm.get_block_name(int(_cm.get_block(x, y, z))))
		if name.contains("water"):
			return y
	return 0


func _frames(count: int) -> void:
	for _i in count:
		await process_frame
