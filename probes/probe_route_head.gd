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

	# --- a CUT-OFF answer is never walked: the roll gets a second AIM -------------
	# `truncated` means a budget ran out, so what it carries is the distance the search got rather
	# than a route to the place -- and a stroll aims STROLL_UP above its feet, which in a pit is the
	# hillside, so the goal is the top of a wall and the walk to it is a climb the 4,000/8 ms could
	# not finish. Walking that is how one roll became a refused hop and then twenty asks at 0.2 s
	# (`nav_trace` 2026-11-27 22:32: `route=0/7`, `blocked=2`, `wish=(0.00, 0.00)`, no movement).
	# The first cut-off re-aims the roll at the body's OWN level, which is the ground it can already
	# walk on; the second gives the goal up, which is what puts the roll back in front of it. A
	# whole answer is walked always, however many shots it took to get one.
	# A goblin spawned beside the player picks it up as an enemy, and a fight owns its plans: the
	# stroll is the idle goblin's business, so this half of the probe is an idle one.
	body.set("_enemy", null)
	var cut := {"nodes": nodes, "truncated": true}
	var goal_kept := Vector3(0.0, 0.0, 1.0)
	var sim: Object = body.get("_sim")
	var body_y: float = (sim.call("get_tick_position") as Vector3).y if sim != null else start.y
	body.call("_abandon_route")
	body.set("_goal", goal_kept)
	body.set("_stroll_reaimed", false)
	body.set("_idle_ticks", 0)
	body.call("_take_route", cut)
	if not (body.get("_route") as Array).is_empty():
		_fail("a cut-off answer was walked (%d cells) instead of re-aiming the roll"
			% (body.get("_route") as Array).size())
	var reaimed = body.get("_stroll_reaimed")
	if reaimed == null or not bool(reaimed):
		_fail("the cut-off answer did not re-aim the roll (%s)" % str(reaimed))
	var aim := body.get("_goal") as Vector3
	if aim == goal_kept:
		_fail("the cut-off answer kept the goal it was asked for")
	if absf(aim.y - body_y) > 1.0:
		_fail("the re-aim is %s, which is not the body's own level (%.1f)" % [str(aim), body_y])
	# ...and a cut-off after that has spent both shots: the goal goes and the roll is held off.
	body.set("_goal", goal_kept)
	body.set("_idle_ticks", 0)
	body.call("_take_route", cut)
	if not (body.get("_route") as Array).is_empty():
		_fail("the re-aimed cut-off was walked as well (%d cells)"
			% (body.get("_route") as Array).size())
	if (body.get("_goal") as Vector3) != Vector3.ZERO:
		_fail("the goal survived both shots at an aim (%s)" % str(body.get("_goal")))
	if int(body.get("_idle_ticks")) <= 0:
		_fail("giving the goal up kept no hold, so the roll cannot be in front of it")
	# ...a WHOLE answer is a place, not a budget: it is walked whatever came before it.
	body.set("_idle_ticks", 0)
	body.call("_take_route", {"nodes": nodes})
	if (body.get("_route") as Array).is_empty():
		_fail("a whole answer was refused after a cut-off one")
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
