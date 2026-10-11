extends SceneTree
## Does the player's own body have a WALK, and does it play it?
##
## The body's clips are extracted from another rig (Animations/Idle_new.anim,
## Animations/Walk.anim, out of models/minecraft-idle-and-walking-animation/
## source/model.gltf) and re-homed onto this body's parts by name on the way in
## (scripts/clip_retarget.gd). Two things can go wrong there and neither one makes
## a sound: a track can be renamed onto a node this body does not have -- which
## plays as no motion at all -- and a clip can be asked to write a limb the body's
## own code already writes every frame (the head, which tracks the player's aim).
##
## So this asks the BODY, not the files:
##
##   * both clips are in the body's own player, under the names the controller
##     asks for (`default/Idle`, `default/Walk`);
##   * every track of both names a node this body actually has;
##   * the WALK turns the limbs and not a little -- arms and legs, and the two legs
##     are OPPOSED to each other rather than moving as one, which is the difference
##     between a walk and a shuffle;
##   * the walk's bob stays on the body: a POSITION track carries the SOURCE rig's
##     own pivot in the source rig's own units, so a mis-scaled one throws the limb
##     clear of the torso (measured: ~1.5 px on the torso, ~4.6 px on a leg);
##   * neither clip carries a HEAD ROTATION track, because the body's own
##     _track_head_look owns that quaternion -- two writers is a race decided by
##     node order, one frame at a time;
##   * the walk LEADS THE WAY THE BODY FACES. This is the one that was wrong
##     first: the pack's own model faces -Z and this body faces +Z, so a faithful
##     copy of the pack's numbers is a walk seen from behind -- the feet lead
##     backwards and slide. Both facings are read off the rigs' own SKINS here
##     (a 64x64 head keeps its face texels in the u,v [8,16) band, and that band
##     sits on a different z face of the head box in each rig), so the check does
##     not restate a convention: the body's forward swing must point at its own
##     face, and the pack's at its own.
##   * the controller's own call -- set_animation_state, once per frame, with the
##     state it can see -- changes the clip, and telling it the same thing again
##     does NOT restart it (a walk restarted every frame is a walk frozen on frame
##     zero, which is what a naive play() gives).
##
## Nothing here writes to the world, and no shot is read, so it runs headless:
##
##   probes/run_probe.sh probes/probe_player_anim.gd

const ClipRetarget := preload("res://scripts/clip_retarget.gd")

const LIMB_SWING := 20.0     # degrees an arm must turn over the walk
const LEG_SWING := 30.0      # ...and a leg, which swings further
const LEG_APART := 20.0      # degrees the two legs must be apart somewhere in it
const TORSO_SWING := 2.0     # the walk's own body rotation, in degrees
const BOB_LIMIT := 8.0       # px a limb may wander from its own rest position
const IDLE_LIMIT := 30.0     # degrees: the idle must stay well inside the walk
const SAMPLES := 24          # samples of each clip's own timeline
const LEAD_STEPS := 96       # ...and of the leg, to find the fastest turn in it
# The pack the clips were lifted out of, for the facing check below.
const SOURCE_SCENE := "res://models/minecraft-idle-and-walking-animation/source/model.gltf"
const SOURCE_CLIP := "walking_test"
const WALK_STATE := "default/Walk"
const IDLE_STATE := "default/Idle"
# The limbs the walk has to move, and which ones are legs.
const ARMS := ["arm", "arm2"]
const LEGS := ["leg", "leg2"]

const PlayerModel := preload("res://scripts/player_model.gd")

