# Far-field LOD: the two modes

Status: **both modes exist.** The voxel tiers are the default; the seed-grid mode
is built, off by default, and switched on by the settings row `Far Mode`. What it
ships as is stages 1-3 below (the sampler, the tiles, the look as far as baked
face shading, real block textures and an opaque water plane); the bevels, the fade
band, edit awareness and the tile cache are **not** built, and the sections that
describe them say so.

The measured cost, from `tools/lod_sample_bench.cpp` on this terrain: a column
costs **~0.58 ms** to sample, and **~0.56 ms of that is the per-call blended biome
amplification** (`blend_amplification`) — one call that every public column or
density query re-derives. That single number is why the mode's spacing doubles
with distance, why tiles are sampled on worker threads under a budget, and why the
density march described below is written, tested and unused: eight density queries
at that price cost six times what one rigorous surface search does. A 256-block
tile is 81 columns at a 32-block spacing (~47 ms) and 9 at 128 (~5 ms).

Farlands' current LOD answers "how do I show more world than I can generate" by
drawing the world it has already generated, more coarsely. The request is for a
second mode that answers it the other way: **draw the geometry the world seed
implies, generating nothing**. The two are not variations of one system — one
reduces detail inside a chunk that exists, the other never makes the chunk — so
they are two modes behind a toggle rather than one mode with a knob, and this page
keeps them apart on purpose: what exists first, honestly (including the parts of
the description that are not what the code does), then the design, then the order
to build it in.

## What the current mode actually does

It is not "merge a 2×2×2 or 4×4×4 block area into one block". No blocks are
combined and no coarser voxel grid is built; three separate mechanisms all read as
"less detail further away":

- **The meshing stride.** `MeshBuilder::set_detail_level` (`src/mesh/mesh_builder.cpp`)
  turns the level into the largest power of two ≤ 1/level, capped at 8 — so
  1.0 / 0.5 / 0.25 / 0.125 mean a stride of 1 / 2 / 4 / 8 cells per axis. The
  builder still walks the real chunk; it samples every Nth cell, so a distant
  slope is emitted as quads several blocks wide instead of one per block.
- **The tier policy.** `MeshManager::compute_chunk_detail_level`
  (`src/mesh/mesh_manager_lifecycle.cpp`) picks the level by Chebyshev distance
  from the player's chunk — 1.0 to `lod_distance`, `lod_detail_level` to
  `far_lod_distance`, `far_lod_detail_level` beyond — and each tier keeps a +1
  ring at its inner edge (the skirt) so a reduced tier always borders one step
  finer and no T-junction opens.
- **The far tier's granularity.** Below level 1, the region mesh in
  `src/mesh/mesh_manager_far.cpp` concatenates its child chunks' cached meshes
  into one 8×8-chunk instance (`kFarRegionSizeXZ`), so the draw-call count is set
  by the region rather than by the chunk.

What none of that changes is the bound that matters: **every visible chunk is a
real generated chunk.** It lives in `ChunkMap`, it was meshed and lit, and it
holds block storage. LOD makes each of those chunks cheaper to *draw*; it does not
make one fewer of them. Render distance is therefore still bounded by generation
throughput and chunk memory — the wall the seed-grid mode exists to remove.

## The second mode in one sentence

Sample the world generator on a world-aligned grid and build the visible surface
from those samples, so terrain beyond the render distance costs a function call
per sample instead of a generated chunk — no chunk allocation, no block storage,
no lighting pass, no save file.

## What "meshy, not voxelly" means concretely

The look the request describes is not a smaller cube, it is the absence of cubes.
The pieces that produce it, in the order they matter visually:

- **A continuous surface.** One quad per grid cell with its four corner heights
  taken from the sampler, so a hillside is a slanted quad rather than a staircase
  of 2-block steps. Steepness comes from the quad's own normal, not from geometry.
- **Bevels and corner blending.** Insets on the cell edges whose neighbouring
  corner is lower, which is what makes the silhouette read as carved rather than
  gridded. This is the largest single visual difference from the stride mode.
- **Baked face shading.** Four constants by orientation (top brightest, then
  north/south, then east/west, bottom darkest) multiplied into the vertex colour.
  No light sampling and no propagation: at these distances the sun angle is all
  that is legible anyway.
- **Per-vertex colour from the sampler.** The column's biome decides the surface
  colour, blended by height and steepness so a mountain does not end at a colour
  boundary. The colours come from the same block definitions the near world draws
  with, not from a second palette — a far field whose greens disagree with the
  near world is worse than no far field.
- **Water as a plane.** One surface at the column's water level with a depth tint,
  which is all an ocean needs at two kilometres and costs no fluid simulation.
- **Vegetation as impostors.** The sampler's biome decides a coarse cross-quad or
  a tinted billboard per forest cell, not a tree.
