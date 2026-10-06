# Probes: proving claims a unit test cannot make

Most of this engine is testable headlessly — 619 doctest cases cover the mesher,
the resolver, the fluid rules, the planner and the save formats. A probe is for
what is left: the real loaded registry, the real texture array, the real world,
and what actually reaches the screen.

They live in `probes/` (52 `.gd` scripts). **The scripts are repository
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
| `probes/run_probe_shot.sh <probe.gd> [timeout]` | Anything visual. Runs **windowed** (a screenshot from the dummy renderer is blank) and harvests `user://menu_shots`, `paste_shots`, `shader_shots` and `heart_shots` into `probes/shots/` |
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
| `probe_item_face_shade.gd` | An item's face shade does not snap while the body turns: the include's arithmetic stays within 1% over a 0.25-degree sweep of the normal (the thresholded table it replaced steps 20%), and a block turned in front of a still camera never changes more than a few percent of the sampled frame (the table does 82%, at the 45-degree yaw where x against z flipped) |
| `probe_item_light_smooth.gd` | The item light model's step: a staged one-frame relight of the held item's/arm's cell, of a dropped item's cell and of the player's own body's cell moves the `item_light` the mesh is handed over ~0.35 s (the value is read back off the instance the shader sees), instead of arriving in one frame |
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