# Instants of the cycle the two rigs are compared at, and the pack rig's unit --
# a Blockbench block is 16 of the body's pixels. That scale only spreads the
# comparison out; the claim below is which NODE a limb is nearest, and the two
# candidates are 11 px apart against a spread of about 1.
const TIMES := [0.05, 0.2, 0.35, 0.5, 0.65, 0.8]
const SOURCE_SCALE := 16.0

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

	var idle_clip: Animation = anim_player.get_animation(IDLE_STATE)
	var walk_clip: Animation = anim_player.get_animation(WALK_STATE)
	_ok("body: the player holds an Idle and a Walk",
		idle_clip != null and walk_clip != null,
		"Idle %s, Walk %s" % ["yes" if idle_clip != null else "NO",
			"yes" if walk_clip != null else "NO"])
	if idle_clip == null or walk_clip == null:
		print("PROBE player animation: %d failure(s)" % _failures)
		quit(1)
		return

	var missing := PackedStringArray()
	for clip in [idle_clip, walk_clip]:
		missing.append_array(ClipRetarget.unresolved(clip, model))
	_ok("body: every track names a node the body has", missing.is_empty(),
		"unresolved: %s" % str(missing))

	# ...and neither may turn the head: the body's own code writes that quaternion
	# from the player's aim every frame.
	var head_tracks := 0
	for clip in [idle_clip, walk_clip]:
		for t in range((clip as Animation).get_track_count()):
			if str((clip as Animation).track_get_path(t)) == "head" \
					and (clip as Animation).track_get_type(t) == Animation.TYPE_ROTATION_3D:
				head_tracks += 1
	_ok("clips: neither turns the head (the body's code owns it)", head_tracks == 0,
		"%d head rotation track(s)" % head_tracks)

	# The controller writes the body's state every frame from _process, so it is
	# silenced for the measurements: otherwise it would put the body back to Idle
	# between two of our own calls.
	player.set_process(false)

	var walk := _swing_per_limb(model, anim_player, WALK_STATE, walk_clip)
	var walked := true
	for path in ARMS:
		walked = walked and float(walk[path]) >= LIMB_SWING
	for path in LEGS:
		walked = walked and float(walk[path]) >= LEG_SWING
	_ok("walk: every arm and leg swings", walked, ", ".join(_report(walk)))
	_ok("walk: the body turns with it", float(walk["torso"]) >= TORSO_SWING,
		"torso %.1f deg" % float(walk["torso"]))

	var apart := _legs_apart(model, anim_player, WALK_STATE, walk_clip)
	_ok("walk: the two legs are opposed, not one shuffle", apart >= LEG_APART,
		"%.1f deg apart at their widest" % apart)

	# Which way the walk LEADS, against which way this body's own skin faces. The
	# two are read off the rigs themselves, so a clip that runs the other way along
	# the body's own axis fails here rather than in the player's eye.
	var body_facing := _facing(model.get_node_or_null("head") as MeshInstance3D)
	var body_leads := _leads(model, anim_player, walk_clip, "leg2")
	_ok("skin: this body's own face texels sit on its +Z face", body_facing > 0.0,
		"front-band vertices at mean z %+.2f" % body_facing)
	_ok("walk: the feet lead the way the body faces", body_leads * body_facing > 0.0,
		"forward swing toward %s, face on %s" % [_axis(body_leads), _axis(body_facing)])

	# ...and the pack faces the OTHER way, which is the whole reason the clips need
	# turning on the way in (CLIP_FRAME in player_model.gd). Re-extracting from a
	# pack that faces this way would leave the clip unturned and fail the check
	# above while this one still passed.
	var pack: Node3D = (load(SOURCE_SCENE) as PackedScene).instantiate()
	root.add_child(pack)
	await process_frame
	var pack_player := _find_player(pack)
	pack_player.play(SOURCE_CLIP)
	var src_clip: Animation = pack_player.get_animation(SOURCE_CLIP)
	var pack_facing := _facing(pack.get_node_or_null("Node/Head2/Head") as MeshInstance3D)
	var pack_leads := _leads(pack, pack_player, src_clip, "Node/RightLeg")
	_ok("pack: its rig faces the other way, and its own clip leads that way",
		pack_facing < 0.0 and pack_leads * pack_facing > 0.0,
		"pack face on %s, its forward swing toward %s -- the opposite of this body"
			% [_axis(pack_facing), _axis(pack_leads)])

	# The complete condition, in GEOMETRY rather than in convention: put the two
	# rigs facing the same way (the frame is DERIVED from the two facings just
	# measured, not restated) and hold the re-homed clip to the only thing it can be
	# asked -- every limb of the pack's walk must sit nearest the body node the
	# SHIPPED mapping gave it, at every instant of the cycle. This is the check the
	# two above are not: a limb on the wrong node is a mirrored body, which the
	# facing and lead checks both pass, and a half-cycle error fails it at every
	# instant while a wrong phase relationship fails it at most of them.
	var frame := Quaternion.IDENTITY
	if pack_facing * body_facing < 0.0:
		frame = Quaternion(Vector3.UP, PI)
	var mapping: Dictionary = PlayerModel.CLIP_TRACK_MAP
	var pairs_wrong := 0
	var worst := 0.0
	var margin := 1e9
	for i in range(TIMES.size()):
		pack_player.seek(src_clip.length * TIMES[i], true)
		anim_player.seek(walk_clip.length * TIMES[i], true)
		var want := {}
		var got := {}
		for src_path in mapping:
			want[src_path] = frame * (_limb_centre(pack.get_node(src_path), pack) * SOURCE_SCALE)
			got[mapping[src_path]] = _limb_centre(model.get_node(mapping[src_path]), model)
		for src_path in mapping:
			var target: Vector3 = want[src_path]
			var nearest := ""
			var nearest_d := 1e9
			var mapped_d := (target - (got[mapping[src_path]] as Vector3)).length()
			for body_path in got:
				var d: float = (target - (got[body_path] as Vector3)).length()
				if d < nearest_d:
					nearest_d = d
					nearest = str(body_path)
			if nearest != str(mapping[src_path]):
				pairs_wrong += 1
			worst = maxf(worst, mapped_d)
			margin = minf(margin, nearest_d - mapped_d)
	_ok("walk: every limb sits on the node the mapping gives it",
		pairs_wrong == 0,
		"%d of %d pairings wrong over %d instants; furthest limb from its own node %.1f px, "
			% [pairs_wrong, mapping.size() * TIMES.size(), TIMES.size(), worst]
			+ "and the nearest wrong node is %.1f px further than the right one" % margin)
	pack.queue_free()

	var bob := _bob(model, anim_player, WALK_STATE, walk_clip)
	_ok("walk: the bob stays on the body", bob <= BOB_LIMIT,
		"%.2f px of travel at most" % bob)

	var idle := _swing_per_limb(model, anim_player, IDLE_STATE, idle_clip)
	var idle_max := 0.0
	var walk_max := 0.0
	for path in idle:
		idle_max = maxf(idle_max, float(idle[path]))
	for path in walk:
		walk_max = maxf(walk_max, float(walk[path]))
	_ok("idle: still, next to the walk", idle_max <= IDLE_LIMIT and idle_max < walk_max,
		"idle max %.1f deg against the walk's %.1f" % [idle_max, walk_max])

	# The controller's own call, and then the same call again: the second must not
	# restart the clip.
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
	_ok("state: walking plays the walk", started == WALK_STATE, "playing '%s'" % started)
	_ok("state: told the same thing again, it does not restart", t1 > t0 + 0.1,
		"playhead %.3f -> %.3f over 0.3 s of repeated calls" % [t0, t1])

	model.call("set_animation_state", false)
	await process_frame
	_ok("state: stopping goes back to the idle",
		anim_player.current_animation == IDLE_STATE,
		"playing '%s'" % anim_player.current_animation)

	# The K-key pose clone is a body with NO AnimationPlayer on purpose -- a frozen
	# dummy. The gate is "no player, no clips", so a clone has to come out with
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

	# The fallback, which is the only thing that keeps Idle.anim on disk: with no
	# new clips to load, the body plays the old one -- and asked to WALK it must
	# land on the idle rather than on an animation it has no clip for. Driven by
	# hand rather than by deleting the two clips, so the check is not the removal.
	var legacy: Node3D = glb.instantiate()
	legacy.set_script(load("res://scripts/player_model.gd"))
	var legacy_player := AnimationPlayer.new()
	legacy.add_child(legacy_player)
	# Built by hand -- a body with a player that has never run _ready -- so the
	# check is about the fallback and not about the two clips being absent.
	legacy.set("_anim_player", legacy_player)
	legacy.call("_load_legacy_idle")
	legacy.call("set_animation_state", true)
	await process_frame
	var legacy_clip: Animation = legacy_player.get_animation(IDLE_STATE)
	var legacy_paths := PackedStringArray()
	if legacy_clip != null:
		for t in range(legacy_clip.get_track_count()):
			legacy_paths.append(str(legacy_clip.track_get_path(t)))
	_ok("fallback: with only the legacy clip the body idles, even asked to walk",
		legacy_clip != null and not legacy_player.has_animation(WALK_STATE)
			and str(legacy.get("_clip_state")) == "Idle"
			and not legacy_paths.has("arm_001") and legacy_paths.has("arm"),
		"states %s, state '%s', paths %s" % [str(legacy_player.get_animation_list()),
			str(legacy.get("_clip_state")), str(legacy_paths)])
	legacy.free()

	print("PROBE player animation: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)


## How far each limb turns over one clip, in degrees. The state is PLAYED first:
## seek() moves the playhead of whatever is current, so seeking a clip that is not
## the current animation would measure the other one.
func _swing_per_limb(model: Node3D, anim_player: AnimationPlayer, state: String,
		clip: Animation) -> Dictionary:
	var out := {}
	anim_player.play(state)
	for path in ARMS + LEGS + ["torso"]:
		var node := model.get_node_or_null(path) as Node3D
		if node == null:
			out[path] = -1.0
			continue
		var widest := 0.0
		var first := Quaternion.IDENTITY
		for i in range(SAMPLES + 1):
			anim_player.seek(clip.length * float(i) / float(SAMPLES), true)
			var pose := node.quaternion
			if i == 0:
				first = pose
			else:
				widest = maxf(widest, rad_to_deg(first.angle_to(pose)))
		out[path] = widest
	return out


## The widest angle between the two legs' orientations over the clip. A walk holds
## them opposed; legs that move as one -- a clip whose two leg tracks both landed
## on one node, or a rig that is not opposed -- read as a shuffle instead.
func _legs_apart(model: Node3D, anim_player: AnimationPlayer, state: String,
		clip: Animation) -> float:
	var left := model.get_node_or_null("leg") as Node3D
	var right := model.get_node_or_null("leg2") as Node3D
	if left == null or right == null:
		return -1.0
	anim_player.play(state)
	var widest := 0.0
	for i in range(SAMPLES + 1):
		anim_player.seek(clip.length * float(i) / float(SAMPLES), true)
		widest = maxf(widest, rad_to_deg(left.quaternion.angle_to(right.quaternion)))
	return widest


## How far any limb wanders from its own rest position over the clip, in the
## body's own units (px). The POSITION tracks are where a mis-scaled retarget
## shows, because they are the only ones that carry the source rig's numbers.
func _bob(model: Node3D, anim_player: AnimationPlayer, state: String,
		clip: Animation) -> float:
	anim_player.play(state)
	var worst := 0.0
	for path in ARMS + LEGS + ["torso", "head"]:
		var node := model.get_node_or_null(path) as Node3D
		if node == null:
			continue
		var rest := node.position
		for i in range(SAMPLES + 1):
			anim_player.seek(clip.length * float(i) / float(SAMPLES), true)
			worst = maxf(worst, (node.position - rest).length())
	return worst


## Which way a rig's own skin faces, in that rig's own space: a 64x64 skin lays
## the head's six faces across the upper-left 32x16, with the FACE (the eyes) at
## u,v [8,16) -- so the vertices of the head mesh carrying those texels are the
## front of the head, and the sign of their z is the way the rig looks. Read this
## way round on purpose: it says what the artwork says, with no convention
## restated and no constant to go stale.
func _facing(head: MeshInstance3D) -> float:
	if head == null or head.mesh == null:
		return 0.0
	var arrays := head.mesh.surface_get_arrays(0)
	var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV]
	var total := 0.0
	var count := 0
	for i in range(mini(verts.size(), uvs.size())):
		if uvs[i].x >= 8.0 / 64.0 and uvs[i].x < 16.0 / 64.0 \
				and uvs[i].y >= 8.0 / 64.0 and uvs[i].y < 16.0 / 64.0:
			total += verts[i].z
			count += 1
	return total / float(count) if count > 0 else 0.0