- **A fade band at the seam.** Alpha blending into the voxel world over the last
  few chunks, so the mode's edge is a gradient rather than a cliff.

## The sampler

Two things already exist; one does not.

What exists, public on `ChunkGenerator` (`src/worldgen/chunk_generator.hpp`):
`get_biome`, `get_terrain_height`, `quick_height_estimate`, `find_surface_y`, and
the four-argument `sample_terrain_density(world_x, world_y, world_z, column)`
(">0 solid, <=0 air"). `find_surface_y` (`src/worldgen/chunk_generator_terrain.cpp`)
is the real topmost air-to-solid transition, which is exactly the answer a grid
wants — but it reaches it by scanning the whole surface band one block at a time,
two density evaluations per block. Fine for a single column (spawn, structure
placement); wrong for a grid, where 2048² at a 4-block spacing is 262,144 columns.

What the mode needs instead is a **march**: start above the band, step down in
coarse increments to the first solid sample, then bisect between the last air and
the first solid. The band is bounded (`SURFACE_BAND_OUTER`, `density_margin()`),
so the coarse step is a handful of evaluations and the refine about five more.
That is the core of the whole mode, and it is a few dozen lines around the density
call that already exists.

The gap is that the per-column work is not callable as one unit. Climate, the
blended amplification and the weirdness mask are recomputed by every public
density call, while `ColumnSample` is a public type whose producer
(`sample_column`) is private. So the first code change is small and boring: **a
public column-sampling entry point that returns the column together with its
shaped surface**, so a grid sampler evaluates the climate-blend-weirdness stack
once per column and then marches. Not a new class, and no change to what
`chunk_generator.cpp` itself does.

Sampling runs off the main thread on per-worker generators, exactly as the column
prefetch already does (`src/world/column_prefetch.hpp`): one generator per worker
configured from a copy published under a config epoch, answers refused under a
retired epoch. That file is the template, and the discipline it encodes is worth
copying rather than rediscovering.

## Where it plugs in

- **A new subsystem under `src/lod/`, pure and Godot-free** — a sampler, a tile
  builder and a manager that owns tiles and their build state. Keeping the
  arithmetic out of the Godot half is what lets the suite test it the way
  `src/pathfinding/` and `src/schematic/` are tested, and it keeps the engine side
  in `src/mesh/`.
- **Tiles, not chunks.** A tile is a fixed square of blocks (512 is a reasonable
  start) sampled at a spacing chosen by distance — 4 near the seam, then 8, 16,
  32 — so vertex count and draw calls stay bounded as the radius grows. Spacing
  that does not scale with distance is the one design mistake that would make this
  mode cost more than the chunks it replaces.
- **The build path is the far-region path**, which is already asynchronous,
  debounced and epoch-gated (`process_far_region_queue` in
  `src/mesh/mesh_manager_far.cpp` is the worked example): a worker produces packed
  vertices and indices, the main thread uploads nearest first.
- **Drawing** is one instance per tile in its own material, reusing the terrain
  shader with the mode's flags. The voxel chunks and far regions inside the mode's
  radius are hidden by the visibility swap the far regions already perform
  (`sync_far_region_members_visibility`).

## The toggle

One cycled button in the settings menu's **Terrain** group, beside `LOD Distance`:
"Far Mode: Off / Seed Grid". That group is where the four LOD rows already live
(`scripts/settings_menu.gd`), and a button row is the pattern the menu already
uses for enumerated choices. Two companions, both sliders: `Far Grid Spacing`
(blocks per sample at the innermost tier, 8–128) and `Far Reach` — how much
further than the level ladder the horizon goes, in blocks of the OUTERMOST spacing
level (`lod_grid_outer_rings`, 256 blocks a ring, 0 = the ladder's own reach, up to
100 rings = 25.6 km).

`Far Grid Spacing` **snaps**, and it has to: a tile is a whole number of cells only
when the spacing divides it, and the tiles of one level share their edge nodes
through that division. A spacing that does not divide the tile makes `build_tile_mesh`
refuse the tile — correctly, and silently: the tile comes back empty, the mode counts
it as built, and that level's ground is simply not drawn. Which is what "only 16, 32,
64 and 128 work" was: every other slider step removed a level. `lod_snap_spacing`
snaps a request onto the nearest power of two that divides the tile (ties to the
smaller), the engine applies that to whatever path sets the property, and the row
snaps the drag to the same values and shows the snapped one (`_snap_lod_spacing`), so
the number on screen is the number the geometry is built at.

