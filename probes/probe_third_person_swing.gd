extends SceneTree
## Does the third-person main hand swing when the first-person one does -- and does it swing on
## an ARC, or just hinge up?
##
## A punch and a block place start ONE stroke on the body's right arm (scripts/player_model.gd:
## `SwingPivot`, `_apply_hand_swing`), the held block or item rides it because it hangs off that
## arm, and the legs, torso and head keep playing whatever they were playing. The triggers are
## the first-person hand's own (`viewmodel.gd` emits `swung(strength)`) and the body only plays
## them, so this measures both halves:
##
##   * on the REAL player's body, that the wire exists and a punch/place reaches it at full /
##     weaker weight -- a body that began listening after `_ready` would silently never swing;
##   * on a still body under a stub owner (probe_held_pose's shape), the stroke itself, taken on
##     the arm's DIRECTION in the body's own frame (shoulder to hand) because that is what a
##     player sees move, and measured against the direction the arm RESTS in -- which is not
##     straight down, because the held pose already turns it 18 degrees forward.
##
## What is being separated here is a punch from a hinge. A pitch and nothing else takes the hand
## up the body's own plane and back down it: the same line in both directions, however far it
## goes. The stroke is the first-person hand's construction (src/core/viewmodel_math.cpp): a bell
## of travel, plus a SWEEP that is zero at the top of the stroke and full either side of it, so
## the hand goes out one side and comes back the other. That is what these checks hold -- the
## hand reaches sideways at all (a hinge keeps that at 0), and the way back is a different side
## from the way out, which is a closed loop rather than a retraced line.
##
## Also held: a second punch restarts the stroke from the top and lands on the same far end
## rather than past it, the arm comes back to exactly the rotation it started in, the item keeps
## its distance from the shoulder all the way out, and a walking body keeps its stride, its arm
## and its 18-degree held turn through the stroke.
##
## The world scene is loaded only because the block and item registries are loaded by the
## engine's own controller; nothing in the live world is written, so this runs headlessly:
##
##   probes/run_probe.sh probes/probe_third_person_swing.gd

## The stroke's numbers, restated here on purpose: the probe is what holds them, so it must not
## read them back from the file it is checking.
const ARC_DEGREES := 84.0
## The reference's `getCurrentSwingDuration()`: six ticks. Restated because this probe measures a
## stroke in SECONDS as well as in fractions of one, and the two are not the same number.
const SWING_SECONDS := 0.3
## The lateral terms are MIRRORED from the reference here, by choice (see `HAND_SWING_LATERAL` in
## scripts/player_model.gd): the reference opens the arm outboard and across its own side line, and
## this body's blow comes inwards across the chest. The rise, the aim and the clocks are the
## reference's own and are NOT mirrored, so this is the one number in the stroke that is ours.
const LATERAL := -1.0
## ...and the one thing this stroke adds: a short outward flick at the start of it
## (`HAND_SWING_START_OUT`, `HAND_SWING_START_SPAN` in scripts/player_model.gd), on top of the
## reference's lateral motion and changing nothing else about it.
const START_OUT := 0.15
const START_SPAN := 0.2
## ...and these are the reference's numbers, at the file's values: raising either one was tried and
## reverted, because its two terms peak at DIFFERENT points of the stroke (the rise at a sixth of
## the way in, the aim halfway), so a bigger aim term drags the maximum late and flattens the top
## into a rise-hold-return instead of one apex. Restated here so a change to either is a failure.
const LIFT := 1.2
const REACH := 0.75
const RISE_MAX := 2.6
const PLACE_SHARE := 0.75
const HELD_PITCH := 18.0
const TOLERANCE := 4.0

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


## What a held body needs of its owner: which slot is selected, what is in it, and how fast the
## body is moving (so the body can pick a clip of its own). Nothing else.
class Stub extends Node3D:
	var block_id := 0
	var velocity := Vector3.ZERO
	# Where the body is aiming: level by default, so every number below is a level-look one.
	# Pitched, it is what the stroke's head-pitch term reads.
	var aim := Vector3(0.0, 0.0, 1.0)

	func get_selected_hotbar_slot() -> int:
		return 0

	func get_hotbar_slot_block_id(_slot: int) -> int:
		return block_id

	func get_aim_direction() -> Vector3:
		return aim

	func get_velocity() -> Vector3:
		return velocity


## Where the hand is, in the BODY's own frame: shoulder to hand, unit length. Everything below
## is taken on this vector, because it is what a player sees the arm do.
func _hand_dir(body: Node3D, arm: Node3D) -> Vector3:
	var shoulder := body.to_local(arm.global_position)
	var down_the_limb := body.to_local(arm.to_global(Vector3(0.0, -1.0, 0.0)))
	return (down_the_limb - shoulder).normalized()


