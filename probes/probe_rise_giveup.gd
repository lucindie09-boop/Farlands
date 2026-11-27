extends SceneTree
## Headless check of the walker's give-up on a rise it cannot actually make, on a stub body rather
## than a world.
##
## The live bug: a goblin parked at a 2-high rise with `blocked` at ZERO for eleven seconds, jumping
## at the same block while it re-planned the identical route head every few seconds
## (`nav_trace` 2026-11-27 14:59: `next=(-2, 247, 2)` on seventeen consecutive samples). The climb
## branch of `_walk` presses and jumps with no progress accounting at all -- `blocked` only counts
## in the branch for a step that is not climbable -- so the press was never the thing that ended;
## the re-plan was, and it answered with the same wall.
##
## What is under test is what the walker does with it: forty sim ticks at one cell with the gap
## barely moving is ten jump flights and no gain, so the route is dropped, and a re-plan that hands
## the same cell back is refused for a while rather than pressed at again.
##
##   probes/probe_rise_giveup.gd  (Godot --headless --path <project> --script res://probes/probe_rise_giveup.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## A body sim with no world in it: the feet, the tick count, and "nothing is solid". Every answer
## the rise clock asks for, and nothing that moves.
const TRACE_SCRIPT := """extends Node
var lines: Array[String] = []
var drops: Array[String] = []
func add_line(text: String) -> void:
	lines.append(text)
func note_drop(reason: String) -> void:
	drops.append(reason)
"""


## A chunk manager with the one answer a stray walk frame asks for: nothing collided.
const WORLD_SCRIPT := """extends Node
func resolve_voxel_collision(_at: Vector3, _by: Vector3, _body: AABB) -> Dictionary:
	return {}
"""

const STUB_SCRIPT := """extends RefCounted
var feet := Vector3.ZERO
var ticks := 0
func get_tick_position() -> Vector3:
\treturn feet
func get_position() -> Vector3:
\treturn feet
func is_in_water() -> bool:
\treturn false
func is_on_ground() -> bool:
\treturn true
func get_velocity() -> Vector3:
\treturn Vector3.ZERO
func is_solid_at(_x: int, _y: int, _z: int) -> bool:
\treturn false
func is_liquid_at(_x: int, _y: int, _z: int) -> bool:
\treturn false
func get_tick_count() -> int:
\treturn ticks
func consume_damage() -> float:
\treturn 0.0
func reset(at: Vector3) -> void:
\tfeet = at
func step(_delta: float, _wish: Vector3, _jump: bool, _sprint: bool, _sneak: bool, _yaw: float,
\t\t_speed: float) -> void:
\tpass
"""

var ok := true


