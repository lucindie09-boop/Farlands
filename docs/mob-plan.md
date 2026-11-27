# Mob plan

What is left to do on the goblins, in the order it is worth doing, written at the end of the
session that gave them a real fight: a chase that plans, a pack that answers a punch, a leash, a
close-in attack, and an egg that spawns one. Each item says what a player sees today, why it is
worth the change, and — where it is cheap to say — what it touches.

The order is by value, not by size. The first two are things a player would call broken; the
next
few need a decision about the game rather than more code; the last group is the structure that
keeps the second mob from costing a week.

Six of these are already done and are kept here so the reasoning survives: **1** (a chase
that cannot reach its target gives up), **2** (which cell a chase is aiming at, including the
low-roof bug in the column scan), **3** (mob bodies collide with each other and with the
player), **9** (water and fall damage), which landed with the same walk, **12** (mob
attributes as data), and **16** (looting, which arrived after this list was written).

## What a player would call broken

### 1. A chase that cannot reach its target gives up — done
The search returns a best-effort route to the frontier it reached when its budget runs out, so a
player on a pillar, behind a sealed wall or in a pit it cannot climb still leaves the goblin
with
something to walk to, and it stood under that target for the whole of its anger window.
`CHASE_WAIT_TICKS` in `scripts/goblin.gd` is the rule now: three seconds with nothing to walk
and
nothing being planned ends the chase. A long way round keeps a route under its feet the whole
time, so it is never cut short by this — the patience measures having nowhere to go, not being
far
away.

### 2. Which cell a chase is aiming at — done
The goal was already the player's own feet cell, anchored by the planner. The bug was in that
anchoring: `NavView::column` answers "what surface could an agent arrive on from a neighbour",
which may be up to `max_rise` *above* the hint, and the endpoints were anchored with it. A
collision in either of the two cells over a body's head — the roof of a two-block gap — was read
as that column's floor, so a body under a low roof planned from standing *on* the roof, and a
goal
under one resolved on top of the ceiling over the player. `NavView::column_at_feet` is the other
question and the planner's endpoints use it: the surface must be at or below the feet the caller
stands at. Moves are still generated from `column`, where a surface above the hint is exactly
what
a step up onto the neighbour is.

The remaining form of the same report — a player standing at the edge of an overhang, where the
column under their feet legitimately holds no platform — is not fixable this way: the planner's
world model holds **one surface per column** and does not represent walking underneath an
overhang at all (`tests/test_nav_view.cpp` pins that). That one landed as the sequel: the goal
is
aimed at the cell the player stands in, the planner anchors it on the neighbouring column whose
surface puts a body's feet in *exactly* that cell (`Pathfinder::resolve_goal`), and a chase
walks
up the staircase ten blocks round instead of standing on the plain three blocks under its target
— which the search used to call `found`, because it is a cell a body can stand on. A chase's
patience counts a plan in flight as nowhere to walk too, so a re-ask every few ticks can no
longer
keep the three-second clock reset, which is how it stood there indefinitely.

A third one came out of the same trace, in the search rather than the chase: **expansions** were
resolving a node's column with the "what could arrive here" question, so a ceiling two cells over
the body's head was read as its floor. Routes then carried moves no body can make — a three-block
rise in one step — and the walk answered by pressing at walls and hopping at one-block gaps it
cannot fit through. The search expands with `column_at_feet` now, and only the TARGET of a move
still looks above the hint, which is what a step up is.

