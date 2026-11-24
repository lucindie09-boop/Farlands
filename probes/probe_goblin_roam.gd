extends SceneTree
## Headless check of the goblin's idle wander: given somewhere to walk, it walks
## there over the ground, one step at a time, and then stands still again.
##
## The route itself is the ground planner's work, and `probes/probe_path.gd` covers
## that half. What is asked here is what the ENTITY does with a route: it steers at a
## stroll pace instead of teleporting along the cells, it arrives and stops instead of
## jittering on the last waypoint, it never stands where there is no ground under it,
## and a punch drops the route rather than letting the next tick steer over the
## knockback.
##
## Needs the real world and the real chunk map -- a route is only a route over
## generated terrain -- so it loads Main.tscn and waits for the player to land.
##
##   probes/probe_goblin_roam.gd  (Godot --headless --path <project> --script res://probes/probe_goblin_roam.gd)

const BODY_SCRIPT := "res://scripts/goblin.gd"
## Far enough to need several seconds of walking, and over the shoulder of the hill the
## player spawns beside, so the route has to descend rather than run flat.
const WALK_BLOCKS := 6
## A tick is STROLL_SPEED (0.09) and a frame can carry a couple of ticks plus the
## interpolation, so anything past this in one frame is a jump, not a walk.
const MAX_STEP := 0.6
## Budgets are counted in PHYSICS frames, not rendered ones: headless renders
## thousands of frames a second while a body takes only 20 ticks a second, so a frame
## budget says nothing about how far anything should have got.
const WALK_BUDGET := 1200
const SETTLE_FRAMES := 30
## The wander's own idle timer is silenced while the probe drives it, so the only
## strolls that happen are the ones it asks for.
const SILENCED_IDLE := 1000000

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


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


