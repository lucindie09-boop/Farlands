# Probes: proving claims a unit test cannot make

Most of this engine is testable headlessly — 791 doctest cases cover the mesher,
the resolver, the fluid rules, the planner and the save formats. A probe is for
what is left: the real loaded registry, the real texture array, the real world,
and what actually reaches the screen.

They live in `probes/` (85 `.gd` scripts). **The scripts are repository
content; what they produce is not.** `.gitignore` ignores `probes/*` except
`*.gd`, `*.sh`, `*.tscn` and `*.gdshader*`, so screenshots, crash reports and
generated sheets (hundreds of megabytes) stay scratch while the probes
themselves are cited by path from ARCHITECTURE.md, AGENTS.md and source comments.
If a fresh clone reports those paths as missing, they simply have not been added:

```bash
git add probes/*.gd probes/*.sh probes/*.tscn probes/*.gdshader
```

## The three runners

| Runner | Use it for |
|---|---|
| `probes/run_probe.sh <probe.gd> [timeout]` | Anything that touches the world. It snapshots `user://chunks`, runs the probe, and restores the snapshot however the probe ended |
| `probes/run_probe_shot.sh <probe.gd> [timeout]` | Anything visual. Runs **windowed** (a screenshot from the dummy renderer is blank) and harvests `user://menu_shots`, `paste_shots`, `shader_shots`, `heart_shots`, `body_shots`, `brightness_shots`, `lod_grid_shots` and `lod_depth_shots` into `.freebuff/shots/` (the script's own `SHOTS`, the ignored local playground) |
| `probes/run_crash_probe.sh [timeout]` | The crash reporter only, with the deliberate fault armed. Separate because it kills the process and because the crash folder must be inspected afterwards |

Both world-touching runners **refuse to start while a Godot game process is
running**, and that check is not a nicety: an earlier version of
`run_probe_shot.sh` restored user data with `rm -rf` on the live directory, which
deleted and recreated the player's data underneath a running game. The crash
times lined up with probe windows, which is what a week of "startup crashes,
sometimes" turned out to be. Nothing there writes to `user://` any more.

Three more rules, each of which cost something:

- **A probe that edits the world is writing persisted content.** `place_block`
  goes through the edit map, which autosaves every 5 s, so a probe that places
  blocks changes the world the player plays even if it fails or is killed. Go
  through `run_probe.sh`.
- **A probe that edits the world must read the write back.** A write to a chunk
  with no render data is *queued* as a pending placement and returns without
  changing anything, which reads exactly like a wrong rule. Move to open ground
  and verify before trusting the result.
- **A probe that lasts more than a few frames should be a benchmark.** Pass/fail
  is the wrong shape for "is streaming fast enough"; `probe_stream_bench.gd`
  measures it at a fixed 60 fps instead.

If a probe does litter a world, `probes/clean_flow_probe_litter.gd` is the
one-shot repair (dynamic fluid and floating shelves only).

## Conventions

- Name it `probe_<subject>.gd`, and start it with a `##` header saying what
  question it answers and how to run it — that header is the documentation.
- Print every finding on a line containing `PROBE`, which is what the runner
  greps for alongside real errors.
- Exit non-zero when a check fails: the runner propagates the status, so a probe
  is a command rather than a page of output to read.
- Sibling scripts are the same idea without the world:
  `probes/check_*.gd` asks a pure question, `probes/scan_*.gd` searches the
  world for something, and a `.tscn` or `*_marker.gdshader` beside them is an
  asset a probe loads.

## The probes the docs cite

This is the set referenced by ARCHITECTURE.md, AGENTS.md, README.md or a source
comment, with the claim each one backs:

| Probe | What it proves |
|---|---|
| `probe_lod_grid.gd` | The seed-grid far mode (`docs/lod-modes.md`): the setting's default and round-trip (spacing, rings and the reach), the seam with the loaded world, and the whole pipeline end to end (see below). Headless, so it runs beside a live game |
| `probe_lod_grid_shot.gd` | That the far field fills the rendered GROUND, near and far, in every direction, off versus on, with an off-versus-off sweep as the control (see below) |
| `probe_lod_depth.gd` | That the far mode's material writes DEPTH, which is what keeps its merged mesh from drawing far terrain over near terrain: two overlapping quads in ONE mesh (near first, far second), with the colours from a texture array the probe builds itself, so "who won the pixels" is a red-versus-blue comparison with no threshold to tune (see below) |
| `probe_lod_grid_look.gd` | That the far field LOOKS like the world it continues: the texture detail the shader puts back, measured on the frame the player gets as LOCAL contrast with the mean colour held still, plus whether the engine's own monitor counts the far mesh at all (see below) |
| `probe_lod_grid_fill.gd` | How long the far field takes to arrive at the top of the reach slider, in tiles a frame and seconds — the number `docs/lod-modes.md` records, and the one a per-frame dispatch rate hides (see below) |
| `probe_lod_grid_zfight.gd` | Which of the far field's terms puts a high-frequency speckle on its faces — "z fighting on every single face" — by measuring one camera in four states of the same shader (see below) |
| `probe_lod_grid_seam.gd` | The far field against the loaded world from the player's own EYE, at the player's own settings: the shipped clip radius against the one this build replaced, one number in one line of the shader apart (see below) |
| `probe_lod_grid_clip.gd` | Where the far field is drawn at all, by painting its fragments flat magenta and counting them from the player's eye — the direct question "is it over the loaded world", with the clip radius read back off the material (see below) |
| `probe_lod_grid_overlap.gd` | The far field's own two surfaces at the player's eye: each rendered alone, then painted by surface, then painted by surface with the DEPTH TEST OFF, and the sheet's own fragments lifted two centimetres (see below) |
| `probe_shapes.gd` | The shape registry, the resolver and the JSON together: every shape's boxes against the documented 16ths model, hidden flags, and the live world's `get_selection_boxes_at` for a fence, a pane and a stair |
| `probe_crucible.gd` | The crucible's nine-box model landed, read back from the real registry |
| `probe_flow.gd` | Poured water really flows: a radius-7 diamond whose stored depth equals its distance from the source, it settles, a shaft under it fills |
| `probe_lava_acid.gd` | Lava and acid blocks, buckets and floods through the real use path, plus every frame the animator pushes landing in the array |
| `probe_stream_bench.gd` | Streaming as the player feels it: time from a 2048-block teleport until the 3×3 columns have ground, a 40 s flight's holes, and what standing still costs |
| `probe_start_fill.gd` | The initial fill as three timestamps at a fixed render distance: loaded chunks, no pending mesh work, and no unrendered far chunks; the numbers in `docs/streaming.md`. It also reports the first mesh upload, the first visible instance, the run's actual worker count (`PROBE fill start ... workers=`), and the in-frustum queue's 50/90% drain, keeps the 24 worst frames over 30 ms (by cost, not arrival) with a phase dump each plus a count before and after the settle, reports the worst sampled frame and how many sampled frames exceeded 33.4 ms, excluding the frame that carries a report call, and with `FARLANDS_LIGHT_PROFILE=1` the worker light pass's own wait/hold and block/sky split (`PROBE light profile:`). `PROBE_TIMEOUT_SECS=N` overrides its 90 s internal deadline; `LATCH_FLIGHT=<s>` instead flies the player at 100 blocks/s once the loading latch raises the mesh budget and prints the latch window's own `PROBE latch` line, whose final fields are the end-of-flight pending mesh queue, its reason mask, how many of those entries pass the current frustum, and that in-frustum set split by LOD tier and 32-block distance band, plus what the frame draws for the far tier (no chunk, all air, no geometry, of those fully solid and enclosed by the mesher's buried test, hidden, drawn per-chunk, drawn by an active region; an AABB test), then how many of those potential holes a 27-ray camera pass finds visible through blocks that are actually drawn (occlusion, not a depth buffer), and finally how many of those visible holes project onto at least one unoccluded screen sample and how many unique 4x4 screen cells they cover (an 8x8 grid over each clipped AABB; overlaps merged); `VSYNC_ON=1` keeps the display's vsync, which is off by default |
| `probe_fps_settle.gd` | The post-fill fps settle, second by second: boots `Main.tscn`, then prints that second's frame mean/worst, `Engine.max_fps`, `get_mesh_rebuild_budget()`, the pending-mesh reason mask and the generation/completion counts, and a summary with the first second that met its settle rule and when the loading budget fell back to idle. At 15 workers/RD 32 the split is in `docs/streaming.md`: fill to 1.68 s, loading latch to 3.42 s, then an idle-budget block that is the `BlockIconRenderer` icon pre-render. `SECS` sets the window; `NO_RENDER=1`, `QUIET=1` (+`QUIET_MATCH`) and `AUTOLOAD_OFF=name,...` stop parts of it, and `KILL_AUTOLOAD=name,...` frees an autoload outright (`BlockIconRenderer` removes the plateau; the freed coroutine logs a resume error and leaks two RIDs at exit) |
| `probe_engine_warmup.gd` | The control for the fps settle: a camera, a light and 64 boxes, no `Main.tscn`. It shows the project's 4 ms/frame plateau, which the autoload bisect pins on `BlockIconRenderer` (keeping only that one leaves 4.1-4.2 ms; keeping any other alone leaves 0.37-0.45 ms). `SECS` sets the window, `KILL_AUTOLOADS=1` frees every project autoload and `KEEP_AUTOLOAD=name,...` keeps just those, `NO_ENV=1` drops the default environment. Its twin in `probes/empty_project` (same `[rendering]` settings, no assets or autoloads) is the 0.4 ms floor |
| `probe_fill_memo.gd` | The in-RAM generation memo's effect on the fill: pass 1 fills the memo at the `generate_chunk` boundary, including the cross-chunk spill list; `clear_editor_chunks()` resets the world, and pass 2 re-fills the same seed with every chunk a memo hit and the spill replayed through the same writer closure (`PROBE memo pass1=... pass1_load=... pass1_end=... pass2=... ratio=... ratio_end=...`). At 3 workers on 4 cores the faithful hit is **10.06 s load / 10.32 s end** against **18.10 s**; a memo-off second pass is **20.63 s** (slower, not warm state). The 2x2 spans **11.82 s** (2.46/64) to **10.06 s** (1.44/192); 5 workers **11.12 -> 5.84 s**, 15 workers warm **4.69 -> 3.05 s** (the decider in `docs/terrain-cache-plan.md`). Set `APPDATA` to an empty directory for a no-edit world (the probe prints `user_dir`); 3 workers there is **16.04 -> 8.05/8.31 s**. The memo and both admission overrides are measurement-only; unset is the shipping behavior. `FARLANDS_GEN_MEMO` disables on `0`/`false`/`no`/`off` (full string, case-insensitive). The 20.63 s control ran unset; the only invalid arm found was a discarded `FARLANDS_GEN_MEMO=0` run at 10.08 s. The probe prints per-class counters split into `mesh_first`, `mesh_remesh` and `region_upload` (plus generation, light, far and prefetch) and reset diagnostics before and after the clear; the split run and its within-run comparison are in `docs/terrain-cache-plan.md`. A true memo-off second pass measured **17.87 -> 20.39 s** in the earlier machine state; before the clear the queues were 1 vegetation chunk / 22 placements and 0 cross-remesh, and all were 0 after |
| `start_fill_ab.py` | Five AB/BA pairs of `probe_start_fill.gd` at RD 32, normal against `NO_RENDER=1` (or, with `--latch --flight`, the 30 s latch cap against no cap during a fast flight; with `--hold`, the initial mesh hold on against `FARLANDS_HOLD_MESH_UNTIL_LOADED=0`; with `--hold-partial N`, both hold and A is `FARLANDS_HOLD_MESH_PARTIAL=0` against B's N nearest rebuilds/uploads a frame), with the arm order alternating each pair so session drift cannot favor one arm; prints per-arm medians/ranges and paired differences, and keeps every log plus `results.csv`. `--cores N` pins the harness and its children to N logical processors (Windows affinity) and sets the pool to N-1 workers unless `--workers` overrides it, `--rd N` changes the render distance, `--fill-trace` prints loaded/generating/completed/pending, budget, fill-settled and far-tail counters once a second, `--vsync` enables the display's vsync for the constrained-hardware arm, `--cache` runs arm A with `FARLANDS_TERRAIN_CACHE=1` and wipes/rebuilds the world's cache before every pair (the cache counters land in `results.csv`), and `--tail-budget-ms N` caps the three post-load clocks at N ms (the shipping 1000 ms is effectively open), and `--final-install-ms N` makes both arms render and A/B the final-wave completion budget (A 0.75 ms, B N ms; shipping is 2 ms, with `FILL_TAIL_TRACE=1` keeping the probe's per-frame ring for the mechanism check). The shipping fill holds mesh work until the fill settles or `FARLANDS_HOLD_MESH_MAX_SEC` (2 s), but allows two nearest rebuilds/uploads a frame through (`hold_mesh_partial = 2`; `FARLANDS_HOLD_MESH_PARTIAL=N` overrides, zero is the all-or-nothing arm), then drains at 512 uploads; it is enabled only at 12+ workers (`FARLANDS_HOLD_MESH_MIN_WORKERS` overrides; an explicit `FARLANDS_HOLD_MESH_UNTIL_LOADED=1` forces it for an A/B), `FARLANDS_MESH_UPLOADS_LOADING=N` overrides the cap, and the results are in `docs/streaming.md` |
| `probe_icons.gd` | The block icon cache after the batched pre-render: how many icons exist and, for a sample of full, shaped and transparent blocks, each icon's hash, size, alpha coverage and opaque bounding box -- the fingerprint that showed the batched render keeps the per-icon look (174 icons, all 192x192). Waits up to 15 s for the cache and prints `PROBE icon cached=...` |
| `probe_launch.gd` | How long a launch takes, in wall clock: engine uptime at script start (engine, project load and the autoloads' `_ready`), then it boots `Boot.tscn` and marks the frame the title appears on, the frame the world (`Main.tscn`, loaded on a thread behind the title) is swapped in behind it, every second's frame mean/worst until `SECS` (6 by default), and whether the cloud layers attached. The title line reports `managers=`: how many of the two deferred autoloads (`SkinManager`, `BlockManager`) exist then -- 0 at the title, 2 by the world. `EMPTY=1` adds a bare node instead of the boot scene (the floor) and `KILL_AUTOLOAD=name,...` frees autoloads before their `_ready`. The split and both boot rewrites are in `docs/streaming.md` |
| `probe_godrays.gd` | Measures the god-ray overlay at 1080p with the sun on screen: median frame time per tap count against the overlay hidden, and a brightness A/B (rays on minus off) that fails if the pass adds no light. Run windowed. |
| `probe_settings_quality.gd` | Builds the Sky page's rows and checks the God Ray Quality slider (25-200%, default 100) exists with the right range, then round-trips a 150% setting through the Render `FR` code. |
| `probe_water_wake.gd` | The bounded generated-water wake at a synthetic shoreline: builds a stone shelf and a 7x7 pond of `surface_water` with the wake off, then places one stone against its edge with it on and reports the conversion, the flood and the frame cost. `PROBE wake dry_edits` times 100 ordinary edits at radius 0 and 2 (the pre-check's cost), `PROBE wake pond` the static pond, and `PROBE wake` the trigger, the live cells in the radius, the settle time, the frames, the mean/worst frame and the fluid queue. Restores nothing itself: run it through `probes/run_probe.sh`. The numbers are in `docs/fluids.md`. |
| `probe_tool_loader.gd` | The deferred debug tools: presses `toggle_chunk_borders` through `Input.parse_input_event` and opens the lab and the pose clone by name (`open_tool`, the path the Tools page and the dummy egg take), checking each spawns exactly one node, in the tree, carrying its own script -- and that a second ask spawns nothing. Exits non-zero on a failure |
| `probe_cloud_boot.gd` | Each stage of the cloud layer's startup generation, timed: `_cloud_values` (the 512x512 mask and the 256x256 unique tile it duplicates), count, threshold sort, mask, texture, greedy mesh and its vertex/index counts, and a hash of every stage. The hashes are the identity check that the threaded, deduplicated rewrite kept the same clouds (`values=2413463503 mask=1239665756 verts=3025404319`) |
| `probe_scene_load.gd` | What the ~0.6 s `Main.tscn` load is made of: every `ext_resource` in the scene, loaded and timed on its own, biggest first, then the scene again from the cache and its instantiate. At the last run the 21 parts total 600-607 ms and `settings_menu.gd` alone is 214 ms of it (a 4,395-line script), which is why the boot's remaining scene-load cost is a script-size problem, not an import one |
| `probe_gen_stats.gd` | What the generation sweep spends its per-frame check budget on, per rejection reason, plus the band-size histogram and the install classification that prices the ±32-block pad. Prints per-phase wall time, chunks/s and the fill line; takes `RD`, `SQUISH` and `VEG` (see below) |
| `probe_frustum.gd` | The plane convention: Godot's frustum normals point outward, ours assume inward, and the engine's own test agrees |
| `probe_bend_cull.gd` | The renderer, not the geometry: primitives and draw calls with the culling compensation off and on |
| `probe_bend_geo.gd` | World Bend's displacement in pixels, against the include's own arithmetic re-run in GDScript |
| `probe_bend_compile.gd` | The world-effect includes survive the compiler and their uniforms reach the real materials |
| `probe_horizon_geo.gd` | The Horizon Curve's drop, in pixels, against its arithmetic |
| `probe_crt_look.gd` | CRT Screen's defaults, looked at over a frozen game frame |
| `probe_crt_unit.gd` | The CRT pass on a flat field: fillet, band count/period/depth, grille pitch, and the normalised gather |
| `probe_shaders_shot.gd` | The Shaders page and the passes: registry vs layers, layering vs menu, a row reaching its uniform, the `FS-` code round trip |
| `probe_anime_shot.gd` | What makes a world effect a world effect, and Hand Drawn's look |
| `probe_liquid_lab.gd` | The Liquid Texture Lab end to end, against the real array |
| `probe_paste.gd` | `/paste` over a real build: buckets, undo accounting, the retry list |
| `probe_biome_search.gd` | A real `/locatebiome` miss in the running game: the radius engine and chat agree on, command-to-answer wall clock against the 4 s budget, and frame mean/p95/p99 before, during and after. Windowed, so the frames are real |
| `probe_paste_shot.gd` | A pasted build actually renders |
| `probe_path.gd` | The planner from the dummy to the player, over real terrain |
| `probe_world.gd` | The plumbing the probes that spawn mob bodies share, and the two things each of them used to get wrong on its own. **The world is not ready when the player is on the floor** — chunks keep installing for two hundred frames after that (`settled=false`, 5,241 loaded, 268 generating), and a fixture written or a body placed in that window can be overwritten by terrain that was already on its way — so `wait_settled` waits on the updater's own `is_fill_settled()`. **A column's surface is not found by walking down from the feet** — this world's spawn is under a roof, so that scan returns a CEILING, and a body placed on a ceiling is inside the hill: it suffocates the five half-hearts its reset already owed and cannot be shoved, which read for two rounds as "mobs are broken in probes" — so `stand_in_column` scans down from above and asks the body's own resolver (`MobBody.is_solid_at`, the view that decides standing from suffocating). `water_in_column` is the same question for a wet stand, and `cell_text` names what a cell is for a probe's own print. `pad` lays a stone floor with air over it and re-writes until the body's resolver agrees, so a fixture can be built in the sky instead of into whatever the probe's world happens to have under the player — a probe launches the engine's default-seed world, not the player's (MenuLayer picks that), and its player is at the scene's saved transform rather than the game's spawn point, so the ground it lands on is ocean, cave or cliff depending on the run |
| `probe_world_ready.gd` | The diagnostic that found it, and the smallest check that the plumbing holds: the chunk manager's block read and the body's resolver are printed for the same column before and after the world settles (they agree), then a goblin is put on the real surface through `stand_in_column` and has to arrive UNDISTURBED (no suffocation) and move when shoved. Both halves fail on the scan it replaced: the body landed on a ceiling at 5 of 12 health and a shove moved it 0.000 blocks, because the hurt window from the suffocation refused the shove |
| `probe_mob_push.gd` | That bodies are bodies: three goblins spawned inside one body's width of each other (dream wander silenced, so nothing but the push can separate them) have to come apart to a whole body width without being lifted off their floor, and a goblin spawned 0.2 off the player's centre has to be pushed clear of his whole width. The arithmetic is `tests/test_entity_push.cpp`; what this adds is the half that makes it a behaviour — that the walker asks for it once a tick, that the delta becomes velocity the sim resolves against the ground, and that a body exactly inside another does not stay there |
| `probe_goblin_corner.gd` | How the walker takes a corner the planner allows (one side solid, the other open): the real script with a stub sim and a stub collision world that answers `resolve_voxel_collision`'s two flags per axis over a set of solid cells. The live bug was "he is able to cross if i go closer to him, but otherwise he kinda just stays stuck on the wall hes trying to go around" — a trace of six asks of a single diagonal cell, `(-2, 249, -1) -> (-1, 249, -2)`, in two seconds, every one dropped `blocked by something it cannot climb`, `wish=(0.00, 0.00)`. A body cannot walk the chord past a solid side (its box cuts it by half a body), so a blocked diagonal must be PRESSED and slid along the wall's face: the wish keeps the chord, and the sim's per-axis clipping holds the blocked half at the face while the free one carries the body — the line the player walks there himself (`nav_trace` 2026-11-26 00:10: `x` pinned at the wall's face for three steps, then free). Narrowing the wish to the free half was the first version, and it is why that corner only sometimes worked: with nothing pressing it across, the body drifts off the face and into the hole. Its last check is the corner press itself: the free axis is the LARGER share of the wish (the resolver takes the bigger horizontal motion first, so a pressed-axis-first wish is clipped before the free axis moves and the body ends the tick over the drop), a corner whose free half has no floor is still taken as a cut when its destination is standable, and the same corner with no far floor keeps the CHORD as the wish and counts as blocked; a corner one block UP jumps (the chord from one block up when that clears, otherwise the cut with the jump pressed), and two blocks up is a wall and is counted as one — the leap itself is timed to the step's face and aimed into it, so a goblin a step away presses without leaping and one at the face leaps with the corner's press still in its wish, the free axis being what carries it across a corner that rises, and it leaps from the face and no earlier (a window that widened with speed leapt a block back and came down short, two hops for one rise); and a lip whose route cell is a real block one DIAGONAL step away -- the body drifted into the far cell's row, so `_hop_ahead`'s two-column count misses it -- is hopped rather than pressed, while the same lip with nothing beyond it is still not (the negative case is in the probe: no far block, no hop); and a hop whose lip the body reaches with NO speed backs off for a run-up (`RUNUP_TICKS`) rather than standing — the deadlock was a refusal that zeroed the wish and then waited for the speed only the wish could give, and it held a goblin at one lip for thirteen seconds (`nav_trace` 2026-11-27 12:52, `at=(-1.06, 247.0, 4.13)` from 63.04s to 76.05s, `blocked` to 87) — with the next tick still backing, and with no ground behind it the route given up instead of the body freezing. Its rise fixtures state the route cells as BLOCK cells — one under the feet — because that is what the planner hands the walker, and an off-by-one there is what made the first version of the check pass while the game never jumped. The probe also pins the two rules that crossing needed: a route whose first node is the cell the plan was asked from (the one carrying a body whose centre is over a hole) is not walked back to, and a blocked frame with no progress costs the route while one that is still getting closer does not — eleven drops of a body pinned at the wall's face came from exactly those two. The probe pins the pressed chord on both sides of the wall, the straight diagonal once the lip is behind the body, a squeeze (both halves solid) still re-planning, an orthogonal blocked wish still counting at once, and a slide that would walk its whole footprint off a drop refused (with a step down it may take still a slide) — the second report on the same corner ("he just walked off into the gap"), where the slide went along the wall's face into the gap beside it and the body fell three blocks into water | The run-up is capped per hop (`HOP_RUNUP_LIMIT`): with the cap spent the probe must see no run-up again, no wish and one blocked frame — a hop a run-up cannot fix is a route the walk gives up, not a body that paces. The run-up is capped per hop (`HOP_RUNUP_LIMIT`): with the cap spent the probe must see no run-up again, no wish and one blocked frame — a hop a run-up cannot fix is a route the walk gives up, not a body that paces. And the level test is asked in the units route cells are in (`_route_level_ok`): level, a two-block rise and a legal three-block drop (`max_drop`) are at the body's level, the fourth block down is not — measured against the block's own `y` a three-block drop read one past the slack, which was 195 of the 330 level-slip drops in `nav_trace` 2026-11-27 13:03 and the shuffle that goblin did in one cell for fifteen seconds. And a route that steps down is WALKED: `_steps_down` names one and three blocks down and not a level cell, and a one-block drop with no floor at the feet's level in front gives a wish toward it with no jump and no run-up — it was unreachable against the level slack, so a step down was a lip and was leapt.
| `probe_goblin_flee.gd` | The run-away, on a stub body with no planner at all (a real world is not the question: `walk_to` answers 0 without a chunk manager, which IS the empty-plan case). The live bug was a goblin that FROZE while fleeing — thirteen `goal column has no navigable surface` asks in four seconds, `wish=(0.00, 0.00)` on every sample, health 4 → 0 while the player hit it: it had an enemy, a speed, a destination and nothing to walk. So: a wounded body must leave a wish pointing AWAY from the enemy when no route arrives (the old branch returned with the wish at zero); the aim must be the column's own standable cell rather than a fixed height over it (a hilltop six blocks up resolves to the cell standing on it, and a column with nothing in it resolves to nothing); and a goblin that is NOT fleeing closes on a target it can see within four blocks when the plan has nothing (`CHASE_CLOSE_GAP`), while the same state with the target twelve blocks off still stands still — a plan that answers with the cell the body already stands in is the search refusing the goal, and standing under the patience for that is the freeze the trace caught at `PLAN 28 ... nodes=1 path=(-2, 247, 3)`; the blind run stays the run-away's alone |
| `probe_swim_route.gd` | The three decisions a chase in water makes, on a stub body with no scene (the world probe that would check it end to end could not stand a body up when this was written — see `probe_world.gd` and `probe_world_ready.gd` for why, and `probe_goblin_water.gd` for the same checks in the real world now that it can). The live bug they fix was "goblins stayed three blocks below me, changing their mind every second" on a staircase out of a stream, and the trace showed the plan was fine: a body in water now KEEPS the route it was given (the water branch used to drop it every tick, throwing away the plan the swim then re-asked for); the swim does not eat a DRY cell above the body (those are the staircase — skipping them walked the climb out from under it), while a wet cell above it IS arrived at, because a bobbing swimmer floats at the surface and a level test would stall; and the swim walks the route before swimming at a DRY target, while a WET one is still swum at directly, its plan only ever ending on the bank; and a route whose next cell is a DRY lip above the body is given up on after ninety frames, because the swim zeroes every clock that measures walking and a body otherwise presses at a staircase foot for as long as its anger lasts (`nav_trace`, 2026-11-25 22:20: `stuck` never passing 6) — and that the give-up is counted against the water's own TOP and not the bobbing feet, because at the top of the rise the feet clear a lip the lift cannot climb and a threshold in their units resets the clock instead of counting it (`nav_trace`, 2026-11-27 14:59: eleven seconds at one bank, `lip` cycling 0/18/36/54/72), and that the route goes with it, which is what leaves the shore fallback its path out |
| `probe_rise_giveup.gd` | The walker's give-up on a rise it cannot actually make, on a stub body with a clock of its own: forty sim ticks at one cell with the gap barely moving drops the route and records why, a re-plan that opens with that same cell is refused until `RISE_REFUSAL_TICKS` runs out, a press that IS closing the gap never counts, and a head met for the first time starts its own clock. The live bug was a goblin parked at a two-high rise for eleven seconds with `blocked` at ZERO, jumping at the same block while it re-planned the identical route head every few seconds (`nav_trace`, 2026-11-27 14:59) — the climb branch pressed and jumped with no progress accounting at all, so the only thing that ever ended the press was a re-plan, and the re-plan answered with the wall |
| `probe_mob_attributes.gd` | That `data/mobs.json` reaches a real body: reads the registry's `goblin`, spawns one and holds `get_max_health`, the follow range, the alert box, the blow and the anger window to the file, then reads the body's SIM for the five numbers it holds rather than the script (step height, the two fall numbers, knockback resistance) and checks a bare goblin's armor costs a two-half-heart hit exactly two; then writes a deliberately different goblin to `user://` (30 health, 12 follow, 4 alert, 1.05 step, 6 safe fall, half price, 8 armor, half knockback), loads it, and checks a second body comes up with the NEW numbers — including 8 armor points softening a four-half-heart hit — and puts the shipped file back. A body reading its own constants passes the first half and fails the second |
| `probe_goblin_water.gd` | Water, heights and the chase in them, in one 5x5 pond two blocks deep built in the real world (every cell snapshotted and restored): a goblin put on the bottom of it floats up and paddles out onto the rim; a goblin dropped ten blocks pays the 7 half-hearts the fall rule owes; an island is then built in the pond with a cell of water all the way round it and a chase from the rim has to cross that water to reach the prey standing on it (the player's "thirteen blocks round a two-block puddle"); and the prey is moved into the water itself, where the chase has to wade in after it and land a blow. The four things a mob had none of: a way across liquid, a landing's damage that was drained and thrown away, a chase that stopped at the bank, and a chase that could not reach a target standing in water. It stands its body on the player's own feet and waits for the world to settle before it writes a single cell, then proves the pond through the body's resolver (`bottom=water two=water air=air`) instead of assuming a write landed: with those two, the four checks are green in the real world: on a stone pad the probe builds forty blocks above the player, so the world under it cannot matter, three of the four are green in the real world — out of the pond, up the staircase out of the water, and a blow landed on a target standing IN it. The fourth, a target on the far rim across five cells of water, is not: with the rim open the body takes the dry way round, and that is CORRECT (five wet cells at twice the price lose to six dry ones), so the check needs a channel with no way round, which the square pond cannot be — walls along its sides block the staircase check's own approach. Its own fixture is a follow-up |
| `probe_trace_player.gd` | The trace's player track, checked against a stub world: the 3x3 of column tops around the player's feet. The goblin's lines say what a plan answered and what the walk dropped, and nothing said what the PLAYER does across the corner the walker calls a wall — he crosses it, so his own line through it is the evidence rather than a guess. The map is one number per column: `0` the floor under the feet, `1` a block at the feet's own level, `2` a two-block wall, `-1` a hole one deep, `X` nothing within reach; a line is written on a cell change, a fifth of a block of movement, or a change in the blocks themselves, and not while he stands still. The probe pins all three: the fixture grid, nothing on a second call while he stands, then `step=(0.25, 0.00, 0.00)` on the third. `NavTrace.muted` exists because of it — `user://nav_trace.txt` is one name, and a check of the lines must not write over the session's own |
| `probe_goblin_loot.gd` | A dead goblin actually drops its loot table: twenty bodies killed in the real world by the same `take_damage` a punch uses leave one to four pebbles every time, never more than two coins, never more than one ear and nothing else, each pile where the body fell — with the ear's own 33% read off three hundred rolls instead of twenty coin flips. Half the sample is armed with an offhand torch and has to leave exactly one, every time, while the unarmed half leaves none. The 25% itself is checked here without luck, because this world resolves `torch`: both sides of the boundary (a roll of 0.2499 carries, 0.25 does not) and the rate over four thousand rolls, plus that a name the registry does not know is an empty hand rather than the -1 the lookup answers |
| `probe_goblin_loot_run.gd` | The looting decision machine, headless, on the real `Main.tscn` (the only place `data/items.json` is loaded, so `is_item_tool` can answer at all) with a scripted sim, a plan stub and a fake `DroppedItems` under `Main` — the world's own is renamed out of the way and stands in for the API half. A pile three blocks away is NOTICED, and noticing means `speed_state = "loot"`, the plan asked for with that role (this is the `loot` in `nav_trace`) and on the loot errand's own budget (12,000 expansions, 16 ms); a pile nine blocks away and one lying in liquid are not. Standing on a pile of two takes exactly ONE into the hand and opens the 60-tick inspection, which holds the hand and the body for its three seconds; a pebble is then `spawn_at`'d once at the hand's own point, and that point is not re-targeted inside `LOOT_IGNORE_TICKS` and is again after it; a pickaxe is kept and draws nothing; a sword and a torch both answer as tools while a stick, a
pebble, a coin and an ear do not, and a torch goes into the free offhand while a second one, with
that hand already full, is kept in the main hand; a full main hand stops the scan. A fight mid-errand cancels it, a knockback during the inspection drops the item rather than leaving it undecided, and an errand that never arrives (a stub body cannot walk) times out at `LOOT_ERRAND_TICKS` and stands. Its first half is the two `DroppedItems` methods against the REAL node: the flat-radius plus height-bound search skipping a pile still inside `PICKUP_DELAY` and one whose centre cell is liquid, `take_one` decrementing a stack and REBUILDING that pile's body to the smaller size (`_set_count`) rather than collecting it, and the last item of a pile going through `_remove`. The forget after an EMPTY take -- merged into a neighbour, or the player took it -- is remembered at `LOOT_FORGOTTEN_REACH` and not the reject's wide one, so the survivor a block away is erranded at once instead of hidden for the whole window. A pile below the feet is taken; one 1.2-2.0 below is given up on in 1 s. |
| `probe_entity_light.gd` | Windowed: that a goblin's torch lights what it is carried over, in either hand. One body stands on the ground at midnight with its model HIDDEN, so two frames differ only by the light it carries — the ground under it must be measurably brighter than with an empty hand, on both the mean and a count of clearly-brighter samples. It also reads the fed set through `apply_item_lighting` (one light, at the DRAWN torch, the item's own tint at 14/15, a 7-block radius — half the torch's level, which is also what the player's glow takes half of), that a bearer 25 blocks down the VIEW still feeds a light — distance is not the cull — and that one moved behind the camera drops the count to 0. This is also the one probe holding a real torch, so the SEAT is proved against the mesh rather than against the seat's own numbers: in the model's own space it is in front (z < 0) and on the left (x < 0), its card's up points where the goblin faces, it wears `item_shader.gdshader`, and an emptied hand stops drawing it. The MAIN hand gets the same treatment, because looting put an item there: `arm2` is the goblin's right, so the card is at model x > 0 and z < 0, the fed light is at `get_main_hand_position`, the ground under it brightens on the same two measures, and a goblin holding a torch in each hand feeds 2 lights — one body contributing two is legal under the 4-light cap. That hand is also HELD the way the player's is, which is the one thing a seat cannot show: the shoulder is turned 18 degrees forward (`HeldArmPivot` on arm2, read as 18.0) so the arm's own down axis leans toward the goblin's face by 0.31, and an emptied hand puts the pivot back to 0 |
| `probe_goblin_drawcalls.gd` | Windowed, because a draw call is the renderer's own answer and a headless run has no renderer. The body's cost, measured as a difference: N = 0/1/10/32/100 goblins in a grid in front of its own camera, `Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME` and the viewport's visible/shadow/object counts, each read with the crowd drawn and again with `/root/BodyBatch` hidden — so what is counted is the crowd and nothing else in the frame (the world streams under the measurement otherwise, which is why every reading waits for four identical frames). What it pins: one body is six parts of ONE surface each, on six shared mesh RIDs and one material RID; two bodies in different cells keep their own `item_light`, each equal to the light of the cell that body's own middle stands in; the body's own part nodes are not drawn at all; and the batch draws each part where its node is, carries each body's eased light, and follows the animation as the node turns. The number it exists for: the crowd's own contribution is 6 draws at 1, at 10, at 32 and at 100 goblins (0 goblins reads 0 or 1, the ±1 of a frame still streaming terrain). Flat, because the body is six `MultiMesh` instances of one part each instead of eleven draws per goblin — see `rendering-notes.md` |
| `probe_route_head.gd` | A walker standing near the edge of its own cell keeps the NEIGHBOUR column at the head of a route: the cells already stood in are skipped by column, and a route whose head is two columns off would be read as a hop across a gap and walked into never (the three-second standstill in the session trace). It also covers a CUT-OFF answer, which is never walked: the first one re-aims the roll at the body's own level (the ground it already stands on) and the second gives the goal up and holds, while a WHOLE answer is walked whatever came before it |
| `probe_hammer.gd` | The hammers end to end, including the applied-write rule above |
| `probe_torch_place.gd` | The torch ITEM places the torch BLOCK through the real bridge (`items.json` `place`) |
| `probe_items.gd` | The item registry's fields and the held-item resting poses |
| `probe_item_face_shade.gd` | An item's face shade does not snap while the body turns: the include's arithmetic stays within 1% over a 0.25-degree sweep of the normal, and a block turned in front of a still camera barely moves the frame (see below) |
| `probe_item_light_smooth.gd` | The item light model's step: a staged one-frame relight of the held item's/arm's cell, of a dropped item's cell and of the player's own body's cell moves the `item_light` the mesh is handed over ~0.35 s instead of one frame |
| `probe_body_light.gd` | The player's own body is lit by the world's light, not the engine's: its rendered luma equals the luma of the pixels it is drawn over, it follows the sun down to midnight, and the body and world share the same neutral white-overhead, warm-at-the-horizon sky light |
| `probe_player_anim.gd` | The live body plays its two clips (idle standing with the arms swaying, run walking) and every track of both lands on a limb; the parked pose port is still measured state by state (see below) |
| `probe_held_pose.gd` | The held-item shoulder pose (`docs/gui-notes.md`): the pivot, the 18 degrees while holding, 0 with an empty hand, and the item moving forward with it (see below) |
| `probe_third_person_swing_shot.gd` | The same stroke PHOTOGRAPHED: the body alone under a stub owner, its stroke clock stepped by hand to nine stops, and the frame saved from where third person puts the camera. Runs in a WINDOW (the dummy renderer has no framebuffer). Writes one PNG per stop into the shots folder, named for the aim (level, down, up) and the stop; `FARLANDS_SWING_AIM=down|up` pitches the look 45 degrees, because the blow is aimed. It exists because the stroke was tuned against measured paths for several revisions and the paths were right while the shape was wrong: at the reference's own 23 degrees of roll and yaw the pictures showed the punch taking an eighth of the stroke, foreshortened away to nothing from behind, and the rest of it sweeping the arm out and up beside the head — which no position or direction number says out loud. `_process` is switched OFF before the first stop, or the clock advances while frames render and every picture is of a later point than its label |
| `probe_third_person_swing.gd` | The third-person punch/place swing (`docs/gui-notes.md`): the `swung` wire on the real player's body, the reference's three-clock stroke (a snapping rise, an outward roll, and a body twist on `sin(sqrt(t)·2π)`), the chest turning into the blow and back, a full hand finishing the holding pose's 18 degrees above an empty one, a place at 0.75, a restart landing on the same far end, the item riding the hand, the aim steering the blow (the reference's head-pitch term), the clock running the direction the reference's does, the pivot held to `setupAttackAnimation`'s own expression at three aims (worst 0.056 deg), and the walk carrying on underneath (see below) |
| `probe_sky_brightness.gd` | What a fully sky-lit surface renders AT, against its own albedo: the body is handed a flat known grey (1x1, no atlas to guess from), the frame is read back per albedo, and each one is held to the model's own claim — never above its own texel, and not far below it |
| `probe_item_size.gd` | A dropped item is not cell-sized: a cube's body and drawn mesh are both half a block, a slab and a stair keep their own shape at that scale, a sprite keeps its silhouette, and a stack of more than one is both drawn and solved bigger by the capped log2 curve (1.25x at 2 through 2x from sixteen), mesh and body one box at every count |
| `probe_item_merge.gd` | Two drops of one kind next to each other become one pile: the counts add up to the 64 cap, the fuller keeps its place, body and mesh take the count's own size, motions combine only when both move, and the drawn size pops past the new one and settles back (at half strength once the pile is already at its largest) |
| `probe_drop_swing.gd` | The drop path itself (`hotbar.gd _drop_selected`) throws the item AND kicks the viewmodel's full punch swing -- the swing timer at 1 with no place stroke -- once per unit and once per Ctrl+Q, writing the slot back each time |
| `probe_drop_icon.gd` | A throw clones the slot's own icon out of its slot whole: it starts on the slot's icon rect, rises, turns as it falls out of the frame, and a slot thrown empty does NOT also shatter in place (a spend still does). Its side is a random pick, not the aim's, and the throw scales with the GUI scale |
| `probe_chunk_borders.gd` | The 32-block chunk-grid overlay |
| `probe_preview_cache.gd` | The wand's build preview cache never hands back the WRONG file after re-aiming |
| `probe_wand_shot.gd` / `probe_wand_menu.gd` | The wand's middle-click menu opens where it should, and its buttons work for real |
| `probe_slider_travel.gd` | Slider behaviour measured in the rendered frame |
| `probe_heart_shatter.gd` | The damage shatter, measured in the rendered frame: the red a hit takes off a heart is all still there at the instant of the hit, the heart's new state is what the row draws, the red that left it is below the row and descending, and the frame is the new state alone once it is over |
| `probe_layout_check.gd` | The startup self-check: the engine agrees with the packing the preview code writes |
| `probe_mm_layout.gd` | What the engine really expects in `MultiMesh.buffer`, learned from the engine (must run windowed) |
| `probe_crash_dump.gd` | The crash handler is live in this build and a real fault writes a report |
| `probe_boot.gd` | The world boots at all — the first thing to run after a change to registration |
| `probe_player_model.gd` | That `Main.tscn`'s third-person body is present and comes up: the `PlayerModel` node, the `player.glb` instance behind it, `player_model.gd`, its mesh parts with materials at body size, the skin, and third person revealing it. The instance used to vanish on save because the controller reparented it into a code-created, ownerless `ModelPivot` — a packed scene keeps only nodes owned by its root — so this exits non-zero instead; the controller now leaves the editor's tree alone and rebuilds a missing body from `player.glb` |
| `probe_bindings.gd` | The GDExtension binding surface GDScript sees |
| `probe_menu.gd` | The title screen and the block curtain: real bodies that settle, a wall closed scanline by scanline, the fuses, the skip blast, Reduce Motion, and the live scene's freeze and hand-back (see below) |
| `probe_bucket.gd` | Bucket fill and pour through the real use path |
| `probe_registry_digest.gd` | A digest of the loaded registry, for comparing two builds |

A few of those rows keep their detail here, because a table cell that long is an essay:

- `probe_lod_grid.gd` — the reach is checked against the horizon the stats REPORT rather than
  against a table of its own, and the pipeline half of the probe asks for every wanted tile:
  sampled, built on the pool, uploaded, then gone again when the mode is switched off. That
  last one is not decoration — the mode once stopped scheduling tiles after a world reset and
  lost a third of them while every other count read healthy. The seam half needs the tiles
  that straddle the world's edge to exist AND the clip to be armed just inside that edge, so
  the two surfaces overlap instead of leaving a sliver nobody draws. Its last section pushes
  the reach out and waits for the WHOLE wanted set, then reads the tile count against the
  ladder's own: a reach whose rings are reported but never built is what the coarser spacings
  did, and it also reads the horizon and the outermost level's spacing back at every value of
  `Far Grid Spacing`, because a detail setting must not move how far the mode sees.
- `probe_lod_grid_shot.gd` — a straight-down camera, so a patch's distance from the frame's
  centre is a ground radius rather than a pitch to guess; each patch is compared with the same
  patch in the mode-off frame, which is why no sky or fog colour has to be assumed. It exists
  because the mode once built a ring and drew exactly one mesh, and once left four wedges at
  the world's edge: neither shows in a count of tiles. An earlier version measured the sun's
  drift until the day/night cycle was frozen for the run. Its reach is pinned at the start,
  because two of its radii are statements about the level LADDER (past 2304 blocks is sky) and
  the scene's settings menu node loads the player's own settings.cfg — a reach somebody moved
  is a reach this measurement would otherwise inherit. It ends by asserting the camera's far
  plane follows the horizon (`4000 -> 4352` at 10 rings) and is given back when the mode is
  off.
- `probe_lod_grid_look.gd` — the camera is the PLAYER's, six blocks above their head, four
  headings, because the detail term lives and dies on how big a cell is on screen: from three
  kilometres straight up a 256-block cell is thirty pixels and the sampler's own mip for the
  detail scale is already past the face's average, so the ratio is 1 by construction and the
  first run of this probe read a spread rise of x1.01 over a frame that turned out to be 464
  triangles of deep-ocean water quads — a measurement of nothing. The metric is local contrast
  for the same reason: deviation-from-the-mean over a far-field mask is dominated by the
  world's own large-scale colour and moved by a hundredth when the term was plainly visible.
  The fog and the water tint are measured out of the way (both paint the distance with a colour
  that carries no texture), and the mask is still "the pixels the mode drew", so nothing about
  the world's own geometry has to be classified. What it measured on the development machine:
  contrast x2.2 to x2.8 across the four headings with the mean held to 0.0011, and
  `RENDER_TOTAL_PRIMITIVES_IN_FRAME` moving by exactly the triangles the mode reports — the
  monitor does count the far mesh, which is the reading that started the question.
- `probe_lod_grid_fill.gd` — one number, measured three ways with this one instrument: the old
  budgets gave 6 tiles a frame, 467 tiles/s and 97.2 s for the reach's 45,369 tiles; fixing the
  dispatch rate gave 31 a frame and 30.1 s; and giving a changing level a merge interval gave 24
  a frame, 2,214 tiles/s and 20.5 s at 92 fps. The probe's assertions fail on the first of those
  three, which is what they are for. It prints the columns sampled and the asks the shared table
  answered beside them, because the arithmetic that remains is the sampler's: ~73,600 columns at
  ~4 ms each over the pool's fifteen workers is ~20 s of the 20.5. Each column is walked TWICE
  (both public entry points the build calls re-derive the blended biome amplification), so the
  next lever is a column query that answers both in one pass.
- `probe_lod_grid_zfight.gd` — the report was "z fighting on every single face… I think caused
  by the biome blending", and it is a probe because the difference between a texture artifact
  and a depth artifact is not something a colour can tell you. Every candidate is a state of
  the SAME camera, reached by patching the shader's own text rather than by a debug uniform:
  the detail term with its footprint fade / the same term with that one line removed / the term
  at the repeat it wore before the tiling was halved / the term off / and the biome blend driven
  to full strength, which no cell reaches. The metrics are the mean difference between a pixel
  and the one two along (a pattern's own contrast) and the fraction of pixels that stand out
  against BOTH neighbours while those two agree (one pixel of another colour, which is what a
  depth fight leaves and what a pattern wider than a pixel cannot). It answered its question:
  the blend's own contribution is **0.0000** on every metric at its real weight, while driving
  it to full strength moves the frame by an order of magnitude — so the plumbing works and the
  weight is not where the speckle is — and the detail term is the only state that moves the
  spike rate at all, by 6-8x over the same frame with `detail_strength` 0. Anisotropic and
  trilinear sampling were measured first, as the other candidate; both make it worse (0.0114
  against 0.0058 on one frame), which is what a filter resolving a pattern finer than the screen
  can carry does. Two things the instrument had to learn about its own view: the camera is 140
  blocks above the player because a ground view from an arbitrary spot in a live world reads a
  tenth of the frame as far field (y 96 gave 0.41, y 192 gave 0.10, same probe), and the crawl
  metric is read above the horizon only, because the camera's own 0.05-block step slides the
  ground at arm's length by twenty-odd pixels. What it settled, and where it was blind: the
  speckle is the detail term's own grain sampled near one texel a pixel, and the answer is to
  hold that sampling rate instead of fading the term out — a texel at 4, 8 and 16 screen pixels
  takes the spike rate from x31 over a no-term control to x5, x2.7 and x1.4, with the term's
  local
  contrast within a quarter of where it started. It cannot see the SEAM, because its camera is
  in
  the air where the loaded world is below the bottom of the frame.
- `probe_lod_grid_clip.gd` — "is the far field being drawn over the loaded world", asked
  directly
  rather than inferred: its fragments are painted flat magenta and counted, which no other
  surface
  in the world can be mistaken for. At the player's eye, the shipped clip (a disc cut from the
  world's STREAMING radius, `render_distance * 32 - 16` = 112 blocks at a render distance of 4)
  painted **45-47% of the frame**, and a clip four times wider painted 13-24% of it — so a
  quarter
  of the screen was far-field cells laid over ground the world was also drawing, which is what a
  ring of fighting cells and covered chunks is. The radius prints off the material itself, which
  is the other half of the point: the uniform was live and correct all along, and the number it
  carried was the wrong number. Cutting it from the world's drawn edge (208 blocks) drops that
  share to 15-25%, i.e. to the ring beyond the world, which is the mode's own job.
- `probe_lod_grid_seam.gd` — the same question at the player's own eye and settings, as the A/B
  of the two radii in ONE camera and ONE fill. It earned its place by disproving the fix's own
  premise: the pixel fight is the SAME at both radii (worst heading 0.1624 at the drawn edge
  against 0.1621 at the old one), and on two of the four headings the two frames are byte for
  byte identical while still reading a 0.1471 spike rate — headings where the far field has no
  pixels inside the old disc at all, so what is left is not the world racing the far field but
  the far field's own rendering at a grazing angle. The drawn-edge radius stands on the coverage
  it removes (0.22, 0.29, 0.28, 0.16 of the frame against 0.29, 0.29, 0.28, 0.26) and not on a
  spike rate it never claimed.
  It now also measures the report's own two colours, because the report named them:
  **blue|green**
  is the share of the mask whose water tint (much more blue than green) has the land's green as
  its neighbour within two pixels, and it reads **0.24-0.29** of the far field — one pixel of
  each
  colour interleaved across the field rather than a coastline, which is one contour. Two more
  states of the same camera rule the candidates out rather than in: dropping the water sheet
  whole
  (`if (is_water > 0.5) discard;`) takes that share to 0.09-0.13 and the far field's coverage
  with
  it — the sheet is a lot of the blue and none of the pattern — and moving the camera's NEAR
  plane
  from 0.05 to 0.209 changes the frame not at all (0.2904 against 0.2905 on the same heading, to
  four decimals), so a 24-bit depth buffer is not what is failing. Reading the frame out
  (`look_seam.py`, an offline classifier) puts the interleave in the far field's own half of the
  screen and leaves the loaded world's half uniform: what is blue there is the far mode's own
  fog
  (`vec3(0.70, 0.80, 0.95)`), which is the colour a grain sampled near one texel a pixel turns
  into at the range where green land and blue fog are the same value.
- `probe_lod_grid_overlap.gd` — the far field's two surfaces against each other, which is the
  question the report's own two colours leave ("a light blue texture fighting with a grass
  texture or sand texture on land, but it doesn't fight with water textures"). It renders them
  apart (the sheet kept and the land discarded, then the other way round) on ONE camera and ONE
  fill, so the two masks are pixel-comparable; a second pair of states paints the fragments by
  SURFACE rather than by texture (red for the sheet, green for the land, no fog and nothing
  else); a third does the same with the DEPTH TEST OFF, so every pixel the two surfaces both
  cross shows one of them whatever the depth buffer would have picked; and a fourth lifts the
  sheet's own fragments two centimetres. What it found, in order:
  - the two masks intersect over **0.26-0.33 of the far field**, and half of those pixels show
    each colour, which is what a fight looks like — but the intersection alone cannot tell a
    contest from a shoreline cell whose land merely stands in front of its own water, and it is
    reported rather than claimed for exactly that reason;
  - the flat two-colour frame is **still a one-pixel red-and-green granite** (0.13-0.16 of the
    frame's pixels are a direct sheet-against-land boundary, median run 2 px), so the interleave
    is not the detail term, not the biome blend and not the texture sampler;
  - the pixels that are light blue with land within one pixel are **99.9-100% the sheet's own
    fragments**, not land tinted blue, and the light blue is the mode's own `water_color`;
  - and the two centimetres move **3-14 pixels out of the far field's ~250,000**, so no pixel
    anywhere in the far field is decided by two surfaces within two centimetres of each other.
    That is the refutation: the report's interleave is not a depth fight at all. It was the mode
    drawing its terrain quad AND a flat sheet over the same shoreline cell — the whole cell
    both, against a guess at where the waterline falls inside it.
  **The two-mask intersection has since been retired as evidence, and reading it as a regression
  would be a mistake.** The coastline rule now draws the sheet over the WHOLE of a cell with any
  corner under the water and clips the ground to the part above the level, so those two masks
  intersect by design over the ground the sheet is behind: the same probe on the same camera
  reads **0.765 of the union at the worst heading (yaw 0, 10,205 px), 0.36 at yaw 270, 0.11 and
  0.01 at 90 and 180**, where it used to read 0.14-0.18 — and the frame is right, because the
  extra intersection is one surface strictly in front of the other. What tells the two apart is
  the geometry, not the mask: no ground vertex below the water level, and the topmost surface at
  every sample point of the tile (`tests/test_lod_surface_shore.cpp`). The same run's own
  numbers: **51,121 quads** (45,420 before the change; the straddling cells' sheets), the sheet
  on 0.361 of the frame and the land on 0.446, and **0 pixels** of the loaded world's land
  painted blue.
  A frame can show the two surfaces and the ordering between them; it can NOT show whether two
  fragments are over the same GROUND, because at a horizon everything is compressed into the
  same few rows and a near sheet in front of distant land is indistinguishable from a sheet
  drawn over the land it is on.  So the probe reports and the geometry test claims:
  `tests/test_lod_surface_shore.cpp` casts a ray straight down at every sample point of a
  shoreline tile and asks what the TOPMOST surface is: no point missed, no point covered by two
  surfaces of one kind, and the answer changing at the crossing the samples' own straight lines
  put there -- which is the property the rule is for, and one a count of quads cannot answer.
  Two instrument notes, both paid for. The world SEED is pinned (`seed` 1337): without it a run
  is a different landscape from the next one. And the EYE IS PLACED on the ground at the origin
  rather than read off the player — the spawn is a height that depends on which chunks had
  loaded when it was chosen (128 and 192 blocks in two runs, over ground whose surface is at
  249), so a camera taken from the player is a different camera every run and nothing measured
  under it can be compared with anything. What the lift pair does NOT need is a reference frame:
  it is one camera and one fill, so the sky, the eye and the world cancel.
- `probe_lod_depth.gd` — three measurements, because one of them is the control: a lone near
  quad (the instrument can see red at all), the two quads with the far one first (index order
  already agrees with depth, so it passes either way), and the subject, near quad first. Before
  the shader fix that last one came back blue: the far quad painted over the near one. It needs
  no world, which is what makes it a measurement of the pipeline rather than a coincidence.
- `probe_item_face_shade.gd` — the thresholded table the include replaced steps 20% where the
  arithmetic stays within 1%, and the frame test is the same claim seen from outside: the old
  table moves 82% of a still frame at the 45-degree yaw where x against z flips, the include a
  few percent.
- `probe_item_light_smooth.gd` — the eased `item_light` is read back OFF THE INSTANCE the
  shader sees, so the number measured is the one being drawn rather than the one assigned.
- `probe_held_pose.gd` — a still body under a stub owner (`HeldArmPivot`, one hotbar slot), so
  every number is exact and no world is written. The pivot takes the model's own arm seat, the arm
  sits centred on the joint with its rotation untouched, and the run clip still reaches it through
  the pivot at vanilla's 1.000 rad — a track left naming the old path plays as no motion at all,
  silently, which is what the path-keyed gain table would have done to the arm. The shoulder is
  -18 degrees while a block is in the hand and 0 with an empty one, set in the same function as the
  item's visibility. The item's own position turns exactly 18 degrees FORWARD about the shoulder,
  which is what fixes the sign: this rig faces +Z with the arm hanging -Y, so the proof is that z
  rises (3.10 blocks at this arm length). And with the body animating again the pivot holds while
  the arm's rotation keeps moving under the tree's pose (0.46 to 3.44 degrees off rest), which is
  the difference between composing over the pose and fighting it. The rule's second half is
  measured on that walking body: at a full swing the holding arm draws 29.8 degrees against 58.4
  empty-handed (x0.51) while the legs stay at 80.2 either way, so the half lands on the arm's walk
  and not on the gait.
- `probe_third_person_swing.gd` — two halves, because the wire and the stroke fail differently. On
  the REAL player's body: the first-person hand carries `swung` and `punch`/`place`, the swing pivot
  is between `HeldArmPivot` and the arm, a `punch()` reaches the body at weight 1.0 and a `place()`
  at 0.75 (a body that began listening after `_ready`, or listened to the wrong node, never hears
  either), the body's own arm draws the stroke and comes back, and the first-person timers still
  play out and expire. On a still body under a stub owner — `probe_held_pose`'s shape, so every
  number is exact and nothing in the world is written — the stroke itself, measured on the arm's
  DIRECTION in the body's frame (what a player sees move) against the direction it RESTS in, which
  is 18.5 degrees forward of down because of the held pose. The measurement exists to separate a
  punch from a hinge, and a stroke with one clock from the reference's three. At a level look the far
  end is (0.09, 0.25, 0.96) in the body's frame — 0.96 forward, 0.25 ABOVE the shoulder (14 degrees
  above horizontal) and 0.09 in toward the body's middle, which one pitch about the body's
  left-right axis cannot reach at any angle (that component stays at exactly zero). The crossing is
  measured twice over: as a direction (0.50 sideways at its widest) and as the HAND'S POSITION
  against the SHOULDER'S, which is what an arm crossing the torso looks like and a direction cannot
  show — 8.44 rig units IN past the shoulder's own side line and 4.11 back OUT past it, the mirror
  of the reference's own blow. The LOOP is measured too: the two visits to the same point of the
  travel sit 37.2 degrees apart, because the arm's turn runs on the square
  root of the progress while its rise runs on an out-quart ease of it, so the way back is not the
  way out. Two more are read by sweeping the stroke's own clock rather than by watching it play:
  the AIM, because the reference's rise is `aa * 1.2 + bb` and `bb` is the head's pitch (the arm's
  elevation at a fifth of the way in is −5.7 degrees looking 45 down, 13.5 level and 32.7 looking
  45 up), and the CLOCK'S DIRECTION, because its `attackTime` counts up and its twist reads
  `sin(sqrt(attackTime) * 2pi)` (the chest is 11.3 degrees round at a sixteenth of the stroke and
  home by the end; read the other way round, those two numbers swap ends). And the strongest check
  here is not a property at all: `_reference_pivot` restates `setupAttackAnimation` from its own
  source — `aa * 1.2 + bb`, the arm's `yRot += body.yRot * 2`, `zRot += sin(t*PI) * -0.4`, composed
  Z then Y then X — and the pivot's rotation is compared to it across 21 stops at three aims, worst
  0.056 degrees. The SHAPE is held too, and it is the check that answers "it looks like two poses
  and a pause": the rise must have its one apex before 0.4 of the stroke, with at most 4 of the 21
  stops within 3 degrees of it (measured: apex at 0.20, 2 stops). Raising the rise's 1.2 or the
  aim's 0.75 breaks that check as well as the formula one — the aim term peaks at halfway, so a
  bigger one pulls the maximum late and flattens the top. Its two lateral terms are mirrored on
  both sides of that comparison (`LATERAL`),
  because this body's blow comes inward across the chest where the reference's opens outward: the
  mirror is the one number in the stroke that is this project's rather than the source's, and it is
  a single switch in the model so the roll, the yaw and the chest cannot disagree. Every earlier revision of this stroke passed the property checks while being the
  wrong stroke; a formula check is what a property check cannot be. So
  is the CHEST: 7 degrees into the blow, back to nothing at both ends of the stroke. Two more
  are the HOLDING POSE: a full hand reaches 18.0 degrees above an empty one, the reference's own
  asymmetry, and the shoulder's 18-degree held turn stays put through the whole stroke instead of
  being handed over. Also held: 73.8 degrees of hand travel, the return to exactly
  the rotation the arm started in, a place at 0.75 of the stroke, a restart landing on the SAME far
  end, and the item keeping its distance from the shoulder while it rises. The restart's wait
  POLLS for the hand to be on its way
  back instead of pausing a fixed time: the bell is flat around its top, so a fixed delay can land
  on the peak. Walking is that body given a walk's speed: the legs still reach their own stride
  under the stroke, the clips keep turning the arm beneath it, and the held shoulder turn stays at
  exactly 18 degrees through both.
- `probe_player_anim.gd` — the body is measured, and then the parked port it no longer uses. The
  body: exactly two clips, `idle` and `run`, and every track of both resolves on this rig (a
  track
  that names no limb plays as no motion at all, silently); standing on the floor it is playing
  the
  idle and its arms are swaying; the run clip comes out on Minecraft's own base (a peak of 1.4
  rad
  of leg against 1.0 of arm, one cycle every 0.4716 s -- the clip's own 16.8 degrees and 1 s are
  re-derived at load, not trusted) and its keys are left at that full swing whatever the body is
  doing (ONE amplitude writer: the tree's blend weight, which is measured next); the loop closes
  on its own value and crosses it
  at the same speed it crosses the middle of the cycle (a hand-authored clip eases its own ends:
  `Run.anim` crossed zero at 31 deg/s at its wrap against 103 half a cycle later, and at
  vanilla's
  80 degrees that crawl is a visible pause -- 503 against 503 after the one-cosine fit); the
  walk's
  tree has the walk's blend onto a rest pose and the idle layer wired over it (rest, run,
  timescale, blend, idle, add, with the `AnimationPlayer` as its host) while no clip carries a
  copied arm hang -- both rest on the model's own pose and the idle's own track turns 0 to 3.4
  degrees. Run at a walk's amounts, the arms' roll spans 3.44 degrees while the leg is at its
  full
  80: it DRIFTS, which a copied constant could not; the blend weight IS the swing (half of it
  gives 40.1 degrees of leg against 80.2); and stopping through the production path settles the
  legs onto rest while the arms' roll carries straight through -- biggest frame-to-frame step
  0.02 degrees, so the idle was never rewound. The K-key pose clone stays frozen:
  `skip_head_look`
  deactivates the TREE as well as the head, or the dummy would keep breathing while it is being
  punched.
- `probe_menu.gd` — the curtain is asked what its bodies did, rather than watched, because the
  claim is physical: 96 of them are `RigidBody2D`, they are tumbling ten physics frames in, and
  by the frame the curtain reports `covered` every one of them has landed. The pile is measured,
  not assumed — a separating-axis check between every pair proves blocks are stacked on each
  other and that none is driven more than a sixth of itself into a neighbour, and an exact
  scanline pass (`_scanline_coverage`, one-pixel lines, stricter than the wall's own two) proves
  the swollen wall leaves no gap anywhere before the swap, at five window sizes from 1280×720 to
  2560×1440. The hold is timed from the settle to the emit with the blocks' positions compared
  before and after (worst 0.00 px) and the closed wall's own stillness checked separately, and
  the reveal is timed to the `revealed` signal. Two failure paths are exercised on purpose: a
  wall whose blocks are moved off the side of the screen still covers and still reveals on its
  fuse, and `skip()` is called mid-fall to assert the wall goes up and every cell moves away
  from
  the press point. Two measurements are also about
  the harness, because both were silently wrong at first: the curtain has to be given one idle
  frame before it is sized and played, or `_ready` runs afterwards and switches the whole
  process
  loop off, and the overlap test has to use a separating axis rather than "close in x", because
  two blocks side by side in the same row are legitimately level. The live half boots the real
  scene and asks the layer what it did: player frozen, hotbar gone, primary action naming the
  save state, focus on it, Settings handing the screen over and giving it back, Reduce Motion
  round-tripping through `settings.cfg`, and Play restoring the HUD at the covered frame. It is
  also the probe that proved a preload cannot name `UIScale`: `preload` compiles before the
  autoloads exist, so the curtain is loaded at run time.

`probe_gen_stats.gd` is the one with switches worth knowing, and the one whose
reading changed. `RD=<n>` boots at another render distance (applied after the
settings menu has loaded, or the saved config overwrites it), `SQUISH=1`
compresses the terrain into a kept region of chunk slices, `SPAN=<n>` sizes that
region (1 = one chunk, 8 = a 256-block world: the height-cost sweep in
[terrain-notes.md](terrain-notes.md)), `VEG=0` turns vegetation off. Its `FILL`
line is the fill measurement — seconds and frames until the sweep reports
nothing left to do and generations, installs and rebuilds have been quiet for 30
frames — and that, not a fixed frame window, is
what the squish A/B in [terrain-notes.md](terrain-notes.md) compares: a fixed
window once made the squish look flat at RD 32 by pricing the normal world's
unfinished backlog.

The item probes (`probe_item_size.gd`, `probe_item_merge.gd`, `probe_drop_swing.gd`,
`probe_drop_icon.gd`) restate the script's own constants rather than reading them off it
(`BASE_SCALE`, `MERGE_SCALE_MAX`, `POP_GROWTH`). That is deliberate — an independent restatement
is what catches a value changing silently — and the cost is that a *deliberate* change has to be
made in both places, and nothing fails loudly when only one moves.

`probe_drop_icon.gd` is also where the icon throw's own motion is pinned: the same rect launched
at GUI scale 2 goes about twice as fast as at scale 1 (`launch`'s `scale`, which scales gravity,
the lift and the push and deliberately not the turn, the drag or the clock), and eight throws at
one camera heading leave both ways, which is what an aim-derived side cannot do. It boots
`main.tscn` and needs a display.

`probe_body_light.gd` is the one that reads the player's own body off the screen, and it has to:
the body is a glb of boxes wearing the item shader's light model (`scripts/player_model.gd`),
and
"a body can never disagree with the ground it stands on" is a claim about pixels, not about the
uniforms handed to them. The scene is what makes it measurable — `Main.tscn` has no light node
at
all, only a sky ambient at 0.027 energy, so a body left on the engine's lighting is a factor of
ten dark and cannot move when the sun goes down. The silhouette comes from hiding the body, and
the mask is taken ONCE and reused: re-derived inside a brighter or darker world it keeps only
the
body's pixels that still contrast with it — the dark ones at noon, the bright ones at midnight —
and reads a step that is not there. It is also where the shared neutral sky-light curve is
pinned: the body's material and a scratch world material must both be white overhead, and the
body's own value must still be warm at the horizon, which is what says its white is a curve and
not a flat colour. Shots land in `user://body_shots/`.

`probe_sky_brightness.gd` answers a question none of the others can: what a surface in FULL sky
light actually renders as, against its own texture. It found that the answer was not the
texture.
The light topped out at `kBlockBrightness[15]` (0.415), the sky term added a flat
`sky_light_intensity * 0.15` keyed off the time of day rather than the cell's own sky light, a
filmic curve lifted what was left, and no albedo sampler carried `source_color` — so the texel
was
treated as a LINEAR value by a pipeline that encodes on output, and that encode lifted it again.
Measured at noon on the body's silhouette: a 0.50 albedo came out at 0.68 (1.36x its own texel)
and a 0.25 at 0.61 (2.43x). The probe puts a known flat grey under all of that (the body's
material is one uniform, so the input is a number rather than a guess at an atlas) and reads the
frame back, then holds each albedo to the model's own claim: never above its own texel, and not
far
below it either. With the albedos decoded, the light curve normalised to 1.0 at level 15, and
the
sky bounce and the filmic curve gone, the same run reads 0.50 at 0.45 median (0.89x) and 0.25 at
0.22 (0.88x) — the shortfall is the body's own side shading, which is 0.6 and 0.8 by design. The
subject is the body because its albedo can be set exactly; the arithmetic it runs is the
terrain's
and the water shader's, word for word, so the number describes all three. The ground behind the
body is printed but not asserted on, having no known albedo of its own: its luma went from 0.43
to
0.31 across this change, which is the same overshoot coming off the terrain.

The four goblin probes cover one entity, and each answers a different question.
`probe_goblin.gd` is headless and asks what the Spawn Goblin tool MAKES: an entity, not
scenery — the body carries `scripts/goblin.gd` (the player's own movement sim plus the hearts)
and
stands in `pose_clone`, the group `PlayerController` punches; it is centred in the aimed cell
rather than on its seam, because a 0.6-wide box on a seam is half inside a wall and cannot be
knocked anywhere; the model rides the body as a child with its own offset and yaw, leaving the
body's box axis-aligned; the material is the body's own `body_shader.gdshader` (opaque: it writes
no `ALPHA`, so the boxes are drawn in the opaque pass — `rendering-notes.md`) with the cell's light
pushed onto it every frame and the pushing stopping when the last goblin goes. Its
speed is Minecraft's own value — 0.30, a spider's, which is 2.86 blocks/s (0.66 of the
player's walk) — with the state table over it (fleeing reaches 0.79 of the player's pace),
though only the wander drives a state today. Its six
hearts are its own health and nothing is drawn over its head -- no `HealthBar` node, no sprite
-- so the twelve half-hearts are twelve punches, one apiece, unchanged by a hit inside the hurt
window, and gone with the body at zero. It needs no world and no display:
`probes/goblin_probe_world.gd` is one aim ray, the two light calls and a body with no world to
move
through, which is the whole `ChunkManager` surface the tool touches. Its offhand half is
registry-free on purpose: with no controller above it there is no `data/items.json` or
`data/block_definitions.json`, so no name resolves and the spawn roll can only answer "empty"
— what it holds is that an UNRESOLVABLE name is an empty hand and never the -1 the lookup
itself answers, and that an empty hand builds no holder. The torch's own 25% is
`probe_goblin_loot.gd`'s and the drawn torch with its light `probe_entity_light.gd`'s.

`probe_goblin_loot_run.gd` is the fifth thing a goblin does, and the only one that is not about
a fight: a pile it can see is walked to at the table's own "loot" pace, ONE item is taken into its
main hand and stared at for three seconds, and it is kept if it is a tool and thrown back if it
is not — a torch being the one kept item that lands in the OFFHAND, which is where a goblin's
torch lives, unless that hand already holds one. The probe is headless and drives ticks by hand, because the question is the DECISION
rather than the path — a stub body cannot walk, so the pile is placed at its feet — and it stands
its body in the real world only because the tool query needs the registry that `Main.tscn`
loads. What it pins is written in the table above: the notice radius, the pace and the plan
role, exactly one item, the two decisions, the ignore window, the fight that interrupts at all
three moments (before the ask, on the way, and mid-inspection), and the timeout. The rig half is
`probe_goblin.gd`'s and `probe_entity_light.gd`'s: the shoulder pivot a held item is turned at
has to be on `arm2`, the arm the goblin holds things in, or a kept tool hangs dead straight at
its side while the torch hand takes the player's own pose.

`probe_goblin_anim.gd` is the body itself, and the only goblin probe that needs no world at all:
the model is stood up under a stub host, so what it measures is the rig the spawn tool never
touches
— the obj's eleven surfaces split back into six hung parts, the feet on the floor with the model
1.44 blocks tall, the hips at the tops of the leg boxes and the shoulders at 1.0 — and then the
player's own clips turning it. Standing, the legs are still and the arms' roll moves; a quarter
block a tick (5 blocks/s, the speed a full swing wants) strides 80.2 degrees of leg with the two
legs opposed and the idle's roll still riding the arms; losing that speed settles the legs onto
rest while the roll carries straight through, so a stop is the amplitude shrinking and never the
idle being re-entered. It also pins that the speed comes from the GOBLIN: `_walk_speed` reads
the
nearest ancestor that answers `get_velocity`, so a goblin walks its own pace with the player
standing still beside it.

`probe_goblin_punch.gd` is the windowed one, because a punch is a mouse press: LMB →
`try_punch_dummy` → the nearest body along the eye ray among ALL of `pose_clone` →
the goblin's `apply_knockback`, which is the sim's shove and the sim's hurt window. The group
lists the far goblin first and it is the near one that
must fly, which is exactly the case a `get_first_node_in_group` pick gets wrong (it did). Its
own lesson is that the spawn point is a cell seam, where no body centred in a cell can be hit,
so the probe steers the aim with synthetic `InputEventMouseMotion` — measure the sensitivity
once, then send the difference — and the click needs the title screen's `_release` and a
captured mouse before it reaches the world.

`probe_goblin_roam.gd` is the idle wander, and it needs the real chunk map because a route is
only a route over generated terrain: it loads the world, stands a goblin on the ground beside
the
player, asks it for a route six blocks east and reads back what the entity does with it. That is
the entity's half of the claim — the walk is steering at a stroll and never a teleport (no frame
moves the body further than a stride), it stands only on blocks that are actually there, it
arrives inside its own arrive distance rather than jittering on the last waypoint, it stops when
the route ends and settles into an idle wait, and a punch empties the route and pauses the
wander
instead of letting the next tick steer over the knockback. The wander's own idle timer is
silenced
in the probe, so every stroll that happens is one it asked for.

`probe_goblin_fight.gd` is the blow, and it needs no world either: the spawn probe's stub gains
one
block ray whose answer the probe sets, and `goblin_probe_enemy.gd` is a body that counts what
lands
on it. It asks what the reach MEANS — a body one block away in the open is hit; a body on the
step
in front of it is hit (1.41 of three-dimensional distance); a body three blocks overhead is not,
and neither is one twenty-five overhead, which is the case that was seen in play when the reach
was
the FLAT distance and height was not measured at all; and a body behind a wall is in reach but
not
in sight, so no blow lands, until the wall is gone and the next one does. Both halves of that
were
wrong, and both are one line in `goblin.gd`: which distance is measured, and what is asked
before
the swing. It holds the fight's limits too — it runs at two hearts (4 half-hearts) and not at
two and
a half, a target 40 blocks away is dropped (the leash is 35), the anger clock (400–799 ticks)
times
out on its own, and a punched goblin wakes a neighbour 20 blocks away while one 40 blocks out
stays
out of it, which is the pigman rule and its 32-block box. The last of its checks is the chase's
patience: the stub has no planner, so nothing is ever walkable — the state a goblin reaches at a
frontier it cannot get past — and three seconds of it must drop the target rather than stand
under
the player for the whole anger window.

The rest of the directory is historical or subject-specific; the header comment
in each one says which.
