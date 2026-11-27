extends SceneTree
## The goblin's LOOT RUN: the decision machine, not the pathing.
##
## A goblin with an empty main hand notices a dropped pile, walks to it at the "loot" pace,
## takes ONE item into its main hand, stands looking at it for three seconds, and then keeps
## it if it is a TOOL and throws it back if it is not (scripts/goblin.gd's LOOT_ constants;
## docs/gui-notes.md has the pose and docs/probes.md the row).
##
## The body cannot walk here and the clock is the probe's, because what is under test is a
## state machine rather than a walk: the body's own sim is replaced with a stub after `_ready`
## (the pattern probes/probe_goblin_corner.gd uses) whose tick count the probe advances one at
## a time, its chunk manager with one that answers `request_path` and nothing else, and the
## world's dropped items with a fake node -- so a pile can be put somewhere, made liquid or
## taken away on purpose.
##
## It boots the REAL world anyway, for two reasons. Whether an id is a tool is the item
## registry's answer (`BlockTextures.is_item_tool`) and only `VoxelEngineController` loads
## data/items.json. And the two NEW dropped-item methods want pinning against the real node
## before the fake takes its place: the search's radius and level, the skip on a fresh throw
## and on a pile lying in liquid, and that taking one item off a stack resizes that pile
## instead of collecting it.
##
##   probes/probe_goblin_loot_run.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_loot_run.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## The session's trace, muted: this probe spawns a body in the real world, and a body in the
## real world installs one. `user://nav_trace.txt` is a session's own file.
const NAV_TRACE := preload("res://scripts/nav_trace.gd")

## The probe's own copies of the numbers it drives to, read from the body where it can:
## these are the fallbacks for a body that does not answer them.
const LOOT_SCAN_TICKS := 10
const LOOT_INSPECT_TICKS := 60
const LOOT_ERRAND_TICKS := 200
const LOOT_IGNORE_TICKS := 200
const LOOT_RANGE := 8.0
const LOOT_LEVEL := 2.0

## A body with no world in it: where its feet are, and the tick count every loot clock runs on.
## `step` does not move it -- the probe places the feet itself, which is what lets a take be
## pinned without a walk.
const SIM_SCRIPT := """extends RefCounted
var feet := Vector3.ZERO
var ticks := 0
func get_tick_position() -> Vector3:
\treturn feet
func get_position() -> Vector3:
\treturn feet
func get_tick_count() -> int:
\treturn ticks
func is_on_ground() -> bool:
\treturn true
func is_in_water() -> bool:
\treturn false
func is_liquid_at(_x: int, _y: int, _z: int) -> bool:
\treturn false
func is_solid_at(_x: int, _y: int, _z: int) -> bool:
\treturn false
func get_velocity() -> Vector3:
\treturn Vector3.ZERO
func get_eye_height() -> float:
\treturn 1.4
func consume_damage() -> float:
\treturn 0.0
func shove(_attacker_position: Vector3, _extra: Vector3) -> bool:
\treturn true
func push(_velocity: Vector3) -> void:
\tpass
func reset(at: Vector3) -> void:
\tfeet = at
func step(_delta: float, _wish: Vector3, _jump: bool, _sprint: bool, _sneak: bool,
\t\t_yaw: float, _speed: float) -> void:
\tpass
"""

## The planner's chair: every ask is accepted with a job id, so an errand gets a route in
## flight (which is what the "loot" role in the trace means) and nothing ever walks.
const PLAN_SCRIPT := """extends Node
var jobs := 0
var expansions := 0
var max_ms := 0.0
func request_path(_from: Vector3, _to: Vector3, want_expansions: int, want_ms: float) -> int:
\tjobs += 1
\texpansions = want_expansions
\tmax_ms = want_ms
\treturn jobs
"""

