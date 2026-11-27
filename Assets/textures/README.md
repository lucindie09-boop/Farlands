# Block Textures

16×16 pixel-art textures for voxel block faces.

## Provenance

**Status: original work by the project author, except the UI font.** Everything in this
directory is original pixel art made for this project, and the same statement covers the rest of
`Assets/`: the models and animations (`Assets/models/`, `Assets/Animations/`), the sounds
(`Assets/sounds/`), the materials (`Assets/materials/`) and the shaders (`Assets/shaders/`). None
of it is Minecraft art, a downloaded pack or an external library, and it is licensed under the
same GPL-3.0 terms as the rest of the project (see `LICENSE`).

The fonts are the one exception: `Assets/fonts/munro.ttf` is **Munro** by Ten by Twenty
(Ed Merritt), © 2007, under the SIL Open Font License 1.1. It ships unmodified and stays under the
OFL rather than GPL-3.0; the license is beside it at `Assets/fonts/LICENSE-munro.txt`, where the
OFL requires a copy to sit for any bundle, and the font may not be sold on its own.

One credit belongs outside this file: `scripts/body_animator.gd`'s pose is a port of the
signature moves of Fresh Animations' player add-on (FA+Player v1.1, CEM animations by FreshLX),
whose terms allow transforming its work with credit but not redistributing it. No file of that
pack is in this repository — the moves are authored here — and the credit is in
[docs/gui-notes.md](../../docs/gui-notes.md).

If any asset is later found to be derived from a third-party source, it must be replaced with an
original work or attributed here with its license.

## Directory Structure

```
Assets/textures/
  blocks/           Active block face textures (16×16 PNG) — the only directory
					the texture arrays build from
  items/            Held-item and tool sprites (bucket and each sort of bucket,
					sticks, pickaxes, the held torch)
  animated/         Block-break crack overlay frames (l0_sprite_01..10, drawn by
					block_break_overlay.gd)
  atmosphere/       Skybox elements (sun, north star)
  gui/              UI elements (inventory, hotbar, hearts, settings/tool icons)
  sprites/          In-game sprites (heart)
  mobs/             Mob skins (zombie)
  Archive (Unused)/ Deprecated texture variants (not loaded at runtime)
```

## Naming Convention

Block textures are referenced by bare name in `data/block_definitions.json`
(e.g. `"stone"`, `"grass_top"`). The `TextureArrayGenerator` resolves these
to `res://Assets/textures/blocks/<name>.png` at load time, and that is the only
directory it looks in — including for a liquid's still frame, which
`tools/bake_liquid_textures.gd` writes there for exactly that reason. Items
name their own files in `data/items.json` and resolve to
`res://Assets/textures/items/<name>.png` (`BlockTextures`), because an item is not
part of the block array.

Two sets here are generated rather than hand-drawn, both idempotently:
`python tools/make_block_textures.py` writes the torch block texture as a
verbatim copy of the held item's art plus the emitter maps for glass, ladder,
chain and carpet; and the cracked-block overlay in `animated/` is the ten
frames the break animation steps through.

## Missing Textures

The following textures are referenced in `block_definitions.json` but have no
file on disk. At runtime, the generator silently falls back to `stone.png`
(albedo) or a solid black image (emissive). Blocks using these textures will
appear as plain stone with no emissive glow.

**Albedo (fall back to stone.png):**
- `wood_top.png` (wood block top/bottom face)

**Emissive (fall back to solid black):**
- `light_block_emit.png`, `light_red_emit.png`,
  `light_green_emit.png`, `light_blue_emit.png`

## Orphan Textures

`coal_ore.png`, `gold_ore.png`, and `ice.png` are now referenced by blocks in
`block_definitions.json`; there are currently no orphan textures on disk.
