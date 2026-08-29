**Not in any particular order, important things that will be done soon**



Block break animation + hardness values - careful: decide now if hardness gates break time, since that changes the break-block code path

Player model animations (walk/idle/etc.)

First-person viewmodel + items/blocks in hand

Isometric icon snapshots (bundle with #4, same render pipeline) - cache the icons, don't re-render every frame

Punch animation

Spawnable dummy + combat calc + knockback - careful: sketch a minimal Entity base class first so the dummy isn't thrown away later

Flesh out entity system (flow-field pathfinding, droppable item entities) - careful: decide if block breaks still insta-pickup or now drop as world items

Crosshair/outline export codes (CS-style) - small, self-contained, slot in anytime; version the code format so future options don't break old codes

LuaJIT modding API (custom models, block models, pathfinding access, explode, break/place, structure placement, on\_break hooks) - careful: no structure/schematic system exists yet, that part's new work not just a binding

