extends RefCounted
## Re-homes one clip onto another rig, by name.
##
##   const ClipRetarget := preload("res://scripts/clip_retarget.gd")
##   var clip := ClipRetarget.retarget(source, TRACK_MAP, body, 16.0, ["head"])
##
## A clip is not portable as it stands. Its tracks name NODES, and two exporters
## of the same character disagree about those names -- this body's parts are
## `head`/`arm`/`arm2`/`leg`/`leg2`, while the pack its clips came from calls them
## `Node/Head2`/`Node/RightArm`/... -- and the numbers inside the tracks are in the
## rig's own units, measured from the rig's own pivots. So each track is re-homed:
##
##   * the path is looked up in the caller's mapping. A track whose limb the
##     mapping does not name is DROPPED AND REPORTED: leaving its path as it came
##     would leave a track that plays perfectly and turns nothing;
##   * a ROTATION track copies verbatim -- a joint's rotation is joint-relative,
##     so it means the same thing on any rig that hangs that limb off that joint;
##   * a POSITION track does not. Its keys are where the SOURCE rig's pivot sits,
##     in the SOURCE rig's units, so writing them here would teleport the limb.
##     The track is rewritten as the destination node's own rest position plus the
##     source's own movement away from its first key, scaled: what carries is the
##     bob, not the pivot;
##   * a limb whose rotation the code drives itself loses its rotation track and
##     keeps its position one. Two writers of one quaternion is a race settled by
##     node order, not a feature (see rotation_owned_by_code).
##
## **The two rigs need not face the same way, and `frame` is the turnaround.**
## A clip's numbers are expressed in ITS rig's axes, so a faithful copy plays the
## same motion in the destination's axes -- which, if the destination faces the
## other way, is that motion seen from behind: a walk whose feet lead backwards
## and slide. `frame` is the rotation that takes the clip rig's axes onto this
## rig's, applied to EVERY track: a rotation is conjugated (frame * q *
## frame.inverse() -- a rotation composed onto itself, so the rest pose stays the
## rest pose) and a position delta is rotated (frame * delta, because a bob is a
## direction). Identity is the default, and it is wrong for two rigs that face
## opposite ways -- measure, do not assume: see the callers for how the facing of
## each rig is read off its own skin (the 64x64 head's face texels).
##
## A track's key times are kept as they are, so a clip that starts late still
## starts late, and length/loop/step come from the source clip unchanged.

## Re-home `clip` onto the nodes of `body`.
##
## `mapping` takes the source track's path (as a String) to the destination
## node's path under `body`. `unit_scale` is how many of this rig's units one of
## the source rig's is (the two rigs measure time the same way -- seconds -- and
## limbs in their own units, so only movement needs it). `frame` is the rotation
## from the clip rig's axes onto this rig's (see above; identity assumes the two
## face the same way). `rotation_owned_by_code` names destination paths whose
## rotation the caller writes itself every frame.
static func retarget(clip: Animation, mapping: Dictionary, body: Node, unit_scale: float,
		frame: Quaternion = Quaternion.IDENTITY,
		rotation_owned_by_code: PackedStringArray = PackedStringArray()) -> Animation:
	var out := Animation.new()
	out.resource_name = clip.resource_name
	out.length = clip.length
	out.step = clip.step
	out.loop_mode = clip.loop_mode

	var kept := 0
	for t in range(clip.get_track_count()):
		var from := str(clip.track_get_path(t))
		if not mapping.has(from):
			print("clip_retarget: dropped track '%s' -- no limb in %s is named that"
				% [from, body.name])
			continue
		var to := NodePath(mapping[from])
		var node := body.get_node_or_null(to) as Node3D
		if node == null:
			print("clip_retarget: dropped track '%s' -- %s has no node '%s'"
				% [from, body.name, to])
			continue
		var kind := clip.track_get_type(t)
		if kind == Animation.TYPE_ROTATION_3D and rotation_owned_by_code.has(str(to)):
			print("clip_retarget: dropped track '%s' rotation -- the code drives it" % to)
			continue
		if kind != Animation.TYPE_ROTATION_3D and kind != Animation.TYPE_POSITION_3D:
			print("clip_retarget: dropped track '%s' -- track type %d is not a transform"
				% [from, kind])
			continue
		_copy_track(clip, t, out, to, node, unit_scale, frame, kind)
		kept += 1
	print("clip_retarget: %s -> %s, %d of %d track(s) kept%s"
		% [clip.resource_name, body.name, kept, clip.get_track_count(),
			"" if frame.is_equal_approx(Quaternion.IDENTITY) else ", turned through %s" % frame])
	return out


## One track, moved: same keys, same times, new path, and -- for a position track
## -- rewritten from the destination node's own rest position. Both kinds are
## carried through `frame`, which is the turnaround between the two rigs' axes.
static func _copy_track(source: Animation, t: int, out: Animation, to: NodePath, node: Node3D,
		unit_scale: float, frame: Quaternion, kind: Animation.TrackType) -> void:
	var index := out.add_track(kind)
	out.track_set_path(index, to)
	out.track_set_interpolation_type(index, source.track_get_interpolation_type(t))
	out.track_set_interpolation_loop_wrap(index, source.track_get_interpolation_loop_wrap(t))
	out.track_set_enabled(index, source.track_is_enabled(t))
	var keys := source.track_get_key_count(t)
	if keys <= 0:
		return
	var frame_inverse := frame.inverse()
	# The source's own displacement from its first key, which is the part of a
	# position track that is about the animation rather than about the rig.
	var origin: Variant = source.track_get_key_value(t, 0)
	for k in range(keys):
		var value: Variant = source.track_get_key_value(t, k)
		if kind == Animation.TYPE_POSITION_3D:
			var delta: Vector3 = (value as Vector3) - (origin as Vector3)
			value = node.position + frame * (delta * unit_scale)
		else:
			# Conjugation, not composition: the rest pose (identity) has to stay the
			# rest pose, and only the axis of the turn moves with the frame.
			value = frame * (value as Quaternion) * frame_inverse
		out.track_insert_key(index, source.track_get_key_time(t, k), value)


## The destination paths a clip would have to name to be playable on `body`:
## every track of `clip` whose path resolves under `body`. Returns the paths that
## do NOT -- the list a caller should see as empty before it plays anything.
static func unresolved(clip: Animation, body: Node) -> PackedStringArray:
	var missing := PackedStringArray()
	var seen := {}
	for t in range(clip.get_track_count()):
		var path := clip.track_get_path(t)
		if seen.has(path):
			continue
		seen[path] = true
		if body.get_node_or_null(path) == null:
			missing.append(str(path))
	return missing
