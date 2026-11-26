extends SceneTree
## Headless check of the RUN-AWAY, on a stub body: the live bug was a goblin that FROZE while
## fleeing. The trace (2026-11-25 22:58) is thirteen `goal column has no navigable surface` asks in
## four seconds, `wish=(0.00, 0.00)` on every sample, and health walking 4 -> 0 while the player hit
## it: it had an enemy, a speed, a destination and nothing to walk, so nothing moved it.
##
## Two things are under test, and they are the two halves of `_run_away`:
##
##   1. the aim is the GROUND of the column eight blocks off, not a fixed height over it. The
##      planner anchors a goal on the level it is handed and its ring wants exactly that level, so
##      a point aimed four blocks above the feet into a slope is a cell inside the hill and every
##      ask is refused;
##   2. a run-away with no route still RUNS. A plan can come back empty (no standable cell, a wall,
##      a budget out) and the branch that answers it has to leave a wish behind, or the body stands
##      there and is beaten to death.
##
## Neither can be checked in the pure test binary -- it is a script -- and the world probe needs a
## chunk manager and a world it does not have. So the body here is the real script with a stub sim:
## feet, solid cells, no planner at all (`walk_to` answers 0 with no chunk manager, which is the
## "no route" case exactly), and no scene.
##
##   probes/probe_goblin_flee.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_flee.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## A body with no world in it: the feet, and which cells are solid. Every answer the walk asks for,
## and nothing that moves.
const STUB_SCRIPT := """extends RefCounted
var feet := Vector3.ZERO
var solid := {}
func get_tick_position() -> Vector3:
	return feet
func get_position() -> Vector3:
	return feet
func is_in_water() -> bool:
	return false
func is_on_ground() -> bool:
	return true
func get_velocity() -> Vector3:
	return Vector3.ZERO
func is_solid_at(x: int, y: int, z: int) -> bool:
	return solid.has(Vector3i(x, y, z))
func is_liquid_at(_x: int, _y: int, _z: int) -> bool:
	return false
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


func _run() -> void:
	# Under a Node3D parent: a body whose parent is the Window has no 3D transform, and its
	# `global_position` reads zero however its `position` is set.
	var stage := Node3D.new()
	stage.name = "Stage"
	root.add_child(stage)
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	stage.add_child(body)
	# Nothing here is stepped: the shoe is a stub, and a physics tick would ask it for a world.
	body.set_physics_process(false)
	body.set_process(false)
	var sim := _stub()
	body.set("_sim", sim)
	var enemy := Node3D.new()
	enemy.name = "Player"
	stage.add_child(enemy)
	sim.set("feet", Vector3(0.5, 64.0, 0.5))
	enemy.position = Vector3(4.5, 64.0, 0.5)
	# A transform set this frame is not readable this frame.
	await process_frame
	print("PROBE enemy at %s, goblin at %s" % [str(enemy.global_position), str(sim.get("feet"))])

	# --- 1. Wounded, with no plan at all, it still runs -----------------------
	# Four blocks from the player is inside the 16 it runs until, well outside its arm, and there is
	# no chunk manager in this world, so `walk_to` answers 0 and no route ever arrives: the exact
	# state the trace was in, on the second of its four frozen seconds.
	body.set("_health", 2.0)
	body.set("_enemy", enemy)
	body.set("_anger_ticks", 400)
	body.set("_repath_ticks", 0)
	body.call("_update_enemy", 1)
	body.set("_wish", Vector3.ZERO)
	body.call("_steer")
	var wish: Vector3 = body.get("_wish")
	print("PROBE flee: fleeing=%s route=%d wish=%s closing=%s"
		% [str(body.get("_fleeing")), (body.get("_route") as Array).size(), str(wish),
			str(body.get("_closing"))])
	if not bool(body.get("_fleeing")):
		_fail("a goblin at 2 health with an enemy was not fleeing")
	if (body.get("_route") as Array).size() != 0:
		print("PROBE note: a route arrived -- this run is not the empty-plan case")
	if wish.length() < 0.5:
		_fail("a fleeing goblin with nothing to walk stood still (wish %s)" % str(wish))
	elif wish.normalized().x > -0.9:
		_fail("the run-away is not away from the enemy (wish %s)" % str(wish))

	# --- 2. The aim is the ground of the column, not a height over it --------
	# A hilltop six blocks above the goblin's feet in the column it is aiming at: the run-away has
	# to point at the cell standing on that top, because a fixed height over it is inside the slope
	# and the planner refuses it.
	sim.set("solid", {Vector3i(8, 70, 0): true})
	var stand: Vector3 = body.call("_stand_over", 8, 0, 64.0)
	print("PROBE aim: column (8, 0) from feet 64 -> %s (the hilltop's own stand)" % str(stand))
	if stand != Vector3(8.5, 71.0, 0.5):
		_fail("the run-away's aim is not the column's standable cell (%s)" % str(stand))
	var none: Vector3 = body.call("_stand_over", 30, 0, 64.0)
	print("PROBE aim: a column with nothing in it -> %s" % str(none))
	if none != Vector3.ZERO:
		_fail("a column with no ground answered %s instead of nothing" % str(none))

	# --- 3. Only a run-away runs blind --------------------------------------
	# A goblin that is not fleeing has no business sprinting off when its plan comes back empty:
	# the chase waits out its patience instead.
	body.set("_health", 12.0)
	body.set("_fleeing", false)
	body.set("_closing", false)
	body.set("_anger_ticks", 400)
	body.set("_repath_ticks", 0)
	body.set("_route", [])
	body.call("_update_enemy", 1)
	body.set("_wish", Vector3.ZERO)
	body.call("_steer")
	var chase_wish: Vector3 = body.get("_wish")
	print("PROBE chase: fleeing=%s wish=%s" % [str(body.get("_fleeing")), str(chase_wish)])
	if chase_wish.length() > 0.01:
		_fail("a goblin that is not fleeing ran off with no route (wish %s)" % str(chase_wish))

	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
