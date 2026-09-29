# Fluid notes

Water that actually flows: the rules, the driver that advances them, and the two
implementations the tests hold to each other.

## Poured water flows, generated water does not

- **Poured water flows, generated water does not.** `fluid_rules.hpp/cpp` answers two questions
  about one cell — what it becomes on its next tick, and which neighbours it pushes fluid into —
  and reads the world only through the `FluidWorld` interface (`fluid_at`, `blocked`), so the
  game backs it with the chunk map (`chunk_fluid.hpp/cpp`, which copies an 11×11×3 window under
  one ranged lock and refuses to judge a cell whose window touches a non-resident chunk) and the
  tests with a grid in a vector. There is no scheduler in the rules on purpose:
  `fluid_sim.hpp/cpp` is the 20 Hz driver (`WorldUpdater::update` advances it, between
  generation and the mesh budgets), and keeping the two apart is what lets the rules be tested
  against a naive "recompute everything until quiet" driver. Three substances declare fluid
  states — `water` (the source, seven runoff depths and the falling column), `lava` (depth 0..3,
  because lava's own traits stop it three cells out) and `acid` (0..7) — and all of them are
  found by SCANNING the registry for blocks that declare a `fluid` state, so a fourth is a JSON
  entry plus one `FluidTraits` row. **worldgen emits `surface_water` for every natural water
  body — sea, lake and cave pool alike — and that block declares no fluid state**, so it never
  ticks, never spills along a shore and never drains. That absence IS the mechanism, and it is
  reused: `surface_lava` and `surface_acid` are the same idea for the other two substances, and
  all three are what a paste places by default (see the block-file bullets), so a pasted lake
  lands as a lake and sits there. They are still liquids to look at and to flow against, which
  is why `mesh_fluid::family_of` names them explicitly and why `BlockType::draws_fluid_surface`
  (and the test that keeps the two agreeing) counts them with the fluid states. It is still a
  wall to the flow (`BlockType::blocks_fluid()`: a liquid the sim does not own blocks it), so a
  poured bucket pools against the sea instead of writing ocean cells away, which would then find
  no supply and delete themselves — holes in the sea. Making oceans live is one `fluid` line in
  `block_definitions.json`, at the cost of runoff creeping along every shore at sea level
- **Lava and acid are the same rules with different numbers, and one rule acid opts out of.**
  `fluids::traits_for(kind)` (`src/fluids/fluid_rules.cpp`) is the only per-substance table:
  decay, `max_depth`, `tick_delay`, `search_distance` (willingness — the rules cap it at the
  cell's own remaining reach, see the drop-search note below), `sources_pair_into_source`. Water
  is the struct's defaults (7 deep, 5 ticks a cell, 4 steps of drop search, pools); lava is
  `{1, 3, 20, 3, true}` — three cells out, a full second between one cell and the next, less
  search, and it pools; acid is `{1, 7, 3, 5, false}` — water's depth on a faster clock, it
  hunts a drop one step further, and it is the one substance that does NOT turn a pair of side
  sources into a spring, so a splash spends itself and drains instead of becoming an endless
  supply. The block half is data (`block_definitions.json` declares lava's five states and
  acid's nine) and `mesh_fluid::family_of` reads the kind off the block, so a lava flow slopes,
  culls against itself and never merges with a body of water. `tests/test_fluid_rules.cpp` pins
  the shape of the table (shallower than water, slower than water, the pooling flag per kind)
  and runs each substance through the same scenes: a lava diamond of radius 3, an acid diamond
  of radius 7, and the two-sources-one-cell-apart scene where water becomes a spring and acid
  does not

## A cell's state is the whole state

- **A cell's state is the whole state**: source (permanent, never thins) / runoff at its
  distance from a source / falling (full strength, depth discarded — falling is ONE state, not
  eight). Nothing in the rules sees a block id or a name; mapping states to blocks is the
  driver's job. A consequence worth knowing before writing that driver: `blocked()` must be
  answered honestly for cells that are not resident, because an unloaded chunk reported as free
  is fluid pouring into space that will be regenerated and overwrite the writes
- **The falling rule runs after dry-up and can overturn it.** A cell in a column has NO side
  supply, so if the dry-up verdict were final a column would delete itself on the way down — and
  worse, the cell above would immediately write it back, so the scene never settles at all.
  `tests/test_fluid_rules.cpp` covers the narrowest case of that directly, and the failure shows
  up as "the sweep driver never settled" (-1) rather than as a wrong value
- **A cell that is drying up pushes nothing.** The reference this was modelled on still wrote a
  full-ish cell downward in that case, which is an artefact of its decision order rather than a
  behaviour; and two different fluids never displace each other (mixing needs a table of what
  each pair makes, so until that exists a cell holding any fluid is not writable)

## Depth, height and the drawn surface

- **Every depth has its own height, and that is data, not code.** A flowing cell's surface drops
  by `(depth + 1) / 9` — the reference's liquid height — so the source is 0.88 tall, depth 1 is
  0.78, and depth 7 is 0.11 (shape `lowered/22` … `lowered/89` in `block_shapes.json`, with
  `top_face_offset` mirroring each). `water_fallen` is a FULL block instead: a column is only
  ever full strength, and it is what makes a waterfall read as a column rather than a stack of
  shallow tiles. The ladder is a player-visible invariant, so it is pinned twice:
  `tests/test_fluid_sim.cpp` checks the C++ default registry (offset per depth, strictly
  increasing, the shape a bottom-anchored column), and `probes/probe_flow.gd` checks the GAME
  registry through `ChunkManager.get_selection_boxes`, which is the only way to catch a shape
  name in `block_definitions.json` that resolves to the wrong height. **That offset is now only
  the cell's NOMINAL surface**, not the height its top face renders at: the drawn surface is per
  corner, and the offset is what the face-visibility test compares a lowered neighbour against
  (see the mesh bullet below). The one thing the reference's renderer does that we still do not:
  it rotates the flowing texture by the flow direction
- **A liquid surface is a quad with four independent corner heights, so liquids are meshed by a
  pass of their own.** (For what a liquid hides around it — nothing, it is transparent — see the
  rendering notes above.) `src/mesh/mesh_fluid.hpp` holds the rule (`passive_fluid_mesh` in
  `mesh_builder_fluid.cpp` is its only caller; the tests drive it directly): each corner of a
  cell takes its height from the 2x2 cells that share it — the cell ABOVE being the same
  substance pins the corner to full height (a column, checked first), a same-substance cell
  contributes its own surface with full-strength cells weighted ten times, anything passable
  contributes a full block of gap, and a solid contributes nothing at all, neither gap nor
  weight, so a corner buried in rock is at full height and a corner with one source and three
  empty cells lands at 0.70. Follow that and a pool is flat at 0.89 in the middle with its RIM
  falling away, which is the whole point of the rule. Two consequences worth knowing before
  touching the mesher: **liquids are excluded from the greedy passes and from the per-AABB
  path** (`MeshBuilder::is_fluid_drawn`), because a merged run carries ONE top height and a
  per-AABB face one per cell, so both can only produce a flat, stepped surface; and a run of
  FLAT cells at one height still merges back into a single quad, which is what keeps an open sea
  at 32 quads per chunk instead of 1,024 (both pinned by `tests/test_fluid_surface.cpp`, the
  second by meshing a synthetic 17,408-cell sea with same-sea neighbours and asserting the exact
  quad count). `BlockType::greedy_mergeable` still describes the SHAPE — a bottom-anchored
  column — and no longer means the greedy flush is who draws it. At LOD stride > 1 liquids are
  drawn as full cubes by the greedy passes exactly as before, because a corner height is
  meaningless at a 2-block stride

## Waking the simulation

- **A wake-up that lands on a tick already reached is not a duplicate.** The driver's pending
  set keeps the earliest due tick per cell, which is what stops a busy flood thrashing — but a
  tick's writes are applied at the END of it, so a cell that was asked during the tick in
  progress was asked against the world before those writes existed (and if it was not fluid yet,
  it answered "nothing here"). Keeping that spent tick silently DROPS the wake-up, and the
  symptom is a flood that stops after one ring while every rule test still passes: `schedule()`
  therefore replaces a due <= `current_tick_`, and `tests/test_fluid_sim.cpp` pins the spread,
  the per-tick delay and the wake-up-on-edit cases that fail if it does not
- **A pending tick and a new wake-up are BOTH owed, and the queue token is what tells them
  apart.** Replacing a reached due tick is right for a cell the tick in progress has already
  asked, but a cell still WAITING its turn in that same tick has an entry in the pending set and
  a token in the queue: it must be ticked (that is the ring advancing) and it must be ticked
  again once the write lands (its own tick read the world before the write existed). So
  `run_tick` honours any token whose tick has been reached, and clears the map entry only when
  the token IS that entry — a map entry due later is the write's wake, left in place. Getting
  this wrong is invisible in the settled world and to every single-axis test: a spreading cell
  wakes +x, -x, +z, -z in that order, so the north/south cells are the ones a same-tick write
  lands on, and dropping their pending tick made a flood grow one cell per five ticks east/west
  and one per TEN north/south — a rectangle, not a diamond.
  `fluid: a flood front advances at the same rate in every direction` measures all four fronts
  tick by tick and fails on the old behaviour
- **The drop-seeking search is the expensive part of a tick, and it is bounded twice.** Fluid
  spreads only toward the directions nearest a drop, so each tick measures the distance from
  each candidate direction to the nearest drop, at most 4 steps and never doubling back (~1400
  world reads in the worst case: flat ground, no drop in range — constant, never world-sized,
  and pinned by a test that runs the same tick in a 16-wide and a 64-wide world and compares the
  counts). The cheap pass comes first and is what makes a flood affordable: a direction that can
  drop straight off scores 0, which is unbeatable and only reachable without searching, so the
  moment one exists there is nothing to search for
- **A drop only steers the flow if the fluid can still reach it.** `search_distance` says how
  far a substance is willing to look, but the hard bound is what the cell has left to travel:
  `reach_after_spread` (`fluid_rules.cpp`) is the neighbour it is about to spread into plus
  every further cell that still leaves it at or above its thinnest depth, and the horizon is the
  smaller of the two. Those numbers disagree, and the trait is the WRONG one to trust — lava
  reaches three cells but looks three steps from its neighbour, i.e. four cells out, so a pit
  just past the third cell captured the whole pour: lava ran at it, died one cell short, and the
  other three directions stayed dry (a four-cell dead-end arm instead of the 25-cell diamond its
  strength is good for). The same class of bug lives in every kind's thin outer cells, where the
  remaining travel is shorter than the substance's look-ahead. A drop beyond the reach now
  scores as no drop at all, so the cell spreads every way it can instead; a drop inside the
  reach still steers exactly as before, which is what rule 8 is for.
  `fluid: a drop the fluid cannot reach does not steer the flow` pins both halves against the
  same floor with the hole moved one cell out, and fails on the uncapped horizon. Note this is
  not uniformity for its own sake: the cap is per-cell, so the cells that close the distance to
  a far drop still turn into it once it is within their own reach

## The driver, and the two the tests hold together

- **The driver is single-threaded, and a wake from a worker is POSTED, not applied.** The
  pending set, the queue and the tick counter all belong to the main thread, and only that
  thread may call `schedule`, `tick`, `advance` or the `run_*` drivers (`FluidSim`'s header
  states the contract). Wakes that come from a worker go through `post_block_changed`, which
  queues a coordinate and nothing else, and the main thread applies them in `drain_posted()` —
  called at the top of `advance()` and of the `run_*` drivers. The one that reaches for this is
  a chunk being GENERATED: `apply_edit_map_to_chunk` runs on the thread pool and every fluid
  cell in the map wakes the sim, so `ChunkWorld` carries two listeners (`set_edit_listener` for
  the main-thread writers, `set_worker_edit_listener` for that path) and the controller points
  the second one at `WorldUpdater::post_block_edit`. Calling the applying one from a worker
  raced the main thread's hash maps and surfaced as heap corruption (`0xC0000374`) on a worker —
  an intermittent startup crash, since it needs a saved edit containing fluid to stream in while
  something is flowing. Two details are load-bearing: the drain happens BEFORE `advance()`'s
  idle early-return, because a chunk arriving while nothing is flowing would otherwise post into
  silence and leave the flood asleep until an unrelated edit woke it; and `clear()` drops the
  inbox, or a world reload applies wakes for the world that just left. The deferral is invisible
  — a wake is already scheduled some ticks ahead — and `tests/test_fluid_sim.cpp` pins the
  deferral, the idle case, the clear, and 4 threads × 500 posts losing nothing
- **Two drivers, and they must agree.** `tests/fluid_test_world.hpp` runs a scene through both a
  slow sweep (recompute every cell every round until quiet) and the queued driver the game will
  use (a work list seeded from the fluid that exists, re-seeded by whatever each step touched),
  then compares the worlds cell by cell. The interesting failure is the two disagreeing, which
  means the queue missed a wake-up — verified by removing the vertical wake-ups and watching
  that comparison fail. Fluid created on top of fluid is the case that needs them: the cell
  BELOW has to notice what is above it (it becomes falling) and the cell ABOVE has to notice its
  exit is occupied (it fans out instead of pouring)
