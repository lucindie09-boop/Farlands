# UI (GDScript)

The Godot-side UI: what each script owns, and the pieces of it that are more than a
screen (the shader-effect stack, the animated-liquid tools, the model previews).

## HUD, chat and menus

- `chat.gd` — Chat system with autocomplete: ghost text suggestions with pulsing effect, tab
  cycling through completions, up/down arrow navigation, hold-to-cycle, parameter hints for
  commands, command execution (`/help`, `/give` with unlimited count, `/tp`, `/fly`,
  `/clearchat`, `/clearinv`, `/version`), mouse wheel scrolling for chat history, caret blink,
  wrapped messages with proper input box anchoring
- `hotbar.gd` — Hotbar UI with mouse wheel cycling, click-to-hold block selection; when the
  last of a stack is spent, the icon it was drawn as comes apart into its own pixels through
  `ui_shatter.gd` (sampled down to the 16 units it is drawn at, so a shard is the same block a
  heart breaks into)
- `ui_shatter.gd` — The shatter's motion, shared: a caller hands over a piece of art's texel mask
  (built by `mask_from_texture`, or a `freed` difference of two masks for what one hit took
  away), where the art is drawn and in what colour, and the helper throws one shard per texel up
  and out, drops them by gravity and draws them from the caller's `_draw()`
- `block_icon_art.gd` — Block icons as pixels, for every surface that shatters one: the isometric
  render (or the block's own texture) sampled down to the 16 units an icon is actually drawn at,
  so a shard is a whole block of the icon rather than a piece of a 300-unit render, cached per
  block id and carrying each texel's own colour
- `healthbar.gd` — Health bar UI: 10 hearts above the hotbar's left edge, each its own 9 units
  on the art's 10-unit pitch, full/half/empty sprites resolved from the half-heart count polled
  off `PlayerController.get_health()`. A hit that takes red off a heart drops it: every red texel
  the old state had and the new one doesn't leaves as a shard of its own through `ui_shatter.gd`
  (see [gui-notes.md](gui-notes.md))
- `death_screen.gd` — Death overlay: "You died!" + Respawn button, shown on the
  `PlayerController.died` signal and hidden on `respawned`
- `inventory.gd` - Full inventory screen with drag-drop stack movement, shift-click
  quick-transfer, RMB drag-place, LMB drag-collect, scroll wheel quick-transfer, double-click
  gather; live 2×2 crafting grid + output preview (click/drag/shift/scroll interactions mirrored
  on the crafting cells; shift-click output crafts as many as possible). Icons are tracked per
  slot, and one that is spent -- a stack emptied, a cell drained by a craft, a preview the
  ingredients no longer support -- comes apart through `ui_shatter.gd` (see [gui-notes.md](gui-notes.md))
- `data/recipes.json` — Crafting recipes (shaped/shapeless), resolved by block name; loaded into
  `RecipeBook` at startup. A shaped `key` entry may list several acceptable ingredients,
  expanded at load into one concrete recipe per combination (per symbol, so all cells of a
  symbol are the same ingredient) — the matcher, the preview gate and the consumption path stay
  id-exact
- `settings_menu.gd` — Adjustable settings with persistence (render, lighting, crosshair,
  controls) opened with Escape key; includes a **Skin Maker** page (color wheel, hex readout,
  orbitable preview) with a dark-mode toggle and a **Block Maker** page (16×16 cube painter)
  with paint tools, noise slider, and gallery. Escape opens a bare pause menu (Resume / Settings
  / Shaders / Controls / Tools over the dimmed world, no chrome) and **Settings** is one
  `_build_scrolling_page` stacking the categories General, Block Outline, Crosshair, Advanced
  Rendering and Render with a transparent content box (Controls and the Tools launcher have
  their own pages from the pause-menu buttons; Shaders lists the screen-shader stack with one
  switch row per shader and a `settings_button.png` icon on each row that opens that shader's
  own page, where its uniforms live — all of it read out of `data/shaders.json`, down to which
  pages exist): a title bar, a centred content box, an action bar, and a vertical scrollbar. A
  resettable row's reset is the square `undo_button.png` icon (20 units, 1:1 with the row
  height); a transparent content box draws no border, which is why the settings/controls/tools
  pages show no grey frame over the world. Each category heading is an HBox: beside the title
  sit square export/import icons (`export_button.png`/`import_button.png`) and a reset-all icon
  (the same `undo_button.png`), all at `UNIT_HEADING_ICON_W` (half the row reset) and tight
  against the non-expanding title. Export, import and reset all are all wired: each category
  carries a codec (its own `FG`/`FO`/`FC`/`FAR`/`FR` code plus a refresh callable that resyncs
  its rows), and the three icons beside its title drive it; reset all replays the section's own
  row resets. Every settings area (GUI, lighting, video, controls, crosshair, block outline, the
  two editors) is a section of that page, each introduced by a `"category"`-marked section
  heading — the reference interface's Video Settings layout, with the areas stacked instead of
  split across screens; the crosshair category interleaves its Cross and Dot rows so each reads
  as one full column. Section builders return row arrays (`_build_lighting_sections()`), so a
  setting is one row and an area is one more section. Every size is a `UNIT_*` constant times
  `_ui_scale()` (one unit is one GUI-scale pixel, so a 200x20-unit button is 400x40 px with 16
  px text at GUI scale 2), which is what the maker pages' chrome and the gallery cards were
  brought onto; nothing in the menu is sized in raw pixels any more

