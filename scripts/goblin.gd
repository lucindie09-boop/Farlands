extends Node3D
## A spawned goblin: six hearts, the player's own movement physics, and something to do
## with itself.
##
## MOVEMENT is not written here. `ChunkManager.make_mob_body()` hands over a `PlayerSim`
## -- the very simulation `PlayerController` walks the player with: the same 20 Hz ticks,
## gravity, ground/air acceleration, per-block slipperiness, step-up, the jump arc, the
## water swim, the render interpolation -- and this script only supplies the input a
## route would: a wish direction and, when the way ahead is a step, the jump control.
## A second mover written for mobs would be a second set of those numbers to keep in
## step with the player's, and they would drift.
##
## Six hearts, counted in half-hearts like the player's own health (12 = 6 x 2), and
## a punch takes one half-heart, vanilla's empty fist, so the twelfth hit is the last.
## The knockback and the damage share ONE gate, and it is the sim's own hurt window:
## `shove()` answers false while it resists, and a resisted hit neither moves the body
## nor takes health -- hold-clicking cannot empty it faster than the window allows.
##
## Nothing is drawn over its head. The hearts are the goblin's own state, not a bar
## hovering in the world: a body standing behind a hill is a body behind a hill, with
## no red dots over the terrain and nothing to depth-test against the blocks around it.
##
## A goblin with nothing to do wanders, which is four parts:
##   - it stands for a few seconds, then looks for somewhere to be;
##   - the somewhere is a random column a few blocks away -- a place rather than a
##     heading, so "walk that way" cannot put it inside a wall;
##   - the route to it is the engine's own ground planner's, on a worker thread, and
##     a plan whose budget ran out is walked as far as it goes;
##   - it walks that route by wishing along it, which is the player's physics at a
##     stroll's pace, and hops a rise the way the player would: see the step, press
##     jump.
## Arriving, or giving up, means standing still again until the next look. A shove
## interrupts all of it -- being knocked back is not something it chose, so the route
## is dropped instead of being steered over a tick later.

const NAV_ROUTES := preload("res://scripts/nav_routes.gd")

## The sim's own body box, for the step probe: the same one the player moves in.
const BODY := Vector3(0.6, 1.8, 0.6)

## 6 hearts, in the half-hearts `PlayerController.get_health()` also counts in.
const MAX_HEALTH := 12.0
## Vanilla's empty fist: one half-heart per hit.
const PUNCH_DAMAGE := 1.0

