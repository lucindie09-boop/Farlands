extends SceneTree
## Headless check of how the WALKER takes a corner the planner now allows: one side solid, the other
## open (a hole), the player's own crossing.
##
## The live bug: "he is able to cross if i go closer to him, but otherwise he kinda just stays stuck
## on the wall hes trying to go around". The trace (2026-11-25 23:2x) is six asks of
## `(-2, 249, -1) -> (-1, 249, -2)` in two seconds — one diagonal cell — every one of them answered
## found, and then dropped: `blocked by something it cannot climb`, `blocked=12`, `wish=(0.00,
## 0.00)`, `jumps=6`. The goblin was handed a legal move it could not walk and the same move again.
##
## A body cannot walk the CHORD across such a corner: from this cell's middle to the next cell's
## middle its own 0.6-wide box cuts the solid side by half a body. A player slides along the wall's
## face and crosses the moment the chord clears; the walker used to press straight into the wall.
## So what is under test, on the real script and a stub world:
##
##   1. a blocked diagonal keeps the DIAGONAL as its wish — the sim slides it along the wall's
##      face, which is the player's own line through that corner — instead of narrowing it to
##      the free half, and does not start counting blocked frames;
##   2. once the body has slid past the lip, the same diagonal is walked straight — both halves;
##   3. a squeeze, both beside cells solid, still counts blocked frames and re-plans;
##   4. only a diagonal slides: an orthogonal wish that is blocked is a wall, as before.
##
## The stub world is the walker's own question — "would half a pace forward be stopped?" — answered
## per axis over a set of solid cells, which is what `resolve_voxel_collision` reports. A real world
## would need the player's own seed, a settled disc and a built fixture to say the same thing.
##
##   probes/probe_goblin_corner.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_corner.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"

## A body with no world in it: the feet, whether it is standing, and where it is.
const SIM_SCRIPT := """extends RefCounted
var feet := Vector3.ZERO
var standing := true
func get_tick_position() -> Vector3:
	return feet
func get_position() -> Vector3:
	return feet
func is_in_water() -> bool:
	return false
func is_on_ground() -> bool:
	return standing
func get_velocity() -> Vector3:
	return Vector3.ZERO
var floors := {}
func is_solid_at(x: int, y: int, z: int) -> bool:
	return floors.has(Vector3i(x, y, z))
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

## The collision query the walker makes, over a set of solid CELLS. The engine sweeps the body's box
## from the feet along the delta; here the box is tested where the half pace ends, which is the same
## answer for anything the walk meets (a cell it was already inside is not a step), and each axis is
## swept on its own because that is what the two flags mean.
const WORLD_SCRIPT := """extends Node
var solid := {}
const HALF := 0.3
const TOP := 1.8
func _hits(at: Vector3) -> bool:
	for cx in range(floori(at.x - HALF), floori(at.x + HALF - 0.0001) + 1):
		for cy in range(floori(at.y), floori(at.y + TOP - 0.0001) + 1):
			for cz in range(floori(at.z - HALF), floori(at.z + HALF - 0.0001) + 1):
				if solid.has(Vector3i(cx, cy, cz)):
					return true
	return false
## The resolver's floor probe: is anything solid under the body's own footprint, one cell down?
## That is `on_floor`, and it is what a slide must not walk off. The half step asks it after a
## DOWNWARD motion (a step down is a move the plan may make), so only the horizontal part of the
## motion moves the footprint: this stub has one floor level, and "would it land" is the question.
func _floor_at(at: Vector3) -> bool:
	# Down to the drop a half step may take (`FALL_PROBE`): a body lands on the first floor within
	# it, so a floor three cells down answers as ground and nothing under it does not.
	for dy in range(1, 5):
		for cx in range(floori(at.x - HALF), floori(at.x + HALF - 0.0001) + 1):
			for cz in range(floori(at.z - HALF), floori(at.z + HALF - 0.0001) + 1):
				if solid.has(Vector3i(cx, floori(at.y) - dy, cz)):
					return true
	return false
