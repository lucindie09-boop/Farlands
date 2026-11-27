# Pathfinding notes

The ground-route planner: what it is built on, the rules its design settled on, and
how it is wired into the game and to the tests.

## The planner

- **Ground-route planner with no Godot dependencies** — `nav_types.hpp` (cell classes,
  `NavQuery`, the cost model, packed node keys), `nav_view.hpp/cpp` (lazy memoised column
  surfaces), `move_generator.hpp` (the movement primitives), `pathfinder.hpp/cpp` (budgeted
  deterministic A*), `path_smoother.hpp/cpp` (string-pull). Covered standalone by
  `tests/test_nav_view|test_move_generator|test_pathfinder|test_path_smoother.cpp`, and the
  chunk-map half by `tests/test_chunk_nav_source.cpp` (ranged read vs per-cell equivalence,
  Unknown-not-air, frozen residency, lock scaling, a whole plan over a real `ChunkMap`; fixture
  is `tests/chunk_map_fixture.hpp`); timing probe is `scons path_cost` (real generated terrain),
  which also prints what a 0.5 ms deadline does to a 192-block query. The results-only headers
  stay Godot-free; `block_class.hpp` / `chunk_nav_source.hpp` / `path_service.hpp/cpp` are the
  Godot-side half
- **The goal is a cell, not a column** — `NavQuery::goal_y_slack` defaults to 0, so the arriving
  node must stand on the surface the goal itself resolved to. A column can hold several
  standable surfaces: the ground *under* a floating staircase is in the same column as the step
  above it, so a column-only goal test is satisfied twenty blocks below the target, which in
  game reads as "the dummy refuses to climb the stairs and walks underneath them instead".
  `navtest::dijkstra_cost` (the brute-force optimality reference) uses the same cell-level rule
  — a reference that accepted the goal's column alone would report a cheaper "optimum" than any
  real route and quietly invalidate every optimality comparison
- **The heuristic is in the cost table's units** — the octile estimate multiplies its orthogonal
  term by `NavCosts::walk`; hardcoding 1.0 there makes any table whose walk term is not exactly
  1.0 overestimate the distance, which costs A* its optimality while still returning a legal
  route
- **Runtime wiring**: `block_class.hpp` holds the single block→nav-cell conversion (do not
  re-derive it — `BlockType::is_full_cube()` is `true` for air in the built-in default registry,
  and is only recomputed from real shapes once `block_definitions.json` loads).
  `chunk_nav_source.hpp` reads the live `ChunkMap` two ways: `sample()` is one locked accessor
  call per cell (residency is answered from under that same lock, so a cell never costs two),
  while `read_column()` reads a whole column range under a single `lock_keys` and releases it
  before returning, so a search's map locks scale with columns resolved rather than with cells
  classified. **Neither path may hold a lock between calls** — a shard held across calls stalls
  chunk generation and edits on that shard, and deadlocks outright if the same thread writes —
  so `NavView` takes an optional ranged reader and prefetches each column's scan window into a
  buffer. A per-instance residency cache keeps a chunk that was missing when the search started
  missing for the rest of it. `PathService` queues a search on the engine thread pool
  (`submit`/`poll`, results keyed by job id, all shared state in a `shared_ptr` so a job can
  outlive the service) and `ChunkManager.request_path(from, to, max_expansions, max_ms)`
  (defaults 20000 expansions / 32 ms) / `poll_paths()` / `get_pending_paths()` expose it.
  **`PathService::submit` takes the `ThreadPool` per call and never stores it**:
  `clear_editor_chunks()` calls `reset_runtime_state(true)`, which destroys the pool and builds
  a new one, so a retained reference would queue into freed memory on the next request. Job
  coordinates are feet positions; results are **support-block** cells (the block a node stands
  on)
