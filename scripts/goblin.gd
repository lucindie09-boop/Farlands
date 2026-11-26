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
##
## A route's HOPS (the planner's jump across a one-cell gap) are taken too: the next cell
## two columns over is one, and it is a jump off the lip rather than a walk into the gap.
## A hop that climbs is not planned at all -- see `MoveGenerator::hop`, which measured
## that arc against this pace.

const NAV_ROUTES := preload("res://scripts/nav_routes.gd")
## The session's pathfinding trace, one per session and owned by the root -- see `_install_trace`.
const NAV_TRACE := preload("res://scripts/nav_trace.gd")
## The recorder's node name under the root, and the path to it: `name` is a StringName and a
## lookup takes a NodePath, so the two are kept apart rather than assigned across.
const TRACE_NAME := "NavTrace"
const TRACE_NODE := NodePath("/root/NavTrace")

## The sim's own body box, for the step probe: the same one the player moves in.
const BODY := Vector3(0.6, 1.8, 0.6)

## Which entry in `data/mobs.json` this body is: the six numbers below are read from it at
## `_ready` (`_apply_attributes`), so a new mob is an entry there with a model rather than a
## copy of this script.
const MOB_TYPE := "goblin"
## 6 hearts, in the half-hearts `PlayerController.get_health()` also counts in. The DEFAULT: what
## a body runs with is `data/mobs.json` `health`, and this is what a build with no file, or an
## entry that leaves the field out, falls back to.
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
## The DEFAULT: `data/mobs.json` `speed` is what the body actually moves at.
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
## A hit is a reason to fight back. A mob keeps whoever hurt it as a revenge target and walks
## at them, and the chase is the same plan-walking this body already does, at a faster state
## and aimed at a person rather than at a roll's column. Reach is the two bodies' own widths
## plus a hand, which is where the attack goal's test lands -- about 1.4 blocks between
## centres -- and the blow lands once a second for a heart.
const CHASE_REACH := 1.45
const ATTACK_TICKS := 20
## The DEFAULT: `data/mobs.json` `damage` is what a blow costs at runtime.
const MOB_ATTACK_DAMAGE := 2.0
## The chase re-plans on that goal's own interval, 4 to 10 ticks, and never waits on a plan
## already in flight (`walk_to` answers 0 for one).
const CHASE_REPATH_MIN := 4
const CHASE_REPATH_MAX := 11
## Badly hurt, it stops fighting and runs: two hearts of its six, in the half-hearts the health
## is counted in, so four of its twelve. (It was one heart -- two half-hearts -- which left a
## goblin fighting on to the end and then running with a single half-heart to lose.)
const FLEE_HEALTH := 4.0
## It runs until the player is this many blocks off, then lets go and goes back to wandering.
const FLEE_DISTANCE := 16.0
## Each sprint of that run is this far, picked away from the player with a little scatter.
const FLEE_RADIUS := 8.0
## How far BELOW its own feet a run-away looks for the ground it is aiming at, after looking up as
## far as a stroll does. The aim is eight blocks out and the ground there can be a hilltop or a drop
## away from the goblin's own level, so the goal has to be a cell something can actually stand in
## rather than a height.
const FLEE_SCAN_DOWN := 8
## A punch is only taken personally from this far: from further off, the goblin has no one to
## find and goes back to what it was doing.
const AGGRO_RANGE := 16.0
## A goblin hurt by a player wakes its pack: every other goblin within this many blocks of it, on
## every axis, turns on the same attacker. The game's own pigmen do exactly this, on a box
## expanded 32 blocks out from the one that was hit. The one that was hit is not special — it
## simply happened to be nearest.
## The DEFAULT: `data/mobs.json` `alert_box` is the box actually walked.
const ALERT_RADIUS := 32.0
## How long that anger lasts once raised: 400 to 799 ticks, twenty to forty seconds, which is the
## span the same mobs are given. Every new hit resets it.
## The DEFAULT: `data/mobs.json` `anger_window` is the pair actually rolled.
const ANGER_MIN_TICKS := 400
const ANGER_MAX_TICKS := 799
## A chase has a LENGTH. Past this many blocks from the player it forgets the target and goes back
## to wandering, which is the follow range a zombie (and therefore a pigman) carries in the game:
## 35 blocks. Without it a goblin followed one punch across the world.
## The DEFAULT: `data/mobs.json` `follow_range` is the leash actually used.
const CHASE_LEASH := 35.0
## How close a chase walks before it plants itself. The reach is where the BLOW lands; this is
## where the walking stops. They were the same number, so a goblin halted a whole reach away and
## swung from there -- it now closes to about two bodies' half-widths and presses in.
const CHASE_STOP := 0.9
## The chase has a patience. When its plans keep coming back with nowhere to walk -- a player on a
## pillar, a sealed pit, a frontier at the goblin's own feet -- it stands there while it re-asks,
## and standing under its target for the whole anger window is not a chase. Three seconds with
## nothing to walk ends it. A real detour keeps a route under its feet the whole way, so this
## measures having nowhere to go rather than being far away, and a long way round is never cut
## short by it. A plan still in flight is NOT a route: counting only the ticks with nothing walking
## AND nothing planned let a goblin that re-asked every few ticks reset the clock forever, because
## the answer was always on its way.
const CHASE_WAIT_TICKS := 60
## The chase plans FURTHER than a stroll. A stroll goes six blocks; a chase can be across a valley,
## up a stair or round a wall, and the stroll's budget cut the search off and returned a route to
## the nearest frontier -- which is how a goblin walked to the ground directly under a platform and
## stood there, its target thirty blocks overhead and the long way round never searched at all. A
## chase that still runs out of budget asks again rather than settling for the frontier.
const CHASE_EXPANSIONS := 20000
const CHASE_MAX_MS := 24.0
## ...and a truncated answer buys the next ask TWICE the budget. `truncated` is the search being cut
## off -- the wall clock ran out before it had reached the target's level -- so asking the same
## question under the same clock gets the same cut-off answer, forever. That is what kept a goblin
## walking a seven-cell frontier under a player five blocks above it, at 8 ms a time, with the way
## round still unsearched. Every truncated chase answer doubles the clock (and the expansion cap
## with it, since they are one budget) up to CHASE_BUDGET_LIMIT_MS; an answer that comes back whole
## puts it back.
const CHASE_BUDGET_LIMIT_MS := 192.0
## Half a pace ahead, at feet height: where a step it has to climb shows up before the
## body reaches it. The sim steps up 0.6 by itself, so this only fires on a full block.
const STEP_PROBE := 0.45
## How far down a slide may end and still count as ground: the planner's own `max_drop`, so the
## walker and the plan agree about which step downs exist.
const FALL_PROBE := 3.0
## A climb is one block up the PLAN can ask for, not any rise the geometry allows: a wall probed
## from on top of the feet answers "clear" and turned every wall into a step the goblin jumped at
## twenty-one times in a three-cell route (`nav_trace` 2026-11-25 23:43: `jumps` 6 -> 15 -> 21).
const CLIMB_RISE := 0.5
## A route whose next cell is this far above or below its feet is a route through a world
## that changed underneath it -- it fell down a hole, or a stair went in -- and is re-planned.
## It has to leave room for a drop the PLAN itself made: `NavCosts::max_drop` is three blocks,
## so a slack under three reads a legal three-block drop as "the ground moved", re-plans on
## every tick standing at the edge of it, and the goblin never takes the step.
const ROUTE_LEVEL_SLACK := 3.5
## Frames of walking into something that is not a step before it gives up on the route: a
## wall is a reason for a new plan, and hopping at it for seconds is what it used to do.