## The piles, as the goblin sees them: the same four calls the world's node answers, over an
## array of {block_id, count, position, age, liquid}. The rules the world's own node keeps are
## repeated here on purpose -- this half of the probe is about what the GOBLIN does with the
## answer, and the node's own rules are pinned against the real one above.
const DROPPED_SCRIPT := """extends Node3D
var piles := []
var spawned := []
var takes := 0
func nearest_item(at: Vector3, radius: float, max_dy: float) -> Dictionary:
\tvar best := -1
\tvar best_flat := INF
\tfor i in piles.size():
\t\tvar p: Dictionary = piles[i]
\t\tif bool(p["liquid"]) or float(p["age"]) < 0.6:
\t\t\tcontinue
\t\tvar pos: Vector3 = p["position"]
\t\tif absf(pos.y - at.y) > max_dy:
\t\t\tcontinue
\t\tvar flat := Vector2(pos.x - at.x, pos.z - at.z).length()
\t\tif flat > radius or flat >= best_flat:
\t\t\tcontinue
\t\tbest = i
\t\tbest_flat = flat
\tif best < 0:
\t\treturn {}
\tvar found: Dictionary = piles[best]
\treturn {"block_id": int(found["block_id"]), "count": int(found["count"]),
\t\t"position": found["position"]}
func take_one(at: Vector3, radius: float, max_dy: float) -> Dictionary:
\tvar found := nearest_item(at, radius, max_dy)
\tif found.is_empty():
\t\treturn {}
\ttakes += 1
\tfor i in piles.size():
\t\tvar p: Dictionary = piles[i]
\t\tif int(p["block_id"]) == int(found["block_id"]) and p["position"] == found["position"]:
\t\t\tif int(p["count"]) > 1:
\t\t\t\tp["count"] = int(p["count"]) - 1
\t\t\telse:
\t\t\t\tpiles.remove_at(i)
\t\t\tbreak
\treturn {"block_id": int(found["block_id"]), "count": 1}
func spawn_at(block_id: int, count: int, at: Vector3, _scatter := 1.1) -> void:
\tspawned.append({"block_id": block_id, "count": count, "position": at})
func get_items() -> Array:
\treturn piles.duplicate(true)
"""

var ok := true
var _sim: RefCounted = null
var _body: Node3D = null
var _fake: Node = null
var _plan: Node = null
var _feet := Vector3.ZERO


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


## A fresh instance of one of the stub scripts above.
func _stub(text: String) -> Object:
	var script := GDScript.new()
	script.source_code = text
	script.reload()
	return script.new()


## One sim tick, then one behaviour tick: the loot clocks read the sim's own count, so the two
## have to move together or a deadline would never be reached.
func _tick(count := 1) -> void:
	for i in count:
		_sim.set("ticks", int(_sim.get("ticks")) + 1)
		_body.call("_advance_ticks", 1)


func _tick_until(limit: int, what: Callable) -> bool:
	for i in limit:
		_tick(1)
		if bool(what.call()):
			return true
	return false


## The top surface of a column, as the feet position standing on it.
func _surface(cm: Node, around: Vector3) -> Vector3:
	var x := floori(around.x)
	var z := floori(around.z)
	var y := floori(around.y) + 24
	while y > 0:
		if cm.get_block(x, y, z) != 0:
			return Vector3(float(x) + 0.5, float(y + 1), float(z) + 0.5)
		y -= 1
	return Vector3.INF


## Empty the ground through the node's own removal path: freeing the nodes directly leaves
## their entries in `_items`, and the next step writes a freed node.
func _clear(dropped: Node) -> void:
	var items: Array = dropped.get("_items")
	for i in range(items.size() - 1, -1, -1):
		dropped.call("_remove", i)


## One pile, where the probe wants it and past its pickup delay, on the REAL node: the solver
## would otherwise have it rising out of the throw it was spawned with by the next frame.
func _place(dropped: Node, block_id: int, count: int, at: Vector3) -> Dictionary:
	dropped.call("spawn_at", block_id, count, at, 0.0)
	var items: Array = dropped.get("_items")
	var item: Dictionary = items[items.size() - 1]
	item["position"] = at
	item["velocity"] = Vector3.ZERO
	item["spin"] = Vector3.ZERO
	item["age"] = 1.0
	return item


## One pile, in the fake node's own list.
func _put(id: int, at: Vector3, count := 1, liquid := false) -> void:
	_fake.set("piles", [{"block_id": id, "count": count, "position": at,
		"age": 1.0, "liquid": liquid}])