### One GUI scale, in units

Every HUD surface is authored in **units** and multiplied by one integer scale. One unit is one
logical pixel, so at GUI scale k a widget `n` units across is `n * k` screen pixels: the whole
HUD grows together and nothing lands between two pixels. `scripts/ui_scale.gd` (the `UIScale`
autoload) owns the scale and the two origin helpers.

- `set_scale()` is the only writer. It rounds and clamps — the menu offers 1x..4x — because a
  fractional scale resamples every texture and every font.
- `centered_origin(viewport_px, panel_units)` and `edge_origin(viewport_px, panel_units,
  inset_units)` are how a panel is placed. Layout must not use `(viewport - panel_px) / 2.0`: at
  an odd viewport width that puts the panel, and every slot inside it, on a half pixel. The
  helpers snap to whole units instead, so the drawing and the hit-testing (`_slot_at_position`,
  `_craft_slot_at_position`) agree to the pixel.

| Surface | Where its units come from |
|---|---|
| Hotbar, inventory, crafting table | the slot art itself (`SLOT_PITCH`, `SLOT_FILL_SIZE`, `SLOT_SIZE_PX`) |
| Health bar | the heart art (`HEART_TEXELS`, `HEART_PITCH_TEXELS`) — a heart is its own 9 units, never a fraction of the bar |
| Crosshair, compass, FPS counter | `FONT_UNITS` / `OUTLINE_UNITS` / `TOP_UNITS` in each script |
| Settings menu, the maker pages, the death screen | the `UNIT_*` table (a 24-unit title and an 8-unit button on the death screen) |
| Chat | **opted out** — a log read while playing, deliberately 1:1 at every scale |

An item is 16 units across in every slot, whatever the slot art's inner box measures: 16 in the
hotbar's 16-unit fill, 16 centered in an 18-unit inventory slot. The icon renderer draws at
192 px so that 16, 32, 48 and 64 each divide it exactly, which is what stops an item icon from
being resampled by a different fraction at each scale.

## The shader-effect overlay

