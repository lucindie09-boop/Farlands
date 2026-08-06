# Block Textures

16×16 pixel-art textures for voxel block faces.

## Provenance

**Status: Unattributed.** These textures are original pixel art created for this project.
No third-party assets, Minecraft-derived textures, or external libraries are used.
All textures in this directory are original works licensed under the same GPL-3.0
terms as the rest of the project (see `LICENSE`).

If any texture is later found to be derived from a third-party source, it must be
replaced with an original work or attributed here with its license.

## Directory Structure

```
textures/
  blocks/           Active block face textures (16×16 PNG)
	Old/            Deprecated texture variants (not loaded at runtime)
  atmosphere/       Skybox elements (sun, north star)
  gui/              UI elements (inventory, hotbar)
  sprites/          In-game sprites (heart)
```

## Naming Convention

Textures are referenced by bare name in `data/block_definitions.json`
(e.g. `"stone"`, `"grass_top"`). The `TextureArrayGenerator` resolves these
to `res://textures/blocks/<name>.png` at load time.

## Missing Textures

The following textures are referenced in `block_definitions.json` but have no
file on disk. At runtime, the generator silently falls back to `stone.png`
(albedo) or a solid black image (emissive). Blocks using these textures will
appear as plain stone with no emissive glow.

**Albedo (fall back to stone.png):**
- `wood_top.png` (wood block top/bottom face)
- `leaves.png`
- `light_block.png`, `light_red.png`, `light_green.png`, `light_blue.png`
- `snow.png`, `gravel.png`, `cactus.png`

**Emissive (fall back to solid black):**
- `light_block_emit.png`, `light_red_emit.png`,
  `light_green_emit.png`, `light_blue_emit.png`

## Orphan Textures

Files present on disk but not referenced by any block in `block_definitions.json`:
- `coal_ore.png`, `gold_ore.png`, `ice.png`

These may be leftover from earlier block definitions or reserved for future use.