## An idle goblin stands still most of the time, and it is not on a timer: a stroll is a
## CHANCE, 1 in 120 a tick, which is how the game's own mobs wander. That is a stroll
## every 6 s on average, but the waits are geometric, not fixed -- half of them are over
## 4 s and a fifth over 12 -- so what it reads as is an animal standing about and
## occasionally going somewhere, rather than one pacing on a beat.
const STROLL_CHANCE := 1.0 / 120.0
## A hold, in ticks, that is NOT the game's own: it keeps a goblin that has just failed to
## find a route from rolling again on the very next tick. It expires into the chance
## above, so a hold of 0 simply means "may roll at once".
const IDLE_HOLD_MIN := 20
const IDLE_HOLD_MAX := 100
## How far away the place it picks may be, in blocks. The near bound keeps it from
## choosing the cell it is already standing on every time, and the far bound is what sets
## how much of its life it spends walking: a stroll of 2 to 6 blocks at the wander's own
## slow pace is 1 to 4 s against the 6 s a roll waits on average, so it stands most of the
## time -- a longer stroll at this pace read as a goblin that never stopped.
const STROLL_MIN_RADIUS := 2.0
const STROLL_RADIUS := 6.0
## How far ABOVE its feet the target is aimed, in blocks. The planner resolves a column
## by scanning DOWN from a little above the height it is given, so a target aimed at the
## goblin's own feet resolves to the inside of the hill the moment the column is
## up-slope -- a surface with no room for a body above it -- and the walk is to a place
## it cannot stand. Aimed at the top of this window, every column resolves to whatever
## ground is actually there, uphill or down.
const STROLL_UP := 4.0
## How fast it moves, in Minecraft's own unit: 0.30, a spider's (the player's walk is
## 0.1, a zombie's 0.23, a villager's 0.5). The value is the ACCELERATION a body adds
## each tick, not blocks a second, and a mob adds 0.2161 of it against ground friction
## 0.546: its terminal speed is value * 0.2161 / (1 - 0.546) * 20 = value * 9.52
## blocks/s. So 0.30 is 2.86 blocks/s -- 0.66 of the player's own 4.317-block walk,
## which is a spider's hunting pace -- and the other mobs land where they should:
## a zombie at 2.19, a villager at 4.76. (The player's 4.317 is NOT 0.1 * 9.52: the
## player adds 0.098 a tick by a different route, which is why its value cannot be read
## with the mob's factor.)
const MOVEMENT_SPEED := 0.30
## A mob's value in blocks per second -- see MOVEMENT_SPEED for the arithmetic.
const BLOCKS_PER_SECOND := 9.52
## The player's own walking pace in blocks a second, which is what the sim's move
## multiplier of 1.0 means.
const PLAYER_WALK_SPEED := 4.317
## What the goblin is doing to its speed, as a share of MOVEMENT_SPEED. Only `idle`
## drives anything today -- it wanders and has no chase, no loot run, no fleeing and no
## sneak approach yet -- but every state is here, one assignment to `speed_state`
## away from being used.
const SPEED_STATES := {
	"idle": 0.6,      # slow, shuffling patrol
	"chasing": 1.0,   # the full 0.30
	"loot": 1.1,      # eager, a little faster than chasing
	"fleeing": 1.3,   # panicked sprint
	"sneaking": 0.7,  # creeping closer
}
## Half a pace ahead, at feet height: where a step it has to climb shows up before the
## body reaches it. The sim steps up 0.6 by itself, so this only fires on a full block.
const STEP_PROBE := 0.45
## A waypoint counts as reached this close to its cell's middle, horizontally.
const ARRIVE_DISTANCE := 0.45
## A stroll target is a few blocks away, so the search should give up quickly rather
## than sweep the country: the expansion cap is a few blocks' worth, and the wall clock
## is a ceiling for a contended machine.
const STROLL_EXPANSIONS := 4000
const STROLL_MAX_MS := 8.0
## Pushing at one waypoint without getting closer for this long drops the route: a rise
## it cannot climb, a gap it cannot cross, a target the world has since changed under.
const STUCK_TICKS := 50
## A plan that came back with nowhere to walk is not worth the whole idle wait: a column
## can be picked inside a cliff or across water and be unreachable, and trying again
## soon after is what keeps a goblin from looking frozen on a bad roll.
const FAILED_RETRY_TICKS := 20
## After a hit it does not set off again until the shove has had time to land.
const HIT_PAUSE_TICKS := 40
## The sim's own tick rate. The facing ease is stated per tick -- that is the clock the
## behaviour moves on -- and spreading one over a frame means scaling by this.
const TICK_RATE := 20.0
## Facing eases this much per tick toward the yaw it wants (a quarter turn in about
## half a second). Only walking sets that yaw: an idle goblin turns its HEAD, not its
## body (see LOOK_CHANCE).
const TURN_EASE := 0.18
## What a standing goblin looks at, and how often: another roll, 1 in 40 a tick, which is
## a glance about every 2 s, to a point within LOOK_RANGE of it and mostly BELOW its eyes
## (LOOK_DOWN_LO..LOOK_UP_HI), which is the tilt that reads as an animal scanning the
## floor. It is a GLANCE: held for LOOK_HOLD ticks and then the head eases back to
## straight ahead, and the roll starts over -- so the head's resting pose, by far most of
## the time, is looking where the goblin is going.
const LOOK_CHANCE := 1.0 / 40.0
const LOOK_RANGE := 6.0
## How far the head may sit off the body before the BODY is dragged along: 35 degrees, the
## player's own limit for its body yaw (kBodyMaxYaw in player_controller_internal.hpp), so a
## goblin turns the way the player's body does -- the head leads within the neck's reach and
## the body comes with it, and the neck never ends up pinned at its 75-degree limit staring
## through its own arm.
const BODY_MAX_YAW := 0.6109
const LOOK_DOWN_LO := -1.4
const LOOK_UP_HI := 0.6
const LOOK_HOLD_MIN := 100
const LOOK_HOLD_MAX := 120
## A player in reach is worth looking at, and that is its own thing rather than a kind of
## glance: the head follows their eyes for a moment and then lets go. It beats the walk's own
## forward look -- watching the player go past is the whole point of it -- and it goes through
## the same neck: the head turns at 10 degrees a tick and the body follows what the neck cannot
## reach.
##
## The goal's own numbers are a 2%-a-tick roll, 8 blocks, and a hold of 40 to 79 ticks (2 to
## 4 s), and with them a mob watches you more than half the time. In the game that reads as a
## mob noticing you because there are dozens of them about and you never stand still beside
## one; with one goblin and the player in front of it, it reads as being stared at. So all
## three are deliberately below the source: half a percent a tick (a look every 10 s on
## average), 4 blocks, and 1 to 2 s of it. The glance rolls its 2% as the game's does, and the
## two still COMPETE for the head -- neither can start while the other holds it -- which is
## what puts real gaps between one watch and the next.
const LOOK_AT_CHANCE := 0.005
const LOOK_AT_RANGE := 4.0
const LOOK_AT_MIN_TICKS := 20
const LOOK_AT_MAX_TICKS := 40
## Where a player's eyes are above their feet, so a mob looks at the face and not the shoes.
const PLAYER_EYE_HEIGHT := 1.62
## While it WALKS the head is tilted down by this much: 22.5 degrees. It is stated as the
## angle and turned into a point (WALK_LOOK_DISTANCE blocks ahead, that far below the
## horizontal), so the tilt is what is being set and the distance cannot change it.
const WALK_LOOK_PITCH := 0.3927
## A walking head drifts slowly side to side on top of that tilt -- a few degrees either
## way over WALK_SWAY_TICKS, so about one slow swing every three and a half seconds. A head
## that holds one line dead still for the whole of a walk reads as a camera on rails; this
## is the whole aim drifting, not a nod, so the downward look stays put.
const WALK_SWAY_YAW := 0.0698
const WALK_SWAY_TICKS := 70.0
const WALK_LOOK_DISTANCE := 2.0
## How high above its feet the point it looks FROM sits, in blocks: the neck.
const WALK_LOOK_HEIGHT := 1.0

