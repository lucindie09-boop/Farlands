# Inventory and GUI notes

The inventory, crafting, chat and settings work, the two in-game editors, and the
first-person viewmodel - what each one owns and the decisions behind it.

## The C++ cores

- **C++ inventory core**: `Inventory` (9 hotbar + 27 main slots, 64 stack limit) with
  add/consume/can_add logic in `src/core/inventory.*`
- **C++ crafting core**: `RecipeBook`/`CraftingRecipe`/`craft_item` in `src/core/crafting.*` —
  shapeless (sorted-multiset) and shaped (bounding-box trim + mirror) matching over an N×N grid;
  atomic all-or-nothing crafting; recipes load from `data/recipes.json` at startup
- **Item registry & tool stats**: `ItemRegistry` (`src/core/item_registry.*`) owns non-placeable
  inventory objects in their own id space above blocks (`FIRST_ITEM_ID` = 1024), loaded from
  `data/items.json` (entry order = id, so APPEND new entries — inserting one re-points every id
  after it, which a saved inventory would notice). An item may carry
  `"tool": {"class": "pickaxe", "tier": 1, "speed": 3.0}`; `get_item_tool(id)` returns those
  stats. Items are NON-placeable by default (`PlayerController` returns early for an item id on
  the place path), the one exception being an item that declares a `"place"` object:
  `"place": {"block": "light_torch", "wall": {"n", "s", "e", "w"}}` names the block the item
  puts in the world, resolved by name at load like a pour, with `ItemPlace::resolve(nx, ny, nz)`
  turning the clicked face into the right variant (a horizontal face the standing block, a
  vertical face the variant that hugs that wall — `n` = -Z, `s` = +Z, `e` = +X, `w` = -X), and
  the placement then consumes the ITEM rather than the block. The `torch` item is the shipped
  case, so the torch you hold and the torch you place are one loop, and the torch recipe now
  yields the ITEM (the lantern recipe names it too) rather than the block;
  `probes/probe_torch_place.gd` drives it end to end and `tests/test_item_place.cpp` pins the
  face mapping. An item may also carry `"use": {"kind": "pour", "block": "water"}` — the
  in-world right-click action, the one hook they were missing before:
  `PlayerController::use_item()` runs the action when the held item declares one and otherwise
  falls through to block placement, and `pour` writes `block` into the cell the crosshair is
  against (the same cell and the same two guards a placement uses, so a pour and a placement can
  never disagree about where the crosshair is). `ItemUseAction::block` resolves by name at load,
  because blocks load before items, so a typo'd target is an `ERR_PRINT` at startup rather than
  a bucket that silently does nothing. The second use kind is `"fill"` (`{ "kind": "fill" }`, no
  block): it EMPTIES the fluid source the crosshair is on and swaps the held item for that
  fluid's filled bucket, resolved by name off `fluid_kind_name` (`water` → `water_bucket`) so
  the mapping stays in data and a fluid with no container is refused rather than spilt. Two
  cells are consulted — the cell the crosshair landed on and the free cell in front of the face
  it landed on — because a fluid is a short block and a flat look skips over its 0.88 surface
  onto whatever is behind it. A source the simulation does not own is refused (`surface_water`:
  no fluid state, so the hole would be permanent) and so is the falling column (full strength,
  but not a source). Room for the swap is checked BEFORE the cell is emptied, so a full
  inventory cannot lose the fluid. A pour is still not consumed. An item may also carry
  `"light": {"level": 14, "color": [x, y, z]}` (parsed into `ItemLight`, a plain float RGB
  triple — no godot type — so the registry still builds in fuzz mode). While a light item is in
  the SELECTED slot it owns the player's dynamic light: `PlayerController::update_held_light()`
  pushes the item's level and colour and forces it on, remembering the scene's own
  `player_light_enabled` as it takes over and restoring it (with `PlayerLight::DEFAULT_LEVEL` /
  `default_color()`) when the item is put away — holding the torch is what turns dynamic
  lighting on, and a toggle the user set themselves still survives a torch in between. Adding an
  item is one entry in `data/items.json` (plus a `textures/items/<name>.png`) — APPEND it,
  because entry order is the id. `wand` is the newest and is the plainest: a name, a texture and
  no behaviour, so it resolves, renders and is refused by the placement path like every other
  item. `src/core/mining.hpp` turns them into a break-speed multiplier: the pure
  `tool_mining_speed()` (unit-tested in `tests/test_mining.cpp`) returns the tool's `speed` only
  when its class matches the block (`preferred_tool`, or `hammer` against any block that names a
  `crush_result` — see the hammer bullet below) and its `tier` meets the block's `min_tier`,
  otherwise 1.0 (bare-hand speed)
- **Block break/place integration**: Breaking collects into the inventory (gated by
  `can_add_block`); placing consumes from the selected hotbar slot

## Inventory screen, drag and persistence