The ladder that snap feeds has a second bound, and it is the one that made `Far
Grid Spacing` feel like a distance setting: a level's spacing is
`min(base * 2^level, 256)` and the **outermost level is 256 whatever the base**
(`lod_spacing_for_level`). A base of 64 asked for levels of 64 / 128 / 256 / 512, and
the last two came back empty — the builder refuses a spacing that does not divide the
tile, and the mode counts a refused tile as built — so above a base of 32 the outer
bands were ground nobody drew, `Far Reach` moved nothing you could see, and 32, the
coarsest setting that still built its last level, was the one that saw furthest. The
outermost level is pinned because it is not terrain but the START of the horizon:
beyond it lie the reach's own rings, at a whole extra ring being 46,000 tiles of one
quad each at 256-block spacing against 64 quads each at 32. Pinned, "how far can I
see" is one distance and one cost at every spacing, and the base still shapes every
band nearer than the horizon. A refused tile is also now final rather than pending
(`Tile::settled`): a sampler is a pure function of its configuration, so re-asking a
hole every frame only ever spent the build budget — nearest first — on holes.

Default **Off** — the mode is opt-in, and switching back must leave the world
exactly as it was.

The reach also has to be VISIBLE, which is not free: Godot's camera far plane
defaults to 4000 blocks, so the outer rings past that are geometry the mode pays
for and the player never sees. While the mode is on it raises the camera's plane to
its own horizon plus a tile (the outermost ring's far corners), and switching the
mode off puts the plane found before the first raise back — it is the player's
camera, and a plane left out at a far field's horizon is a change to the whole
world's rendering made by a setting that is switched off. Nothing else reads that
value: the plane is never lowered while the mode is on, so shrinking the reach only
costs a wider depth range.

What the reach costs, measured on this terrain (`probes/probe_lod_grid.gd`'s own
settings turned to the top of the slider, 100 rings): **45,369 tiles**, 185,017
columns, 53,564 quads, 321,384 vertices and **4 draw calls** for a 27 km horizon,
and the build side answers about **600 tiles a second** — 6.7 ms a tile, four
columns of ~1.7 ms each, a dozen in flight. That is ~1.5 minutes for the whole
slider, but it is spent nearest first, so the distance you can SEE grows the way
the tile count does: **~9 km in the first 10 seconds, ~16 km by 30**, with the last
stretch of horizon the tail. Two things are knowingly left on the table: each node
is sampled once per tile that touches it (four times over at the outer level,
where a tile is one cell), which a shared node cache would cut to a quarter, and
the tile refresh at that reach costs 7 ms, once per 256 blocks walked.

Reach belongs on the outer level and nowhere else, which is the whole reason it
can be a slider at all: the outermost level samples at one quad per 256-block tile,
so extra horizon costs one quad per tile, while the same 256 blocks at the innermost
spacing is a level-0 tile of 4,096 quads. Growing the reach therefore
moves no tile that already exists — `lod_level_for_distance` keeps every level
inside the outer band exactly where it was — so a drag adds a ring of quads rather
than re-sampling the world, and shrinking it drops that ring again. The mode's own
stats report the horizon it ended up with (`outer_radius`), which is the number to
read rather than the slider's.

Persistence follows the existing render rows: saved under `render/` in
`settings.cfg` (`lod_grid_enabled`, `lod_grid_spacing`, `lod_grid_outer_rings`) and
loaded in the same block, with the mode enabled last so it never comes up sampling
a configuration the rest of the load has not applied yet. These rows are not part
of the settings codec, unlike the lighting ones. The accessors are `ChunkManager`
properties mirroring the `lod_distance` pair
(`src/godot_bindings/chunk_manager_properties.cpp` plus its `BIND_PROP` row), so
probes and presets can drive the mode without the menu.

## Coexistence and the seam

The seed-grid mode **owns only the rings beyond `far_lod_distance`**; everything
inside stays the voxel world, unchanged, streamed and editable. That choice keeps
the risky half of the feature away from the player: generation, lighting,
collision, fluids and the save format all remain the near world's, and the whole
mode can be deleted by deleting one manager.

The seam is where this can fail visibly. The innermost tile's surface must agree
with the voxel ring it meets — both read the same generator, so heights agree by
construction *if* the sampler marches the same density field, which is the reason
not to invent a second height model. The fade band the two modes blend across is
the one place the alpha blend is required rather than decorative.

The inner boundary itself is the part that is easy to get wrong, and it is worth
stating as a rule: **the world's coverage is a DISC and the tiles are a SQUARE
lattice, and the boundary between them is the disc.** It is enforced in the
fragment stage (`shaders/lod_grid.gdshader`) against the radius the world streams
with, not by leaving a hole in the tile set. Two consequences, and both were bugs
first:

- Tiles that **straddle** the disc's edge are built, because they are the only
  geometry that closes the corners of the lattice the disc cannot reach. Dropping
  whole tiles inside the radius — which is what "the mode starts at
  `far_lod_distance`" reads like as code — leaves four wedges, from the world's
  radius out to 1.41× it, that neither the world nor the grid draws. A tile the
  world covers entirely is still skipped, but the test for that is the disc with a
  tile's *farthest* corner (plus half a tile for where in its own tile the player
  is standing), not the tile index.