## How much of the IN THE WAY axis a corner press keeps, against 1.0 of the free one.
##
## The resolver takes the larger horizontal motion first (`collision_resolver.cpp`: "Y first,
## then whichever horizontal axis has the LARGER motion magnitude"), and that decides whether
## a corner can be walked at all. A press whose BLOCKED axis is the larger is tested first --
## while the box is still inside the wall's cell -- so it is clipped, the free axis only moves
## afterwards, and the tick ends with the body over the drop and its press spent. That is the
## 00:47 run: at the lip, aiming at the far cell's centre gives `x` the larger share (the body
## sits at the wall's face and the centre is 0.8 away in `x`, 0.3 in `z`), `x` goes first and
## is clipped, `z` then carries the body past the wall with `x` still at -1.300005 -- and it
## falls to y=247.85, below the far floor, where the floor's own SIDE face blocks the press
## and it slides down into the water. With the free axis the larger, the same tick clears `z`
## and then takes 0.09 of `x`, which is over the far floor before the tick ends: the player's
## own line. So a corner press is never the chord -- the free axis is kept at 1.0 and the
## in-the-way axis at this much of it.
const CORNER_PRESS := 0.6
const REPLAN_AFTER_BLOCKED := 12
## A hop is taken by pressing jump off its LIP, and these two numbers are what "off the lip"
## means, measured on the sim this body walks on at the wandering pace. The arc carries it
## 0.9 blocks while it is airborne and a one-cell gap needs a little over 0.7 of them, so a
## jump pressed within about 0.2 blocks of the edge lands on the far side and one pressed
## half a block out lands in the gap. The window is in TICKS of the body's own travel rather
## than in blocks, because a slower body covers less ground in the same arc. And the arc has
## a floor: below about 0.07 blocks a tick (the pace at a standstill, or just after a turn)
## there is not enough travel to reach the far side at all, and the walk stops at the lip
## rather than stepping into the hole.
const HOP_LEAD_TICKS := 2.5
const HOP_MIN_SPEED := 0.078
## A chase stops at the reach and only walks again once the player is this much further off,
## so it is not re-planning and dropping the route every tick at the boundary.
const CHASE_SLACK := 0.5
## A waypoint counts as reached this close to its cell's middle, horizontally.
const ARRIVE_DISTANCE := 0.45
## ...and only from its own LEVEL, give or take this much. A step is not arrived at from
## underneath: while the body stands at the foot of a climb, the cell it has to step onto is
## horizontally under its feet the whole time, so an arrival test that looks at x/z alone skips
## every climb in a route and then pushes at the level above from below -- a goblin standing at
## the bottom of a staircase, re-planning that staircase, every second. (Route cells are SUPPORT
## cells, so the surface to compare against is the cell's own y + 1.)
const ARRIVE_HEIGHT := 0.35
## A stroll target is a few blocks away, so the search should give up quickly rather
## than sweep the country: the expansion cap is a few blocks' worth, and the wall clock
## is a ceiling for a contended machine.
## A body in water holds its jump and paddles at the nearest dry column it can find. The search
## rings out to this many blocks, and the answer is kept for a second rather than rescanned every
## tick: a ring of 12 is a couple of hundred cells and there is no reason to ask twenty times a
## second. A pond wider than this is a lake, and a goblin floats in it rather than crossing it.
const SHORE_REACH := 12
## Ticks between shore searches: a second, which is long enough for a paddle stroke to have
## changed which column is nearest and short enough that it never swims at a wall for long.
const SHORE_RESCAN_TICKS := 20
const STROLL_EXPANSIONS := 4000
const STROLL_MAX_MS := 8.0
## Pushing at one waypoint without getting closer for this long drops the route: a rise
## it cannot climb, a gap it cannot cross, a target the world has since changed under.
const STUCK_TICKS := 50
## Frames pressed at a lip the swim cannot climb before the route is dropped and asked again. The
## clocks that measure WALKING are zeroed in water (`_swim`), so a route that asks for a climb the
## lift cannot make used to be leaned on forever: nav_trace, 2026-11-25 22:20, `stuck` never
## passing 6 while the body pressed at a staircase foot for twenty seconds. The planner refuses
## that move now; this is the second lock, for a route planned before the water or the ground
## under it moved.
const LIP_GIVE_UP_FRAMES := 90
## A plan that came back with nowhere to walk is not worth the whole idle wait: a column
## can be picked inside a cliff or across water and be unreachable, and trying again
## soon after is what keeps a goblin from looking frozen on a bad roll.
const FAILED_RETRY_TICKS := 20
## The sim's own tick rate. The facing ease is stated per tick -- that is the clock the
## behaviour moves on -- and spreading one over a frame means scaling by this.
const TICK_RATE := 20.0
## Facing eases this much per tick toward the yaw it wants (a quarter turn in about
## half a second). Walking sets that yaw to the travel direction, and a look the neck
## cannot reach drags it round too (BODY_MAX_YAW); nothing else turns the body.
## It is the player's own body-yaw rate (kBodyTurnPerTick), applied per DRAWN frame in
## `_process` with that frame's delta, so the goblin's body turns at the speed and the
## smoothness of the player's: at 0.18 the head was arriving and the body still on its way.
const TURN_EASE := 0.3
## What a standing goblin looks at, and how often: a roll, 1 in 120 a tick, which is a
## glance about every 6 s, to a point within LOOK_RANGE of it and mostly BELOW its eyes
## (LOOK_DOWN_LO..LOOK_UP_HI), which is the tilt that reads as an animal scanning the
## floor. It is a GLANCE: held for LOOK_HOLD ticks and then the head eases back to
## straight ahead, and the roll starts over -- so the head's resting pose, by far most of
## the time, is looking where the goblin is going. The roll is what sets how busy it looks,
## and 1 in 40 (a glance every 2 s, with a 1-2 s hold) is a head that is never still: a
## glance every 6 s against that hold leaves it resting most of the time and moving now and
## then, which is the difference between watching a mob think and watching it twitch.
const LOOK_CHANCE := 1.0 / 120.0
const LOOK_RANGE := 6.0
## How far the head may sit off the body before the BODY is dragged along: 35 degrees, the
## player's own limit for its body yaw (kBodyMaxYaw in player_controller_internal.hpp), so a
## goblin turns the way the player's body does -- the head leads within the neck's reach and
## the body comes with it, and the neck never ends up pinned at its 75-degree limit staring
## through its own arm.
const BODY_MAX_YAW := 0.6109
const LOOK_DOWN_LO := -1.4
const LOOK_UP_HI := 0.6
## How long a glance is held: 20 to 40 ticks, 1 to 2 s, which is the idle look goal's own
## hold in the game. The length of this is the difference between a mob whose head is doing
## something most of the time and one wearing a pose -- and since a held glance is also time
## the player-watch cannot have the head, short glances are what keep it from staring
## without changing any of the watch's own numbers.
const LOOK_HOLD_MIN := 20
const LOOK_HOLD_MAX := 40
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
## three are deliberately below the source: a quarter of a percent a tick (a look every 20 s
## on average), 4 blocks, and 1 to 2 s of it. The glance rolls its 2% as the game's does, and the
## two still COMPETE for the head -- neither can start while the other holds it -- which is
## what puts real gaps between one watch and the next.
const LOOK_AT_CHANCE := 0.0025
const LOOK_AT_RANGE := 4.0
const LOOK_AT_MIN_TICKS := 20
const LOOK_AT_MAX_TICKS := 40
## Where a player's eyes are above their feet, so a mob looks at the face and not the shoes.
const PLAYER_EYE_HEIGHT := 1.62
## While it WALKS the head is tilted down by this much: 22.5 degrees. It is stated as the
## angle and turned into a point (WALK_LOOK_DISTANCE blocks ahead, that far below the
## horizontal), so the tilt is what is being set and the distance cannot change it.
const WALK_LOOK_PITCH := 0.3927
## A walking head drifts side to side on top of that tilt -- 8 degrees either way over
## WALK_SWAY_TICKS, so one swing every 2.2 s. A head that holds one line dead still for the
## whole of a walk reads as a camera on rails; this is the whole aim drifting, not a nod, so
## the downward look stays put. The size is what makes it visible: at 2 blocks ahead 8 degrees
## is about 28 cm of aim, where 4 degrees over a longer swing was a head that never moved.
const WALK_SWAY_YAW := 0.14
const WALK_SWAY_TICKS := 44.0
const WALK_LOOK_DISTANCE := 2.0
## How high above its feet the point it looks FROM sits, in blocks: the neck.
const WALK_LOOK_HEIGHT := 1.0

## Which SPEED_STATES entry is in force. A chase, a loot run, a flee or a sneak
## approach sets this and nothing else: the pace, the stride and the animation all
## follow from it.
var speed_state := "idle"

var _health := MAX_HEALTH

# The body's numbers, which `data/mobs.json` owns. `_apply_attributes` fills every one of these
# from the registry in `_ready`, and the constants of the same names are the fallback -- nothing
# below reads a constant, so tuning a goblin is a diff in the data file and not in this script.
var _max_health := MAX_HEALTH
var _damage := MOB_ATTACK_DAMAGE
var _movement_speed := MOVEMENT_SPEED
var _follow_range := CHASE_LEASH
var _alert_box := ALERT_RADIUS
var _anger_min_ticks := ANGER_MIN_TICKS
var _anger_max_ticks := ANGER_MAX_TICKS

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
## Where the route it is walking was asked for, so a route that stops being walkable can be
## asked for again without losing the destination.
var _goal := Vector3.ZERO
## Where the plan in hand was asked for, so a chase can ask again only when the answer could
## have changed rather than on a clock.
var _planned_goal := Vector3.ZERO
## The cell the current plan was ASKED FROM -- not always the column the body is in: at a lip its
## centre is over the hole and a neighbouring cell carries it (`_standing_cell`), so the plan comes
## back with that cell as its first node. See `_take_route`.
var _planned_from := Vector3.INF
## Set when the route in hand is no longer a route (blocked, or the ground moved under it)
## and a new plan is owed once the last one has landed.
var _needs_replan := false
var _blocked_frames := 0
## Frames pressed at a lip above the water line: see LIP_GIVE_UP_FRAMES.
var _lip_frames := 0
## Ticks spent stroking, this session: the trace's "is it actually swimming" number, which a
## position cannot answer for a body bobbing on the spot.
var _strokes := 0
## Whether it is inside its fighting reach right now, which carries the slack above.
var _in_reach := false
## Whether it is closing the last pace on the player: inside the reach but not yet at CHASE_STOP,
## it walks straight at them instead of standing at the edge of its own arm.
var _closing := false
## Ticks of a chase spent with no route to walk and no plan in flight: what ends a chase whose
## plans keep stopping at the same frontier.
var _chase_wait_ticks := 0
## The recorder this body reports to (`scripts/nav_trace.gd`), looked up on first use, and the
## role the plan in flight was asked for: a route can land after the speed state has moved on.
var _trace_node: Node = null
var _plan_role := "idle"
## The clock the chase is currently asking with -- CHASE_MAX_MS, doubled by every truncated answer
## up to CHASE_BUDGET_LIMIT_MS -- and the budget the goal in hand was asked with, which is what a
## plan that stopped being walkable is asked with again (a chase re-asks with the escalating
## clock instead, so the two cannot disagree about how much the search may spend).
var _chase_ms := CHASE_MAX_MS
var _goal_expansions := STROLL_EXPANSIONS
var _goal_max_ms := STROLL_MAX_MS
## Which of the route tests asked for the plan being re-asked, for the trace, and how many jumps
## this body has pressed: a goblin that re-plans a staircase every second either never presses
## jump, or presses it at something it cannot climb.
var _replan_reason := ""
var _jumps := 0

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
## Who it is fighting or running from (`null` is the ordinary wandering goblin), which of the
## two it is doing, and the two clocks a fight runs on: the blow's cooldown and the next
## re-plan.
var _enemy: Node3D = null
var _fleeing := false
## Ticks of anger left (ANGER_MIN_TICKS..ANGER_MAX_TICKS). A goblin with an enemy is not angry by
## default: a stroll is interrupted only by being hit or by the shout of a goblin that was.
var _anger_ticks := 0
var _attack_ticks := 0
var _repath_ticks := 0
## Where it last decided the dry ground was, and how long that answer stands (see `_swim`).
var _shore_cell := Vector3.ZERO
var _shore_ticks := 0
## Whether the thing it is chasing is standing in liquid (see `_update_enemy`).
var _wet_target := false


func _ready() -> void:
	_apply_attributes()
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
	# A real world is what a trace is for: a stub body (a probe) has no plans to record.
	if _chunk_manager != null and _chunk_manager.has_method("request_path"):
		_install_trace()