- **GDScript GUI**: `hotbar.gd` / `inventory.gd` `Control` overlays — E toggles the inventory,
  mouse wheel cycles the hotbar, click-to-hold / drag-drop stack movement, hover/selection
  highlights built by pixel-color-keyed texture recolor (no hand-drawn art)
- **A spent stack comes apart**: when the last item of a stack is used, the icon is no longer
  just gone — its own pixels are thrown up and out of the slot and fall away, through the same
  `scripts/ui_shatter.gd` the hearts use. It is in five places: the hotbar, the inventory (any
  slot emptied, including by a `/clearinv`), and the two crafting grids, where a craft comes
  apart into the cells it drained *and* the output preview once the ingredients no longer support
  it. The art comes from `scripts/block_icon_art.gd`, which samples an icon down to the 16 units
  it is actually drawn at, so a shard is the block the hearts break into rather than a piece of a
  300-unit render, and carries the colour the art had there (the iso render, or the block texture
  where a shape has no icon)
- **Health bar**: `healthbar.gd` draws 10 hearts (`heart_full.png` / `heart_half.png` /
  `heart_empty.png`, 9×9) floating above the hotbar's left edge; each heart is its own 9 units
  on the art's 10-unit pitch (the row spans 99 units), placed through `UIScale`'s helpers and
  NEAREST like every other HUD surface; polls `PlayerController.get_health()` (half-hearts
  0–20) and redraws only on change
- **Damage shatters the hearts.** A hit that takes red off a heart no longer just draws the
  heart a state emptier: every red texel the hit removed is read off the art (red in the
  heart's old state, not in its new one — 14 for a full → half drain, 20 for half → empty, 34
  for a heart lost whole). Each of them leaves as a shard of its own, drawn at its own texel
  size and place — so the frame at the instant of the hit is the frame it was. The motion lives
  in `scripts/ui_shatter.gd`, shared with any other surface whose art comes apart. A hit throws
  the freed red up and out: a lift every shard gets carries the sheet up, and the pulse from the
  heart's middle fans it outward — only a share of that pulse acts vertically (`PULSE_RISE`), or
  it would sling the top of the heart up and the bottom down and the lift would be lost in it.
  Gravity is hard (420 units/s² against a 52.5 units/s lift), so what goes up comes down inside
  the frame instead of floating, and a shard is gone 0.71–0.95 s after the hit. Angles and
  speeds are spread half to double per shard and the whole burst lands harder or softer from one
  hit to the next, which is what stops a heart coming apart from reading as a puff. The shards
  are never turned: a shard is one texel, two device pixels across at GUI scale 2, and a square
  that size covers the same four pixels at every angle it could be drawn at, so a spin could not
  show.
  `probes/probe_heart_shatter.gd` reads the frames back: the red is all still on the screen at
  the instant of the hit, the heart's own rect then holds its new state alone, the red that left
  it is below the row (at most one pixel per texel taken, fewer where the spray overlaps itself,
  and descending frame to frame), and the frame is the new state alone once the effect is over.
  Healing drops nothing at all.
- **2×2 inventory crafting menu** (the crafting table's 3×3 grid is a separate
  `crafting_table_menu.gd` menu): The atlas' color-coded slots (`#7e7d7e` inputs / `#7e7d7f`
  output vs. `#7e7d7d` regular) are located by their fill colors and wired to the C++ RecipeBook
  via `match_recipe(grid_ids, grid_counts)` (availability-gated preview — no ghost results after
  ingredients run out) and `craft_recipe` (atomic verify + deduct). Grid state lives GUI-side
  and persists across open/close so items are never lost
- **Inventory drag operations**: RMB drag-place (spread 1 unit per slot), LMB drag-collect
  (sweep matching blocks), shift-click/drag quick-transfer (move between hotbar/main), scroll
  wheel quick-transfer (push/pull 1 unit between zones), double-click gather (sweep all matching
  blocks into cursor); all mirrored on the crafting grid cells, shift-click output crafts as
  many as possible
- **Inventory persistence**: `user://chunks/inventory.bin` (`INVE` magic, version 1), saved in
  `PlayerController::_exit_tree` (nodes still alive) with a cached `ChunkManager` pointer — the
  old destructor-time tree lookup always failed at teardown

## The GUI scale

- **One integer scale, applied in units.** The GUI was scaled by each script multiplying its own
  numbers by `UIScale.value`, which let four surfaces silently ignore the scale (the crosshair
  held a fixed 9 px arm, the compass a fixed 20 px font, the FPS label whatever the scene pinned,
  and the chat opted out on purpose), and let the death screen scale by two thirds of everything
  else. `scripts/ui_scale.gd` now owns the scale (`set_scale` rounds and clamps) and the two
  placement helpers (`centered_origin`, `edge_origin`), and every panel origin goes through them
  rather than `(viewport - panel_px) / 2.0` — that form lands the panel and its slots on a half
  pixel at an odd viewport width. Draw and hit-test agree because both call the helper.