func _run() -> void:
	NAV_TRACE.muted = true
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var player := main.get_node_or_null("Player") as Node3D
	var cm := main.get_node_or_null("ChunkManager")
	var real := main.get_node_or_null("DroppedItems")
	if player == null or cm == null or real == null:
		print("PROBE FAIL: the world has no Player, ChunkManager or DroppedItems")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break

	# --- the ids, and the TOOL query --------------------------------------------
	# `is_item_tool` is the registry's own answer (ItemToolStats), so the two sides of it are a
	# tool that exists, and everything that is not one: another tool, plain items, a block, and
	# ids that are not items at all.
	var pickaxe := BlockTextures.get_block_id_by_name("wooden_pickaxe")
	var axe := BlockTextures.get_block_id_by_name("iron_axe")
	var pebble := BlockTextures.get_block_id_by_name("pebble")
	var coin := BlockTextures.get_block_id_by_name("silver_coin")
	var ear := BlockTextures.get_block_id_by_name("goblin_ear")
	var torch := BlockTextures.get_block_id_by_name("torch")
	var stick := BlockTextures.get_block_id_by_name("stick")
	var stone := BlockTextures.get_block_id_by_name("stone")
	var water := BlockTextures.get_block_id_by_name("water")
	print("PROBE item ids pickaxe=%d axe=%d pebble=%d coin=%d ear=%d torch=%d stick=%d stone=%d water=%d"
		% [pickaxe, axe, pebble, coin, ear, torch, stick, stone, water])
	if pickaxe <= 0 or axe <= 0 or pebble <= 0 or coin <= 0 or ear <= 0 or torch <= 0 \
			or stone <= 0 or water <= 0:
		print("PROBE FAIL: an item or block name does not resolve, so nothing here can be tested")
		quit(1)
		return
	if not ClassDB.class_has_method("BlockTextures", "is_item_tool"):
		_fail("BlockTextures has no is_item_tool (the DLL is older than this script)")
	if not BlockTextures.is_item_tool(pickaxe):
		_fail("wooden_pickaxe is not a tool")
	if not BlockTextures.is_item_tool(axe):
		_fail("iron_axe is not a tool")
	for id in [torch, pebble, coin, ear, stone, 0, -1, 999999]:
		if BlockTextures.is_item_tool(int(id)):
			_fail("id %d answers as a tool" % int(id))
	if stick > 0 and BlockTextures.is_item_tool(stick):
		_fail("a stick answers as a tool")

	# --- the two new methods, against the REAL node ------------------------------
	# The search is flat-radius plus a height bound, the nearest of them, and it looks past a
	# pile still inside its pickup delay and one lying in liquid.
	_clear(real)
	_feet = _surface(cm, player.global_position + Vector3(3.0, 0.0, 0.0))
	if _feet == Vector3.INF:
		print("PROBE FAIL: no ground beside the player")
		quit(1)
		return
	var near_at := _feet + Vector3(0.4, 0.3, 0.0)
	var far_at := _feet + Vector3(9.0, 0.3, 0.0)
	# A fresh throw is not a pile yet: inside PICKUP_DELAY there is nothing to find.
	_place(real, pebble, 1, near_at)
	var fresh: Array = real.get("_items")
	fresh[fresh.size() - 1]["age"] = 0.1
	if not real.call("nearest_item", _feet, LOOT_RANGE, LOOT_LEVEL).is_empty():
		_fail("nearest_item answers a pile that has only just been thrown")
	_clear(real)
	# Nine blocks out is past the radius: that pile on its own is not found from the feet, so
	# the radius really is the walk rather than a sphere that happens to be wide enough.
	_place(real, pebble, 1, far_at)
	if not (real.call("nearest_item", _feet, LOOT_RANGE, LOOT_LEVEL) as Dictionary).is_empty():
		_fail("nearest_item answers a pile nine blocks away")
	_clear(real)
	# The near one is the answer, and the one nine blocks out is not: the radius is the walk.
	var stack: Dictionary = _place(real, pebble, 3, near_at)
	_place(real, pebble, 1, far_at)
	var found: Dictionary = real.call("nearest_item", _feet, LOOT_RANGE, LOOT_LEVEL)
	if int(found.get("block_id", 0)) != pebble or int(found.get("count", 0)) != 3:
		_fail("nearest_item answered %s, not the pile of three beside the feet" % str(found))
	if (found.get("position", Vector3.ZERO) as Vector3).y > _feet.y + LOOT_LEVEL:
		_fail("nearest_item answered a pile above its own height bound")
	# ...and taking one off a stack RESIZES it rather than collecting it: count 3 -> 2, the same
	# pile in the same place, its body rebuilt to the smaller size.
	var before_half: float = (stack["half"] as Vector3).x
	var taken: Dictionary = real.call("take_one", _feet, LOOT_RANGE, LOOT_LEVEL)
	if int(taken.get("block_id", 0)) != pebble or int(taken.get("count", 0)) != 1:
		_fail("take_one answered %s, not one pebble" % str(taken))
	if int(stack["count"]) != 2:
		_fail("take_one left the pile at %d, not 2" % int(stack["count"]))
	if (stack["half"] as Vector3).x >= before_half:
		_fail("the decremented pile kept the body of the bigger one (half %.4f -> %.4f)"
			% [before_half, (stack["half"] as Vector3).x])
	if (real.call("get_items") as Array).size() != 2:
		_fail("taking one item off a stack took the whole pile out of the world")
	# The last one of a pile does take it out of the world: 3 -> 2 -> 1 -> gone, with the pile
	# nine blocks out untouched through all three takes.
	real.call("take_one", _feet, LOOT_RANGE, LOOT_LEVEL)
	real.call("take_one", _feet, LOOT_RANGE, LOOT_LEVEL)
	var left: Array = real.call("get_items")
	if left.size() != 1 or not ((left[0] as Dictionary)["position"] as Vector3).is_equal_approx(far_at):
		_fail("emptying a pile did not remove it (%d piles left)" % left.size())
	_clear(real)
	# A pile in LIQUID is not taken from: the take is from dry ground, and swimming loot is not
	# part of this. One cell of water is written, asked about in the same frame -- before the
	# fluid simulation has had a tick to spread it -- and put back.
	var cell := Vector3i(floori(_feet.x) + 1, floori(_feet.y), floori(_feet.z))
	var wet_at := Vector3(float(cell.x) + 0.5, float(cell.y) + 0.5, float(cell.z) + 0.5)
	cm.set_block(cell.x, cell.y, cell.z, water)
	_place(real, pebble, 1, wet_at)
	var wet: Dictionary = real.call("nearest_item", wet_at, LOOT_RANGE, LOOT_LEVEL)
	cm.set_block(cell.x, cell.y, cell.z, 0)
	if not wet.is_empty():
		_fail("nearest_item answers a pile lying in water")
	_clear(real)
	for i in 2:
		await process_frame

	# --- the body, and the stubs it talks to ------------------------------------
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("goblin")
	body.position = _feet
	main.add_child(body)
	await process_frame
	await process_frame
	_sim = _stub(SIM_SCRIPT) as RefCounted
	_sim.set("feet", _feet)
	body.set("_sim", _sim)
	body.set("_last_ticks", 0)
	body.set("_drawn_prev", _feet)
	body.set("_drawn_cur", _feet)
	_plan = _stub(PLAN_SCRIPT) as Node
	_plan.name = "PlanStub"
	main.add_child(_plan)
	body.set("_chunk_manager", _plan)
	# The clock is the probe's: the body's own tick would advance the sim's count under the
	# scripted one, and a deadline would arrive when the frame rate said so.
	body.set_physics_process(false)
	# A goblin with nothing to do wanders, and this one is driven a tick at a time.
	body.set("_idle_ticks", 1000000)
	_body = body
	# The world's own piles are the API checks' business; from here the goblin asks the fake.
	real.name = "RealDroppedItems"
	_fake = _stub(DROPPED_SCRIPT) as Node
	_fake.name = "DroppedItems"
	main.add_child(_fake)
	await process_frame

	# --- 1. a pile three blocks away is NOTICED ----------------------------------
	# The plan role is the speed state the ask was made in, so this is also how the errand is
	# labelled "loot" in nav_trace -- and the budget is the loot errand's, not a stroll's.
	var at_three := _feet + Vector3(3.0, 0.3, 0.0)
	_put(pebble, at_three)
	_tick(LOOT_SCAN_TICKS + 1)
	if String(body.get("speed_state")) != "loot":
		_fail("a pile three blocks away left it on '%s'" % body.get("speed_state"))
	if not (body.get("_loot_target") as Vector3).is_equal_approx(at_three):
		_fail("its errand is for %s, not the pile at %s"
			% [str(body.get("_loot_target")), str(at_three)])
	if String(body.get("_plan_role")) != "loot":
		_fail("the plan was asked for as '%s', not 'loot'" % body.get("_plan_role"))
	if int(_plan.get("jobs")) != 1:
		_fail("%d plans asked for one pile" % int(_plan.get("jobs")))
	if int(_plan.get("expansions")) != 12000 or absf(float(_plan.get("max_ms")) - 16.0) > 0.01:
		_fail("the errand was planned with %d expansions / %.1f ms, not the loot budget"
			% [int(_plan.get("expansions")), float(_plan.get("max_ms"))])

	# --- 2. a pile NINE blocks away is not ---------------------------------------
	body.call("_cancel_loot")
	_put(pebble, _feet + Vector3(9.0, 0.3, 0.0))
	var jobs := int(_plan.get("jobs"))
	_tick(LOOT_SCAN_TICKS + 2)
	if String(body.get("speed_state")) != "idle" or body.get("_loot_target") != Vector3.INF:
		_fail("a pile nine blocks away started an errand ('%s')" % body.get("speed_state"))
	if int(_plan.get("jobs")) != jobs:
		_fail("a pile nine blocks away was planned for")

	# --- 3. ...and neither is one in LIQUID --------------------------------------
	_put(pebble, at_three, 1, true)
	body.set("_loot_scan_ticks", 0)
	_tick(LOOT_SCAN_TICKS + 2)
	if String(body.get("speed_state")) != "idle" or body.get("_loot_target") != Vector3.INF:
		_fail("a pile in liquid started an errand ('%s')" % body.get("speed_state"))

	# --- 4. arrival takes exactly ONE, and starts the inspection ------------------
	var in_reach := _feet + Vector3(0.2, 0.3, 0.0)
	_put(pebble, in_reach, 2)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return int(body.get("main_hand_item")) > 0):
		_fail("it never took the pile it was standing on")
	else:
		if int(body.get("main_hand_item")) != pebble:
			_fail("it took %d, not the pebble %d" % [int(body.get("main_hand_item")), pebble])
		if int(body.get("_inspect_ticks")) != LOOT_INSPECT_TICKS:
			_fail("the inspection started at %d ticks, not %d"
				% [int(body.get("_inspect_ticks")), LOOT_INSPECT_TICKS])
		if String(body.get("speed_state")) != "idle":
			_fail("it is walking while standing still inspecting ('%s')" % body.get("speed_state"))
		if body.get("_loot_target") != Vector3.INF:
			_fail("the errand outlived the take")
		if int(_fake.get("takes")) != 1:
			_fail("%d takes for one pile" % int(_fake.get("takes")))
		# Two were lying there and ONE went into the hand: the pile is still down at one.
		var left_over: Array = _fake.get("piles")
		if left_over.size() != 1 or int(left_over[0]["count"]) != 1:
			_fail("it took %s, not exactly one of the two" % str(left_over))
	# The inspection holds the hand and the body for its three seconds: nothing else happens.
	_tick(20)
	if int(body.get("_inspect_ticks")) != LOOT_INSPECT_TICKS - 20:
		_fail("twenty ticks of inspection left %d, expected %d"
			% [int(body.get("_inspect_ticks")), LOOT_INSPECT_TICKS - 20])
	if int(body.get("main_hand_item")) != pebble:
		_fail("the item left its hand mid-inspection")

	# ...and a NON-TOOL is thrown back down once the look is over.
	_fake.set("spawned", [])
	_tick(LOOT_INSPECT_TICKS)
	var thrown: Array = _fake.get("spawned")
	if int(body.get("main_hand_item")) != 0:
		_fail("a pebble was kept (%d in hand)" % int(body.get("main_hand_item")))
	if thrown.size() != 1:
		_fail("%d things thrown down, expected exactly 1" % thrown.size())
	else:
		if int(thrown[0]["block_id"]) != pebble or int(thrown[0]["count"]) != 1:
			_fail("it threw %s, not one pebble" % str(thrown[0]))
		# The spot is remembered by POSITION -- a new body is built there, and a merge can
		# replace the node -- and it is the hand's own point.
		if not (thrown[0]["position"] as Vector3).is_equal_approx(body.get("_loot_ignore_at")):
			_fail("the rejected spot is %s but the item was dropped at %s"
				% [str(body.get("_loot_ignore_at")), str(thrown[0]["position"])])
		if int(body.get("_loot_ignore_until")) <= int(_sim.get("ticks")):
			_fail("the reject was not ignored for any time at all")

	# --- 5. the reject is not re-targeted inside the window, and is after it ------
	var reject_at: Vector3 = in_reach
	if thrown.size() == 1:
		reject_at = thrown[0]["position"]
	_put(pebble, reject_at)
	body.set("_loot_scan_ticks", 0)
	jobs = int(_plan.get("jobs"))
	var ticks_at_reject := int(_sim.get("ticks"))
	_tick(LOOT_SCAN_TICKS * 2)
	if int(_plan.get("jobs")) != jobs or body.get("_loot_target") != Vector3.INF:
		_fail("it picked its own reject straight back up")
	# Past the window it may be asked about again -- and the pile it just threw is still there,
	# so a plan going out again is the proof that the memory expired.
	_tick(LOOT_IGNORE_TICKS - (int(_sim.get("ticks")) - ticks_at_reject) + LOOT_SCAN_TICKS + 2)
	if int(_plan.get("jobs")) <= jobs:
		_fail("the rejected spot was never asked about again (ignore window never ran out)")
	body.call("_cancel_loot")
	body.set("main_hand_item", 0)
	_fake.set("spawned", [])

	# --- 6. a TOOL is KEPT, and a full main hand stops the looting ----------------
	_put(pickaxe, in_reach)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return int(body.get("main_hand_item")) > 0):
		_fail("it never took the pickaxe")
	_tick(LOOT_INSPECT_TICKS)
	if int(body.get("main_hand_item")) != pickaxe:
		_fail("a pickaxe was thrown away (%d in hand)" % int(body.get("main_hand_item")))
	if not (_fake.get("spawned") as Array).is_empty():
		_fail("a kept tool was dropped on the ground")
	if int(body.get("_inspect_ticks")) != 0:
		_fail("the inspection is still running after its ticks")
	if String(body.get("speed_state")) != "idle":
		_fail("a goblin holding a tool it just decided to keep is on '%s'"
			% body.get("speed_state"))
	# One carried item, no inventory: the scan does not run again while the hand is full.
	_put(pebble, at_three)
	var takes := int(_fake.get("takes"))
	body.set("_loot_scan_ticks", 0)
	_tick(LOOT_SCAN_TICKS * 3)
	if int(_fake.get("takes")) != takes or body.get("_loot_target") != Vector3.INF:
		_fail("a goblin with a full main hand started another loot errand")
	if String(body.get("speed_state")) != "idle":
		_fail("a goblin with a full main hand is on '%s'" % body.get("speed_state"))

	# --- 7. an ENEMY mid-errand cancels it ----------------------------------------
	body.set("main_hand_item", 0)
	body.call("_cancel_loot")
	body.set("_loot_ignore_at", Vector3.INF)
	_put(pebble, at_three)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return body.get("_loot_target") != Vector3.INF):
		_fail("it never noticed the pile before the fight")
	body.set("_enemy", player)
	body.set("_anger_ticks", 600)
	_tick(1)
	if body.get("_loot_target") != Vector3.INF:
		_fail("the loot errand survived a fight starting")
	if String(body.get("speed_state")) == "loot":
		_fail("it is still pacing at the loot run while fighting")
	body.call("_give_up")

	# --- 8. a HIT during the inspection drops the item -----------------------------
	body.set("main_hand_item", 0)
	body.call("_cancel_loot")
	_fake.set("spawned", [])
	_put(pebble, in_reach)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return int(body.get("main_hand_item")) > 0):
		_fail("it never took the pile it stood on for the interruption check")
	_tick(5)
	body.call("apply_knockback", _feet + Vector3(1.0, 0.0, 0.0))
	_tick(1)
	if int(body.get("main_hand_item")) != 0:
		_fail("a hit during the inspection left an undecided item in its hand")
	var dropped_by_hit: Array = _fake.get("spawned")
	if dropped_by_hit.size() != 1 or int(dropped_by_hit[0]["block_id"]) != pebble:
		_fail("an interrupted inspection dropped %s, not the pebble" % str(dropped_by_hit))
	if int(body.get("_inspect_ticks")) != 0:
		_fail("the inspection kept running after a hit")
	body.call("_give_up")

	# --- 9. an errand that never arrives times out ------------------------------
	# The body cannot walk here, so this is exactly the case: a plan in flight, the pile out of
	# reach, and ten seconds of nothing. It gives up on the place and stands.
	body.set("main_hand_item", 0)
	body.call("_cancel_loot")
	body.set("_loot_ignore_at", Vector3.INF)
	_put(pebble, at_three)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return body.get("_loot_target") != Vector3.INF):
		_fail("it never set off on the far pile")
	if String(body.get("speed_state")) != "loot":
		_fail("the errand it set off on is on '%s', not 'loot'" % body.get("speed_state"))
	_tick(LOOT_ERRAND_TICKS + 2)
	if body.get("_loot_target") != Vector3.INF:
		_fail("an errand that never arrived is still running")
	if String(body.get("speed_state")) != "idle":
		_fail("a timed-out errand left it on '%s'" % body.get("speed_state"))
	if int(body.get("_job")) != 0 or not (body.get("_route") as Array).is_empty():
		_fail("a timed-out errand kept its plan in flight")

	# --- 10. a take that comes back EMPTY does not hide the pile beside it --------
	# The pile it walked to is GONE when it gets there -- taken by the player, or merged into a
	# neighbour -- so the take answers nothing and the spot is forgotten. That memory is about
	# the pile and not the place: the survivor of the merge stands a block away, inside the wide
	# reach a rejected item is remembered at, and `nav_trace` 2026-11-27 21:26 has a goblin stand
	# the whole ten seconds of LOOT_IGNORE_TICKS beside exactly such a pile, with nothing to do.
	body.set("main_hand_item", 0)
	body.call("_cancel_loot")
	body.set("_loot_ignore_at", Vector3.INF)
	_put(pebble, in_reach)
	body.set("_loot_scan_ticks", 0)
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return body.get("_loot_target") != Vector3.INF):
		_fail("it never set off on the pile that was about to vanish")
	# The survivor: the same item a block and a half off -- out of the HAND's reach from here, so
	# this take really does come back empty, and inside the 2.0 a rejected item's memory covers.
	var survive_at := _feet + Vector3(1.9, 0.3, 0.0)
	_put(pebble, survive_at)
	jobs = int(_plan.get("jobs"))
	if not _tick_until(LOOT_SCAN_TICKS + 4,
			func(): return body.get("_loot_target") != Vector3.INF):
		_fail("an empty take left it standing beside a pile it could see")
	if not (body.get("_loot_target") as Vector3).is_equal_approx(survive_at):
		_fail("the errand after an empty take is for %s, not the pile beside it at %s"
			% [str(body.get("_loot_target")), str(survive_at)])
	if not (body.get("_loot_ignore_at") as Vector3).is_equal_approx(in_reach):
		_fail("the spot it found nothing at was not remembered at all")
	if int(_plan.get("jobs")) <= jobs:
		_fail("the pile beside the forgotten spot was never planned for")
	body.call("_cancel_loot")
	body.set("_loot_ignore_at", Vector3.INF)
	body.set("main_hand_item", 0)

	print("PROBE is_item_tool tells a tool from everything else; nearest_item/take_one search "
		+ "flat with a height bound and take one item off a pile; the loot run notices, walks "
		+ "at 'loot', takes one, keeps a tool and throws the rest back, and a spot it found "
		+ "empty hides only that pile and not its neighbours")
	print("PROBE PASS" if ok else "PROBE FAILED")
	quit(0 if ok else 1)
