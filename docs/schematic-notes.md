# Block file notes (src/schematic/)

Reading and pasting the build files other tools write - the decoder, the translation
table, the planning/writing split, the in-game wand, and the probes that pin it.

## Reading both families of build file

- **Build files live in `schematics/`** (project root, plus `user://schematics/`), and are
  recognised by their CONTENTS, not their extension: `/paste <name>` tries the name as given,
  then with each of `.schematic`, `.schem`, `.nbt`, and looks in both schematics folders before
  the project root. `/paste list` prints what the folders hold. A build is named whatever its
  author called it, so the format is sniffed (dimensions + a `Blocks` array vs a palette of
  names) rather than trusted from the filename
- **Read-only decoder for both families of build file, no Godot and no zlib** —
  `nbt_reader.hpp/cpp` (a strict forward cursor over a tag tree, big- or little-endian),
  `gzip_inflate.hpp/cpp` (self-contained DEFLATE + gzip/zlib/raw framing, checksums verified,
  output capped), `schematic_reader.hpp/cpp`, which decodes both into one shape (a palette of
  states plus one index per cell):
  - **classic** (`.schematic`): a `Schematic` root with dimensions, `Blocks`, `Data`,
    `AddBlocks`, tile-entity and entity counts, and the writer's origin tags
  - **palette** (`.schem`, the newer format): named states with properties
    (`"minecraft:oak_stairs[facing=west,half=top]"`) and one base-128 varint index per cell, v2
    at the root and v3 nested under a `Blocks` compound, both reported as what they are
    (`format`, `format_version`, `data_version`, `offset`)

## Quirks of the writers we tolerate