- Because the clip is a shader, a tile that straddles the edge is built once and
  stays correct while the player walks across it. Clipping by rebuilding tiles as
the player moved would trade the holes for a permanent rebuild churn on the
  innermost ring.

## Edits

The seed cannot reproduce a placed block, and that is the mode's real integration
cost. Three options, in the order they should be attempted:

1. **Keep the mode beyond reach.** It starts past `far_lod_distance`, so an edited
   block and a grid tile are never in the same place. Stage 1 works this way and
   contains no edit code at all.
2. **Sample the edit map.** The sparse edit maps already persist exactly the cells
   that differ from the generator, which is the delta layer this mode's no-files
   design otherwise lacks. A tile queries the cells in its range and displaces
   those corners.
3. **Fall back to voxel meshing** for any tile that contains an edit.

## Budget, priority and persistence

- A per-frame tile budget beside the existing counts in `FrameBudgets`
  (`src/core/frame_budgets.hpp`), with a per-worker cap on tiles in flight — the
  lesson generation paid for (`max_generating_in_flight_per_worker`): an unbounded
  in-flight set turns a burst into a backlog that lands as one stall. The count of
  builds in flight is *taken from the tiles* (`schedule_builds`), never kept
  beside the queue: a twin counter is a second source of truth, and this one was
  poisoned by `free_all_tiles()` throwing away results that had finished but not
  been drained, after which the ceiling was never satisfied again and the mode
  stopped scheduling tiles for the rest of the session — ~90 of 289 tiles missing,
  with nothing failing anywhere. A build that has not come back in
  `kBuildTimeoutSeconds` is re-asked, so a task lost to a pool shutdown is a retry
  rather than a hole.
- Distance-ordered always, so the tiles nearest the seam exist first and an
  unfinished grid reads as a distant gap rather than a missing horizon.
- Instrumentation from the first stage rather than the last: samples per second,
  tile build ms, upload ms, live tiles and estimated bytes, reported through
  `src/render/world_render_stats.hpp` so the mode appears in the perf report
  beside every other phase.
- Persistence is optional and later. The mode is a pure function of the seed and
  the configuration, so a cache is a speed-up and never a correctness
  requirement — and it must be keyed by the same config epoch the prefetch uses,
  so a configuration change cannot serve stale geometry.

## The staged plan

**Done.** 1. **Measure the sampler.** A program under `tools/` — the shape of
   `tools/heightmap_voxel.cpp` and `tools/memory_estimate.cpp` — that links the
   generator and nothing else: sample a 2048² area at 8-, 4- and 2-block spacing
   and report ms, samples/s and peak RSS per spacing, then set that against
   generating the same area as chunks (`probes/probe_stream_bench.gd` already
   measures the chunk side). **No engine change.** This fixes the spacing, the
   tile size, and whether the mode is worth building at all.
**Done.** 2. **Sampler, tiles, toggle — off by default.** Flat biome colours, no
   water, no fade, no edits: the mode draws, the toggle switches, the voxel far tier
   is hidden inside it, and the seam is deliberately a visible edge.
**Partly done.** 3. **Make it look like terrain.** Baked face shading, the block
textures on their own layer and the water plane shipped with stage 2; the bevels,
corner blending, tree impostors and the fade band are still open, so the seam is
still a hard edge and the nearest ring still reads as quads.
4. **Edits**, by whichever of the three options stages 2 and 3 leave open.
5. **Cache, budgets and instrumentation**, then a probe that keeps the mode
   honest the way the streaming probes keep generation honest.

## What shipped

The mode is `src/lod/lod_grid.cpp` (tiles, budget, scheduling), `lod_grid_upload.cpp`
(the per-level merge and the one instance each level gets), `lod_grid_build.cpp`
(the worker's sampler), `lod_grid_camera.cpp` (the scenario and the far plane that has
to reach the horizon) and the pure geometry in `lod_surface.cpp` / `lod_march.hpp`,
with `shaders/lod_grid.gdshader` and `materials/lod_grid_material.tres` to draw it.
A tile is 256 blocks a side and is sampled at 32, 64, 128 or 256 blocks by distance
level, two tile rings per level, four levels — so a tile is 64, 16, 4 or 1 quad
across and the geometry per tile falls as fast as the tile count rises. The surface
is textured with the world's own texture array on the surface block's top-layer,
anchored in world block coordinates, so a far quad takes the mip level that is that
block's average colour and the far field wears the near world's palette by
construction rather than by a second table.

**The bug worth writing down: "tiles built" is not "tiles seen".** The tile
geometry is in world coordinates, and the instance was ALSO given the tile's origin
as its transform. Every tile but the first was therefore drawn at double its offset,
and its cull box — rebuilt from the tile index rather than from the mesh — described
neither where it was nor where it went. The result looked like a mode that worked:
tiles built, uploaded and counted, with **exactly one mesh visible** and the rest
culled or fogged out where nobody was looking. Two things came out of it, both now
in the code:

