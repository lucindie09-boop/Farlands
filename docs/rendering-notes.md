# Rendering notes

The renderer's decisions: the LOD and mesh pipeline, the sky and fog passes, the
procedurally generated liquid textures, and the shader-effect stack end to end.

## LOD, the mesh pipeline and the two water emitters

- **Three-tier LOD with region merging**: Per-chunk distance-based reduction (not chunk merging)
  — full detail, mid stride/detail reduction, and a far tier with its own detail level + render
  start (`far_lod_distance`/`far_lod_detail_level`); LOD-reduced chunks are cached and merged
  into 8×8-chunk region instances so the coarse rings cost only a handful of draw calls
- **Dynamic water shader**: Translucent water with edge fade, bounce light, sun glint, flowing
  texture animation, and separate blend-mix surface. The geometry under it is the per-corner
  liquid surface (see the fluid section below)
- **Water is drawn by two different emitters, and both have to agree about it.** At full detail
  the liquid surface pass draws every liquid cell (per-corner heights, see the fluid bullets);
  at LOD stride > 1 it does not run at all and the *generic* emitters draw each liquid cell as a
  plain box — into the same water buffer. So their liquid skip is tied to the stride
  (`stride_xz_ <= 1 && is_fluid_drawn(...)`) and their `is_water` test is the fluid-FAMILY
  predicate, not the two natural-water ids: untied and id-based, the pair left LOD chunks with
  no water geometry, and put every flowing state (`water_runoff_*`, `water_fallen`) in the
  opaque buffer where the terrain material drew it as grey blocks.
  `tests/test_liquid_mesh_geometry.cpp` pins all four combinations (greedy / per-AABB × stride 1
  / 2) with a runoff cell — deliberately not `water` or `surface_water`, because those two ids
  are what the old test would have passed
- **The mesh upload hash covers BOTH surfaces, and the renderer checks the result.** Opaque
  terrain and the liquid surface go to the GPU in one `mesh_add_surface_from_arrays` pair under
  a single "did the content change?" test, so a content hash over the opaque mesh alone skipped
  the whole upload whenever only the water changed — which is the common case, a liquid being
  transparent (same blocks, same light, different water). The symptom is water you can swim in,
  collide with and outline but cannot see, until some unrelated edit moves an opaque vertex.
  `src/mesh/mesh_content_hash.hpp` is the hash (both surfaces + a separator byte so the two
  cannot be swapped) and its test asserts a change confined to the water changes the result. On
  top of that the renderer asserts the invariant where it can see both sides — a chunk with
  liquid in its data (`ChunkData::liquid_count`, maintained next to `block_count` on every write
  path) and opaque geometry on the GPU must also have liquid geometry on the GPU — remeshes the
  chunk once per `mesh_version` if not, and reports `Liquid missing` / `Liquid repairs` /
  `Mesh uploads` / `Dedup skips` / `Water-only skips` / `Unrendered` in the performance report
  (printed every 2 s with the rest of it). Those counters exist because a water drop is
  otherwise invisible from inside the game, and the two that must stay 0 are `Water-only skips`
  and `Unrendered`
- **A full cube hides a face only if it is also OPAQUE.** "Fills the cell" and "covers what is
  behind it" are two different questions, and two culling paths answered it with the first
  alone: the per-AABB test (`should_cull_aabb_face`, used directly by both partial-block
  emitters) and the vertical greedy "surrounded on all four sides, skip every face" shortcut.
  The blocks that expose the difference are the full cubes drawn with transparency — a column of
  falling water (`water_fallen`, the only liquid at full height; the runoff states are lowered
  so they never reach either path) and leaves: a shaft full of falling water lost the walls'
  faces toward it, and a slab lost its side against a waterfall. Both now ask for opaque as
  well, and `tests/test_mesh_culling.cpp` pins each (the shaft case measures face COVERAGE,
  because side faces merge along Y into one quad and a raw quad count would be testing the merge
  instead)

## Sky, fog and light

- **Vegetation generation**: Sparse oak trees on hills — a fraction of qualifying chunks get a
  single isolated tree (chance/spacing from `data/vegetation.json`), deferred cross-chunk writes
- **Night sky & starfield**: Dynamic procedural twinkling starfield during night sun elevations
- **Emissive texture support**: Second `Texture2DArray` for per-face glow maps
- **Soft curved AO**: Non-linear power-curve smoothing to eliminate diagonal triangulation seams
- **Fog system**: 4 fog modes (Disabled, Edge, Linear, Exponential) with fog color matching sky
  color throughout day/night cycle
- **God rays**: Toggleable atmospheric lighting effects with dynamic sample count and twilight
  optimization
- **Sky turbidity**: Rayleigh/Mie haze effects gated by sky light for proper night darkness

## Procedurally animated liquid textures

- **Procedural animated liquid textures + the Liquid Texture Lab (O key)**: animated
  water/lava/acid sprites are *generated*, not drawn — a three-field cellular automaton over an
  N×N grid of floats (`surface` = the visible height that becomes the pixel colour, `flow` =
  momentum it pushes into, `surge` = a decaying energy source re-ignited by a per-cell dice
  roll), stepped per frame and mapped through a colour ramp. That is how the classic block game
  produced its still-water and still-lava sprites before textures were image files; the field
  names, kernels, presets and colours here are ours. `src/render/liquid_texture.hpp` is pure (no
  Godot types) so the lab, the animator and the tests run identical code; `LiquidTextureGen` is
  the GDScript face and `liquid_texture_lab.gd` (autoload, O) is the tool: style presets, every
  automaton/ramp knob as a slider, live animated preview + a 3×3 tiled seam check + frame
  thumbnails, save/load as a vertical strip PNG plus a settings JSON in `user://liquids/`, a
  **Live** mode that pushes frames straight into the running world's texture-array layer
  (`ChunkManager::push_texture_frame` → `Texture2DArray::update_layer`), and a **Bind to world**
  button that saves the strip as `<liquid>.json` + `.png` — the file the runtime animator loads
  for that liquid
