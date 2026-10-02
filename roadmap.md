**Not in any particular order, important things that will be done soon**



~~Block break animation + hardness values - careful: decide now if hardness gates break time,
since that changes the break-block code path~~

Player model animations - `Animations/Idle.anim` shipped and plays (`player_model.gd` loads it
into an `AnimationLibrary` for the `default/Idle` clip); walk, run, jump and the third-person
body still have no clips, so locomotion is the head-tracking and `ModelPivot` yaw lag described
in ARCHITECTURE.md

~~First-person viewmodel + items/blocks in hand~~

~~Isometric icon snapshots (bundle with #4, same render pipeline) - cache the icons, don't
re-render every frame~~

~~Punch animation~~

~~Phase 1 GDScript→C++ migration - port hot-path per-frame logic to native bindings:
BlockTextures (texture atlas), BlockOutline (node), ViewmodelMeshes (held item/block/sprite
meshes), ViewmodelPose (bob/sway/swing math), SkinPixels (pixel/noise helpers). SkinManager's
stateful autoload logic (persistent ImageTexture + debounced save + noise state machine) remains
GDScript for now~~

~~Spawnable dummy + combat calc + knockback - careful: sketch a minimal Entity base class first
so the dummy isn't thrown away later~~

Flesh out entity system (flow-field pathfinding, droppable item entities) - careful: the planner
that exists is budgeted A* over the chunk map (`src/pathfinding/`, no Godot deps), not a flow
field, so decide whether entities reuse it per-agent or want one shared field; and decide if
block breaks still insta-pickup or now drop as world items

Dropped items want their own animation, not the shatter - the whole item texture, intact and
rigid, thrown toward the crosshair, rotating, colliding as one piece, then falling off screen. The
shatter is for art being *destroyed*, which a drop is not. What is wanted, and what is still open,
is written up in [docs/gui-notes.md](docs/gui-notes.md) under "A dropped item is NOT a shatter".

~~Crosshair/outline export codes (CS-style) - small, self-contained, slot in anytime; version
the code format so future options don't break old codes~~

LuaJIT modding API (custom models, block models, pathfinding access, explode, break/place,
structure placement, on\_break hooks) - the structure/schematic system this used to block on now
exists (`src/schematic/`: both build-file families, `data/minecraft_blocks.json`, `paste_plan`
and `BlockEditor::apply_paste`, driven in game by `/paste`), so structure placement has real
call sites to bind to rather than being new work; what is still missing is a decision on whether
a paste is moddable at all, and on what an `on\_break` hook sees now that drops, `crush_result`
and family collapse are separate steps

GDScript is the remaining long-file problem: the C++ side is now capped at 500 lines
(`scons sizecheck`, CI-gated), while `settings_menu.gd` is **4,008** lines and `viewmodel.gd`,
`liquid_texture_lab.gd`, `inventory.gd`, `chat.gd` and `crafting_table_menu.gd` are each near or
past 900. Moving more of it into C++ for interaction performance is the old item; the cheaper
half is to give the guard a GDScript root so a 4,000-line script at least has to be split
deliberately - the menu is already sectioned by `_build_*_sections()` builders, so the split is
mechanical
