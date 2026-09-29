# Probes: proving claims a unit test cannot make

Most of this engine is testable headlessly — 588 doctest cases cover the mesher,
the resolver, the fluid rules, the planner and the save formats. A probe is for
what is left: the real loaded registry, the real texture array, the real world,
and what actually reaches the screen.

They live in `probes/` (98 `.gd` scripts). **The scripts are repository
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
| `probes/run_probe_shot.sh <probe.gd> [timeout]` | Anything visual. Runs **windowed** (a screenshot from the dummy renderer is blank) and harvests `user://menu_shots`, `paste_shots` and `shader_shots` into `probes/shots/` |
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
| `probe_gen_stats.gd` | What the generation sweep spends its per-frame check budget on, per rejection reason |
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
| `probe_chunk_borders.gd` | The 32-block chunk-grid overlay |
| `probe_preview_cache.gd` | The wand's build preview cache never hands back the WRONG file after re-aiming |
| `probe_wand_shot.gd` / `probe_wand_menu.gd` | The wand's middle-click menu opens where it should, and its buttons work for real |
| `probe_slider_travel.gd` | Slider behaviour measured in the rendered frame |
| `probe_layout_check.gd` | The startup self-check: the engine agrees with the packing the preview code writes |
| `probe_mm_layout.gd` | What the engine really expects in `MultiMesh.buffer`, learned from the engine (must run windowed) |
| `probe_crash_dump.gd` | The crash handler is live in this build and a real fault writes a report |
| `probe_boot.gd` | The world boots at all — the first thing to run after a change to registration |
| `probe_bindings.gd` | The GDExtension binding surface GDScript sees |
| `probe_bucket.gd` | Bucket fill and pour through the real use path |
| `probe_registry_digest.gd` | A digest of the loaded registry, for comparing two builds |

The rest of the directory is historical or subject-specific; the header comment
in each one says which.