- **Its second consumer is the spawned goblins**: `scripts/goblin.gd` asks for a route to a
  random
  nearby column on an idle timer and walks it **cell by cell** (`nodes`, not the smoothed
  `waypoints`): a waypoint is a long straight leg, and walking a leg as a straight line is how
  the body stepped off ledges and waded through what the plan had gone around. Every node in
  that list was reached by a legal move, which is what makes the walk follow the plan. A route
  that stops being walkable -- a block placed across it, a fall the plan did not expect -- is
  re-planned rather than hopped at, a jump is only taken on a step it can climb, and the plan's
  OWN hops are jumped off the lip of the gap (see the findings below). A rise it PRESSES at has
  its own clock (`_rise_clock`), because `blocked` never counted one: that branch only runs for a
  step that is not climbable at all, so a climb the body believed in read `blocked=0` while it
  jumped at the same block, and the only thing that ever ended the press was a re-plan — which
  answered with the same cell, every few seconds, because the planner has no reason to doubt a move
  it thinks is legal (a live goblin stood at a two-high rise for eleven seconds this way; `nav_trace`,
  2026-11-27 14:59: `next=(-2, 247, 2)` on seventeen samples in a row, `blocked=0` throughout). Forty
  sim ticks at one cell without the gap closing is ten jump flights and no gain, so the route is
  dropped; and because re-asking from the same cell is the same answer, that rise is then REFUSED
  for ten seconds (`RISE_REFUSAL_TICKS`) — a plan that opens with it is dropped rather than pressed
  at again. The body is left with no route at all, which is the point: what it walks next is a
  chase's patience or a stroll's roll, a path it can take instead of the wall it cannot. This is the planner's
  first use as behaviour rather than as a debug picture (both collect results through
  `scripts/nav_routes.gd`,
  since `poll_paths()` empties its queue as it answers)
- **In-game test harness**: the dummy egg spawns the rigid dummy, P plans a route from it to the
  player and
  draws it as translucent red cubes, amber when the plan was cut short by its budget (L toggles
  the raw A* grid path versus the string-pulled waypoints, P again clears); the printed line
  reports found/truncated/timed_out and expansions/columns/cells/locks. Verified headless by
  `probes/probe_path.gd`
  (`Godot --headless --path <project> --script res://probes/probe_path.gd`), which loads the
  real scene, waits for the player to land, plans a 96-block route and asserts it starts on the
  origin column, reaches the goal column, stands every node on a solid block and never teleports

## What the design settled

