# Mob plan

What is left to do on the goblins, in the order it is worth doing, written at the end of the
session that gave them a real fight: a chase that plans, a pack that answers a punch, a leash, a
close-in attack, and an egg that spawns one. Each item says what a player sees today, why it is
worth the change, and — where it is cheap to say — what it touches.

The order is by value, not by size. The first two are things a player would call broken; the next
few need a decision about the game rather than more code; the last group is the structure that
keeps the second mob from costing a week.

Two of these are already done and are kept here so the reasoning survives:
**1** (a chase that cannot reach its target gives up) and **2** (which cell a chase is aiming at,
including the low-roof bug in the column scan) landed with it.

## What a player would call broken

### 1. A chase that cannot reach its target gives up — done
The search returns a best-effort route to the frontier it reached when its budget runs out, so a
player on a pillar, behind a sealed wall or in a pit it cannot climb still leaves the goblin with
something to walk to, and it stood under that target for the whole of its anger window.
`CHASE_WAIT_TICKS` in `scripts/goblin.gd` is the rule now: three seconds with nothing to walk and
nothing being planned ends the chase. A long way round keeps a route under its feet the whole
time, so it is never cut short by this — the patience measures having nowhere to go, not being far
away.

### 2. Which cell a chase is aiming at — done
The goal was already the player's own feet cell, anchored by the planner. The bug was in that
anchoring: `NavView::column` answers "what surface could an agent arrive on from a neighbour",
which may be up to `max_rise` *above* the hint, and the endpoints were anchored with it. A
collision in either of the two cells over a body's head — the roof of a two-block gap — was read
as that column's floor, so a body under a low roof planned from standing *on* the roof, and a goal
under one resolved on top of the ceiling over the player. `NavView::column_at_feet` is the other
question and the planner's endpoints use it: the surface must be at or below the feet the caller
stands at. Moves are still generated from `column`, where a surface above the hint is exactly what
a step up onto the neighbour is.

The remaining form of the same report — a player standing at the edge of an overhang, where the
column under their feet legitimately holds no platform — is not fixable this way: the planner's
world model holds **one surface per column** and does not represent walking underneath an
overhang at all (`tests/test_nav_view.cpp` pins that). Reaching that player means routing to a
cell beside them, which is a planner change and not a chase change.

### 3. Mob bodies do not collide with anything
A goblin walks through the player and through other goblins. `CHASE_STOP` (0.9 blocks, about two
bodies' half-widths) is a stand-in for the body that should be stopping it, and a pack of five
stands in one silhouette. Mutual push-apart plus a player body is what makes "it presses in" and
"there is a pack" read as those things. Touches `MobBody`/the collision resolver, and every mob at
once — do it once, in the shared place, not per mob.

### 4. Target lifecycle around death
When the player dies the goblins keep swinging at the body, and their anger survives the respawn,
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
and plans every few. Persist a spawned mob (position, health, no target) and despawn it past about
128 blocks, as the game does.

### 8. Reach against the player's own
The goblin hits at 1.45 blocks three-dimensionally; if the player's own reach is nearer four, the
goblin can only land a blow by walking into range and the fight is decided before it starts. Pick
both numbers together.

### 9. Water and heights
The planner refuses liquid steps, so a goblin shoved into a pond stands on the bottom (and, before
item 1, never gave up either). Floating and paddling out is the game's answer. Mob fall damage is
the other half: it would make shoving one off a ledge a tactic rather than a favour.

### 10. Anger and flight have no tell
A pigman raises its arms when angry. A goblin looks identical calm or furious, and a sprinting
flee at 1.3× speed is the only sign it is running. A pose or an eye tint for anger and a distinct
run for fleeing would communicate two whole rules without a tutorial.

### 11. Does a goblin that fled ever fight again
`FLEE_HEALTH` reads as a one-way latch and mobs do not regenerate, so a goblin that escapes at
five half-hearts may flee for the rest of its life and can only be killed by chasing it down.
Settle the design — heal slowly out of sight, or re-engage after a while — before more behaviour
is built on it.

## Structure, so the next mob is cheap

### 12. Mob attributes as data
Health, damage, speed, follow range, alert box and anger window in a file of their own under
`data/`, the way blocks, biomes, items and shaders are. Tuning becomes a diff instead of a rebuild, and the next mob
becomes a data entry with a model.

### 13. A mob base out of `goblin.gd`
The script now holds health, the hurt window, knockback, anger, chase, flee, gathering and the
planner steering. Every item above lands in it. Extract after the correctness work, so the base is
cut from behaviour that is already right.

### 14. Guards on the egg
Spawning into a block, on a liquid, in mid-air, or inside the player's own feet; a short use
cooldown so one click is one goblin; whether the egg is consumed. Trivial, and the first thing
anyone tries.

### 15. Measure a pack before optimising it
Ten angry goblins each raycast every tick and plan with a 20,000-expansion budget. Nobody has
measured that. A probe that spawns ten and reports the per-frame cost comes first; if it is
expensive, throttle the sight check and share one plan per pack with offsets.
