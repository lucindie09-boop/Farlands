extends SceneTree
## Headless check of the three decisions a chase in water makes, on a stub body rather than a
## world: the live bug they fix was "goblins stayed three blocks below me, trying to change their
## mind every second", on a staircase out of a stream.
##
## The trace said the plan was never the problem -- it climbed the staircase, identically, once a
## second -- so what is under test is what the walker did with it:
##
##   1. the water branch of `_update_enemy` KEEPS the route. It used to drop it on every tick it
##      was in water, which threw away the very plan the swim then asked for again a moment later
##      (nav_trace: route=0/5, 0/4, 0/0, plan after identical plan).
##   2. `_swim_along` does not eat a DRY cell above the body. Those cells are the staircase, and
##      skipping them by horizontal distance alone walked the climb out from under the body -- it
##      consumed the whole route while still below it and swam at the player's column instead.
##   3. `_swim` walks the route before swimming at a DRY target, so a player standing on the stairs
##      is reached by the plan rather than by pressing into the trunk under him. A WET target is
##      still swum at directly: a route to one ends on the bank, which is why that rule exists.
##
## None of it can be checked in the pure test binary (it is a script), and the world probe that
## would check it end to end (`probes/probe_goblin_water.gd`) cannot currently stand a body up at
## all in this environment -- it freezes at its spawn cell and suffocates at HEAD too. So the body
## here is a stub: real script, hand-fed route and terrain, no scene.
##
##   probes/probe_swim_route.gd  (Godot --headless --path <project> --script res://probes/probe_swim_route.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## A body sim with no world in it: the feet, whether it is wet, and which cells are liquid. Every
## answer the swim asks for, and nothing that moves.
const STUB_SCRIPT := """extends RefCounted
var feet := Vector3.ZERO
var wet := false
var liquid := {}
func get_tick_position() -> Vector3:
	return feet
func get_position() -> Vector3:
	return feet
func is_in_water() -> bool:
	return wet
func is_on_ground() -> bool:
	return true
func get_velocity() -> Vector3:
	return Vector3.ZERO
func is_solid_at(_x: int, _y: int, _z: int) -> bool:
	return false
func is_liquid_at(x: int, y: int, z: int) -> bool:
	return liquid.has(Vector3i(x, y, z))
func get_tick_count() -> int:
	return 0
func consume_damage() -> float:
	return 0.0
func reset(at: Vector3) -> void:
	feet = at
func step(_delta: float, _wish: Vector3, _jump: bool, _sprint: bool, _sneak: bool, _yaw: float,
		_speed: float) -> void:
	pass
"""

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _stub() -> RefCounted:
	var script := GDScript.new()
	script.source_code = STUB_SCRIPT
	script.reload()
	var out: RefCounted = RefCounted.new()
	out.set_script(script)
	return out


func _route(cells: Array) -> Array[Vector3]:
	var out: Array[Vector3] = []
	for cell in cells:
		out.append(cell)
	return out