- **Water is traversable; a liquid with no body in it never is**: a column whose feet stand in
  lava or acid has no route through it, and `is_usable` refuses it whatever the numbers say — a
  cost is not a wall, and the old +0.6 surcharge was read as "wade anyway", so a goblin walked
  into lava. Water is the other case, because a body floats in it and swims out: `Cell::wade`
  (set by `classify_block` from the block's own `fluid_kind`, the one place the two liquids can
  still be told apart) makes a wet column a legal step at `NavCosts::liquid`, a surcharge and
  not a wall. **A wadeable column's surface is the liquid's own TOP**, the level a swimmer rides
  at, and not the floor under it: at the floor every way out of a pond is a climb the planner
  refuses, which is how a goblin came to walk round a pond instead of across it (see the
  findings). Liquid is never HOPPED — you wade it — and a body already in water can route out of
  it, because the check is on the cell entered and not the one left — but only onto a bank at
  the water's own top. The swim's lift (the swim-into-a-wall boost in `PlayerSim`) exists only
  while the body is still in the liquid, so a shore one block ABOVE the water line is a wall it
  presses against and never climbs; `step` refuses that one direction (wet to dry, uphill)
  rather than promising a move no body can make, which is a live goblin bobbing at a staircase
  foot, plan after identical plan, for as long as its anger lasts (`nav_trace`, 2026-11-25
  22:01). Rising INTO water is still allowed: a body strokes up, and there is nothing to jump.
  A plan that still asks for the climb — one made before the water or the ground under it moved —
  is dropped by the goblin itself after `LIP_GIVE_UP_FRAMES` (90) pressed at the lip, because
  the swim zeroes every clock that measures walking and would otherwise never conclude anything.
  Two corrections were needed before that clock could fire at all. It is counted against the
  water's own TOP, not the feet, because the feet BOB: measured in their units the threshold
  was met on every rise of the cycle and cleared on every trough, so `lip` ran 0, 18, 36, 54,
  72 and then 0 again and never reached ninety while one goblin pressed at the same bank for
  eleven seconds (`nav_trace`, 2026-11-27 14:59: `next=(-2, 247, 2)` on seventeen samples in a
  row, `at` swinging 246.2 to 247.5 with `wet` flipping beside it). And the ROUTE goes with
  the give-up: re-asking from the same cell comes back the same way, so the body pressed, gave
  up and pressed again for as long as its anger lasted. With no route the swim falls through
  to the shore it CAN reach (`_shore_from` takes only a bank at or below the water's top),
  gets out there, and the chase plans the rest from dry ground — the path it can take instead
  of the wall it cannot
- **Unknown is never traversable**: a cell in an unresident chunk must sample as
  `CellClass::Unknown`. A raw block query returns AIR for a missing chunk, which would otherwise
  read as walkable emptiness
- **Nodes are feet cells, not positions**: vertical movement is an edge kind (step up / drop /
  hop), which keeps the search 2.5D. `NavView::Column` resolves the topmost standable surface in
  the window plus whether the body (1.8 blocks) fits on it — a standable block above the ground
  is simply a *higher surface*, not an obstruction, so what actually blocks a column is a
  collision the agent cannot stand on sitting in its body's span
- **Column results are memoised per (x, z, hint)**: the downward scan starts just above the
  querying node's feet, so a column read from a low vantage cannot see a taller surface above it
  — the hint is part of the cache key on purpose
- **`NavPath::truncated` means a budget ran out** — the expansion cap, or the optional
  `NavQuery::max_ms` wall clock (`NavStats::time_exhausted` says which). Either way the result
  is a best-effort run to the frontier node nearest the goal that still starts at the agent's
  cell, which is what makes a tight budget usable rather than empty. That is different from
  `found == false, truncated == false`, which means the search exhausted the whole reachable
  graph and there is genuinely no route; `Pathfinder::last_error()` says which. The two are told
  apart by the OPEN SET, not by the count: a cap that lands on exactly the number of expansions
  the graph holds leaves the heap empty, and that is exhaustion. Reporting it as truncated sends a
  caller after a bigger budget for a goal no budget reaches (`tests/test_pathfinder_limits.cpp`)
- **A query's two ends are the columns the agents STAND IN** — `NavView::column` answers "what
  surface
  could an agent arrive on here", which is up to `max_rise` above the hint, and that is what
  makes a
  two-block pillar a surface a neighbour steps up onto (pinned in `tests/test_nav_view.cpp`). It
  is
  the wrong question for an endpoint: a collision two cells over a body's head — the roof of a
  two-block gap — then becomes that column's floor, so a body under a low roof planned from
  standing
  on the roof and a chase's goal resolved on top of the ceiling over a player standing under it.
  `NavView::column_at_feet` is the endpoint question (a surface at or below the feet), the move
  generator keeps `column`, and the memo is keyed by the scan's start cell rather than by the
  hint,
  so the two answers cannot be mistaken for each other
- **A node is expanded from the floor it STANDS ON** — the same question as the endpoints, and
  the same failure without it: `column` looks up to `max_rise` above the hint, so the ceiling of a
  two-block corridor answered as that column's floor and every step out of the node was measured
  from the ceiling. A sealed room planned a route onto its own roof through the ceiling, and a live
  route carried a three-block rise in a single move — which a body answers by walking into a wall
  and hopping at gaps it cannot fit through. The search expands with `column_at_feet` and the move
  generator still asks `column` about the TARGET column, which is the one place a surface above the
  hint is what is wanted
- **A goal is anchored where the target STANDS, not merely in their column** — a target at the
  lip
  of a shelf stands with their centre in the column past it, and that column's floor is the
  plain
  three blocks below. A goal there is a cell an agent can walk to and stand on, so the search
  calls
  the route `found` and the agent parks under its target, with the only way up — a staircase off
  the direct line — never asked for. `Pathfinder::resolve_goal` keeps the aimed cell when its
  column
  holds ground at that level, and otherwise takes the neighbouring column whose surface puts the
  feet in *exactly* the aimed cell; when nothing beside them holds that level (a one-block
  pillar)
  the aimed cell is kept and a best-effort route to it is the answer (pinned in
  `tests/test_pathfinder.cpp`). A WET aimed column is anchored by the same ring for the other
  reason: its floor can hold the right level — a target standing in a one-deep puddle is over the
  plain — but no move enters liquid, so the aimed cell is a goal nothing walks to and the answer
  is "no route" to a target two blocks away. The dry shore beside them is the goal instead
  (`tests/test_pathfinder_limits.cpp`). A column whose floor is there but whose body space is not
  goes there too — a target under a one-block roof, which is also what an aim into a hillside
  resolves to, since the scan walks down from the level it is given and finds the hill's own
  ground with the hill over the body's head. Only a column with no floor at all, and one that is
  not loaded, are refused outright (an unloaded column anchored beside would hide a residency bug
  behind a route to somewhere near the target). The roofed case used to be refused on the spot,
  so the ring was never asked: a fleeing goblin whose run-away point landed uphill answered "goal
  column has no navigable surface" to every ask and stood still while the player killed it
  (`tests/test_pathfinder_limits.cpp`, and `docs/ui.md` has the aim that avoids the case too)
- **A truncated answer is re-asked with twice the budget** — `truncated` means the clock ran out
  before the search reached the goal's level, so the same question under the same clock gets the
  same cut-off answer. `CHASE_BUDGET_LIMIT_MS` in `scripts/goblin.gd` is the ladder (24 → 48 →
  96 →
  192 ms, with the expansion cap scaled to match, since the two are one budget), and a whole
  answer
  resets it. A plan re-asked because the route stopped being walkable keeps the budget its goal
  was
  asked with: it used to be re-asked with the *stroll's* 4,000/8 ms, which is truncated by
  construction for anything a chase would ask
- **...and a STROLL never walks a cut-off one: it re-aims, then gives the goal up** — it has no
  ladder and no patience clock, so a goal the budget could not resolve used to be re-asked
  forever: twenty asks in two seconds, `route=0/7`, `blocked=2`, `wish=(0.00, 0.00)`, a goblin
  that never moved (`nav_trace` 2026-11-27 22:32) — and each round cost it a refused hop first.
  `truncated` is a budget running out rather than a route to the place, so what it carries is only
  how far the search got. The first cut-off therefore re-aims the roll ONCE, at the body's own
  level: a wander aims `STROLL_UP` (4) above its feet so an up-slope column still resolves, and in
  a pit that height is the hillside itself, which makes the goal the top of a wall — a climb the
  stroll's 4,000-expansion/8 ms cannot pay for. The feet's own level is the ground the body is
  already standing on, so the second question is a short walk along it rather than a climb out. A
  second cut-off gives the goal up and sets the failed-retry hold, which is what puts the wander's
  roll back in front of it. A WHOLE answer is walked always, whichever shots it took to get one,
  and a chase or a loot errand is untouched: the chase escalates `CHASE_BUDGET_LIMIT_MS` and the
  errand has its own deadline
- **A chase has a patience** — a best-effort route to a frontier is not an arrival, so three
  seconds with no route under its feet ends it (`CHASE_WAIT_TICKS` in `scripts/goblin.gd`). A
  plan
  still in flight is not a route either: counting only the ticks with nothing walking *and*
  nothing
  planned let a goblin that re-asked every few ticks reset the clock forever, because the answer
  was
  always on its way. Without it a goblin stood at the frontier under a target it could not reach
  — a
  pillar, a sealed pit — for the whole of its anger window (up to forty seconds)
- **A session's plan history is written when the game closes** — `scripts/nav_trace.gd` records
  what build produced it (the engine, a hash and timestamp of `scripts/goblin.gd`, every extension
  library's stamp), a tally of why routes were dropped, every ask and every answer (found,
  truncated, the cell the route ends on, expansions, ms) plus
  a
  1 Hz sample of the player and every goblin, and dumps `user://nav_trace.txt` on exit; how to
  read it is in [debugging.md](debugging.md)
- **All costs live in `NavCosts`**: a level orthogonal step is 1.0, a diagonal √2, climbing
  `step_up` per block, dropping `fall` per block (at most `max_drop` = 3 per move), hopping a
  one-cell gap a flat `jump` = 2.0, liquid +0.6, plus the same descent rate for a hop that lands
  lower. A hop is level or downward and never climbs (its clearance is what carries the body
  across, and a block of rise spends it — see the findings). Every descent pays `fall` a block —
  a hop once took half of it on the theory that the
  jump was carrying the body, which nothing measured, and the heuristic had to be softened to
  match it or the estimate would sit above a hop-down route. Softened to half the descent rate,
  the estimate became loose enough that mid-range queries tripled their expansions (65 -> 257 at
  64 blocks) to stay admissible against a move a route rarely ends on. Charging the full rate in
  `hop()` instead keeps the estimate admissible AND exact, and the expansion counts where they
  were. Measured on live terrain (seed 1337): a 192-block cross-country route is ~2,000
  expansions / ~2.5 ms on one core, so the single-tier A* is enough at mob-relevant ranges
- **A diagonal is a corner the body passes, and it needs room on ONE side of it**: the gate is
  body SPACE at the move's height (`NavView::body_fits`) in at least one of the two beside columns,
  so two blocks meeting at a corner — a squeeze no 0.6-wide body fits through — is the only thing
  refused; a wall beside open ground or beside a hole is a corner a crossing slants past, while the
  body's own footprint stays on the cells it is crossing. A liquid no body may be in refuses it
  outright (`is_usable`'s own rule, asked of the column), so a route may not cut a lava corner;
  water is a crossing. The beside column is resolved once through `view_.column`, which prefetches
  it — sampling cells of a column the search has not visited buys a ranged read per CELL, and the
  number of lock acquisitions a search takes is a pinned budget (`test_chunk_nav_source.cpp`).
  The rule was briefly stricter, requiring the open side to be a column the body could *step into*,
  and that was wrong in the field: the player's own crossing has a hole beside the wall, so it was
  refused and the goal became unreachable — `no route to the goal column` after 4,338 nodes, from a
  body ONE CELL from the player, over and over, while the player watched it stand there
  (`nav_trace` 2026-11-25 23:43). Footing is the walker's question, not the planner's: it slides
  along the open side, refuses a half that walks off a drop, and takes a step down within the
  planner's own `max_drop` (`_slide_around`, `docs/ui.md`). `tests/test_move_generator.cpp` pins the
  squeeze, the wall-beside-a-hole, the lava corner and the same water corner staying a crossing

## Findings from the abandoned causeway (2026-11-25)

The goblin briefly had a way of crossing liquid it could not route over: a block taken out of
the
ground beside it, put down into the channel, one cell every thirty seconds. It was removed — a
mob
that edits the world while it fights you is a mechanic of its own, not a pathfinding fix — but
the
attempt found four things, and the first two are kept as fixes:

- **A liquid column is not a gap, and the planner thought it was** (fixed,
  `MoveGenerator::hop`).
  Liquid supports no body, so a column of it had no body space over it, which is exactly the
  shape of a one-cell gap: a route was found straight across a one-wide channel of lava, the
  goblin jumped, landed in it and stood there while the player watched — and because a route
  existed it never concluded the player was unreachable, so nothing else was ever tried either.
  A hop clears a gap; a hazard is not a gap, and `hop` refuses a middle column that is liquid.
- **A hop the walker could not take was planned anyway, and the walk answered by walking off
  the edge** (fixed, `MoveGenerator::hop` + `scripts/goblin.gd`).
  The planner emits a hop across a one-cell gap and the walker only knew how to press jump at a
  block it was blocked by — and a gap blocks nothing. The body walked off the lip, fell in,
  re-planned the same route and walked off again. The walk jumps off the lip now
  (`_hop_from_lip`: inside 2.5 ticks of the body's own travel, and only above 0.078 blocks a
  tick), with the window measured on the same sim the body walks on: at a goblin's wandering
  pace the arc covers 0.9 blocks while airborne against the 0.7 a one-cell gap needs, so a jump
  pressed 0.2–0.3 blocks out lands on the far side, one pressed 0.45 out lands in the gap, and
  below about 0.07 blocks a tick it never reaches. Two more pieces are why the loop was
  invisible: a waypoint now counts as reached only while STANDING (`_walk`), because mid-arc
  the far cell is within half a block horizontally with the gap still underneath, and counting
  it there ended the route in the air and stopped the wish; and a hop that CLIMBS is not
  planned at all, because the rise spends the clearance on the climb — a hop up one block lands
  against the far wall and drops into the gap at every launch distance at that pace (it clears
  only at a sprint). The rising gap is reached by a step up beside it, so the search goes round.
- **A route's first cell was dropped by a RADIUS, and the walk read the next one as a hop**
  (fixed, `scripts/goblin.gd::_take_route`).
  The cells already stood in were skipped by their distance from the body (0.9 blocks to the
  middle), a radius wide enough for the body's own corner (0.707) — and wide enough for the
  NEIGHBOUR whenever the body stood near the edge of its own cell: 0.05 past the edge, the next
  cell's middle is 0.5 away. That cell went, the route began two columns off, and `_hop_ahead`
  reads a cell two columns off as the planner's jump across a one-cell gap — so the walk stopped
  at the lip to time a jump it must not take, refused it for want of speed, dropped the route,
  and was handed the same one straight back. A punch is what puts a body there: the knockback
  that shoved a goblin 0.0046 blocks past its own edge left it on one coordinate for three
  seconds of the session trace (`blocked=10`, re-planning every 0.2 s) while the player it should
  have been chasing stood four blocks away. Cells are skipped by COLUMN now: exactly the ones it
  already stands in, and nothing else.
- **A pool with no floor under it resolved as `found == false`** (fixed,
  `NavView::compute_column`).
  The scan descends through liquid to the floor, so liquid over empty ground or an unloaded
  chunk
  found nothing and the column read as a hole — the same gap, by a second route. A column now
  keeps
  the liquid it descended through, whether or not it found a floor.
- **`BlockEditor::place_block` refuses a non-air block over a non-air block**, so no caller can
  fill a wet cell — a causeway, a bucket, a quay — unless that rule gains a liquid exception.
  The
  exception was written and then reverted with the feature; a caller that digs a block first and
  finds its placement silently declined is standing beside a hole it made for nothing, which is
  exactly what happened in play before the refusal was understood.
- **A spoil search that reaches below the agent's feet leaves a pit it then walks into.** Taking
  the ground beside it is the tempting block when a bank has no wall to pull down, and it is the
  one that puts the body in a hole; walls first, its own level second, the ground last, and
  never
  its own footing.

Recorded so the next attempt starts from these and not from a debugger.