## Which SPEED_STATES entry is in force. A chase, a loot run, a flee or a sneak
## approach sets this and nothing else: the pace, the stride and the animation all
## follow from it.
var speed_state := "idle"

var _health := MAX_HEALTH

# The movement sim, and the node it is drawn at: the sim owns the position.
var _sim: RefCounted = null
var _chunk_manager: Node = null
var _last_ticks := 0

# The body's position at the last two PHYSICS frames, so a rendered frame can be
# placed between them: see `_process`.
var _drawn_prev := Vector3.ZERO
var _drawn_cur := Vector3.ZERO

# This frame's input to the sim.
var _wish := Vector3.ZERO
var _jump := false

# Wander state. `_job` is the plan in flight (0 when none), `_route` the support
# cells it came back with, `_next` the one being walked toward. `_idle_ticks` is the
# hold described at IDLE_HOLD_MIN; a probe can pin it high to keep the goblin standing.
var _idle_ticks := 0
var _job := 0
var _route: Array[Vector3] = []
var _next := 0
var _closest := INF
var _stuck_ticks := 0
var _hit_pause := 0

# The facing is the MODEL's yaw, never the body's: the body's box is what the punch is
# tested against, and a turned box would no longer be the box the click was aimed at.
var _model: Node3D = null
var _yaw := 0.0
var _yaw_target := 0.0
var _yaw_ready := false
## Ticks left holding the point it is looking at, that point (world space, for the head to
## turn to -- the model's own `look_at_point`), and whether it is looking at one at all:
## not looking is the head's rest, straight ahead.
var _look_ticks := 0
var _look_point := Vector3.ZERO
var _looking := false
## Watching a player: ticks of the hold left, and whether it is watching one at all.
var _gazing := false
var _gaze_ticks := 0
## Where the walk's slow side-to-side drift is in its swing (radians, one turn per cycle).
var _sway := 0.0


func _ready() -> void:
	_chunk_manager = get_node_or_null("/root/Main/ChunkManager")
	if _chunk_manager != null and _chunk_manager.has_method("make_mob_body"):
		_sim = _chunk_manager.make_mob_body()
		# Where the spawn tool put it is where the body starts: from here the sim owns
		# the position and the node is only where it is drawn.
		_sim.reset(global_position)
		_last_ticks = int(_sim.get_tick_count())
		_drawn_prev = global_position
		_drawn_cur = global_position
	_idle_ticks = randi_range(IDLE_HOLD_MIN, IDLE_HOLD_MAX)


