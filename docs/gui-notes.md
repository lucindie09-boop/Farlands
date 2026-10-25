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
- **A dropped stack comes apart too**: Q throws the selected slot's item into the world (Ctrl+Q the
  whole stack). The hotbar writes the slot back one smaller through the C++ inventory and the world
  half is `scripts/dropped_items.gd`, so the stack emptying is seen by the same spent-stack hook as
  any other spend and comes apart on its own — one shard burst per unit as the stack drains, or one
  for a Ctrl+Q dump. Nothing about the drop needs its own effect code. The throw punches the
  viewmodel — the same full-strength swing a left click makes, not the weaker place stroke —
  because nothing was put down: the item left the hand. Two drops of one kind that come to lie next
  to each other are one pile from the frame they meet: the world half adds their counts (up to the
  inventory's 64), their sizes and their motions, and the drawn size pops once as it grows (at half
  strength once the pile is already at its largest, where a merge adds no size to show).
- **A spent stack comes apart**: when the last item of a stack is used, the icon is no longer
  just gone — its own pixels are thrown up and out of the slot and fall away, through the same
  `scripts/ui_shatter.gd` the hearts use. It is in five places: the hotbar, the inventory (any
  slot emptied, including by a `/clearinv`), and the two crafting grids, where a craft comes
  apart into the cells it drained *and* the output preview once the ingredients no longer support
  it. The art comes from `scripts/block_icon_art.gd`, which samples an icon down to the 16 units
  it is actually drawn at, so a shard is the block the hearts break into rather than a piece of a
  300-unit render, and carries the colour the art had there (the iso render, or the block texture
  where a shape has no icon)
- **Debris lands instead of falling out of frame.** `burst()` takes an optional `UIShatter.Surface`:
  what the shards come to rest on. Left null they fall as they always did, which is what a caller
  wants when there is nothing underneath to land on. A Surface is one of two shapes:
  - **`Surface.box(rect)`** — a flat top and hard sides, nothing read off any art. What a slot or a
    grid wants, and all five of those are this:
    - an inventory slot's debris stays in **its own slot rect**, in both inventory grids
    - a **hotbar** slot's debris stays in **its own slot rect**, for the reason below
    - a crafting cell's debris lands on **the whole grid it is drawn in** — the 2×2 in
      `inventory.gd`, the 3×3 in `crafting_table_menu.gd` — via a `_craft_grid_bounds()` that
      merges the input cells
    - a crafting **output preview** is outside its grid, so its own cell is its floor
  - **`Surface.from_top_edge(tex, origin, texel)`** — the floor measured **off the art**, for a
    panel whose top edge is not a straight line. The hotbar's is not: it steps down two texels at
    every gap between slots, ten times across its 182 columns. A flat line along it floated up to
    three texels above the actual art at each of those gaps, so shards landed on air there.
    `rest_y()` samples the shard's own column at the shard's *middle*, so it cannot jitter either
    side of a column boundary, and a column with no ink at all is a gap the shards fall through
    (the hotbar has none; the 182-column profile is solid).

- **A floor has to be at or below where the shards spawn.** This is the whole reason the hotbar's
  spent stacks use their own slot box rather than the panel's texel-accurate top edge, and it cost
  a bug to find. Those shards spawn *inside* their slot — panel-relative y 3..19 — while the panel's
  top edge sits at y 0..3, entirely above them. Handed one, every shard fails its **first**
  collision check (`pos.y + size.y > rest`, trivially true for a shard below its own floor), so
  `pos.y = rest - size.y` yanks it up to above the bar: about 57 units at scale 3, straight through
  the hotbar's own art, which is what "teleports outside immediately and glitches" was. Nothing
  about the profile was wrong; it was the wrong floor for art that was already below it.
  The **hearts** are the opposite case and the reason the profile exists at all: they are drawn
  `GAP_ABOVE_HOTBAR` (2 texels) *above* the hotbar, so they fall onto its top edge and land on it
  texel by texel, dips included. `healthbar.gd:_hotbar_floor()` reads that profile once and keeps
  it, for as long as the scale it was read at holds — a Surface carries the origin and texel size it
  was built with, so a UI-scale change has to rebuild it. The art never changes, and reading it is a
  pass over every texel of the panel, which is not worth repeating on every hit.

- **An edge is a floor, not a box.** `Surface.has_sides()` is false for a top edge and true for a
  box, and that one distinction is three fixes at once. A box **is** the art, so its left and right
  are real edges and a shard that reaches one has run out of panel. A top edge is only the panel's
  top: off either end of it there is no art and no panel either, just open screen. Clamping there
  put a wall in the air at the hotbar's left and right ends — shards stopped dead in mid-air
  against an edge nothing had drawn, which read as a bug rather than as debris. Left open, they
  roll off the end and fall. `Surface.column_at()` answers `-1` for a shard that is clear of the art
  altogether, which is what makes that true rather than merely unobstructed: answering for the
  column nearest the end instead would wall the shards into the panel's width no matter how the
  sides were handled. A shard overlapping the last column still lands on it; only one entirely past
  the art is unsupported.