func _run() -> void:
	# Under a Node3D parent: a body whose parent is the Window has no 3D transform at all, and its
	# `global_position` reads zero however its `position` is set -- which is an enemy 64 blocks away.
	var stage := Node3D.new()
	stage.name = "Stage"
	root.add_child(stage)
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	stage.add_child(body)
	# Nothing here is moved by a tick: the shoe is a stub, and a physics step would ask it for a
	# `step` it does not have.
	body.set_physics_process(false)
	body.set_process(false)
	var sim := _stub()
	body.set("_sim", sim)
	var enemy := Node3D.new()
	stage.add_child(enemy)

	# --- 1. A body in water keeps its route ------------------------------------
	sim.set("feet", Vector3(0.5, 64.0, 0.5))
	sim.set("wet", true)
	# Close enough to be inside the leash and far enough out of reach that the chase is walking, not
	# swinging -- the branch under test is the one past the reach.
	enemy.position = Vector3(2.5, 64.0, 0.5)
	# A transform set this frame is not readable this frame: the global one updates on the tree's own
	# pass, and every read here is the walker asking a node where it is.
	await process_frame
	print("PROBE enemy: position=%s global=%s" % [str(enemy.position), str(enemy.global_position)])
	body.set("_health", 12.0)
	body.set("_enemy", enemy)
	body.set("_anger_ticks", 400)
	body.set("_wet_target", false)
	body.set("_route", _route([Vector3(1, 63, 0), Vector3(1, 64, 0)]))
	body.set("_next", 0)
	print("PROBE water: before anger=%s leash=%s health=%s enemy=%s feet=%s wet=%s"
		% [str(body.get("_anger_ticks")), str(body.get("_follow_range")), str(body.get("_health")),
			str(body.get("_enemy")), str(sim.get("feet")), str(sim.get("wet"))])
	body.call("_update_enemy", 1)
	print("PROBE water: after anger=%s enemy=%s route=%d"
		% [str(body.get("_anger_ticks")), str(body.get("_enemy")), (body.get("_route") as Array).size()])
	var kept: Array = body.get("_route")
	print("PROBE water: route %d cell(s) after a tick in water (was %d)" % [kept.size(), 2])
	if kept.size() != 2:
		_fail("a body in water dropped the route it was given (%d of 2 cells left)" % kept.size())

	# --- 2. The staircase is not skipped --------------------------------------
	# Its own cell counts as arrived; the dry cell one ABOVE it is a lip and does not.
	sim.set("feet", Vector3(10.2, 65.0, 0.5))
	body.set("_route", _route([Vector3(10, 64, 0), Vector3(10, 65, 0)]))
	body.set("_next", 0)
	body.set("_wish", Vector3.ZERO)
	var steering := bool(body.call("_swim_along", sim.get("feet")))
	var next := int(body.get("_next"))
	var wish: Vector3 = body.get("_wish")
	print("PROBE climb: steering=%s next=%d wish=%s (needs next=1, wish along +x)"
		% [str(steering), next, str(wish)])
	if not steering or next != 1:
		_fail("the swim consumed the lip it had not climbed (next=%d of 2)" % next)
	elif wish.x < 0.9:
		_fail("the swim is not steering at the lip (wish %s)" % str(wish))

	# ...and a WET cell above it IS eaten: a swimmer bobs at the surface rather than standing on it,
	# so a level test there would stall the route on every trough of the bob.
	sim.set("feet", Vector3(20.2, 64.0, 0.5))
	sim.set("liquid", {Vector3i(20, 65, 0): true})
	body.set("_route", _route([Vector3(20, 65, 0)]))
	body.set("_next", 0)
	body.call("_swim_along", sim.get("feet"))
	# Arrived means the one-cell route is walked out and dropped -- by the WET rule, since the body is
	# two blocks below that cell's surface and a level test would have refused it.
	print("PROBE bob: route %d cell(s) left of 1 after a wet cell above the body"
		% (body.get("_route") as Array).size())
	if not (body.get("_route") as Array).is_empty():
		_fail("a wet cell above the body was not arrived at, so a bobbing swimmer would stall")
	sim.set("liquid", {})

	# --- 3. The route wins over swimming at a DRY target ----------------------
	# The route's next cell is off along +z; the target is along +x. Which way the wish points says
	# which answer was taken.
	sim.set("feet", Vector3(30.2, 64.0, 0.5))
	sim.set("wet", true)
	enemy.position = Vector3(40.5, 64.5, 0.5)
	await process_frame
	body.set("_enemy", enemy)
	body.set("_anger_ticks", 400)
	body.set("_route", _route([Vector3(30, 64, 1)]))
	body.set("_next", 0)
	body.set("_wish", Vector3.ZERO)
	body.set("_closing", true)
	body.set("_wet_target", false)
	body.call("_swim", sim.get("feet"))
	var dry_wish: Vector3 = body.get("_wish")
	var stroke := bool(body.get("_jump"))
	print("PROBE dry target: wish=%s stroke=%s (needs +z, the route)" % [str(dry_wish), str(stroke)])
	if dry_wish.z < 0.5 or absf(dry_wish.x) > 0.6:
		_fail("the swim steered at a dry target's column instead of along its route (wish %s)"
			% str(dry_wish))
	elif not stroke:
		_fail("the swim did not press the stroke")

	# A target standing IN water is still swum at: its plan can only end on the bank.
	body.set("_wish", Vector3.ZERO)
	body.set("_wet_target", true)
	body.set("_closing", true)
	body.set("_enemy", enemy)
	body.set("_route", _route([Vector3(30, 64, 1)]))
	print("PROBE wet target: wet=%s closing=%s enemy=%s"
		% [str(body.get("_wet_target")), str(body.get("_closing")), str(body.get("_enemy"))])
	body.call("_swim", sim.get("feet"))
	var wet_wish: Vector3 = body.get("_wish")
	print("PROBE wet target: wish=%s (needs +x, straight at it)" % str(wet_wish))
	if wet_wish.x < 0.9:
		_fail("a target standing in water was not swum at directly (wish %s)" % str(wet_wish))

	# --- 4. A lip it cannot climb is given up on ------------------------------
	# The swim zeroes every clock that measures walking, so a route asking for a climb the lift
	# cannot make used to be pressed at for as long as the body's anger lasted (nav_trace,
	# 2026-11-25 22:20: `stuck` never passing 6 at a staircase foot). Ninety frames, then a re-plan.
	sim.set("feet", Vector3(30.2, 64.0, 0.5))
	body.set("_route", _route([Vector3(30, 66, 0)]))  # surface 67, three above the body
	body.set("_next", 0)
	body.set("_needs_replan", false)
	body.set("_lip_frames", 0)
	for i in 89:
		body.call("_lip_clock", sim.get("feet"))
	var early := bool(body.get("_needs_replan"))
	for i in 2:
		body.call("_lip_clock", sim.get("feet"))
	var late := bool(body.get("_needs_replan"))
	print("PROBE lip: replan after 89 frames=%s, after 91=%s (needs false, then true)"
		% [str(early), str(late)])
	if early or not late:
		_fail("a lip the swim cannot climb is not given up on (89=%s, 91=%s)"
			% [str(early), str(late)])

	# ...and a WET cell above it is not a lip: that is the water itself, which the swim rides.
	sim.set("liquid", {Vector3i(30, 66, 0): true})
	body.set("_needs_replan", false)
	body.set("_lip_frames", 0)
	for i in 100:
		body.call("_lip_clock", sim.get("feet"))
	print("PROBE lip: a wet cell above still replans=%s (needs false)"
		% str(body.get("_needs_replan")))
	if bool(body.get("_needs_replan")):
		_fail("a wet cell above the body was treated as a lip to climb")

	body.set("_enemy", null)
	stage.queue_free()
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
