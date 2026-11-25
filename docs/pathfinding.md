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
  re-planned rather than hopped at, and a jump is only taken on a step it can climb. This is
  the planner's first use as behaviour rather than as a debug picture (both collect results
  through `scripts/nav_routes.gd`,
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

- **Liquid is never traversable**: a column whose feet stand in water, lava or acid has no
  route through it (`is_usable` rejects it). The costs used to *discourage* liquid (+0.6 a
  move) rather than forbid it, which the planner read as "wade anyway" whenever the dry way
  round was more than half a step longer; a cost is not a wall, and the walking body has no
  swim. A body already standing in liquid can still route out of it: the check is on the cell
  being entered, not the one being left — and it is never HOPPED either, which took a second
  fix: see the findings below
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
  graph and there is genuinely no route; `Pathfinder::last_error()` says which
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
  `tests/test_pathfinder.cpp`)
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
  every ask and every answer (found, truncated, the cell the route ends on, expansions, ms) plus
  a
  1 Hz sample of the player and every goblin, and dumps `user://nav_trace.txt` on exit; how to
  read it is in [debugging.md](debugging.md)
- **All costs live in `NavCosts`**: a level orthogonal step is 1.0, a diagonal √2, climbing
  `step_up` per block, dropping `fall` per block (at most `max_drop` = 3 per move), hopping a
  one-cell gap a flat `jump` = 2.0, liquid +0.6. The A* heuristic (octile over columns plus the
  same vertical terms) is admissible against those move costs. Measured on live terrain (seed
  1337): a 192-block cross-country route is ~2,000 expansions / ~2.3 ms on one core, so the
  single-tier A* is enough at mob-relevant ranges

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