- `shader_overlay.gd` — The shader-effect stack, in the kinds `data/shaders.json` declares — and
  it declares both the kinds there are and what the menu calls each one, so a third kind is an
  entry in that list and a shader rather than a page of menu code. A **screen** entry gets a
  full-screen `ColorRect` and a `ShaderMaterial` of its own, in registry order, hidden while its
  effect is off. Each pass also carries a `BackBufferCopy` immediately before it, and that is
  what makes a stack of passes a stack rather than a race: Godot copies the screen once per
  frame, at the first node that reads it, so a second pass is handed the frame as it stood
  *before* the first pass drew and - writing its picture back over the screen - deletes that
  pass along with every HUD item drawn between the two. With a copy of its own each pass reads
  the frame as drawn up to where it sits (the world alone for a world pass, the world plus the
  passes and the HUD art under a screen pass), the copy is ordered with its pass by the same
  `z_index`, and it is hidden when the effect is off. A pass that declares the `previous_frame`
  uniform gets a pair of `SubViewport`s (`_build_history`, advanced by `_advance_histories`),
  each holding nothing but the main viewport's own texture, and they trade places every frame —
  one captures while the other, stopped, is handed over as the frame before this one. One is not
  enough, and a single capture cannot be it: a capture taken this frame holds this frame. The
  pair is the one thing no shader can read for itself, it follows the window's size with the
  frame, and turning the effect off stops it and hands the pass a blank. A pass is a full-rect
  child of this node, so the node's own rect is set from the viewport rather than left to its
  anchors: zero-sized passes draw nothing at all, silently, which is what cost this effect its
  first afternoon. A **world** entry is the same layer and the same shader drawn somewhere else:
  at a negative `z_index` (`WORLD_PASS_Z_INDEX`), which is over the 3D world and under every
  other CanvasItem in the HUD, so it reads the frame with no HUD in it and what it writes is put
  back under one — a difference no shader can express, because a shader is handed one frame and
  has no idea where in the stack it was drawn. A **vertex** entry gets no rect and no pass: it
  names the `materials` the world is drawn with (the registry's `materials`) and the
  `enable_key` uniform its switch is pushed to, and those materials are *loaded*, not
  duplicated, so a uniform set here is set on the very resources the renderer draws with. A
  **texture** entry is the same wiring around another subject: the same `materials`, the same
  `enable_key`, the same loaded resources, but what it changes is what those materials
  *sample* — so the block textures themselves are changed for every block at once, live, while
  the geometry stays exactly where the mesh put it. Moving no vertex, it has nothing to hand
  the culling code the vertex kind talks to, and that is the whole of the difference between
  the two kinds on this side. `shaders/block_noise.gdshaderinc` is the kind's first member,
  **Noisy Blocks**: the makers' own 0..100 grain, the same ceiling (`MAX_GRAIN`), added to the
  texel each fragment of both world materials reads, monochromatic and faded out where a texel
  shrinks below a screen pixel. `shaders/grain.gdshader` is **Film Grain**, a screen pass with
  no history and nothing measured: a hash of each `grain_size`-pixel cell of the frame, nailed
  to the screen by default and re-rolled 24 times a second with **Animated** on. Neither
  reaches the held item's mesh or the inventory's block icons, which read a block's texture
  directly rather than through the world's materials. A **camera** entry
  (`_build_camera_effect`, driven by `_process_camera_effect`) is the newest
  kind and gets none of those things: it moves the current `Camera3D` itself through
  `h_offset`/`v_offset` — a held, re-rolled micro-offset for the jitter and never a decaying
  impact shake — and takes only its own writes back off, so the player's own steering of the
  camera is never overwritten. Owns the live state (switch + one value per uniform) and pushes
  the `frame_size` the passes count their patterns in, re-pushed on `size_changed`;
  `settings_menu.gd` is the one that persists it. Sits in the HUD *below* `SettingsMenu` so the
  world and gameplay HUD wear the effect while the menus over them stay readable.
  `shaders/crt.gdshader` is the registry's fourth entry and one of its two *screen* passes: a
  corner-filleted tube bend with a feathered black case, and the picture built the way a tube
  builds one — the frame averaged down onto a coarse raster of `picture_scale`-screen-pixel dots
  (`picture_scale` is a magnification rather than a line count, so a dot is always a whole
  number of pixels whatever the window size, which is what keeps the raster from beating against
  the frame's own pixel grid), a re-normalised Gaussian beam spot gathered over each dot, dark
  scanline gaps paid back into the lines, convergence, an RGB grille one triad per picture cell,
  a halo gathered out of the picture's own cells, then vignette, tone and mains hum. Its
  defaults are chosen by looking, not by arithmetic, over a frozen game frame
  (`probes/probe_crt_look.gd`); `data/shaders.json` is the registry that names each effect,
  its `params` and the uniform each param drives. `shaders/phosphor.gdshader` is the registry's
  fifth entry and the other *screen* pass, and the first effect to need the frame before the one
  it draws into — which no shader can get for itself, since the engine's screen copy is of the
  frame *being drawn*. `shader_overlay.gd` keeps a viewport whose whole content is the main
  viewport's own texture, the screen one render ago, and sets it on any pass whose shader
  declares `previous_frame`: the name is the contract, one viewport per pass, stopped with its
  effect, and the frame it holds is the *screen* the last frame ended with rather than the world
  alone. The effect is then a single line — the frame is the brighter of what is on the screen
  and what the phosphor still holds — which is why it stacks safely: brightness is added and
  never accumulated, so nothing runs away and a still scene is unchanged. The held picture is
  aged before it is compared: decayed by `phosphor_retention`, gathered over a disc of its own
  pixels by `phosphor_spread` (so it diffuses a little further every frame and old ghosts are
  the soft ones), held longer where it was brighter by `phosphor_bright_hold` (the
  square-root-of-retention behaviour of a real tube), and gated by `phosphor_floor` so a pixel
  too dark to have lit the phosphor keeps nothing and a night does not smear into fog. A pass
  with a history is handed a blank for its first frame, so its trail starts from the frame it
  was switched on in. `shaders/invert.gdshader` and `shaders/sepia.gdshader` are the registry's
  sixth and seventh entries and the `filter` kind's first two members — a screen pass under
  another heading, and a pair deliberately as plain as a pass can be: one read of the frame, one
  write, no history, no offsets and nothing measured in pixels, so neither declares
  `frame_size`. Invert takes every channel to its opposite through a mix, which is what makes a
  partial invert a wash rather than a half-negative and usable as a grade; Sepia is the classic
  per-channel sepia transform (three dot products, written explicitly rather than as a mat3
  multiply) mixed by amount, then pushed off its own grey along its own hue (`TONE_CHROMA`) —
  the raw matrix's three rows sit close together and grade a bright frame to washed tan, and the
  chroma lift is what turns the ramp into the cream-to-coffee brown of a photograph while a
  small amount stays a warm grade under the other effects. Both are kinded only for the menu: to
  the overlay a filter *is* a screen effect, so they stack with the CRT and the phosphor exactly
  as any other screen pass does. The registry's eighth entry, **Camera Jitter**, is the first of
  the `camera` kind and the first effect that is not a picture at all — it moves the camera,
  which no shader can do, since a shader is handed a frame after the eye has seen it and moving
  the picture after the fact is not moving the eye. So `shader_overlay.gd` builds it with no
  rect, no shader and no materials (`_build_camera_effect`), and runs it in `_process`
  (`_process_camera_effect`, the motion read out of `_camera_jitter_motion`): a held pair of
  −1..1 rolls re-rolled at `camera_jitter_rate` hz and scaled by `camera_jitter_strength` metres
  — a rattle, eased in over a couple of frames rather than per-frame noise. It writes additively
  onto the current `Camera3D`'s `h_offset`/`v_offset`, which displace the eye without touching
  where it looks, and takes only its own share back off before the next write
  (`_camera_restore`), so mouse look, head-bob and the frustum are untouched and a switched-off
  — or zero-strength — effect leaves the camera exactly as it found it. The driver is built for
  more camera effects than the one it has: the next one is a motion function and a registry
  entry. `shaders/anime.gdshader` is that registry's one *world* effect so far — the Shaders
  page's third category — and it draws at the world's own depth: its colours are flattened by
  taking the average of the flattest of four quadrant windows around each pixel (Kuwahara, 1976,
  so texture inside a surface is averaged away while the edge *between* two surfaces comes out
  sharper), its shading is the frame's own light rounded to `anime_bands` flats and blended in
  by `anime_shading`, and its shapes are inked by a Sobel over *those* bands, because texture
  that stays inside a band draws no line. The rounding is bounded (`SHADE_GAIN`, a third either
  way) for the reason the pass shipped its first version wrong: an unbounded one divides by a
  flat that can be zero, which crushed every shadow in the frame to a blotch. The line is the
  picture's own edges rather than the world's geometry, since a canvas pass has no depth buffer
  to compare — Godot's canvas built-ins expose none. `shaders/world_bend.gdshaderinc` is the
  registry's second entry and `shaders/world_horizon.gdshaderinc` is its third, and both are
  *vertex* effects — the Shaders page's second category — the ground moving because the world's
  own *vertices* move rather than because anything was drawn over them. World Bend's ground
  curves up and away from the camera. `shaders/voxel_shader.gdshader` and
  `shaders/voxel_shader_water.gdshader` both `#include` it and call it from `vertex()` with
  their vertex's world position and the camera's, then put the result back into the mesh's own
  space for the engine to project — so the terrain and the liquids bend by one shared, pure
  function of world position, which is what keeps a shoreline welded. The horizontal distance
  from the eye is mixed with that distance wrapped onto a cylinder of `world_bend_radius`
  (`atan(distance/radius) * radius`), a convex mix so that `world_bend` scales the bend while
  the map stays monotone, and the lift
  `world_bend * world_bend_rise * radius * (1 - 1/sqrt(1 + ratio²))` levels off at
  `rise * radius` instead of running away to infinity. Both horizontal axes are scaled by the
  same factor, so the bend is around the camera's vertical axis; the water alone declares
  `world_bend_sway`, a ripple off the include's own clock that the ground does not carry. It is
  deliberately neither a projection (the geometry moves before the camera's matrices, so walking
  and looking keep working on a world whose vertices have moved) nor collision (reach and the
  block you are mining do not move; past a few dozen blocks the ground is drawn somewhere other
  than where it is, which is under a block at the distances a player builds at).
  `shaders/world_horizon.gdshaderinc` is the other and the opposite: a sphere of
  `world_horizon_radius` touches the ground at the player's feet, the flat world is its tangent
  plane, and a point a horizontal distance d out is drawn
  `radius * (1 - 1/sqrt(1 + (d/radius)²))` below it — the same expression as the bend's lift
  with the opposite sign, one knob (the planet's radius, default 1,500 blocks, because on a
  sphere a separate "amount" would be a second slider doing what the first one does) and, unlike
  the bend, nothing but y: it leaves every distance measured on the ground, every vertical edge
  and the whole plan of the world exactly where they were, which is also why wrapping the world
  onto the sphere for real is three orders of magnitude of pull it does not spend. The two are
  independent — each is measured from where a vertex really is — so either can be on alone and
  both together compose in the world materials' order. `src/core/world_cull.hpp` +
  `src/mesh/mesh_manager.cpp` are the half that cannot be a shader: a vertex shader runs
  after the engine has decided what to draw, so every chunk is culled against a box describing
  where it *was* and the bend pulls ground into view that the frustum has already thrown away.
  The header is the include's arithmetic again on the CPU — the pull and the lift at the box's
  far corner, where a point's distance from the camera's vertical axis is largest and both of
  the bend's terms are largest, inward in x and z because the mix can only shrink a distance and
  upward only for the lift because the lift is never negative — and the .cpp grows every
  resident chunk's cull box and every far region's by it, per whole block of camera movement
  with two blocks of slack already in the box, writing an instance's box only when its margin
  has really moved and handing the mesh's own boxes back when the effects go off, so a player
  who never turns either of them on pays nothing for them. The Horizon Curve's bound is the same
  idea with one term — the drop at the box's far corner, applied to the bottom of the box and
  nothing else — and the two effects' boxes are applied one after the other rather than merged,
  which is exact because each moves a point by an amount that is a function of where the point
  really is. `shader_overlay.gd` is what tells the engine as well as the material — one method
  per effect, named after the effect's id, with the knobs each one needs listed in the overlay
  rather than in the registry — and it finds the engine by walking outwards for the first node
  that can take *every* vertex effect the registry declares rather than by a path, because the
  path it used was the HUD's own child and the only symptom was that the culling silently never
  got better. Pinned by `tests/test_world_bend.cpp` and `tests/test_world_horizon.cpp` (the
  grown box contains the bent, and the sunk, image of every corner of every box, over a spread
  of cameras, radii, amounts and placements, and one box holds both effects at once — see
  `tests/world_effect_warp.hpp` for the single transcription of both includes that neither test
  can drift from) and by `probes/probe_bend_cull.gd`, which measures the renderer rather than
  the geometry: 1,452,116 primitives in 87 draw calls with the engine not told, the same camera
  drawing 3,263,096 in 320 once it is, and exactly what it started with when the bend is
  switched off. Pinned by `probes/probe_bend_geo.gd`, which stands one marker at a time on
  the ground at 40/80/140/220 blocks and compares its measured pixel centroid against the
  include's arithmetic re-run in GDScript: within 0.54 px of the camera's own unprojection with
  the bend off, within 0.90 px of the prediction with it on, and both real materials landing on
  the marker to 0.00 px

## Liquid texture tools

- `liquid_texture_lab.gd` — Liquid Texture Lab (O key, autoload): procedural animated-texture
  authoring for water/lava/acid — style presets, every automaton and ramp knob as a slider, live
  animated preview plus a 3×3 tiled seam check and frame thumbnails, save/load as a vertical
  strip PNG + settings JSON in `user://liquids/`, a **Live** mode that pushes frames into the
  running world's texture-array layer for the chosen liquid (turning texture compression off for
  the duration, since a compressed layer cannot take an RGBA frame), and **Bind to world**,
  which saves the current strip as `<liquid>.json`/`.png` — the file the animator below loads
  for that liquid — and a **World anim** switch onto that animator
- `liquid_animator.gd` — LiquidAnimator (autoload, always on): animates water, lava and acid in
  the running world with no panel open. Per liquid it takes `user://liquids/<liquid>.json` if
  the Lab bound one and the built-in preset otherwise, regenerates the strip in C++
  (deterministic, so the saved settings are the whole source), slices it, and pushes a frame
  into that liquid's array layer every `frame_time` ticks, wrapping a looping strip to index 1.
  It turns texture compression off while it animates and restores the setting when disabled, and
  it is paused for whichever liquid the Lab's Live currently owns (`pause`/`resume`) — one
  writer per layer. `get_status()` reports source, frames, current frame, the frame last pushed,
  and why a liquid is idle; `probes/probe_lava_acid.gd` drives all of it against the real
  world

## Previews, models and the tools around them

- `skin_preview.gd` — Transparent-background sub-viewport that orbits `player.glb` behind the
  skin maker; the camera orbits the model's AABB center rather than being a child of the
  rotating node
- `block_manager.gd` — Autoload holding the single persistent 16×16 block texture (one
  `ImageTexture` shared by every cube face), with debounced saves to `user://current_block.png`,
  a restart-recovery noise base (`user://block_noise_base.png`), and a reversible
  grayscale-noise slider living on the autoload so it survives page rebuilds
- `block_preview.gd` — Transparent-background sub-viewport behind the block maker that
  drag-orbits a cube; DRAW/FILL/BOX painting over primitive triangle raycasts, undo (Ctrl+Z),
  noise slider integration, and clamped zoom
- `player_model.gd` — Applies the skin texture to `player.glb`'s `StandardMaterial3D` surfaces
  with nearest filtering (no mipmaps, avoiding smeared UV islands), and drives the
  Minecraft-style head look (`_track_head_look`): it rebuilds the aim basis from the
  controller's world yaw + pitch (`get_aim_direction()`) so the `head` mesh follows the player's
  look in every view, with a camera-follow fallback for scenes without a `PlayerController`
  (skin preview)
- `player.glb` — Voxel-style player model with a tightly-packed 64×64 skin-texture atlas; node
  pivots re-baked onto the true Blockbench joints (limb tops, head neck) by
  `tools/rebake_player_pivots.py` because Blockbench's glTF export flattens cube-pivot metadata
- `pose_clone_debug.gd` — Debug tool (K key): spawns a rigid, punchable physics dummy — a frozen
  copy of the player model (no Idle animation, no head tracking) running `dummy.gd`'s vanilla
  1.8.8 gravity/drag/knockback at 20 tps — standing on the aimed block, with a bright
  depth-test-off cube at every mesh's origin (see `shaders/pose_pivot_marker.gdshader`); prints
  each part's pivot for marker-vs-transform verification
- `dummy.gd` — Combat physics for the K-key pose clone: 20 tps vanilla 1.8.8 gravity/drag,
  knockback (`apply_knockback`: base 0.4 + sprint bonus, 10-tick hurt-resistance gate),
  interpolated rendering between ticks
- `tools/bake_liquid_textures.gd` — Headless baker for a liquid's still block texture: runs the
  generator's own preset for `lava`/`acid` (or any style named on the command line) and writes
  frame 1/4 of the strip to `textures/blocks/<style>.png`, the PNG the texture array builds its
  layer from. `godot --headless --path . --script res://tools/bake_liquid_textures.gd`, then
  `--import`
- `tools/rebake_player_pivots.py` — Idempotent re-baker for `player.glb`: moves each node origin
  onto its pivot while shifting that mesh's vertices by the delta so world placement is
  byte-identical (glTF has no pivot field — a node's origin is its rotation anchor)
- `viewmodel.gd` — Thin first-person hand + held item/block glue (child of `Camera3D`): node
  tree, `_input`, F12 HUD, and peak-pose constants. Per-frame animation math lives in
  `ViewmodelPose` and held-mesh geometry in `ViewmodelMeshes` (both native C++)
- `block_break_overlay.gd` — Draws the 10-stage crack overlay
  (`textures/animated/l0_sprite_01..10.png`) on the mined block, driven by `get_break_state()`
- `block_textures.gd` — ~~Block texture atlas generation from `textures/blocks/`~~ ported to the
  native `BlockTextures` GDExtension binding (registered in
  `src/godot_bindings/register_types.cpp`); the GDScript file has been deleted
