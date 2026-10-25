extends RefCounted
## The player body's pose, per frame: a hand-port of the signature moves in the
## Fresh Animations player add-on (FA+Player v1.1, CEM animations by FreshLX --
## see docs/gui-notes.md for the credit and for why this is a port rather than a
## file import).
##
## The pack is not a set of clips. Its .jem/.jpm files recompute every part's
## rotation from live state each frame -- the swing of the legs, whether the feet
## are down, whether a sneak is held, how fast the body is rising -- and blend the
## states with weights that ease in and out, because a walk that cuts to an idle
## reads as a glitch. This is that idea, authored for THIS body's rig: one pose
## per state, weighted by what the sim is actually doing, composed onto the rig's
## rest pose.
##
## Nothing here touches a node. step() returns angles and player_model.gd applies
## them, so the whole thing is a pure function of its inputs and the probe can
## measure it without a world.
##
## Channels are DEGREES. The rig's limbs and torso all hang on +Y, so a rotation
## about +X swings an end backwards: on a limb the hand/foot goes back, on the
## torso the chest tips forward. <name>_roll is about +Z, the side-to-side tip.
##
## Sign conventions this works in, so a change cannot quietly mirror the walk:
##   * a leg at a NEGATIVE x angle is that foot FORWARD;
##   * an arm on the same side as a forward leg is BACKWARD (positive), which is
##     the counter-swing every biped walk has;
##   * the torso's POSITIVE x angle leans the chest forward.

# The sim's tick rate: get_velocity() is in blocks per tick, the sim's own unit.
const TICK_RATE := 20.0
# Radians of walk cycle per block travelled. A vanilla walking speed is 4.317
# blocks/s, so a walk turns this at ~11 rad/s: a little under two steps a second.
const STRIDE_RATE := 2.55
# Vanilla walking speed, blocks/s. Leg and arm amplitudes are quoted at it and
# scale with the real speed, so a slow step is a small one.
const WALK_SPEED := 4.317
# Degrees per arm at rest. The old Idle.anim's two tracks measured 3.5 degrees
# peak to peak, and this is the same sway: the idle should not change when the
# body stops playing a clip for it.
const IDLE_SWAY := 1.7
const LEG_SWING := 30.0
const ARM_SWING := 24.0
# How much of the amplitude a sneak keeps, and how far a sneak leans.
const SNEAK_SWING := 0.55
const SNEAK_LEAN := 24.0
# A sprint's extra: a little more stride, and a chest that leans into it.
const SPRINT_SWING := 0.18
const SPRINT_LEAN := 7.0
# Rate (per second) at which a weight crosses to a new state. State changes are
# seconds apart and the pose is continuous, so this is what makes a stop, a
# landing and a jump-into-fall read as motion rather than as a cut.
const EASE_RATE := 12.0

var _clock := 0.0          # seconds, for the idle's own sway
var _phase := 0.0          # radians of walk cycle, advanced by distance travelled
var _swim_phase := 0.0
var _swing := 0.0          # 0..~1.35, the walking amplitude, eased
var _sneak_w := 0.0        # each of these eases 0..1 toward its state
var _sprint_w := 0.0
var _air_w := 0.0
var _swim_w := 0.0


## One frame of pose, in degrees per channel.
##
## speed and vertical_speed are blocks per SECOND (the caller converts the sim's
## per-tick velocity), vertical positive upwards.
func step(delta: float, speed: float, vertical_speed: float, on_floor: bool,
		in_water: bool, sneaking: bool, sprinting: bool) -> Dictionary:
	_clock += delta
	_swing = _ease(_swing, clampf(speed / WALK_SPEED, 0.0, 1.35), delta, EASE_RATE)
	_sneak_w = _ease(_sneak_w, 1.0 if sneaking else 0.0, delta, EASE_RATE)
	_sprint_w = _ease(_sprint_w, 1.0 if sprinting else 0.0, delta, EASE_RATE)
	_swim_w = _ease(_swim_w, 1.0 if in_water else 0.0, delta, EASE_RATE)
	_air_w = _ease(_air_w, 1.0 if (not on_floor and not in_water) else 0.0,
		delta, EASE_RATE)
	# Distance travelled is the walk's clock: standing on a wall and pushing into
	# it moves nothing, so it must not stride. Water still paddles, slowly.
	if on_floor and not in_water:
		_phase += speed * STRIDE_RATE * delta
	_swim_phase += (1.6 + speed * 1.2) * delta

	# Ground states share one remainder between them; air and water are exclusive
	# and take over completely as their weight arrives.
	#
	# The walk's AMPLITUDE is _swing, inside the pose, and its weight here only
	# picks the state. Scaling both by speed would square it -- a creep at a third
	# of walking speed swung a ninth of the way, which read as no motion at all.
	# Walking of any speed therefore claims the state, and the stride shrinks in
	# the pose.
	var ground := (1.0 - _air_w) * (1.0 - _swim_w)
	var moving := clampf(_swing / 0.35, 0.0, 1.0)
	var w_swim := _swim_w
	var w_air := _air_w * (1.0 - _swim_w)
	var w_sneak := _sneak_w * ground
	var w_walk := (1.0 - _sneak_w) * moving * ground
	var w_idle := (1.0 - _sneak_w) * (1.0 - moving) * ground

	var pose := _blank()
	_add(pose, _idle_pose(), w_idle)
	_add(pose, _walk_pose(), w_walk)
	_add(pose, _sneak_pose(), w_sneak)
	_add(pose, _air_pose(vertical_speed), w_air)
	_add(pose, _swim_pose(), w_swim)
	return pose


