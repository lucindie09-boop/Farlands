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
- **In-game test harness**: K spawns the rigid dummy, P plans a route from it to the player and
  draws it as translucent red cubes, amber when the plan was cut short by its budget (L toggles
  the raw A* grid path versus the string-pulled waypoints, P again clears); the printed line
  reports found/truncated/timed_out and expansions/columns/cells/locks. Verified headless by
  `probes/probe_path.gd`
  (`Godot --headless --path <project> --script res://probes/probe_path.gd`), which loads the
  real scene, waits for the player to land, plans a 96-block route and asserts it starts on the
  origin column, reaches the goal column, stands every node on a solid block and never teleports

## What the design settled

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
- **All costs live in `NavCosts`**: a level orthogonal step is 1.0, a diagonal √2, climbing
  `step_up` per block, dropping `fall` per block (at most `max_drop` = 3 per move), hopping a
  one-cell gap a flat `jump` = 2.0, liquid +0.6. The A* heuristic (octile over columns plus the
  same vertical terms) is admissible against those move costs. Measured on live terrain (seed
  1337): a 192-block cross-country route is ~2,000 expansions / ~2.3 ms on one core, so the
  single-tier A* is enough at mob-relevant ranges
