# Terrain generation notes

What worldgen is made of, why the oceans lose every argument with the biomes, and
which file each knob lives in. The description of the generator is in
[../ARCHITECTURE.md](../ARCHITECTURE.md); every field of every data file is in
[data-schemas.md](data-schemas.md).

## The macro surface and the density field

- **Signed 3D density field**: Overhangs, shelves, and arches via 3D fBm deformation around the
  macro heightmap surface
- **4×4×4 world-aligned shape lattice**: Ensures bit-identical results across chunk boundaries
- **Chunk-level fast paths**: Chunks entirely above/below height band skip all lattice/density
  work (~7× speedup on deep chunks)
- **Stacked-noise macro surface**: The per-column height comes from layers summed at a
  domain-warped sample point — a 12,000-block base octave (`height_base_y` ± 500), ~1,000-block
  detail (±100), ×16 ridged flow, and bilinearly-lerped ~500-block (amplitude 90, 8-block nodes)
  and ~150-block (amplitude 25, 2-block nodes) relief fields — sampled on a 4×4 world-aligned
  lattice so the whole field is one continuous surface (no chunk seams). Domain-warp amplitudes
  (18/30/10/16 across two recursive octaves) give landforms their flowing ridges. All tuning
  loads from `data/terrain_config.json`
- **Biomes shape terrain first, oceans decide water last**: the climate grid selects a land
  biome (Plains/Hills) on every column and that biome's amplification knobs shape its height;
  any column still below `sea_level` (200) then becomes Ocean last — water fills to sea level,
  the biome switches to the ocean set (sand surfaces), and the sea bed keeps the height the land
  biome gave it (Plains basins shelf shallow, Hills drop steep). Coasts stay seamless by
  construction; continentalness never gates water (it is a live per-column value carried for
  future preferred-profile selection)
- **Weirdness-gated 3D shaping**: A 2D fBm (~42-block lobes, `weirdness_scale` 0.024) through
  `smoothstep(0.10, 0.75)` picks where shaping is strong; `strength = lerp(5, 50, weirdness)`
  scales the signed 3D shape field, and a surface band fades it fully out 9→28 blocks from the
  macro surface (density can never leave `density_margin()` of it). The per-biome
  `weirdness_size_amplification` knob scales **both** halves of that envelope — the strength
  range and the band (1.0 = neutral 5..50 / 9→28; 0 = the zone alters nothing; 2 = double both,
  band 18→56), so unlike `weirdness_amplification` (which only slides a column along the fixed
  ramp) it changes how much of the world a weirdness zone can touch. `density_margin()` is
  derived from the widest band in the config, so the chunk scheduler's padding stays a valid
  bound (still 30 at neutral)
- **Near-water shoreline handling**: Per-chunk 2-pass Manhattan distance transform seeded from
  the actual density surface swaps in per-biome wet materials next to real water

## Data-driven worldgen

- **Data-driven worldgen**: Macro-surface layers load from `data/terrain_config.json`; per-biome
  surfaces/tree variants load from `data/biomes.json`. Temperature/humidity samplers are live
  (~8000-block climate features at default `biome_size` 1, scales from `climate_*_base_scale`),
  read through a recursive anisotropic domain warp (`climate_warp_amp_x1/z1/x2/z2`, same scheme
  as the macro height warp) so biome boundaries flow and meander, and sampled on a 4-block
  world-aligned lattice with bilinear interpolation (no chunk seams); the macro land height gets
  the same per-chunk lattice treatment (81 evaluations per chunk instead of 4 per column). The
  3×3 climate grid maps the temperate band to Plains and the cold/hot bands to Hills — adding a
  biome is a grid entry + enum value + JSON entry. The per-biome amplification knobs
  (height/weirdness/min_weirdness/weirdness_size) are blended across borders: each 4-block
  lattice node's effective knobs are the arithmetic mean over the biome nodes within
  `climate_blend_radius_nodes` (2 nodes = 8 blocks), which ramps the knobs linearly from one
  biome's plateau to the next across the whole window — each node of radius ≈ 4 blocks of
  transition per side, so the band genuinely widens with the radius (an inverse-distance kernel
  was tried first and front-loaded the transition at the border line, leaving a sharp step no
  matter the radius). Each node's window mean is computed by a separable 2D prefix pass over the
  per-chunk biome grid — O(1) per node regardless of radius, so the cap is high
  (`CLIMATE_BLEND_MAX_RADIUS = 20`, ≈160-block full band) and the only remaining cost at large
  radii is classifying the wider halo of biome nodes once per chunk. Radius 0 disables blending:
  every column uses its own biome's knobs exactly. Ocean columns use the ocean biome's own knobs
  for 3D shaping (the blend field is climate-derived and never contains ocean); their macro
  seabed height is the land biome's blended knob applied before the ocean override

