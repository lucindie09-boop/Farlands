extends SceneTree
## Headless check of what a walker keeps at the HEAD of a route.
##
## `_take_route` skips the cells the body already stands in, and it used to do that by RADIUS
## (0.9 blocks to the cell's middle) -- wide enough for the body's own corner (0.707), and also
## wide enough for the NEIGHBOUR whenever the body stands near the edge of its own cell: 0.05
## before the edge, the next cell's middle is 0.55 away. The neighbour went, the route began two
## columns off, and `_hop_ahead` reads a cell two columns off as the planner's jump across a
## one-cell gap -- so the walk stood at the lip to time a jump it must not take, refused it for
## want of speed, dropped the route, and was handed the same one straight back. A punch puts a
## body there: the knockback that shoved a goblin 0.0046 blocks past its own edge left it on one
## coordinate for three seconds of `nav_trace.txt` (blocked=10, re-planning every 0.2 s) while
## the player it should have been chasing stood four blocks away.
##
## Run: Godot --headless --path <project> --script res://probes/probe_route_head.gd

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	if player == null:
		print("PROBE FAIL: no player")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	var feet := player.global_position.floor()
	var start := Vector3(feet.x + 0.95, feet.y, feet.z + 0.5)
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load("res://scripts/goblin.gd"))
	body.add_to_group("goblin")
	body.position = start
	main.add_child(body)
	body.set("_idle_ticks", 100000000)
	for i in 6:
		await process_frame
	# Read the route the same frame it is handed over: the body starts walking on the next.
	body.call("_abandon_route")
	var support := int(floorf(feet.y)) - 1
	var nodes := PackedVector3Array([
		Vector3(feet.x, support, feet.z),
		Vector3(feet.x + 1.0, support, feet.z),
		Vector3(feet.x + 2.0, support, feet.z)])
	body.call("_take_route", {"nodes": nodes})
	var route: Array = body.get("_route")
	if route.is_empty():
		_fail("the whole route was skipped")
		print("PROBE PASS" if ok else "PROBE FAILED")
		quit(1)
		return
	var head: Vector3 = route[0]
	print("PROBE at=%s head=%s own_cell=%d head_cell=%d route=%d"
		% [str(start), str(head), int(feet.x), int(head.x), route.size()])
	if int(head.x) != int(feet.x) + 1:
		_fail("the route begins at cell %d, not the neighbour %d"
			% [int(head.x), int(feet.x) + 1])
	if route.size() != 2:
		_fail("route has %d cells, not the 2 left after the one it stands in" % route.size())
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
