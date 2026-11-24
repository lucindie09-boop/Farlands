extends SceneTree
## Does the goblin walk the way the player's body does?
##
## The goblin is drawn as the player's body wearing the goblin's mesh (scripts/
## goblin_model.gd): `Goblin2.obj`'s eleven surfaces split back into the six parts it
## was authored as, each hung from its own joint, and the player's own clips and blend
## tree turning them. So what is measured here is that the rig exists and stands on the
## floor, that a standing goblin plays the idle with its legs still, that a walking one
## strides 80 degrees of leg with the two legs opposed and its arms carrying the idle's
## drift, that stopping settles the legs without rewinding the idle, and that the speed
## it walks at is the GOBLIN's own -- read from the body it rides, never the player's.
##
## Nothing here reads a shot or writes to the world, so it runs headless:
##
##   probes/run_probe.sh probes/probe_goblin_anim.gd

const GOBLIN_MODEL := preload("res://scripts/goblin_model.gd")
const GOBLIN_MESH := preload("res://Assets/models/Goblin2.obj")
## Blocks per TICK, which is what a body's `get_velocity` answers: 0.25 is 5 blocks/s,
## the speed a full swing wants.
const FULL_SWING := 0.25


## The body the model rides: the goblin's own `get_velocity`, in blocks per tick. A
## model with no such ancestor stands at 0, which is the other half of the seam.
class Stub extends Node3D:
	var velocity := Vector3.ZERO

	func get_velocity() -> Vector3:
		return velocity


var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


## How far a part is turned away from the model's own rest pose, in degrees.
func _off_rest_deg(node: Node3D) -> float:
	return rad_to_deg(node.quaternion.angle_to(Quaternion.IDENTITY))


## How far an arm's hang is rolled out from the model's rest, in degrees about Z: the
## idle's own motion, which a walk must not flatten into a constant.
func _roll_deg(node: Node3D) -> float:
	var e := node.quaternion.get_euler()
	return rad_to_deg(e.z)