## The squish (test toggle, not a world type)

`worldgen/terrain_squish.hpp` compresses the whole vertical relief into **one chunk slice**, so a
column has a single terrain chunk instead of a band. It exists to answer one question by
measurement: how much of generation and streaming is the vertical axis? It is deliberately **not**
superflat — a flat world would be cheap for a different reason (nothing to mesh) and would measure
the wrong thing.

- **The map is monotone and saturating**, not a linear scale: `c + 10·tanh((h − sea_level)/softness)`
  (`c` = slice centre, `softness` = `TerrainParams::squish_softness`, 300 blocks). Linear scaling
  sized for the worst-case relief bound flattens typical terrain to under a block; tanh sends
  typical relief to most of the slice and compresses only the extremes, so the horizontal shape —
  what the mesher actually pays for — survives.
- **Sea level squishes with it** (`squish::sea_level`), so the ocean/land split and water depth
  survive: every water-fill and near-water site reads that, not `params.sea_level`, or a squished
  world drowns under a sea level of 200 while its terrain sits at y≈48.
- **The 3D shape envelope shrinks to `kBandOuter = 3`** (`shape_envelope`), strength and reach scaled
  by 3/28. Left at ±28 the surface band would leave the slice and the slices above and below would
  generate to hold nothing. With the clamp, everything a column can contain lies in
  `[c − 13, c + 13]`, and `kHalfRange (10) + margin (5) < half a slice (16)` keeps the scheduler's
  height range inside it too.
