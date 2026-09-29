# Decisions: what was tried and rejected

Most of this file is a **lookup table**, not a record. Before re-litigating an
idea, find it here. Every entry says what was tried, what the verdict was, and
where the full reasoning is — the numbers are in ARCHITECTURE.md and AGENTS.md,
because that is where they were first written and they should not be copied.

The convention for anything new: if you try something and reject it *with a
measurement*, add a row. A rejection without a number is a preference, and those
do not belong here.

## Rendering

| Tried | Verdict | Why |
|---|---|---|
| 2×2×2 LOD group merging | Replaced | Per-chunk three-tier LOD with 8×8 region merging gave the same draw-call win with better detail |
| Cloud layer (fBm noise) | Removed | Never looked like clouds |
| Lighting presets (Main/Spooky, separate visual sky) | Reverted | Two environments to keep in sync for one toggle |
| Occluder boxes for fully-solid chunks | Reverted | Cost more than the chunks they hid |
| Hashing the opaque mesh only for upload dedup | Replaced | The opaque and liquid surfaces upload together, so a water-only change skipped the whole upload. The hash now covers both, with a separator byte |
| A single view-first mesh heap | Replaced | A saturated queue meant an off-screen chunk never reached the front: "it forgets to mesh stuff until you look at it". Two heaps, with a reserve for distance order |
| Culling a face whose neighbour "fills the cell" | Replaced | Filling a cell is not the same as covering what is behind it: falling water and leaves are full cubes drawn transparent, so both culling paths now also require opaque |
| Scaling the mesh budget up with the backlog | Reverted | Main-thread work: 4× the rebuilds took the phase from ~1.0 ms to ~2.7 ms with no visible gain |
| A dynamic mesh budget scaled by a visible-chunk ratio | Removed | The ratio was never assigned, so the scale was a constant and the lines were dead arithmetic |
| Gauging the LOD transition band cell by cell over the enclosing box | Replaced | ~1.7M iterations per tier per frame to reach ~100k band cells; `for_each_shell_cell` visits the band directly |
| Greedy meshing liquids as boxes | Replaced | One merged quad carries one top height, so every pool rendered as a stepped box. Liquids use the per-corner surface instead |
| Skipping liquids in the LOD emitters by block id | Replaced | Left LOD chunks with no water geometry and put every flowing state in the opaque buffer. The skip is tied to the stride and tested by fluid family |

## Generation and streaming

| Tried | Verdict | Why |
|---|---|---|
| A frustum-priority generation pass | Replaced by the generation chain | The chain reorders the same work by "next to what already exists": +5% throughput, −30% cold start, −35% sweep time, holes still 0 |
| Gating the frustum pass' re-arm on a view change | Reverted | Removed its idle cost exactly, at 8–10% of streaming throughput. A large share of checks is not a large cost |
| One candidate list over the whole world height | Replaced | 82.4% of every pass was vertically impossible entries; the list is per-column bands now |
| Per-chunk sharding for the column residency check | Replaced | A column spanned up to 16 shard locks and queued behind writers. Sharding by column made it one acquisition plus lock-free probes |
| Re-queueing a refused chain offer | Bug | The queue refills faster than any budget drains it: 174k offers became 13.3M in one flight. A refused offer is dropped and returns on its own |
| The vertical arm of the generation chain | Gated **off** | 18.4k offers per flight, zero generations on this terrain. One bool to turn back on when terrain crosses borders upward |
| An inverse-distance kernel for biome blending | Replaced | It front-loaded the transition into the two cells touching the border; a uniform mean over the window is what makes the band widen with the radius |
| Separate generation and mesh thread pools | Reverted | Starved generation throughput ~7× |
| `queue_dirty_chunk()` inside a lock scope | Replaced by `queue_dirty_chunk_fast()` | Re-acquiring a shard already held shared hard-deadlocks on Windows SRW locks |
| Eleven biomes, then Tundra/Taiga/Savanna/StonePlateau | Replaced | The JSON set (plains/hills/ocean) plus blending gave more variety per line of tuning code. Erosion-driven mountains were removed with them |

## Collision, physics and shapes

| Tried | Verdict | Why |
|---|---|---|
| 3D DDA collision | Reverted | Binary-search AABB was faster and simpler |
| A `[feet, feet + step_height)` headroom probe for step-up | Replaced | It always contained the obstruction being stepped onto, so it could never succeed. Step-up raises the full body AABB instead |
| Variant-per-state shapes (128 stair ids) | Replaced | The engine has no neighbour-update pass, and shapes would be wrong after worldgen, a paste or a load. Resolving at mesh time costs zero new ids |
| An outer-corner part that replaces another | Not needed | Because the step is itself a claimed part, a cut is the step switching off with a remnant switching on |
| Holding a shard lock across calls in the planner | Replaced | Every lock is scoped to one call; holding one across calls stalls generation on that shard |

## Fluids

| Tried | Verdict | Why |
|---|---|---|
| The reference's flowing/settled block pair | Replaced | "Recompute; if the cell did not change, stop asking it" needs no second block state |
| Trusting `search_distance` as the horizon a drop can steer from | Bug | Lava reaches three cells but looks four, so a pit just past the third captured a whole pour. The bound is the smaller of the look-ahead and what the cell has left to travel |
| Waking the fluid simulation from a generation worker | Bug | The pending set belongs to the main thread; applying a wake from a worker raced its hash maps and surfaced as heap corruption. Wakes are posted and drained on the main thread |

## Architecture and process

| Tried | Verdict | Why |
|---|---|---|
| An injected light block for the player's dynamic light | Removed | Pushed as a shader uniform instead; the block-in-the-grid design carried locking it did not need |
| `std::vector` for block-name storage | Bug | Reallocation dangled every stored name; a `std::deque` keeps them stable |
| Whole-chunk snapshots with their own file (`chunk_persistence.hpp`) | Replaced (commit `68a725b`) | Sparse edit maps persist only what changed, which is what made cross-version loads possible. Godot file orchestration is `ChunkWorld`'s job; only the pure formats live in `core/` |
| A separate `docs/` split vs one giant architecture file | See [docs/README.md](../README.md) | The architecture file is the current answer, with per-question pages beside it |