## What this body IS, from `data/mobs.json` through the `Mobs` binding: health, damage, speed,
## follow range, alert box and anger window. The constants above are the fallback -- a build with
## no file, or an entry that leaves a field out, keeps them -- so a missing file is a mob with its
## script's own numbers rather than a body with no health. The lookup is guarded on the binding
## existing at all, because a probe can run against a DLL built before this class did.
func _apply_attributes() -> void:
	var attrs: Dictionary = {}
	if ClassDB.class_exists("Mobs") and ClassDB.class_has_method("Mobs", "attributes"):
		attrs = Mobs.attributes(MOB_TYPE)
	_max_health = float(attrs.get("health", MAX_HEALTH))
	_damage = float(attrs.get("damage", MOB_ATTACK_DAMAGE))
	_movement_speed = float(attrs.get("speed", MOVEMENT_SPEED))
	_follow_range = float(attrs.get("follow_range", CHASE_LEASH))
	_alert_box = float(attrs.get("alert_box", ALERT_RADIUS))
	var window: Array = attrs.get("anger_window", [ANGER_MIN_TICKS, ANGER_MAX_TICKS])
	if window.size() == 2:
		_anger_min_ticks = int(window[0])
		_anger_max_ticks = int(window[1])
	if _anger_max_ticks < _anger_min_ticks:
		_anger_max_ticks = _anger_min_ticks
	_health = _max_health


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
	# What the sim queued is a LANDING's now: a fall past `SAFE_FALL_DISTANCE` enters its queue
	# as a Fall source like any other and is resolved against the same hurt window a blow is, so
	# dropping a goblin off a ledge costs it half-hearts. Breath and contact damage ride the same
	# queue, which is why this is the drain rather than a fall-only counter.
	var hurt: float = float(_sim.consume_damage())
	if hurt > 0.0:
		_take_hurt(hurt)


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
	# The facing turns on the drawn frame too, not once per physics step: a body that
	# updates its rotation at 60 Hz while its position interpolates at the display's rate
	# reads as stepping behind its own head. `_turn` takes a delta and is frame-rate
	# correct, so it means the same turn per second here as it would anywhere.
	_turn(_delta)


## The tick-rate half: the idle clock, the look-around and the stuck clock. Everything
## here is in ticks so it reads the same at any frame rate (the facing ease is not: a
## turn is drawn every frame, see `_turn`).
func _advance_ticks(ticks: int) -> void:
	_sway = fmod(_sway + TAU * float(ticks) / WALK_SWAY_TICKS, TAU)
	# A glance is held whether it is walking or not, so its clock runs before either roll: what
	# is left of it is what decides whether the watch may take the head.
	_look_ticks = maxi(_look_ticks - ticks, 0)
	_update_gaze(ticks)
	_update_enemy(ticks)
	_separate_bodies()
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
		# A fight owns the head and the plans, so the glancing and the wandering below are
		# the standing goblin's business only.
		if _enemy == null and _look_ticks == 0 and not _gazing:
			# A glance is a glance: its hold running out is the head going back to straight
			# ahead, and only then does the roll get another chance at one.
			_clear_look()
			if randf() < LOOK_CHANCE * ticks:
				_look_ticks = randi_range(LOOK_HOLD_MIN, LOOK_HOLD_MAX)
				_pick_look()
		if _enemy == null and _idle_ticks == 0 and _job == 0 \
				and bool(_sim.is_on_ground()) and randf() < STROLL_CHANCE * ticks:
			_stroll()
	else:
		var feet: Vector3 = _sim.get_tick_position()
		# Only while STANDING: a body in the air is not at its route's level by definition, and a
		# plan made while falling anchors its start on the ground the fall is headed for -- which
		# read as "the ground moved" and dropped the route every few ticks of the descent.
		if bool(_sim.is_on_ground()) and absf(_route[_next].y - feet.y) > ROUTE_LEVEL_SLACK:
			_replan_reason = "the ground moved (level slip)"
			_needs_replan = true
		var gap := _flat(_route[_next], feet).length()
		if gap < _closest - 0.02:
			_closest = gap
			_stuck_ticks = 0
		else:
			_stuck_ticks += ticks
			if _stuck_ticks > STUCK_TICKS:
				_replan_reason = "not getting closer (stuck)"
				_needs_replan = true
				_stuck_ticks = 0
		if _needs_replan and _job == 0 and _goal != Vector3.ZERO:
			# ...and not on the next frame. A plan this body has just refused comes back identical
			# from the same place, so an immediate re-ask is a loop at the frame rate rather than an
			# answer: six asks in a quarter of a second, the same eleven nodes each time, for eight
			# seconds (`nav_trace` 2026-11-25 23:36). A chase waits its own re-plan clock before
			# asking again, which is the pacing the periodic re-plan already uses. Anything else (a
			# stroll) is unchanged: its goal is not being chased, and its clock is the idle one.
			if _enemy != null and _repath_ticks > 0:
				return
			_needs_replan = false
			_repath_ticks = randi_range(CHASE_REPATH_MIN, CHASE_REPATH_MAX)
			_trace_route_dropped()
			_closest = INF
			_blocked_frames = 0
			# A chase re-asks with the CHASE's budget, which is the one that escalates. Asking here
			# with the budget of the first ask made the escalation dead: a doubled clock never reached
			# the planner on a re-ask, so a chase cut off once was cut off every time -- the frontier
			# under the target, walked again, for the whole anger window. Anything else (a stroll)
			# re-asks with the budget its goal was planned with, which is not a chase's.
			if _enemy != null:
				walk_to(_goal, _chase_expansion_cap(), _chase_ms)
			else:
				walk_to(_goal, _goal_expansions, _goal_max_ms)


## Claim a finished plan, walk it if there is one, otherwise stand and wait.
func _steer() -> void:
	_wish = Vector3.ZERO
	# A finished plan is claimed HERE, before the water branch. The swim is answered first and it
	# returns, so a body in water never polled: `_job` stayed set for the rest of the session and a
	# re-plan waits for `_job == 0`, so nothing was ever asked again either -- while the route it
	# fell into the pond with still pointed four blocks above it and the wish flipped every second
	# (`nav_trace` 2026-11-25 23:30: `job=5`, `route=5/6`, `next=(-1, 248, -2)` for three seconds
	# with the body sitting at 245 in water, under the player).
	if _job != 0:
		NAV_ROUTES.poll(_chunk_manager)
		var result := NAV_ROUTES.take(_job)
		if not result.is_empty():
			_take_route(result)
	# Water next, and before anything the planner has to say about it: a route through it is legal
	# now, but it is walked by the swim and not by `_walk` (a swimmer is never on the ground, so the
	# walk's arrival test cannot advance). `_swim` also carries the two answers no route has -- the
	# stroke itself, and the shore for a body that has no plan.
	if bool(_sim.is_in_water()):
		_swim(_sim.get_tick_position())
		return
	# The last pace before a blow is not a route: the thing it wants is right there, so it walks at
	# it and stops at CHASE_STOP rather than at the edge of its own arm.
	if _closing and _enemy != null and is_instance_valid(_enemy):
		# Away from it when it is a run-away, at it when it is the last pace before a blow: one
		# steer, mirrored. A body with nothing to walk still has to be moving somewhere.
		var feet_now: Vector3 = _sim.get_tick_position()
		var at := _enemy.global_position
		_step_at(feet_now, feet_now * 2.0 - at if _fleeing else at)
		return
	if not _route.is_empty():
		_walk()
		return
	# Nothing to do. Whether it WANTS somewhere to be is the roll in `_advance_ticks`'
	# business, on the tick clock and as a chance -- standing here is the answer most
	# frames, which is most of what an idle mob does.


## Getting through water: the stroke, the heading, and the way back to the ground.
##
## The planner crosses water now (a wet column is a legal step at the `liquid` surcharge), so a
## route through a pond is walked exactly like any other -- with one difference: a swimmer is
## never ON the ground, so `_walk`'s own arrival test (which wants the feet standing on the cell)
## would never advance and the body would push at its first wet cell forever. That is `_swim_along`.
##
## What the planner still cannot do is get a body OUT of water it was dropped into with nowhere
## planned: the goal's ring anchors on dry ground, and a route whose first steps are all wet was
## refused outright until water became walkable. So the body also has its own answer -- it floats
## and paddles at the nearest dry column it can find -- which is the game's own behaviour for a
## mob in water, and it is what a chase in water uses too. The floating is the sim's water physics
## (a body in water is lifted by the jump control and lifted again when it collides while
## swimming, which is what carries it over the lip); this supplies the stroke and the heading.
func _swim(feet: Vector3) -> void:
	# In water the jump control IS the stroke, so one stroke a tick is the paddle.
	_jump = true
	_strokes += 1
	# Every clock that measures WALKING is meaningless in water: the body is not on the ground, so
	# the stuck clock would count every tick and drop a route it never walked.
	_blocked_frames = 0
	_stuck_ticks = 0
	_closest = INF
	# A target standing IN the water is swum at directly: a route to it has no ending worth
	# walking, because the goal's ring anchors on DRY ground and the plan stops on the bank.
	# Same steer as the last pace before a blow.
	if _wet_target and _closing and _enemy != null and is_instance_valid(_enemy):
		_step_at(feet, _enemy.global_position)
		return
	# A route first, and the shore only when there is none: a plan through water is the plan the
	# planner costed, and the ring is the fallback for a body that has no route at all. This is
	# the half that gets a body OUT: a player standing on a staircase above the water is a target
	# no straight line reaches, and swimming at their column instead is a goblin pressing its face
	# into the tree under them for as long as its anger lasts.
	if _swim_along(feet):
		_lip_clock(feet)
		return
	_lip_frames = 0
	# No route at all: a goal it can still swim at (the plan has not come back, or there is none).
	if _closing and _enemy != null and is_instance_valid(_enemy):
		_step_at(feet, _enemy.global_position)
		return
	_shore_ticks -= 1
	if _shore_ticks <= 0 or _shore_cell == Vector3.ZERO:
		_shore_cell = _shore_from(feet)
		_shore_ticks = SHORE_RESCAN_TICKS
	if _shore_cell != Vector3.ZERO:
		var to := Vector3(_shore_cell.x + 0.5 - feet.x, 0.0, _shore_cell.z + 0.5 - feet.z)
		if to.length_squared() < 0.04:
			# At the column it picked and still wet (a rim one block up, whose face is being
			# climbed): keep the stroke, and look again for a nearer one.
			_shore_cell = Vector3.ZERO
			_shore_ticks = 0
		else:
			_wish = to.normalized()
			_yaw_target = atan2(-_wish.x, -_wish.z)
			return
	# Nothing to steer at: a lake wider than SHORE_REACH with no plan is a float in place, which is
	# what the stroke above already does.


