extends SceneTree
## Is the third-person right arm posed like a hand holding something?
##
## The arm carries the block or item (scripts/player_model.gd: HELD_ARM,
## _init_held_item, _hold). While it carries one it sits 18 degrees forward -- a turn
## at the SHOULDER, which vanilla composes over the arm's own pose instead of writing
## into it, because the AnimationTree owns that local rotation and rewrites it every
## frame. So the turn lives on the arm's parent (`HeldArmPivot`), and this is the proof:
##
##   * the arm is still on its own joint -- the pivot is exactly where the model's arm
##     node was, the arm's local position is zero, and its rotation is untouched, so the
##     walk's keys are still measured from the pose they were measured from;
##   * the clips still reach the arm through the pivot, at vanilla's own swing (a track
##     left naming the old path would play as no motion at all, silently);
##   * the pivot is 18 degrees forward with something in the hand and 0 for an empty
##     hand, set beside the item's own visibility so the two cannot disagree;
##   * the held item MOVES FORWARD with it, by the angle a shoulder turn of 18 degrees
##     puts it at -- which is what fixes the sign (this rig faces +Z with the arm hanging
##     -Y, so a NEGATIVE turn about X is the one that leads);
##   * and with the body animating again the pivot holds that turn while the arm's own
##     rotation keeps moving under the tree's pose, so the turn is composed over it
##     rather than fighting it (or accumulating, which is what a write on the arm does
##     with the tree stopped).
##
## The world scene is loaded only because the block and item registries are loaded by
## the engine's own controller; the body measured here is a separate one under a stub
## owner, so nothing in the live world is touched. Still: a probe that boots a world
## goes through the runner, which snapshots and restores it.
##
##   probes/run_probe.sh probes/probe_held_pose.gd

const FORWARD_DEGREES := 18.0
const HELD_SWING_RATIO := 0.5      # vanilla halves the arm's walk swing while it holds
const PITCH_TOLERANCE := 0.5     # the item's turn about the shoulder, in degrees
const ARM_SWING_TOLERANCE := 0.02  # vanilla's 1.0 rad, as the clip's own base

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-56s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


## What a held item needs of its owner: which slot is selected, what is in it, and how
## fast the body is moving (so the body can pick a clip of its own). Nothing else.
class Stub extends Node3D:
	var block_id := 0
	var velocity := Vector3.ZERO

	func get_selected_hotbar_slot() -> int:
		return 0

	func get_hotbar_slot_block_id(_slot: int) -> int:
		return block_id

	func get_aim_direction() -> Vector3:
		return Vector3(0.0, 0.0, 1.0)

	func get_velocity() -> Vector3:
		return velocity


## The pivot's turn about X, in degrees -- the shoulder's pitch, and the one number this
## probe is about.
func _pitch_deg(pivot: Node3D) -> float:
	return rad_to_deg(pivot.rotation.x)


## The biggest turn each of the arm and the leg takes away from its own rest pose over
## `seconds`, in degrees -- what the body actually draws, through the tree.
func _swing_peaks(body: Node3D, seconds: float) -> Dictionary:
	var arm := body.find_child("arm", true, false) as Node3D
	var leg := body.find_child("leg", true, false) as Node3D
	var clip_rest: Dictionary = body.get("_clip_rest")
	var rest_arm: Quaternion = clip_rest["arm"]
	var rest_leg: Quaternion = clip_rest["leg"]
	var arm_peak := 0.0
	var leg_peak := 0.0
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame
		arm_peak = maxf(arm_peak, rad_to_deg(arm.quaternion.angle_to(rest_arm)))
		leg_peak = maxf(leg_peak, rad_to_deg(leg.quaternion.angle_to(rest_leg)))
	return {"arm": arm_peak, "leg": leg_peak}


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame


