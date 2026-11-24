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


## Play a clip's motion SMALLER OR LARGER without touching a single key time: every
## rotation key's angle away from its part's rest rotation is multiplied by
## `factor`, about the same axis.
##
## A key is `rest * axis-turn(angle)`, so scaling is the turn measured from rest,
## shortened, and put back: `rest * IDENTITY.slerp(rest.inverse() * key, factor)`.
## The times stay where they were, so a scaled clip is the same cycle at the same
## phase taking a shorter (or longer) step -- which is what lets one walk cycle
## serve a shuffle and a sprint. `rest` maps a track's path (as a String) to that
## part's rest quaternion; a track with no entry is left alone, so a clip's own
## one-key rest tracks are not touched.
##
## `gains` scales parts DIFFERENTLY: one multiplier per track path, on top of
## `factor`, for a clip whose limbs are not on the amplitude the caller wants
## (vanilla swings a leg 1.4 rad and an arm 1.0, so a clip authored at one
## amplitude for both needs 4.8x here and 3.4x there). A path with no entry keeps
## `factor` alone.
##
## Applied IN PLACE and MULTIPLICATIVELY, so a caller that eases its factor calls
## this with `new / old` rather than re-deriving every key from the clip file.
## The mixer caches what it read out of an Animation, so a caller that is already
## playing this clip has to clear those caches afterwards.
static func scale_stride(clip: Animation, rest: Dictionary, factor: float,
		gains: Dictionary = {}) -> void:
	if is_equal_approx(factor, 1.0) and gains.is_empty():
		return
	for t in range(clip.get_track_count()):
		if clip.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		var path := str(clip.track_get_path(t))
		if not rest.has(path):
			continue
		var part_scale: float = factor * float(gains.get(path, 1.0))
		if is_equal_approx(part_scale, 1.0):
			continue
		var base: Quaternion = rest[path]
		var inverse := base.inverse()
		for k in range(clip.track_get_key_count(t)):
			var key: Quaternion = clip.track_get_key_value(t, k)
			clip.track_set_key_value(t, k, base * Quaternion.IDENTITY.slerp(inverse * key, part_scale))


## Close a cycle: rebuild every rotation track from a few harmonics of the motion
## it already has, so the curve is periodic in VALUE and in SLOPE.
##
## A hand-authored clip is not, in general, a closed curve. Every key carries the
## easing it was authored with (Blockbench bakes bezier easing into each one), and
## the easing on the FIRST and LAST keys has no neighbour to average against, so a
## cycle can pass through one value slowly at its seam and the same value quickly in
## the middle -- `Run.anim` crosses zero at 31 deg/s at the loop point and at 103
## deg/s half a cycle later. A body that slows to a crawl for a tenth of a second
## every wrap is a hitch you can see, and amplitude makes it worse: at 17 degrees it
## vanished, at vanilla's 80 it is a stutter.
##
## Keeping harmonics 1..`harmonics` of the cycle and evaluating that series at the
## clip's OWN key times gives a curve with no seam at all -- t=0 and t=length are the
## same point of a periodic function -- while the peaks, phases and opposition of the
## source stay where they were. The keys, their times and the clip's length do not
## move; only their values do. `rest` maps a track's path to its part's rest
## quaternion, and a track with no entry, or with fewer than four keys, is left alone.
##
## **One harmonic is the default, and it is the point**: one cosine is exactly what
## the motion should be -- a walk is the phase's own cosine scaled by the amount, so it
## IS a fundamental, and its two crossings are both at the maximum slope. More harmonics keep more of the source's own skew, and the skew is
## the easing: a 3-harmonic fit of `Run.anim` reproduced its easing so faithfully that
## the seam stayed 4.6x slower than the crossing half a cycle later, which is the
## hitch this exists to remove. Amplitude and phase still come from the source, and
## the amplitude is renormalised afterwards (player_model.gd::_prepare_run_clip).
static func smooth_cycle(clip: Animation, rest: Dictionary, harmonics: int = 1) -> void:
	if clip.length <= 0.0:
		return
	for t in range(clip.get_track_count()):
		if clip.track_get_type(t) != Animation.TYPE_ROTATION_3D:
			continue
		var path := str(clip.track_get_path(t))
		if not rest.has(path):
			continue
		var count := clip.track_get_key_count(t)
		if count < 4:
			continue
		var base: Quaternion = rest[path]
		# The one axis every key of this track turns about, taken from its biggest
		# turn: a walk cycle is a single hinge, and the fit needs to be a scalar one.
		var axis := Vector3.ZERO
		var biggest := 0.0
		for k in range(count):
			var rel: Quaternion = base.inverse() * (clip.track_get_key_value(t, k) as Quaternion)
			if rel.get_angle() > biggest:
				biggest = rel.get_angle()
				axis = rel.get_axis()
		if biggest <= 0.0001 or axis.length_squared() <= 0.0:
			continue
		var times := PackedFloat32Array()
		var angles := PackedFloat32Array()
		var mean := 0.0
		for k in range(count):
			times.append(clip.track_get_key_time(t, k))
			var rel: Quaternion = base.inverse() * (clip.track_get_key_value(t, k) as Quaternion)
			var angle := rel.get_angle()
			angles.append(angle if rel.get_axis().dot(axis) >= 0.0 else -angle)
			mean += angles[k]
		mean /= float(count)
		var cosines := PackedFloat32Array()
		var sines := PackedFloat32Array()
		for h in range(1, harmonics + 1):
			var c := 0.0
			var s := 0.0
			for k in range(count):
				var phase := TAU * float(h) * times[k] / clip.length
				c += (angles[k] - mean) * cos(phase)
				s += (angles[k] - mean) * sin(phase)
			cosines.append(2.0 * c / float(count))
			sines.append(2.0 * s / float(count))
		for k in range(count):
			var value := mean
			for h in range(1, harmonics + 1):
				var phase := TAU * float(h) * times[k] / clip.length
				value += cosines[h - 1] * cos(phase) + sines[h - 1] * sin(phase)
			clip.track_set_key_value(t, k, base * Quaternion(axis, value))


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