## Follows the route while swimming. True when a cell of it is being steered at.
##
## A WET cell is arrived when the body is over it: the planner put every wet node at the water's
## own surface and a swimmer bobs at that surface rather than standing on it, so a level test there
## would stall the route on every trough of the bob.
##
## A DRY cell above the body is not arrived, and that is the difference that matters: it is a lip
## to climb, and skipping it by horizontal distance alone walked a staircase out from under a
## goblin climbing out of a pond -- it consumed the whole route while still three blocks below the
## player and swam at the tree's trunk instead (nav_trace: route=0/5, 0/4, 0/0, once a second).
## Aimed at, the stroke pressed against the lip is what carries it out: the sim lifts a body that
## collides while swimming, which is the same lift that gets it out of the water at all.
func _swim_along(feet: Vector3) -> bool:
	while _next < _route.size() and _flat(_route[_next], feet).length() < ARRIVE_DISTANCE \
			and (_wet_cell(_route[_next]) or feet.y >= _route[_next].y + 1.0 - ARRIVE_HEIGHT):
		_next += 1
	if _next >= _route.size():
		if not _route.is_empty():
			_stop_walking()
		return false
	var dir := _flat(_route[_next], feet)
	if dir.length_squared() < 1e-6:
		return false
	dir = dir.normalized()
	_wish = dir
	_yaw_target = atan2(-dir.x, -dir.z)
	return true


## A route whose next cell is DRY and above the body is a climb, and a climb out of the water is
## the one move the swim's lift makes for it -- so it is also the one place a route can ask for
## something the body cannot do: a lip above the water's own top. `_swim` zeroes every clock that
## measures walking, because a swimmer is never "getting closer" on the ground's terms, which is
## what let a body press at such a lip for twenty seconds (nav_trace, 2026-11-25 22:20). Ninety
## frames at one, then the route is dropped and asked for again.
func _lip_clock(feet: Vector3) -> void:
	if _next >= _route.size():
		_lip_frames = 0
		return
	var cell: Vector3 = _route[_next]
	if _wet_cell(cell) or cell.y + 1.0 <= feet.y + ARRIVE_HEIGHT:
		_lip_frames = 0
		return
	_lip_frames += 1
	if _lip_frames >= LIP_GIVE_UP_FRAMES:
		_lip_frames = 0
		_replan_reason = "pressed at a lip above the water line"
		_needs_replan = true


## Whether a route cell is liquid, which is what "the swimmer is already there" means for it.
func _wet_cell(cell: Vector3) -> bool:
	return bool(_sim.is_liquid_at(floori(cell.x), floori(cell.y), floori(cell.z)))


## The nearest column of dry, standable ground around the body, or ZERO. It rings out from the
## body's own column so a goblin at the edge of a pond swims the short way, and every cell it asks
## about is asked through the sim's own predicates, so the swim and the physics can never
## disagree about what water or a wall is.
func _shore_from(feet: Vector3) -> Vector3:
	var cx := floori(feet.x)
	var cy := floori(feet.y)
	var cz := floori(feet.z)
	# ...and only a shore at or below the water's own TOP. The lift that carries a body over a rim
	# exists only while it is still in the liquid, so a bank above the water line is a wall it
	# presses at, not a shore it reaches -- picking one is the same stuck goblin the planner's own
	# wet-to-dry rule refuses (see `step` in pathfinding/move_generator.hpp).
	var top := _water_top(feet)
	for radius in range(1, SHORE_REACH + 1):
		for dz in range(-radius, radius + 1):
			for dx in range(-radius, radius + 1):
				# The ring, not the square: a column found last round is not looked for again.
				if maxi(absi(dx), absi(dz)) != radius:
					continue
				var stand := _dry_stand(cx + dx, cy, cz + dz, top)
				if stand != Vector3.ZERO:
					return stand
	return Vector3.ZERO


## The top of the liquid the body is floating in, as a feet height: the cells above its own feet
## are scanned up while they are liquid, so a pond's surface is one past its last full cell and a
## body bobbing above it still has a surface to be measured from.
func _water_top(feet: Vector3) -> float:
	var x := floori(feet.x)
	var z := floori(feet.z)
	var y := floori(feet.y)
	var top := feet.y
	while y <= floori(feet.y) + 3 and bool(_sim.is_liquid_at(x, y, z)):
		top = float(y + 1)
		y += 1
	return maxf(top, feet.y)


## The feet cell of a column it could stand on and be dry in, on one of three levels: the body's
## own, one ABOVE (a shore it has to climb over a lip to reach -- the common case, since a pond's
## rim is at the water line) and one below (a beach that slopes into the water). Empty and not
## liquid in the feet cell, not liquid over the head, and solid underneath. A wall is none of
## these: it is solid in the feet cell, which is why the old "not liquid" test had it paddling
## into the side of the pond at the level it was standing on.
##
## `top` is the water's own surface, and a stand above it is refused: that is the one climb the
## swim cannot make.
func _dry_stand(x: int, cy: int, z: int, top: float) -> Vector3:
	for ly in [cy, cy + 1, cy - 1]:
		if float(ly) > top + 0.01:
			continue
		if bool(_sim.is_solid_at(x, ly, z)) or bool(_sim.is_liquid_at(x, ly, z)):
			continue
		if bool(_sim.is_liquid_at(x, ly + 1, z)):
			continue
		if not bool(_sim.is_solid_at(x, ly - 1, z)):
			continue
		return Vector3(x, ly, z)
	return Vector3.ZERO


## The standable cell in one column, scanning from eight above the goblin's feet down to eight
## below them. `_dry_stand` asks the same question in a three-cell window around a hint, which is
## right for a waypoint the body is standing beside and wrong for a run-away aimed eight blocks out:
## the ground there can be a hilltop several blocks above its own level, and an aim at a FIXED
## height over a slope is a cell inside the slope -- no floor at the level asked about and the hill
## over the body's head. Answers ZERO when that column has nothing to stand on in the scan.
func _stand_over(x: int, z: int, feet_y: float) -> Vector3:
	for ly in range(int(feet_y) + int(STROLL_UP) + 4, int(feet_y) - FLEE_SCAN_DOWN, -1):
		if bool(_sim.is_solid_at(x, ly, z)) or bool(_sim.is_liquid_at(x, ly, z)):
			continue
		if bool(_sim.is_liquid_at(x, ly + 1, z)):
			continue
		if not bool(_sim.is_solid_at(x, ly - 1, z)):
			continue
		return Vector3(float(x) + 0.5, float(ly), float(z) + 0.5)
	return Vector3.ZERO


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
	if _job != 0 or _sim == null or not bool(_sim.is_on_ground()):
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
## Bodies are boxes, and two boxes in one place is not something the reference allows either: it
## pushes a pair apart on the axis their centres are furthest apart, by at most 0.05 a tick
## (`Entity::push`), and the world's own collision does the rest. Without this a goblin walked
## through the player and through its own pack -- five of them one silhouette -- and CHASE_STOP
## was the only thing keeping a goblin out of the player's body: a number standing in for a
## missing body rather than a body.
##
## Once a TICK, not once a frame. The push is per tick in the reference, so a frame-rate call on
## a fast machine would squeeze a crowd twice as hard as on a slow one.
##
## A pair is answered by the body with the LOWER instance id and applied to both, so one pair is
## pushed once a tick rather than once from each side -- and the player, who has no pass of his
## own, is pushed by whoever is leaning on him.
func _separate_bodies() -> void:
	if _sim == null or not is_inside_tree() or not ClassDB.class_exists("Bodies"):
		return
	var feet: Vector3 = _sim.get_tick_position()
	for node in get_tree().get_nodes_in_group("goblin"):
		var other := node as Node3D
		if other == null or other == self:
			continue
		if int(other.get_instance_id()) <= int(get_instance_id()):
			continue
		var other_sim: Object = other.get("_sim")
		if other_sim == null:
			continue
		var pair: Dictionary = Bodies.push_pair(feet, BODY, other_sim.get_tick_position(), BODY)
		if pair.is_empty():
			continue
		_sim.push(pair["a"])
		other_sim.push(pair["b"])
	var player := _player()
	if player != null and player.has_method("push"):
		var pair: Dictionary = Bodies.push_pair(feet, BODY, player.global_position, BODY)
		if not pair.is_empty():
			_sim.push(pair["a"])
			player.call("push", pair["b"])


func _player() -> Node3D:
	var scene := get_tree().current_scene
	if scene == null:
		return null
	return scene.get_node_or_null("Player") as Node3D