A fourth came out of walking what those routes said. The planner **hops** a one-cell gap, and
the walker only knew how to press jump at a block it was blocked by — a gap blocks nothing — so
the hop read as a clear edge: the body walked off the lip, fell in and asked for the same route
again. The walk jumps off the lip now (`goblin.gd`), in the window the arc was measured in (0.9
blocks airborne at a mob's pace against the 0.7 a gap needs), and the planner no longer emits
the hop that cannot be taken at all — one that climbs, where the rise spends that clearance.

### 3. Mob bodies do not collide with anything — done
A goblin walked through the player and through other goblins, and `CHASE_STOP` (0.9 blocks, about
two bodies' half-widths) was a number standing in for a body that did not exist: a pack of five
read as one silhouette. Bodies are boxes now, and two boxes in one place is not something the
reference allows either — its answer is `Entity::push(Entity)`, which
`src/engine/entity_push.cpp` implements as written because the FEEL is the point:

  * the two boxes have to intersect, which is the test the reference gets from the query that
    finds an entity's neighbours;
  * the pair is decided on the larger of the two centre gaps, horizontally, and the push is
    normalised by that gap's SQUARE ROOT rather than its length, with the factor capped at one —
    so a wider pair is pushed the full `kEntityPush` (0.05 a tick, a block a second) and a pair
    that barely overlaps is nudged less, which is what stops a crowd in a doorway exploding;
  * two bodies sharing a centre exactly are left alone: there is no direction to pick, and the
    reference has the same hole. The next tick's walk separates them.

The push is VELOCITY, not a position write: `PlayerSim::push` is `velocity_ += velocity`, so the
world's own collision still resolves it against the ground and nobody is ever pushed through a
wall or off a ledge by it. It is once a TICK rather than once a frame (a frame-rate call would
make a fast machine squeeze twice as hard), it is applied to both bodies at once, and a pair is
answered by whichever body has the lower instance id so it costs one push rather than two.

The player is a body in the same list: `goblin.gd` pushes his half through a `push` binding on
`PlayerController`, so a pack leans on the body it is crowding the same way the bodies crowd each
other. Being leaned on is not a blow — it opens no hurt window and cancels no route; `CHASE_STOP`
is still where a chase plants itself, but it is now a walking choice rather than the only thing
keeping a goblin out of the player's box.

`probes/probe_mob_push.gd` puts three goblins inside one body's width of each other, and one
inside the player, and checks they come apart to a body width; `tests/test_entity_push.cpp` pins
the arithmetic, the box gate, the cap and the coincident-centre hole.

### 4. Target lifecycle around death
When the player dies the goblins keep swinging at the body, and their anger survives the
respawn,
so a return can be met by a mob that is already angry across the map. `died` should clear every
goblin's target. Cheap, visible, and the one path a stub-world probe cannot see.

### 5. The fight has no sound and no reaction
No hurt sound, no attack sound, no death sound; no red flush on a hit, no death animation, no
loot. A blow is currently a number going down. This is the highest feel-per-line item left: hurt
flash and death fall are local changes to the goblin's model and health, sound needs only the
existing audio path checked.

## Needs a decision, not just code

### 6. A headless roam probe that can run
Every behaviour is proved against a stub world; the one thing never tested is a goblin pathing a
real chase, which is where all four of the last reports came from. A synthetic grid with a
platform, a ledge, a pit and a valley would pin the platform, leash and close-in cases as
regressions. `probes/probe_path.gd` is red on its own precondition at spawn today, so the nav
suite is dead weight until that is fixed too.

### 7. Persistence, despawn and a cap
If a spawned goblin is only a scene node, a save/load wipes it and the egg is a toy. A
spam-clicked egg is also an unbounded per-tick cost — each chaser raycasts for sight every tick
and plans every few. Persist a spawned mob (position, health, no target) and despawn it past
about
128 blocks, as the game does.

### 8. Reach against the player's own
The goblin hits at 1.45 blocks three-dimensionally; if the player's own reach is nearer four,
the
goblin can only land a blow by walking into range and the fight is decided before it starts.
Pick
both numbers together.

### 9. Water and heights — done
Two halves, and in the end one story: a goblin that could not cross water could not be chased
into it either.

The PLANNER crosses water now. A wet column was refused outright (`is_usable`), because a cost is
not a wall and the old +0.6 surcharge had been read as "wade anyway" by a goblin that walked into
lava — so the refusal moved onto the cell, and the two liquids are separated where they can still
be told apart: water sets `Cell::wade` in `classify_block`, from the block's own `fluid_kind`.
A wadeable column is a legal step at `NavCosts::liquid` — 2 on top of a walk — so a two-cell
crossing costs six against a thirteen-cell detour, and a three-cell swim already loses to a short
dry way round. The price is only half of it: the SURFACE is now the liquid's own top, the level a
swimmer rides at. Descended to the floor, as the scan used to do, every way out of a pond is a
climb a step-up cannot make, so no route could use the water at all — which is exactly the
"thirteen blocks round a two-block puddle" a player would see.

In the water the body is the walker's. `_swim` holds the jump control (in water that IS the
stroke — the sim lifts a body on it, and lifts it again when it collides while swimming, which is
what carries it over a lip) and follows the route cell by cell through `_swim_along`, which skips
cells by horizontal distance alone: a swimmer is never STANDING on one, so `_walk`'s arrival test
would never advance and the body would push at its first wet cell forever. With no route it
paddles at the nearest dry column it can find (`_shore_from`, ringed out to `SHORE_REACH` through
the body's own `is_liquid_at`/`is_solid_at`, kept for a second), which is the way out of water it
was dropped into; and a chase's give-up clock does not run while paddling, so falling in a pond is
not the player becoming unreachable.

A target standing IN water is the one chase a route still cannot answer, because the goal's ring
anchors on dry ground. That chase is steered STRAIGHT AT instead: the route is dropped, the
wade/swim is the same steer as the last pace before a blow, and the look is the same ray the
reach is measured through, which water does not stop — so the blow lands in the pond as it would
on the bank. Once both are dry the planner is asked again.

The other half is the fall. `PlayerSim` already tracked a landing and queued it as a Fall source
through the same hurt window a blow uses, and the mob's body was DRAINING that queue and throwing
the answer away, so a ledge cost a goblin nothing. `MobBody::consume_damage` hands it back in
half-hearts, and `_take_hurt` is now the one place health comes off — a fist or the ground — so a
fall can kill, and a body killed that way drops what it was carrying.

`probes/probe_goblin_water.gd` digs a 5x5 pond two deep beside the player and watches a goblin
climb out of it onto the rim; drops one ten blocks and holds it to the 7 half-hearts the rule
owes; builds an island in the pond with a cell of water all the way round it and watches a chase
cross to prey standing on it; and finally puts the prey in the water itself, where the chase has
to wade in and swing.

### 10. Anger and flight have no tell
A pigman raises its arms when angry. A goblin looks identical calm or furious, and a sprinting
flee at 1.3× speed is the only sign it is running. A pose or an eye tint for anger and a
distinct
run for fleeing would communicate two whole rules without a tutorial.

### 11. Does a goblin that fled ever fight again
`FLEE_HEALTH` reads as a one-way latch and mobs do not regenerate, so a goblin that escapes at
five half-hearts may flee for the rest of its life and can only be killed by chasing it down.
Settle the design — heal slowly out of sight, or re-engage after a while — before more behaviour
is built on it.

## Structure, so the next mob is cheap

### 12. Mob attributes as data — done
Health, damage, speed, follow range, alert box and anger window live in `data/mobs.json`, the
way blocks, biomes, items and shaders do, and the walker reads the entry its own `MOB_TYPE`
names. The shape is the loot table's: a Godot-free parser (`src/core/mob_attributes.cpp`,
`schematic::parse_json`, so the test binary reads an entry out of a string), a `Mobs` binding
for the file read and the struct -> Dictionary translation, and one load at startup beside the
loot tables.

Two decisions worth keeping. Every field is OPTIONAL, defaulting to the struct's own value, so
a new entry states only what it changes and is two lines rather than seven — while the goblin
states all six, because that file is the answer to "what is a goblin" and not a patch over a
script. And a file that cannot be a body is REFUSED with the field named (no health, a negative
blow, a zero speed, a window that runs backwards), leaving the previous entries in force: a
typo in a data file must not be a body with zero health in a live world.

On the script side the six constants stay as the fallback and nothing reads them any more:
`_apply_attributes` fills `_max_health`, `_damage`, `_movement_speed`, `_follow_range`,
`_alert_box` and the anger pair from the registry in `_ready`, and every use site reads those.
So a missing file, an entry that leaves a field out or a DLL from before this existed all fall
back to the numbers the goblin was tuned with, and tuning is a diff in the data file.
`probes/probe_mob_attributes.gd` is the end-to-end half: a spawned body runs with the file's
numbers, and with a RELOADED file's — which a body reading its own constants would fail.

### 13. A mob base out of `goblin.gd`
The script now holds health, the hurt window, knockback, anger, chase, flee, gathering and the
planner steering. Every item above lands in it. Extract after the correctness work, so the base
is
cut from behaviour that is already right.

### 14. Guards on the egg
Spawning into a block, on a liquid, in mid-air, or inside the player's own feet; a short use
cooldown so one click is one goblin; whether the egg is consumed. Trivial, and the first thing
anyone tries.

### 15. Measure a pack before optimising it
Ten angry goblins each raycast every tick and plan with a 20,000-expansion budget. Nobody has
measured that. A probe that spawns ten and reports the per-frame cost comes first; if it is
expensive, throttle the sight check and share one plan per pack with offsets.

## Added after this list

### 16. Looting — done
A goblin with an empty main hand that notices a dropped pile — eight blocks flat, two of height,
scanned twice a second while it stands with nothing to do — turns its head to it, asks for a
route with the pace table's own `loot` state (1.1x the walk, and `walk_to` copies the state into
`_plan_role`, so `nav_trace` labels the errand `loot`), walks there, takes ONE item into its main
hand and raises that arm until the item is in front of its face for three seconds. A tool stays
in the hand for good and is dropped with the torch when the body dies; anything else is thrown
back down in front of it. A full main hand stops the scan: one carried item, no inventory and no
UI, and the kept tool changes no damage and mines nothing — it is CARRIED.

