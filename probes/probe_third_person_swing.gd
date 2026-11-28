extends SceneTree
## Does the third-person main hand swing when the first-person one does?
##
## A punch and a block place start ONE stroke on the body's right arm (scripts/player_model.gd:
## `SwingPivot`, `_apply_hand_swing`), the held block or item rides it because it hangs off that
## arm, and the legs, torso and head keep playing whatever they were playing. The triggers are
## the first-person hand's own (`viewmodel.gd` emits `swung(strength)`) and the body only plays
## them, so this measures both halves:
##
##   * on the REAL player's body, that the wire exists and a punch/place reaches it at full /
##     weaker weight -- a body that began listening after `_ready` would silently never swing;
##   * on a still body under a stub owner (probe_held_pose's shape), the stroke itself: the
##     peak is the arc's own number, a place is 0.75 of it, a second punch starts it over from
##     the top instead of stacking (and lands on the same peak, not a higher one), the arm
##     comes back to EXACTLY the rotation it started in, the item keeps its distance from the
##     shoulder all the way out and crosses toward the body's middle, and the break-loop's
##     clock restarts the stroke rather than never firing.
##
## Walking is measured on that same still body by giving its owner a walk's speed: the legs
## keep reaching their own stride while the arm swings, the clips keep turning the arm under
## the stroke, and the 18-degree held turn is not disturbed by either.
##
## The world scene is loaded only because the block and item registries are loaded by the
## engine's own controller; nothing in the live world is written, so this runs headlessly:
##
##   probes/run_probe.sh probes/probe_third_person_swing.gd

## The stroke's numbers, restated here on purpose: the probe is what holds them, so it must not
## read them back from the file it is checking.
const PEAK_DEGREES := 78.0
const PLACE_SHARE := 0.75
const HELD_PITCH := 18.0
const TOLERANCE := 3.0

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

	func get_selected_hotbar_slot() -> int:
		return 0

	func get_hotbar_slot_block_id(_slot: int) -> int:
		return block_id

	func get_aim_direction() -> Vector3:
		return Vector3(0.0, 0.0, 1.0)

	func get_velocity() -> Vector3:
		return velocity


## The stroke's own angle off rest, in degrees, FORWARD being positive -- the pivot's pitch has
## the rig's sign, and this rig comes forward on a negative turn about X.
func _forward_deg(pivot: Node3D) -> float:
	return -rad_to_deg(pivot.rotation.x)


## Watch the clock for `seconds` and report what it drew: how far forward the stroke reached,
## how far PAST rest it went on the way back in (the overshoot), and where it finished.
func _watch(pivot: Node3D, seconds: float) -> Dictionary:
	var out := {"peak": 0.0, "back": 0.0, "end": 0.0}
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame
		var forward := _forward_deg(pivot)
		out["peak"] = maxf(float(out["peak"]), forward)
		out["back"] = maxf(float(out["back"]), -forward)
	out["end"] = _forward_deg(pivot)
	return out


