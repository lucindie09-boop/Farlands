# Light

Two channels share one cell: a scalar **sky level** and a coloured **block light**
level. Sky light is what the world gets for free from an open column; block light
is what an emitter placed in it gives. They are stored side by side in a single
`uint16_t` per cell -- four bits each for sky, red, green and blue -- in the
palette-compressed light sections, so the two travels are independent all the way
down to the packing.

Levels run 0-15. A step between neighbouring cells costs **one level plus the
destination block's `light_opacity`**, and an opaque block receives nothing. Air
and glass cost nothing to enter, water and acid cost three more per step, lava
takes everything. That single rule is what makes a pool darken as it deepens and
a torch dimmer two cells into water than beside it.

## How a level arrives

**Block light** is a breadth-first walk outward from every emitter, with a matching
removal walk that un-propagates a source when its block is broken. Emitters are
found by scanning the chunk, the volume a pass touches is the 3x3x3 of chunks
around the change, and the region form of the pass wipes the chunks the change can
reach and rebuilds from the emitters in them. All of that lives in
[`src/lighting/light_propagator.cpp`](../src/lighting/light_propagator.cpp) and
[`src/lighting/block_light_region.hpp`](../src/lighting/block_light_region.hpp).

**Sky light** has no emitter. Its source is the open column:

1. A **per-column scan** walks each column from the top down. The level starts at
   the chunk above's level (15 above the world) and passes through blocks that
   absorb nothing at full strength; the first block that absorbs anything costs its
   opacity; every block below that point costs at least one more level, whatever it
   is. That scan is exact for a column and says nothing about the cell beside it,
   which is why there is a second half.
2. A **six-neighbour walk** spreads sky light the same way block light spreads:
   one level per step, plus the destination's opacity, never into an opaque block.
   Its seeds are the cells the column scan left dim -- the shade -- and the border
   planes of a chunk that has just arrived, so light crosses a chunk line as if it
   were not there.

What that produces: a roof is a dimple rather than a hole. A 3x3 platform leaves
14 under its edge and 13 under its middle; a 4x4 leaves the outer ring at 14 with
13 inside it; a shaft lit from above carries full strength to its floor; a cave
mouth fades instead of stopping. 0 is left for volumes no open column can reach.

## Where the walk runs

| Trigger | Entry point | Cost |
|---|---|---|
| A block change that moves a column (opaque set, or opacity) | `sky_light_recompute_column_locked` -- re-scan the column, un-propagate what it used to hand out, relax what survives | one small walk in the 3x3x3 band the edit already holds |
| A chunk becoming resident | `scatter_sky_light_region_locked`, from the install worker, sharing the band and the completion mask with the block-light pass | nothing at all when no chunk in the band has shade -- the worker is not fired and no band is taken. Otherwise a border sweep, plus a walk over the shade |
| A chunk whose neighbour just arrived | the same pass, driven from either side of the border | as above |
| A paste | `sky_light_recompute_columns_locked` -- every column the paste re-scans goes in as one batch, so a pasted roof's volume is walked once, not once per column | one walk per touched chunk, and the mask it returns dirties the neighbours' meshes |

Three properties make it affordable, and each one is load-bearing:

- **The walk only ever raises a level.** A column is a lower bound on the cell it
  wrote, so relaxation from lower bounds reaches the same field whatever order the
  passes run in: a partial pass is safe, a re-run is a no-op, and nothing needs a
  snapshot to compare against.
- **A pass only looks where something can happen.** The install pass reads a flag
  the column scan set (the chunk contains a dim cell that is neither opaque nor
  liquid) and then only the cells that can beat a neighbour. A plain, an ocean and
  a solid chunk walk nothing at all -- and the flag is asked of the whole 3x3x3
  band, not just the arriving chunk, because an open chunk's own 15s are a
  legitimate source for a neighbour's dim border cells. A band with no shade in it
  is skipped before the worker is fired, so it takes no exclusive lock either;
  with no shade anywhere every cell is either 15, an opaque one the walk cannot
  enter, or a liquid one it does not target, which is what makes skipping it
  lossless rather than merely cheap.
- **A pass cannot leave its band.** A chain from a seed cannot travel more than 15
  cells and a chunk is 32 wide, so the 3x3x3 exclusive band its caller holds is
  always enough -- and a step toward anything outside it is refused rather than
  taken under a lock nobody holds. The seeds that are already outside the centre
  chunk are why this is exact rather than merely safe: such a seed is by
  construction the neighbour's cell one step across the boundary, local 0 or 31 on
  the axis it crossed, so leaving the band from there would need the neighbour's
  other 31 cells first -- 32 steps against a budget of 14.

## What is deliberately left out

- **Liquids are not walked sideways.** An ocean's column already dims by three
  levels a block, the sideways contribution there is a rounding difference, and
  walking every water cell of every arriving ocean chunk would put this feature on
  the streaming hot path for a result nobody can see. The exclusion is for the
  cells being filled, not the sources: water can still light its neighbours.
- **A paste only walks the shade its own columns can reach** (the 3x3x3 band it
  already holds). That is enough for everything it wrote and for the neighbours its
  light spills into, and nothing further can reach light from a level below 15
  anyway.
- **Sky light is not saved.** Like block light it is derived: chunks are re-scanned
  when they load, and only block edits are persisted.

## Tests

`tests/test_sky_light.cpp` pins the shape rather than a number that happens to fall
out: the 3x3 dimple and the 4x4 rings, the open chunk that walks nothing, the edit
path landing on the same field as the install pass, growing a roof darkening the
ring it swallowed, taking a roof off restoring full light exactly, the gradient
crossing a chunk line, the sky walk leaving the block-light channels untouched,
and the two halves of the install gate: a band with no shade in it anywhere (the
case allowed to skip), and a shaded **neighbour** of an open chunk (the case that
is not, because the open chunk's border is the only thing that can light it).
