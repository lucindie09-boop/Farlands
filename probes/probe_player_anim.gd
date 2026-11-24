extends SceneTree
## Does the player's body move the way its clips say it does?
##
## The body plays two clips and no others: `Idle.anim` standing, `Run.anim` walking
## (scripts/player_model.gd). A clip is only motion if its tracks name limbs this
## rig actually has, so what is measured here is that the two clips exist, that
## every one of their tracks resolves, that a standing body is playing the idle and
## that its arms are moving, and that a pose clone plays nothing at all.
##
## The park is still measured too: scripts/body_animator.gd -- the hand-port of
## FA+Player's moves the clips replaced -- is a pure function of its inputs, so the
## checks below drive it directly (idle, walk, sprint, jump, fall, sneak, swim). It
## is on disk, it is documented, and nothing in the game calls it.
##
## Nothing here writes to the world and no shot is read, so it runs headless:
##
##   probes/run_probe.sh probes/probe_player_anim.gd

const BodyAnimator := preload("res://scripts/body_animator.gd")
const ClipRetarget := preload("res://scripts/clip_retarget.gd")
const DT := 1.0 / 60.0
const WALK := 4.317     # vanilla walking speed, blocks/s
const SPRINT := 5.6


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


var _failures := 0


func _run() -> void:
	# --- the maths, state by state -------------------------------------------
	var idle := _measure(_fresh(), 0.5, 4.0, 0.0, 0.0, true, false, false, false)
	_ok("idle: the legs are still", _range(idle, "leg") < 0.001 and _range(idle, "leg2") < 0.001,
		"leg range %.3f, leg2 %.3f" % [_range(idle, "leg"), _range(idle, "leg2")])
	_ok("idle: the arms keep the old clip's sway",
		_range(idle, "arm") > 3.0 and _range(idle, "arm") < 3.8,
		"arm %.2f deg peak to peak" % _range(idle, "arm"))
	_ok("idle: the torso breathes without leaning", _range(idle, "torso") > 0.3
		and _range(idle, "torso") < 1.3, "torso %.2f deg" % _range(idle, "torso"))

	var walk := _measure(_fresh(), 1.0, 2.0, WALK, 0.0, true, false, false, false)
	var walk_leg := _range(walk, "leg")
	_ok("walk: the legs stride", walk_leg > 50.0, "%.1f deg peak to peak" % walk_leg)
	var opposed := 0
	var arms_opposed := 0
	for s in walk:
		if absf(float(s["leg"])) > 5.0:
			if signf(float(s["leg"])) == signf(float(s["leg2"])):
				opposed += 1
			if signf(float(s["arm"])) == signf(float(s["leg"])):
				arms_opposed += 1
	_ok("walk: the two legs are opposed", opposed == 0, "%d sample(s) moving as one" % opposed)
	_ok("walk: each arm swings against its own leg", arms_opposed == 0,
		"%d sample(s) with the arm and its leg together" % arms_opposed)

	var sprint := _measure(_fresh(), 1.0, 2.0, SPRINT, 0.0, true, false, false, true)
	_ok("sprint: a longer stride than the walk", _range(sprint, "leg") > walk_leg * 1.2,
		"%.1f deg against the walk's %.1f" % [_range(sprint, "leg"), walk_leg])
	_ok("sprint: the chest leans into it", _peak(sprint, "torso") > 6.0,
		"%.1f deg of lean" % _peak(sprint, "torso"))

	var rise := _measure(_fresh(), 0.8, 0.4, 3.0, 6.0, false, false, false, false)
	_ok("jump: the arms come up", _peak(rise, "arm") > 25.0,
		"arm %.1f deg" % _peak(rise, "arm"))
	_ok("jump: the legs split", _peak(rise, "leg") > 8.0 and _peak(rise, "leg2") > 6.0,
		"leg %.1f, leg2 %.1f" % [_peak(rise, "leg"), _peak(rise, "leg2")])
	_ok("jump: the arms are spread outward", absf(float(rise[0]["arm_roll"])) > 5.0,
		"arm_roll %.1f deg" % float(rise[0]["arm_roll"]))
	var fall := _measure(_fresh(), 0.8, 0.4, 3.0, -6.0, false, false, false, false)
	_ok("fall: a different shape from the rise",
		absf(float(fall[0]["arm"])) > absf(float(rise[0]["arm"])) + 5.0,
		"arming %.1f falling against %.1f rising" % [float(fall[0]["arm"]), float(rise[0]["arm"])])

	var slow := _measure(_fresh(), 1.0, 2.0, 1.3, 0.0, true, false, false, false)
	var sneak := _measure(_fresh(), 1.0, 2.0, 1.3, 0.0, true, false, true, false)
	_ok("sneak: the chest tips forward", _peak(sneak, "torso") > 20.0,
		"%.1f deg" % _peak(sneak, "torso"))
	_ok("sneak: the stride all but disappears", _range(sneak, "leg") < _range(slow, "leg") * 0.7,
		"%.1f deg against a %.1f deg slow walk" % [_range(sneak, "leg"), _range(slow, "leg")])

	var swim := _measure(_fresh(), 0.8, 1.0, 1.2, 0.0, false, true, false, false)
	_ok("swim: the chest pitches over the water", _peak(swim, "torso") > 12.0,
		"%.1f deg" % _peak(swim, "torso"))
	_ok("swim: the arms paddle", _range(swim, "arm") > 40.0,
		"%.1f deg of stroke" % _range(swim, "arm"))
	_ok("swim: the air pose does not fight it",
		_peak(swim, "leg") < _peak(rise, "leg"),
		"leg %.1f against the jump's %.1f" % [_peak(swim, "leg"), _peak(rise, "leg")])

	# A stop is a pose that decays, not one that cuts: at 1.0 s of exact walking
	# the cycle sits near a leg's peak, so the frame after the stop is still most
	# of the way there.
	var stopping := BodyAnimator.new()
	_measure(stopping, 0.5, 0.5, WALK, 0.0, true, false, false, false)
	var first := stopping.step(DT, 0.0, 0.0, true, false, false, false)
	for i in range(int(round(0.8 / DT))):
		stopping.step(DT, 0.0, 0.0, true, false, false, false)
	var final := stopping.step(DT, 0.0, 0.0, true, false, false, false)
	_ok("stop: the stride decays instead of cutting",
		absf(float(first["leg"])) > 8.0 and absf(float(final["leg"])) < 1.0,
		"leg %.1f on the frame after the stop, %.2f after 0.8 s"
			% [float(first["leg"]), float(final["leg"])])

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

	# Two clips and no others, and every track of both lands on a limb this body has:
	# a track that names nothing plays as no motion at all, silently.
	var clips := model.find_child("Clips", false, false) as AnimationPlayer
	var names := clips.get_animation_list() if clips != null else PackedStringArray()
	_ok("body: the body has its two clips, idle and run",
		clips != null and names == PackedStringArray(["idle", "run"]), "clips %s" % str(names))
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

	# The K-key pose clone is frozen on purpose: skip_head_look must gate the pose
	# too, or the "dummy" breathes at the player while it is being punched.
	var glb: PackedScene = load("res://Assets/models/player.glb")
	var clone: Node3D = glb.instantiate()
	var clone_rest: Quaternion = (clone.get_node("arm") as Node3D).quaternion
	clone.set_script(load("res://scripts/player_model.gd"))
	clone.set("skip_head_look", true)
	root.add_child(clone)
	for i in range(30):
		await process_frame
	var clone_arm: Node3D = clone.get_node("arm")
	_ok("clone: a pose clone is frozen, body included",
		clone_arm.quaternion.is_equal_approx(clone_rest),
		"arm %.3f deg off rest" % rad_to_deg(clone_arm.quaternion.angle_to(clone_rest)))
	clone.queue_free()

	print("PROBE player animation: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


func _fresh() -> BodyAnimator:
	return BodyAnimator.new()


## Settle for `settle` seconds, then collect `collect` seconds of poses.
func _measure(animator: BodyAnimator, settle: float, collect: float, speed: float,
		vertical: float, on_floor: bool, in_water: bool, sneaking: bool,
		sprinting: bool) -> Array:
	var out := []
	for i in range(int(round(settle / DT))):
		animator.step(DT, speed, vertical, on_floor, in_water, sneaking, sprinting)
	for i in range(int(round(collect / DT))):
		out.append(animator.step(DT, speed, vertical, on_floor, in_water, sneaking, sprinting))
	return out


func _range(samples: Array, channel: String) -> float:
	var lo := 1e9
	var hi := -1e9
	for s in samples:
		var v := float((s as Dictionary).get(channel, 0.0))
		lo = minf(lo, v)
		hi = maxf(hi, v)
	return hi - lo


func _peak(samples: Array, channel: String) -> float:
	var peak := 0.0
	for s in samples:
		peak = maxf(peak, absf(float((s as Dictionary).get(channel, 0.0))))
	return peak


func _span(values: Array) -> float:
	var lo := 1e9
	var hi := -1e9
	for v in values:
		lo = minf(lo, float(v))
		hi = maxf(hi, float(v))
	return hi - lo