func _seconds(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < int(seconds * 1000.0):
		await process_frame


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
	var viewmodel: Node = main.get_node_or_null("Player/Camera3D/Viewmodel")
	var player_model := main.find_child("PlayerModel", true, false) as Node3D
	if viewmodel == null or player_model == null:
		print("PROBE FAIL: Main.tscn is missing the viewmodel or PlayerModel")
		quit(1)
		return
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
	var wired: Dictionary = await _watch(player_swing, 0.45)
	_ok("wire: ...and the body's own arm draws it and comes back",
		absf(float(wired["peak"]) - PEAK_DEGREES * PLACE_SHARE) <= TOLERANCE
			and absf(float(wired["end"])) < 0.01,
		"%.1f deg forward, ends at %.2f" % [float(wired["peak"]), float(wired["end"])])
	viewmodel.call("punch")
	var first_person: Dictionary = await _watch(player_swing, 0.45)
	_ok("first person: the hand's own swing still plays and expires",
		absf(float(first_person["peak"]) - PEAK_DEGREES) <= TOLERANCE
			and absf(float(viewmodel.get("_swing"))) < 0.001
			and absf(float(viewmodel.get("_swing_place"))) < 0.001,
		"%.1f deg, viewmodel timers %.3f / %.3f" % [float(first_person["peak"]),
			viewmodel.get("_swing"), viewmodel.get("_swing_place")])

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

	_ok("rest: a body that has not swung is exactly where its clips left it",
		pivot.rotation_degrees.is_equal_approx(Vector3.ZERO),
		"%s" % pivot.rotation_degrees)

	body.call("_on_swung", 1.0)
	var punch: Dictionary = await _watch(pivot, 0.45)
	_ok("punch: the arm reaches the arc's far end and returns to rest",
		absf(float(punch["peak"]) - PEAK_DEGREES) <= TOLERANCE
			and absf(float(punch["end"])) < 0.01
			and pivot.rotation_degrees.is_equal_approx(Vector3.ZERO),
		"%.1f deg forward, ends %.2f, %s" % [float(punch["peak"]), float(punch["end"]),
			pivot.rotation_degrees])
	_ok("punch: it overshoots a little PAST rest on the way back",
		float(punch["back"]) > 1.0 and float(punch["back"]) < PEAK_DEGREES * 0.25,
		"%.1f deg past rest" % float(punch["back"]))

	body.call("_on_swung", PLACE_SHARE)
	var place: Dictionary = await _watch(pivot, 0.45)
	_ok("place: the same stroke at the weaker endpoint",
		absf(float(place["peak"]) - PEAK_DEGREES * PLACE_SHARE) <= TOLERANCE,
		"%.1f deg against the punch's %.1f" % [float(place["peak"]), float(punch["peak"])])

	# A second click during the return starts the stroke over -- from the top, at full weight,
	# and NOT past it: the arc is a function of one clock, so two clicks cannot add up.
	body.call("_on_swung", 1.0)
	await _seconds(0.12)
	var mid_return := _forward_deg(pivot)
	body.call("_on_swung", 1.0)
	var restarted: Dictionary = await _watch(pivot, 0.45)
	_ok("restart: a second punch starts over and lands on the SAME peak",
		mid_return < PEAK_DEGREES - 2.0
			and absf(float(restarted["peak"]) - PEAK_DEGREES) <= TOLERANCE,
		"restarted at %.1f deg, reached %.1f" % [mid_return, float(restarted["peak"])])

	# The held thing hangs off the arm, so the swing carries it with no item code: its distance
	# from the shoulder cannot change, and the SWEEP shows as it crossing toward the body's
	# middle (a turn about X alone would leave x where it was).
	var rest_local: Vector3 = body.to_local(item.global_position)
	var rest_radius: float = (item.global_position - arm.global_position).length()
	var top_local := rest_local
	var top_forward := -999.0
	var top_radius := rest_radius
	body.call("_on_swung", 1.0)
	var swung_at := Time.get_ticks_msec()
	while Time.get_ticks_msec() - swung_at < 400:
		await process_frame
		if _forward_deg(pivot) > top_forward:
			top_forward = _forward_deg(pivot)
			top_local = body.to_local(item.global_position)
			top_radius = (item.global_position - arm.global_position).length()
	# In the MODEL's own units, which is what the rig is laid out in (an arm is 12.3 of them
	# here): the invariant is the equality, not the scale.
	_ok("item: it rides the hand -- same distance from the shoulder all the way out",
		absf(top_radius - rest_radius) < 0.001 and top_local.y > rest_local.y + 0.05,
		"%.4f against %.4f at rest, risen %.3f rig units"
			% [top_radius, rest_radius, top_local.y - rest_local.y])
	_ok("item: the sweep crosses toward the body's middle",
		top_local.x > rest_local.x + 0.005,
		"x %.4f -> %.4f" % [rest_local.x, top_local.x])

	# --- walking: the arm swings over a body that keeps walking ------------------
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
		swing_peak = maxf(swing_peak, _forward_deg(pivot))
	_ok("walk: the legs keep reaching their own stride under the swing",
		leg_peak > 45.0 and swing_peak > 40.0,
		"leg %.1f deg, swing %.1f" % [leg_peak, swing_peak])
	_ok("walk: the clips keep turning the arm underneath the stroke",
		arm_hi - arm_lo > 1.0,
		"arm %.2f to %.2f deg off rest" % [arm_lo, arm_hi])
	_ok("walk: the 18-degree held turn is not disturbed by either",
		absf(shoulder_hi - shoulder_lo) < 0.001 and absf(shoulder_lo + HELD_PITCH) < 0.01,
		"shoulder %.3f to %.3f deg" % [shoulder_lo, shoulder_hi])

	host.queue_free()
	main.queue_free()
	print("PROBE third-person swing: %d failure(s)" % _failures)
	quit(1 if _failures > 0 else 0)