- **A shard is pinned to the column it landed in.** It rests by asking `rest_in(column)` for the
  column it settled in, not by re-asking from its current position. Re-asking let a settled shard
  drift a texel sideways, cross into the next column, and snap up onto the lip of the dip it was
  sitting in — the pop the dip's own depth makes possible, and something the eye reads as a glitch
  rather than as anything settling. The column is forgotten the moment the shard leaves the floor
  (`vel.y != 0`), so where it comes down is wherever it is by then, not where it left from.
  Settling zeroes `vel.x` too: a shard resting on an edge with nothing to stop it walks along the
  edge for the whole time it is down there, which is what walks it out of its dip and into the
  next one. Boxes are unaffected by the pinning (they are flat, so the column is always `-1`) but
  do settle sideways now, which is the same "settled" reading as before.

- **Damage impulse scales with how much was taken.** `healthbar.gd` passes
  `strength = roll_strength()` and `intensity = maxi(old_health - new_health, 1)`, both counts in
  half-hearts. The shatter is tuned so that what it throws **is** what one half-heart of damage
  throws; a second half-heart doubles it, a whole heart doubles it again. One `strength` is still
  shared across all the hearts in one hit, so a big hit throws every heart's red the same way
  rather than each heart at its own strength.
  The damage rides on `intensity`, **not** on `strength`, and the distinction is the whole point:
  `strength` multiplies only the throw, `intensity` multiplies the throw **and** the life. Folding
  the damage into `strength` scaled the arc while leaving the debris vanishing at the same rate as
  a nick, so a big hit threw further and hung around no longer than a scratch.

  **Debris draws UNDER the stack following the cursor.** `_shards.draw(self)` sits before the
  held-stack block in both `inventory.gd` and `crafting_table_menu.gd`. The stack is the thing
  being dragged, so debris painting over it hides the very item the player is moving — which is
  what picking an item up and dragging it out of a slot always looked like. The hotbar draws no
  cursor item, so it has no such order.

  `RESTITUTION = 0.35` is low on purpose: a shard keeping most of its speed bounces around the box
  for its whole life and never looks like it settled. `REST_SPEED = 14.0` is what makes "comes to
  rest" literally true — without it a landed shard is re-accelerated into the floor by gravity every
  frame and buzzes there for the rest of its life instead of settling.