- **An item is 16 units in every slot**, and the icon renderer dropped from 300 px to 192 so
  16/32/48/64 all divide it. The old sizes were fractions of the slot (`fill_size * 0.9`, `width
  * 0.8`) and the cursor-held stack was a flat 48 px, so an icon was resampled by a different
  ratio at every scale and the held stack never grew at all.
- **The hearts stopped being a fraction of the bar.** They were sized `0.4 / 91` of the hotbar's
  on-screen width — about 7.2 units for a 9-unit sprite, on a 10-unit pitch — and drawn with
  `TEXTURE_FILTER_LINEAR` in an otherwise all-NEAREST HUD. A heart is now its own 9 units on the
  art's 10-unit pitch (the row spans 99 units, about 54% of the bar, rather than the ~43% the
  fraction gave) with NEAREST, so the sprite is never resampled.
- **Crosshair, compass and FPS are in units now.** The crosshair scales inside `draw_crosshair`,
  which both the HUD node and the settings-menu preview call, so the preview cannot show a size
  the live crosshair does not have; the slider values are read as units, which roughly doubles
  the on-screen crosshair at GUI scale 2 compared with the fixed-pixel version.

## Chat and the settings menu

- **Chat system**: `chat.gd` with advanced autocomplete — ghost text suggestions with pulsing
  effect (0.25-0.4 alpha), tab cycling through completions, up/down arrow navigation,
  hold-to-cycle (0.1875s intervals), parameter hints for commands (`/give <block> [count]`,
  `/tp <x> <y> <z>`), commands: `/help`, `/give` (unlimited count), `/tp`, `/fly`, `/clearchat`,
  `/clearinv`, `/version`, `/genstats` (what the generation sweep's per-frame check budget is
  spent on — including how many columns were found already built and how many the walk skipped —
  see ARCHITECTURE.md's frustum-loading section for the measured split, and `/genstats reset` to
  zero it), `/squish [on|off|slice <n>|span <n>]` (the test toggle that compresses the terrain into
  a kept region of chunk slices — one by default, `span` slices otherwise — and regenerates, for
  measuring generation without the vertical axis — see [terrain-notes.md](terrain-notes.md)),
  mouse wheel scrolling for chat history, caret blink, wrapped messages with proper
  input box anchoring
- **Settings menu**: `settings_menu.gd` with adjustable settings (render — including an MSAA 3D
  Off/2x/4x/8x cycle button that sets the root viewport's `msaa_3d` live — plus lighting,
  crosshair; Controls and Shaders are their own Escape-menu pages) opened with Escape key, all
  settings persist across sessions