## The direction a walk's leg LEADS in a rig's own axes: +Z, the way a face on
## the +Z side looks, or -Z -- the same convention _facing reports, so the two can
## be compared directly. The swing leg travels forward quickly and the stance leg
## back slowly, so the fastest turn of the hip is the way the clip itself calls
## "forward": the animation's own statement about direction, with nothing else to
## compare it to. Play the clip first: this only seeks within whatever is current.
func _leads(rig: Node3D, anim_player: AnimationPlayer, clip: Animation, leg: String) -> float:
	var node := rig.get_node_or_null(leg) as Node3D
	if node == null:
		return 0.0
	var previous := 0.0
	var fastest := 0.0
	for i in range(LEAD_STEPS + 1):
		anim_player.seek(clip.length * float(i) / float(LEAD_STEPS), true)
		var angle := _swing_angle(node)
		if i > 0 and absf(angle - previous) > absf(fastest):
			fastest = angle - previous
		previous = angle
	# _swing_angle is positive toward -Z, so the lead direction flips it into the
	# +Z-is-forward convention _facing uses.
	return -fastest


## The union AABB centre of every mesh under `node`, in the RIG ROOT's own space.
## The body's limbs ARE the mesh nodes; the pack's are pivots holding meshes, so
## both shapes are taken. Root space keeps the body's own placement in Main.tscn
## out of it -- that flip belongs to the player, not to the clip.
func _limb_centre(node: Node, rig: Node3D) -> Vector3:
	if node == null or rig == null:
		return Vector3.ZERO
	var box := AABB()
	var any := false
	var meshes: Array = node.find_children("", "MeshInstance3D", true, false)
	if node is MeshInstance3D:
		meshes.append(node)
	for child in meshes:
		var mi := child as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var local: Transform3D = rig.global_transform.affine_inverse() * mi.global_transform
		var b: AABB = local * mi.mesh.get_aabb()
		box = b if not any else box.merge(b)
		any = true
	return box.get_center() if any else Vector3.ZERO


## A limb's own angle about its rig's X axis: positive turns its foot toward -Z.
## Its LOCAL basis on purpose -- the body's node in Main.tscn is turned and scaled
## to face the player, and reading that would put the clip and the skin in two
## different spaces (the skin check reads mesh-local vertices). Both rigs' pivot
## parents have an identity basis, so local IS the rig's own axes.
func _swing_angle(node: Node3D) -> float:
	var down := -node.transform.basis.y
	return rad_to_deg(atan2(-down.z, -down.y))


func _axis(value: float) -> String:
	return "+Z" if value > 0.0 else "-Z"


func _find_player(node: Node) -> AnimationPlayer:
	if node is AnimationPlayer:
		return node
	for c in node.get_children():
		var found := _find_player(c)
		if found != null:
			return found
	return null


func _report(swings: Dictionary) -> PackedStringArray:
	var out := PackedStringArray()
	for path in swings:
		out.append("%s %.1f deg" % [path, float(swings[path])])
	return out