func resolve_voxel_collision(origin: Vector3, delta: Vector3, _body: Vector3) -> Dictionary:
	return {
		"collided_x": _hits(origin + Vector3(delta.x, 0.0, 0.0)),
		"collided_z": _hits(origin + Vector3(0.0, 0.0, delta.z)),
		"on_floor": _floor_at(origin + Vector3(delta.x, 0.0, delta.z)),
	}
"""

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _scripted(script_text: String) -> Object:
	var script := GDScript.new()
	script.source_code = script_text
	script.reload()
	var out: Object = Node.new() if script_text == WORLD_SCRIPT else RefCounted.new()
	out.set_script(script)
	return out


## The fixture: everything at `cells` solid, plus the one floor cell the body stands on (a body has
## to be standing for any of this to be about a blocked step). The lip case leaves the floor out
## beyond that cell on purpose.
func _fixture(world: Object, cells: Array, floors: Array = []) -> void:
	var solid := {Vector3i(0, 63, 0): true}
	for cell in cells:
		solid[cell] = true
	for cell in floors:
		solid[cell] = true
	world.set("solid", solid)


func _route(cells: Array) -> Array[Vector3]:
	var out: Array[Vector3] = []
	for cell in cells:
		out.append(cell)
	return out


## One frame of `_walk` at these feet, answering the wish it left behind.
func _wish_at(body: Node3D, sim: RefCounted, feet: Vector3, route: Array, next: int = 0) -> Vector3:
	sim.set("feet", feet)
	body.set("_route", _route(route))
	body.set("_next", next)
	body.set("_wish", Vector3.ZERO)
	body.set("_jump", false)
	body.call("_walk")
	return body.get("_wish")


func _run() -> void:
	var stage := Node3D.new()
	stage.name = "Stage"
	root.add_child(stage)
	var world := _scripted(WORLD_SCRIPT) as Node
	world.name = "ChunkManager"
	stage.add_child(world)
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	stage.add_child(body)
	body.set_physics_process(false)
	body.set_process(false)
	# `_ready` runs on the tree's own pass, and it looks for the real ChunkManager under `/root/Main`
	# and finds nothing here -- so the stubs are handed over AFTER it, or it takes them back to null
	# and every step probe answers "null instance".
	await process_frame
	var sim: RefCounted = _scripted(SIM_SCRIPT) as RefCounted
	body.set("_sim", sim)
	body.set("_chunk_manager", world)

	# The user's own picture in cells: 2 is (0, 0) where the body stands, 3 is the diagonal (1, 1),
	# 1 is the WALL at (1, 0) two blocks up (one block would be a step it can jump) and 4 is the
	# open side, (0, 1) — a hole, no floor, plenty of air.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)

	# --- 1. The blocked diagonal slides along the open side -------------------
	var wish := _wish_at(body, sim, Vector3(0.5, 64.0, 0.5), [Vector3(1, 64, 1)])
	print("PROBE slide: wish=%s blocked_frames=%d (needs z dominant, x pressed, 0)"
		% [str(wish), int(body.get("_blocked_frames"))])
	if wish.length() < 0.5:
		_fail("a blocked diagonal left no wish at all (%s)" % str(wish))
	elif wish.z < 0.7 or wish.x < 0.4:
		_fail("the corner press is not free-axis-dominant with the wall pressed too (%s)" % str(wish))
	elif absf(wish.z) <= absf(wish.x):
		_fail("the pressed axis is the larger one, so the resolver tests it first (%s)" % str(wish))
	elif int(body.get("_blocked_frames")) != 0:
		_fail("a diagonal it can slide past started counting blocked frames")

	# ...and the wall is on the x side, so the slide must be z whichever side the wall is on.
	_fixture(world, [Vector3i(0, 64, 1), Vector3i(0, 65, 1)])
	var other := _wish_at(body, sim, Vector3(0.5, 64.0, 0.5), [Vector3(1, 64, 1)])
	print("PROBE slide: with the wall at (0,1) instead, wish=%s (needs x dominant, z pressed)" % str(other))
	if other.x < 0.7 or other.z < 0.4 or absf(other.x) <= absf(other.z):
		_fail("the slide does not follow the free side whichever side the wall is on (%s)" % str(other))

	# --- 2. Past the lip, the same diagonal is walked straight ----------------
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)])
	# z = 1.35: the body's box (1.05..1.65) no longer overlaps the wall's own (0..1), so the chord
	# is clear and there is nothing left to slide for.
	var through := _wish_at(body, sim, Vector3(0.5, 64.0, 1.35), [Vector3(1, 64, 1)])
	print("PROBE through: wish=%s (needs both halves)" % str(through))
	if through.x < 0.5 or through.z < 0.05:
		_fail("a corner the body has already passed was not walked diagonally (wish %s)"
			% str(through))

	# --- 3. A squeeze is still a wall ----------------------------------------
	# Both beside cells solid: no half is free, so it counts as before and re-plans — this is the
	# move the planner refuses too, and one the walker must not invent a slide for.
	_fixture(world, [
		Vector3i(1, 64, 0), Vector3i(1, 65, 0),
		Vector3i(0, 64, 1), Vector3i(0, 65, 1),
	])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	body.set("_replan_reason", "")
	for i in 12:
		_wish_at(body, sim, Vector3(0.5, 64.0, 0.5), [Vector3(1, 64, 1)])
	print("PROBE squeeze: blocked_frames=%d replan=%s reason=%s"
		% [int(body.get("_blocked_frames")), str(body.get("_needs_replan")),
			str(body.get("_replan_reason"))])
	if not bool(body.get("_needs_replan")):
		_fail("a squeeze between two solid columns never re-planned")
	elif str(body.get("_replan_reason")) != "blocked by something it cannot climb":
		_fail("the squeeze re-planned for the wrong reason (%s)" % str(body.get("_replan_reason")))

	# --- 4. Only a diagonal slides -------------------------------------------
	# An orthogonal wish into a wall has no half to narrow: it counts at once, as it always did.
	_fixture(world, [Vector3i(0, 64, 1), Vector3i(0, 65, 1)])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	var straight := _wish_at(body, sim, Vector3(0.5, 64.0, 0.5), [Vector3(0, 64, 1)])
	print("PROBE straight wall: wish=%s blocked_frames=%d (needs +z, 1)"
		% [str(straight), int(body.get("_blocked_frames"))])
	if int(body.get("_blocked_frames")) != 1:
		_fail("an orthogonal wish into a wall is counted as %d, not 1"
			% int(body.get("_blocked_frames")))
	elif straight.z < 0.9:
		_fail("an orthogonal wish stopped pointing at its cell (wish %s)" % str(straight))

	# --- 5. A slide off the edge is refused -----------------------------------
	# The wall on the x side, and the open side is a drop: the body stands on the last floor cell and
	# the half pace would put its whole footprint over air. That is the fall the trace has (three
	# blocks into water, `wish` flipping under the player), so the half must be refused and the
	# corner counted as a wall instead of walked off.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	var at_lip := _wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 64, 1)])
	print("PROBE lip: wish=%s blocked_frames=%d (needs the diagonal kept, no slide)"
		% [str(at_lip), int(body.get("_blocked_frames"))])
	if at_lip.x < 0.5 or absf(at_lip.z) > 0.7:
		_fail("a slide that walks off the drop was taken (wish %s)" % str(at_lip))
	elif int(body.get("_blocked_frames")) < 1:
		_fail("a corner with no floor on the open side was not counted as a wall")

	# --- 6. A step down is not a lip -----------------------------------------
	# The same lip, with the floor of the open side two blocks lower: a step down of that size is a
	# move the plan itself may make (`max_drop`), so the slide is taken and the body drops onto it.
	# Measured only at the body's own level, this floor would not exist and the corner would read as
	# a wall -- a goblin stopping at every drop it was meant to walk down.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)], [Vector3i(0, 62, 1)])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	var step_down := _wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 64, 1)])
	print("PROBE step down: wish=%s blocked_frames=%d (needs z dominant, x pressed, 0)"
		% [str(step_down), int(body.get("_blocked_frames"))])
	if step_down.z < 0.7 or step_down.x < 0.4:
		_fail("a slide onto a floor two blocks down was refused (%s)" % str(step_down))
	elif int(body.get("_blocked_frames")) != 0:
		_fail("a step-down slide counted as blocked")

	# --- 7. The ask starts on the cell it STANDS on ---------------------------
	# A body at the edge of a lip has its centre over the hole and is carried by the cells its box
	# overlaps. Asked from that hole column, the planner anchors the start on the pit floor and hands
	# back a route that begins three blocks below -- the walk then drops it as "the ground moved", or
	# walks it off the ledge (`nav_trace` 2026-11-25 23:49: `path=(-2, 245, -2)>...` from a body at
	# 249, `ground=false`, `vy=-0.52`). So the ask uses the cell with a floor under it.
	sim.set("floors", {Vector3i(0, 63, 0): true})
	var at_lip_cell: Vector3 = body.call("_standing_cell", Vector3(-0.5, 64.0, 0.5))
	print("PROBE stand: standing cell of a body at (-0.5, 64, 0.5) over a lip -> %s (needs (0, 64, 0))"
		% str(at_lip_cell))
	if at_lip_cell != Vector3(0.0, 64.0, 0.0):
		_fail("the plan would be asked from the hole column (%s)" % str(at_lip_cell))
	var on_its_own: Vector3 = body.call("_standing_cell", Vector3(0.5, 64.0, 0.5))
	print("PROBE stand: standing cell of a body in the middle of its own cell -> %s (needs (0, 64, 0))"
		% str(on_its_own))
	if on_its_own != Vector3(0.0, 64.0, 0.0):
		_fail("a body standing on its own cell asked from somewhere else (%s)" % str(on_its_own))

	# --- 7. A hop is a gap, not a fall ---------------------------------------
	# The planner's hop may descend up to `max_drop`, so a route can end two columns over and three
	# blocks down — into a pit. That is walked over the lip, not jumped at: hopping it was a goblin
	# bouncing at the mouth of the hole the player had fallen into (`jumps` 12 -> 15, `ground=false`
	# at y=250.25). One block down is still a gap to clear, so it stays a hop.
	var level_hop: bool = body.call("_hop_ahead", Vector3(2, 64, 0), Vector3(0.5, 64.0, 0.5))
	var down_one: bool = body.call("_hop_ahead", Vector3(2, 63, 0), Vector3(0.5, 64.0, 0.5))
	var down_three: bool = body.call("_hop_ahead", Vector3(-3, 246, -2), Vector3(-1.4, 249.0, -1.05))
	print("PROBE hop: level=%s one_down=%s three_down=%s (needs true, true, false)"
		% [str(level_hop), str(down_one), str(down_three)])
	if not level_hop:
		_fail("a level two-column move is no longer a hop")
	elif not down_one:
		_fail("a hop one block down stopped being a hop")
	elif down_three:
		_fail("a two-column move three blocks down was hopped instead of walked")

	# --- 8. The route may not start with the cell it stands ON --------------
	# At a lip the plan is asked from the cell that CARRIES the body (`_standing_cell`), and that
	# cell comes back as the route's first node. Walking to it is walking backwards, away from the
	# corner just pressed: eleven rounds of it in one trace, x pinned at the wall's face while z
	# climbed back a tenth at a time (`nav_trace` 2026-11-26 00:23).
	sim.set("floors", {Vector3i(-2, 63, -1): true})
	sim.set("feet", Vector3(-1.30, 64.0, -1.13))
	body.set("_planned_from", Vector3(-2.0, 64.0, -1.0))
	body.set("_job", 0)
	body.set("_route", [] as Array[Vector3])
	body.set("_next", 0)
	body.call("_take_route", {"found": true, "nodes": PackedVector3Array([
		Vector3(-2, 63, -1), Vector3(-1, 63, -2), Vector3(0, 63, -2)])})
	var route: Array = body.get("_route")
	print("PROBE ask from: route starts %s (needs the diagonal, not the cell behind)"
		% (str(route[0]) if not route.is_empty() else "nothing"))
	if route.is_empty():
		_fail("a plan whose first cell is the standing cell left no route at all")
	elif route[0] != Vector3(-1, 63, -2):
		_fail("the route starts at the cell it stands on (%s)" % str(route[0]))

	# --- 9. A blocked frame is a frame that made no progress -----------------
	# Pressing a corner shrinks the gap every tick while the free component slides the body along
	# the face, so those frames must not count towards the re-plan: counting them dropped the route
	# a tenth of a block short of the crossing. A squeeze makes no progress and still counts.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)])
	body.set("_blocked_frames", 0)
	body.set("_closest", 1.2)
	_wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 64, 1)])
	var closer_frames := int(body.get("_blocked_frames"))
	body.set("_blocked_frames", 0)
	body.set("_closest", 0.3)
	_wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 64, 1)])
	var stuck_frames := int(body.get("_blocked_frames"))
	print("PROBE progress: blocked frames closer=%d stuck=%d (needs 0, 1)"
		% [closer_frames, stuck_frames])
	if closer_frames != 0:
		_fail("a frame that got closer counted as blocked (%d)" % closer_frames)
	elif stuck_frames != 1:
		_fail("a frame with no progress counted as %d blocked frames" % stuck_frames)

	# --- 10. A route cut down to one cell is still a route -------------------
	# A two-node answer whose head is the standing cell leaves exactly one, and throwing that away
	# is a body mid-air over a hole with no wish at all: it fell three blocks into the pit instead of
	# stepping onto the far floor (`nav_trace` 2026-11-26 00:30).
	sim.set("floors", {Vector3i(-1, 63, -2): true})
	sim.set("feet", Vector3(-1.30, 64.0, -1.13))
	body.set("_planned_from", Vector3(-1.0, 64.0, -2.0))
	body.set("_job", 0)
	body.set("_route", [] as Array[Vector3])
	body.set("_next", 0)
	body.call("_take_route", {"found": true, "nodes": PackedVector3Array([
		Vector3(-1, 63, -2), Vector3(0, 63, -2)])})
	var short: Array = body.get("_route")
	print("PROBE trimmed: %d cell(s), starts %s (needs 1, (0, 63, -2))"
		% [short.size(), (str(short[0]) if not short.is_empty() else "nothing")])
	if short.size() != 1:
		_fail("a two-node answer trimmed to the standing cell left %d cells" % short.size())
	elif short[0] != Vector3(0, 63, -2):
		_fail("the surviving cell is not the destination (%s)" % str(short[0]))

	# --- 11. The corner cut: the free leg is taken although it has no floor under it
	# At the lip the free leg is short by design and ends over the FAR FLOOR, so `_stands_after`
	# refuses it -- and a corner is the one place where that guard is wrong. The combination that
	# allows it is "the other half is a wall AND this diagonal's destination is standable", which is a
	# corner cut and nothing else: without the far floor it is a drop, and is refused.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)], [Vector3i(1, 63, 1)])
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	sim.set("floors", {Vector3i(1, 63, 1): true})
	var cut_wish := _wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 63, 1)])
	print("PROBE corner cut: wish=%s blocked=%d (needs z dominant, x pressed, 0)"
		% [str(cut_wish), int(body.get("_blocked_frames"))])
	if cut_wish.z < 0.7 or cut_wish.x < 0.4:
		_fail("the corner cut was not taken as a free-dominant press (%s)" % str(cut_wish))
	elif int(body.get("_blocked_frames")) != 0:
		_fail("a corner cut that was taken counted a blocked frame")
	# ...and with no far floor it is a drop, and refused: the chord stays the wish.
	_fixture(world, [Vector3i(1, 64, 0), Vector3i(1, 65, 0)])
	sim.set("floors", {})
	body.set("_blocked_frames", 0)
	body.set("_needs_replan", false)
	var no_floor := _wish_at(body, sim, Vector3(0.5, 64.0, 0.9), [Vector3(1, 63, 1)])
	print("PROBE corner cut: no far floor -> wish=%s blocked=%d (needs the chord, 1)"
		% [str(no_floor), int(body.get("_blocked_frames"))])
	if absf(no_floor.x) <= absf(no_floor.z) * 0.5:
		_fail("a slide into a drop was taken as a corner cut (%s)" % str(no_floor))
	elif int(body.get("_blocked_frames")) < 1:
		_fail("a corner with no far floor was not counted as a wall")

	stage.queue_free()
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