## The fight, when there is one: chase and swing, or run once it is badly hurt. Everything
## else about it is the machinery this body already has -- the planner walks the chase, the
## look drags the body round, and the pace is the speed table.
func _update_enemy(ticks: int) -> void:
	if _enemy == null:
		return
	var feet: Vector3 = _sim.get_tick_position()
	if not is_instance_valid(_enemy) \
			or (_enemy.has_method("is_dead") and bool(_enemy.call("is_dead"))):
		_give_up()
		return
	var enemy_at := _enemy.global_position
	# Two distances, because they answer two questions. How far away it is to WALK is the flat
	# one, which is what a run-away is measured in; how far away it is as a BODY is the
	# three-dimensional one, which is what a blow is measured in. The reach used to be the flat
	# distance alone, so a player twenty-five blocks above a goblin was still "in reach" and it
	# hit them straight up through the cliff -- a distance is not a distance if one of its axes
	# is dropped.
	var away := Vector2(feet.x - enemy_at.x, feet.z - enemy_at.z).length()
	var gap := feet.distance_to(enemy_at)
	# The anger runs out, and so does the chase: past its own follow range a mob forgets the target.
	# Without either, one punch bought a goblin a walk across the world.
	_anger_ticks = maxi(_anger_ticks - ticks, 0)
	if _anger_ticks == 0 or gap > _follow_range:
		_give_up()
		return
	if _health <= FLEE_HEALTH and not _fleeing:
		_fleeing = true
		_closing = false
		_repath_ticks = 0
		_abandon_route()
	if _fleeing:
		speed_state = "fleeing"
		if away >= FLEE_DISTANCE:
			_give_up()
			return
		_run_away(feet, enemy_at, ticks)
		# ...and it RUNS whatever the planner said. `_run_away` only asks; the answer may be nowhere
		# to go -- an aim with no standable cell, a wall between, a budget out -- and this branch
		# used to return with `_wish` left at zero, so the body stood still and waited for a plan
		# that was never coming while the player beat it to death (the trace above: `wish=(0.00,
		# 0.00)` for the whole of its last four seconds). `_closing` is the direct steer, which for a
		# body that is running points away from what it is running from.
		_closing = _route.is_empty()
		return
	speed_state = "chasing"
	# It watches what it is fighting: the head on the player's eyes, and the body dragged
	# round after it by the same rule every other look uses.
	_set_look(enemy_at + Vector3(0.0, PLAYER_EYE_HEIGHT, 0.0))
	_attack_ticks = maxi(_attack_ticks - ticks, 0)
	# The reach has a slack on the way out: inside it, it stops and swings; it only takes the
	# walk up again once the player is past the reach by CHASE_SLACK, so a boundary it hovers
	# on does not have it dropping and re-planning a route every tick.
	# ...and it has to be able to SEE what it is hitting. A reach without a look is a goblin
	# swinging through a wall: the distance is a distance, and a blow is a blow only if it lands
	# on a face rather than on the stone in front of it.
	_in_reach = gap <= CHASE_REACH + (CHASE_SLACK if _in_reach else 0.0) \
		and _sees(feet, enemy_at)
	if _in_reach:
		# The reach is where it swings, not where it stops walking: inside it the route is dropped
		# and it closes the last pace straight at the player, so a chase ends against the body it
		# wanted instead of a whole reach short of it. Below CHASE_STOP it plants itself.
		_closing = gap > CHASE_STOP
		if not _route.is_empty():
			_stop_walking()
		if _attack_ticks == 0:
			_attack_ticks = ATTACK_TICKS
			_swing(feet)
		return
	# A target standing IN liquid is still the case a route cannot answer: the goal's ring anchors on
	# DRY ground, so a chase after a player in a pond resolves to the bank and the goblin stands at
	# the water's edge while the player wades about in front of it. In liquid, or with the target in
	# liquid, it wades and swims straight at them instead: the one chase that answers with the body
	# rather than a plan, on the same steer as the last pace before a blow.
	_wet_target = bool(_sim.is_liquid_at(floori(enemy_at.x), floori(enemy_at.y + 0.1),
		floori(enemy_at.z)))
	if _wet_target or bool(_sim.is_in_water()):
		_closing = gap > CHASE_STOP
		# A body in the water KEEPS its route: the swim walks it (`_swim_along`). Dropping it here
		# dropped, every tick, the very plan the swim had just asked for -- a goblin paddling at the
		# foot of a staircase out of a stream re-planned that staircase about once a second and
		# abandoned the answer on the next tick (nav_trace: route=0/5, then 0/4, then 0/0, plan
		# after identical plan, all of it three blocks below the player). On DRY land it is still
		# dropped: there `_closing` walks straight at the target, and a route held while it does is
		# a route nothing reads.
		if not bool(_sim.is_in_water()) and not _route.is_empty():
			_stop_walking()
		return
	_closing = false
	# Nothing to walk, with the player out of reach: the plans are coming back with nowhere to go,
	# and without this it stood at that frontier until its anger ran out. A plan in flight counts as
	# having nowhere to go too -- see CHASE_WAIT_TICKS for why asking again does not buy the chase
	# more time.
	if _route.is_empty():
		# ...but a body paddling out of a pond has nowhere to walk for a reason the chase cannot
		# fix, and forgetting the player on the second stroke is what counting that costs.
		if not bool(_sim.is_in_water()):
			_chase_wait_ticks += ticks
		if _chase_wait_ticks > CHASE_WAIT_TICKS:
			_give_up()
			return
	else:
		_chase_wait_ticks = 0
	_repath_ticks -= ticks
	if _repath_ticks > 0:
		return
	# Asked again only when the answer could differ: there is no route, or the player has
	# moved a block since the last ask, which is the attack goal's own rule. Re-planning on a
	# timer instead flips between two near-equal answers -- a three-block drop against a few
	# blocks of walking round -- and that reads as a mob changing its mind.
	if _job == 0 and (_route.is_empty() or _planned_goal.distance_to(enemy_at) >= 1.0):
		_repath_ticks = randi_range(CHASE_REPATH_MIN, CHASE_REPATH_MAX)
		# A chase is planned with the chase's own budget: the stroll's cut the search off and the
		# goblin walked to the nearest place it could reach instead of the far one it needed. That
		# budget grows while the answers keep being cut off -- see CHASE_BUDGET_LIMIT_MS.
		walk_to(enemy_at, _chase_expansion_cap(), _chase_ms)


## How high its eyes are above its feet: the sim knows the body, and a stub may not.
func _eye_height() -> float:
	if _sim != null and _sim.has_method("get_eye_height"):
		return float(_sim.call("get_eye_height"))
	return 1.4


## A wish straight at a point, with no route behind it: the pace before a blow, and the only walking
## a body does without the planner's help.
func _step_at(feet: Vector3, target: Vector3) -> void:
	var to := Vector3(target.x - feet.x, 0.0, target.z - feet.z)
	if to.length_squared() < 1e-6:
		return
	_wish = to.normalized()
	_yaw_target = atan2(-_wish.x, -_wish.z)


## Whether it can see the player from where it stands: the engine's own block ray from its eyes
## to theirs, and nothing solid between. The same ray the player's own picking and punching use,
## so what a goblin calls a clear line is what the world calls one.
func _sees(feet: Vector3, enemy_at: Vector3) -> bool:
	if _chunk_manager == null or not _chunk_manager.has_method("raycast"):
		return true
	var eye := feet + Vector3(0.0, _eye_height(), 0.0)
	var face := enemy_at + Vector3(0.0, PLAYER_EYE_HEIGHT, 0.0)
	var to := face - eye
	var reach := to.length()
	if reach < 0.01:
		return true
	var hit: Dictionary = _chunk_manager.call("raycast", eye, to / reach, reach)
	if not bool(hit.get("success", false)):
		return true
	# A hit at the far end is the face it was aimed at; anything earlier is a wall.
	var point: Vector3 = hit.get("hit_point", eye)
	return eye.distance_to(point) >= reach - 0.1


## One blow. Through the player's own hit entry when the build has one -- that is the version
## that brings the hurt window, the lean and the push away from the goblin with it -- and by
## the health it costs otherwise, so a body from before that entry existed still takes the
## damage instead of the goblin standing there swinging at nothing.
func _swing(feet: Vector3) -> void:
	if _enemy == null:
		return
	if _enemy.has_method("take_mob_hit"):
		_enemy.call("take_mob_hit", _damage, feet)
	elif _enemy.has_method("get_health") and _enemy.has_method("set_health"):
		var left := float(_enemy.call("get_health")) - _damage
		_enemy.call("set_health", int(round(left)))


## A sprint of the run-away: away from the player, with a little scatter, a few blocks at a
## time. Re-planned on the same interval as the chase, so a player following it is answered
## rather than run from in one stale direction.
func _run_away(feet: Vector3, enemy_at: Vector3, ticks: int) -> void:
	_repath_ticks -= ticks
	if _repath_ticks > 0:
		return
	# Same rule as the chase: a sprint of the run-away is kept until it has been run or the
	# player has moved a block, rather than re-picked every few ticks into a new direction.
	if _job != 0 or (not _route.is_empty() and _planned_goal.distance_to(enemy_at) < 1.0):
		return
	_repath_ticks = randi_range(CHASE_REPATH_MIN, CHASE_REPATH_MAX)
	var away := feet - enemy_at
	away.y = 0.0
	if away.length_squared() < 0.01:
		away = Vector3(randf_range(-1.0, 1.0), 0.0, randf_range(-1.0, 1.0))
	away = away.normalized().rotated(Vector3.UP, randf_range(-0.45, 0.45))
	# Aimed at the GROUND of the column eight blocks off, not at a height over it. A fixed height
	# above the feet is only a hint, and a hint that lands inside rising ground is a cell in the
	# hill: the planner anchors a goal on the level it is given and its one-column ring wants
	# exactly that level, so every ask aimed into a slope came back "goal column has no navigable
	# surface" -- thirteen of them in four seconds, with the goblin standing still from 4 health to
	# zero while the player hit it (`nav_trace`, 2026-11-25 22:58). The column's own standable cell
	# is a goal the planner can anchor: dry, clear over the body, and at the level the ground there
	# actually holds, uphill or down.
	var aim := feet + away * FLEE_RADIUS + Vector3.UP * STROLL_UP
	var stand := _stand_over(floori(aim.x), floori(aim.z), feet.y)
	if stand != Vector3.ZERO:
		aim = stand
	walk_to(aim, CHASE_EXPANSIONS, CHASE_MAX_MS)


