# Probes: proving claims a unit test cannot make

Most of this engine is testable headlessly — 619 doctest cases cover the mesher,
the resolver, the fluid rules, the planner and the save formats. A probe is for
what is left: the real loaded registry, the real texture array, the real world,
and what actually reaches the screen.

They live in `probes/` (58 `.gd` scripts). **The scripts are repository
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
| `probes/run_probe_shot.sh <probe.gd> [timeout]` | Anything visual. Runs **windowed** (a screenshot from the dummy renderer is blank) and harvests `user://menu_shots`, `paste_shots`, `shader_shots`, `heart_shots`, `body_shots`, `brightness_shots`, `lod_grid_shots` and `lod_depth_shots` into
`probes/shots/` |
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
| `probe_paste_shot.gd` | A pasted build actually renders |
| `probe_path.gd` | The planner from the K dummy to the player, over real terrain |
| `probe_hammer.gd` | The hammers end to end, including the applied-write rule above |
| `probe_torch_place.gd` | The torch ITEM places the torch BLOCK through the real bridge (`items.json` `place`) |
| `probe_items.gd` | The item registry's fields and the held-item resting poses |
| `probe_item_face_shade.gd` | An item's face shade does not snap while the body turns: the include's arithmetic stays within 1% over a 0.25-degree sweep of the normal, and a block turned in front of a still camera barely moves the frame (see below) |
| `probe_item_light_smooth.gd` | The item light model's step: a staged one-frame relight of the held item's/arm's cell, of a dropped item's cell and of the player's own body's cell moves the `item_light` the mesh is handed over ~0.35 s instead of one frame |
| `probe_body_light.gd` | The player's own body is lit by the world's light, not the engine's: its rendered luma equals the luma of the pixels it is drawn over, it follows the sun down to midnight, and it is the one consumer handed the sun's own colour instead of the world's midday cream |
| `probe_sky_brightness.gd` | What a fully sky-lit surface renders AT, against its own albedo: the body is handed a flat known grey (1x1, no atlas to guess from), the frame is read back per albedo, and each one is held to the model's own claim — never above its own texel, and not far below it |
| `probe_sky_tint.gd` | The Sky Tint setting reaches what the world is drawn with: the world's own material is handed the midday cream while it is ON, a white zenith while it is OFF, and both are the same warm colour at the horizon |
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
| `probe_bindings.gd` | The GDExtension binding surface GDScript sees |
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
  takes the spike rate from x31 over a no-term control to x5, x2.7 and x1.4, with the term's local
  contrast within a quarter of where it started. It cannot see the SEAM, because its camera is in
  the air where the loaded world is below the bottom of the frame.
- `probe_lod_grid_clip.gd` — "is the far field being drawn over the loaded world", asked directly
  rather than inferred: its fragments are painted flat magenta and counted, which no other surface
  in the world can be mistaken for. At the player's eye, the shipped clip (a disc cut from the
  world's STREAMING radius, `render_distance * 32 - 16` = 112 blocks at a render distance of 4)
  painted **45-47% of the frame**, and a clip four times wider painted 13-24% of it — so a quarter
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
  It now also measures the report's own two colours, because the report named them: **blue|green**
  is the share of the mask whose water tint (much more blue than green) has the land's green as
  its neighbour within two pixels, and it reads **0.24-0.29** of the far field — one pixel of each
  colour interleaved across the field rather than a coastline, which is one contour. Two more
  states of the same camera rule the candidates out rather than in: dropping the water sheet whole
  (`if (is_water > 0.5) discard;`) takes that share to 0.09-0.13 and the far field's coverage with
  it — the sheet is a lot of the blue and none of the pattern — and moving the camera's NEAR plane
  from 0.05 to 0.209 changes the frame not at all (0.2904 against 0.2905 on the same heading, to
  four decimals), so a 24-bit depth buffer is not what is failing. Reading the frame out
  (`look_seam.py`, an offline classifier) puts the interleave in the far field's own half of the
  screen and leaves the loaded world's half uniform: what is blue there is the far mode's own fog
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
the body is a glb of boxes wearing the item shader's light model (`scripts/player_model.gd`), and
"a body can never disagree with the ground it stands on" is a claim about pixels, not about the
uniforms handed to them. The scene is what makes it measurable — `Main.tscn` has no light node at
all, only a sky ambient at 0.027 energy, so a body left on the engine's lighting is a factor of
ten dark and cannot move when the sun goes down. The silhouette comes from hiding the body, and
the mask is taken ONCE and reused: re-derived inside a brighter or darker world it keeps only the
body's pixels that still contrast with it — the dark ones at noon, the bright ones at midnight —
and reads a step that is not there. It is also where the body's own midday tint is pinned, in
both directions at once: the body's material must be handed a white overhead warmth while the
world is still handed its cream (1.0, 0.85, 0.60) — the second read off a scratch material put
through the call the terrain's own materials are fed through, so it cannot be a different number
— and the body's own value must still be warm at the horizon, which is what says its white is a
curve and not a flat colour. Scoping the cast to the body is therefore a claim that cannot be
quietly undone. Shots land in `user://body_shots/`.

`probe_sky_tint.gd` is the one that needs no scene, and it is the only probe that can run
beside a live game: it instantiates a bare `ChunkManager` and never adds it to the tree, so
nothing is generated, meshed or saved, and the numbers it reads come off the call the terrain's
own materials are fed through (`apply_item_lighting` on a scratch material). What it holds is the
behaviour of a *setting* — that `sky_light_warmth` is the cream at noon with the tint on, white
with it off, and the same colour at the horizon either way.

`probe_sky_brightness.gd` answers a question none of the others can: what a surface in FULL sky
light actually renders as, against its own texture. It found that the answer was not the texture.
The light topped out at `kBlockBrightness[15]` (0.415), the sky term added a flat
`sky_light_intensity * 0.15` keyed off the time of day rather than the cell's own sky light, a
filmic curve lifted what was left, and no albedo sampler carried `source_color` — so the texel was
treated as a LINEAR value by a pipeline that encodes on output, and that encode lifted it again.
Measured at noon on the body's silhouette: a 0.50 albedo came out at 0.68 (1.36x its own texel)
and a 0.25 at 0.61 (2.43x). The probe puts a known flat grey under all of that (the body's
material is one uniform, so the input is a number rather than a guess at an atlas) and reads the
frame back, then holds each albedo to the model's own claim: never above its own texel, and not far
below it either. With the albedos decoded, the light curve normalised to 1.0 at level 15, and the
sky bounce and the filmic curve gone, the same run reads 0.50 at 0.45 median (0.89x) and 0.25 at
0.22 (0.88x) — the shortfall is the body's own side shading, which is 0.6 and 0.8 by design. The
subject is the body because its albedo can be set exactly; the arithmetic it runs is the terrain's
and the water shader's, word for word, so the number describes all three. The ground behind the
body is printed but not asserted on, having no known albedo of its own: its luma went from 0.43 to
0.31 across this change, which is the same overshoot coming off the terrain.

The rest of the directory is historical or subject-specific; the header comment
in each one says which.
