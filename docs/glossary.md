# Glossary

The words this codebase uses in a specific sense. Each is defined by what it
does here, not by what it usually means elsewhere.

## Space and storage

- **Chunk** — 32×32×32 blocks, the unit of generation, meshing and saving.
- **Section** — 16³ blocks; a chunk holds 8, and each has its own palette.
- **Palette storage** — a section stores distinct block ids plus one index per
  block, so a uniform section costs one entry instead of 32,768.
- **Shard** — one of 64 independently locked pieces of the chunk map. A shard is
  chosen per **column**, not per chunk, so all of a column's chunks share a lock.
- **Column** — all chunks at one (x, z), i.e. the vertical line a surface query
  walks. Bands and residency are per column.
- **Band** — the slice range of a column that can contain terrain, derived from
  the column's own surface bounds. A chunk outside every band is impossible and
  is never offered to the generator.
- **Slice** — one chunk's worth of y inside a band.
- **Edit map** — the sparse record of what a player changed in a chunk. Terrain
  is regenerated; edits are layered on top.
- **Pending placement** — a write queued for a chunk that has no render data yet;
  the block does not change until that chunk is ready.

## Terrain

- **Macro surface** — the stacked-noise height field before 3D shaping: the
  smooth "where the ground roughly is".
- **Density field** — the signed field that decides solid vs air. Positive is
  solid. It is the macro surface plus a 3D shape term, so it produces overhangs.
- **Shape lattice** — the 4×4×4 world-aligned grid the 3D shape noise is sampled
  on and interpolated from, which keeps neighbouring chunks seam-free.
- **Weirdness** — the 2D mask that decides where the 3D shaping is strong.
  Most of the world sits at low weirdness, so most terrain is smooth.
- **Strength** — what weirdness is scaled into: how far the 3D field may push
  the surface.
- **Surface band / envelope** — how far from the macro surface the 3D field is
  allowed to reach, which is what `density_margin()` pads every height range by.
- **Continentalness** — sampled and carried on every column for future
  profile-based biome selection. **Generation does not gate on it today.**
- **Biome blending** — averaging each biome's amplification knobs over the
  lattice nodes within `climate_blend_radius_nodes`, so borders ramp instead of
  stepping. Radius 0 turns it off.
- **Generated water** — `surface_water` and friends: liquid to look at and to
  flow against, but with no fluid state, so it is completely inert.

## Meshing and rendering

- **Greedy meshing** — merging coplanar faces of the same block into one quad.
- **Per-AABB emission** — the path for shaped blocks, where each box of a shape
  is emitted separately.
- **Corner surface** — the liquid surface: each liquid cell's four corners take
  independent heights, so a pool's rim slopes rather than stepping.
- **Stride / detail** — the LOD reduction: stride > 1 samples the chunk coarsely,
  detail < 1 merges more aggressively.
- **Tier** — one of the three LOD bands (full, mid, far), each with its own
  distance and detail level.
- **Far region** — 8×8 LOD-reduced chunks merged into one instance, so the coarse
  rings cost a handful of draw calls instead of thousands.
- **Skirt** — the stride-1 ring drawn at an LOD transition to hide the T-junction
  crack between two detail levels.
- **Transition shell** — the band of chunks between two tiers, which is what the
  reprioritise pass walks. Its shape is `max(|dx|,|dy|,|dz|) ∈ [tier-1, tier+1]`.
- **Mesh version / epoch** — a per-chunk counter and a per-world counter used to
  drop stale builds and stale saves.

## Shapes

- **Shape key** — the name a block's `shape` field refers to: `<shape>` or
  `<shape>/<variant>` (`slab/bottom`, `stair/n`, `fence/all`).
- **Part** — one box list inside a shape, optionally conditional. Parts resolve
  at **mesh time**, so a cell's geometry is a pure function of the cell and its
  six neighbours.
- **Rule** — the condition on a part (`fence`, `pane`, `wall_arm`, and the stair
  rules). An unknown rule name draws the part unconditionally.
- **Claim** — which neighbour cells a rule asks about. By default it is *derived
  from the part's boxes*, so a rule cannot be authored against a face the part
  does not reach; only the stair rules declare one explicitly.
- **Canonical resolution** — the resolution used when there is no world (an
  inventory icon): every rule is assumed satisfied.
- **Family** — a set of ids that are one block to the registry (a stair's eight
  orientations, a wall's five bodies). The family's non-base members are hidden
  from `/give`.
- **Unresolved shape** — a `shape` name that is not in the file. It logs and
  leaves a full cube, which is why it is worth reading the boxes back.

## Blocks, items and fluids

- **Property** — a capability flag: `Solid`, `Transparent`, `Opaque`, `Liquid`,
  `RenderAllFaces`, `NoOcclusion`, `Emissive`. `Liquid` is what makes
  `stops_bodies()` false.
- **`drops`** — what mining yields. It may name an **item**, which is how a
  placed torch hands back the torch item.
- **`crush_result`** — what a hammer yields instead of the block itself. It wins
  over `drops`.
- **`min_tier`** — a tool below this tier is not rewarded, but the break is never
  blocked.
- **`still` form** — the same substance with no fluid state (`surface_lava`).
  What a paste places by default, so a pasted lake stays where it was put.
- **Place bridge** — an item's `place` object. The only declared path from item
  space into block space; without it an item is not placeable at all.
- **Fluid state** — a block that *is* a fluid: `source`, `runoff_N` at its
  distance from one, or `fallen`. Resolved by name, never by id.
- **Traits** — a substance's numbers: reach, tick rate, how far it looks for a
  drop, whether a pair of sources pools.

## Imported builds

- **Classic / palette format** — the two build-file families: numbered ids
  (`.schematic`) and named states (`.schem`).
- **State palette** — a decoded file is a palette of states plus one index per
  cell; what each state *becomes* is a separate, data-driven step.
- **Bucket** (in a paste plan) — the outcome every cell is counted into: placed,
  substituted, declined liquid, declined stand-in, skipped, unknown, unresolved,
  air ignored. `schematic_report --plan` prints them.
- **`variant_set`** — a legacy data-value layout shared by every material, with
  `{family}` substituted for the material.
- **`species`** — which of our four woods stands in for another game's wood, and
  whether that is a stand-in (`substitute: true`) or the same block.
- **Unknown vs skipped** — `unknown` means the table has never heard of the
  state; `skipped` means it is a deliberate no. Neither is guessed at.

## Concurrency and process

- **`_locked`** — a method that assumes the caller already holds the exclusive
  lock. It may use `_fast` accessors and must not call anything that locks.
- **`_fast`** — an accessor that does not take a shard lock, so it is only
  callable where the caller already holds one.
- **Auto-locking** — a plain method that takes its own shared lock, and therefore
  must never be called while holding an exclusive one.
- **Frame budget** — a wall-clock or item-count cap on a per-frame stage, so a
  backlog can never stall a frame.
- **Probe** — a script that proves something a unit test cannot, by running the
  real engine. See [probes.md](probes.md).
- **Gate** — one of the four checks CI runs that a local build cannot see:
  `sizecheck`, `portability`, `docscheck` and clang-tidy.