func _physics_process(delta: float) -> void:
	if _sim == null:
		return
	# Timers move in the sim's own 20 Hz ticks, not in frames: a frame is whatever the
	# machine renders at, and 50 ticks of "stuck" has to mean the same second wherever.
	var now := int(_sim.get_tick_count())
	var ticks := now - _last_ticks
	_last_ticks = now
	if ticks > 0:
		_advance_ticks(ticks)
	_steer()
	_sim.step(delta, _wish, _jump, false, false, _yaw, get_speed_multiplier())
	_jump = false
	# The sim's own interpolated position, sampled once per physics frame; `_process`
	# draws the rendered frame between this one and the last.
	_drawn_prev = _drawn_cur
	_drawn_cur = _sim.get_position()
	global_position = _drawn_cur
	_turn(delta)
	# Its health is the fist's, not the fall's: the sim's queued hits are dropped rather
	# than accumulating. Applied would be a behaviour change, not a movement one.
	_sim.drain_damage()


## Drawing, which is not the clock the body moves on. Physics runs 60 times a second
## and a frame is drawn far more often than that, so a body placed only in
## `_physics_process` holds still for several frames and then jumps -- which is exactly
## what "no interpolation" looks like on something standing in a still world. Placing it
## between the last two physics samples is the engine's own rule for a hand-interpolated
## body, and it costs one physics frame of delay (invisible on a goblin, and the same
## tick's delay the sim's own render position already has).
func _process(_delta: float) -> void:
	if _sim == null:
		return
	global_position = _drawn_prev.lerp(_drawn_cur, Engine.get_physics_interpolation_fraction())


## The tick-rate half: the idle clock, the look-around and the stuck clock. Everything
## here is in ticks so it reads the same at any frame rate (the facing ease is not: a
## turn is drawn every frame, see `_turn`).
func _advance_ticks(ticks: int) -> void:
	if _hit_pause > 0:
		_hit_pause = maxi(_hit_pause - ticks, 0)
	_sway = fmod(_sway + TAU * float(ticks) / WALK_SWAY_TICKS, TAU)
	# A glance is held whether it is walking or not, so its clock runs before either roll: what
	# is left of it is what decides whether the watch may take the head.
	_look_ticks = maxi(_look_ticks - ticks, 0)
	_update_gaze(ticks)
	if _route.is_empty():
		if _idle_ticks > 0:
			_idle_ticks = maxi(_idle_ticks - ticks, 0)
		# Both rolls are per TICK, so they are stated per tick and scaled by the ticks this
		# call covers -- a frame rate can never make a goblin wander more.
		# The body follows the head on the player's own rule (BODY_MAX_YAW): what it wants to
		# face is dragged round to keep the look inside that much of the body, and `_turn`
		# eases it there -- so a glance behind turns the goblin instead of folding its neck.
		if _looking:
			var diff := wrapf(_look_yaw() - _yaw, -PI, PI)
			if absf(diff) > BODY_MAX_YAW:
				_yaw_target = wrapf(_look_yaw() - signf(diff) * BODY_MAX_YAW, -PI, PI)
		# The glance and the watch take turns at the head rather than one outranking the other:
		# while it is watching a player no glance starts, and while a glance is held no watch
		# starts (see `_update_gaze`).
		if _look_ticks == 0 and not _gazing:
			# A glance is a glance: its hold running out is the head going back to straight
			# ahead, and only then does the roll get another chance at one.
			_clear_look()
			if randf() < LOOK_CHANCE * ticks:
				_look_ticks = randi_range(LOOK_HOLD_MIN, LOOK_HOLD_MAX)
				_pick_look()
		if _idle_ticks == 0 and _job == 0 and _hit_pause == 0 \
				and bool(_sim.is_on_ground()) and randf() < STROLL_CHANCE * ticks:
			_stroll()
	else:
		var gap := _flat(_route[_next], _sim.get_tick_position()).length()
		if gap < _closest - 0.02:
			_closest = gap
			_stuck_ticks = 0
		else:
			_stuck_ticks += ticks
			if _stuck_ticks > STUCK_TICKS:
				_stop_walking()
				return