- **Three writer quirks are tolerated by the reader, because each produces a plausible-looking
  file that decodes into silently wrong blocks**: the nibble-packed arrays (`Data`, `AddBlocks`)
  are sometimes one byte longer than the packed size (accepted, and reported as
  `data_padded`/`add_blocks_padded`); a build may be wrapped in an unnamed root (the build is
  found by shape, and the root's name is reported, not required); and the palette's first index
  may be non-zero or sparsely used (indices are compacted, and a cell pointing outside the
  palette is refused). `14664.schematic` in the schematics folder is the padded-`AddBlocks`
  case. **A fourth trap is one of ours rather than the file's**: a value being SKIPPED (an
  ignored section: tile entities, entities, the writer's metadata) still has to consume its own
  tag width, and because every width error desynchronises the cursor instead of failing where it
  happens, a Float read as one byte surfaces much later as `unknown tag type 128` deep inside a
  dense list. `10179.schematic` is the case — 1,328 tile entities and 170 entities, all floats
  and doubles — and `tests/test_schematic_reader.cpp` pins it with a fixture built for exactly
  that
- `gzip_inflate` exists because the standalone test binary and `bin/schematic_report` have no
  engine runtime and the build links no compression library — the same code path runs in-game
  and out. Godot *can* inflate gzip through `FileAccess`, so a caller that already has the bytes
  may hand them over decompressed; `load_schematic_bytes` sniffs the container itself either way
- **Report tool**: `scons schematic_report` →
  `bin/schematic_report <file> [--top N | --all] [--table PATH] [--plan] [--repeat N]` prints
  the format it detected (and the container and both array layouts, including any padded array),
  size/fill/bbox plus the full palette of states with cell counts, most-used first, and beside
  each state what this game would place (`*` = stand-in, `(liquid)`), then a one-line coverage
  summary (`2500 of 3478 cells placed (71.9%): 689 substituted, 289 skipped, 0 unknown`) and the
  `gaps` list of every state that is not an exact match with the table's reason. `--plan` goes
  one step further and builds the real paste plan, timing it (best of N with `--repeat`), which
  is the only honest way to know what a given file costs to paste. It reads the file off disk
  directly, so it works headlessly on any build

## The translation table

- **The translation table is data, in `data/minecraft_blocks.json`**: one row per legacy id,
  resolved by `mc_palette.hpp/cpp` into `Unknown` (no row — reported, never guessed), `Skipped`
  (a `"skip"` row, or a variant explicitly mapped to null) or `Mapped` (a block name, flagged
  `substitute` and/or `fluid` when it is one, and `still` for the form a paste falls back to
  when it is not asked for liquids). A `still` without `fluid` on its row is refused, because it
  is meaningless on its own and is nearly always a forgotten flag. The table also has `names`
  rows for `minecraft:water` and `minecraft:lava` as well as the numbered ids, because a
  palette-format file names its liquids: without them a `.schem`'s water resolved as *unknown*,
  which is a hole in the build rather than a decision. Rows can carry a `variants` object for
  per-data-value overrides, or name a top-level set from `variant_sets` with a `family`
  substituted into `{family}` — every stair type spells its eight orientations identically, so
  the convention lives once and a wrong pasted staircase has one place to be fixed. `family`
  with an inline `variants` object is refused rather than ignored, and a table name that is not
  a real block is caught by the tests, because both failures are otherwise silent until
  something is pasted
- **The table is read by `minimal_json`, not `godot::JSON`**: the report tool and the test
  binary have no engine runtime, so `minimal_json.hpp/cpp` reads the same file in both places
  and in the game. It is strict on purpose (trailing commas, comments, bare keys, duplicate keys
  and unbounded nesting are all refused, every error naming its byte offset) because the file is
  hand-edited, and a failed parse commits nothing — the caller's value comes back empty rather
  than half-built
- **A legacy id is not a block** — the decoder returns a palette of `(id, data)` states and one
  palette index per cell; what those ids should *become* is a data question for a translation
  table, deliberately not hardcoded here. Ids are positional (old numbered ids, `AddBlocks`
  carrying the top nibble for ids above 255), and the two arrays that pair with `Blocks` are
  `Data` (a 4-bit value per cell) and `AddBlocks` (4-bit high nibble per cell, packed two to a
  byte)

## Data layouts and cell order

- **`Data` comes in two layouts, told apart by length**: one byte per cell, or nibble-packed
  (`(cells + 1) / 2`). Guessing wrong does not fail loudly, it decodes every oriented block to a
  plausible wrong value — so the reader picks by comparing the array length against the cell
  count and refuses a file that matches neither, naming both
- **Cell order is `(y * Length + z) * Width + x`** (x fastest, then z, then y). Read the other
  way round and the build still looks like a build, just a scrambled one, which is why
  `tests/test_schematic_reader.cpp` checks all 60 cells of its fixture against the coordinate
  formula the fixture was written from rather than asserting a few ids
- **Fixtures are encoded by an independent tool**: the compressed bytes in
  `tests/schematic_test_data.hpp` come from python's gzip/zlib and a hand-written big-endian NBT
  writer, regenerated in place by `python tools/make_schematic_fixtures.py` (`--check` fails if
  the header is stale). The stored-DEFLATE case is built in C++ instead, so no separate encoder
  is trusted for both sides of a case. `tests/test_gzip_inflate.cpp` / `test_nbt_reader.cpp` /
  `test_schematic_reader.cpp` cover the block types, nesting cap, list walking, byte order, both
  `Data` layouts, `AddBlocks`, malformed/truncated files, and the checksum and output caps

## Planning and writing a paste

- **Planning a paste is engine-free and separate from writing it** — `paste_plan.hpp/cpp` turns
  a decoded file plus the table into a `PastePlan` (world coordinates + block ids) and counts
  every cell into one bucket: placed / substituted / stilled / declined_fluid /
  declined_substitute / skipped / unknown / unresolved (plus air_ignored). Policy lives in
  `PasteOptions` (`fluids` off, `substitutes` on, `write_air` off, `replace_solid` on,
  `max_cells` 0 = no cap), a stand-in that is also a liquid has to clear both gates, and
  `max_cells` REFUSES a plan whole rather than truncating it. **A liquid lands either way**:
  `fluids` off places the table's `still` form — the same substance with no fluid state, drawn
  and collided as the liquid it is and never looked at by the simulation, which is what keeps a
  pasted lake from running downhill — while `fluids` on places the live source instead. The one
  case a paste leaves out is a liquid row with no `still` form (counted `declined_fluid`), and a
  stilled cell counts as `stilled` even when its row is a stand-in, because that counter answers
  "what happened to the water" The pass is per DISTINCT STATE, not per cell: every target name
  the table can ask for is resolved once up front, the table is asked once per palette slot (a
  build repeats a handful of states across a million cells), the cells are walked flat in their
  stored order rather than re-deriving an index each time, and a name this build does not have
  is counted as unresolved rather than placed as air. Measured with `--plan --repeat 20`: 4.2 ms
  for a 1.39M-cell classic file and 1.2 ms for a 108k-cell `.schem` with 353 states, against 6.3
  ms and 3.8 ms when the table is re-asked per cell. `replace_solid` is the one option the
  planner cannot answer (it needs the world), so the writer enforces it and reports `covered`
- **The writer is `BlockEditor::apply_paste`, not a loop over `place_block`**: that one refuses
  any cell where `old != AIR && new != AIR`, so a build dropped onto terrain would silently lose
  every cell it overlaps. `apply_paste` groups the plan by chunk, takes ONE exclusive 3×3×3 band
  per chunk (rather than a lock per block), writes, then per chunk recomputes only the sky-light
  COLUMNS whose opacity changed, and afterwards persists each cell through `add_block_edit`
  (which is also what wakes pasted fluid), dirties every touched chunk for a remesh, relights
  the 3×3×3 around only those touched chunks where a written cell could have MOVED block light
  (opacity, light opacity, or either block emitting — `light_touched`), and keeps one level of
  undo in `PasteUndo`. **A region relight reports the chunks it actually CHANGED, not the 27 it
  visited.** `BlockLightRegion::modified_mask` compares each slot's light BYTES against a
  snapshot taken before the pass first wrote to it (`ChunkData::append_light_state` /
  `light_state_equals`), and `mark_chunk_dirty_for_light` marks only those: a
  clear-and-repropagate normally lands back on exactly the values it wiped, so the old "mark all
  27" rule queued 26 rebuilds of meshes that were already correct — per arriving chunk and per
  paste write. That is why loading a built-up world cost more than building it, and the region
  pass is also skipped outright where no written cell could move light. Pinned by
  `tests/test_light_region_dirty.cpp` (a repeat pass reports 0 of 27; a pass that does move
  light reports its chunks; the clear reports only real block light and leaves sky light alone).
  Two traps are worth remembering: the reported min/max must accumulate ACROSS chunks (per-chunk
  init reports the last chunk's box), and `chunks == 0` from a successful write means no remesh
  or relight was queued — the blocks would be in the data and invisible
- **Cells whose chunk is not resident WAIT for it, they are not skipped.** A build is pasted
  where the player is standing but is usually bigger than what the streaming sweep covers — the
  sweep decides from the player's position and a per-column surface band, so the sky above a
  build is never in it and anything past the render distance is out of scope — which is why the
  writer hands those cells back and `VoxelEngineController` makes them a job: the chunks are
  requested with `request_urgent_chunk` (the updater generates those FIRST, ahead of the sweep),
  they are `pin_chunk`ed so the unload pass leaves them alone, and the write is retried every
  frame with the still-unwritten cells until none are left, 30 s or 16 chunks in flight at a
  time. The writer's own list is the state, NOT the chunk map asked afterwards: a chunk can
  arrive in the middle of a write, so a cell the writer had already given up on would look
  resident and be dropped — that race lost 394,810 cells of one 937,143-cell paste, which is
  exactly what `probes/probe_paste.gd` counts. Undo is one level deep, survives a revert that
  wrote nothing, and is not replaced by a paste that wrote nothing. **A revert can come up short
  too** (a build that wide has outer chunks evicted since the paste), so it reports
  `cells`/`unchanged`/`out_of_bounds`/`unloaded` — which add up to what was written — and KEEPS
  the record narrowed to the cells it could not reach, so another `/paste undo` finishes it
  instead of leaving half a building standing. `unchanged` is a real outcome, not a failure: the
  world can have regenerated since, which puts the pre-paste block back on its own

## The in-game entry points and the wand

- **The in-game entry points**: `ChunkManager.inspect_schematic(bytes, options)` decodes and
  plans WITHOUT writing (what a preview needs, and how a caller learns the file's size),
  `ChunkManager.paste_schematic(bytes, x, y, z, options)` writes it, `undo_paste()` puts the
  last one back, and `/paste <file> [fluids] [gaps] [air] [strict]` / `/paste undo` /
  `/paste list` drive them from chat. Chat reports the stilled liquids with the word to change
  that (`add "fluids" to place the running kind`), so the default is discoverable from the line
  it affects GDScript reads the file (it is the side that knows res:// and user://) and the
  anchor is the block the crosshair is against, so where you look is where the build's corner
  goes. Every counter the engine reports is in the returned Dictionary, and both halves are
  reported separately because they can disagree: the plan counts what the file implies, the
  write counts what the world took
- **A preview is re-planned, not re-decoded** (`VoxelEngineController::decode_and_plan`): the
  wand calls `preview_schematic` at every re-aim, and the plan depends on where you aim while
  the decode (a gzip inflate plus a parse) does not — so the decoded build is kept in a
  one-entry cache keyed by an FNV-1a fingerprint of the file's bytes, which re-decodes on a
  different build or the same file edited. One entry on purpose: the wand works on one file at a
  time. `out_file` is therefore BORROWED (a `const SchematicData*` into the cache) rather than
  copied, which is only safe because `decode_and_plan` has exactly three callers and none of
  them re-enters it — a half-million-cell copy per preview would put back the cost the cache
  exists to remove. The same call returns `transforms`, the ghost's instance buffer built
  engine-side (one unit transform per cell, translation at the cell's centre), so the script
  does a single `buffer` assignment instead of a `set_instance_transform` call per cell, which
  was a hitch at every re-aim on a big build. **That buffer's float order is a knife edge.** A
  MultiMesh packs a 3D transform as three ROWS OF FOUR — each basis row followed by that row's
  origin component — so the identity diagonal sits at 0/5/10 and the translation at 3/7/11.
  Godot's own docs describe the *unpacked* `transform_array` as "x, y, z, and origin", which is
  a DIFFERENT order, and a wrong order does not error: it decodes as a degenerate basis
  somewhere off screen, so every ghost instance silently fails to appear. Measured from the
  engine, not read off the docs:
  `set_instance_transform(0, Transform3D(Basis(), Vector3(11, 22, 33)))` packs to
  `1,0,0, 11, 0,1,0, 22, 0,0,1, 33`. `probes/probe_mm_layout.gd` prints that packing and what
  each wrong order decodes to. Two rules follow for anyone verifying this: compare through a
  real MultiMesh (`get_instance_transform`), never against the floats themselves — a
  self-consistent expectation is exactly how a wrong order passes — and run WINDOWED, because
  the dummy renderer keeps no multimesh data, so under `--headless` a buffer assignment is a
  silent no-op that reads back as identity (which is how the first probe "confirmed" the wrong
  layout). `probes/probe_preview_cache.gd` does it properly: aiming A → B → A reports each
  file's own dimensions (the risk a one-entry cache actually carries), the buffer's first and
  last instances read back as the aimed cell centres with an identity basis, the renderer's own
  AABB for the instance data covers them (a MultiMesh is culled as one object, so a box that
  missed the cells would hide the ghost at some angles), and a moved aim moves the plan by the
  same amount. **The packing is checked at startup too**, because a silent wrong answer is the
  whole hazard: `ChunkManager::_ready` (debug builds) runs
  `render::verify_multimesh_instance_layout`, which packs one known transform through BOTH the
  engine and `render::pack_unit_instance_transform` — the single place the layout now lives, so
  the check cannot drift from the code that uses it — and prints both packings, the consequence
  and the fix when they disagree. It is bound as `debug_multimesh_layout_ok` so a probe can ask
  without a scene or a crash (`probes/probe_layout_check.gd`). It stays silent under
  `--headless` on purpose: the dummy renderer keeps no instance data, so there is nothing to
  compare and any "failure" there would be the harness talking
- **The wand's menu is the load menu, four times the size**: `wand.gd`'s middle-click menu is
  built from the SKIN MAKER'S LOAD MENU (the skin gallery in `settings_menu.gd`) — the same
  panel colours, the same header with the title on the left and CLOSE on the right, and the same
  square bordered cards in a scrolling grid — and every constant for it is one of that menu's
  (`MENU_UNITS_WIDE` 660, `MENU_TILE_GAP` 12, `MENU_GAP` 8, the panel's 14/10/14 stylebox
  margins plus its 8/8/6/4 `MarginContainer`, and the `PANEL_*`/`CARD_*` palette), so the two
  can be read side by side. `MENU_SCALE` is the exception: the wand's menu is read from a
  distance in the middle of the screen, so the PANEL is that menu at 4x — and 4x 660x504 does
  not fit a 1080p screen, which `MENU_MAX_FRACTION` answers by clamping it to a share of the
  window. **The panel and the content inside it are scaled APART, and that split is the whole
  point of the two constants.** Scaling the content with the panel drew a 430 px card with 90 px
  rows, which is enormous next to anything; `MENU_CONTENT_SCALE` (1.0) draws the things in the
  panel at the load menu's OWN sizes — 16 px text, 144 px cards, 40 px buttons — inside a panel
  that is 4x. (`MENU_TILE_UNITS` is the one constant that is NOT that menu's: 72 rather than its
  96, so a card is 144 px where it would have been 192. A tile there holds a rendered model in a
  full-screen gallery; here it holds a name and sits several to a row, so a quarter less is the
  same grid with more of it on screen.) So the panel is big and its contents are not, which is
  what was asked for twice in a row ("4x bigger", then "each individual box is way too big").
  Measured on a 1920x1061 window: panel 1766x976, cards 144x144, the two tabs 260x40 at 32 px
  below the panel's top edge, the close 40x40 at the right-hand end of that row
  (`textures/gui/close_button.png` on a square button — an icon, not the word CLOSE, so it is
  found by its node name `MenuClose` rather than by matching text), and the state line ending 36
  px above the panel's bottom, 44 px in from its left. Cards are square and the COLUMNS are
  computed from the room they actually have (`_tile_columns`), not hard-coded like the load
  menu's 5, because the wand's panel is whatever the window permitted. **A card's BACKGROUND is
  the game's own square button art**, not a flat stylebox:
  `textures/gui/button_square_large.png` (50x50 - the same art as the close button's 20x20
  `button_square.png`, drawn at a larger resolution so a 192 px card stretches it 3.8x rather
  than 9.6x and the bevel comes out ~4 px instead of ~10, and 50, not 20, is what to reach for
  whenever the art is upscaled this far) stretched to the whole 192 px card and deliberately NOT
  9-sliced, so the menu root's NEAREST filter turns each source pixel into a solid block and the
  bevel scales with the card instead of smearing or shrinking to a 1 px hairline. Hover and
  press are that SAME image tinted rather than second assets (`_square_style` plus
  `BUTTON_HOVER_TINT`/`BUTTON_PRESS_TINT`), which is one helper shared with the close button so
  the two cannot disagree about what a button looks like. Both tabs' cards come from
  `_make_tile`, so the schematic tiles wear it too; the `CARD_BG`/`CARD_BORDER`/`CARD_HOVER`
  colours are gone because nothing reads a flat card any more. **The top of the panel is two
  TABS, not a title**: BUILDING TOOLS (the tool cards) and SCHEMATICS (the build cards), and the
  tabs ARE the page name, which is why there is no title label at all — the tab you are on wears
  the same tint a selected card does, and the pair is rebuilt on every switch (`_refresh_menu`
  rebuilds the tab row AND the body into `_menu_column`, because a tint that does not follow the
  page it switched to is worse than no tint). Picking a tool that needs a build turns to the
  schematics tab, since the tool is no use until one is chosen. The state line —
  `build: church.schematic · 12,345 cells`, or `build: (none chosen)` — is drawn on BOTH tabs in
  the bottom left corner, one line under whatever the page put above it, because it is the one
  thing the whole menu is about; the button that used to do that job (CHOOSE BUILD), its BACK
  sibling and the tool hint are gone. The menu root sets
  `texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST`, which every Control under it inherits (a
  child's filter is `PARENT_NODE`): the interface art is pixel art and Godot's canvas default is
  LINEAR, so without it the buttons smear as soon as they are scaled up. **Every menu in the
  game sets this on its own root, and a new one that forgets it is a bug nobody sees until they
  do** — `settings_menu.gd`, `crafting_table_menu.gd`, `hotbar.gd`, `inventory.gd` and now
  `wand.gd` and `death_screen.gd` have it; the layout probe asserts the wand's root filter so it
  cannot be dropped again by accident. Four things about it were each a bug first: it is centred
  by a full-rect `CenterContainer` rather than by anchoring the panel
  (`set_anchors_preset(PRESET_CENTER)` on the panel puts its TOP-LEFT CORNER on the middle of
  the screen and grows it down-right from there, which is a dialog off centre by half its own
  size); the panel is a box of a FIXED size rather than one that grows to its content (the file
  page lists a folder of builds, and a content-driven height pushed the panel's own border off
  the bottom of a 1061 px window at 1196 px tall — the cards scroll inside it instead); the
  content width is the panel less `MENU_PANEL_PAD_H` TWICE, which is what the `_menu_content_px`
  the pages lay out against has to be (one pad short and the panel silently grew past the window
  to 1870 px); and the size is floored to whole pixels, because a fractional size centres with a
  floor and leaves the two gaps a pixel or two apart. `probes/probe_wand_shot.gd` (windowed)
  opens BOTH pages and asserts the gaps match on each axis and that no widget hangs outside the
  panel — anything inside a `ScrollContainer` is reported and NOT judged, since being taller
  than the panel is what a scrolling list is for. `probes/probe_wand_menu.gd` presses a card,
  a file tile, BACK and CLOSE for real, because a card that draws but is wired to nothing looks
  identical to a working one; run it headless for the click path and in a window for the sizes,
  since `--headless` gets a 64x64 dummy viewport where the menu is deliberately tiny (the layout
  falls back to the size it wants when there is no window to fit into, so it never collapses to
  nothing there)

## Probes

- **Verified by two probes**: `probes/probe_paste.gd` (headless) pastes a real file into open
  air and asserts the write, a repeat paste with `gaps` writing nothing, undo emptying the box,
  and the chat command's path resolution, option words and error messages — then runs every file
  in `schematics/` through inspect, paste, wait and undo, holding each one to
  `written + unchanged + covered + still queued == planned` and to the same sum on the revert.
  That accounting is the whole point of the probe: it is what proves a big build's cells landed
  (`10179.schematic`: 937,143 planned, 542,333 written across 174 chunks after ~5 s of chunk
  generation, the other 394,810 already identical to the terrain they landed in) and that
  nothing in either direction is quietly dropped. It also writes a build of its own through
  Godot's gzip writer — a stone floor with ONE water cell on it — because the bytes the reader
  is handed that way were not produced by this project's encoder: that case asserts the water
  plans as `stilled` rather than declined, lands as `surface_water`, and is STILL exactly one
  cell after 90 frames (the still forms are never touched by the simulation), then pastes the
  same file with `fluids` and waits for the source beside it to become runoff. `10179.schematic`
  is the same proof at scale: 952,704 planned rather than 937,143, and 557,894 written rather
  than 542,333 — the difference is exactly its 15,561 liquid cells, which used to be left out.
  It is also how the palette format and the padded `AddBlocks` array are held to working end to
  end; `probes/probe_paste_shot.gd` (windowed, via `run_probe_shot.sh`) pastes in front of
  the player and screenshots it, because landed blocks that never render are the failure mode a
  data check cannot see