What the hand is DRAWN in is the player's own rule, because an item hanging dead straight at a
side does not read as held: the shoulder pivot (`HeldArmPivot`) sits on `arm2` — the arm this
body holds things in — and tips it 18 degrees forward while the hand is full, with the walk's
swing halved over that same arm. The inspection lets the pivot go for its three seconds: the 80
degrees is the arm's angle from rest, and composed over the held turn the hand would end up
above the face.

Two things here are worth keeping for the next mob. **"Looks at it" is a pose, not a head** —
the neck pitches 40 degrees down and an item hanging at the side is 70 below the head, so the arm
has to come up, and the AnimationTree rewrites that arm's rotation every frame: the pose stops
the tree for the three seconds (the same switch `skip_head_look` uses), writes
`_clip_rest["arm2"] * Quaternion(RIGHT, pitch)` for that frame and still tracks the head, and
lets the tree take the arm back on release. No second shoulder pivot, because that changes the
arm's node path and `_clip_for_rig`'s part map, while freezing the tree cannot break a clip
path. **The reject is remembered by POSITION, not by a node** — `spawn_at` builds a new body and
a merge can replace it, so the ten-second ignore window is a point and a deadline.

The one C++ change is the tool query (`BlockTextures.is_item_tool`, the registry's own
`is_tool`, since nothing on the script side could answer it), and the two methods on
`scripts/dropped_items.gd` are the only new search: `nearest_item` and `take_one`, which share
one loop and one `_set_count`. That `_set_count` is the trap the task was worth writing down:
`_join` already rebuilt a pile's size, boxes, shape row, mesh scale and pop for a new count, and
a decremented pile that keeps the old body is drawn and solved as the size it no longer is.
`probes/probe_goblin_loot_run.gd` pins the state machine and `probes/probe_entity_light.gd` the
second hand's light; `docs/gui-notes.md` holds the holder and the pose.
