# Possibilities

Ideas for the UI, most of them small and precise, in the spirit of the texel-accurate
debris floors: things that cost little and are felt every second of play. None of them
are tasks — [roadmap.md](roadmap.md) is the list of things that will be done, and an
item moves there when it gets picked up.

Swept on 2026-10-03 while fixing the shatter's floors. What shaped the list, checked in
the tree at the time: **there is no audio anywhere** (`AudioStream`/`AudioServer` — zero
hits in `scripts/` or `src/`); inventory items set no tooltips, though the mechanism is
already used on buttons (`settings_menu.gd:632`, `wand.gd:356`); and hurt resistance is
simulated in `dummy.gd:63-66` with **no screen feedback at all**. Already present and
worth building on: stack counts (`inventory.gd:235`), ten real crack textures
(`block_break_overlay.gd`, `CRACK_COUNT = 10`), a configurable crosshair with a contrast
shader (`crosshair.gd`), the pixel-recolor hover/selection highlights, and the whole
`UIShatter` effect.

---

1. **Sound — the elephant, and it is not UI.** Nothing plays anywhere. This is the largest
   "feel" item by an order of magnitude, and the version that fits this project is the
   precise one rather than the musical one: one break noise per material family (stone,
   wood, glass, foliage), a click pitched by slot on hotbar scroll, footsteps keyed off
   the block actually underfoot. Listed first despite being outside the UI, because
   nothing else on this page competes with it.

2. **Item name popup above the hotbar on scroll.** The selected item's name fades in over
   the bar for ~2s when the wheel moves the selection, then out. The most-loved UI tell
   in the genre, entirely absent, and roughly twenty lines. Reuses the same art and the
   same fade curve as `scripts/ui_shatter.gd`, so it reads as part of the same effect
   rather than a new one bolted on.

3. **Tooltips on inventory items.** Name and count on hover, in both grids and on the
   hotbar. The mechanism already ships — `tooltip_text` on buttons — slots just never set
   it. Cheap, and right now the inventory assumes you already know what an iso cube is.

4. **Hurt feedback: a directional red vignette plus a crosshair kick.** `dummy.gd` already
   computes the 20-tick hurt window and the direction a hit came from; none of it reaches
   the screen. A red edge vignette pulsing from the side you were struck, and the crosshair
   jumping out a texel and snapping back. The biggest missing *gameplay* feel item here.

5. **A heartbeat in the healthbar.** Below full health the hearts breathe; under four
   half-hearts they pulse faster and drift toward red. Hearts already became the one
   element that comes apart (see [docs/gui-notes.md](docs/gui-notes.md)); this makes them
   the one element that is *alive*, in the same per-heart draw `healthbar.gd:99` already
   does on every frame.

6. **A "stamp" on the craft output cell.** A successful craft currently announces itself
   only by the *ingredients* coming apart, which reads like losing things — ambiguous at
   best. The output preview should punch in with a one-shot scale pop and a ring in the
   produced item's own colour. Success needs its own signal.

7. **A ghost recipe hint in empty craft cells.** A faint icon (say 15% alpha) of what the
   current grid can produce. `data/recipes.json` already holds every recipe, so this is a
   lookup rather than new content. It is the difference between the crafting grid being a
   puzzle and being a chore.

8. **Stack count legibility.** Counts draw as bare text at `inventory.gd:235-236`, straight
   over whatever the icon happens to be, and are unreadable on a light block. A dark plate
   behind the number, plus a one-frame scale punch when the count changes. The smallest
   item on this page and probably the one that gets noticed most.

9. **Crosshair grows corner brackets when something is targetable.** It already has a
   contrast shader and configurable geometry. A plain dot when aiming at nothing, four
   brackets when a block is in reach, says "you can break this" without being read.

10. **The viewmodel swing tightens with block hardness, and the block spits its own colour
    at each crack stage.** Cracks are ten real textures, but the swing cadence is constant
    regardless of how much life the block has left, so breaking stone and breaking dirt
    feel identical. Chips of the block's own colour — sampled the way
    `scripts/block_icon_art.gd` already samples icons — tie the two together.

---

If only three get done: **2, 4, 5**. All three are self-contained, none touch the engine,
and each is a loop that already runs every frame.