## Claim a finished plan, walk it if there is one, otherwise stand and wait.
func _steer() -> void:
	_wish = Vector3.ZERO
	if _job != 0:
		NAV_ROUTES.poll(_chunk_manager)
		var result := NAV_ROUTES.take(_job)
		if not result.is_empty():
			_take_route(result)
	if not _route.is_empty():
		_walk()
		return
	# Nothing to do. Whether it WANTS somewhere to be is the roll in `_advance_ticks`'
	# business, on the tick clock and as a chance -- standing here is the answer most
	# frames, which is most of what an idle mob does.


## Somewhere to be: a random column a few blocks off, handed to the planner as a point
## rather than as a direction, because the planner is what knows where the ground is.
## The target is aimed STROLL_UP above the goblin, not at its feet -- see that constant.
func _stroll() -> int:
	var angle := randf() * TAU
	var distance := randf_range(STROLL_MIN_RADIUS, STROLL_RADIUS)
	return walk_to(_sim.get_tick_position()
		+ Vector3(cos(angle) * distance, STROLL_UP, sin(angle) * distance))


## Set off on a stroll now, without waiting for the roll: a probe drives the same walk
## this way, and a later "go there" has one entry point rather than two.
## Returns the plan's job id, or 0 when nothing was asked.
func stroll_now() -> int:
	if _job != 0 or _sim == null or not bool(_sim.is_on_ground()) or _hit_pause > 0:
		return 0
	return _stroll()


## A point to look at, within LOOK_RANGE of its own feet and around its own height. The
## game picks a random spot around a standing mob the same way, and it is the HEAD that
## turns to it: the body never turns on the spot, it only faces where it walks.
func _pick_look() -> void:
	var angle := randf() * TAU
	var distance := randf_range(1.5, LOOK_RANGE)
	# Mostly at the ground in front of it, sometimes level, rarely up: that spread is what
	# gives the 20-odd-degree downward glance when the roll lands on a near spot, without
	# the head ever being parked looking down.
	_set_look(_sim.get_tick_position() + Vector3(cos(angle) * distance,
		randf_range(LOOK_DOWN_LO, LOOK_UP_HI), sin(angle) * distance))


## A player in reach, watched the way the game's own look-at-player goal does it: the roll
## starts it, the hold runs it out, and either walking out of range or the hold ending stops
## it. Nothing about it is a check for sight -- the goal's own test is distance, and a goblin
## that cannot see through a wall still turns its head to it, which is what the game's mobs do.
func _update_gaze(ticks: int) -> void:
	var player := _player()
	if player == null:
		_gazing = false
		return
	var eyes: Vector3 = player.global_position + Vector3(0.0, PLAYER_EYE_HEIGHT, 0.0)
	var feet: Vector3 = _sim.get_tick_position()
	if _gazing:
		_gaze_ticks -= ticks
		if _gaze_ticks > 0 and eyes.distance_to(feet) <= LOOK_AT_RANGE:
			_set_look(eyes)
			return
		_gazing = false
		return
	# The head has to be free: a glance being held is the other goal winning this one's flag,
	# and the game's watch goal cannot start until it lets go.
	if _look_ticks <= 0 and eyes.distance_to(feet) <= LOOK_AT_RANGE \
			and randf() < LOOK_AT_CHANCE * ticks:
		_gazing = true
		_gaze_ticks = randi_range(LOOK_AT_MIN_TICKS, LOOK_AT_MAX_TICKS)
		_set_look(eyes)


## The player, if this world has one (a preview or a probe may not).
func _player() -> Node3D:
	var scene := get_tree().current_scene
	if scene == null:
		return null
	return scene.get_node_or_null("Player") as Node3D


## The yaw of the point it is looking at, which is the direction its head wants to face.
func _look_yaw() -> float:
	var to: Vector3 = _look_point - _sim.get_tick_position()
	return atan2(-to.x, -to.z)


## Point the head at a world point (and remember that it is looking at one).
func _set_look(point: Vector3) -> void:
	_look_point = point
	_looking = true
	if _model == null:
		_model = get_node_or_null(NodePath("Model")) as Node3D
	if _model != null and _model.has_method("look_at_point"):
		_model.call("look_at_point", _look_point)