## The stroke swept by its own CLOCK, for the checks that are about what the stroke READS rather
## than how it lands in real time: the highest the item climbs above its rest height, and the
## chest's turn at the reference's own early peak and at the very end.
##
## The clock is set and `_apply_hand_swing(0.0)` written by hand, so no frame runs between a sample
## and its reading -- the same courtesy the capture gives itself, because a stroke this short is
## 0.07 of its way along per frame and a "sample" would otherwise be a moving average.
##
## The two chest numbers are the reference's clock DIRECTION made visible: its `attackTime` counts
## up from 0 and the twist reads `sin(sqrt(attackTime) * 2pi)`, which is already a quarter turn in
## at 1/16 of the stroke, so the body is most turned round almost at once and home by the end. The
## mirrored reading (`sqrt(1 - t)`) puts those two numbers the other way round.
func _sweep(body: Node3D, arm: Node3D, item: Node3D) -> Dictionary:
	var rest_y: float = body.to_local(item.global_position).y
	var climbed := -1000.0
	var chest_early := 0.0
	var chest_last := 0.0
	# The arm's OWN elevation at a fifth of the way in, which is the number that says where the
	# blow is pointing. Taken at a fixed point of the stroke rather than at the highest one:
	# looking further up runs the arm into its own ceiling, where a maximum cannot rise any more
	# but the direction at any given instant still does.
	var elev_mid := 0.0
	# The whole elevation profile, because "it looks like two points" is a claim about the SHAPE of
	# the rise: one apex, and no long sit near it. Counted as samples within 3 degrees of the top.
	var elevs: Array = []
	for step in 21:
		var p := float(step) / 20.0
		body.set("_hand_swing_strength", 1.0)
		body.set("_hand_swing", 1.0 - p)
		body.call("_apply_hand_swing", 0.0)
		climbed = maxf(climbed, body.to_local(item.global_position).y - rest_y)
		var elev := rad_to_deg(asin(clampf(_hand_dir(body, arm).y, -1.0, 1.0)))
		elevs.append(elev)
		if step == 4:
			elev_mid = elev
		var chest := body.get("_chest_pivot") as Node3D
		if chest != null:
			var turned := rad_to_deg(chest.quaternion.angle_to(Quaternion.IDENTITY))
			if step == 1:
				chest_early = turned
			if step == 20:
				chest_last = turned
	body.set("_hand_swing", 0.0)
	body.set("_hand_swing_strength", 1.0)
	body.call("_apply_hand_swing", 0.0)
	var top := -1000.0
	var apex_at := 0.0
	for i in elevs.size():
		if float(elevs[i]) > top:
			top = float(elevs[i])
			apex_at = float(i) / 20.0
	var near_max := 0
	for e in elevs:
		if float(e) > top - 3.0:
			near_max += 1
	return {"climbed": climbed, "chest_early": chest_early, "chest_last": chest_last,
		"elev_mid": elev_mid, "top": top, "apex_at": apex_at, "near_max": near_max}


## The reference's whole stroke, rewritten here from its own source, in its own units, to be held
## against what the body actually writes:
##
##   body.yRot        = sin(sqrt(attackTime) * 2PI) * 0.2
##   aa               = sin(outQuart(attackTime) * PI)
##   bb               = sin(attackTime * PI) * -(head.xRot - 0.7) * 0.75
##   attackArm.xRot  -= aa * 1.2 + bb
##   attackArm.yRot  += body.yRot * 2        (the chest already gives it the first)
##   attackArm.zRot  += sin(attackTime * PI) * -0.4
##   parts compose   Z, then Y, then X
##
## `attackTime` counts UP, 0 to 1. The probe restates it rather than reading the file's constants
## back, because the point of the check is that a change to either one is a difference.
func _reference_pivot(t: float, aim: float, towards: float, strength: float) -> Quaternion:
	var aa := sin((1.0 - pow(1.0 - t, 4.0)) * PI)
	var body_turn := sin(sqrt(t) * TAU) * 0.2
	var x_rot := clampf(LIFT * aa + sin(t * PI) * (0.7 - aim) * REACH, -0.3, RISE_MAX)
	var y_rot := 2.0 * body_turn
	var z_rot := sin(t * PI) * -0.4
	# The conversions, once: its xRot is SUBTRACTED from a rest of -pi/10, so the amount it takes
	# off goes in with this rig's facing; its yRot is the same way round as this rig's yaw; and its
	# zRot is "outboard", which is -X on the arm that attacks here, so it goes in negated by the
	# facing rather than multiplied by it.
	var start_out := START_OUT * sin(minf(t / START_SPAN, 1.0) * PI)
	return Quaternion(Vector3.RIGHT, x_rot * towards * strength) \
		* Quaternion(Vector3.UP, y_rot * -towards * LATERAL * strength) \
		* Quaternion(Vector3.BACK, (-z_rot * LATERAL + start_out) * towards * strength)


