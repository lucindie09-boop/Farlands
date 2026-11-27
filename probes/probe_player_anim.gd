extends SceneTree
## Does the player's body move the way its clips say it does?
##
## The body plays two poses and a rest: `Idle.anim` standing, `Run.anim` walking, both
## blended onto a still pose with the idle ADDED over the result (scripts/player_model.gd).
## A clip is only motion if its tracks name limbs this rig actually has, so what is
## measured here is that all three exist and every track resolves, that a standing body
## is idle and its arms are moving, that the walk is Minecraft's (80 degrees of leg at
## 0.4716 s a cycle) with its amplitude carried by the tree's blend weight and its fade
## being that amplitude shrinking -- so a stop settles the legs and never rewinds the
## idle -- and that a pose clone plays nothing at all.
##
## Nothing here writes to the world and no shot is read, so it runs headless:
##
##   probes/run_probe.sh probes/probe_player_anim.gd

## A body's controller, for the checks that need one: _animate_body reads the speed the
## body is moving at, and a body with no controller in its ancestry stands at 0.
class Stub extends Node3D:
	var velocity := Vector3.ZERO

	func get_velocity() -> Vector3:
		return velocity

	func get_aim_direction() -> Vector3:
		return Vector3(0.0, 0.0, -1.0)
const ClipRetarget := preload("res://scripts/clip_retarget.gd")


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


var _failures := 0