- the instance transform is set **explicitly to identity**, with the reason beside
  it (the vertices are already in the world, so the transform is not a place to put
  the origin), and
- the instance's cull box comes from **the mesh's own bounds** (`TileMesh::min_x`
  and friends) rather than from the tile's index, so a future change of convention
  moves the geometry and the box together instead of silently culling a ring.

`tests/test_lod_surface.cpp` pins the convention (`the mesh is in world coordinates
and says so in its bounds`), and `probes/probe_lod_grid_shot.gd` measures what the
earlier probes could not: the rendered ground as a **radius profile** from a
straight-down camera, off versus on, with each patch's own off-frame colour as the
reference — so a ring that builds but does not draw fails a check on the frame, not
a count of tiles in a dictionary. It is a plan view because a band of screen rows is
a band of ground *distances* that depends on how high the camera happens to be;
reading the profile by radius removes that guess, and the camera's centre is the
point the world's disc is centred on, so the measurement is concentric with the
clip. Two things it needs to be able to see anything at all: the grid's fog
neutralised for the capture (at three kilometres up every pixel of the far field is
beyond the fog's end, so a correct grid and a missing one differ by hundredths),
and the day/night cycle frozen (a drifting sun moves every sky pixel, which is what
an earlier version of this probe was measuring).

**The second bug, and the one the user saw: a pipeline can stop without failing.**
Around ninety of the 289 wanted tiles never arrived in a session, which is a hole
in the ground wherever those tiles were — and `tiles`, `uploads`, `failed` and
`dropped` all read healthy, because the counter that gated the schedule
(`Sink::in_flight`) was decremented only by a finishing task or a drain, while
`free_all_tiles()` — a world reset, a spacing change, a toggle of the setting —
threw away results that had finished and not yet been drained. Each such reset
leaked a few counts; once four had leaked, the ceiling was never satisfied again
and the mode never scheduled another tile for the rest of the session. The fix is
the shape of the fix for all of this class: **the count is taken from the tiles**
(`schedule_builds`), so it cannot disagree with the work that exists, and a build
that has not come back in `kBuildTimeoutSeconds` is re-asked rather than waited on
forever. `probes/probe_lod_grid.gd` now fails if the wanted set is not fully
accounted for, which is the assertion that would have caught it before a player
did.

## Drawing it: one call per spacing level

Draw calls, not vertices, are what a far field costs in Godot, and the first version
spent one per tile — around 130 of them for ten thousand vertices. Every tile shares
one material and one set of uniforms, and its vertices are already in world
coordinates, so merging them into one mesh is an append: each spacing level now has
one instance and one mesh, rebuilt only when one of its tiles arrives or leaves, and
the whole mode costs **one draw call per non-empty level** (four at most) whatever
the tile count. The world-coordinate convention that caused the double-offset bug is
what makes the merge trivial — that is the trade it was for. The tiles keep their own
geometry (`Tile::raw`) and the per-tile mesh is never built, so nothing is duplicated.

Six more things came out of using it, each a seam, a patch or a cost that only shows
in a frame:

- **Cracks along level boundaries.** Two adjacent tiles sampled at different spacings
  agree exactly at the nodes they share and fan apart between them, so a wedge of sky
  ran down every spacing boundary. The finer tile now snaps the shared edge onto its
  coarser neighbour's chord (`build_tile_mesh`'s `neighbour_spacing`), giving up real
  detail along one row of cells to close the crack exactly rather than hiding it under
  a skirt.
- **Slivers along the world's edge.** The world's coverage is whole chunk squares, so
  it can end a few blocks inside the radius it streams with, and a clip exactly on
  that radius left slivers that neither side drew. The clip is now half a chunk
  INSIDE the world's own radius, which turns a possible gap into a guaranteed overlap.
