extends Node3D
## Vanilla 1.8.8-accurate combat physics for the K-key pose clone ("dummy").
##
## Implements the documented vanilla living-entity combat movement —
## gravity, drag, knockback and hit throttling — in blocks/tick at 20 tps:
##
## Per tick:
##   - The horizontal drag factor is read from the PRE-MOVE on-ground state:
##     0.546 (standard block slipperiness 0.6 x 0.91) when grounded, 0.91 in
##     air. Vanilla reads it before moving, so the first tick of a knockback
##     keeps ground friction instead of instantly switching to air drag.
##   - moveEntity(motion) -> ChunkManager.resolve_voxel_collision. Collided
##     axes have their velocity zeroed (horizontal axes on wall hits, the
##     vertical axis on landing/ceiling hits).
##   - gravity: motion.y -= 0.08
##   - vertical drag: motion.y *= 0.98
##   - horizontal drag: motion.x/z *= the pre-move factor above
##   - |motion| < 0.005 on any axis is zeroed (per-tick velocity cull).
##
## Base knockback on hit:
##   - halves the current velocity on every axis
##   - adds 0.4 blocks/tick horizontally, pointing directly away from the
##     attacker along the two entities' actual x/z positions (not the
##     attacker's facing)
##   - adds 0.4 blocks/tick upward, capped at 0.4
##
## Hit throttling (i-frames): a full hit opens a 20-tick hurt-resistance
## window; hits landing while more than 10 ticks remain are fully resisted
## (no damage, no knockback). Every punch deals equal damage, so resisted
## hits are no-ops and knockback lands at most once per ~10 ticks (0.5 s)
## no matter how fast the player clicks.
##
## Sprint bonus: a sprinting attacker adds 0.5 blocks/tick along their facing
## plus 0.1 upward on top of the base knockback, applied after it with no
## re-cap, so the victim inherits the attacker's forward momentum. Walking
## adds nothing in vanilla.
##
## Movement/collision is resolved by the engine's CollisionResolver (the same
## code path the player uses), which implements vanilla's axis-separated
## swept collision against each block's real collision boxes, Y-first then the
## larger horizontal axis, with the grounded state from a floor probe — step_height 0, so
## a knocked entity never auto-steps up (vanilla knockback behavior).
##
## The node origin is the entity's feet (position = feet center), matching the
## player sim convention and the 0.6 x 1.8 x 0.6 vanilla player box used by the
## punch raycast in PlayerController.
##
## Rendering is interpolated like the player sim: physics advances in 20 Hz
## ticks, then each rendered frame lerps between the previous and current tick
## positions by the accumulator fraction, so motion stays smooth at any FPS.

# vanilla physics constants (blocks / tick at 20 tps).
const TICK_RATE := 20.0
const TICK := 1.0 / TICK_RATE
const WIDTH := 0.6   # vanilla player box width
const HEIGHT := 1.8  # vanilla player box height
const GRAVITY := 0.08
const DRAG_AIR := 0.91
const DRAG_GROUND := 0.6 * 0.91 # 0.546 — standard block slipperiness * 0.91
const VERTICAL_DRAG := 0.98
const VELOCITY_CUTOFF := 0.005  # vanilla per-tick velocity cull
const KNOCKBACK_SPEED := 0.4    # vanilla base knockback speed (0.4 blocks/tick)
# Vanilla hurt resistance (i-frames): a full hit opens a 20-tick window; a
# hit while more than 10 ticks remain is fully resisted.
const MAX_HURT_RESISTANT_TIME := 20
const HURT_RESIST_WINDOW := 10 # half the 20-tick window

var _motion := Vector3.ZERO # blocks/tick
var _on_ground := false
var _hurt_resistant_ticks := 0
var _accum := 0.0
var _chunk_manager: Node = null
# Tick-simulated feet position and the position at the start of the most
# recently completed tick (the interpolation pair; see _physics_process).
var _pos := Vector3.ZERO
var _prev_pos := Vector3.ZERO

func _ready() -> void:
	# Same node path the C++ side uses (break_block/place_block).
	_chunk_manager = get_node_or_null("/root/Main/ChunkManager")
	_pos = global_position
	_prev_pos = _pos