func _run() -> void:
	# --- the body -------------------------------------------------------------
	var scene: PackedScene = load("res://Main.tscn")
	if scene == null:
		print("PROBE FAIL: Main.tscn missing")
		quit(1)
		return
	var main: Node = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame

	var player: Node = main.get_node_or_null("Player")
	var model: Node3D = main.find_child("PlayerModel", true, false) as Node3D
	if player == null or model == null:
		print("PROBE FAIL: no Player/PlayerModel in Main.tscn")
		quit(1)
		return
	_ok("body: the controller exposes the state the clips are chosen by",
		player.has_method("get_velocity") and player.has_method("is_on_floor"),
		"get_velocity %s" % player.has_method("get_velocity"))

	# Two POSES and the rest pose they are worn over, and nothing else -- and every track
	# of all three lands on a limb this body has: a track that names nothing plays as no
	# motion at all, silently.
	var clips := model.find_child("Clips", false, false) as AnimationPlayer
	var names := clips.get_animation_list() if clips != null else PackedStringArray()
	_ok("body: the body has its two poses and the rest they blend from",
		clips != null and names == PackedStringArray(["idle", "rest", "run"]),
		"clips %s" % str(names))
	var unresolved := PackedStringArray()
	if clips != null:
		for clip_name in names:
			unresolved.append_array(ClipRetarget.unresolved(clips.get_animation(clip_name), model))
	_ok("body: every track of both clips names a limb this body has",
		clips != null and unresolved.is_empty(), "missing %s" % str(unresolved))

	# The live arms sway on their own: the whole _process -> clip -> rig path, and the
	# clip it picked is the idle. The body is let down first, because a body in the air
	# is not standing and the idle is what this is about.
	var arm: Node3D = model.find_child("arm", true, false) as Node3D
	# The clock here is real time, not frames: headless frames are far shorter than
	# a rendered one, so a frame count is not a duration and would measure a
	# fraction of the sway.
	var started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 4000 and not player.is_on_floor():
		await process_frame
	var off_rest := []
	var grounded := 0
	var frames := 0
	started = Time.get_ticks_msec()
	while Time.get_ticks_msec() - started < 2500:
		await process_frame
		frames += 1
		if player.is_on_floor():
			grounded += 1
		off_rest.append(rad_to_deg(arm.quaternion.angle_to(Quaternion.IDENTITY)))
	var sway := _span(off_rest)
	_ok("body: standing still, it plays the idle and the arms sway",
		model.get("_clip") == "idle" and sway > 0.2 and sway < 6.0 and grounded == frames,
		"clip '%s', %.2f deg of sway over 2.5 s, %d/%d frames on the floor"
			% [model.get("_clip"), sway, grounded, frames])

	# Minecraft derives a walk from one number -- the walk amount: full swing at 5
	# blocks/s, 80 degrees of leg against 57 of arm, one cycle every 0.4716 s, and the
	# swing shortens with the pace. The clip's own base is none of that (16.8 degrees,
	# one cycle a second), so the body re-bases it at load -- measured here off the clip
	# it built, in radians away from each part's rest pose.
	var stride_body: Node3D = (load("res://Assets/models/player.glb") as PackedScene).instantiate()
	stride_body.set_script(load("res://scripts/player_model.gd"))
	root.add_child(stride_body)
	await process_frame
	var stride_clips := stride_body.find_child("Clips", false, false) as AnimationPlayer
	var stride_run: Animation = stride_clips.get_animation("run")
	var leg_rest: Quaternion = stride_body.get("_clip_rest")["leg"]
	var arm_rest: Quaternion = stride_body.get("_clip_rest")["arm"]
	var full := _swing(stride_run, leg_rest, "leg")
	var full_arm := _swing(stride_run, arm_rest, "arm")
	_ok("stride: the clip's base is vanilla's -- 80 degrees of leg, 57 of arm",
		absf(full - 1.4) < 0.02 and absf(full_arm - 1.0) < 0.02,
		"leg %.3f rad, arm %.3f rad" % [full, full_arm])
	var rate := float(stride_body.call("_full_swing_rate"))
	_ok("stride: a full swing plays one cycle every 0.47 s",
		absf(rate - 1.0 / 0.4716) < 0.01, "%.3f clip lengths/s" % rate)
	# ...and its keys stay at that full swing whatever the body is doing: how far it
	# actually swings is the tree's blend weight (measured below), so there is ONE
	# amplitude writer. Two of them would multiply, and a pace would swing a quarter as
	# far as it should.
	_ok("stride: the clip is untouched by the pace -- the blend is the amplitude",
		is_equal_approx(stride_run.length, 1.0)
			and stride_run.track_get_key_count(_track(stride_run, "leg")) == 29,
		"%.4f rad of leg, %.3f s, %d keys" % [full, stride_run.length,
			stride_run.track_get_key_count(_track(stride_run, "leg"))])
	# The loop point is where a hand-authored clip shows its seams: the source eases
	# INTO and OUT OF its own first and last keys, so it crossed zero at 90 deg/s at
	# the wrap and 415 half a cycle later -- a crawl at every end/start. The clip is
	# fitted to one cosine (vanilla's own shape), so the swing crosses both at once.
	# The two crossings of a cycle travel OPPOSITE ways (one leg forward, the other
	# back), so it is their speeds that have to agree.
	var seam_speed := absf(_slope(stride_run, leg_rest, "leg", 0.0))
	var mid_speed := absf(_slope(stride_run, leg_rest, "leg", stride_run.length * 0.5))
	_ok("stride: the swing crosses the loop point as fast as the middle",
		mid_speed > 1.0 and absf(seam_speed - mid_speed) < mid_speed * 0.12,
		"%.0f deg/s at the seam against %.0f mid-cycle" % [seam_speed, mid_speed])
	# The arms' hang is baked into NOTHING. Minecraft adds it to the walk as a motion of
	# its own, and so does this body: the tree's `add` node lays the idle over the run,
	# which is where both the hang and its 4.2 s drift come from. Both clips therefore
	# have to be played on the model's own rest.
	var tree: AnimationTree = stride_body.get("_tree")
	var stride_tree: AnimationTree = tree
	var layers: Array = tree.tree_root.get_node_list() if tree != null else []
	var tree_ok := tree != null and tree.anim_player != NodePath()
	for layer in ["rest", "run", "timescale", "blend", "idle", "add"]:
		tree_ok = tree_ok and layers.has(layer)
	_ok("tree: the walk blends onto a rest pose and the idle is added over it",
		tree_ok, "nodes %s, host '%s'" % [str(layers), tree.anim_player if tree != null else "-"])
	var idle_turns := _turns(stride_clips.get_animation("idle"),
		stride_body.get("_clip_rest")["arm"], "arm")
	_ok("tree: the hang and its drift belong to the idle, 0 to 3.4 deg out",
		idle_turns[0] < 1.5 and idle_turns[1] > 2.0 and idle_turns[1] < 4.5,
		"%.1f to %.1f deg from the model's rest" % [idle_turns[0], idle_turns[1]])
	_ok("tree: no clip carries a copy of it -- both rest on the model's own pose",
		_run_seam_is_base(stride_run, stride_body.get("_clip_rest")["arm"])
			and stride_body.get("_clip_rest")["leg"].is_equal_approx(Quaternion.IDENTITY)
			and stride_body.get("_clip_rest")["torso"].is_equal_approx(Quaternion.IDENTITY),
		"arm at 0 s is the rig's own rest")
	# ...and a walking body's arms carry the idle's roll AND its drift, which is the
	# difference between a layered hang and a copied constant: a constant reads a flat
	# line. The body is kept from steering its own tree and the tree is left at a walk's
	# amounts, so what is measured is the tree's own composite -- a swing about X leaves
	# the hanging arm's x alone, so that is the roll.
	stride_body.set_process(false)
	stride_tree.active = true
	stride_tree.set("parameters/add/add_amount", 1.0)
	stride_tree.set("parameters/blend/blend_amount", 1.0)
	# A body with no controller in its ancestry stands at 0, so the tree's time scale is
	# 0 too (vanilla's phase advances by the amount). This probe drives the tree itself.
	stride_tree.set("parameters/timescale/scale", 1.0)
	var stride_arm := stride_body.find_child("arm", true, false) as Node3D
	var stride_leg := stride_body.find_child("leg", true, false) as Node3D
	var roll_lo := 999.0
	var roll_hi := -999.0
	var leg_peak := 0.0
	var prev_roll := -999.0
	var walk_started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - walk_started < 4600:
		await process_frame
		var roll := _arm_roll_deg(stride_arm)
		roll_lo = minf(roll_lo, roll)
		roll_hi = maxf(roll_hi, roll)
		prev_roll = roll
		leg_peak = maxf(leg_peak, _off_rest_deg(stride_leg))
	_ok("tree: at a walk's amounts the arms carry the idle's DRIFT, not a constant",
		roll_hi - roll_lo > 2.0 and leg_peak > 45.0,
		"roll %.2f to %.2f deg (span %.2f) while the leg reaches %.0f"
			% [roll_lo, roll_hi, roll_hi - roll_lo, leg_peak])
	# The blend's weight IS the swing: it is the walk amount, so half of it is half the
	# step -- the fade is the amplitude, not a crossfade between two poses.
	stride_tree.set("parameters/blend/blend_amount", 0.5)
	var half_peak := 0.0
	var half_started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - half_started < 1400:
		await process_frame
		half_peak = maxf(half_peak, _off_rest_deg(stride_leg))
	_ok("tree: the blend's weight IS the swing -- half the weight, half the leg",
		absf(half_peak / leg_peak - 0.5) < 0.12,
		"%.1f deg against %.1f (x%.2f)" % [half_peak, leg_peak, half_peak / leg_peak])
	# The stop, through the production path: the body is given a speed to read and then
	# loses it, so what is measured is the real easing. The legs must settle onto rest (a
	# fade of the amplitude, not a crossfade) and the arms' roll must carry straight
	# through, because the idle is never re-entered -- a rewind would be one big step.
	# Stepping the blend to 0 in a single frame instead would read the roll through a
	# swing that vanished between two frames, and cos(swing) shrinks it: an artifact of
	# the probe, not of the body.
	var host := Stub.new()
	root.add_child(host)
	root.remove_child(stride_body)
	host.add_child(stride_body)
	stride_tree.set("parameters/blend/blend_amount", 0.0)
	stride_body.set_process(true)
	host.velocity = Vector3(0.25, 0.0, 0.0)
	var walked := Time.get_ticks_msec()
	while Time.get_ticks_msec() - walked < 700:
		await process_frame
	prev_roll = _arm_roll_deg(stride_arm)
	host.velocity = Vector3.ZERO
	var jump := 0.0
	var still_leg := 0.0
	var still_lo := 999.0
	var still_hi := -999.0
	var stop_started := Time.get_ticks_msec()
	while Time.get_ticks_msec() - stop_started < 1400:
		await process_frame
		var roll := _arm_roll_deg(stride_arm)
		jump = maxf(jump, absf(roll - prev_roll))
		prev_roll = roll
		still_lo = minf(still_lo, roll)
		still_hi = maxf(still_hi, roll)
		# The settle is measured once it has happened: vanilla's 0.4-a-tick easing takes
		# about 0.4 s to reach rest, so the first half-second IS that fade.
		if Time.get_ticks_msec() - stop_started > 700:
			still_leg = maxf(still_leg, _off_rest_deg(stride_leg))
	_ok("tree: the stop settles the legs and does NOT rewind the idle",
		still_leg < 1.0 and jump < 0.6 and still_hi - still_lo > 0.1,
		"leg %.2f deg, roll %.2f to %.2f (biggest step %.2f deg), so it kept breathing"
			% [still_leg, still_lo, still_hi, jump])
	stride_tree.active = false
	_ok("stride: the loop closes on its own value",
		absf(_angle_at(stride_run, leg_rest, "leg", 0.0)
			- _angle_at(stride_run, leg_rest, "leg", stride_run.length)) < 0.01,
		"%.4f rad at 0 s against %.4f at %.3f s"
			% [_angle_at(stride_run, leg_rest, "leg", 0.0),
				_angle_at(stride_run, leg_rest, "leg", stride_run.length), stride_run.length])
	host.queue_free()

	# The K-key pose clone is frozen on purpose: skip_head_look must gate the pose
	# too, or the "dummy" breathes at the player while it is being punched.
	var glb: PackedScene = load("res://Assets/models/player.glb")
	var clone: Node3D = glb.instantiate()
	var clone_rest: Quaternion = (clone.find_child("arm", true, false) as Node3D).quaternion
	clone.set_script(load("res://scripts/player_model.gd"))
	clone.set("skip_head_look", true)
	root.add_child(clone)
	for i in range(30):
		await process_frame
	var clone_arm: Node3D = clone.find_child("arm", true, false)
	_ok("clone: a pose clone is frozen, body included",
		clone_arm.quaternion.is_equal_approx(clone_rest),
		"arm %.3f deg off rest" % rad_to_deg(clone_arm.quaternion.angle_to(clone_rest)))
	clone.queue_free()

	print("PROBE player animation: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


## A clip's rotation track for `part` (find_track needs the track type as well).
##
## A track names its limb by the path it has from the body root, and the right arm's is
## `HeldArmPivot/arm` since the shoulder pivot went in (player_model.gd::_init_held_arm),
## so the part is looked up by the node the track lands on -- the last segment, here.
func _track(clip: Animation, part: String) -> int:
	var exact := clip.find_track(NodePath(part), Animation.TYPE_ROTATION_3D)
	if exact >= 0:
		return exact
	for t in range(clip.get_track_count()):
		if clip.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		if str(clip.track_get_path(t)).get_file() == part:
			return t
	return -1


## The biggest angle a part's track turns away from its rest pose, in radians.
func _swing(clip: Animation, rest: Quaternion, part: String) -> float:
	var t := _track(clip, part)
	if t < 0:
		return 0.0
	var peak := 0.0
	for k in range(clip.track_get_key_count(t)):
		var q: Quaternion = clip.track_get_key_value(t, k)
		peak = maxf(peak, (rest.inverse() * q).get_angle())
	return peak


## The smallest and largest angle a part's track turns away from `rest`, in degrees.
func _turns(clip: Animation, rest: Quaternion, part: String) -> Array:
	var t := _track(clip, part)
	if t < 0:
		return [0.0, 0.0]
	var lo := 999.0
	var hi := -999.0
	for k in range(clip.track_get_key_count(t)):
		var q: Quaternion = clip.track_get_key_value(t, k)
		var angle := rad_to_deg((rest.inverse() * q).get_angle())
		lo = minf(lo, angle)
		hi = maxf(hi, angle)
	return [lo, hi]


## A limb's rotation away from the rig's rest, in degrees: what is actually drawn.
func _off_rest_deg(node: Node3D) -> float:
	return rad_to_deg(node.quaternion.angle_to(Quaternion.IDENTITY))


## An arm's ROLL in degrees, out from the body. A swing about X turns the hanging arm in
## the YZ plane and leaves its x alone, so the hanging direction's x IS the roll -- which
## is how the idle's contribution is measured on its own, with the walk running.
func _arm_roll_deg(node: Node3D) -> float:
	var hanging: Vector3 = node.quaternion * Vector3.DOWN
	return rad_to_deg(asin(clampf(hanging.x, -1.0, 1.0)))


## Is the clip's arm key at t=0 exactly `base`? The stride and the fit scale about
## that pose, so a clip that starts anywhere else would be holding the arms by the
## model's rest instead of vanilla's.
func _run_seam_is_base(clip: Animation, base: Quaternion) -> bool:
	var t := _track(clip, "arm")
	if t < 0:
		return false
	var key: Quaternion = clip.track_get_key_value(t, 0)
	return key.angle_to(base) < 0.01


## A part's signed angle about the track's own axis at `at`, in degrees. The time
## WRAPS around the cycle, the way the mixer does, or a measurement taken either
## side of the loop point would read a clamped pose as a slow one.
func _angle_at(clip: Animation, rest: Quaternion, part: String, at: float) -> float:
	at = fposmod(at, clip.length) if clip.length > 0.0 else 0.0
	var t := _track(clip, part)
	if t < 0:
		return 0.0
	var count := clip.track_get_key_count(t)
	var prev: Quaternion = clip.track_get_key_value(t, 0)
	var prev_time := clip.track_get_key_time(t, 0)
	var q := prev
	var last := count - 1
	for k in range(1, count):
		var time := clip.track_get_key_time(t, k)
		if at > time:
			prev = clip.track_get_key_value(t, k)
			prev_time = time
			continue
		var span := time - prev_time
		var f := 0.0 if span <= 0.0 else (at - prev_time) / span
		q = prev.slerp(clip.track_get_key_value(t, k), clampf(f, 0.0, 1.0))
		break
	if at > clip.track_get_key_time(t, last):
		q = clip.track_get_key_value(t, last)
	var rel: Quaternion = rest.inverse() * q
	return rad_to_deg(2.0 * atan2(rel.x, rel.w) if rel.w >= 0.0 else 2.0 * atan2(-rel.x, -rel.w))


## How fast the swing is moving at `at`, in degrees per second.
func _slope(clip: Animation, rest: Quaternion, part: String, at: float) -> float:
	var step := 1.0 / 480.0
	var before := _angle_at(clip, rest, part, at - step)
	var after := _angle_at(clip, rest, part, at + step)
	return (after - before) / (2.0 * step)


func _span(values: Array) -> float:
	var lo := 1e9
	var hi := -1e9
	for v in values:
		lo = minf(lo, float(v))
		hi = maxf(hi, float(v))
	return hi - lo