- **The band is then exactly one slice**: `WorldUpdater::band_pad()` returns 0 when squished (the
  sweep's usual ±32 pad would accept three slices where one holds everything), and
  `column_fill_enabled()` disables the near-player underground fill, which would otherwise put the
  whole column back under the player. Vegetation is skipped for the same reason: every tree would
  grow into the slice that is deliberately never generated.
- **Never persisted, and now not even savable.** `world.meta` stores a fixed list of fields and
  the squish fields are not in it, so a squished world cannot be reloaded as if it were real
  terrain. The `ChunkManager` properties are additionally bound without `PROPERTY_USAGE_STORAGE`
  (`BIND_PROP_VOLATILE`), because the default binding let a *scene save* with the toggle on write
  `squish_enabled = true` into `Main.tscn` — after which every boot, including every measurement
  run, came up squished. The toggle is a live switch: visible in the inspector, settable from
  GDScript, written nowhere.

Toggle it live with `/squish [on|off|slice <n>]` (the command sets the terrain param and calls
`clear_editor_chunks()` so the world regenerates), or from a probe/script via
`ChunkManager.set_squish_enabled/set_squish_slice` + `clear_editor_chunks()`. Only chunks generated
afterwards use it; everything else about the world is unchanged.

Measured (idle machine, same terrain, same column positions):

| Metric | Normal (1024-tall) | Squished |
|---|---|---|
| `bin/benchmark` per column | 3.34 ms, 5.62 chunks (4.34 full gens, 1.28 fast-path) | 0.85 ms, 1.00 chunk (1 full gen) — **3.95×** |
| RD 32 disc, candidates | 20,230 band slices for 3,209 columns | 3,209 (one per column) — 6.3× fewer |
| **Fill the disc, RD 32** (`probe_gen_stats` `FILL`, `VEG=0`) | **7.28 s** / 711 frames — last install 6.41 s, 16,123 gens | **1.45 s** / 157 frames — last install 1.15 s, 3,061 gens — **5.0×** |
| **Fill the disc, RD 64** | **14.33 s** / 1,765 frames — last install 13.18 s, 47,939 gens | **3.94 s** / 375 frames — last install 3.37 s, 9,110 gens — **3.6×** |
| Fill the disc, hand-timed in the game (RD 64) | ≈12 s | ≈3 s — ≈4×, agrees with the probe |
| Band shape at fill | 6.30 slices/column (max 10) | 1.00 (max 1) |
| Pad-only empty generations (24-chunk flight) | 3,635 of 10,168 (36%) | 0 of 1,502 |
| `generate_chunk` cost (24-chunk flight) | 1.15 ms avg over 6,047 | 1.70 ms avg over 250 (one full gen per column) |

So the vertical axis costs about **4× the CPU per column** (3.95× in the benchmark) and the disc
fills **≈4–5× faster** at both radii measured — the squish is not a large-radius-only win.

Time to **fill** is the metric, and an earlier version of this section got it wrong by measuring
the wrong thing: it compared a fixed 300-frame flight window and concluded the squish was "flat in
wall time at RD 32". That window was the artifact — it ran while the normal world was still
draining its boot queue (no completed sweep, ~40% of the list), so it priced a backlog the squished
world had already finished, not the cost of filling. The `FILL` line measures the fill itself: it
waits for the engine's own "the walk found nothing left to do" (`sweeps_completed`) plus a quiet
window on generations, installs and rebuilds, so both sides are compared at the same point in their
work, and the numbers above are the result. The remaining asymmetry is only in shape: the normal
world needs 6.3× the candidate slices per column (6.30 vs 1.00), of which the pad-only ones are
pure overhead — 36% of its 24-chunk-flight installs came back empty, against 0 for the squish, and
its `generate_chunk` is cheaper per call because so much of that band is fast-path fill rather than
terrain. That is also why the per-column CPU gap (3.95×) is slightly wider than the fill gap:
some of what the squish removes was cheap padding, and what remains — one full generation per
column, plus its mesh — costs more per chunk.

## The data files

- **`data/biomes.json`** → `BiomeConfig` (`src/worldgen/biome_config.hpp`): keyed by biome name,
  hosts the `BiomeType` enum (Ocean/Hills/Plains), per-biome surface/subsurface + near-water
  block names (resolved via `BlockRegistry::get_block_id_by_name`), height/weirdness
  amplification knobs (+ per-biome weirdness floor and weirdness-size envelope scale, clamped to
  [0, 2]), `preferred_continentalness`/`preferred_temperature`/`preferred_humidity` (reference
  data for future biome selection; not consumed by generation), `tree_density`, `tree_variants`
  weights, and climate-grid thresholds
  (`temp_cold_max`/`temp_hot_min`/`hum_dry_max`/`hum_humid_min`)
- **`data/vegetation.json`** → `VegetationConfig` (`src/worldgen/vegetation_config.hpp`): hills
  (single-tree chance, spacing)
- **`data/terrain_config.json`** → `TerrainParams::load_from_json`
  (`src/core/terrain_params.cpp`): `height_base_y`, macro warp amplitudes
  (`macro_warp_amp_x1/z1/x2/z2`), shape-warp amps (`shape_warp_amp_x/z`), the mid/small relief
  fields (`*_lattice_spacing`/`*_frequency`/`*_amplitude`), shape-strength range
  (`shape_strength_min`/`shape_strength_max`), the weirdness mask
  (`weirdness_scale`/`weirdness_low`/`weirdness_high`), the climate warp amps
  (`climate_warp_amp_x1/z1/x2/z2`), and the amplification blend radius
  (`climate_blend_radius_nodes`, 0 = disabled)
- **`data/block_shapes.json`** → Shared shape registry for non-full blocks (slabs, stairs,
  walls, poles) with selection/collision boxes
- **`data/recipes.json`** → `RecipeBook::load_from_json` (`src/core/crafting.cpp`): `grid_size`
  plus shaped (`pattern` + `key`, `' '` = empty) and shapeless (`ingredients`) entries; results
  resolved by block name; unknown names/ragged rows skip that recipe with a WARN_PRINT. A key
  entry may name ONE ingredient or LIST several that any of which fills the cell — matching is
  id-exact everywhere (the preview gate and the consumption path both compare ids), so the
  loader EXPANDS such a recipe into one concrete recipe per combination rather than teaching the
  matcher about tags. The expansion is per SYMBOL, not per cell, so a symbol keeps one choice
  across the whole pattern and a tool is always one wood type throughout (oak head on a pine
  handle matches nothing); combinations are capped at 32, and a pattern over 3 cells on a side
  is rejected outright (the grid is at most the crafting table's 3×3, so it could never be
  filled)
- Loaded in `VoxelEngineController::load_world_configs()` at startup; missing files/keys fall
  back to built-in defaults that mirror the old hardcoded values
- Config threads to generation workers via `WorldUpdater` → `ChunkWorld::generate_chunk`
  (captured per call) → the `thread_local ChunkGenerator`