func _initialize() -> void:
	# Deferred by one idle step: inside `_initialize` the tree is not yet the ACTIVE scene tree, so
	# the body's own `_ready` runs after this script's writes and puts its null chunk manager back.
	_run.call_deferred()


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
	# Under a Node3D parent: a body whose parent is the Window has no 3D transform at all.
	var stage := Node3D.new()
	stage.name = "Stage"
	root.add_child(stage)
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	stage.add_child(body)
	body.set_physics_process(false)
	body.set_process(false)
	var sim := _stub()
	body.set("_sim", sim)
	var world_script := GDScript.new()
	world_script.source_code = WORLD_SCRIPT
	world_script.reload()
	# ...and a NODE, because the goblin's own field is typed `Node`: a RefCounted assigned to it is
	# silently refused and the walker is left with null in the frames after this probe's text.
	var world := Node.new()
	world.name = "ChunkManager"
	world.set_script(world_script)
	stage.add_child(world)
	body.set("_chunk_manager", world)
	# ...and a recorder, because `_trace()` resolves its node by absolute path and a probe body
	# outside the running scene cannot, and because the drop line is half of what is under test.
	var trace_script := GDScript.new()
	trace_script.source_code = TRACE_SCRIPT
	trace_script.reload()
	var tracer := Node.new()
	tracer.name = "NavTrace"
	tracer.set_script(trace_script)
	stage.add_child(tracer)
	body.set("_trace_node", tracer)
	sim.set("feet", Vector3(0.5, 65.0, 0.5))
	var cell := Vector3(1, 64, 0)  # a rise: the block's top is 65, the body's feet are at 65

	# --- 1. Forty ticks at one rise with nothing gained drops the route --------
	body.set("_route", ([cell] as Array[Vector3]))
	body.set("_next", 0)
	body.set("_refused_head", Vector3.INF)
	body.set("_refused_until", 0)
	var early := false
	for i in 39:
		sim.set("ticks", i)
		early = bool(body.call("_rise_clock", sim.get("feet"), cell))
	var held_before := not (body.get("_route") as Array).is_empty()
	sim.set("ticks", 40)
	var late := bool(body.call("_rise_clock", sim.get("feet"), cell))
	var dropped := (body.get("_route") as Array).is_empty()
	var refused: Vector3 = body.get("_refused_head")
	print("PROBE give-up: replan before 40 ticks=%s, at 40=%s, route held before=%s dropped=%s "
		% [str(early), str(late), str(held_before), str(dropped)])
	if early or not held_before:
		_fail("a rise was given up on before its clock ran out")
	elif not late or not dropped:
		_fail("forty ticks at one rise with no gain did not drop the route (returned %s, route %d)"
			% [str(late), (body.get("_route") as Array).size()])
	elif refused != cell:
		_fail("the rise was dropped without being remembered as refused (%s)" % str(refused))
	var drops: Array = tracer.get("drops")
	var said := str(drops[0]) if drops.size() > 0 else "<nothing>"
	print("PROBE trace: drops=%d, said=%s" % [drops.size(), said])
	if drops.size() != 1 or said != "pressed at a rise it cannot climb":
		_fail("the drop was not recorded with its own reason (%d drops, %s)" % [drops.size(), said])

	# --- 2. The same rise handed back is refused, not pressed at again --------
	body.set("_route", ([] as Array[Vector3]))
	body.set("_next", 0)
	body.call("_take_route", {"nodes": PackedVector3Array([cell])})
	var reasked := (body.get("_route") as Array).size()
	print("PROBE re-plan: cells in a route that opens with the refused rise=%d (needs 0)"
		% reasked)
	if reasked != 0:
		_fail("a plan that opened with the rise it had given up on was walked again")

	# ...and the refusal expires: a world that changed under the body must not be refused forever.
	sim.set("ticks", 40 + 200)
	body.call("_take_route", {"nodes": PackedVector3Array([cell])})
	var later := (body.get("_route") as Array).size()
	print("PROBE refusal: cells after it expires=%d (needs 1)" % later)
	if later != 1:
		_fail("a rise stayed refused after its window (route %d)" % later)

	# --- 3. A press that gains restarts the clock ----------------------------
	# The gap shrinks a tenth a call, as a body sliding up to a step does: sixty ticks and no
	# give-up, because the press is WORKING.
	body.set("_route", ([cell] as Array[Vector3]))
	body.set("_next", 0)
	body.set("_refused_head", Vector3.INF)
	body.set("_refused_until", 0)
	var gave_up := false
	for i in 60:
		sim.set("ticks", i)
		sim.set("feet", Vector3(0.5 + 0.005 * float(i), 65.0, 0.5))
		if bool(body.call("_rise_clock", sim.get("feet"), cell)):
			gave_up = true
	print("PROBE progress: gave up while getting closer=%s (needs false)" % str(gave_up))
	if gave_up:
		_fail("a press that was closing on the rise was given up on")

	# --- 4. A different rise starts its own clock ----------------------------
	sim.set("ticks", 60)
	sim.set("feet", Vector3(0.5, 65.0, 0.5))
	var other := Vector3(0, 64, 1)
	body.set("_route", ([other] as Array[Vector3]))
	body.set("_next", 0)
	var fresh := bool(body.call("_rise_clock", sim.get("feet"), other))
	sim.set("ticks", 90)
	var soon := bool(body.call("_rise_clock", sim.get("feet"), other))
	print("PROBE new head: gave up at once=%s, after 30 ticks=%s (needs false, false)"
		% [str(fresh), str(soon)])
	if fresh or soon:
		_fail("a rise met for the first time inherited the last one's clock")

	stage.queue_free()
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