- **...and the radius it is half a chunk inside is the radius the world DRAWS to, not
  the radius it streams at.** The unload pass keeps `kChunkRetentionChunks` (2) of
  hysteresis as loaded chunks, a loaded chunk keeps its mesh, and a chunk of the last
  ring spans a whole chunk past its centre -- so the world's edge is `render_distance +
  2 + 1` chunks, not `render_distance`. The clip was cut from the streaming radius:
  at a render distance of 4 that is a disc of 112 blocks drawn over a world reaching
  224, and `probe_lod_grid_clip.gd` measures what that costs — the far field painted
  **45-47% of the frame** from the player's eye, which fell to 15-25% once the disc was
  cut from the drawn edge (`world_drawn_radius_blocks`). What it does NOT fix is the
  pixel fight: `probe_lod_grid_seam.gd` renders both radii on one camera and the spike
  rate is the same at each (0.1624 against 0.1621 at its worst heading), including on
  headings where the two frames are byte for byte identical because the far field has
  no pixels inside the old disc at all. The drawn-edge number stands on the coverage it
  removes, and the fight that remains is the far field's own rendering at a grazing
  angle rather than chunks racing it.
- **Fog that disagreed at the border.** The world's fog was tuned to the loaded
  radius, so with the mode on the loaded chunks faded out at their border while the
  field continuing them did not — a bright ring. `set_lod_grid_fog_active` puts the
  world on the far mode's fog range for as long as the mode is on.
- **A shoreline lost its beach — the water and the land stopped meeting.** The
  floor under a deep ocean is skipped, because an opaque water quad is drawn over
  it and nobody can see through both. "Deep" was decided from the cell's DEEPEST
  corner, and a cell that steps from a deep floor up onto the land has one: its
  terrain quad was skipped too, taking the beach with it, so the water sheet end
  at the cell's edge while the land behind it rose above that sheet. Two surfaces
  stopping at the same line at different heights have nothing bridging the step,
  so a band of rays just above the waterline crossed the whole tile and the frame
  showed the inside of the hill through the gap the user reported. `tests/
  test_lod_surface_shore.cpp` pins it in the frame's own terms: rays at the heights
  between the shelf and the land must be STOPPED by the surface rising out of the
  water (15 of them crossed the tile before the fix), with rays above the land as
  the control that a ray which hits nothing is still measurable.
- **...and the light blue over the far field's own land: every corner wet, or no water at
  all.** The fix above answered the gap by drawing the whole cell as terrain AND the whole
  cell as a flat sheet, which is two surfaces over the same ground. That is the shape the
  mode carried into the next report — "a light blue texture fighting with a grass texture
  or sand texture on land, but it doesn't fight with water textures" — and the description
  is exact: the light blue is the sheet's own `water_color`, the ground it covers is the
  mode's own grass and sand, and over open water the sheet has nothing to disagree with.
  `probe_lod_grid_overlap.gd` measured it two ways. Rendering the two surfaces one at a
  time and intersecting the masks gave **0.26-0.33 of the far field's drawn pixels covered
  by both**, and the pixels that are light blue with land within a pixel are **99.9-100%
  the sheet's own fragments**, not land tinted blue. Lifting the sheet's fragments **two
  centimetres** moved 3-14 pixels of ~250,000, so this was never a depth fight: the two
  surfaces were simply both there, the land drawn and the sheet laid over it.

  The cause is the sampler's resolution. A cell's four corners are samples a cell apart
  joined by straight lines, so a cell that straddles a coast has an underwater corner and
  a dry one and its surface crosses the water level somewhere inside it — but WHERE it
  crosses is a guess, and on gentle ground the guess is wrong by most of a cell. A sheet
  drawn from that guess is water over ground the samples call land, and cutting the cell at
  the crossing only narrows the guess: the waterline is still wherever a straight line
  between two samples hundreds of blocks apart crosses sea level.

  The first answer to that was **every corner wet, or no water at all**: a cell drew its
  sheet only when all four of its samples were under their own water, and any cell with a
  land sample on it was drawn as land, whole. A far cell was then either water or land and
  the two could never be two surfaces over one pixel. It cost the coast's last cell — the
  sea ended at the last wholly wet cell — and that is what the next report was:

  **...and then gaps between the land and the water, with sand near it speckled light
  blue.** Both halves are the same sized mistake. A cell here is **128 to 256 blocks**
  wide, so "the last wholly wet cell" is up to a quarter of a kilometre of sea replaced by
  beach at every coast, and the cells *before* it are all water: a coastline came back as
  a strip of dry-looking ground followed by one or two pixels of sheet, cell by cell,
  which is a blue speckle through the sand rather than a shoreline. The `is_water` flag
  the shader read had been fixed one report earlier (it was the low bit of a packed
  attribute, and it was set on the field's own terrain), so what was left in the frame
  was this alternation and no tint where it did not belong.

  A cell now draws **the sheet over the whole of itself** whenever any corner is under
  the water, and **its ground only where the ground stands above that water** (the two
  triangles of the cell's top face, each cut by the water plane; a dry cell keeps both
  whole and an all-wet cell loses them both). Where the ground is above the sheet the
  ground is in front and depth drops the sheet behind it, and where the ground is under
  the sheet the sheet is the surface that shows — so the shoreline inside the cell is the
  ground's own crossing and nothing has to guess where it is. A cell no longer alternates
  between two kinds of surface either: the sheet is there whether the cell is all water or
  straddling, and the coast is one continuous (if cell-resolution) line.

  Cutting the ground away is half the rule rather than a tidy-up: a sea floor one block
  under the sheet, kilometres out, is two surfaces within one block of each other, and a
  pair that close is what leaves sand speckled through the water a pixel at a time —
  picked by the depth buffer's own rounding at that range rather than by either surface.
  Clipping removes the pair instead of testing how close the buffer can get.

  `tests/test_lod_surface_shore.cpp` pins all of it on the geometry. On its fixture — two
  columns of ocean, one straddling cell and five of beach, with the ramp crossing the
  water at x = 89.6 — it checks that the sheet reaches the far side of the straddling
  cell, that **no ground vertex is ever below the water level**, that the ground's own
  edge is that crossing, and then casts a ray straight down at every sample point of the
  tile: **no point is missed, no point is hit by two surfaces of one kind, and the
  topmost surface changes at x = 89.6 and nowhere else** — the counts are what a straight
  shoreline at the crossing predicts. A count of quads cannot see any of that: it stays
  constant while a cell covers its ground twice, or not at all.

  What the change measures out in the field, on the same probe and the same placed camera
  as the runs above: **51,121 quads** against 45,420 (the sheets the straddling cells now
  draw), **12,255 tiles carrying water** against 6,506, and **every water vertex still at
  the world's own sea level** (73,542 vertices, min = max = 200.0, `probe_lod_grid_water.gd`).
  `probe_lod_grid_overlap.gd` reads 0 pixels of the loaded world's land painted blue, with
  the sheet on 0.361 of the frame and the land on 0.446.
- **A patchwork instead of a slope — a face table over a normal that moves.** A
  cell's shade came from the near world's table (top 1.0, north/south 0.8, east/west
  0.6) read off the gradient of ONE corner, with a strict tie-break between x and z.
  A gradient a hair either side of either threshold flipped the whole cell by 0.4 —
  40% of the light it receives — so one smooth slope came out light, dark, dark,
  light, and at 256 blocks a cell that is a patchwork across a whole horizon rather
  than a slope. The shade now comes from the cell's own normal (`face_shade`, the mean
  of its two diagonals) with the same squared-component weights
  `shaders/item_lighting.gdshaderinc` uses for a turning body, which reaches the three
  constants exactly on the axes and slides between them. `tests/test_lod_surface.cpp`
  pins both halves: across the old threshold two neighbouring cells differ by under
  0.02, and cell by cell along a ramp no pair of neighbours steps.
- **Blue shallows — the sea floor was wearing the water's own texture.** A flooded
  column's terrain (a floor shallow enough that its quad is kept) was drawn with
  `water_layer`, the LIQUID quad's texture, because the worker's column context was
  filled from the wrong member. Every shallow shelf and coastline therefore wore the
  water texture, which is exactly the blue the far field's shallows had. It takes the
  biome config's underwater surface layer now, the one `refresh_layers` resolves for
  precisely this question.
- **Terrain drawn through terrain — the merge had been hiding a missing depth
  write.** A mesh per tile was ordered for free (the transparent pipeline sorts
  instances back to front) and a mesh that contains every tile is sorted against
  nothing, so the material's own depth write is the only thing left ordering its
  triangles. It had none: the fragment stage wrote `ALPHA = 1.0`, and writing ALPHA
  is what sends a spatial shader down the **transparent pipeline**, whose contract
  is no depth write — `depth_draw_opaque` ("write depth for opaque materials") had
  nothing to write, and the triangles landed in index order, so a far ridge painted
  over the near ridge in front of it. The shader now declares `depth_draw_always`
  and writes no ALPHA, which is also the cheaper pass (opaque, early-Z, no sorting).
  The world's terrain shader writes `ALPHA = 1.0` too and gets away with it only
  because each chunk is its own instance and is sorted. Two pins, one static and one
  measured: `tests/test_lod_shader_depth.cpp` asserts the shader text (the
  unconditional depth write, `cull_disabled`, and no ALPHA assignment), and
  `probes/probe_lod_depth.gd` renders two overlapping quads in ONE mesh and reports
  which of them wins the pixels.
- **The fill was dispatching once a frame, and "12 in flight" was read as a ceiling.**
  `schedule_builds` tops the in-flight count up to `max_builds_in_flight` ONCE PER
  FRAME, so the ceiling was also a rate: 12 tiles a frame, 12 x the frame rate, however
  fast the workers were. At the setting's far end that is 45,369 tiles and the reaching
  took forty seconds — the pool's fifteen workers idle for most of it, and a faster
  machine unable to change any of it. The ceiling is 64 now (a queue deeper than the
  workers by enough that none waits for the next frame's dispatch) and the upload
  budget follows it at 256, because the merge is charged once per level per frame
  whether one tile arrived or two hundred. Measured headless, one probe, the same
  100-ring reach: **6 tiles a frame / 467 tiles/s / 97.2 s**, then **31 a frame /
  30.1 s**.
- **...and a level that is still filling was re-merging its whole mesh every frame.**
  The dirty flag means "tiles arrived since the last merge", which during a fill is
  every level on every frame, so four levels were rebuilt and re-uploaded — up to 321k
  vertices each — per frame. Those frames ran at 49 fps with the tiles arriving at 31 a
  frame: the merge was most of what a filling frame cost, which is the "drops to 100 fps
  while it loads" that prompted this. A level now waits `kMergeIntervalFrames` (3)
  between merges, counted down every frame so a settled reach still answers a lone
  arriving tile on the next frame rather than the fourth. 20 Hz at 60 fps is not a rate
  anybody can see on a horizon two kilometres out, and the same reach now fills in
  **20.5 s at 92 fps** (2,214 tiles/s, 24 tiles a frame) — 4.7x the 97.2 s the same
  probe measured before the two changes, and its own assertions fail on that baseline.
  What is left is the sampler and not this arithmetic: ~73,600 columns at ~4 ms each
  across fifteen workers is ~20 s of the 20.5, and each column is walked twice by the
  two public entry points the build calls, so the next lever is one column query that
  answers both.
- **The detail term does not fade itself out, and believing it did is what put a speckle
  on every face.**  The term is a ratio of a sample of the face at the detail scale against the face's own
  average, so the mean cannot drift — and the first version of it claimed the
  ratio also went to 1 past the distance where one repeat is a pixel, which is not what the
  arithmetic does: a FINE sample only reaches the average's mip once a face texel is a pixel,
  which at 8 blocks a repeat is ~8 km out, and over the ~2 km before that the ratio is a
  2x2-texel pattern drawn at one or two pixels a repeat. A repeat that narrow is not texture:
  the sampler answers with whichever mip level its own 2x2 pixel quad landed on, the divisor
  stays put, and the difference is a speckle that crawls when the camera moves — which came
  back from the field as "z fighting on every single face". Measured in
  `probes/probe_lod_grid_zfight.gd`, on one camera and one frame at a time: the term's own
  spike rate is 6-8x the same frame with the term off, the biome blend's contribution at its
  real weight is **0.0000** (the blend was the report's own guess and the frame says it is not
  the one, though its plumbing is fine — driven to full strength it moves everything), and
  anisotropic and trilinear sampling both make the spike rate **worse** (0.0114 against 0.0058),
  so the filter was not the lever either. The fix is the world's own: fade the term by its
  footprint (`fwidth` of the world-coordinate UV, in repeats rather than texels — the same rule
  and the same reason as `shaders/block_noise.gdshaderinc`), which leaves the term exactly as
  authored out to ~1.8 km and takes it away where the screen was doing the drawing. Both
  `tests/test_lod_shader_depth.cpp` and the shader's own comment carry the corrected claim.
- **A biome boundary is a ramp across a cell, not a line along its edge.** Worldgen
  mixes the near world's biomes before it picks a surface material; a far cell is one
  flat quad wearing one texture array layer, so the same boundary arrived as a straight
  line as long as the cell is wide. The weight is a share of a five-node neighbourhood
  (the same ring the occlusion reads), so it decays over a node's reach instead of
  flipping, and it is computed per NODE rather than per cell, so two cells sharing a
  corner carry the same value there and the blend has no seam along the edge between
  them. A cell names the pair it blends between — the layer most of its corners wear and
  the one their neighbourhoods do — and both cells at a boundary name the same pair
  from their own side, which is what makes the two agree. The weight rides in the vertex
  colour's alpha and the second layer above the water flag in `UV2.y`, so no vertex
  attribute had to be added; `tests/test_lod_surface_blend.cpp` pins the arithmetic and
  `tests/test_lod_shader_depth.cpp` the two halves of the packing against each other.

## What this is not

- Not a change to generation, streaming, lighting or the save format: the near
  world stays exactly what it is today.
- Not GPU-driven indirect drawing or a culling-cluster system. The mode is
  ordinary meshes and instances until a measurement says otherwise.
- Not infinite distance: spacing is a knob and the far plane is still a far plane.
- Not derived from the child chunks. The sampler reads the seed, so there is no
  parent/child mesh derivation, no border stitching and no T-junction bookkeeping
  to get wrong. The trade is that edits no longer come along for the ride, which
  is why they have their own section above.
- Not a data-file-driven feature: no new JSON and no new block ids. A mode that
  needs a schema before it draws anything is one nobody can A/B.

## Risks worth naming before building

- **Sample cost.** The march is the whole feature's cost model and it is
  unmeasured. If a column costs a fraction of a generation, the mode is a slower
  route to the same wall.
- **Colour agreement.** The grid must take its colours from the block definitions
  the near world draws with; a second palette is the shortest route to a visible
  two-terrain seam.
- **Geometry volume.** 4-block spacing over 2048 blocks is 262,144 quads — more
  geometry than the entire far field today. Distance-scaled spacing is not an
  optimization here, it is the design.
- **Draw calls.** One instance per tile only works while the tile is large; small
  tiles at a long radius are thousands of instances.
- **The look.** If stage 3 reads as a toy, the honest outcome is to say so and
  keep the voxel tiers — which is why stage 1 is a measurement and stage 3 is a
  screenshot rather than a refactor.