- **Settings menu layout — one size table, no raw pixels**: the whole menu is a page per area
  plus the two editors. Escape opens the **pause menu**, which is bare — Resume, Settings,
  Shaders, Controls and Tools on their own over the dimmed world, no title band, no content box,
  no footer (the column is sized from that list, so a page is one more entry and nothing else) —
  and **Settings** is the single `_build_scrolling_page` with the categories **General, Block
  Outline, Crosshair, Advanced Rendering, Render** stacked on it (no content-box background —
  the world shows through). The page itself is bare too — the title and the Back/Done actions
  have no dark bar behind them (`_bar` is gone), they stand directly on the dimmed world;
  **Controls** (`_build_controls_page`), **Shaders** (`_build_shaders_page`, one switch row per
  `data/shaders.json` entry plus that shader's own uniforms under its own heading) and **Tools**
  (`_build_tools_page`, the two editors' launcher) are their own pages entered from the pause
  menu. Each area is a *section* of its page, marked by a third element `"category"` in the
  section array (`["Lighting", [], "category"]`), which `_build_scrolling_page` draws in
  `CATEGORY_COLOR` with NO gap above it — no sub-headings between categories, and per-row reset
  buttons where the row carries a reset callable — a square `undo_button.png` icon
  (`UNIT_UNDO_W` = `UNIT_BUTTON_H` = 20 units, so it stays 1:1 at every scale). A transparent
  content box draws no border, which is why the settings/controls/tools pages have no grey frame
  over the world. A category's heading is an HBox, not a bare Label: beside the title sit a
  per-section export icon (`export_button.png`), import icon (`import_button.png`) and reset-all
  icon (the same `undo_button.png`) — all square at `UNIT_HEADING_ICON_W` (half the row reset's
  `UNIT_UNDO_W`), sat tight against the title (the heading Label does not expand). Export and
  import run the category's own **codec** — the `{"export", "import", "refresh", "hint"}`
  dictionary its builder attaches as a Dictionary-marked section, which `_category` carries onto
  the heading — so every area exports and imports its own settings as one short
  `FG`/`FO`/`FC`/`FAR`/`FR` code and resyncs its rows from it; reset all replays every row's own
  reset callable. A section has no Preset rows of its own any more: the three icons beside its
  title are its only export/import/reset. The crosshair category zips its two row lists with
  `_interleave` so Cross reads as the left column and Dot as the right; the block outline
  category does the same so outline reads left and the fill "overlay" (face highlight) reads
  right. Labels in a paired column carry their column's name on BOTH sides so the two halves are
  distinguishable on the page: `Show Cross`/`Show Dot`, `Cross Rotation`/`Dot Rotation`,
  `Cross Colour`/`Dot Colour`, `Cross Opacity`/`Dot Opacity`, `Dot Size` (the dot's one row the
  cross has no echo of stays `Dot ...`), `Show Outline`/`Show Overlay`,
  `Outline Pulse Speed`/`Overlay Pulse Speed`, and so on — one prefix per column, no bare label.
  Adding a setting means one row array in the matching `_build_*_sections()`; adding an area
  means one more `_category(...)` call in `_build_settings_page`. Every size is one of the
  `UNIT_*` constants times `_ui_scale()` — a widget is `UNIT_BUTTON_H` (20) units tall with
  `UNIT_FONT` (8)-unit text and an option is `UNIT_OPTION_W` wide, so at GUI scale 2 a button is
  the 200x20 units (400x40 px, 16 px text) the reference calls Normal. Sizing anything in raw
  pixels is the bug this replaced: buttons were drawn at 2/3 of the unit grid while their labels
  stayed full size. The maker pages' side/bottom chrome follows the same table via
  `_maker_row_top`/`_maker_slot`. Sliders all wear the interface's own track and thumb
  (`slider_button.png` / `slider.png`, pre-scaled by `_slider_track_style()` and
  `_scaled_thumb_tex()`) with `center_grabber` **off**, so the thumb travels the track's width
  minus its own: its leftmost pixel sits on the track's leftmost pixel at the minimum and its
  rightmost on the track's rightmost at the maximum. Leaving `center_grabber` on centres the
  icon on the value instead and hangs half of it off each end of the track. The surface the
  pointer's state reaches is the HANDLE, not the track: `_style_slider_thumb()` gives the thumb
  a brightened `grabber_highlight` (and a dimmed `grabber_disabled` — without that override
  Godot draws its own 16x16 white square in place of our thumb on an uneditable slider, which is
  the mipmap-bias row with mipmaps off), while the track is one flat stylebox with no
  highlighted twin. `probes/probe_slider_travel.gd` measures both travel ends and all three
  handle states in the rendered frame. A colour row's swatch is painted over the button's own
  text, so its caption is an outlined child Label (`_add_color_caption`) — a bare
  `ColorPickerButton` row shows a colour with no name at all
- **Settings menu rows carry their own label**: a row widget's text is "Label: value", kept in
  `row_label`/`row_value` metadata by `_row_label`/`_row_value`, and a slider's value lives in a
  Label drawn over its track (`value_label`). Handlers must therefore call
  `_row_value(widget, value)` rather than assigning `.text` — a direct assignment silently drops
  the label off the row. A span row (`[label, control, null, "span"]`) is a non-option child
  spanning the box (a live preview, a status line). A status-line span is a **collapsed hint**:
  `_make_hint_label` makes the row start hidden and follow its text via `_set_hint_text`, and
  `_make_span_row` keeps the whole row hidden while the hint has no message — a hidden child
  takes no layout space — so an empty feedback line cannot stretch the page. Every category's
  sections ends with such an empty hint (`["", [["", hint, null, "span"]]]`), which is exactly
  why the gap above a section title used to be a full 20-unit row taller than every other gap.
  The title row itself is 24 units tall (its 10-unit icons with enough air for the centred title
  to sit 8 units below the row's top), so the whitespace above a category title measures 12
  units with the VBox's own 4-unit gap — an intentional section break, three times the plain row
  gap

## Skin Maker

- **Skin Maker**: Settings → Skin Maker page — a MUNRO-font restyled ColorPicker (custom `Theme`
  on the picker plus per-node overrides via `_tint_picker_internals`, applied to its internal
  `get_children(true)` controls — bare `get_children()` returns 0 for internals in 4.7), live
  hex readout, and `skin_preview.gd`, a transparent-background sub-viewport that drag-orbits
  `player.glb` around its AABB center (the camera must NOT be a child of the rotating pivot, and
  4.7's `own_world_3d` leaves `world_3d` null so the viewport environment has to be built
  manually). The internal picker headers (Swatches / Recent Colors) default to `font_pressed`
  1.0 white — the reason hovered text looked white
- **Skin Maker paint tools**: DRAW/FILL/BOX paint tools with inclusive box semantics and
  off-face freeze. Grayscale noise slider owned by SkinManager (persists per skin + across
  restarts). Mirror paint edits into the noise base so slider changes keep your work. Fixed
  stale ImageTexture swaps so every model always renders the live skin.
- **Skin gallery**: 3D spinning previews with load and delete functionality. Fixed stale
  ImageTexture swaps so every model always renders the live skin.
- **Skin dark-mode toggle**: Top-right "DARK MODE"/"LIGHT MODE" toggle (`_skin_dark_mode`,
  persisted in `settings.cfg` under `gui/skin_dark_mode`); `_apply_skin_palette()` flips page
  bg, fg/borders, hex/hint colors, swatch tiles, and rebuilds the picker theme; the toggle's
  `PRESET_TOP_RIGHT` offsets must stay positive or it renders off-screen. Preview needs no
  changes — its transparent bg shows the page behind

## Block Maker

- **Block Maker**: Settings → Block Maker page — `block_manager.gd` autoload +
  `block_preview.gd`, mirrors the Skin Maker for one 16×16 cube texture. BlockManager owns a
  single persistent `ImageTexture` shared by every cube face (all 6 faces use the one 16×16
  atlas), debounced saves to `user://current_block.png` plus a restart-recovery snapshot
  `user://block_noise_base.png`, and a reversible grayscale-noise slider whose value/base
  survive page rebuilds because they live on the autoload. Noise/sidecars persist per block as
  `user://blocks/<name>.png` + `<name>.json`
- **Block Maker noise, world-wide**: the makers' sliders stay per-texture and repaint on the
  CPU; the Shaders page's **Noisy Blocks** is the same grain over every block at once (0..100,
  live, in the world's own fragment stage — see `docs/rendering-notes.md`)
- **Block Maker paint tools**: DRAW/FILL/BOX over primitive triangle raycasts (`MeshInstance3D`
  has no physics node), inclusive box semantics clamped to the clicked face, undo stack (Ctrl+Z)
  recording per-texel old/new colours, brush-stroke gap-bridging interpolation, and zoom clamped
  `ZOOM_MIN 12`/`ZOOM_MAX 40` with the cube at scale 12 (min zoom stays outside the corner
  radius ~10.4)
- **Block gallery**: 3D spinning cube previews with load/delete. The card cube is a
  byte-for-byte copy of the editor's `_build_cube_mesh` (every face upright — the old generic
  builder rotated 4 of 6 faces 90° and read as a diagonal texture stretch); card buttons are
  pinned square (`SIZE_SHRINK_CENTER`) so the fixed 96×96 viewport is never squished into a
  wider-than-tall rect, and the viewport resizes to match the button via a `resized` signal
- **Input-guard parity**: `_can_start_drag` in both `block_preview.gd` and `skin_preview.gd`
  uses a geometry fallback — `gui_get_hovered_control()` lags one event right after a button
  press, so if the pointer is inside the preview rect but the claimed control isn't under it the
  press belongs to the preview (otherwise the first paint/orbit click after pressing a button
  was swallowed)

## Breaking, drops and the hammer

- **Block break animation + hardness**: Blocks carry a `hardness` value in
  `data/block_definitions.json` (seconds to break by hand; `-1.0` = unbreakable, used for
  bedrock/water) and an optional `preferred_tool` class (+ `min_tier`) naming the tool mined
  fastest against them. Breaking is hold-to-break: `PlayerController::update_break_progress`
  accumulates `delta * tool_speed / hardness` each frame while LMB is held on the raycast
  target, where `tool_speed` is `mining_speed_multiplier(block, held)` from the selected hotbar
  slot (1.0 for a bare hand or a non-matching tool) (mouse captured, no UI open, inventory gate
  matching `break_block`), then performs the existing instant-break logic. `get_break_state()`
  exposes {active, x/y/z, stage 0-9} to `block_break_overlay.gd`, a `BlockBreakOverlay` node
  that draws a slightly-enlarged transparent cube over the mined block and swaps the 10 stage
  textures (`textures/animated/l0_sprite_01..10.png`); faces against solid neighbors are
  depth-occluded so cracks only show on visible faces. **Progress resets on release** (releasing
  LMB or losing the block resets `break_progress_`/`break_target_valid_`), so the crack vanishes
  and the viewmodel's looping punch stops when you stop mining.
- **Hammer crushing (`crush_result`)**: a block may name the block a HAMMER leaves behind
  instead of itself (`"crush_result"` in `data/block_definitions.json`; 0/AIR = not crushable).
  That one field is the hammer's **entire** contract, and both halves live in
  `src/core/mining.hpp`: `tool_mining_speed()` counts a `hammer`-class tool as matching any
  block that names a `crush_result` (so cobblestone can prefer the pickaxe and still be mined
  faster by a hammer; a block may also name `"hammer"` outright to get the speed without being
  crushable), and `resolve_block_drop(block, held)` swaps the drop for the crush target.
  Crushing is keyed on the block actually broken (a slab of a crushable block is still a slab)
  and is deliberately **not** tier-gated — tier only scales break speed. Current chain:
  cobblestone → gravel, gravel → sand.
- **Block drops (`drops`)**: a block may name the block it yields when broken, whatever the tool
  (`"drops"` in `data/block_definitions.json`; 0/AIR = drop this block itself). It is
  deliberately separate from `crush_result`: `drops` is what ANY break yields, `crush_result` is
  what only a hammer makes of it, and a crush wins over a drop. So **stone yields cobblestone**
  (which means stone blocks are no longer obtainable by mining — anything needing stone as an
  ingredient needs cobblestone instead) and **a hammer does not turn stone into gravel**, only
  cobblestone. Both fields are resolved name→id in ONE post-pass of
  `BlockRegistry::load_from_json`, because a target is normally declared later in the file than
  the block naming it. **Both `break_block()` and the hold-to-break inventory gate resolve their
  drop through `resolve_block_drop`, so the gate can never check room for something other than
  what the break then hands over** (that mirroring was hand-copied before and had already
  drifted). A block whose `textures` name has no file under `res://textures/blocks/` silently
  falls back to stone.png — `_check_textures` in the hammer probe catches that for the blocks it
  names.

## GDScript moving into C++

- **Phase 1 GDScript→C++ migration (viewmodel math)**: `ViewmodelPose` (static binding over
  `src/core/viewmodel_math.*`) now owns viewmodel.gd's per-frame animation math — walk bob
  (`step_walk_bob`), mouse sway (`step_sway`), punch/place swing + equip pose
  (`compute_swing_pose`), and held item/block swing transform (`compute_swing_transform`), plus
  `smoothstep_01`. Each is a pure function of its inputs returning pose fields; the thin
  `viewmodel.gd` glue still owns the node tree, `_input`, and the F12 HUD and applies the
  returned poses. The math is locked by byte-for-byte tests in `tests/test_viewmodel_math.cpp`.
  The F12-HUD-tuned peak constants (`PEAK_ROT`/`PEAK_POS`, `PEAK_ROT_BLOCK`/`PEAK_POS_BLOCK`,
  and the per-type item peaks in `_item_poses`) remain in `viewmodel.gd` and are passed in as
  Vector3 pairs so they stay the single source of truth.
- **Phase 1 scope (other ports)**: `BlockTextures` (native binding, `block_textures.gd` deleted)
  owns block-texture atlas generation from `textures/blocks/`; `SkinPixels` (static binding over
  `src/godot_bindings/skin_pixels.*`) owns the pixel/noise helpers (noise map, gray noise,
  UV→texel bounds) shared by `skin_manager.gd` and the settings galleries. The one Phase 1 item
  **not** fully ported: `skin_manager.gd`'s stateful autoload logic (persistent `ImageTexture`,
  debounced `user://current_skin.png` + noise-base saves, `set_noise` state machine) still lives
  in GDScript — its heavy loops are native via `SkinPixels`, but the autoload state itself is a
  deferred follow-up.

## The viewmodel and what is held

- **First-person viewmodel & held-item/block rendering**: `viewmodel.gd` (child of `Camera3D`,
  eye space +X right/+Y up/−Z forward) builds the hand + held item/block each frame. The arm is
  an explicit shoulder→grip limb aligned to `MC_ARM_BASIS`; the held item (F12 ITEM mode) and
  held block (F12 BLOCK mode) render on `_item_scale_node` with a per-mode rest pose. Held items
  render with a RESTING POSITION from `_item_poses` (F12 modes ITEM and ITEM2), selected per
  item by the optional `"pose"` field in `data/items.json`; ITEM2 is built from ITEM in
  `_ready()` with its Z term lowered by `ITEM2_Z_DELTA`, never hand-copied, so re-tuning ITEM
  cannot leave ITEM2 stale. The buckets, the torch and the iron ingot use ITEM2 because their
  artwork sits a quarter turn round on the sprite plane; the wand, like the stick and the
  hammers, is a held tool drawn on the sprite plane and keeps the default ITEM pose
  (`probes/probe_items.gd` asserts each of those five names resolves to ITEM2, so one added
  without the pose cannot ship silently out of line).
- **Shared viewmodel/cube mesh builders**: `ViewmodelMeshes` (static binding over
  `src/core/viewmodel_meshes.*`) carries the geometry for the held-block cube, shaped-block
  (slab/stair/wall/pole selection boxes), and extruded-sprite item meshes that `viewmodel.gd`
  used to build inline. The cube builder (texture-top = world-top on every face) is also used by
  `block_break_overlay.gd`, `block_preview.gd`, and the settings_menu block gallery — all four
  byte-for-byte copies now call the one C++ builder; the sprite mesh (front/back + silhouette
  rims) is locked byte-for-byte against an independent Python port of the original GDScript in
  `tests/test_viewmodel_meshes.cpp`.
- **Punch/swing animation**: Minecraft-style punch on LMB (0.225s). The depth curve `s` goes
  0→1→0 over the punch, reshaped by a cubic smoothstep (`x*x*(3-2x)`) for a flatter crest; the
  arm traces a two-sided circular arc (`sin(angle)*0.15`, out one side → 0 at peak → other side
  back). Rest/peak poses are tunable via constants — arm (`PEAK_ROT`/`PEAK_POS`), held item (the
  `_item_poses` rest/peak pairs, raw F12 ITEM / ITEM2 values), held block
  (`PEAK_ROT_BLOCK`/`PEAK_POS_BLOCK`, raw F12 BLOCK values). The held item/block swings are
  driven on `_item_scale_node` inside `_update_item_transform`/`_update_block_transform` (which
  run every frame and own that node, so parent-pivot offsets can't hit the exact tuned values;
  the swing math itself is `ViewmodelPose.compute_swing_transform`). The punch **loops while
  breaking** (via `get_break_state()["active"]`) and is gated so clicks in a UI (mouse released)
  never swing. **Place animation** (`place()`) fires only on the C++ `block_placed` signal
  (emitted after a block verifiably lands + inventory is consumed), reusing the same swing at
  75% endpoint strength.
- **Walk bobbing**: vanilla walk-bob style hand/item/block bob driven by accumulated walk
  distance (`_walk_dist*PI*0.6`) with an amplitude envelope (`_bob`) that ramps with horizontal
  speed and **decays to zero when airborne** (ground state from a new
  `PlayerController::is_on_floor()` binding). The vertical term `-abs(cos(...))` keeps the bob
  on the lower half of the circle (never above rest); position/rotation amplitude is scaled to
  10% strength, uniform cadence (no frequency fold). Exposed as `global_position`-tracked in
  `viewmodel.gd`; the math lives in `ViewmodelPose.step_walk_bob`.
- **Player model**: `player.glb` is a voxel-style model with slim 3-px arms and a tightly-packed
  64×64 skin atlas laid out over pixel-face UV islands (`patch_slim.py` in temp is the source of
  truth if the glb ever needs re-patching — it must read the pristine file); `player_model.gd`
  applies the skin texture with nearest filtering (linear/mipmap blends texels across UV
  islands). Node pivots are re-baked onto the true joints (limb tops at y=24/12, head at the
  neck (0,24,1.5) on its center axis, torso center) by `tools/rebake_player_pivots.py` —
  Blockbench's glTF export anchored them at cube bottom-corners and glTF has no pivot field, so
  Godot was swinging every part around the wrong point (see the pivot note under Constraints).
  **The body is a scene node, not code**: `Main.tscn` instances it as `Player/PlayerModel`
  (scale 0.05625, +0.0844 z, `player_model.gd`, plus a direct `AnimationPlayer` child for
  `Idle.anim`), and `PlayerController::_ready` only *finds* it by name and reparents it under
  `ModelPivot`. An editor re-save of `Main.tscn` can drop the node and its ext_resources without
  any other diff, which silently leaves F5 showing no body, so the controller now warns loudly
  when it can't find one
- **Minecraft-style head look**: `player_model.gd::_track_head_look()` points the head at the
  player's LOOK, not the camera — it rebuilds the aim basis from the controller's world
  quaternion + pitch (pitch read back out of `get_aim_direction()` in the controller frame) and
  unwinds the head's parent transform so it composes with the model flip, body-yaw lag, and
  player yaw. Camera-tracking was fine in the back view but the 180°-flipped front-view camera
  spun the head a full turn (the bug). Pose clones and previews without a controller in their
  ancestor chain resolve the scene's live `Player` node; only scenes with no controller at all
  fall back to following the viewport camera

## Cameras, targets and overlays

- **F5 camera cycle (Minecraft)**: first person → behind → in front (face view). Both
  third-person cameras sit on the player's look ray 4 blocks out and are pulled in before solid
  terrain by `camera_clear_distance()` (0.25-block solidity samples through
  `CollisionResolver::is_solid_at`, min 0.25). Camera rotation is the player's look rotation
  directly (front view mirrors: (−pitch, π)); aim-derived rotations locked up near vertical. The
  body lags the look through the `ModelPivot` wrapper (added in `_ready`): the torso eases
  toward the travel direction at 0.3/20 Hz tick — moving or midair — and holds standing still,
  clamped so the head leads by at most ±35° before dragging it (body-yaw semantics; dials are
  `kBodyTurnPerTick`/`kBodyMaxYaw`)
- **Eye-ray block targeting**: `ChunkManager::raycast_from_camera()` casts from the player's EYE
  along the look direction via `get_aim_origin()`/`get_aim_direction()`, with a Camera3D
  fallback only for scenes without a controller — so first/back/front views always aim at the
  same block and the outline sits under the crosshair. `Main.tscn` centers the PlayerModel
  (+0.0844 z origin; pose clone mirrors it) so the head's rotation axis lands on the player's
  x/z and the eye lies on the head axis like `vanilla model`
- **Punchable combat dummy (K)**: `pose_clone_debug.gd` spawns a **rigid** copy of the player
  model (no Idle animation, no head tracking) as a physics body standing on the aimed block,
  with a bright depth-test-off cube at every mesh's origin (`pose_pivot_marker.gdshader`);
  because the glb pivots are baked, those origins are the real joints. `dummy.gd` runs vanilla
  1.8.8 living-entity movement — 20 tps gravity/drag, pre-move ground-drag read, landing
  velocity zeroing, and interpolated rendering between ticks. Left-click punches it
  (`PlayerController::try_punch_dummy`, 3.0-block survival reach against the vanilla 0.6×1.8×0.6
  box): base knockback 0.4 blocks/tick away from the attacker (+0.4 up, capped), an extra 0.5
  along the attacker's facing + 0.1 up when **sprinting** (the vanilla sprint-attack bonus), a
  10-tick hurt-resistance window so spam can't stack, held-LMB re-attacks every 0.5 s, and
  mining is suppressed while the dummy is under the crosshair closer than the aimed block.
  Console prints (`print_pivots`) list each part's origin for verifying the markers against the
  transform hierarchy
- **Chunk border overlay (B)**: `chunk_borders.gd` (autoload, `toggle_chunk_borders`) draws the
  world's chunk grid around the player as unshaded, depth-test-OFF (X-ray) lines: verticals at
  every chunk corner spanning the player's slice ± 64 blocks, and the floor and ceiling planes
  of that slice as one square per chunk, the player's own chunk in a brighter colour. It is the
  tool for boundary bugs — a seam, a stale vertex or a face culled on the far side is a question
  about which chunk owns what, and a border hidden inside rock or under a liquid answers
  nothing, which is the whole reason for the X-ray. Rebuilt only when the player's CHUNK changes
  (the grid is world-anchored and the vertical span is measured from the slice, not from the
  feet, so walking inside one chunk cannot move a line), toggled in gameplay only (mouse
  captured) like the other debug keys, and not on the settings Controls page for the same reason
  the other debug keys are not. Pinned by `probes/probe_chunk_borders.gd`: the action is
  bound to B, every vertex sits on a 32-block multiple in x/z, both slice planes are drawn
  exactly at the chunk's floor and ceiling, the mesh is parented into the loaded scene with an
  unshaded vertex-colour no-depth-test material, and the toggle hides it and redraws the same
  grid
- **Texture pack system**: Custom texture packs with per-block texture overrides loaded from
  `user://packs/`, converter tool in `tools/pack_converter.py`
- **Block outline system**: Adjustable block outline with pulse effects, thickness control
  (0.0-0.99), fill box with separate color/opacity controls. **Fully native**: the
  `BlockOutline` Node3D class (src/godot_bindings/block_outline.*) replaces block_outline.gd —
  all 16 settings are exposed properties (read/written by settings_menu.gd), raycast throttling,
  pulse animation, and material/geometry management all run in C++; mesh building lives in the
  tested `BlockOutlineBuilder`/`block_outline_mesh.cpp` core
- **Crosshair customization**: Adjustable crosshair with rotation, spacing, dot, and
  color-inversion modes
- **Shareable setting codes — one per settings category**: every category on the settings page
  (General, Block Outline, Crosshair, Advanced Rendering, Render) exports and imports its own
  settings as one compact base32 string, written by the export icon beside its title and read
  back by the import icon (`FC-ABCDE-FGHIJ-KLMNO`, `FO-...`, `FG-...`, `FAR-...`, `FR-...`). The
  prefix names the area the code belongs to, so a code is only accepted by the section that made
  it — the import reports `Invalid code format` into that section's hint line and leaves the
  settings alone. Format: a version byte, then that area's values packed as tightly as their
  ranges allow (`_pack_float16` for fractions, whole bytes for small integers, a flags byte for
  the switches, 32-bit RGBA per colour), base32-encoded and dashed in five-character chunks.
  `_decode_section_code` checks the prefix, the version and the payload length before anything
  is applied. Import then runs the codec's `refresh` callable so the rows show the imported
  values immediately, and both directions report into the section's own hint label (auto-cleared
  after 3 seconds). `_export_crosshair_code`/`_import_block_outline_code` and friends are
  hand-written per area; the three manager-backed areas (General, Advanced Rendering, Render)
  share `_decode_section_code`. Adding a setting to an area means one more field in its packer
  and one more line in its refresh closure — a General code is 12 characters, Render 26,
  Advanced Rendering and Crosshair in the 50s.
- **Controls rebinding**: Settings → Controls page with per-action rebindable keys/buttons.
  Click a binding then press a key/button to rebind; Escape cancels. Bindings applied live to
  InputMap and persisted to settings.cfg. Per-row Reset restores the pristine project.godot
  binding.
- **Controls conflict detection**: Rejects rebinds that would map two actions to the same
  key/button with on-page hints. Physical-key aware comparison ensures captures and persistence
  agree.
- **Reset All controls**: Restores every action to its project.godot binding with single button
  click. On-page hints provide clear feedback for conflict resolution and reset operations.

## Block icons

- **Isometric block icon rendering**: `BlockIconRenderer` autoload singleton renders 3D
  isometric block icons for inventory UI using a SubViewport with orthographic camera at
  vanilla's dimetric angle (45° yaw, 30° pitch). Pre-renders all blocks asynchronously at
  startup and caches results. Supports custom block shapes (slabs, stairs, walls, poles,
  lowered) by building meshes from `data/block_shapes.json` selection boxes. Icons are 300×300
  resolution with AABB centering so all blocks appear at consistent distance. Integrated into
  hotbar, inventory slots, crafting cells, and drag operations with fallback to BlockTextures
  during initial load. `/testicons` command saves test renders to `user://`.