## Off the fight: back to wandering, at the wandering pace.
func _give_up() -> void:
	_trace_say("chase ends (anger=%d wait=%d/%d route=%d job=%d)"
		% [_anger_ticks, _chase_wait_ticks, CHASE_WAIT_TICKS, _route.size(), _job])
	_chase_ms = CHASE_MAX_MS
	_enemy = null
	_fleeing = false
	_closing = false
	_wet_target = false
	_chase_wait_ticks = 0
	_anger_ticks = 0
	_in_reach = false
	_attack_ticks = 0
	_repath_ticks = 0
	_needs_replan = false
	_blocked_frames = 0
	_goal = Vector3.ZERO
	_planned_goal = Vector3.ZERO
	_planned_from = Vector3.INF
	speed_state = "idle"
	_abandon_route()


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
func walk_to(target: Vector3, expansions := STROLL_EXPANSIONS, max_ms := STROLL_MAX_MS) -> int:
	# The destination is remembered even when the ask is refused, so a route that has to be
	# re-planned later knows where it was going.
	_goal = target
	if _chunk_manager == null or _sim == null or _job != 0:
		return 0
	if not _chunk_manager.has_method("request_path"):
		return 0
	# The planner re-anchors both ends onto their column's surface, so a target is a
	# column to stand in; its height is a hint, not a demand -- WHICH IS WHY THE START HAS TO BE
	# THE CELL THE BODY STANDS ON. At the edge of a lip the body's centre is over the hole and it is
	# carried by the cells its box overlaps; asking from that hole column, the planner anchors the
	# start on the pit floor three blocks below and hands back a route that begins underground: the
	# walk sees `the ground moved (level slip)` and drops it, or worse walks it off the ledge
	# (`nav_trace` 2026-11-25 23:49: `path=(-2, 245, -2)>...` from a body at 249, then `ground=false`,
	# `vy=-0.52`).
	var from: Vector3 = _standing_cell(_sim.get_tick_position())
	_planned_from = from
	_job = int(_chunk_manager.request_path(from, target, expansions, max_ms))
	_planned_goal = target
	_goal_expansions = expansions
	_goal_max_ms = max_ms
	_plan_role = speed_state
	_trace_plan_asked(_job, from, target, expansions, max_ms)
	if _job == 0:
		# Nothing was asked (the planner is busy or refused): hold off a moment rather than
		# rolling again on the next tick.
		_idle_ticks = FAILED_RETRY_TICKS
	return _job


## The cell this body is STANDING ON rather than merely inside: its own when that column has a
## floor at its feet, and otherwise the nearest neighbour whose floor is under the box -- which is
## the cell carrying it. A body at the edge of a lip is the second case, and it is the one that
## matters: a plan asked from the hole column starts on the pit floor.
func _standing_cell(feet: Vector3) -> Vector3:
	var own := Vector3(floorf(feet.x), feet.y, floorf(feet.z))
	if _has_floor(own, feet):
		return own
	var best := own
	var best_distance := INF
	for dx in [-1, 0, 1]:
		for dz in [-1, 0, 1]:
			if dx == 0 and dz == 0:
				continue
			var cell := own + Vector3(dx, 0.0, dz)
			if not _has_floor(cell, feet):
				continue
			var distance := Vector2(cell.x + 0.5 - feet.x, cell.z + 0.5 - feet.z).length()
			if distance < best_distance:
				best_distance = distance
				best = cell
	return best


## A floor at the feet's own level under this cell: the block the body would be standing on.
func _has_floor(cell: Vector3, feet: Vector3) -> bool:
	if _sim == null or not _sim.has_method("is_solid_at"):
		return true
	return bool(_sim.call("is_solid_at", int(cell.x), floori(feet.y) - 1, int(cell.z)))


## The plan is back. What is walked is `waypoints`, the string-pulled route: those are
## the cells a walker would actually cross, rather than every cell of the grid search.
## A route the budget cut short is still worth walking as far as it goes, so whether
## the planner called it `found` is not consulted.
func _take_route(result: Dictionary) -> void:
	_trace_plan_answered(_job, result)
	# A cut-off answer is a reason to ask again with more to spend, and a whole one to stop paying
	# for it: this is the chase's escalation, in the one place every answer passes through.
	if _enemy != null:
		if bool(result.get("truncated", false)):
			_chase_ms = minf(_chase_ms * 2.0, CHASE_BUDGET_LIMIT_MS)
		else:
			_chase_ms = CHASE_MAX_MS
	_job = 0
	_route.clear()
	_next = 0
	# The planner's own cells, not the smoothed waypoints: the smoother pulls a route into
	# long straight legs, and a leg is walked as a straight line -- which is how a goblin
	# stepped off ledges and waded through what the plan had gone round. Cell by cell is what
	# a mob walks, and every cell in this list was reached by a legal move.
	var cells: PackedVector3Array = result.get("nodes", PackedVector3Array())
	if cells.is_empty():
		cells = result.get("waypoints", PackedVector3Array())
	var feet: Vector3 = _sim.get_tick_position()
	# The cells already stood in are skipped by COLUMN, not by a radius to their middles. The
	# radius was 0.9, wide enough to cover the body's own corner (0.707) -- and also wide enough
	# to cover the NEXT cell when the body stands near the edge of its own: 0.05 past the edge,
	# the neighbour's middle is 0.5 away. That cell was dropped, the route began two columns off,
	# and the walk reads a cell two columns off as a HOP across a gap (`_hop_ahead`) and refuses
	# to walk into it for want of speed -- so it stood at the lip, dropped the route, was handed
	# the same one back, and stood again (blocked=10, identical position, three seconds of it in
	# nav_trace.txt). Equality of columns is what "where it already stands" means.
	var own_x := floori(feet.x)
	var own_z := floori(feet.z)
	# ...and the column the body STANDS ON, which is not always the one it is in: at the edge of a
	# lip the centre is over the hole and a neighbour carries it, so the plan is asked from that
	# neighbour (`_standing_cell`) -- and the route comes back with it as its first node. Walking to
	# it is walking BACKWARDS, away from the corner the body had just pressed. `nav_trace`
	# 2026-11-26 00:23: eleven rounds of exactly that, `x` pinned at -1.300 (the wall's own face, so
	# the press did work) while `z` climbed back a tenth at a time, because every re-plan put the
	# cell behind it first (`next=(-2, 248, -1)` on the one-second lines). The plan's next node is
	# one legal move from the cell the body stands on, so dropping this one loses nothing.
	var stand := _planned_from if _planned_from != Vector3.INF else _standing_cell(feet)
	var stand_x := floori(stand.x)
	var stand_z := floori(stand.z)
	for cell in cells:
		if _route.is_empty() and ((floori(cell.x) == own_x and floori(cell.z) == own_z) \
				or (floori(cell.x) == stand_x and floori(cell.z) == stand_z)):
			continue
		_route.append(cell)
	# A route that goes NOWHERE is not a stroll: look again soon rather than standing for the whole
	# idle wait on a column that turned out to be unreachable. EMPTY, not short: the head-skip above
	# can cut a two-node answer down to one, and that cell is a real destination -- it is where the
	# body was mid-air over a hole when the route was thrown away, wish and all, and it dropped into
	# the pit instead of stepping onto the far floor (`nav_trace` 2026-11-26 00:30: at 8.77 a
	# `(-1, 249, -2)>(0, 249, -2)` answer became no route at all, `route=0/0` and a 3-block fall).
	if _route.is_empty():
		_route.clear()
		_idle_ticks = FAILED_RETRY_TICKS
		return
		_idle_ticks = FAILED_RETRY_TICKS
		return
	_closest = INF
	_stuck_ticks = 0


## This frame's wish, from the next waypoint the route has not been crossed at.
func _walk() -> void:
	var feet: Vector3 = _sim.get_tick_position()
	var was := _next
	# Reached means horizontally there AND at its level -- see ARRIVE_HEIGHT -- and STANDING
	# on it. A hop's far cell is under half a block away horizontally from the middle of the
	# arc, with the gap still under the body, so an arrival test that ignores the floor ends
	# the route in mid-air: the walk stops wishing and the body drops into the hole it had
	# just jumped.
	while _next < _route.size() and bool(_sim.is_on_ground()) \
			and _flat(_route[_next], feet).length() < ARRIVE_DISTANCE \
			and feet.y >= _route[_next].y + 1.0 - ARRIVE_HEIGHT:
		_next += 1
	if _next != was:
		_closest = INF
		_stuck_ticks = 0
	if _next >= _route.size():
		_trace_say("route walked out (%d cells, %d jumps)" % [_route.size(), _jumps])
		_stop_walking()
		return
	var to := _flat(_route[_next], feet)
	var dir := to.normalized()
	_wish = dir
	_yaw_target = atan2(-dir.x, -dir.z)
	# Walking, it looks where it is going: the look-around is for standing, and a head
	# still turned at whatever it was looking at half a minute ago reads as broken. A
	# player it is already watching keeps the head, and one it is fighting has it outright.
	if not _gazing and _enemy == null:
		_pick_walk_look(dir)
	# A hop first: the next cell two columns over IS the planner's jump across a one-cell gap,
	# and there is nothing in a gap for the step probe below to be blocked by -- a walker that
	# only knows how to press jump at a wall walks off the lip, falls in and asks for the same
	# route again. The jump has to be timed off the lip instead (`_hop_from_lip`).
	var grounded := bool(_sim.is_on_ground())
	# A half pace with no floor under it is a fall, unless the ROUTE is stepping down: the plan may
	# walk off a ledge on purpose, and nothing else may. Asked before the wish is acted on, because
	# the sim will happily walk a body off the edge and the route it was walking then ends in mid-air
	# (`nav_trace` 2026-11-25 23:49: `ground=false`, `vy=-0.52`, `route=0/10`).
	var off_lip := grounded and not _steps_down(_route[_next], feet) \
		and not _stands_after(dir, feet)
	if grounded and _hop_ahead(_route[_next], feet):
		_hop_from_lip(dir, feet)
	# The sim climbs a step of up to 0.6 by itself; a full block it does not, so the step
	# is seen half a pace ahead and the jump control is pressed for this frame. Only a step
	# it can climb, though: anything taller is a wall in a route that no longer exists, and
	# hopping at it is what used to keep a goblin jumping into a block for whole seconds.
	elif grounded and (_blocked(dir, feet) or off_lip):
		# ...and only for a rise the ROUTE asks for: the next cell at least half a block above the
		# feet. `_climbable` alone is the geometry's opinion of the space one block up, which is
		# clear over a wall as often as over a step.
		if _climbable(dir, feet) and _route[_next].y + 1.0 > feet.y + CLIMB_RISE:
			_jump = true
			_jumps += 1
		elif _slide_around(dir, feet, _route[_next]):
			pass
		else:
			# ...counted only while it is NOT getting closer. Pressing a corner and sliding along its
			# face shrinks the gap every tick, and counting those frames dropped the route in the
			# middle of a crossing: eleven drops at `x=-1.300`, a tenth of a block short of the `+x`
			# component coming free, each one re-asked and walked back from the cell behind it
			# (`nav_trace` 2026-11-26 00:23). A squeeze makes no progress, so it still counts.
			if _closest == INF or _flat(_route[_next], feet).length() > _closest + 0.02:
				_blocked_frames += 1
			if _blocked_frames >= REPLAN_AFTER_BLOCKED:
				_replan_reason = "blocked by something it cannot climb"
				_needs_replan = true
	else:
		_blocked_frames = 0