func _run() -> void:
	# The registries (block ids, item textures) are loaded by the engine's controller.
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: Main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	var stone := BlockTextures.get_block_id_by_name("stone")
	if stone <= 0:
		print("PROBE FAIL: the block registry has no 'stone' (got %d)" % stone)
		quit(1)
		return

	var glb: PackedScene = load("res://Assets/models/player.glb")
	if glb == null:
		print("PROBE FAIL: player.glb missing")
		quit(1)
		return

	# The model as authored: the arm's own seat and rest rotation, which the pivot takes
	# over and the arm gives up.
	var bare: Node3D = glb.instantiate()
	var bare_arm := bare.find_child("arm", true, false) as Node3D
	if bare_arm == null:
		print("PROBE FAIL: player.glb has no 'arm'")
		quit(1)
		return
	var seat: Vector3 = bare_arm.position
	var rest: Quaternion = bare_arm.quaternion
	bare.free()

	var host := Stub.new()
	host.name = "HeldPoseHost"
	root.add_child(host)
	var body: Node3D = glb.instantiate()
	body.set_script(load("res://scripts/player_model.gd"))
	# A still body for the measurements that need an exact number: the clips are not this
	# probe's subject (probe_player_anim is), and a swaying arm would move the item too.
	body.set("skip_head_look", true)
	host.add_child(body)
	await process_frame
	await process_frame

	var pivot := body.find_child("HeldArmPivot", true, false) as Node3D
	var arm := body.find_child("arm", true, false) as Node3D
	var item := body.get("_held_item") as MeshInstance3D
	if pivot == null or arm == null:
		print("PROBE FAIL: the body has no shoulder pivot / arm to measure")
		quit(1)
		return
	if item == null:
		print("PROBE FAIL: the body holds no item node (no world? no controller?)")
		quit(1)
		return

	_ok("arm: the pivot is in, and the arm hangs from it",
		pivot.get_parent() == body and arm.get_parent() == pivot,
		"pivot under '%s', arm under '%s'" % [pivot.get_parent().name, arm.get_parent().name])
	_ok("arm: the pivot took the arm's own seat",
		pivot.position.is_equal_approx(seat),
		"pivot %s against the model's arm %s" % [pivot.position, seat])
	_ok("arm: centred on the joint, its own rotation untouched",
		arm.position.is_equal_approx(Vector3.ZERO) and arm.quaternion.is_equal_approx(rest),
		"position %s, %.3f deg off the model's rest"
			% [arm.position, rad_to_deg(arm.quaternion.angle_to(rest))])

	# The clips still reach the arm: their track was re-homed onto the path the arm has
	# now, and its swing is still vanilla's 57 degrees of arm (the gains are keyed by the
	# track's path, so a track that changed path without a gain would quietly play small).
	var clips := body.find_child("Clips", false, false) as AnimationPlayer
	var run: Animation = clips.get_animation("run") if clips != null else null
	var arm_track := -1
	var track_paths := PackedStringArray()
	if run != null:
		for t in range(run.get_track_count()):
			var path := str(run.track_get_path(t))
			track_paths.append(path)
			if path.get_file() == "arm":
				arm_track = t
				break
	var peak := 0.0
	if arm_track >= 0:
		for k in range(run.track_get_key_count(arm_track)):
			var key: Quaternion = run.track_get_key_value(arm_track, k)
			peak = maxf(peak, (rest.inverse() * key).get_angle())
	_ok("clips: the arm's track follows it under the pivot, at vanilla's swing",
		arm_track >= 0 and absf(peak - 1.0) <= ARM_SWING_TOLERANCE,
		"track '%s' of %s, %.3f rad peak"
			% [str(run.track_get_path(arm_track)) if arm_track >= 0 else "-", track_paths, peak])

	# An empty hand is the model's own pose.
	_ok("empty hand: the shoulder is at rest",
		absf(_pitch_deg(pivot)) < 0.001 and not item.visible,
		"%.3f deg, item visible %s" % [_pitch_deg(pivot), item.visible])

	# ...and a block in it turns the shoulder forward, at the same moment the item is
	# shown: _hold does both, so they cannot disagree.
	host.block_id = stone
	await process_frame
	await process_frame
	_ok("held: the shoulder is 18 degrees forward",
		absf(_pitch_deg(pivot) + FORWARD_DEGREES) < 0.01 and item.visible,
		"%.3f deg about X, item visible %s" % [_pitch_deg(pivot), item.visible])

	# The turn moves the ITEM, and which way: the pivot is the shoulder, so the vector from
	# it to the item is the same vector turned by the shoulder's angle. Disabling the turn
	# for one frame gives the same arm pose unposed, which is what makes the comparison a
	# measurement rather than a restated formula.
	var origin := body.to_local(pivot.global_position)
	var posed := body.to_local(item.global_position)
	pivot.rotation_degrees = Vector3.ZERO
	await process_frame
	var unposed := body.to_local(item.global_position)
	pivot.rotation_degrees = Vector3(-FORWARD_DEGREES, 0.0, 0.0)
	await process_frame
	var turned := rad_to_deg((posed - origin).angle_to(unposed - origin))
	_ok("held: the item moves FORWARD with it, by that angle",
		posed.z > unposed.z and absf(turned - FORWARD_DEGREES) <= PITCH_TOLERANCE,
		"z %.4f -> %.4f (%.4f forward), %.2f deg about the shoulder"
			% [unposed.z, posed.z, posed.z - unposed.z, turned])

	# Back to an empty hand, through the production path.
	host.block_id = 0
	await process_frame
	await process_frame
	_ok("empty hand: the shoulder returns to rest and the item is hidden",
		absf(_pitch_deg(pivot)) < 0.001 and not item.visible,
		"%.3f deg, item visible %s" % [_pitch_deg(pivot), item.visible])

	# A body with nothing driving it -- a goblin, a preview, the K-key pose clone -- never
	# holds anything, so its shoulder stays at rest.
	var still: Node3D = glb.instantiate()
	still.set_script(load("res://scripts/player_model.gd"))
	still.set("skip_head_look", true)
	root.add_child(still)
	await process_frame
	var still_pivot := still.find_child("HeldArmPivot", true, false) as Node3D
	_ok("no controller: the shoulder stays at 0",
		still_pivot != null and absf(_pitch_deg(still_pivot)) < 0.001,
		"%.3f deg" % (_pitch_deg(still_pivot) if still_pivot != null else 0.0))
	still.queue_free()

	# ...and it composes with the tree rather than fighting it: animating again, the pivot
	# keeps its 18 degrees while the arm's own rotation keeps moving under the idle. A
	# turn written onto the arm would be replaced by the tree here, and would accumulate
	# with the tree stopped -- both of which this probe would read.
	body.set("skip_head_look", false)
	host.block_id = stone
	await process_frame
	var off_low := 999.0
	var off_high := -999.0
	var pivot_low := 999.0
	var pivot_high := -999.0
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 2500:
		await process_frame
		var off := rad_to_deg(arm.quaternion.angle_to(rest))
		off_low = minf(off_low, off)
		off_high = maxf(off_high, off)
		pivot_low = minf(pivot_low, _pitch_deg(pivot))
		pivot_high = maxf(pivot_high, _pitch_deg(pivot))
	_ok("held: the turn sits OVER the tree's own pose",
		off_high - off_low > 0.2 and absf(pivot_high - pivot_low) < 0.001
			and absf(pivot_high + FORWARD_DEGREES) < 0.01 and arm.get_parent() == pivot,
		"arm %.2f to %.2f deg off rest, shoulder %.3f to %.3f deg"
			% [off_low, off_high, pivot_low, pivot_high])

	# Vanilla's held-item rule has a second half that is a SCALING, not a turn: the arm's
	# walk swing is halved while that hand holds something. At a full swing (a quarter
	# block a tick) the item therefore moves through half the arc an empty fist does -- and
	# the legs do not change at all, because what a hand carries is not a gait.
	host.velocity = Vector3(0.25, 0.0, 0.0)
	host.block_id = 0
	await process_frame
	var empty: Dictionary = await _swing_peaks(body, 2.0)
	host.block_id = stone
	await process_frame
	var full: Dictionary = await _swing_peaks(body, 2.0)
	var ratio: float = float(full["arm"]) / maxf(float(empty["arm"]), 0.001)
	_ok("held: the walk's arm swing is HALVED, as vanilla's rule says",
		absf(ratio - HELD_SWING_RATIO) < 0.08,
		"%.1f deg against %.1f empty-handed (x%.2f)"
			% [float(full["arm"]), float(empty["arm"]), ratio])
	_ok("held: the legs are not touched by what the hand carries",
		absf(float(full["leg"]) - float(empty["leg"])) < 1.0,
		"%.1f deg against %.1f" % [float(full["leg"]), float(empty["leg"])])

	host.queue_free()
	main.queue_free()
	print("PROBE held pose: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