func _run() -> void:
	var host := Stub.new()
	root.add_child(host)

	# --- the rig --------------------------------------------------------------
	var model := Node3D.new()
	model.set_script(GOBLIN_MODEL)
	model.set("source_mesh", GOBLIN_MESH)
	host.add_child(model)
	await process_frame

	var parts := {}
	for part in ["arm", "arm2", "leg", "leg2", "torso", "head"]:
		var mi := model.get_node_or_null(NodePath(part)) as MeshInstance3D
		if mi == null or mi.mesh == null:
			_failures += 1
			print("PROBE FAIL: the rig has no `%s` part" % part)
			continue
		parts[part] = mi
	_ok("rig: the obj's eleven surfaces are six hung parts", parts.size() == 6,
		"%d of 6 -- %s" % [parts.size(), str(parts.keys())])
	if parts.size() != 6:
		print("PROBE goblin animation: %d failure(s)" % _failures)
		quit(1)
		return

	var union := AABB()
	var any := false
	for part in parts:
		var mi: MeshInstance3D = parts[part]
		var box: AABB = (mi.transform * (mi.mesh as Mesh).get_aabb())
		union = box if not any else union.merge(box)
		any = true
	_ok("rig: it stands on the floor, 1.44 blocks tall",
		absf(union.position.y) < 0.01 and absf(union.size.y - 1.44) < 0.02,
		"feet at %.3f, height %.3f" % [union.position.y, union.size.y])
	# A joint is what a part turns about, so it is where the clip's angles are meant to
	# be applied: the hips are the tops of the leg boxes, the shoulders the tops of the
	# arm boxes on the body's side.
	var leg_hip := (parts["leg"] as Node3D).position
	var arm_shoulder := (parts["arm2"] as Node3D).position
	_ok("rig: the legs hang from the hips, the arms from the shoulders",
		absf(leg_hip.y - 0.44) < 0.01 and absf(arm_shoulder.y - 1.0) < 0.01,
		"hip y %.2f, shoulder y %.2f" % [leg_hip.y, arm_shoulder.y])

	# --- the player's clips, on the goblin's rig ------------------------------
	var clips := model.find_child("Clips", false, false) as AnimationPlayer
	var tree := model.find_child("ClipsTree", false, false) as AnimationTree
	var named := clips != null and clips.has_animation("idle") \
		and clips.has_animation("run") and clips.has_animation("rest")
	_ok("clips: the goblin carries the player's idle, run and rest", named,
		"%s" % (str(clips.get_animation_list()) if clips != null else "no Clips node"))
	var rest: Dictionary = model.get("_clip_rest")
	var all_resolved := rest.size() == 6
	for part in ["arm", "arm2", "leg", "leg2", "torso", "head"]:
		all_resolved = all_resolved and rest.has(part)
	_ok("clips: every part of the rig has a rest pose to play on", all_resolved,
		"%d of 6 in _clip_rest" % rest.size())

	# --- standing -------------------------------------------------------------
	var leg := parts["leg"] as Node3D
	var leg2 := parts["leg2"] as Node3D
	var arm := parts["arm"] as Node3D
	host.velocity = Vector3.ZERO
	var still := 0.0
	var roll_lo := 999.0
	var roll_hi := -999.0
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 900:
		await process_frame
		still = maxf(still, maxf(_off_rest_deg(leg), _off_rest_deg(leg2)))
		var roll := _roll_deg(arm)
		roll_lo = minf(roll_lo, roll)
		roll_hi = maxf(roll_hi, roll)
	_ok("idle: a standing goblin leaves its legs alone",
		still < 0.5 and model.get("_clip") == "idle",
		"leg %.3f deg, clip '%s'" % [still, model.get("_clip")])
	_ok("idle: and keeps breathing -- the arms' roll moves", roll_hi - roll_lo > 0.05,
		"roll %.2f to %.2f deg" % [roll_lo, roll_hi])

	# --- walking --------------------------------------------------------------
	# The walk amount is the body's own speed: FULL_SWING blocks a tick is 5 blocks/s,
	# where the swing is whole.
	host.velocity = Vector3(FULL_SWING, 0.0, 0.0)
	var speed := float(model.call("_walk_speed"))
	_ok("walk: the speed is read off the goblin's own body",
		absf(speed - FULL_SWING * 20.0) < 0.01,
		"%.2f blocks/s from %.2f blocks/tick" % [speed, FULL_SWING])
	var peak := 0.0
	var walk_roll_lo := 999.0
	var walk_roll_hi := -999.0
	var opposed := 0
	started = Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 1600:
		await process_frame
		peak = maxf(peak, _off_rest_deg(leg))
		var roll := _roll_deg(arm)
		walk_roll_lo = minf(walk_roll_lo, roll)
		walk_roll_hi = maxf(walk_roll_hi, roll)
		if absf(_off_rest_deg(leg)) > 5.0 and leg.quaternion.is_equal_approx(leg2.quaternion):
			opposed += 1
	var swing := float(model.get("_swing"))
	_ok("walk: the legs reach the player's own 80 degrees", peak > 75.0 and peak < 85.0,
		"%.1f deg of leg, walk amount %.3f" % [peak, swing])
	_ok("walk: the two legs are opposed", opposed == 0, "%d sample(s) together" % opposed)
	_ok("walk: the arms still carry the idle over the stride",
		walk_roll_hi - walk_roll_lo > 1.0,
		"roll span %.2f deg while the leg reaches %.0f" % [walk_roll_hi - walk_roll_lo, peak])

	# --- stopping -------------------------------------------------------------
	host.velocity = Vector3.ZERO
	started = Time.get_ticks_msec()
	var settled := 0.0
	var stop_lo := 999.0
	var stop_hi := -999.0
	while Time.get_ticks_msec() - started < 1400:
		await process_frame
		# Measured once the ease has played out: 40% a tick takes about 0.4 s to rest.
		if Time.get_ticks_msec() - started > 700:
			settled = maxf(settled, maxf(_off_rest_deg(leg), _off_rest_deg(leg2)))
		var roll := _roll_deg(arm)
		stop_lo = minf(stop_lo, roll)
		stop_hi = maxf(stop_hi, roll)
	_ok("stop: the legs settle onto rest", settled < 1.0, "%.2f deg off rest" % settled)
	_ok("stop: the idle is not rewound -- it breathes straight through",
		stop_hi - stop_lo > 0.05, "roll %.2f to %.2f deg" % [stop_lo, stop_hi])

	# --- the idle look: the head turns, the body does not ----------------------
	# A standing mob looks at things: the point is near it, the head turns to it inside
	# what a neck allows, and the body -- which only faces where it walks -- does not move.
	var head := parts["head"] as Node3D
	var model_yaw := model.rotation.y
	var eye := model.global_position + Vector3(0.0, 1.0, 0.0)
	model.call("look_at_point", eye + Vector3(2.0, 0.0, -2.0))
	for i in 90:
		await process_frame
	var turned := rad_to_deg(head.quaternion.get_euler().y)
	_ok("look: the head turns to the point it was given",
		absf(turned) > 30.0 and absf(turned) < 60.0, "%.1f deg off the rig's rest" % turned)
	_ok("look: the body does not turn with it", is_equal_approx(model.rotation.y, model_yaw),
		"model yaw %.3f" % model.rotation.y)
	# Behind it, a neck clamps rather than following the point round.
	model.call("look_at_point", eye + Vector3(0.0, 0.0, 3.0))
	for i in 90:
		await process_frame
	turned = rad_to_deg(head.quaternion.get_euler().y)
	_ok("look: a point behind it is clamped to what a neck does",
		absf(turned) > 60.0 and absf(turned) < 76.0, "%.1f deg" % turned)
	# Straight ahead again: it settles back onto the rig's own rest.
	model.call("look_at_point", eye + Vector3(0.0, 0.0, -3.0))
	for i in 120:
		await process_frame
	turned = rad_to_deg(head.quaternion.get_euler().y)
	_ok("look: and it comes back to rest when the look comes back ahead",
		absf(turned) < 5.0, "%.1f deg" % turned)
	# A glance that has run out is a head going back to straight ahead, which is where a mob
	# reads most of the time: `clear_look` stops asking, and the rest of the rig's own pose
	# is the target.
	model.call("look_at_point", eye + Vector3(2.0, -1.5, -2.0))
	for i in 60:
		await process_frame
	model.call("clear_look")
	for i in 120:
		await process_frame
	var euler := head.quaternion.get_euler()
	_ok("look: with nothing picked the head settles straight ahead",
		absf(rad_to_deg(euler.y)) < 1.0 and absf(rad_to_deg(euler.x)) < 1.0,
		"yaw %.2f, pitch %.2f" % [rad_to_deg(euler.y), rad_to_deg(euler.x)])
	# ...and it turns about its own neck. The joint is read from the head's box alone: with
	# the ears, chin and nose averaged in it sat at y 0.94 and z -0.155, which is a chin's
	# height and a nose's depth, and a head turning about that point sweeps instead of
	# turning where it sits.
	var joint := head.position
	_ok("rig: the head turns about its neck, not about what is attached to it",
		absf(joint.y - 1.0) < 0.01 and absf(joint.z + 0.09) < 0.01 and absf(joint.x + 0.03) < 0.01,
		"joint %s (the union gave y 0.94, z -0.155)" % str(joint))
	# The look a WALKING goblin wears: goblin.gd aims two blocks ahead and 22.5 degrees below
	# the horizontal from its neck, and what the head does with that point is that tilt -- the
	# pipeline from `_pick_walk_look`'s geometry to a pitch on the neck, end to end.
	var neck := model.global_position + Vector3(0.0, 1.0, 0.0)
	var down_tan := tan(0.3927)
	model.call("look_at_point", neck + Vector3(0.0, -2.0 * down_tan, -2.0))
	for i in 90:
		await process_frame
	var walk_pitch := rad_to_deg(head.quaternion.get_euler().x)
	_ok("look: the walk's downward glance reads 20-25 degrees",
		walk_pitch < -20.0 and walk_pitch > -25.0, "%.1f deg" % walk_pitch)

	# The turn is a RATE, not an ease: a neck moves 10 degrees a tick, so a jump to a new point
	# 	is travelled at one speed instead of racing off and creeping into the finish. Driven
	# a frame at a time with a known delta, so the rate is what is measured.
	model.set_process(false)
	model.set("_head_delta", 1.0 / 60.0)
	model.call("look_at_point", eye + Vector3(0.0, 0.0, -3.0))
	for i in 120:
		model.call("_track_head_look")
	model.call("look_at_point", eye + Vector3(-3.0, 0.0, 0.0))
	# Three frames at a sixtieth are the first 20 Hz tick, so this is one tick's worth of
	# neck -- and the drawn head is a little way onto it rather than all the way.
	for i in 3:
		model.call("_track_head_look")
	var one_tick := rad_to_deg(head.quaternion.get_euler().y)
	_ok("look: a neck turns 10 degrees a tick, it does not snap there",
		absf(one_tick) > 3.0 and absf(one_tick) < 12.0, "%.2f deg after one tick" % one_tick)
	for i in 57:
		model.call("_track_head_look")
	var arrived := rad_to_deg(head.quaternion.get_euler().y)
	_ok("look: and a full turn takes about a third of a second",
		absf(arrived) > 70.0 and absf(arrived) < 76.0, "%.1f deg after a second of frames" % arrived)

	host.queue_free()
	print("PROBE goblin animation: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