- **The world animates its own liquids: `liquid_animator.gd` (autoload, `LiquidAnimator`), no
  panel involved.** Live only exists while the Lab is open, so this node is the other half: for
  each of water, lava and acid it resolves a source in order — `user://liquids/<liquid>.json`
  (what Bind writes, i.e. a strip you tuned) and then the built-in preset — regenerates the
  strip in C++ (the generator is deterministic from its settings, which is why nothing here
  reads the saved PNG), slices it, and pushes frame N into that liquid's array layer on
  `frame_time` ticks with the panel's own playback rule (a looping strip wraps to index 1, not
  0). Three things follow from that. It turns texture compression **off** for the session while
  it animates and puts the setting back when it is disabled, because a compressed layer cannot
  take a raw RGBA frame and nothing on the CPU can build a matching chain. While the Lab's Live
  owns a liquid's layer the animator is paused for that liquid (`pause`/`resume`) — one layer,
  one writer, or the two blink against each other. And a missing layer means no animation plus a
  note saying so, which is exactly what lava and acid used to report: the array is built from
  `res://textures/blocks/<name>.png`, so a liquid with no PNG has no layer to write even though
  its blocks exist
- **A liquid's still texture is baked from its own preset** (`tools/bake_liquid_textures.gd`:
  `godot --headless --path . --script res://tools/bake_liquid_textures.gd`, then `--import` to
  re-import the PNGs it writes). That PNG is what the array builds from, what shows before the
  first pushed frame, and what `restore_texture_layer` puts back when animation stops, so it has
  to look like the liquid it belongs to: the tool takes frame 1/4 of the preset's own strip —
  past the warm-up, before the loop's arriving tail — and writes it to
  `textures/blocks/<style>.png`. Default targets are lava and acid; the hand-authored
  `water.png` is only touched if it is named on the command line