## Straight ahead again: the head eases there, so this only has to stop asking for a point.
func _clear_look() -> void:
	if not _looking:
		return
	_looking = false
	if _model != null and _model.has_method("clear_look"):
		_model.call("clear_look")


## Ask the planner for a route to `target` and walk it when it arrives. Public so a
## probe (or a later "go there" call) drives the same path the idle timer does.
## Returns the job id, or 0 when nothing was asked.
func walk_to(target: Vector3) -> int:
	if _chunk_manager == null or _sim == null or _job != 0:
		return 0
	if not _chunk_manager.has_method("request_path"):
		return 0
	# The planner re-anchors both ends onto their column's surface, so a target is a
	# column to stand in; its height is a hint, not a demand.
	_job = int(_chunk_manager.request_path(_sim.get_tick_position(), target,
		STROLL_EXPANSIONS, STROLL_MAX_MS))
	if _job == 0:
		# Nothing was asked (the planner is busy or refused): hold off a moment rather than
		# rolling again on the next tick.
		_idle_ticks = FAILED_RETRY_TICKS
	return _job


## The plan is back. What is walked is `waypoints`, the string-pulled route: those are
## the cells a walker would actually cross, rather than every cell of the grid search.
## A route the budget cut short is still worth walking as far as it goes, so whether
## the planner called it `found` is not consulted.
func _take_route(result: Dictionary) -> void:
	_job = 0
	_route.clear()
	_next = 0
	var cells: PackedVector3Array = result.get("waypoints", PackedVector3Array())
	for cell in cells:
		_route.append(cell)
	# A route that goes nowhere is not a stroll: look again soon rather than standing
	# for the whole idle wait on a column that turned out to be unreachable.
	if _route.size() < 2:
		_route.clear()
		_idle_ticks = FAILED_RETRY_TICKS
		return
	_closest = INF
	_stuck_ticks = 0


## This frame's wish, from the next waypoint the route has not been crossed at.
func _walk() -> void:
	var feet: Vector3 = _sim.get_tick_position()
	var was := _next
	while _next < _route.size() and _flat(_route[_next], feet).length() < ARRIVE_DISTANCE:
		_next += 1
	if _next != was:
		_closest = INF
		_stuck_ticks = 0
	if _next >= _route.size():
		_stop_walking()
		return
	var to := _flat(_route[_next], feet)
	var dir := to.normalized()
	_wish = dir
	_yaw_target = atan2(-dir.x, -dir.z)
	# Walking, it looks where it is going: the look-around is for standing, and a head
	# still turned at whatever it was looking at half a minute ago reads as broken. A
	# player it is already watching keeps the head, though.
	if not _gazing:
		_pick_walk_look(dir)
	# The sim climbs a step of up to 0.6 by itself; a full block it does not, so the step
	# is seen half a pace ahead and the jump control is pressed for this frame. The answer
	# is the world's own collision, not a guess at block ids, so a slab and a stair count
	# exactly as they collide.
	if bool(_sim.is_on_ground()) and _blocked(dir, feet):
		_jump = true


## The head follows the walk: a couple of blocks along the direction of travel, and
## WALK_LOOK_PITCH below the horizontal, so the body's turn and the head's agree while it is
## going somewhere and the walk wears the downward look.
func _pick_walk_look(dir: Vector3) -> void:
	var flat := Vector3(dir.x, 0.0, dir.z)
	if flat.length_squared() <= 0.0:
		return
	flat = flat.normalized().rotated(Vector3.UP, sin(_sway) * WALK_SWAY_YAW)
	flat *= WALK_LOOK_DISTANCE
	var neck: Vector3 = _sim.get_tick_position() + Vector3(0.0, WALK_LOOK_HEIGHT, 0.0)
	_set_look(neck + flat - Vector3(0.0, flat.length() * tan(WALK_LOOK_PITCH), 0.0))


func _stop_walking() -> void:
	_abandon_route()
	# The stroll ends by asking for no further step; the sim's own friction walks the
	# body to a stop from there. No hold follows it -- the next stroll is the roll's
	# business, which is what makes the standing spells geometric rather than paced.
	_idle_ticks = 0


## Drop the route and any plan in flight, and touch nothing else: this is also what a
## punch calls, and a knockback must not have its shove wiped by housekeeping.
func _abandon_route() -> void:
	_route.clear()
	_next = 0
	_closest = INF
	_stuck_ticks = 0
	_job = 0