## Does the route step DOWN here? A cell below the feet by more than the level slack is a move the
## plan made on purpose, and walking off its lip is what it means.
func _steps_down(cell: Vector3, feet: Vector3) -> bool:
	return cell.y + 1.0 < feet.y - ROUTE_LEVEL_SLACK


## Whether the next route cell is a HOP: two columns over on one axis and level on the other, and not
## a FALL. Route cells are one legal move apart (a walk, a step, a drop or a hop), so a cell exactly
## two away is the planner's jump across a one-cell gap -- but the planner's hop may also DESCEND, up
## to `max_drop`, and two columns three blocks down is not a jump: it is the pit the route is aimed
## at, walked over the lip rather than launched at. Jumping those was a goblin hopping in place at the
## mouth of a four-deep hole instead of dropping in after the player: `nav_trace` 2026-11-26 00:10,
## route `(-1, 249, -2)>(-3, 246, -2)>(-2, 245, -2)>...`, `jumps` 12 -> 15 with `ground=false` at
## y=250.25 -- above the ledge it was trying to jump down FROM. A hop one block down is still a gap
## to clear and is still jumped.
func _hop_ahead(cell: Vector3, feet: Vector3) -> bool:
	if cell.y + 1.0 < feet.y - 1.0:
		return false
	var dx: int = absi(int(cell.x) - floori(feet.x))
	var dz: int = absi(int(cell.z) - floori(feet.z))
	return (dx == 2 and dz == 0) or (dx == 0 and dz == 2)


## Take the hop, or refuse it. The jump is pressed when the lip is within a couple of ticks
## of the body's own travel (`HOP_LEAD_TICKS`), which is the window the arc was measured in:
## pressed there it lands on the far side, pressed much earlier it lands in the gap. A body
## arriving at the lip too slowly to carry itself across does not step in -- it stops and
## gives up on the route, which is the walk refusing a move it cannot make rather than
## falling into the hole and being handed the same one back.
func _hop_from_lip(dir: Vector3, feet: Vector3) -> void:
	var lead: float = _lip_distance(dir, feet)
	var vel: Vector3 = _sim.get_velocity()
	var reach := Vector2(vel.x, vel.z).length()
	if reach >= HOP_MIN_SPEED and lead <= HOP_LEAD_TICKS * reach:
		_jump = true
		_jumps += 1
		_blocked_frames = 0
		return
	if lead <= HOP_LEAD_TICKS * HOP_MIN_SPEED and reach < HOP_MIN_SPEED:
		_wish = Vector3.ZERO
		_blocked_frames += 1
		if _blocked_frames >= REPLAN_AFTER_BLOCKED:
			_replan_reason = "a hop it cannot carry itself across"
			_needs_replan = true
		return
	# Still walking up to it, or already too slow to take it: nothing to press yet.
	_blocked_frames = 0


## How much of the cell it stands in is left before the gap, along the hop's own axis.
func _lip_distance(dir: Vector3, feet: Vector3) -> float:
	if absf(dir.x) >= absf(dir.z):
		return absf(floorf(feet.x) + (1.0 if dir.x > 0.0 else 0.0) - feet.x)
	return absf(floorf(feet.z) + (1.0 if dir.z > 0.0 else 0.0) - feet.z)


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
	_closing = false
	_closest = INF
	_stuck_ticks = 0
	_lip_frames = 0
	_job = 0


## The session's trace, if the tree has one. It is a child of the ROOT rather than of this body so
## the file has one owner and outlives whichever goblin installed it; a session with no trace in
## it (a probe, a stub world) simply records nothing.
func _install_trace() -> void:
	var tree := get_tree()
	if tree == null or tree.root == null or not NAV_TRACE.claim():
		return
	var recorder: Node = NAV_TRACE.new()
	recorder.name = TRACE_NAME
	tree.root.add_child.call_deferred(recorder)


## The expansion cap that belongs with the chase's current clock: the two are one budget, and a
## doubled clock with a fixed cap would just be cut off by the count instead.
func _chase_expansion_cap() -> int:
	return int(CHASE_EXPANSIONS * (_chase_ms / CHASE_MAX_MS))


## Why the route in hand was dropped, and the counters that produce it. A chase re-asking the same
## plan once a second IS a route being dropped; which of the three tests dropped it is the whole
## question, and the waypoint it was pushing at says whether the walk was ever aimed at the climb.
func _trace_route_dropped() -> void:
	if _sim == null or _trace() == null:
		return
	var at := "none"
	if not _route.is_empty() and _next < _route.size():
		at = str(_route[_next])
	_trace_say("route dropped: %s (at=%s next=%s route=%d/%d blocked=%d stuck=%d jumps=%d)"
		% [_replan_reason, str(_sim.get_tick_position()), at, _next, _route.size(),
		   _blocked_frames, _stuck_ticks, _jumps])
	var trace := _trace()
	if trace != null:
		trace.note_drop(_replan_reason)


## The recorder, looked up once: a chase records a handful of lines a second, and a path lookup
## per line would be absurd.
func _trace() -> Node:
	if _trace_node == null or not is_instance_valid(_trace_node):
		_trace_node = get_node_or_null(TRACE_NODE)
	return _trace_node


func _trace_say(text: String) -> void:
	var trace := _trace()
	if trace != null and _sim != null:
		trace.add_line("goblin@%s %s" % [str(_sim.get_tick_position()), text])


func _trace_plan_asked(job: int, from: Vector3, target: Vector3, expansions: int,
		max_ms: float) -> void:
	var trace := _trace()
	if trace != null and job != 0:
		trace.plan_asked(job, _plan_role, from, target, expansions, max_ms)


func _trace_plan_answered(job: int, result: Dictionary) -> void:
	var trace := _trace()
	if trace != null and job != 0:
		trace.plan_answered(job, _plan_role, result)


## Where this body is and what its route is doing, for the trace's once-a-second sample
## (`scripts/nav_trace.gd`): one line and no newlines, because it is read in a column beside the
## player's own position.
func trace_state() -> String:
	if _sim == null:
		return "goblin@%s state=%s (no body)" % [name, speed_state]
	var enemy := "none"
	if _enemy != null and is_instance_valid(_enemy):
		enemy = str(_enemy.global_position)
	var next := "none"
	if not _route.is_empty() and _next < _route.size():
		next = str(_route[_next])
	# The format string is built as one value FIRST: `%` binds tighter than `+`, so a split string
	# formatted on its second half alone is the quiet kind of wrong.
	# wish is what the walker is ASKING for and vy is what the body is doing with it: a body pressed
	# against a wall has a wish and no speed, and a body that is swimming has both and no ground --
	# the two look alike in a position. `lip` is the frames it has spent pressed at a climb only the
	# swim's lift can make, and `strokes` counts the ticks it has stroked this session (it rises
	# while a body is in water and stands still while it is not).
	var wish := Vector3.ZERO
	var vy := 0.0
	if _sim != null:
		wish = _wish
		vy = float(_sim.get_velocity().y)
	var line := ("goblin@%s state=%s at=%s enemy=%s planned=%s next=%s " +
		"route=%d/%d wait=%d/%d reach=%s job=%d blocked=%d stuck=%d jumps=%d ground=%s " +
		"wish=(%.2f, %.2f) vy=%.2f wet=%s lip=%d strokes=%d")
	return line % [name, speed_state, str(_sim.get_tick_position()), enemy, str(_planned_goal),
		next, _next, _route.size(), _chase_wait_ticks, CHASE_WAIT_TICKS, _in_reach, _job,
		_blocked_frames, _stuck_ticks, _jumps, bool(_sim.is_on_ground()),
		wish.x, wish.z, vy, bool(_sim.is_in_water()), _lip_frames, _strokes]


## The waypoint's cell middle, relative to the feet. Only x/z matter to the walk: the
## feet follow whatever the ground does under them.
func _flat(cell: Vector3, feet: Vector3) -> Vector3:
	return Vector3(cell.x + 0.5 - feet.x, 0.0, cell.z + 0.5 - feet.z)


## Would half a pace forward be stopped? The same collision the body itself resolves.
func _blocked(dir: Vector3, feet: Vector3, reach: float = STEP_PROBE) -> bool:
	var res: Dictionary = _chunk_manager.resolve_voxel_collision(
		feet, Vector3(dir.x, 0.0, dir.z) * reach, BODY)
	return bool(res.get("collided_x", false)) or bool(res.get("collided_z", false))


## Can it step up onto whatever is in front? A one-block step is a jump the body can make;
## anything taller is a wall. The body is asked about the space one block up, so a slab, a
## stair and a full block all answer as they collide rather than by their ids.
func _climbable(dir: Vector3, feet: Vector3) -> bool:
	var res: Dictionary = _chunk_manager.resolve_voxel_collision(
		feet + Vector3(0.0, 1.0, 0.0), Vector3(dir.x, 0.0, dir.z) * STEP_PROBE, BODY)
	return not (bool(res.get("collided_x", false)) or bool(res.get("collided_z", false)))