- **Two traps this tool sits on.** (1) `TextureArrayGenerator::find_texture_layer()` is the
  *writer's* lookup and answers -1 for "no such layer"; `get_texture_index()` answers 0, which
  is indistinguishable from the fallback layer — using it to write would repaint stone. (2) A
  GPU-compressed array (the settings menu's compression toggle) cannot take a raw RGBA frame, so
  Live rebuilds uncompressed and restores the user's setting when it stops. Related: Godot 4.7's
  `TextureLayered::get_layer_data()` returns null even for an array built in the same process,
  so `ChunkManager::get_texture_layer_image()` is best effort and `fit_texture_frame()` (which
  *is* verifiable, and tested) is the half of the path that can be held to account
- **Looping animated textures: the last frame is the first frame's image, and the seam has to be
  *spread*, not moved.** The automaton is not periodic, so a strip's end never sits next to its
  start — playing 0..N-1 then 0 pops by a whole decorrelation (measured: a 36/255 step in the
  water preset, against 5-6 for a normal frame). With `loop` on (default) the generator morphs
  the last `loop_window` frames (default 8, about the kernels' decorrelation time) into the run
  that led into frame 0, so the final frame lands on frame 0 exactly and the step into it is a
  normal frame change. A window of 1 is the bare duplicate: it satisfies "last frame == first
  frame" while simply moving the pop one frame earlier — the suite asserts the morphed seam is
  *smaller than every step inside the strip*, which a bare duplicate fails. Playback rule that
  goes with it: a looping strip advances from the last frame to **index 1**, because index 0
  already played as the last frame (`Strip::looped` says so, `liquid_texture_lab.gd::_advance()`
  does it)
- **The automaton's divisor is a stability knob, not a taste knob.** The neighbourhood sum must
  be divided by more than the number of cells the kernel sums (row → 3.3, box → 9.9) or the
  surface amplifies itself every step until a frame is a single flat ramp stop and the animation
  stops animating; the same applies to `field_scale` against the ramp.
  `tests/test_liquid_texture.cpp` pins both (every preset must flatten when its excite source is
  off, and every preset's field must land inside its ramp with contrast), and both caught real
  bugs while this was written — the acid preset shipped divergent, and then over-bright

## Texture and vertex compression

- **GPU texture compression**: Optional S3TC/BC1-BC3 compression for texture arrays to reduce
  VRAM usage
- **Vertex compression**: 24 bytes per vertex (-40% VRAM) with fixed-point positions
- **Lighting customization**: Adjustable AO color/strength, darkness color, contrast, and
  saturation

## Shader effects and the pass stack

- **Shader effects (pause menu → Shaders)**: an effect is an entry in `data/shaders.json`, not
  code, and both the kinds of effect that exist and what the menu calls them come from the
  registry's own `kinds` list. A **screen** effect is the default: it names a `shader` and is
  drawn as a full-screen picture over the frame. A **vertex** effect is the world's own geometry
  moved by its vertex shader — it names the `materials` the world is drawn with and the
  `enable_key` uniform its switch is pushed to, because a material has no visibility to hide —
  and it gets no pass and no rect of its own. A **texture** effect is that same wiring around a
  different subject: the same `materials`, the same `enable_key`, no pass and no rect either,
  but what it changes is what those materials *sample* — the block textures themselves — rather
  than where their vertices are, so unlike the vertex kind it moves nothing out of the boxes
  the chunks were culled against and has nothing to hand the culling code. A `world` effect is a
  pass over the world's own picture: it names a `shader` and gets a layer of its own exactly like
  a screen effect's,
  but that layer is drawn at the *world's* depth in the stack (a negative `z_index`), which puts
  it over the 3D world and under every other CanvasItem in the HUD. So it reads the frame with
  no HUD in it and what it writes is put back under the HUD, where a screen effect grades the
  finished screenful - and that is the whole of the difference between the two kinds on the
  overlay's side, because it is not a difference a shader can express: a shader is handed one
  frame and cannot know where in the stack it was drawn. Declaring the kind was what made Hand
  Drawn an entry rather than a page of menu code, and the menu needed no change that day. The
  registry names each one (`id`, `name`, `enabled`) and its `params` — `key` (the uniform it
  drives, so the shader's uniforms, the JSON and the saved config keys all carry the same name),
  `label`, `type` (`float` or `bool`), `default` and, for a float, `min`/`max`/`step` plus an
  optional `suffix`. `shader_overlay.gd` (`HUD/ShaderOverlay` in Main.tscn) brings each entry up
  according to its kind. A screen entry gets a full-screen `ColorRect` and a `ShaderMaterial` of
  its own, in registry order, so a later entry samples a frame that already holds the earlier
  ones. A world entry gets the registry's own material *resources*, loaded rather than
  duplicated, because `load()` goes through the resource cache and hands back the very instances
  the renderer is drawing with — a copy would take the same uniform and draw exactly as before,
  which is a switch that appears to do nothing. Each param's default is then pushed to whichever
  of its target's materials declares it (the water's ripple is declared by the water material
  alone, which is why the overlay warns when a param is on *none* of them rather than on any),
  every material that declares `frame_size` is handed the frame its patterns are counted in
  (re-pushed on `size_changed`), and its switch is pushed as `enable_key` = 1 or 0 rather than
  hiding anything. The node owns the live state; `settings_menu.gd` owns the file, saving
  `<id>_enabled` and `<id>_<key>` under a `[shaders]` section of `user://settings.cfg` and
  giving the page's category a `FS-` share code holding the whole stack. The Shaders page is
  built from the registry and never hard-codes a knob, and it is a list of effects *grouped by
  kind*: one category per kind, in the order the registry's `kinds` list gives, under the name
  that list gives it (`Screen Shaders`, then `Vertex Shaders`, then `Texture Shaders`, then
  `World Shaders`, then `Colour Filters`, then `Camera Effects`), and a kind
  with nothing in it is not shown at all — so the next kind is a shader and a registry entry,
  and adding one changes no menu code. Effects that are different kinds of thing should not read
  as one list: one of them is a picture laid over the frame, the next is the world's own
  geometry moved, and the third is a picture of the world drawn under the HUD, and a player
  looking for the switch that curves the ground should not have to find it under a CRT's
  heading. Inside a category it is one row per effect, that row carrying the switch plus the
  square `settings_button.png` icon that opens the effect's own page, whose rows are that
  effect's uniforms and nothing else. The uniforms are not on the list page because one effect
  with a dozen knobs would bury the next effect below the fold, and the effect's page carries no
  switch because that is the same switch one page apart: turning an effect on is the list's job,
  where the row itself says whether it is on, and the page is the fine adjustment under it. A
  screen or world effect's rows are its own shader's uniforms and a vertex effect's are its
  materials', and the page does not know the difference. A page whose effect is off breathes its
  *title* instead — darker and back, slowly, at `TITLE_PULSE_HZ` — so a page of sliders that is
  changing nothing on screen says so without becoming a second switch; the menu's own `_process`
  does it (started and stopped with the menu), and the shot probe samples the title through a
  whole beat to pin both halves of it, because a still frame cannot show a pulse. Each row keeps
  the usual per-row undo reset, and one `syncs` list is shared by the list page and every
  per-shader page — which is exactly why it is a list — so the section's `syncs` callables pull
  every row on every one of them back in step after an imported code sets the state out of band.
  The export/import icons and their status line belong to the whole *stack* rather than to one
  category of it, because the code carries every shader: the icons ride on the first category
  that has any rows (and each heading's own reset-all still acts on its own rows), and the
  status line stays at the bottom of the page below all of them. `ShaderOverlay` sits in the HUD
  *below* `SettingsMenu` on purpose: the world and the gameplay HUD under it wear a screen
  effect, while every menu that opens over them stays readable (the crosshair and compass above
  it stay crisp — the reticle is not part of the picture). A world effect is drawn below *all*
  of it instead, so it wears the world and nothing else: the hotbar and the health bar are as
  crisp as the menu is, which is the difference between grading the ground and grading the
  screenful. A pass reads that frame through a copy of it, and Godot makes **one** copy per
  frame - at the first node that reads the screen, wherever that node is - which makes a stack
  of these passes exactly the thing the engine does not hand you: the second pass is given the
  frame as it stood *before* the first one drew, so it grades an older picture and writes it
  back over the screen, deleting the pass before it and every HUD item drawn in between. That is
  not a subtle failure; it is a missing hotbar. Hand Drawn's own copy made it visible the day a
  second pass existed to be deleted, because a world pass is the first node in the layer that
  reads the screen and therefore the pass the engine takes its one copy at. Each pass - screen
  or world - therefore carries its own `BackBufferCopy` immediately before it, at its own depth,
  so every pass reads the frame as drawn up to where it sits, any number of them stack, and the
  copy is hidden with the pass when the effect is off, which keeps an off effect free and keeps
  it from standing between the passes that are on.

### Hand Drawn

- **Hand Drawn** (`shaders/anime.gdshader`) is
  the registry's first entry and the `world` kind's first member, and it stylises the world's
  own picture in three steps. The colours are *flattened*, not blurred: every pixel takes the
  average colour of whichever of four quadrant windows around it is flattest (Kuwahara, 1976),
  so a texture inside a surface is averaged away while the edge between two surfaces comes out
  *sharper* than it went in, because the quadrant that straddles an edge is the one the test
  throws away. The shading is *banded* into a few flats - the frame's own light rounded to
  `anime_bands` steps, softened over a third of a band so a step reads as light rather than as a
  contour, and blended in by `anime_shading` so how hard it lands is taste rather than a
  constant. That rounding is a division, and the first version of this pass divided by it: a
  pixel whose flat rounds to zero goes black and its neighbour a hair brighter jumps by two
  hundred percent, which is not banding but a contrast control wound up, and it turned every
  shadow in the frame into a blotch. The gain is therefore bounded to a third either way
  (`SHADE_GAIN`), which is what keeps a band a band. The shapes are *inked in* by a Sobel over
  the banded brightness rather than the raw one - the step map is the second reason the shading
  is stepped, since texture that stays inside a band draws no line, so the line lands where the
  shading had to jump and the flattening is what makes that the shape of a thing rather than the
  grain of it - with the ink multiplied in rather than painted over, so it darkens what is under
  it and keeps the colour of the shadow it is drawn along. The outlines are the *picture's*
  edges and not the world's geometry, which is the honest limit of a canvas pass: Godot's canvas
  built-ins expose no depth buffer, so a wall and a sky of the same brightness are one flat
  region here and two faces of one block meeting at an angle are one unless the lighting
  separates them. `frame_size` is pushed into this pass because `SCREEN_PIXEL_SIZE` is visible
  in `fragment()` and nowhere else, so a line one pixel wide has to be told what a pixel is.

### CRT Screen

- **CRT Screen** is the registry's fourth entry (`shaders/crt.gdshader`), and one of the
  registry's two *screen* effects — the three before it are Hand Drawn, World Bend and the
  Horizon Curve, below, of which only the first draws anything at all, and it draws under the
  HUD rather than over the screenful — and it draws the picture on a tube rather than laying a
  tint over a sharp frame. The frame is not the picture: the set has its own coarse raster,
  square dots `picture_scale` screen pixels on a side (1 is the frame itself, 2 is 480 lines
  down a 960-pixel frame, 4 is 240, 8 is 120 — the count across follows the frame's shape, and
  the whole grid is derived from the *warped* position, so the raster bends with the glass
  instead of sliding under it). The raster is a *magnification* and deliberately not a count of
  lines: a count of lines re-derives a dot size that is almost never a whole number of screen
  pixels, and the averaging taps then land at a different place inside every dot, so what you
  see is the pattern of that difference — a blotchy lattice whose period is the dot size, which
  reads as a screen overlay rather than a raster. It was that, not taste, that made 180 lines
  look right at 720 and wrong at 960. Whole-pixel dots are clean at every value and every window
  size. Building the picture is then an *average*, not a sample: each dot is the mean of the
  frame under it — `picture_scale`² taps one screen pixel apart, which is an exact box (bilinear
  taps a pixel apart tile flat) and which at a three-pixel dot is exactly the nine taps a third
  of a dot apart this pass started with — plus four cheaper taps on each of the eight dots
  around it — because detail the picture cannot carry, sampled anyway, comes out as a stipple of
  what the frame happened to be doing at the tap positions rather than as a coarse picture. Each
  cell is then drawn by the beam as a soft Gaussian spot gathered over its 3×3 neighbourhood and
  **re-normalised**, so spreading the picture over its neighbours never dims it — which is why a
  one-frame-pixel checkerboard (the sharpest thing a frame can hold, and something no set can
  draw) comes back as the grey it was made of, while 4-frame-px blocks keep their plateaus and
  only their boundaries soften. The spot has a *floor* (`beam_softness` ≥ 0.15, clamped in the
  shader): a Gaussian narrow enough that every weight underflows still sums to zero, and a
  gather divided by zero is a black screen. The beam is off between the picture's lines, so the
  dark bands *are* the lines: a bar of light across the middle of each line with a soft edge,
  and the light the gaps take is paid back into the lines, so `scanline_depth` changes the shape
  of the picture instead of only dimming it. The guns' convergence is free — shifting the spot's
  weights sideways is what moving the picture under the beam does, so red and blue are the same
  gather with their weights shifted rather than a second set of samples. The phosphor grille is
  one triad per picture *cell*, `mask_pitch` picture pixels wide, with the three guns inside it:
  a triad a cell wide puts a pixel's whole colour inside that pixel, where a triad three cells
  wide puts one gun in each of three pixels, which is what turns a picture into confetti. Its
  light comes back through a gain, so switching the mask on colours the picture without dimming
  it. The halo is gathered out of those same nine cells rather than out of frame pixels — a
  handful of taps on a circle draws the circle into the picture wherever it crosses a gradient —
  and only what is over the phosphor threshold spills, so dark rock does not smear. A
  corner-weighted bend files the four corners round while all four edges still reach the bezel:
  how far a sample fell outside the frame is what draws the black case, feathered, so the fillet
  is the same shape as the warp that made it. Vignette + brightness/contrast/saturation + a
  rolling mains hum finish the glass and the picture. The defaults sit where real frames look
  right rather than where the arithmetic is tidiest: the pass is looked at over a frozen game
  frame by `probes/probe_crt_look.gd`, which renders the world once, freezes it, and draws
  every candidate pass over that same frame so a change can only be the shader. Pinned by
  `probes/probe_crt_unit.gd`, which measures the fillet, the band count/period/depth, the
  guns' pitch-apart stagger, the vignette falloff and the normalised gather on a flat field,
  then measures the *stretch* on a one-pixel checkerboard (detail collapses, average preserved)
  and on 4-frame-px blocks (plateaus kept), and prints a zoom of the frame at one character per
  pixel so the spots and the dark rows between them can be looked at rather than only counted —
  plus `probes/probe_shaders_shot.gd`, which opens the page in the real game, checks the
  registry against the layers and against the vertex effects' materials, the layering against
  the menu, that dragging a row reaches the uniform, that the frame really changes, and that the
  settings file and the `FS-` code round trip.

### Phosphor Trail

- **Phosphor Trail** (`shaders/phosphor.gdshader`)
  is the registry's fifth entry and, with the two colour filters below, one of the *screen-ish*
  passes drawn over the screenful and it is the first effect to need something the engine does
  not hand a shader at all: the frame before the one it draws into. A reading pass is given a
  copy of the screen taken where it sits, so the only frame it can look at is its own, and
  `shader_overlay.gd` makes up the difference by keeping a viewport whose whole content is the
  main viewport's own texture — the screen as it stood one render ago — and handing it to any
  pass whose shader declares `previous_frame`; that name is the whole contract, and a pass
  declaring it is a function of two pictures instead of one. What the second picture holds is
  the *screen* the last frame ended with, this node's own passes and the HUD over them included,
  which is why a phosphor trail trails what was visible rather than what the world was doing.
  The effect is one line — the frame is the *brighter* of what is on the screen and what the
  phosphor still holds — and that line is the whole of why it stacks: brightness can only be
  added and never accumulated, so nothing runs away, and a scene standing still comes out
  bit-for-bit itself. The held picture is aged before it is compared. It is decayed by
  `phosphor_retention`, so a ghost one frame old is a little dimmer, two frames old dimmer
  again, and a fast turn reads as a length rather than as a double exposure. It is gathered over
  a disc of its own pixels (`phosphor_spread`), so the ghost of a moving object is light rather
  than a copy of it — and because it is gathered again every frame, the old ghosts are the soft
  ones, which is the trail diffusing with age the way light in glass does. It is held longer
  where it was brighter (`phosphor_bright_hold`): a real tube keeps a bright spot for longer
  than a dim one, which is the retention taken to a power that falls with the pixel's own
  brightness rather than a flat per-frame multiply. And it is gated by `phosphor_floor`, so a
  pixel that was too dark to light the phosphor keeps nothing of itself and a cave or a night
  does not smear into a fog of its own noise. The knobs are named for what they do: Retention is
  how long the after-image lives, Burn Threshold what a pixel had to be to leave one, Glow
  Spread how far the ghost diffuses in one frame, and Bright Holds Longer whether the aging has
  that preference at all. The history is a *pair* of viewports that trade places every frame —
  one captures the frame being drawn while the other stands still holding the frame before —
  because a capture taken this frame holds this frame, and what the pass needs is the one before
  it; the capture is taken after the pass rather than before it for the same reason a trail is a
  trail, since what has to be aged is the after-image the pass itself drew a moment ago, and a
  history without the pass's own picture in it decays to nothing in two frames instead of
  building up. Switching it on starts the trail from the frame it was switched on in rather than
  from whatever was on screen the last time it was on, because a pass with a history is handed a
  blank for its first frame and nothing else; switching it off stops and blanks that history, so
  an after-image that is off is a hidden rect and nothing else. A pass is a full-rect child of
  this node, so the node's own rect is set from the viewport rather than left to its anchors — a
  zero-sized overlay is a whole stack of zero-sized passes, drawing nothing, with no error to
  say so. **Invert Colours** (`shaders/invert.gdshader`) and **Sepia**
  (`shaders/sepia.gdshader`) are the registry's sixth and seventh entries and the first two of
  the `filter` kind, a heading of their own. A filter is a screen pass under
  another name — the overlay builds it exactly as it builds a screen effect, so it stacks like
  any other pass — and the kind exists only because of where a player goes looking: a pass whose
  whole job is grading the picture's colours is not the kind of thing to hunt for under a CRT's
  heading. The two are written as plainly as a pass can be, one read of the frame and one write
  with no history and nothing measured in pixels, which is the other half of why they need no
  code: there is no `frame_size` to push and no `previous_frame` to build. The invert mixes the
  frame with its own negative by `invert_amount`, so a partial invert is a wash rather than a
  half-negative, and the sepia mixes the frame with the classic per-channel sepia transform by
  `sepia_amount` — three dot products written out rather than a mat3, then lifted off its own
  grey (`TONE_CHROMA`) because the raw matrix's rows sit close together and grade a bright frame
  to washed tan rather than brown. **Camera Jitter** (`camera` kind, no shader) is the
  registry's eighth entry and the first effect that is not a picture at all: it moves the
  *camera*, which no shader can do — a shader is handed a frame after the eye has seen it, and
  moving the picture after the fact is not moving the eye, which would not move what the frustum
  chose but only smear it. So there is no shader, no rect and no materials: the overlay runs it
  itself in `_process` (`_process_camera_effect`, the motion read out of its own function),
  writing the movement where the engine reads it — the current `Camera3D`'s own
  `h_offset`/`v_offset`, which displace the eye in view space without touching where it looks,
  so nothing that steers the camera (mouse look, head-bob, the frustum) needs to know this is
  here. The movement is a *rattle* and deliberately not a shake: a fresh pair of −1..1 offsets
  rolled every `camera_jitter_rate` of a second and *held* until the next roll (a re-roll every
  frame is noise, not a jitter), scaled by `camera_jitter_strength` metres and eased in over two
  or three frames so it does not tick like a metronome. The write is additive and only what the
  effect itself wrote is taken back off before the next write (`_camera_restore`), so a value
  the player's own controls wrote between frames survives, and an effect switched off — or
  enabled with its strength at zero — leaves the camera exactly as it found it; the driver is
  built for more camera effects than the one it has, so the next one is a motion function and a
  registry entry.

### World Bend

- **World Bend** (`shaders/world_bend.gdshaderinc`) is the registry's second
  entry and one of its two vertex effects, and it does not draw anything: it moves the vertices
  the world is made of, so the ground curves up and away from the player and the whole far field
  closes into a disc around them. The include is shared by the two materials the world is made
  of — `shaders/voxel_shader.gdshader` for all chunk terrain and
  `shaders/voxel_shader_water.gdshader` for water, lava and acid — and it is a *pure function of
  a vertex's world position*, which is the whole reason the two can be bent at all: the vertices
  a mesh edge shares with its neighbour's edge are moved by the same function to the same place,
  so bent ground and bent water still meet and a shoreline cannot open up. The bend is measured
  from the *camera's* own position rather than the world's origin. The horizontal distance from
  the eye is mixed with that same distance wrapped onto a cylinder of `world_bend_radius`
  (`atan(distance/radius) * radius`), so a vertex a hundred blocks out is drawn at ninety-odd
  and an endless world folds into a disc of radius (π/2)·radius in front of the player, with the
  horizon a circle rather than a line. The mix is a convex one, which is what makes `world_bend`
  an amount: it scales the bend between none of it and all of it while keeping the map monotone,
  so two vertices at different distances can never trade places — a map that turns around gives
  the same piece of ground away twice, which is a crease and not a bend. The same pull is
  applied to both horizontal axes at once, by scaling the pair, rather than to x and z one at a
  time, so the bend closes the ground in towards the camera's *vertical axis* and looks the same
  whichever way the player is facing; a marker sixty blocks to one side is pulled 2.6 blocks
  inward on x and 4.3 on z, measured. The lift is
  `world_bend * world_bend_rise * radius * (1 - 1/sqrt(1 + ratio²))`: distance squared over the
  radius at first — the shape of a sheet pinned at the player's feet and lifted at its far edge
  — levelling off at `rise * radius` rather than running away. A lift that never levels off is
  what the tangent of the wrapped distance gives, and a vertex sent to infinity is not a fold
  but a broken frame; at the default 256-block radius the cap is 256 blocks of sky, which is
  more than any camera can see over. The radius is the ballast of the whole effect: the amount
  of the bend, the distance it is measured against and the height it tops out at are all one
  number, so no setting can be tuned into something unrenderable. Two things it deliberately is
  not. It is **not a projection** — the geometry moves before the camera's own matrices run, so
  walking and looking up and down go on working on a world whose vertices have moved. But the
  engine's culling does **not** simply go on working, and that is the one part of this that is
  not a shader: a vertex shader runs *after* the engine has decided what to draw, so every chunk
  is culled against a box that describes where it *was*, and the bend pulls ground into view
  that the frustum has already thrown away — looking down at a bent world, the ring of ground
  the effect exists to show is the ring that is missing. `src/core/world_cull.hpp` is the
  include's arithmetic a second time, on the CPU, answering the one question culling has to ask
  — how far can the bend move a point of a box this big, this far away — and
  `src/mesh/mesh_manager.cpp` grows every resident chunk's cull box (and every far
  region's) by that much: inward by the pull in x and z, because the mix can only ever shrink a
  distance, and upward only for the lift, because the lift is never negative. The bound is the
  box's *far corner*, evaluated once: a point's distance from the camera's vertical axis is
  largest at the corner where |dx| and |dz| are each largest, which is the same corner, and both
  of the bend's terms increase with that distance. It is a bound and not a fit, deliberately — a
  box grown by too much costs a few extra draws, and a box grown by too little is a hole in the
  world. The boxes are a function of where the *camera* is, so they are refreshed per whole
  block of camera movement with two blocks of slack already in them, which is exact rather than
  merely usually right, costs a standing player nothing but a length comparison, and writes an
  instance's box only when its margin has really moved; switching the bend off hands the mesh's
  own boxes back, so a player who never turns the effect on pays nothing for it. That last part
  was not true when it was written: what the refresh remembered about an instance was the box's
  own *position* change, which is negative whenever the box is grown — the bend pushes the
  corner out to one side — so the test for "has this box really moved?" was never once satisfied
  while an effect was on and every resident chunk was rewritten on every block of camera
  movement. The Horizon Curve's own bound needed the same bookkeeping, and the test written for
  it asked the margin to be a growth: it is a magnitude now (sideways, and taller), both
  components are zero exactly when the box is the mesh's own, and the skip fires.
  `shader_overlay.gd` is what tells the engine as well as the material — the same switch and the
  same knobs, one set per effect, named after the effect's id (`set_world_bend`,
  `set_world_horizon`) and listed in the overlay rather than in the registry, because the
  registry is what the menu offers and the water's ripple is a knob the menu offers and the
  culling has no use for — and it finds the engine by walking outwards for the first node that
  can take *every* vertex effect the registry declares rather than by a path, because the path
  it used to use pointed at the HUD's own child, which is not where the engine is, and the only
  symptom was that the culling silently never got better. Pinned by
  `probes/probe_bend_cull.gd`, which measures what the *renderer* did rather than where the
  geometry went: with the bend on and the engine not told the frame's primitives are unchanged
  to the last one — the bug stated exactly — and with the overlay's hand-off intact the same
  camera draws 3,263,096 primitives in 320 draw calls against 1,452,116 in 87, a quarter again
  of geometry and 8.3% of the frame's pixels, with the bend switched off returning the renderer
  to exactly what it started with. `probes/probe_bend_handoff.gd` answers the wiring on its
  own, in seconds and without a world to stream, and `tests/test_world_bend.cpp` proves the
  property underneath all of it: the grown box contains the bent image of every corner of every
  box, over a spread of cameras, radii, amounts and box placements (30k+ assertions), so the
  compensation is a containment rather than a guess. And it is **not collision**: collision,
  block reach and the block you are mining stay exactly where they are, so ground past a few
  dozen blocks is drawn somewhere other than where it is, and the difference is under a block at
  the distances a player actually builds and mines at. That is why this can be a look rather
  than a lie, and it is also why the *near* field is the part that has to be exact: the ground
  lifts 2.1 blocks at 40 blocks out, 8.2 at 80, 22 at 140 and 43 at 220, which is the whole of
  the effect. The water alone declares `world_bend_sway` and passes it to the include — a wave
  rolling up the sheet off the include's own clock, its phase shifted by height so it reads as
  the surface moving rather than as the whole lake sliding — while the terrain passes a literal
  0, because a world whose *ground* ripples is a different effect with a different name. The
  held item, the block-break overlay, particles and the third-person body are separate meshes
  and are deliberately not bent: they are all within a few blocks of the eye, where this
  function is the identity to within a hundredth of a block. Pinned by
  `probes/probe_bend_geo.gd`, which is the probe for a thing that cannot be seen in the
  frame's own pixels: it stands one marker at a time on the ground at 40, 80, 140 and 220 blocks
  — sized in proportion to its distance so its size on screen never changes — draws the two
  *real* materials on it and a marker that calls the include exactly as they do, and compares
  the measured centroid of the pixels it covers against the include's own arithmetic re-run in
  GDScript. With the bend off the marker lands within 0.54 px of the camera's own unprojection,
  which is the reading being sound before anything is claimed; with it on, within 0.90 px of the
  prediction at every distance; and `materials/voxel_material.tres` and
  `materials/voxel_material_water.tres` both land on the marker's own pixels to 0.00 px, with
  identical pixel counts — the terrain and the water bend by the same amount at the same place,
  which is what a welded shoreline is. It also measures the switch (0.00 px back), the amount
  (nothing at 0, 128 px of move at 1), the radius (91 px at 128 against 51 at 512) and the
  water's ripple (0.00 px of drift at 0, 11 px of wander at the top of its slider).
  `probe_shaders_shot.gd` adds the live world's own version: the bend moves 27% of pixels, 0.032
  across the far field and 0.002 along the ground underfoot — the band a player is standing on
  is the one band that does not move — and the two effects stack, so the tube scans a bent world
  rather than bending a picture of a set.

### Horizon Curve

- **Horizon Curve**
  (`shaders/world_horizon.gdshaderinc`) is the registry's third entry and the other vertex
  effect, and it is the opposite idea to the bend: nothing closes in and nothing climbs, and the
  ground a long way off is drawn a long way *down*, so a plain curves away from the player
  instead of running flat to the edge of the loaded world and the horizon becomes the place
  where the ground has fallen too far to be seen over. The geometry is the Earth's own: a sphere
  of `world_horizon_radius` touches the ground at the player's feet and the flat world is the
  plane tangent to it there, so a point a horizontal distance d out is drawn
  `radius * (1 - cos(atan(d/radius)))` below that plane — distance squared over twice the radius
  at first, and never past the radius however far away the point is — which written with
  `inversesqrt` is the same expression the bend's *lift* uses with the opposite sign, so the two
  effects are one piece of geometry a quarter turn apart. Only y moves, and that is the whole of
  why it is safe to draw: wrapping the world onto the sphere for real would also pull it in by
  `d³/(6·radius²)`, five blocks at a thousand blocks out against two hundred-odd of drop, and
  leaving that third-order term out keeps every vertical edge vertical, every distance measured
  on the ground the distance it was, and — because the drop is a continuous function of a
  vertex's world position, and the terrain and the water are both drawn through it — every
  shared edge welded, which is what keeps a shoreline from opening up as the far field sinks. It
  is included beside the bend by the same two materials and applied after it, and each effect is
  measured from where a vertex really *is* rather than from where the other one put it, so the
  two are independent and either looks the same whether or not the other is switched on. It has
  one knob and not two, deliberately: the drop goes as the square of the distance over twice the
  radius, so an "amount" of a half would be very nearly the same picture as a doubled radius,
  and a slider that does what another slider does is a slider that makes the menu worse — the
  registry offers the planet's radius in blocks, 1,000 to 32,000, defaulting to 1,500, which
  draws the far edge of the view (1,024 blocks out) 261 blocks lower than flat, the ground 512
  blocks out 80 blocks lower and the ground 128 blocks out five. The band a player builds and
  mines in is the part that does not move (0.34 blocks at 32 blocks out, under a block out to
  40), so this is a look rather than a lie for exactly the reasons the bend is. It is culled by
  the same code as the bend and in the same place: `src/core/world_cull.hpp` holds a bound for
  each effect — `world_horizon_sag` is the drop at the box's far corner, where the distance from
  the camera's vertical axis and the drop with it are largest, and the curve's bound is the
  simpler of the two because it only ever moves a box's *bottom* — and `world_cull_aabb` applies
  the bend's box and then the curve's rather than merging them, which is exact and not merely
  tidy: each effect moves a point by an amount that is a function of where the point really is,
  so the box the bend's margin produced, lowered by the sag at the *original* box's far corner,
  contains everything the pair of them can do to it. Its own sensitivity to the camera is less
  than the bend's, too: the drop's largest derivative with respect to a point's distance is
  0.385 blocks per block whatever the radius, against the bend's 1.8, so a refresh per block of
  camera movement is if anything more exactly right here. Pinned by
  `tests/test_world_horizon.cpp`: the grown box contains the sunk image of a 5×5×5 grid through
  every box, over a spread of cameras, radii (the slider's floor, its ceiling, two in between
  and the curve switched off) and placements; the growth is downward and nothing else, the plan
  and the top of the box being the mesh's own to the last bit; the one box holds both effects at
  once; and the growth a box is remembered by moves whenever the box does — the property whose
  absence was the culling bug above. Measured on the GPU by `probes/probe_horizon_geo.gd`,
  the horizon's own version of the bend's marker probe: one marker at a time at 40, 160, 400 and
  800 blocks, plus a horizontal plate spanning 150 to 700 blocks whose predicted centroid is
  area-weighted cell by cell because a tilted surface's projection is not affine, and with the
  curve off the marker's pixels land on the camera's own unprojection of its centre before
  anything is claimed. `probes/probe_bend_compile.gd` now checks both includes on all four
  shaders that consume them (the terrain, the water and a marker per effect) and both registry
  entries param by param, since an effect whose switch or knob is renamed in one place and not
  the other is an effect that draws nothing when it is turned on. And unlike the bend, which has
  to be judged by where the world's vertices go, this one is visible in the frame itself:
  `probe_shaders_shot.gd` and the eye both see a plain curve away.

### Film Grain

- **Film Grain** (`shaders/grain.gdshader`) is the registry's tenth entry, its third *screen*
  pass, and the last thing put on the picture: the tube, the trail and the two colour filters
  are all under it, so what it grains is the picture as the rest of the stack left it. It is
  deliberately the plainest pass in the file — one read of the frame and one write, with no
  history and nothing measured against the frame — so the whole of the effect is where its
  pattern comes from. The
  pattern is a hash of the *cell* a fragment falls in, a cell being `grain_size` screen pixels
  square by default one, so a cell is a pixel — measured on the frame's own pixel grid through
  `frame_size` rather than as a fraction of it, for the reason the CRT's raster is: a pattern
  defined as a fraction of the frame is a different grain at every window size. `grain_strength`
  is how far a cell is pushed from its own colour, and the delta goes to all three channels at
  once, because static is grey — a grain that moved the channels apart would colour the picture.
  The grain is *static* by default, which is exactly what it sounds like: the hash is a function
  of the cell's index, so the pattern is nailed to the screen and the world moves under it. With
  **Animated** on, the frame's own *step* is hashed in beside the cell — `floor(TIME * 24)` and
  not the clock, so the grain is held for a few frames and re-rolled at a fixed rate instead of
  once per frame, which would make the effect a different thing at 30 fps than at 144 and would
  show the frame rate rather than grain; 24 a second is what film grain is re-rolled at, and it
  is the same reason Camera Jitter holds its own steps. Alpha is forced opaque like the sepia's,
  because a transparent frame in the stack must not punch a hole through the grain instead of
  being grained.

### Noisy Blocks

- **Noisy Blocks** (`shaders/block_noise.gdshaderinc`) is the registry's ninth entry and the
  `texture` kind's first member, and its subject is neither the frame nor a vertex but the
  *atlas*: the makers' own grain, 0 to 100, over every block in the world at once and live as
  the slider is dragged. Nothing is repainted to do it. The makers' sliders are a CPU repaint of
  one texture each (`SkinPixels.apply_gray_noise` over a fixed per-texel random field, with a
  clean base kept so the slider can undo itself), and a live version of that over the *atlas*
  would be a texture upload per slider tick, because the atlas is shared by every block there
  is. So the same grain is added where the world samples it: both of the world's materials
  `#include` this file and add its delta to the texel the fragment stage has just read, before
  any light touches it, which is what makes the grain part of the block rather than a wash over
  it. It is monochromatic like the makers' — one delta on all three channels — and its scale is
  the makers' own 0..100 with the makers' own `MAX_GRAIN` as the ceiling, 0.35 of full brightness
  (`scripts/block_manager.gd`), so Noise 100 in the world is the texture the block maker's
  preview showed. The grain is a hash of the *texel*, per atlas layer: a 16×16 face is a field
  of 256 of them, and up close one texel is many screen pixels wide, so the grain is a patch of
  colour per texel, which is exactly what the makers' noise map is. Further out it is the other
  way round — one texel covering less than a screen pixel — and a field sampled finer than its
  own cells is an aliasing pattern that crawls with the camera, so the grain is faded out over
  that footprint with `fwidth` — full where a texel is a pixel or wider, gone by the time a
  texel has shrunk to half of one. Where it fades the atlas is being mip-filtered to a flat
  average anyway, and grain over an average is not this grain. The water
  passes its *flowed* coordinate, so a flowing surface carries its grain along with its texture,
  and both materials share the one field, so a shoreline is one grain and not two. It drives its
  materials exactly as the vertex effects do — `_build_material_effect` in `shader_overlay.gd`
  builds both kinds, `materials` + `enable_key` and no layer — and that is the whole of what the
  `texture` kind is: the same wiring around a subject that is neither a frame nor a vertex. What
  it does not reach is the held item's own mesh and the inventory's block icons, which read a
  block's texture directly rather than through the world's materials: this is the world's grain,
  not the picture's.