## The waypoint's cell middle, relative to the feet. Only x/z matter to the walk: the
## feet follow whatever the ground does under them.
func _flat(cell: Vector3, feet: Vector3) -> Vector3:
	return Vector3(cell.x + 0.5 - feet.x, 0.0, cell.z + 0.5 - feet.z)


## Would half a pace forward be stopped? The same collision the body itself resolves.
func _blocked(dir: Vector3, feet: Vector3) -> bool:
	var res: Dictionary = _chunk_manager.resolve_voxel_collision(
		feet, Vector3(dir.x, 0.0, dir.z) * STEP_PROBE, BODY)
	return bool(res.get("collided_x", false)) or bool(res.get("collided_z", false))


## Standing still it looks about, slowly. There is no head rig here, so the body's own
## picture turning is what "looking" is. The ease is per FRAME, not per tick: TURN_EASE
## is a share of the turn per tick, and a frame carries a share of a tick, so turning
## only when the body ticks would rotate the picture in 20 Hz jumps -- a small object
## turning in visible steps beside a player whose own body eases every frame.
func _turn(delta: float) -> void:
	if not _yaw_ready:
		_model = get_node_or_null(NodePath("Model")) as Node3D
		if _model == null:
			return
		# Whatever the spawn tool turned it to face is where the turning starts.
		_yaw = _model.rotation.y
		_yaw_target = _yaw
		_yaw_ready = true
	var ease := 1.0 - pow(1.0 - TURN_EASE, delta * TICK_RATE)
	_yaw = lerp_angle(_yaw, _yaw_target, clampf(ease, 0.0, 1.0))
	_model.rotation.y = _yaw


## Put the body somewhere else with no velocity (a probe, a future respawn): the same
## reset the player's own node does.
func teleport(position: Vector3) -> void:
	if _sim == null:
		return
	_sim.reset(position)
	# Both drawn samples move with it, or the next frame interpolates from where it
	# used to be and the body slides across the gap.
	_drawn_prev = position
	_drawn_cur = position
	global_position = position
	_abandon_route()


## A punch, from PlayerController. The shove and the damage share the sim's hurt window:
## `shove` answers false while it resists, so a resisted hit is neither knockback nor
## damage -- which is what stops hold-clicking from stacking either.
func apply_knockback(attacker_position: Vector3, extra_velocity := Vector3.ZERO) -> void:
	if _sim == null:
		return
	if not bool(_sim.shove(attacker_position, extra_velocity)):
		return
	# A shove is not a stroll. A plan still in flight is dropped here too (its result
	# is ignored when it lands), and it stands off until the flight is over.
	_abandon_route()
	_hit_pause = HIT_PAUSE_TICKS
	take_damage(PUNCH_DAMAGE)


func take_damage(amount: float) -> void:
	if _health <= 0.0:
		return
	_health = maxf(_health - amount, 0.0)
	print("Goblin hit: %.0f of %.0f half-hearts left" % [_health, MAX_HEALTH])
	if _health <= 0.0:
		die()


## At zero it goes: the body is freed, which the spawn tool notices on its next frame
## (it drops the dead body's light entry with it).
func die() -> void:
	print("Goblin dies")
	queue_free()


func get_health() -> float:
	return _health


func get_max_health() -> float:
	return MAX_HEALTH


## Blocks a second this state asks for.
func get_move_speed() -> float:
	return MOVEMENT_SPEED * BLOCKS_PER_SECOND * float(SPEED_STATES.get(speed_state, 1.0))


## The sim's own parameter for that pace: 1.0 is the player's own walk, so a goblin
## creeps at 0.4 of it while wandering and reaches 0.79 while fleeing. The walk
## animation needs none of this -- it reads the velocity the sim actually moved, so a
## slower goblin takes shorter steps at a slower cadence by itself.
func get_speed_multiplier() -> float:
	return get_move_speed() / PLAYER_WALK_SPEED


## Blocks per tick, from the sim the body actually moved with.
func get_velocity() -> Vector3:
	return _sim.get_velocity() if _sim != null else Vector3.ZERO


func is_on_ground() -> bool:
	return _sim != null and bool(_sim.is_on_ground())
