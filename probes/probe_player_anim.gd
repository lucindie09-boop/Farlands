extends SceneTree
## Does the player's own body play the OLD clip, and nothing else?
##
## The body loads exactly one clip: Animations/Idle.anim, an arm-swing idle made
## for an older rig, whose tracks name `arm_001`/`leg_001` where this body has
## `arm2`/`leg2`. Three things can go wrong there and none of them makes a sound:
## a track can be left on a node this body does not have, which plays as no
## motion at all; one of the pack's two clips (Animations/Idle_new.anim,
## Animations/Walk.anim) can creep back in as the walk this body is meant not to
## have; and a state the controller asks for can restart the clip it is already
## in, which freezes it on its first pose.
##
## So this asks the BODY, not the files:
##
##   * the player's AnimationPlayer holds the idle and NOT a walk, and is playing
##     it;
##   * every track of that clip names a node this body actually has, and none is
##     left under the source rig's `arm_001`/`leg_001` names;
##   * the clip turns the ARMS and never the head, whose quaternion the body's own
##     _track_head_look writes from the player's aim every frame;
##   * set_animation_state -- the controller's own call, once a frame -- keeps
##     that clip running: walking neither switches it nor restarts it;
##   * a body with no AnimationPlayer (the K-key pose clone) stays frozen.
##
## Nothing here writes to the world and no shot is read, so it runs headless:
##
##   probes/run_probe.sh probes/probe_player_anim.gd

const IDLE_STATE := "default/Idle"
const WALK_STATE := "default/Walk"
const ARMS := ["arm", "arm2"]
# The old idle is a GENTLE sway, not the pack's big swing: measured, each arm
# turns 3.5 degrees across the clip (the two new clips turned 65-95). The bar is
# therefore low on purpose -- it is asking whether the track moves at all, which
# is what a path left on a node the body does not have would silently not do.
const SWING_MIN := 2.0   # degrees an arm must turn over the idle
const SAMPLES := 24      # sample points across the clip's own timeline

var _failures := 0


func _initialize() -> void:
	_run()


