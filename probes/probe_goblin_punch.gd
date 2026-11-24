extends SceneTree
## Windowed check of the goblin as a punchable ENTITY, through the real C++ path:
## LMB -> PlayerController::try_punch_dummy -> the nearest body in `pose_clone` ->
## the goblin's own `apply_knockback` (the player sim's shove).
##
## Two goblins are spawned: the first is moved out of reach, the second stands at the
## aimed cell. The click has to land on the NEAREST one -- the group lists the far one
## first, which is exactly the case a group-order pick gets wrong -- and the far one
## must be untouched.
##
## Windowed: it needs a real mouse-mode capture and a real Input event, so run it with
##
##   probes/run_probe_shot.sh probes/probe_goblin_punch.gd

const TOOL_PATH := "res://scripts/goblin_spawn.gd"

var ok := true


func _initialize() -> void:
	_run()


func _fail(msg: String) -> void:
	ok = false
	print("PROBE FAIL: " + msg)


func _goblins(scene: Node) -> Array:
	var found: Array = []
	for node in scene.get_tree().get_nodes_in_group("goblin"):
		if scene.is_ancestor_of(node):
			found.append(node)
	return found


func _send_motion(relative: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.relative = relative
	root.push_input(motion)


## Point the player at a target. The C++ reads the look from mouse motion (yaw from
## relative.x, pitch from relative.y), so the probe measures the sensitivity once and
## then sends the difference -- and reads the new aim straight back, because the node
## applies the event as it arrives.
func _aim_at(player: Node3D, target: Vector3) -> void:
	var sens := 0.0
	for attempt in 6:
		var eye: Vector3 = player.call("get_aim_origin")
		var dir: Vector3 = player.call("get_aim_direction")
		var want: Vector3 = (target - eye).normalized()
		var err_yaw := wrapf(atan2(-want.x, -want.z) - atan2(-dir.x, -dir.z), -PI, PI)
		var err_pitch := asin(clampf(want.y, -1.0, 1.0)) - asin(clampf(dir.y, -1.0, 1.0))
		if absf(err_yaw) < 0.01 and absf(err_pitch) < 0.01:
			return
		if sens <= 0.0:
			var before := atan2(-dir.x, -dir.z)
			_send_motion(Vector2(100.0, 0.0))
			var moved: Vector3 = player.call("get_aim_direction")
			sens = absf(atan2(-moved.x, -moved.z) - before) / 100.0
			if sens <= 0.0:
				sens = 0.002
			continue
		_send_motion(Vector2(-err_yaw / sens, -err_pitch / sens))


func _click_left() -> void:
	var down := InputEventMouseButton.new()
	down.button_index = MOUSE_BUTTON_LEFT
	down.pressed = true
	# Pushed straight into the tree: a script-only run has no focused OS window for
	# the event queue to come back from.
	root.push_input(down)
	Input.parse_input_event(down)
	var up := InputEventMouseButton.new()
	up.button_index = MOUSE_BUTTON_LEFT
	up.pressed = false
	root.push_input(up)
	Input.parse_input_event(up)


## A body's position belongs to its movement sim, so a probe moves it the way the game
## does: the sim's own reset, which clears the velocity and any fall with it.
func _teleport(body: Node3D, pos: Vector3) -> void:
	body.call("teleport", pos)


func _run() -> void:
	root.add_child((load("res://Main.tscn") as PackedScene).instantiate())
	var main: Node = root.get_child(root.get_child_count() - 1)
	current_scene = main
	for i in 150:
		await process_frame
	var player := main.get_node_or_null("Player") as Node3D
	if player == null:
		_fail("no player")
		quit(1)
		return
	# The title screen is up over the live world and holds PlayerController's settings
	# flag -- which is what stops a click reaching the world. `_release` is the menu's
	# own "hand the screen back to the player", the call CONTINUE makes once the
	# curtain is down, so this is that state without the click through the menu.
	for child in root.get_children():
		if child is CanvasLayer and String(child.name) == "MenuLayer":
			child.call("_release")
			child.visible = false
	for i in 3:
		await process_frame
	player.call("set_settings_open", false)
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	await process_frame

	var loader := root.get_node_or_null("ToolLoader")
	var tool: Node = loader.open_tool("goblin")
	tool.call("spawn")
	await process_frame
	var first := _goblins(main)
	if first.size() != 1:
		_fail("%d goblins after the first spawn, expected 1" % first.size())
		quit(1)
		return
	var far: Node3D = first[0]
	# Out of punch reach, and FIRST in the group: a group-order pick would knock this
	# one instead of the one under the crosshair.
	var away: Vector3 = player.global_position + Vector3(0.0, 0.0, 8.0)
	_teleport(far, away)
	for i in 4:
		await process_frame
	print("PROBE far goblin at %s" % far.global_position)

	tool.call("spawn")
	for i in 2:
		await process_frame
	var bodies := _goblins(main)
	if bodies.size() != 2:
		_fail("%d goblins after the second spawn, expected 2" % bodies.size())
		quit(1)
		return
	var near: Node3D = null
	for body in bodies:
		if body != far:
			near = body
	if near == null:
		_fail("could not tell the two goblins apart")
		quit(1)
		return
	print("PROBE near goblin at %s" % near.global_position)
	var near_before: Vector3 = near.global_position
	var far_motion_before: Vector3 = far.call("get_velocity")

	# The crosshair starts on a cell seam (the spawn point is a world corner), where no
	# body centred in a cell can be hit: aim at the goblin itself, which is also what a
	# player does before punching one.
	_aim_at(player, near.global_position + Vector3(0.0, 0.9, 0.0))
	print("PROBE mouse mode before the click %d" % Input.mouse_mode)
	# The two gates ahead of the punch, measured the way the C++ measures them: the
	# block under the crosshair and the body box, both along the eye ray.
	var eye: Vector3 = player.call("get_aim_origin")
	var dir: Vector3 = player.call("get_aim_direction")
	var hit: Dictionary = main.get_node("ChunkManager").call("raycast_from_camera", 10.0)
	var block_dist := -1.0
	if hit.get("success", false):
		block_dist = (Vector3(hit["hit_point"]) - eye).length()
	print("PROBE block under the crosshair: %s at %.3f" % [hit.get("position", "none"), block_dist])
	var box_min := near.global_position - Vector3(0.3, 0.0, 0.3)
	var box_max := near.global_position + Vector3(0.3, 1.8, 0.3)
	print("PROBE eye %s dir %s box %s..%s" % [eye, dir, box_min, box_max])
	var vm := main.get_node_or_null("Player/Camera3D/Viewmodel")
	print("PROBE swing before %s" % (vm.get("_swing") if vm != null else "no viewmodel"))
	_click_left()
	for i in 3:
		await process_frame
	print("PROBE swing after %s" % (vm.get("_swing") if vm != null else "no viewmodel"))

	var near_motion: Vector3 = near.call("get_velocity")
	var far_motion: Vector3 = far.call("get_velocity")
	print("PROBE near motion %s (was %s), far motion %s" % [near_motion, near_before, far_motion])
	# dummy.gd's base knockback: +0.4 away from the attacker and +0.4 up, capped. A
	# couple of ticks later the horizontal half may already be eaten by a wall it was
	# shoved into, so the launch is read as "moving at all" or "upward", and the rest
	# is measured as travel below.
	if near_motion.y <= 0.0 and near.global_position.distance_to(near_before) < 0.2:
		_fail("the near goblin was not launched (motion %s, still at %s)" % [near_motion, near.global_position])
	if far_motion.y > far_motion_before.y + 0.1:
		_fail("the far goblin took the hit instead: the pick went by group order, not distance")

	# It is a body, so the knockback carries it: away from the player, horizontally.
	for i in 12:
		await process_frame
	var near_after: Vector3 = near.global_position
	var travelled := near_after - near_before
	var bearing := near_before - player.global_position
	if travelled.length() < 0.5:
		_fail("the near goblin did not move after the hit (%s -> %s)" % [near_before, near_after])
	var horizontal := Vector2(travelled.x, travelled.z).dot(Vector2(bearing.x, bearing.z))
	if horizontal <= 0.0:
		_fail("the goblin travelled %.2f toward the player, not away" % horizontal)
	print("PROBE near goblin flew %s -> %s (away %.2f)" % [near_before, near_after, horizontal])

	if ok:
		print("PROBE PASS: left click knocks back the goblin under the crosshair, "
			+ "through the same group and knockback the K dummy uses")
	else:
		print("PROBE FAILED")
	quit(0 if ok else 1)