## Walk a blocked DIAGONAL by PRESSING it: a free-dominant wish the sim can take in one tick.
##
## A diagonal corner is two cells either side, and the planner allows the move when only ONE of them
## stops the body (`passes_beside`, src/pathfinding/move_generator.hpp). The CHORD from this cell's
## middle to the next one's passes through the solid side, so the body cannot walk it as a straight
## line -- its own box cuts that cell by half a body -- and it must not go flat along the wall either,
## because nothing then presses it across when the wall ends.
##
## Two things have to hold for the crossing, and the resolver's own order is the second:
##   - the wish keeps BOTH components, so the wall's face holds the press while the free axis carries
##     the body along, and the press takes the moment the box leaves the wall's cell;
##   - the FREE axis is the larger one (`CORNER_PRESS`), because `collision_resolver.cpp` takes "Y
##     first, then whichever horizontal axis has the LARGER motion magnitude". Pressed-axis-first is
##     tested while the box is still inside the wall's cell: it is clipped, the free axis moves after
##     it, and the tick ends with the body past the wall and its press spent -- `nav_trace`
##     2026-11-26 00:47, `x` stuck at -1.300005 while `z` ran to -1.545 and `y` fell to 247.85.
## The result is the line a player walks there, one tick wide: across `(-2, 249, -1) -> (-1, 249, -2)`,
## wall at `(-1, -1)` two high and a four-deep hole at `(-2, -2)`, his `x` holds at -1.30 while `z`
## runs -0.93 -> -1.58 and then both move inside one tick (`nav_trace` 2026-11-26 00:10).
##
## A free half is normally taken only while the resolver says the body would still be STANDING on it
## (`_stands_after`), because the fall that rule exists for was a slide along a wall's face into the
## gap beside it. At a corner that guard refuses the only correct line: the free leg is short by
## design and ends over the FAR FLOOR, not over the gap. So it is allowed when the other half is a
## wall and the diagonal's own destination is standable (`_has_floor`) -- a corner cut, and nothing
## else. Both halves clear is not a corner (the chord would not be blocked); both solid is a squeeze
## no 0.6-wide body fits through, and the caller counts it.
func _slide_around(dir: Vector3, feet: Vector3, cell: Vector3) -> bool:
	if absf(dir.x) <= 0.05 or absf(dir.z) <= 0.05:
		return false
	var halves: Array[Vector3] = [Vector3(dir.x, 0.0, 0.0), Vector3(0.0, 0.0, dir.z)]
	var walls: Array[Vector3] = []
	for half in halves:
		var step := half.normalized()
		if _blocked(step, feet):
			walls.append(step)
	for half in halves:
		var step := half.normalized()
		if _blocked(step, feet):
			continue
		if not _stands_after(step, feet) and not (walls.size() == 1 and _has_floor(cell, feet)):
			continue
		_wish = step
		if walls.size() == 1:
			_wish = (step + walls[0] * CORNER_PRESS).normalized()
		_yaw_target = atan2(-_wish.x, -_wish.z)
		_blocked_frames = 0
		return true
	return false



## Would the body still be STANDING a half pace that way? A slide walks along ground, so a half that
## walks off a drop is not a way round the corner -- it is a body that falls. That is the second half
## of this rule, from the same session: the slide went along the wall's face into the gap beside it,
## the body fell three blocks into water (`ground=false`, `vy=-0.45`, then `lip=30` under the
## player) and kept the route it fell with.
##
## `on_floor` is the resolver's own answer to the same question -- the body's own footprint, put
## down a hair from where the half pace ends -- so this cannot disagree with what the sim does next
## tick. A world that does not answer it is treated as floor, which is the behaviour before this.
func _stands_after(dir: Vector3, feet: Vector3, reach: float = STEP_PROBE) -> bool:
	# ...and a STEP DOWN counts: a half pace into a one-block drop lands a body on the floor below it,
	# which is a move the planner routes through (`max_drop`), so the probe is taken from where the
	# pace ends and asks whether the body would land within the drop it may take. Measured only at
	# the feet's own level, a step-down slide was refused and the lip became a wall.
	var landed: Dictionary = _chunk_manager.resolve_voxel_collision(
		feet + Vector3(dir.x, 0.0, dir.z) * reach, Vector3(0.0, -FALL_PROBE, 0.0), BODY)
	return bool(landed.get("on_floor", true))


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
	# `blend`, not `ease`: the name of the built-in function is not a variable's to take.
	var blend := 1.0 - pow(1.0 - TURN_EASE, delta * TICK_RATE)
	_yaw = lerp_angle(_yaw, _yaw_target, clampf(blend, 0.0, 1.0))
	_model.rotation.y = _yaw


## Put the body somewhere else with no velocity (a probe, a future respawn): the same
## reset the player's own node does.
## `to`, not `position`: Node3D already has a property by that name, and a parameter that shadows
## it reads as if this function were setting the node's own.
func teleport(to: Vector3) -> void:
	if _sim == null:
		return
	_sim.reset(to)
	# Both drawn samples move with it, or the next frame interpolates from where it
	# used to be and the body slides across the gap.
	_drawn_prev = to
	_drawn_cur = to
	global_position = to
	_abandon_route()


## A punch, from PlayerController. The shove and the damage share the sim's hurt window:
## `shove` answers false while it resists, so a resisted hit is neither knockback nor
## damage -- which is what stops hold-clicking from stacking either.
##
## It does NOT stop what the goblin was doing. A hit is motion and a hurt flash, not a
## command: a mob in the game keeps its path through one -- the shove is a velocity, and its
## own walking keeps pushing against it -- so a goblin punched on its way somewhere keeps
## going there, staggering for a step. A hit that cancelled the stroll read as a stun the
## game does not have.
func apply_knockback(attacker_position: Vector3, extra_velocity := Vector3.ZERO) -> void:
	if _sim == null:
		return
	if not bool(_sim.shove(attacker_position, extra_velocity)):
		return
	take_damage(PUNCH_DAMAGE)
	# And it takes it personally, the way a mob keeps a revenge target: the puncher becomes
	# the one to walk at -- or, under FLEE_HEALTH, the one to run from.
	var player := _player()
	if player != null \
			and player.global_position.distance_to(_sim.get_tick_position()) <= AGGRO_RANGE:
		_enemy = player
		_fleeing = _health <= FLEE_HEALTH
		_anger_ticks = randi_range(_anger_min_ticks, _anger_max_ticks)
		_chase_ms = CHASE_MAX_MS
		_trace_say("hit: enemy taken, health %.0f, fleeing=%s" % [_health, _fleeing])
		_attack_ticks = 0
		_repath_ticks = 0
		_chase_wait_ticks = 0
		_abandon_route()
		# ...and it shouts. One goblin hurt by a player wakes its pack, which is what turns a row of
		# separate fights into one: see ALERT_RADIUS.
		_alert_nearby(player)


## Every goblin within ALERT_RADIUS of this one turns on the same attacker. The shout is the
## alert, not a fight of its own: each body takes the anger the way it takes a hit's, and the
## one that was hit is no different from any other by the time the box is walked.
func _alert_nearby(attacker: Node3D) -> void:
	if attacker == null or _sim == null or not is_inside_tree():
		return
	var here: Vector3 = _sim.get_tick_position()
	for other in get_tree().get_nodes_in_group("goblin"):
		if other == self or not (other is Node3D) or not other.has_method("take_anger"):
			continue
		var at: Vector3 = (other as Node3D).global_position
		if absf(at.x - here.x) > _alert_box or absf(at.z - here.z) > _alert_box:
			continue
		if absf(at.y - here.y) > _alert_box:
			continue
		other.call("take_anger", attacker)


## Turn on an attacker without having been hit: the alert another goblin's fight sends out. A
## goblin already badly hurt still runs rather than joins, the same rule its own hits follow.
func take_anger(attacker: Node3D) -> void:
	if _sim == null or attacker == null:
		return
	_enemy = attacker
	_fleeing = _health <= FLEE_HEALTH
	_anger_ticks = randi_range(_anger_min_ticks, _anger_max_ticks)
	_chase_ms = CHASE_MAX_MS
	_trace_say("alerted: enemy taken, health %.0f" % _health)
	_attack_ticks = 0
	_repath_ticks = 0
	_chase_wait_ticks = 0
	_abandon_route()


func take_damage(amount: float) -> void:
	_take_hurt(amount)


## The one place half-hearts come off, whether they were thrown by a fist or by the ground it
## landed on: a fall is health like a punch is, so it can kill and it drops what the body was
## carrying (`die`).
func _take_hurt(amount: float) -> void:
	if _health <= 0.0:
		return
	_health = maxf(_health - amount, 0.0)
	print("Goblin hit: %.0f of %.0f half-hearts left" % [_health, _max_health])
	if _health <= 0.0:
		die()


## At zero it goes: what it was carrying falls where it stood, and then the body is
## freed, which the spawn tool notices on its next frame (it drops the dead body's light
## entry with it).
func die() -> void:
	print("Goblin dies")
	_drop_loot()
	_give_up()
	queue_free()


## What a dead goblin leaves: the loot table's own answer, dropped where it fell. The
## table is data (`data/loot_tables.json`, rolled by `LootTables.roll`), not three rolls
## written here, so what a goblin drops is a file rather than a script change.
##
## The drops are spawned through the world's dropped-item node (`spawn_at`), which is
## the same body a thrown item is: they fall, land, can be walked over, and the
## inventory's own stack cap decides what a pickup can take.
func _drop_loot() -> void:
	var dropped := get_node_or_null(NodePath("/root/Main/DroppedItems"))
	if dropped == null or not dropped.has_method("spawn_at"):
		return
	var at := global_position
	if _sim != null:
		# Out of the body, not out of its feet: the pile falls and settles where the
		# goblin was standing instead of starting under the ground it lies on.
		at = _sim.get_tick_position() + Vector3(0.0, 0.9, 0.0)
	for drop in LootTables.roll("goblin"):
		dropped.call("spawn_at", int(drop.get("block_id", 0)), int(drop.get("count", 0)), at)


func get_health() -> float:
	return _health


func get_max_health() -> float:
	return _max_health


## Blocks a second this state asks for.
func get_move_speed() -> float:
	return _movement_speed * BLOCKS_PER_SECOND * float(SPEED_STATES.get(speed_state, 1.0))


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