- **A dropped item is NOT a shatter — it wants its own effect.** Noted here because the shatter is
  the obvious thing to reach for and it would be the wrong one. Shatter is for art that is being
  *destroyed*: what it takes is the difference between the old mask and the new one, so a stack that
  is spent, and a heart that is lost, come apart into their own pixels and the pixels are what
  remain. A dropped item is not destroyed, it is *in transit* — the player still has it, and it is
  coming back. So it should leave as **the whole item texture, intact and rigid**, not as texels.
  The shape wanted, roughly: it collides as one piece rather than as a thousand shards; it is
  **thrown out of its slot** and then falls off the bottom of the screen or tumbles away; and it
  **rotates** as it goes, because a rigid thing tumbling is the read, and a shatter cannot rotate
  because it is not one thing any more.
  It is built that way, in two halves: `scripts/dropped_items.gd` is the item itself (a rigid body
  in the world, solved against the voxels) and `scripts/ui_icon_throw.gd` is the slot's own art
  leaving the HUD whole. The open questions were settled with it, and one of them was settled the
  other way later: the nudge out of the slot began as the aim's sideways part read onto the screen
  (a drop while looking east leaves to the right), which is wrong on its own terms. The HUD is
  reacting to a *slot emptying*, and which way the player is facing is not part of that — so a
  straight-ahead drop threw straight up with no nudge at all, and every drop on the same heading
  left the same way. It picks a random side in -1..1 instead (`_throw_icon`), and the tumble is
  `CanvasItem.draw_set_transform` about the art's middle, not a second node per piece; and it simply
  falls out of frame — nothing is below it but empty screen, so there is no `Surface` to land on.
  **The throw is scaled to the icon** (`launch`'s `scale`, the GUI scale the caller passes): the
  motion's three length terms — gravity, the lift and the sideways push — are multiplied by it, so a
  UI at scale 2 throws twice as far and the icon still clears the bar it came off. The turn, the
  drag and the clock are already in the shape's own terms and deliberately do not scale, which is
  what keeps it the *same* arc at every scale rather than a different one.
  `UIShatter` did not grow it: one effect for destruction, another for
  departure. The one rule between them: a slot whose last unit left by being THROWN does not also
  shatter (`hotbar.gd`, `_thrown_out`), because the icon that went is already out there falling, and
  pixels coming apart on top of it would say the drop was destroyed.
- **A pickup is the throw's mirror, and it plays in both places the item was and is.** The body is
  handed to the inventory on the frame it is touched (so the flight can never lose it) and then
  leaves the physics list for a 0.16 s quadratic from where it lay to the player's chest, lifted
  0.45 above the straight line and shrinking to 0.15 of its drawn size (`dropped_items.gd`,
  `_begin_collect`/`_advance_collect`, `COLLECT_*`). It is out of `_items` while it flies, so the
  native solve, the pair pass, the merge pass and the pickup walk never see a body that is no longer
  there — the same rule `_remove` obeys. The receiving slot's icon then plays the **arrival pop**:
  `hotbar.gd` already compares every slot's id and count each frame (`_needs_redraw`), and a count
  going UP sets that slot's pop, which swells the icon by 22% about its centre and settles over
  0.18 s. A pickup that lands in the main inventory (full hotbar, existing stack there) has no
  hotbar slot to pop, and that is correct: nothing in the bar changed.
- **The inventory's spent *slots* are much quieter than everything else**
  (`UIShatter.SPEND = 0.3`, passed as `burst()`'s `intensity`) — the *slots only*. A slot empties as
  a stack is dragged off it, one at a time and repeatedly, and at the full throw the shards of one
  are still falling when the next slot empties: a tidy grid fills with debris that reads as clutter
  rather than as feedback. `intensity` scales both halves of the burst — the throw, through the
  `strength` it already multiplies, and each shard's life — so a quiet burst neither flies nor
  lingers, and the screen settles between drags. Everything else is at 1.0, the effect as tuned: the
  hotbar, **both crafting grids**, and the hearts. A craft is an event like the hearts; it happens
  once, it is what the player was reaching for, and its cells coming apart is the confirmation it
  worked.
  **There are two inventory grids, and both take the quiet throw.** `inventory.gd` draws its own,
  and `crafting_table_menu.gd` draws a second one inside itself, each with its own `_spend_slot()`;
  a pickup while the crafting table is open is handled by the *menu's* copy, so quieting only
  `inventory.gd` left that case at full strength. The constant therefore lives on `UIShatter`
  rather than in either file, and the quiet is passed at the `_spend_slot` call sites — not inside
  the shared `_burst_from()` — because both files' `_burst_from()` also serves their crafting
  boxes, which are the case that must stay loud
- **Health bar**: `healthbar.gd` draws 10 hearts (`heart_full.png` / `heart_half.png` /
  `heart_empty.png`, 9×9) floating above the hotbar's left edge; each heart is its own 9 units
  on an 8-unit pitch, so a heart's right outline column is the next heart's left one and the two
  share a single border pixel (the row spans 81 units), placed through `UIScale`'s helpers and
  NEAREST like every other HUD surface; polls `PlayerController.get_health()` (half-hearts
  0–20) and redraws only on change
- **Air bar**: `airbar.gd` draws ten bubbles (`bubble.png`, 9×9, generated by
  `tools/make_gui_textures.py`) above the hotbar's right edge, the mirror of the hearts — same
  9-unit sprite on an 8-unit pitch, right-aligned within the hotbar's 81-unit row, on the same
  baseline, placed through `UIScale`'s helpers and NEAREST. The row is drawn only while the
  breath is not full (`get_air()` below `get_max_air()`), each bubble one tenth of the maximum,
  and a depleted slot reuses the same art under a dark modulate; polls `get_air()`/`get_max_air()`
  and redraws only on change, like `healthbar.gd`
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
  on-screen width — about 7.2 units for a 9-unit sprite — and drawn with
  `TEXTURE_FILTER_LINEAR` in an otherwise all-NEAREST HUD. A heart is now its own 9 units on an
  8-unit pitch, so neighbours share their outline column (the row spans 81 units, about 44% of
  the bar, rather than the ~43% the fraction gave) with NEAREST, so the sprite is never resampled.
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
  that draws ONE slightly-enlarged transparent cube per **selection box** of the mined block's
  resolved shape (`ChunkManager.get_selection_boxes_at`, the same boxes the outline uses, so a
  stair cracks on both of its boxes and a pole on its post rather than on the whole cell) and
  swaps the 10 stage textures (`textures/animated/l0_sprite_01..10.png`); faces against solid
  neighbors are depth-occluded so cracks only show on visible faces. **Progress resets on release** (releasing
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
  byte-for-byte copies now call the one C++ builder. Shaped meshes wind every quad inward-facing
  with its normal named for the wall it sits on (the +X face used to mirror -X and left a hole
  in the right side of every dropped/held shape) and sample the per-face texture slice a placed
  slab or stair does — the partial-block UV walk in `mesh/mesh_builder_faces_aabb.cpp` — so a
  half-height step shows its own half of the texture instead of a squashed full copy; the sprite
  mesh (front/back + silhouette rims) is locked byte-for-byte against an independent Python port
  of the original GDScript in `tests/test_viewmodel_meshes.cpp`.
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
  75% endpoint strength. The drop is the opposite case: Q **punches** (`hotbar.gd`,
  `_drop_selected`), so the hand swings full strength as the item leaves it.
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
  **The body runs the world's light, not the engine's**: `player_model.gd` gives it one
  `ShaderMaterial` of `shaders/item_shader.gdshader` — the same include the dropped items and
  the held arm run — over every surface, and pushes the light of the cell the body's *middle* is
  in (read from the model's own geometry, so a re-modelled body moves the sample with it) plus
  the world uniforms once a frame, easing the cell light over 0.15 s exactly as the other two
  consumers do. The engine's lighting is what this replaces: `Main.tscn` has no light node at
  all, so a body left on it renders near-black at noon and identically black at midnight. A body
  inside a **SubViewport** keeps the engine's lighting, because that is a preview — the skin
  maker and the settings gallery put the model in a world of their own with an orbiting camera
  and their own sun and fill, and there is no cell under it to read. The K-key pose clone is a
  plain instance in the main viewport, so it is lit like the player's own body
  (`probes/probe_body_light.gd` reads the rendered frame back).
  **...and it is the one consumer that does not take the world's midday tint**: the world's
  `sky_light_warmth` is a warm cream even at noon — it is what the sky, the fog, the sun light
  and the terrain are all fed, and it reads as nothing on grass, dirt and stone. On a pale,
  untextured skin that same cream reads as orange, so `player_model.gd` overwrites the uniform on
  the body's material with the sun's own colour — white overhead, still warm at the horizon, from
  `ChunkManager.get_body_sky_warmth` — after the world's value has been written. The fix is the
  body's alone: at any hour the ground keeps the cast it had, and the two curves meet at the
  horizon, which is why a sunset still lights the body exactly as it lights the ground
  (`probes/probe_body_light.gd` reads both materials, at noon and at the horizon).
  **The body is a scene node, not code**: `Main.tscn` instances it as `Player/PlayerModel`
  (scale 0.05625, +0.0844 z, `player_model.gd`), and `PlayerController::_ready` only *finds* it by name and reparents it under
  `ModelPivot`. An editor re-save of `Main.tscn` can drop the node and its ext_resources without
  any other diff, which silently leaves F5 showing no body, so the controller now warns loudly
  when it can't find one. That is what happened on 2 Oct: the scene's `PackedScene` reference
  still carried the uid `player.glb` had at the repository root, and that uid lives on only in a
  leftover root `player.glb.import` whose `source_file` (`res://player.glb`) no longer exists —
  it never named `models/player.glb`. The node and both of its ext_resources were gone from the
  saved scene, while the dummy-spawn button kept working, because that preloads the model by
  *path* and never touches `Main.tscn`. A `Main.tscn` reference to this model must therefore
  carry `models/player.glb`'s current uid (and its path), and no `.import` for a source that does
  not exist belongs at the repository root
- **The body's animation is a hand-port of FA+Player, and it is code rather than clips.**
  `scripts/body_animator.gd` computes a pose per frame from what the sim is doing -- horizontal
  speed and vertical velocity (`get_velocity()`), `is_on_floor`, `is_in_water`, `is_sneaking`,
  `is_sprinting` -- and `player_model.gd` composes it onto each part's rest rotation
  (`rest * X * Z`, degrees per channel), so no pose can drift and a part the pose does not name
  stays exactly as the model shipped. It replaces the clip loader entirely: the body plays no
  `.anim` any more. `Animations/Idle.anim` and the parked `Idle_new`/`Walk` pair stay on disk and
  none of them is loaded; the `AnimationPlayer` node in `Main.tscn` is now unused.
  **What was ported, and from where**: the signature moves of Fresh Animations' player add-on
  (FA+Player v1.1, CEM animations by FreshLX) -- the idle's arm sway, the opposed leg/arm stride,
  a sprint's lean, the split legs and raised arms of a jump and a fall, a sneak's crouch and a
  swim's paddle. The pack is not a set of clips either: its `.jem`/`.jpm` files (677 expressions
  over 27 parts) recompute every part from live state each frame, so a port of it is a pose
  driver, not a file import. **Credit is required and its files are NOT in this repository**: its
  terms allow transforming its work with credit but not redistributing unedited assets, and this
  repository is GPL, so nothing of the pack is vendored -- the moves are authored here.
  **The model is this body's, not the pack's.** FA+Player also models the jacket, sleeves, pants,
  headwear, cape and a first-person hand; `player.glb` has head/torso/arm/arm2/leg/leg2, so the
  limb motion ports and the clothing layers do not, and the head stays the code's
  (`_track_head_look` owns it, and no pose channel names it).
  **How the pose is weighted**: one pose per state, each claimed by an eased weight, with the
  walk's amplitude (`_swing`, from speed against `WALK_SPEED`) inside the pose and the weight
  only choosing the state -- scaling both by speed squared the stride, and a creep at a third of
  walking speed swung a ninth of the way, which read as no motion. A stop and a landing are the
  same weights crossing back out at `EASE_RATE`, which is what stops them reading as a cut.
  Measured by `probes/probe_player_anim.gd` (24 checks): the idle keeps the old clip's 3.4
  degrees of arm sway, a walk is 60 degrees of opposed stride with each arm against its own leg,
  a sprint is 1.5x the stride and 9.6 degrees of lean, the jump/fall/sneak/swim shapes differ as
  they should, a stop decays instead of cutting, the live body's own arms sway while it stands
  there, and the K-key pose clone stays frozen.
  **The CEM files** live outside the project (`FA+Player-v1.1/assets/minecraft/emf/cem`), and two
  things in them are worth knowing before revisiting: their tracks name the pack's rig
  (`Node/Head2`, `Node/RightArm`, ...) in its units, and that rig faces -Z where this body faces
  +Z, so a copy of its numbers is that motion seen from behind.
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

## The title screen and its transition

- **The title is an autoload layer, not a node in `Main.tscn`.** Seventeen probes instantiate
  `Main.tscn` by hand and then drive the player; a title screen living in that scene would freeze
  every one of them. `menu_layer.gd` arms itself only when `SceneTree.current_scene` is that
  scene, which is the one property a normal boot has and a hand-made fixture does not.
- **A title button is still a button.** The rows were designed at 240x28 units with 12-unit
  labels, against the 200x20 and 8-unit rows the death screen and the settings menu draw from
  the same `button.png` - so on the title they were 40% taller and half again in the text, and
  the focused row was near-white while the other two were dark blue-grey. Read together that is
  one bright slab among two placeholders, in an interface that does not match the game behind
  it. They are now the house row at the house fill: the same 200 x 20 units and 8-unit labels,
  the texture at its own grey with white labels, and the selected row lifted to 1.10 with the
  accent bar rather than becoming a different button. Darkening the three rows to sit them into
  the plate was tried between those two and looked like a dark-mode menu: the game's buttons are
  all this one light texture, and the title is the first screen, so it should show that
  interface rather than a variant of it. The row size is not a matter of taste here either -
  the texture, font and scale are already fixed by every other screen.
- **The row you are on turns its own colour.** Play goes green, Settings steel, Quit red, so the
  row says what it does before it is pressed; Quit is already two presses, and this is a second
  warning rather than the only one. Two details are not obvious from the code. The tint is a
  MULTIPLY of the texture's own fill (`#6d6d6d`), so a target hex is divided by that grey - the
  green's modulate is 1.36 in green, over 1 and fine, and that is the only way a grey texture
  reaches `#68944e`. And **Godot draws the focus stylebox OVER the state's box**, so a filled
  focus hides the pressed state entirely - the row would look identical held and hovered. Focus
  is therefore an empty box and the accent bar carries selection; `probe_menu.gd` checks that
  rather than trusting it, alongside multiplying each row's modulate back to its hex. At full
  strength the green and the red read as signals on a dark plate (a status light and an error),
  so both are pulled a third of the way toward the button's grey before use.
- **The column starts under the wordmark, and the gap is a bug once you can see it.** At a first
  row of 168 units the band between the wordmark's underline and CONTINUE was 62 units - wide
  enough to read as a missing row, which is what a screenshot showed it as. It is 14 units now
  (28 px at 1080p), so the underline works as the list's own rule and the wordmark and the five
  rows read as one block. `probe_menu.gd` bounds the gap rather than pinning the constant, since
  the point is the proportion on screen and not the number in the file.
- **SINGLEPLAYER is a page, not a screen.** The world list swaps the column and the footer on the
  same `Control`: same scrim, same wind, same row factory, same focus machinery, one `_page`.
  A second scene would have to copy all of that, and the copy is what drifts out of step. The
  list itself is handed in by `menu_layer._worlds()`, which is the side that knows where a save
  lives and the only thing that has to change when the engine can hold more than one world.
  Two states are worth having in code: BACK rebuilds the title column and restores the launch's
  own footer, and Escape is the same gesture, handled by the screen because the list is the
  screen's own state and there is nothing else for Escape to mean while the player is frozen.
- **MULTIPLAYER says so instead of doing nothing.** There is no netcode, and a row that swallows
  a press reads as a bug. It puts `MULTIPLAYER IS NOT IN THIS BUILD YET` in the footer for three
  seconds - the footer is already this screen's status line - and clears itself on a timer, so
  the message cannot outlive the moment it answered.
- **A row shows where you are with its text, not with a box.** The first version of the flat rows
  gave hover and focus a 5% white fill, which inside a dark panel draws a lighter rectangle around
  whatever the pointer is near - the panel painting a second panel. Rest is now the recessed grey,
  the current row is white with one 2-unit accent bar on its left edge, and pressed turns the label
  the accent colour for as long as the finger is down. Nothing changes shape on hover.
- **The right-hand column is one stack of three panels, and the profile card's head has ONE air
  value on all four sides.** Profile card, friends panel, log, one cell apart, all the column's
  width and all right-aligned. Each panel's height is its content rounded UP to whole cells, so the
  spare has to go somewhere, and the card went through three answers before this one. Halving the
  spare above and below the content left the bottom gap wider, because the last row's label is
  centred in its 11-unit row and that slack lands under the text. Splitting the card into three
  equal gaps (above the head, head to first row, under the last row's text) then made the *header*
  read low: the rule sits half a gap under the head, so what the eye measures is 20 units above the
  face and 10 below it, and the side inset was 9 against a 20-unit gap - closer to the edge than to
  the top or bottom, which is exactly what the next report said. The model is now:
  **`PROFILE_AIR` (5 units) is the head's air on all four sides and the rows' left inset**, so the
  face and the labels share one margin and the body air is the same above and below it; the header
  band ends one `air` under the head at the rule, and the spare the cell rounding leaves below the
  rule is split evenly between the first row and the bottom edge. The labels' text is then the same
  distance from the rule as from the card's bottom. `probe_menu.gd` measures all four of the head's
  gaps and the two label gaps from the drawn geometry and fails if either pair differs by more
  than a pixel. `_friends_boxes()` exists so the layout and the drawing compute a panel's interior
  once instead of twice.
- **A hovered profile row gets a chevron BEFORE its label, in a gutter it reserves at rest.** The
  row already turns its text from recessed grey to white on hover; the arrow adds "this one goes
  somewhere" without a fill, which is the rule for every flat row on this screen. Two details make
  it not move anything: the label's content margin is 6 units (the arrow's width plus a gap) so the
  chevron has its own column even when it is absent, and the arrow is drawn on the row's own left
  edge - the head's line - rather than after the text, which is what the first version did. Hover
  state is `_profile_hover` on the node, fed by `mouse_entered`/`mouse_exited`; the EXIT is filtered
  by row index because Godot can deliver the exit from row A after the enter of row B, and an
  unfiltered exit would take the arrow off the row the pointer is actually on.
- **The friends panel is the shell, and its list is empty because there are no friends.** The
  reference is a social panel with a live count, tabs, a search field and a friend per row; what
  this build can honestly have is the chrome - header, search, tabs, Add Friend - over an empty
  list that says `NO FRIENDS YET / MULTIPLAYER IS NOT IN THIS BUILD`. It sits between the profile
  card and the log rather than in the bottom-left corner, because the corner is part of the
  world's picture and the right-hand column is where this screen's information lives. The state in the header is
  `Not connected` rather than a number, because there is no service to count. The tabs and the
  search field are wired rather than decorative: a tab is state on the panel, and the field
  filters the list, so the empty state changes to "nothing matched" - the code is the same code a
  full list would use. What is deliberately NOT written is a friend-row renderer: rows arrive
  with multiplayer, and a renderer for data that cannot exist yet is a guess at what it will look
  like.
- **The profile card is a placeholder with nothing to lie about.** There is no account system and
  no network, so the name is a constant and the state says `Online` because that is what the
  reference has - the card is a piece of furniture for the layout, and the one thing it does
  honestly is draw the head from the player's own `textures/skin.png` (the 8×8 front face at (8,
  8), nearest-filtered), so it cannot disagree with the body. Its three rows are real `Button`s
  with flat styleboxes rather than `button.png`, because a grey button inside a dark panel is a
  second, louder interface; they are deliberately NOT in `_buttons`, which is the action column
  and whose index positions the accent bar. Each row answers in the footer - the same channel
  Multiplayer uses - until there is something behind it.
- **The right-hand side is one stack, not two islands.** The profile card sits on the wordmark's
  line and the log starts one cell under it, both the column's width, both on the cell grid: the
  left edge of the screen is wordmark and rows, the right edge is card and log, and the probe
  holds the gap between them to two cells so they cannot drift apart.
- **The log is the column's own width, and every edge of it is on the cell grid.** The width and
  the right edge are both rounded to the nearest cell - which can move either by up to half a
  cell and never cuts a square in half - and the probe checks all four edges against the grid,
  plus the width against `COLUMN`, rather than trusting the look. It has been placed in three
  corners while this screen settled (top right, bottom left, and now the right-hand column under
  the profile card); the grid and the width are what survived every move, so they are what is
  pinned in the probe.
- **The card is one flat fill; the noise and the wind are the plate's.** The noise exists to break
  up a gradient, and the card has no gradient - so per-square noise there only read as a patchwork
  of squares sitting slightly proud of each other, which is what "its squares are flying away"
  looked like. One fill makes it a surface, and the grid is still honoured by its edges.
- **The update log is a card, because the sky is the brightest thing here.** The right-hand
  emptiness was the composition's dead space until something went in it, and plain text cannot:
  grey labels on that sky measure about 1.1:1. So it is a card of the scrim's own squares at
  0.52 of the tint, which composites the brightest sky down to a measured **4.75:1** against the
  entries - the AA figure for normal text - and the probe recomputes that ratio from the two
  constants rather than trusting the design. The entries live in `data/changelog.json`, and the
  probe also checks that the file's `version` matches `application/config/version`, so a build
  cannot advertise another one's changelog. The card is dropped, not squeezed, when the window
  is too narrow for its lines: a changelog narrower than its own words is worse than none.
- **The card is framed in the button's bevel, at one texel.** Read out of `textures/gui/button.png`
  rather than matched by eye: `#262626` outline, `#9b9b9b` lit top and left, `#424242` shaded
  bottom and right, over the fill - a three-tone bevel with the light coming from above, drawn
  procedurally as four rects per ring. It grows with the GUI scale, so a texel is 2 px at scale 2
  and the frame thickens with the rest of the interface instead of staying a hairline. The lit
  tone is the one that did not survive the move: `#9b9b9b` is a +42 step over the button's
  `#6d6d6d` fill and reads as a bevel there, but over a nearly black card it read as a bright
  outline drawn round the panel - out of place in a screen of quiet greys - so it is pulled to
  `#686e76` (about 0.67 of the texture's tone, and cooler). The probe reads the outline and shade texels back and compares them to the
  constants, and holds the lit tone below the texture's while keeping it above the shade, so
  the frame can be tuned without ceasing to be the button's bevel.
- **The freeze reuses `set_settings_open`.** That flag is already the gate every system respects
  (movement, look, mouse capture, the block outline, the crosshair, the compass, the interaction
  door), so the title cannot be the one overlay a later system forgets. The cost is a semantic
  one: `is_settings_open()` is true while the title is up, so a reader of either side should know
  that. The alternative — a second flag — buys a cleaner name and a new way to fail.
- **`world.meta` always exists by the time the title is up.** The world behind the menu is
  already running and writes the file on boot, so asking the filesystem then reports CONTINUE on
  a first run. The menu layer reads the file's existence in `_ready`, which is the moment before
  the scene is instantiated; `PLAY` comes from that value, not from a later check.
- **A Control under a `CanvasLayer` has no parent rect.** The full-rect anchor preset leaves its
  size at zero (measured: 64×64, the headless viewport's default), so both the title and the
  curtain take their size from `get_viewport().get_visible_rect()`, follow `size_changed`, and
  re-fit on every open — a window resized while the title was hidden fires that signal at a
  Control nobody is listening to.
- **`preload` compiles before the autoloads exist.** A script that names the `UIScale` singleton
  cannot be preloaded by a `--script` probe: the compile fails, the const becomes null, and the
  cached failure then poisons the autoload's own copy of the same class. `probes/probe_menu.gd`
  loads the curtain at run time for that reason, and the failure looked like two unrelated bugs
  until the two were connected.
- **A pile of driven squares shivers until the solver can see it land.** The curtain is real
  physics, so its failure modes are physics failures. Three of them looked like the same bug and
  were not. A thin floor at 60 Hz was stepped through (a block arriving at 3,100 px/s travels 52
  px between contact checks); a box exactly the screen's width put the pile under compression,
  and a compressed pile reports a few hundred px/s downward while its position does not change;
  and `RigidBody2D`'s own sleep is island-wide, so one block still grinding keeps every block it
  touches awake. The three fixes are one shape: a 300 Hz rate and CCD so the landing is seen, the
  collider at 0.86 of the drawn square so the packing has room, and a per-block landing rule that
  freezes a block into the pile once it has stopped drifting — frozen is static, so the next
  landing lands on a wall rather than on a negotiating crowd. Deepest collider overlap went from
  63 px to 5–7 px of a 138 px block, and the pile holds with a worst-case motion of 0.00 px.
- **Weight is the absence of help.** The first version looked light for a reason that had nothing
  to do with mass: the blocks were released with an initial downward speed, the linear damping
  braked them on the way down, and they spun at up to 4.5 rad/s. Fast rotation is the strongest
  cue for "small", and air drag is a feather's property. They are now let go at rest, damping is
  nearly gone (0.08 linear) so the whole of the acceleration is gravity, the tumble is 0.3–1.4
  rad/s, and friction is 0.98, so a block stops where it lands instead of sliding. The landing
  speed is unchanged in feel because it comes from gravity alone.
- **A square scrim is solid because it is one fill, not because its neighbours are far away.**
  The title's backdrop was horizontal bands plus a dozen tall plate rectangles, and the one-pixel
  overlap each band had to stop seams showed as a lattice of rectangles. Two follow-up mistakes
  are worth keeping: drawing a darker square under each cell to make the grid visible put a
  one-pixel line around every square, which read as a grid drawn on the screen rather than as
  tiles; and quantising the tone into a handful of steps made the grid obvious at the cost of the
  gradient itself - the darkening became five uneven terraces. What it is now: one cell size
  for both axes, one flat fill per cell sampled at its centre, no second layer and no rounding.
  The scrim itself is a single plate: 0.58 at the left edge, easing to nothing just past the
  buttons, and no darkening anywhere else on the screen. Holding it flat to the column's edge
  and dropping it over a short tail was the last version and it read as a blob of four colours -
  one tone across 90% of the plate and two or three cells of tail - so the ramp across the
  plate's own width is what gives it the 23 tones the probe counts at 1080p. `probe_menu.gd`
  asks the tone function for those numbers directly rather than re-deriving the arithmetic,
  which is the only way "does it end where it should" can be a test instead of an opinion.
  On top of that ramp the plate carries NOISE, and the noise is per square: each cell gets its
  own offset of ±0.06 alpha, keyed on (col, row) so it survives a resize, and scaled by how much
  plate is at that point so the lit end of the falloff is not noisy and the dark end is not
  patchy. That is the difference between texture and a field: a screen of flat squares reads as
  squares, so what varies the plate has to vary per square - a smooth value-noise wash laid under
  the grid (two octaves, drifting upward, tried first) looked like a printed gradient with a
  hidden second grid in it, and was replaced. `probe_menu.gd` asks for a 6 x 6 block of the
  plate's own tones: a ramp has six (one per column), noise has thirty-three.
  The animation is the plate coming apart: every 0.09 s one square is picked at random from the
  plate, leaves its hole behind and is carried to the right at 7 cells/s with a little vertical
  drift, fading out over the last 45% of its 1.1 s life. The hole fades back in over half a
  second, so the plate is a surface with wind on it rather than one being eaten away, and a square
  in the air stays a square - same size, same flat tone, just moved. Picks are spread over the
  whole plate rather than along its right edge, so the wind arrives from everywhere; a square
  already in the air, or still healing, is skipped, so the plate never tears open. Reduce Motion
  stops the wind and keeps the noise - the plate becomes a still texture, which is what the
  setting promises - and the probe checks both halves: squares in the air, a lifted square moving
  right, holes healing behind them, and nothing lifting at all with the switch on.
  The scrim was drawn as one baked texture for a while, so that the whole plate was a single
  draw call. That path is gone, and the reason is worth keeping: **a tiled texture draw does
  not stretch a texture, it repeats it at its native size**, so a 72 x 40 texel tile across
  1920 px became fifty 72 px bands of the plate - vertical stripes with the world showing
  between them. A moving, per-square effect wants per-square drawing anyway; the grid is a few
  thousand `draw_rect` calls, which is what this screen did before the bake.
  Three darkenings were tried and removed along the way, and each looked reasonable on paper:
  a vertical gradient (it darkened the world at the top for nothing), a frame vignette on all
  four edges (with only one plate, the edges it darkened were the ones with no interface on
  them), and a soft oval behind the rows to lift them off the plate's falloff (once the plate
  is full strength to the column's right edge, there is nothing left for the oval to do). One
  darkening where the interface is beats three overlapping ones.
- **A pile does not tile, so the wall stops being a pile.** Rotated squares leave hairline cracks
  at their seams, and no amount of settling closes the last of them. Each square therefore swells
  where it landed until the wall is covered, and then the pile is swapped for a lattice that tiles
  the screen by construction. Both halves are invisible under pure black, and the size is measured
  rather than assumed because the pile is different every run (it has needed 1.6 to 1.9 of a cell
  to close).
- **A sampled coverage grid is a check that passes a hole.** The swell used to stop when a stamp grid
  reported every cell covered, and a cell is twelve pixels wide: a crack narrower than that could
  run between two sample points and survive to the swap, where it closed in one frame - a line of
  world flickering shut. The answer is the question the eye asks, per horizontal line across the
  screen: the union of the x-intervals the squares cover, against the width. Each square's
  intersection with a scanline is one interval (both of its local-axis constraints are linear in x),
  so it is two native sorts and a merge per line, which runs in a few milliseconds at 1080p. The
  measure is a count of uncovered *lines*, not a ratio, because a ratio of 0.9999 is still a crack.
  Its one trap: the merged length is what IS covered, so the gap is `width - reach` - the first
  version accumulated `width - reach` as the covered total and reported 0.0 coverage on a wall with
  no gaps at all, which the probe caught immediately because it printed the number.
- **A transition that can stall will stall on the one player who is not clicking.** Reported as
  "if the only click was Continue, the growing part never happens and the blocks only drop when I
  click": the curtain was sitting in its cover phase, so the swell (which lives in the hold) never
  ran, and a press was the only thing that could move it on. The cause is environmental - a first
  entry has the world streaming in, so frames are long, and at the transition's 300 Hz the default
  eight physics steps per frame turn the fall into slow motion, during which the pile never reports
  itself settled. Three changes, in order of importance: the cover and the hold are driven by
  **physics ticks** rather than the render loop, so neither can be starved by a frame; each phase
  has a **wall-clock fuse** (4 s to settle, 2 s to close) that moves it forward regardless; and the
  engine's `max_physics_steps_per_frame` is raised with the rate, so slow frames no longer slow the
  fall down. `probes/probe_menu.gd` sabotages a wall that cannot close and asserts it still covers
  with no input.
- **The skip throws the wall; it does not cut to black.** The early exit is a click's own position
  becoming a blast: the lattice goes up first (invisible, same pure black) and every cell gets an
  impulse away from the press, with its own spin and drag. The lattice is what is thrown rather than
  the pile, because a skip can land while the sheet is still in the air above the screen - throwing
  those would be a blast of off-screen blocks and an instant, invisible end. The first version did
  exactly that and the probe measured 0 of 96 blocks moving, all of them already outside the
  viewport.
- **The drop has to be per cell, or it is a rectangle.** A lattice of black squares that leaves as
  one sheet has nothing in it for the eye to read as cubes — it is a sliding black bar. Each cell
  departs on its own beat instead, bottom rows first, with a little drift and spin, so the wall is
  eaten away from underneath and the world shows through the gaps. The same reason applies to the
  fall: a tidy grid released at once falls as one object, exactly, so the sheet is spaced wider
  than a block, bonded half a row, and jittered per column before it reads as blocks at all.
- **"Settled" has to be measured where the eye measures it.** The internal pile keeps squashing
  for a while after it looks landed, and a velocity test calls that "still moving" forever. What
  is visible is the top edge, so that is the criterion: the edge stops climbing for 0.3 s while
  90% of the sheet is on screen. Below the surface, black on black, nothing can be seen to move.
- **A probe can measure a dead harness.** `_ready` runs on the first idle frame, so a probe that
  adds the curtain, sizes it and starts it in the same frame gets a node whose own `_ready` then
  calls `set_process(false)` — the bodies still fall under physics, so it looks alive while the
  phase machine never advances. One `await process_frame` before sizing is the fix, and the
  give-away was a settle that never happened with every body at exactly zero velocity.
- **Reduce Motion lives where the settings live.** `settings_menu.gd` owns the switch and writes
  it to `settings.cfg`; `menu_layer.gd` reads that file rather than reaching into the menu scene,
  so the transition works whether or not the settings screen has ever been opened.