func _physics_process(delta: float) -> void:
	if _chunk_manager == null:
		return
	# Fixed 20 Hz accumulator: vanilla ticks the entity 20 times per second,
	# so the velocity math below is per-tick regardless of render framerate.
	# Clamp catch-up after a stall (e.g. window minimized) to a few ticks.
	_accum = minf(_accum + delta, TICK * 5.0)
	while _accum >= TICK:
		_accum -= TICK
		_prev_pos = _pos
		_tick()
	# Render interpolation (same scheme as the player sim): the node sits at
	# the lerp of the last two tick positions by the partial tick remaining,
	# so the dummy glides smoothly between 20 Hz physics steps.
	global_position = _prev_pos.lerp(_pos, _accum / TICK)

func _tick() -> void:
	# Resolve the full 3-axis sweep against the voxel world. The engine returns
	# the clamped position and which axes collided; vanilla zeroes colliding
	# axes' velocities (horizontal on wall hits, vertical on landing/ceiling
	# hits), so mirror that.
	# The horizontal drag factor must be read from the PRE-move on-ground state
	# (the state left by the previous tick), not from the floor probe after the
	# move — otherwise the launch tick of a knockback uses air drag 0.91
	# instead of ground drag 0.546 and the dummy over-flies by ~25-30%.
	var was_on_ground := _on_ground
	var res: Dictionary = _chunk_manager.resolve_voxel_collision(
		_pos, _motion, Vector3(WIDTH, HEIGHT, WIDTH))
	_pos = res["position"]
	if res.get("collided_x", false):
		_motion.x = 0.0
	if res.get("collided_y", false):
		_motion.y = 0.0
	if res.get("collided_z", false):
		_motion.z = 0.0
	_on_ground = res.get("on_floor", false)

	# Gravity + drag, applied after the move like vanilla.
	_motion.y -= GRAVITY
	_motion.y *= VERTICAL_DRAG
	var drag := DRAG_GROUND if was_on_ground else DRAG_AIR
	_motion.x *= drag
	_motion.z *= drag

	# Per-tick velocity cull.
	if absf(_motion.x) < VELOCITY_CUTOFF:
		_motion.x = 0.0
	if absf(_motion.y) < VELOCITY_CUTOFF:
		_motion.y = 0.0
	if absf(_motion.z) < VELOCITY_CUTOFF:
		_motion.z = 0.0

	# Hurt resistance ticks down once per entity tick.
	if _hurt_resistant_ticks > 0:
		_hurt_resistant_ticks -= 1

# Base vanilla 1.8.8 knockback, called by PlayerController when the player
# punches the dummy. attacker_position is the attacker's feet position; the
# knockback direction is from the entities' actual x/z positions, not the
# attacker's facing:
#   motion -= horizontal(attacker - victim) / dist * 0.4
# i.e. the dummy is pushed directly AWAY from the attacker along the line
# between the two entities' positions.
func apply_knockback(attacker_position: Vector3, extra_velocity := Vector3.ZERO) -> void:
	# Inside the first 10 ticks of the hurt-resistance window a repeat hit of
	# equal damage is fully resisted — no knockback. This is what stops
	# spam/hold-clicking from stacking 0.4 every swing.
	if _hurt_resistant_ticks > HURT_RESIST_WINDOW:
		print("Dummy punch resisted (i-frames: %d ticks left)" % _hurt_resistant_ticks)
		return
	_hurt_resistant_ticks = MAX_HURT_RESISTANT_TIME
	var dir := Vector3(
		_pos.x - attacker_position.x,
		0.0,
		_pos.z - attacker_position.z)
	if dir.length_squared() < 1e-8:
		# Vanilla jitters the direction randomly when attacker and victim share
		# the same x/z position.
		dir = Vector3(randf_range(-1.0, 1.0), 0.0, randf_range(-1.0, 1.0)).normalized()
	else:
		dir = dir.normalized()
	_motion.x = _motion.x * 0.5 + dir.x * KNOCKBACK_SPEED
	_motion.y = _motion.y * 0.5 + KNOCKBACK_SPEED
	_motion.z = _motion.z * 0.5 + dir.z * KNOCKBACK_SPEED
	_motion.y = minf(_motion.y, KNOCKBACK_SPEED)
	# The sprint bonus is added after the base knockback (and its vertical
	# cap), so the dummy's launch can exceed 0.4 vertically.
	_motion += extra_velocity
	print("Dummy punched: motion=%s (on_ground=%s)" % [_motion, _on_ground])