func _ok(label: String, condition: bool, detail: String = "") -> void:
	print("PROBE %-58s %s %s" % [label, "ok  " if condition else "FAIL", detail])
	if not condition:
		_failures += 1


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

	var player: Node = main.get_node_or_null("Player")
	var model: Node3D = main.find_child("PlayerModel", true, false) as Node3D
	var anim_player: AnimationPlayer = null
	if model != null:
		anim_player = model.get_node_or_null("AnimationPlayer") as AnimationPlayer
	if player == null or model == null or anim_player == null:
		print("PROBE FAIL: no Player/PlayerModel/AnimationPlayer in Main.tscn")
		quit(1)
		return

	# One clip, and it is the old one.
	var list := anim_player.get_animation_list()
	_ok("body: holds the old idle and no walk",
		list.has(IDLE_STATE) and not list.has(WALK_STATE), str(list))
	var idle_clip: Animation = anim_player.get_animation(IDLE_STATE)
	if idle_clip == null:
		print("PROBE player animation: %d failure(s)" % _failures)
		quit(1)
		return
	_ok("body: is playing it", anim_player.current_animation == IDLE_STATE,
		"playing '%s'" % anim_player.current_animation)

	# The paths are hand-renamed on the way in, so a track can name a limb this
	# body does not have -- and a path that names nothing plays as no motion.
	var paths := PackedStringArray()
	for t in range(idle_clip.get_track_count()):
		paths.append(str(idle_clip.track_get_path(t)))
	var missing := PackedStringArray()
	for path in paths:
		if not model.has_node(path):
			missing.append(path)
	_ok("clip: every track names a node the body has", missing.is_empty(),
		"unresolved %s of %s" % [str(missing), str(paths)])
	_ok("clip: no track keeps the source rig's limb names",
		not paths.has("arm_001") and not paths.has("leg_001"), str(paths))

	# The head's quaternion is the body's own: _track_head_look writes it from the
	# aim every frame, and two writers is a race decided by node order.
	var head_tracks := 0
	for t in range(idle_clip.get_track_count()):
		if str(idle_clip.track_get_path(t)) == "head":
			head_tracks += 1
	_ok("clip: does not turn the head (the body's code owns it)", head_tracks == 0,
		"%d head track(s)" % head_tracks)

	# It swings the arms -- an idle with no motion is a file that loaded and says
	# nothing.
	var swing := _swing_per_limb(model, anim_player, idle_clip)
	var moved := true
	for path in ARMS:
		moved = moved and float(swing[path]) >= SWING_MIN
	_ok("idle: both arms swing", moved, ", ".join(_report(swing)))

	# The controller's own call, repeated: walking must not switch clip and must
	# not restart it. The controller writes the state every frame, so it is
	# silenced while we measure -- otherwise it would put the body back between
	# two of our own calls.
	player.set_process(false)
	anim_player.play(IDLE_STATE)
	anim_player.seek(0.0, true)
	model.call("set_animation_state", true)
	await process_frame
	var started: String = anim_player.current_animation
	var t0: float = anim_player.current_animation_position
	var t1 := t0
	var began := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - began) < 300.0:
		model.call("set_animation_state", true)
		await process_frame
		t1 = anim_player.current_animation_position
	_ok("state: walking still plays the old idle", started == IDLE_STATE,
		"playing '%s'" % started)
	_ok("state: told the same thing again, it does not restart", t1 > t0 + 0.1,
		"playhead %.3f -> %.3f over 0.3 s of repeated calls" % [t0, t1])

	model.call("set_animation_state", false)
	await process_frame
	_ok("state: standing still keeps it", anim_player.current_animation == IDLE_STATE,
		"playing '%s'" % anim_player.current_animation)

	# The K-key pose clone is a body with NO AnimationPlayer on purpose -- a frozen
	# dummy. The gate is "no player, no clip", so a clone has to come out with
	# nothing loaded and its limbs at rest, rather than quietly animating.
	var glb: PackedScene = load("res://models/player.glb")
	var clone: Node3D = glb.instantiate()
	var rest: Node3D = glb.instantiate()
	clone.set_script(load("res://scripts/player_model.gd"))
	root.add_child(clone)
	await process_frame
	var clone_arm := clone.get_node_or_null("arm2") as Node3D
	var rest_arm := rest.get_node_or_null("arm2") as Node3D
	_ok("clone: a body with no player loads nothing and stays at rest",
		clone.get("_anim_player") == null and clone_arm != null and rest_arm != null
			and clone_arm.quaternion.is_equal_approx(rest_arm.quaternion),
		"player %s, arm2 %s" % ["none" if clone.get("_anim_player") == null else "present",
			"at rest" if clone_arm != null and rest_arm != null \
				and clone_arm.quaternion.is_equal_approx(rest_arm.quaternion) else "moved"])
	clone.queue_free()
	rest.free()

	print("PROBE player animation: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


## How far each limb turns over the clip, in degrees. Ranges are measured between
## samples rather than against the rest pose, so a track that starts off-rest
## still reports the motion it has. The clip is PLAYED first: seek() moves the
## playhead of whatever is current.
func _swing_per_limb(model: Node3D, anim_player: AnimationPlayer, clip: Animation) -> Dictionary:
	var out := {}
	anim_player.play(IDLE_STATE)
	for path in ARMS:
		var node := model.get_node_or_null(path) as Node3D
		if node == null:
			out[path] = -1.0
			continue
		var widest := 0.0
		var first := Quaternion.IDENTITY
		for i in range(SAMPLES):
			anim_player.seek(clip.length * float(i) / float(SAMPLES - 1), true)
			var q := node.quaternion
			if i == 0:
				first = q
			widest = maxf(widest, rad_to_deg(first.angle_to(q)))
		out[path] = widest
	return out


func _report(swing: Dictionary) -> PackedStringArray:
	var parts := PackedStringArray()
	for path in swing:
		parts.append("%s %.1f deg" % [path, float(swing[path])])
	return parts