## Walk one stroke and report what the hand drew: the far end (the direction it reached and how
## far it turned to get there, measured from the rest direction), the widest it crossed
## sideways, where it finished, the item's ride, and the LOOP -- how far apart the hand's own
## path is on the way out and on the way back at the same point of its travel. A hinge brings
## those two together to nothing; a sweep keeps them apart.
##
## `rest_dir` is the direction the arm hung in BEFORE the swing was kicked, passed in rather
## than read off the first frame: the stroke is over in 0.3 s (six ticks), so a headless frame can already
## be well into it by the time this is called.
func _stroke(body: Node3D, arm: Node3D, pivot: Node3D, item: Node3D, rest_dir: Vector3,
		seconds: float) -> Dictionary:
	var dirs: Array = []
	var rest_item: Vector3 = body.to_local(item.global_position)
	var radius: float = (item.global_position - arm.global_position).length()
	var radius_error := 0.0
	var rise := 0.0
	# How far the HAND goes past the shoulder's own side line, in the body's frame: an arm that
	# opens away from the torso is the difference between a punch and a wing, and it is the one
	# thing about the stroke a direction-only measurement cannot see.
	var shoulder_x: float = body.to_local(arm.global_position).x
	var outboard := 0.0
	var inboard := 0.0
	var chest_turn := 0.0
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame
		dirs.append(_hand_dir(body, arm))
		radius_error = maxf(radius_error,
			absf((item.global_position - arm.global_position).length() - radius))
		rise = maxf(rise, body.to_local(item.global_position).y - rest_item.y)
		var hand: Vector3 = body.to_local(item.global_position)
		outboard = maxf(outboard, shoulder_x - hand.x)
		inboard = maxf(inboard, hand.x - shoulder_x)
		var chest := body.get("_chest_pivot") as Node3D
		if chest != null:
			chest_turn = maxf(chest_turn,
				rad_to_deg(chest.quaternion.angle_to(Quaternion.IDENTITY)))

	var peak_index := 0
	var peak_turn := 0.0
	for i in dirs.size():
		var turn := rad_to_deg((dirs[i] as Vector3).angle_to(rest_dir))
		if turn > peak_turn:
			peak_turn = turn
			peak_index = i
	var peak_dir: Vector3 = dirs[peak_index]
	var across := 0.0
	for i in dirs.size():
		across = maxf(across, absf((dirs[i] as Vector3).x))
	# The two frames at the same point of the travel, one on the way out and one on the way
	# back: the first and the last that reach this far up the stroke.
	var threshold := peak_turn * 0.6
	var first := -1
	var last := -1
	for i in dirs.size():
		if rad_to_deg((dirs[i] as Vector3).angle_to(rest_dir)) >= threshold:
			if first < 0:
				first = i
			last = i
	var loop := 0.0
	if first >= 0 and last > first:
		loop = rad_to_deg((dirs[first] as Vector3).angle_to(dirs[last]))
	return {
		"peak_dir": peak_dir,
		"peak_turn": peak_turn,
		"across": across,
		"loop": loop,
		"end": rad_to_deg(_hand_dir(body, arm).angle_to(rest_dir)),
		"identity": pivot.quaternion.is_equal_approx(Quaternion.IDENTITY),
		"rise": rise,
		"radius_error": radius_error,
		"outboard": outboard,
		"inboard": inboard,
		"chest": chest_turn,
		"chest_end": rad_to_deg((body.get("_chest_pivot") as Node3D).quaternion
			.angle_to(Quaternion.IDENTITY)) if body.get("_chest_pivot") != null else 0.0,
	}


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame


## The whole stroke, sampled by the CLOCK rather than by frames: the stroke is driven by
## `_hand_swing` and `_apply_hand_swing(0.0)` moves that clock nowhere, so each sample can be set
## exactly. Every row is the hand's own position in the body's frame (rig units: the arm is 12,
## the torso is 8 wide) and the direction the limb points.
func _trace_stroke(body: Node3D, arm: Node3D, item: Node3D, stone: int) -> void:
	var shoulder: Vector3 = body.to_local(arm.global_position)
	var rows: Array = []
	for step in 21:
		var p := float(step) / 20.0
		body.set("_hand_swing_strength", 1.0)
		body.set("_hand_swing", 1.0 - p)
		body.call("_apply_hand_swing", 0.0)
		var hand: Vector3 = body.to_local(item.global_position)
		rows.append({"p": p, "hand": hand - shoulder, "dir": _hand_dir(body, arm)})
	body.set("_hand_swing", 0.0)
	body.set("_hand_swing_strength", 1.0)
	body.call("_apply_hand_swing", 0.0)
	print("TRACE shoulder (body units) %s, item id %d" % [shoulder, stone])
	for row in rows:
		print("TRACE p %.2f  hand %s  dir %s" % [row["p"], row["hand"], row["dir"]])
	_plot(rows, 0, "BEHIND (x across, y up)")
	_plot(rows, 2, "SIDE (z forward, y up)")


## One coarse plot of the traced hand, in the body's frame, with 'S' at the shoulder and 'h' at
## the hand; the axis not plotted is dropped. 25 columns by 12 rows of a 30-unit window, which
## holds a 12-unit arm and the whole of its swing.
func _plot(rows: Array, axis: int, title: String) -> void:
	var w := 25
	var h := 12
	var lo := -15.0
	var span := 30.0
	var grid: Array = []
	for y in h:
		grid.append(" ".repeat(w))
	var cell := func(x: float, y: float) -> Vector2i:
		var cx := int(clampf((x - lo) / span * float(w - 1), 0.0, float(w - 1)))
		var cy := int(clampf((y - lo) / span * float(h - 1), 0.0, float(h - 1)))
		return Vector2i(cx, cy)
	for row in rows:
		var hand: Vector3 = row["hand"]
		var at: Vector2i = cell.call(hand[axis] if axis == 0 else hand[2], hand.y)
		grid[at.y] = (grid[at.y] as String).substr(0, at.x) + "h" \
			+ (grid[at.y] as String).substr(at.x + 1)
	print("TRACE %s  (x %.0f..%.0f, y %.0f..%.0f)" % [title, lo, lo + span, lo, lo + span])
	for y in range(h - 1, -1, -1):
		print("TRACE |" + (grid[y] as String) + "|")


