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

## The session's trace, muted: this probe spawns a body in the real world, and a body in the real
## world installs one. `user://nav_trace.txt` is a session's own file.
const NAV_TRACE := preload("res://scripts/nav_trace.gd")

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _run() -> void:
	NAV_TRACE.muted = true
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

	# --- a CUT-OFF answer handed back with the same head gives the GOAL up --------
	# `truncated` means a budget ran out, so the same question under the same clock is handed back
	# the same route -- and a stroll's answer to a route it cannot walk is to ask again, which is
	# the 0.2 s loop of `nav_trace` 2026-11-27 22:32: twenty asks at `blocked=2`, `route=0/7`,
	# `wish=(0.00, 0.00)` and no movement. The second identical cut-off drops the goal, which is
	# what puts the stroll's roll back in front of it. A whole answer, and a cut-off with a
	# DIFFERENT head, are both still walked.
	var cut := {"nodes": nodes, "truncated": true}
	var goal_kept := Vector3(0.0, 0.0, 1.0)
	body.call("_abandon_route")
	body.set("_goal", goal_kept)
	body.call("_take_route", cut)
	if (body.get("_route") as Array).is_empty():
		_fail("a cut-off answer was refused before it had been seen twice")
	else:
		var first_head: Vector3 = (body.get("_route") as Array)[0]
		var remembered = body.get("_cut_off_head")
		if remembered == null or not (remembered as Vector3).is_equal_approx(first_head):
			_fail("the cut-off head %s was not remembered (%s)"
				% [str(first_head), str(remembered)])
	body.set("_goal", goal_kept)
	body.set("_idle_ticks", 0)
	body.call("_take_route", cut)
	if not (body.get("_route") as Array).is_empty():
		_fail("the same cut-off answer was handed back and walked again (%d cells)"
			% (body.get("_route") as Array).size())
	if (body.get("_goal") as Vector3) != Vector3.ZERO:
		_fail("the goal survived the same cut-off answer twice (%s)" % str(body.get("_goal")))
	if int(body.get("_idle_ticks")) <= 0:
		_fail("giving the goal up kept no hold, so the roll cannot be in front of it")
	# ...a WHOLE answer is not a loop, however much it looks like one.
	body.set("_idle_ticks", 0)
	body.call("_take_route", {"nodes": nodes})
	if (body.get("_route") as Array).is_empty():
		_fail("a whole answer was refused after a cut-off one")
	if (body.get("_cut_off_head") as Vector3) != Vector3.INF:
		_fail("a whole answer left a cut-off head remembered (%s)"
			% str(body.get("_cut_off_head")))
	# ...and neither is a cut-off somewhere ELSE: a different frontier is a different answer.
	body.call("_abandon_route")
	body.call("_take_route", cut)
	var elsewhere := PackedVector3Array([
		Vector3(feet.x, support, feet.z + 1.0),
		Vector3(feet.x + 1.0, support, feet.z + 1.0),
		Vector3(feet.x + 2.0, support, feet.z + 1.0)])
	body.set("_goal", goal_kept)
	body.call("_take_route", {"nodes": elsewhere, "truncated": true})
	if (body.get("_route") as Array).is_empty():
		_fail("a cut-off answer with a different head was refused")
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