## Is there ground under any part of the body's box? A 0.6-wide body standing on the
## edge of a step has its centre over air and a shoulder over the block, and the
## collision resolver counts that as floor too; only a body with nothing under all of
## itself has fallen through the world.
func _supported(cm: Node, at: Vector3) -> bool:
	var y := floori(at.y) - 1
	for offset in [0.0, 0.3, -0.3]:
		for other in [0.0, 0.3, -0.3]:
			if cm.get_block(floori(at.x + offset), y, floori(at.z + other)) != 0:
				return true
	return false


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	var cm := main.get_node_or_null("ChunkManager")
	var player := main.get_node_or_null("Player") as Node3D
	if cm == null or player == null:
		_fail("the world has a ChunkManager and a Player")
		quit(1)
		return
	var waited := 0
	while waited < 4000:
		await process_frame
		waited += 1
		if waited > 60 and player.is_on_floor():
			break
	print("PROBE world ready after %d frames, player at %s" % [waited, player.global_position])

	# A goblin standing on the ground beside the player: the entity the spawn tool
	# makes, placed without needing the crosshair pointed at anything.
	var feet := _surface(cm, player.global_position + Vector3(2.0, 0.0, 0.0))
	if feet == Vector3.INF:
		_fail("no ground beside the player")
		quit(1)
		return
	var body := Node3D.new()
	body.name = "Goblin"
	body.set_script(load(BODY_SCRIPT))
	body.add_to_group("pose_clone")
	body.add_to_group("goblin")
	body.position = feet
	main.add_child(body)
	body.set("_idle_ticks", SILENCED_IDLE)
	for i in 4:
		await process_frame
	print("PROBE goblin at %s" % body.global_position)

	var goal := _surface(cm, feet + Vector3(float(WALK_BLOCKS), 0.0, 0.0))
	if goal == Vector3.INF:
		_fail("no ground %d blocks east" % WALK_BLOCKS)
		quit(1)
		return
	if int(body.call("walk_to", goal)) == 0:
		_fail("walk_to asked the planner for nothing")
		quit(1)
		return

	var asked := Engine.get_physics_frames()
	while Engine.get_physics_frames() - asked < 300 and (body.get("_route") as Array).is_empty():
		await process_frame
	var route: Array = body.get("_route")
	if route.is_empty():
		_fail("no route arrived in 300 physics frames")
		quit(1)
		return
	print("PROBE route of %d waypoints to %s" % [route.size(), route[route.size() - 1]])

	var start := body.global_position
	var last := start
	var biggest := 0.0
	var unsupported := 0
	var done_at := -1
	var begin := Engine.get_physics_frames()
	while Engine.get_physics_frames() - begin < WALK_BUDGET:
		await process_frame
		var at := body.global_position
		biggest = maxf(biggest, Vector2(at.x - last.x, at.z - last.z).length())
		last = at
		if bool(body.call("is_on_ground")) and not _supported(cm, at):
			unsupported += 1
		if done_at < 0 and (body.get("_route") as Array).is_empty():
			done_at = Engine.get_physics_frames()
		if done_at > 0 and Engine.get_physics_frames() - done_at > SETTLE_FRAMES:
			break

	var end := body.global_position
	var walked := Engine.get_physics_frames() - begin
	var travelled := Vector2(end.x - start.x, end.z - start.z).length()
	var left := Vector2(end.x - goal.x, end.z - goal.z).length()
	var motion: Vector3 = body.call("get_velocity")
	print("PROBE walked %.2f blocks over %d physics frames, %.2f from the target, "
		% [travelled, walked, left]
		+ "biggest step %.3f, motion %s" % [biggest, motion])
	if done_at < 0:
		_fail("the stroll never ended inside %d physics frames" % WALK_BUDGET)
	if travelled < 1.5:
		_fail("the goblin did not walk anywhere (%.2f blocks)" % travelled)
	if left > 2.0:
		_fail("the goblin stopped %.2f blocks from the target" % left)
	if biggest > MAX_STEP:
		_fail("one frame moved %.2f blocks: the route is being teleported along" % biggest)
	if unsupported > 0:
		_fail("it stood on nothing for %d frames" % unsupported)
	if absf(motion.x) > 0.01 or absf(motion.z) > 0.01:
		_fail("it kept walking after the route ended (motion %s)" % motion)
	# ...and then it stands there. The next stroll is a 1-in-120-a-tick roll, not a wait,
	# so the wander is silenced before this is measured -- otherwise the check would be a
	# coin toss on the roll landing inside the window it watches.
	body.set("_idle_ticks", SILENCED_IDLE)
	var held := body.global_position
	for i in 120:
		await process_frame
	var drifted := Vector2(body.global_position.x - held.x, body.global_position.z - held.z).length()
	if drifted > 0.05:
		_fail("a silenced goblin wandered anyway (%.2f blocks)" % drifted)

	# The wander's own start, which is the half the game actually runs and the half a
	# probe that drives `walk_to` by hand never reaches: the goblin's own pick has to
	# become a stroll, and the stroll has to be somewhere it can stand. A target aimed at
	# its own feet resolved to the inside of the hill and left it standing. Driven by
	# `stroll_now` rather than by waiting on the roll, because the roll is a chance and a
	# probe may not hang its verdict on one.
	var spot := body.global_position
	body.set("_idle_ticks", 0)
	if int(body.call("stroll_now")) == 0:
		_fail("stroll_now asked the planner for nothing")
	body.set("_idle_ticks", SILENCED_IDLE)
	var moved := 0.0
	begin = Engine.get_physics_frames()
	while Engine.get_physics_frames() - begin < 900:
		await process_frame
		moved = Vector2(body.global_position.x - spot.x, body.global_position.z - spot.z).length()
		if moved > 1.0:
			break
	print("PROBE the idle timer walked it %.2f blocks on its own" % moved)
	if moved <= 1.0:
		_fail("the goblin never left its spot on its own (%.2f blocks in 15 s)" % moved)

	# A shove interrupts the wander: the route is dropped, and the pause it takes is
	# what stops the next tick from steering over the knockback.
	var second := _surface(cm, body.global_position + Vector3(0.0, 0.0, 3.0))
	if second == Vector3.INF:
		_fail("no ground 3 blocks north of where it stopped")
	elif int(body.call("walk_to", second)) == 0:
		_fail("the second stroll asked the planner for nothing")
	else:
		asked = Engine.get_physics_frames()
		while Engine.get_physics_frames() - asked < 300 and (body.get("_route") as Array).is_empty():
			await process_frame
		if (body.get("_route") as Array).is_empty():
			_fail("the second stroll never got a route")
		else:
			body.call("apply_knockback", body.global_position + Vector3(0.0, 0.0, 2.0))
			await process_frame
			if (body.get("_route") as Array).is_empty():
				_fail("a punch dropped the route: a hit shoves a mob, it does not call it off")
			# And it carries on: the shove is a velocity, the stroll is the plan.
			var walking := false
			for i in 60:
				await process_frame
				var wish: Vector3 = body.get("_wish")
				if wish.length() >= 0.5:
					walking = true
					break
			if not walking:
				_fail("a punched goblin never steered again: a hit should not call the stroll off")

	if ok:
		print("PROBE PASS: the goblin walks a planned route at a stroll over solid "
			+ "ground, one step at a time, stops when it arrives, and a punch shoves it "
			+ "without calling the stroll off")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)