func _run() -> void:
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

	# --- the wire, on the real player's own body ---------------------------------
	var player: Node = main.get_node_or_null("Player")
	var viewmodel: Node = main.get_node_or_null("Player/Camera3D/Viewmodel")
	var player_model := main.find_child("PlayerModel", true, false) as Node3D
	if player == null or viewmodel == null or player_model == null:
		print("PROBE FAIL: Main.tscn is missing the Player, the viewmodel or PlayerModel")
		quit(1)
		return
	player.call("set_hotbar_slot", 0, stone, 1)
	var player_swing := player_model.find_child("SwingPivot", true, false) as Node3D
	var player_arm := player_model.find_child("arm", true, false) as Node3D
	_ok("hand: the first-person hand owns the trigger surface",
		viewmodel.has_signal("swung") and viewmodel.has_method("punch")
			and viewmodel.has_method("place"),
		"punch %s, place %s, swung %s" % [viewmodel.has_method("punch"),
			viewmodel.has_method("place"), viewmodel.has_signal("swung")])
	_ok("body: the swing pivot sits between the shoulder and the arm",
		player_swing != null and player_arm != null and player_arm.get_parent() == player_swing
			and player_swing.get_parent() != null and player_swing.get_parent().name == "HeldArmPivot",
		"arm under '%s', that under '%s'"
			% [player_arm.get_parent().name if player_arm != null else "-",
				player_swing.get_parent().name if player_swing != null else "-"])

	# The arm's REST is the holding pose, and the holding pose is written when the body first sees
	# what its hand carries -- NOT in _ready. Measured before that lands, every number below is
	# read against a pose the body is not in yet (that cost a "102.8 deg of travel, ends 18.3 off
	# rest" once: the 18.3 was the held turn itself, and it inflated the peak by the same amount).
	var seated_until := Time.get_ticks_msec()
	while Time.get_ticks_msec() - seated_until < 1000:
		if player_model.get("_held_item") != null and (player_model.get("_held_item") as Node3D).visible:
			break
		await process_frame
	var player_rest: Vector3 = _hand_dir(player_model, player_arm)
	viewmodel.call("punch")
	_ok("wire: a punch reaches the body at full weight",
		absf(float(player_model.get("_hand_swing")) - 1.0) < 0.0001
			and absf(float(player_model.get("_hand_swing_strength")) - 1.0) < 0.0001,
		"clock %.3f, weight %.3f" % [player_model.get("_hand_swing"),
			player_model.get("_hand_swing_strength")])
	viewmodel.call("place")
	_ok("wire: a place is the weaker stroke",
		absf(float(player_model.get("_hand_swing_strength")) - PLACE_SHARE) < 0.0001,
		"weight %.3f" % float(player_model.get("_hand_swing_strength")))
	var wired_peak := 0.0
	var wired_until := Time.get_ticks_msec()
	while Time.get_ticks_msec() - wired_until < 450:
		await process_frame
		wired_peak = maxf(wired_peak, rad_to_deg(_hand_dir(player_model, player_arm)
			.angle_to(player_rest)))
	# ...at the place's weight, so the bar is 0.75 of a punch's own travel rather than the full
	# stroke's.
	_ok("wire: ...and the body's own arm draws it and comes back",
		wired_peak > 35.0
			and rad_to_deg(_hand_dir(player_model, player_arm).angle_to(player_rest)) < 2.0,
		"%.1f deg of hand travel, ends %.1f off rest" % [wired_peak,
			rad_to_deg(_hand_dir(player_model, player_arm).angle_to(player_rest))])
	viewmodel.call("punch")
	await _seconds(0.4)
	_ok("first person: the hand's own swing still plays and expires",
		absf(float(viewmodel.get("_swing"))) < 0.001
			and absf(float(viewmodel.get("_swing_place"))) < 0.001,
		"viewmodel timers %.3f / %.3f" % [viewmodel.get("_swing"),
			viewmodel.get("_swing_place")])

	# --- the stroke itself, on a body with an exact clock ------------------------
	var glb: PackedScene = load("res://assets/models/player.glb")
	if glb == null:
		print("PROBE FAIL: player.glb missing")
		quit(1)
		return
	var host := Stub.new()
	host.name = "SwingHost"
	root.add_child(host)
	var body: Node3D = glb.instantiate()
	body.set_script(load("res://scripts/player_model.gd"))
	# The clips are not this probe's subject and a breathing arm would move the item under the
	# measurement; the walk gets its own part at the end, with the tree running again.
	body.set("skip_head_look", true)
	host.add_child(body)
	await process_frame
	await process_frame
	host.block_id = stone
	await process_frame
	await process_frame

	var pivot := body.find_child("SwingPivot", true, false) as Node3D
	var arm := body.find_child("arm", true, false) as Node3D
	var shoulder := body.find_child("HeldArmPivot", true, false) as Node3D
	var item := body.get("_held_item") as MeshInstance3D
	if pivot == null or arm == null or shoulder == null or item == null or not item.visible:
		print("PROBE FAIL: the body has no swing pivot / arm / held item to measure")
		quit(1)
		return
	var clip_rest: Dictionary = body.get("_clip_rest")

	# At rest the arm hangs with the held pose's own 18 degrees forward and nothing else; the
	# pivot, which is the only writer of the stroke, is exactly identity.
	var rest_from_down := rad_to_deg(_hand_dir(body, arm).angle_to(Vector3.DOWN))
	_ok("rest: the arm hangs at the held pose, with the stroke at nothing",
		pivot.quaternion.is_equal_approx(Quaternion.IDENTITY)
			and absf(rest_from_down - HELD_PITCH) < 3.0,
		"identity %s, hand %.1f deg forward of down"
			% [pivot.quaternion.is_equal_approx(Quaternion.IDENTITY), rest_from_down])

	var rest_dir := _hand_dir(body, arm)
	body.call("_on_swung", 1.0)
	var punch: Dictionary = await _stroke(body, arm, pivot, item, rest_dir, 0.45)
	var peak_dir: Vector3 = punch["peak_dir"]
	# The far end is a POSE, not a hinge angle: forward, ACROSS, and ABOVE the shoulder -- the
	# arm ends up in front of the face rather than at the horizon, which is what one pitch
	# alone (and a small one at that) gives.
	_ok("punch: the hand ends up in front of and above the shoulder",
		peak_dir.z > 0.8 and peak_dir.y > 0.1 and absf(peak_dir.x) < 0.4
			and float(punch["peak_turn"]) > 80.0,
		"far end %s, %.1f deg of travel" % [peak_dir, punch["peak_turn"]])
	# ...and the holding pose is UNDER the stroke, not swapped for it: the reference adds its
	# attack angle to its own held-arm rule (the same 18 degrees), so a full hand reaches exactly
	# the holding pose higher than an empty one. Measured, because it is the difference between
	# the stroke being composed with the arm's pose and being written over it.
	host.block_id = 0
	await process_frame
	await process_frame
	var empty_rest := _hand_dir(body, arm)
	body.call("_on_swung", 1.0)
	var empty: Dictionary = await _stroke(body, arm, pivot, item, empty_rest, 0.45)
	_ok("punch: the holding pose is composed under the stroke, not replaced",
		absf(rad_to_deg((empty["peak_dir"] as Vector3).angle_to(peak_dir)) - HELD_PITCH)
			< TOLERANCE,
		"a full hand reaches %.1f deg higher than an empty one"
			% rad_to_deg((empty["peak_dir"] as Vector3).angle_to(peak_dir)))
	host.block_id = stone
	await process_frame
	await process_frame
	# The sideways component is the whole difference between an arc and a hinge: a pitch about
	# the body's left-right axis keeps the hand in the body's own plane, where x is exactly 0.
	# ...and it is a PUNCH's sideways component, not a windmill's: the reference's own lateral terms
	# put four fifths of the visible stroke into an outward sweep beside the head, which is what the
	# capture (probes/probe_third_person_swing_shot.gd) showed; the bar is an arc at all, at the
	# size a forward blow has.
	_ok("punch: it crosses the body -- an ARC, not a hinge up the side",
		float(punch["across"]) > 0.15,
		"the hand reaches x %.3f (a hinge stays at 0)" % float(punch["across"]))
	# ...on a DIFFERENT clock from the rise, so the way back is not the way out: the reference's arm
	# turns with the body's twist (a whole turn of a sine on the sqrt of the progress) while it is
	# still coming down, which opens the path into a wide loop rather than a retraced line.
	_ok("punch: and it comes back on a different line from the way out",
		float(punch["loop"]) > 10.0,
		"%.1f deg apart out and back at the same point of the travel" % float(punch["loop"]))
	# ...and it OPENS, out past the shoulder's own side line, then crosses back in: the reference's
	# roll is negative on the arm it attacks with, and the hand's own position against the
	# shoulder's is what an arm opening away from the torso looks like -- a direction cannot show
	# it, which is why this is measured here at all.
	_ok("punch: the arm crosses IN past the shoulder, not out past it",
		float(punch["inboard"]) > 1.0 and float(punch["inboard"]) > float(punch["outboard"]),
		"hand reaches %.2f in past the shoulder and %.2f out past it"
			% [float(punch["inboard"]), float(punch["outboard"])])
	# ...and the CHEST turns into it and comes back: the reference twists its body box by 0.2 rad
	# and places both arms from that twist, so a punch there is a shoulder going round the spine,
	# not an arm turning against a rigid torso.
	_ok("punch: the chest turns into the blow and comes back",
		float(punch["chest"]) > 5.0 and float(punch["chest"]) < 20.0
			and float(punch["chest_end"]) < 0.5,
		"chest reached %.1f deg, ends at %.2f" % [float(punch["chest"]),
			float(punch["chest_end"])])
	_ok("punch: and it ends exactly where it started",
		float(punch["end"]) < 0.01 and bool(punch["identity"]),
		"ends %.2f deg off rest, pivot at identity %s"
			% [float(punch["end"]), bool(punch["identity"])])

	# --- the FORMULA, not a property of it --------------------------------------
	# Everything above is a property a wrong stroke can also have: an arc, a loop, a height, a
	# chest that turns. The revisions that looked wrong passed all of them. This compares the
	# pivot's own rotation to the reference's expression, restated above, across the whole stroke
	# and at three aims -- so a mirrored clock, a rescaled angle or a dropped term is a failure and
	# not a matter of taste.
	var worst := 0.0
	var worst_at := 0.0
	var towards := float(body.call("_inspect_sign"))
	for aim in [0.0, 0.785398, -0.785398]:
		host.aim = Vector3(0.0, -sin(aim), cos(aim))
		for step in 21:
			var p := float(step) / 20.0
			body.set("_hand_swing_strength", 1.0)
			body.set("_hand_swing", 1.0 - p)
			body.call("_apply_hand_swing", 0.0)
			var off := rad_to_deg(pivot.quaternion.angle_to(
				_reference_pivot(p, aim, towards, 1.0)))
			if off > worst:
				worst = off
				worst_at = p
	host.aim = Vector3(0.0, 0.0, 1.0)
	body.set("_hand_swing", 0.0)
	body.set("_hand_swing_strength", 1.0)
	body.call("_apply_hand_swing", 0.0)
	_ok("formula: the stroke IS the reference's own expression",
		worst < 0.5, "worst %.3f deg off, at t %.2f" % [worst, worst_at])
	# ...over the reference's own DURATION. Its curve is stated in fractions of a stroke, so the
	# fractions were always right while the stroke was 0.225 s -- four and a half ticks -- and a
	# stroke that short spends 36 ms at its top. One duration of clock, spent in one call, has to
	# take the whole stroke; that is the number `getCurrentSwingDuration()` answers with six ticks.
	body.set("_hand_swing", 1.0)
	body.call("_apply_hand_swing", SWING_SECONDS)
	var spent := float(body.get("_hand_swing"))
	body.set("_hand_swing", 0.0)
	body.call("_apply_hand_swing", 0.0)
	_ok("clock: one stroke is six ticks of the reference's clock",
		absf(spent) < 0.001, "a whole duration leaves the clock at %.4f" % spent)

	rest_dir = _hand_dir(body, arm)
	body.call("_on_swung", PLACE_SHARE)
	var place: Dictionary = await _stroke(body, arm, pivot, item, rest_dir, 0.45)
	# The WEIGHT is exact and checked on the wire (0.750); this is the arc it draws, sampled by
	# frames, so the band says "clearly weaker, not a different stroke" rather than promising a
	# ratio a dropped frame can move.
	_ok("place: the same arc at the weaker endpoint",
		float(place["peak_turn"]) / maxf(float(punch["peak_turn"]), 0.001) > 0.55
			and float(place["peak_turn"]) / maxf(float(punch["peak_turn"]), 0.001) < 0.85,
		"%.1f deg against the punch's %.1f (x%.2f)"
			% [float(place["peak_turn"]), float(punch["peak_turn"]),
				float(place["peak_turn"]) / maxf(float(punch["peak_turn"]), 0.001)])

	# A second click during the return starts the stroke over -- from the top, at full weight,
	# and NOT past it: the arc is a function of one clock, so two clicks cannot add up.
	#
	# The wait POLLS rather than guessing a pause: the bell is flat for a good share of the
	# stroke around its top, so a fixed delay can land on the peak, and a stroke restarted from
	# there would neither test the return nor leave the arm at rest for what follows.
	rest_dir = _hand_dir(body, arm)
	var peak_deg := float(punch["peak_turn"])
	body.call("_on_swung", 1.0)
	var up_until := Time.get_ticks_msec()
	while Time.get_ticks_msec() - up_until < 200:
		await process_frame
		if rad_to_deg(_hand_dir(body, arm).angle_to(rest_dir)) > peak_deg * 0.9:
			break
	var mid_return := peak_deg
	var down_until := Time.get_ticks_msec()
	while Time.get_ticks_msec() - down_until < 300:
		await process_frame
		mid_return = rad_to_deg(_hand_dir(body, arm).angle_to(rest_dir))
		if mid_return < peak_deg * 0.7:
			break
	body.call("_on_swung", 1.0)
	# Read the restart's far end off the CLOCK, not off the frames: the stroke's peak is 45 ms long
	# and one dropped frame turns a frame-sampled measurement into a measurement of the frame rate
	# (this check was failing on exactly that, at a peak that was half the stroke's).
	var restarted_clock := float(body.get("_hand_swing"))
	body.set("_hand_swing", 1.0 - 0.2)
	body.call("_apply_hand_swing", 0.0)
	var restarted_far := rad_to_deg(_hand_dir(body, arm).angle_to(rest_dir))
	body.set("_hand_swing", 0.0)
	body.call("_apply_hand_swing", 0.0)
	_ok("restart: a second punch starts over and lands on the SAME far end",
		mid_return < float(punch["peak_turn"]) - 4.0
			and absf(restarted_clock - 1.0) < 0.01
			and absf(restarted_far - float(punch["peak_turn"])) <= TOLERANCE,
		"restarted at %.1f deg with the clock at %.2f, far end %.1f"
			% [mid_return, restarted_clock, restarted_far])

	# The held thing hangs off the arm, so the swing carries it with no item code: its distance
	# from the shoulder cannot change, and it rises with the hand. Measured on the first stroke,
	# which starts from rest.
	_ok("item: it rides the hand -- same distance from the shoulder all the way out",
		float(punch["radius_error"]) < 0.001 and float(punch["rise"]) > 0.05,
		"radius error %.5f, risen %.3f rig units"
			% [float(punch["radius_error"]), float(punch["rise"])])

	# --- what the stroke READS besides the clock -------------------------------
	# The reference's rise is `aa * 1.2 + bb`, and `bb` is the HEAD'S PITCH: a level look gives the
	# blow 0.525 rad of lift, a look 40 degrees down gives none at all and a look 40 up gives twice
	# as much. Ours was written with the level-look value baked in as a constant, so a punch at the
	# floor rose like a punch at the horizon -- which is the difference between an animation and a
	# blow that is aimed.
	var level_aim: Dictionary = _sweep(body, arm, item)
	host.aim = Vector3(0.0, -1.0, 1.0).normalized()
	var aim_down: Dictionary = _sweep(body, arm, item)
	host.aim = Vector3(0.0, 1.0, 1.0).normalized()
	var aim_up: Dictionary = _sweep(body, arm, item)
	host.aim = Vector3(0.0, 0.0, 1.0)
	_ok("aim: the blow points where the crosshair does",
		float(aim_down["elev_mid"]) < float(level_aim["elev_mid"]) - 8.0
			and float(aim_up["elev_mid"]) > float(level_aim["elev_mid"]) + 8.0,
		"arm %.1f deg above horizontal at 45 down, %.1f level, %.1f at 45 up"
			% [float(aim_down["elev_mid"]), float(level_aim["elev_mid"]),
				float(aim_up["elev_mid"])])
	# ...and the clock runs the way the reference's does. Its `attackTime` counts UP from 0 and the
	# twist reads `sin(sqrt(attackTime) * 2pi)`, which is a quarter turn in at 1/16 of the stroke:
	# the body is most turned round almost at once and home by the end. Written the other way round
	# (`sqrt(1 - t)`, as this file had it) those two numbers swap ends.
	# ...and the rise is ONE apex rather than two points and a hold: the reference's rise peaks a
	# sixth of the way in and is already well down by halfway, and its aim term is deliberately too
	# small to pull that maximum late. Raised, that term peaks at half a stroke and the sum sits at
	# its top from a fifth to a half -- which is what "there is an apex pause" looks like on screen.
	_ok("shape: one apex, and no holding at it",
		float(level_aim["apex_at"]) < 0.4 and int(level_aim["near_max"]) <= 4,
		"apex at t %.2f (%.1f deg), %d of 21 samples within 3 deg of it"
			% [float(level_aim["apex_at"]), float(level_aim["top"]),
				int(level_aim["near_max"])])
	_ok("clock: the twist is in at a sixteenth and home by the end",
		float(level_aim["chest_early"]) > 6.0 and float(level_aim["chest_last"]) < 0.5,
		"chest %.1f deg at 1/16 of the stroke, %.1f at the end"
			% [float(level_aim["chest_early"]), float(level_aim["chest_last"])])

	# --- walking: the arm swings over a body that keeps walking ------------------
	body.set("skip_head_look", false)
	host.velocity = Vector3(0.25, 0.0, 0.0)
	await _seconds(1.0)
	var leg := body.find_child("leg", true, false) as Node3D
	var leg_peak := 0.0
	var arm_lo := 999.0
	var arm_hi := -999.0
	var shoulder_lo := 999.0
	var shoulder_hi := -999.0
	var swing_peak := 0.0
	body.call("_on_swung", 1.0)
	var walked_at := Time.get_ticks_msec()
	while Time.get_ticks_msec() - walked_at < 700:
		await process_frame
		leg_peak = maxf(leg_peak, rad_to_deg(leg.quaternion.angle_to(clip_rest["leg"])))
		var arm_off := rad_to_deg(arm.quaternion.angle_to(clip_rest["arm"]))
		arm_lo = minf(arm_lo, arm_off)
		arm_hi = maxf(arm_hi, arm_off)
		shoulder_lo = minf(shoulder_lo, rad_to_deg(shoulder.rotation.x))
		shoulder_hi = maxf(shoulder_hi, rad_to_deg(shoulder.rotation.x))
		swing_peak = maxf(swing_peak, rad_to_deg(_hand_dir(body, arm).angle_to(rest_dir)))
	_ok("walk: the legs keep reaching their own stride under the swing",
		leg_peak > 45.0 and swing_peak > 45.0,
		"leg %.1f deg, hand travel %.1f" % [leg_peak, swing_peak])
	_ok("walk: the clips keep turning the arm underneath the stroke",
		arm_hi - arm_lo > 1.0,
		"arm %.2f to %.2f deg off rest" % [arm_lo, arm_hi])
	_ok("walk: the 18-degree held turn is not disturbed by either",
		absf(shoulder_hi - shoulder_lo) < 0.001 and absf(shoulder_lo + HELD_PITCH) < 0.01,
		"shoulder %.3f to %.3f deg" % [shoulder_lo, shoulder_hi])

	# The stroke as a PATH, on demand: a list of samples and two coarse plots of it (from behind,
	# and from the side), because a hand's direction at one instant says nothing about the shape
	# of the thing, and the shape is what a player judges. Off unless asked for, so a normal run
	# stays a pass/fail list.
	if OS.get_environment("FARLANDS_SWING_TRACE") == "1":
		_trace_stroke(body, arm, item, stone)

	host.queue_free()
	main.queue_free()
	print("PROBE third-person swing: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
