extends SceneTree
## Headless integration probe for the path planner.
##
## Loads the real scene so the world streams in, waits until the player is
## actually standing on the ground, then asks the engine for a route 96 blocks
## east and checks what came back: it must be found, non-empty, start on the
## origin column, reach the goal column, and every grid node must stand on a
## solid block.
##
## Run: Godot --headless --path <project> --script res://probes/probe_path.gd

var main: Node3D
var ok := true

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)

	var cm: Node = main.get_node_or_null("ChunkManager")
	var player: Node3D = main.get_node_or_null("Player")
	if cm == null or player == null:
		_fail("ChunkManager/Player missing")
		quit(1)
		return

	# Wait for the world to stream in AND the player to land: reading the feet
	# mid-fall would put the route's start inside the terrain below.
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited < 30:
			continue
		if player.is_on_floor() and _solid_under(cm, player.global_position):
			break
	print("probe: player landed after %d frames at %s" % [waited, player.global_position])

	var start := player.global_position.floor()
	var goal := _surface_point(cm, start + Vector3(96, 0, 0), start.y)
	if goal == Vector3.INF:
		_fail("no ground 96 blocks east of the player")
		quit(1)
		return
	if not _clear_above(cm, start):
		_fail("the player's own column has no headroom at %s" % start)
		quit(1)
		return
	print("probe: planning %s -> %s" % [start, goal])

	var job := int(cm.request_path(start, goal))
	if job == 0:
		_fail("request_path returned 0")
		quit(1)
		return

	var result := {}
	var frames := 0
	while frames < 900:
		await process_frame
		frames += 1
		for entry in cm.poll_paths():
			if int(entry.get("id", 0)) == job:
				result = entry
		if not result.is_empty():
			break
	if result.is_empty():
		_fail("no path result after %d frames" % frames)
		quit(1)
		return

	var nodes: PackedVector3Array = result.get("nodes", PackedVector3Array())
	var waypoints: PackedVector3Array = result.get("waypoints", PackedVector3Array())
	print("probe: found=%s truncated=%s ms=%.2f expansions=%d columns=%d grid=%d waypoints=%d error=\"%s\""
		% [result.get("found", false), result.get("truncated", false),
		   float(result.get("ms", 0.0)), int(result.get("expansions", 0)),
		   int(result.get("columns", 0)), nodes.size(), waypoints.size(),
		   result.get("error", "")])

	if not bool(result.get("found", false)):
		_fail("route not found: " + str(result.get("error", "")))
	elif nodes.is_empty() or waypoints.is_empty():
		_fail("empty route")
	elif absf(nodes[0].x - start.x) > 1.0 or absf(nodes[0].z - start.z) > 1.0:
		_fail("route does not start at the origin column (%s vs %s)" % [nodes[0], start])
	elif absf(nodes[nodes.size() - 1].x - goal.x) > 1.0:
		_fail("route does not reach the goal column (%s vs %s)" % [nodes[nodes.size() - 1], goal])
	elif waypoints.size() > nodes.size():
		_fail("waypoints exceed the grid path")
	else:
		print("probe: first=%s last=%s waypoints=%s" % [nodes[0], nodes[nodes.size() - 1], waypoints])
		# Every grid node must stand on a solid block (a support block is what
		# the overlay draws and what an agent would step onto).
		var floating := 0
		for node in nodes:
			if cm.get_block(int(node.x), int(node.y), int(node.z)) == 0:
				floating += 1
		if floating > 0:
			_fail("%d of %d route nodes stand on air" % [floating, nodes.size()])
		# Consecutive nodes must be one move apart, never a teleport.
		var jumps := 0
		for i in range(1, nodes.size()):
			var d: Vector3 = (nodes[i] - nodes[i - 1]).abs()
			if maxf(d.x, d.z) > 2.0 or d.y > 4.0:
				jumps += 1
		if jumps > 0:
			_fail("%d consecutive route nodes are more than one move apart" % jumps)

	print("PROBE %s" % ("PASS" if ok else "FAIL"))
	quit(0 if ok else 1)

# Topmost solid block in the column, returned as the feet position on top of it.
func _surface_point(cm: Node, from: Vector3, hint_y: float) -> Vector3:
	var x := int(floor(from.x))
	var z := int(floor(from.z))
	var y := int(hint_y) + 64
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(x, y + 1, z)
		y -= 1
	return Vector3.INF

func _solid_under(cm: Node, feet: Vector3) -> bool:
	var x := int(floor(feet.x))
	var z := int(floor(feet.z))
	return cm.get_block(x, int(floor(feet.y)) - 1, z) != 0

func _clear_above(cm: Node, feet: Vector3) -> bool:
	var x := int(feet.x)
	var z := int(feet.z)
	var y := int(feet.y)
	return cm.get_block(x, y, z) == 0 and cm.get_block(x, y + 1, z) == 0