## The idle: the same gentle arm sway the old clip had, plus a breath in the
## torso. Slow on purpose -- an idle that moves fast reads as impatience.
func _idle_pose() -> Dictionary:
	return {
		"arm": IDLE_SWAY * sin(_clock * 1.15),
		"arm2": IDLE_SWAY * sin(_clock * 1.15 + 0.9),
		"torso": 0.5 * sin(_clock * 0.6),
		"torso_roll": 0.4 * sin(_clock * 0.6 + 1.2),
	}


## The walk, and the sprint on the same cycle: legs opposed, each arm against its
## own leg, a small side-to-side weight shift in the torso.
func _walk_pose() -> Dictionary:
	var amp := _swing * (1.0 + SPRINT_SWING * _sprint_w)
	var leg := LEG_SWING * amp * sin(_phase)
	var arm := ARM_SWING * amp * (1.0 + 0.25 * _sprint_w) * sin(_phase)
	return {
		"leg": -leg,
		"leg2": leg,
		"arm": arm,
		"arm2": -arm,
		"torso": 2.0 * _swing + SPRINT_LEAN * _sprint_w,
		"torso_roll": 1.3 * _swing * sin(_phase),
	}


## Sneaking: the chest tips forward, the hands hang slightly ahead of it, and the
## stride all but disappears. There is no knee in the rig, so the crouch reads
## through the lean rather than through a lowered body; the camera has already
## dropped, which is what sells it.
func _sneak_pose() -> Dictionary:
	var leg := LEG_SWING * _swing * SNEAK_SWING * sin(_phase)
	var arm := ARM_SWING * _swing * SNEAK_SWING * sin(_phase)
	return {
		"leg": -leg,
		"leg2": leg,
		"arm": arm - 8.0,
		"arm2": -arm - 8.0,
		"torso": SNEAK_LEAN,
		"torso_roll": 0.6,
	}


## Airborne: the arms come up and out, the legs split -- and the split leans with
## the vertical speed, so a rise and a fall are different shapes rather than one
## held pose.
func _air_pose(vertical_speed: float) -> Dictionary:
	var rise := clampf(vertical_speed / 6.0, -1.0, 1.0)   # +1 rising, -1 falling
	var arm := -32.0 - 12.0 * (1.0 - rise) * 0.5
	return {
		"leg": -14.0 - 4.0 * (1.0 - rise) * 0.5,
		"leg2": 10.0 + 4.0 * (1.0 - rise) * 0.5,
		"arm": arm,
		"arm2": arm,
		"arm_roll": -9.0,
		"arm2_roll": 9.0,
		"torso": 3.0,
	}


## Swimming: the chest pitches forward over the water and the arms paddle out of
## phase with each other, which reads as stroke rather than as a walk.
func _swim_pose() -> Dictionary:
	var stroke := sin(_swim_phase)
	return {
		"arm": -50.0 + 26.0 * stroke,
		"arm2": -50.0 - 26.0 * stroke,
		"leg": 9.0 * sin(_swim_phase * 1.3),
		"leg2": -9.0 * sin(_swim_phase * 1.3),
		"torso": 14.0 + 4.0 * sin(_swim_phase * 0.7),
		"torso_roll": 1.5 * stroke,
	}


func _blank() -> Dictionary:
	return {
		"arm": 0.0, "arm2": 0.0, "leg": 0.0, "leg2": 0.0, "torso": 0.0,
		"torso_roll": 0.0, "arm_roll": 0.0, "arm2_roll": 0.0,
	}


## pose += other * weight. Every channel a pose names is summed; one it omits
## contributes nothing, which is why _blank() exists.
func _add(pose: Dictionary, other: Dictionary, weight: float) -> void:
	if weight <= 0.0:
		return
	for channel in other:
		pose[channel] += float(other[channel]) * weight


func _ease(current: float, target: float, delta: float, rate: float) -> float:
	if rate <= 0.0:
		return target
	return lerpf(current, target, 1.0 - exp(-rate * delta))
