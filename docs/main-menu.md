# Main menu: the evidence, and a shape for ours

Farlands' title screen is `scripts/menu_layer.gd` over the running world, drawn by
`scripts/title_screen.gd`, with `scripts/ui_block_curtain.gd` covering the change into the game.
This page is the research behind that screen — the theory, the numbers, and the decisions it
made. It keeps the evidence graded, because a good deal of what is written about menus is
convention stated as fact, and the numbers here are starting points to test against this game
rather than rules.

Two things it deliberately keeps apart: what has been measured (Fitts's law, contrast ratios,
response-time limits, colour-emotion studies) and what is craft convention dressed up as fact
(left-aligned columns, "four to six items", the 60-30-10 rule, pixel sizes). The last section says
which is which, because the difference decides how much an argument is worth.

## What the screen is for

Three jobs, in this order:

1. Get the player into the game. For a returning player that should be one input.
2. Set the tone. It is the only screen where atmosphere is worth spending pixels on.
3. Housekeeping — settings, quit, credits, version. Necessary, and none of it competing with 1.

A designer's summary of the usual failure: most weak menus fail the first job while trying very
hard at the second, and "a beautiful screen that takes four inputs to get past is a worse menu
than a plain one that takes one" ([h-idris.com](https://h-idris.com/blog/game-main-menu-design.html)).
The first job also has a deadline: aesthetic judgments of a screen form within **50 ms** of
exposure, with effects detectable at **17 ms** in the original study ([Google
Research](https://research.google/pubs/the-role-of-visual-complexity-and-prototypicality-regarding-first-impression-of-websites-working-towards-understanding-aesthetic-judgments/)).
That is appeal, not usability, but it means the composition has to work as a still frame.

## What the psychology supports

| Principle | What it says | What it means here |
|---|---|---|
| Fitts's law | `a + b·log₂(2D/W)`: bigger and closer targets are faster, and a screen edge is an infinite target ([NN/g](https://www.nngroup.com/articles/fitts-law/)) | the whole row is the target, tall, at rest or against an edge |
| Hick's law | decision time rises with the number and complexity of choices ([Laws of UX](https://lawsofux.com/hicks-law/)) | few first-level choices — not "7±2" ([NN/g](https://www.nngroup.com/videos/hicks-law-long-menus/)) |
| Serial position | the first and last items are remembered best ([Laws of UX](https://lawsofux.com/serial-position-effect/)) | primary first, nothing essential in the middle |
| Von Restorff | the item that differs is the one remembered | exactly one visually distinct item |
| Aesthetic-usability | attractive interfaces are judged more usable ([NN/g](https://www.nngroup.com/articles/aesthetic-usability-effect/)) | polish buys patience and hides bugs |
| Choice overload | more options can cost decisions, conditionally ([Iyengar & Lepper](https://business.columbia.edu/faculty/research/when-choice-demotivating-can-one-desire-too-much-good-thing)) | keep the first decision small |
| First-click | a correct first click predicts success ([MeasuringU](https://measuringu.com/first-choice/)) | the right click must be obvious unread |

The numbers behind the last two rows: the jam study found 6 options chosen more often than 24–30,
and a meta-analysis of 99 observations and 7,202 participants found the effect conditional — on
task difficulty, option complexity, uncertainty about preferences and the wish to save effort
([Kellogg](https://www.kellogg.northwestern.edu/academics-research/research/detail/2015/when-product-assortment-leads-to-choice-overload-a-conceptual/)).
On first clicks, a correct one led to success 87% against 46% after a wrong one, a live-site
replication found 78% against 70%, and tree tests 80% against 14%.

The combination that matters: Fitts says bigger and closer wins, Von Restorff says only one thing
can be "the different one", and Hick says every extra peer-level choice costs time. Together they
argue for one large primary action and a small set of visibly subordinate others — not five
buttons of equal weight.

## Position: the art decides, not a rule

The honest finding is that there is no studio standard for button sizes, screen thirds, or
left-versus-centre. A searched-for "one-third column" rule has no source, Game UI Database
returned 403, and GDC Vault's pages were down, so there is no measured prevalence to quote. What
decides the layout is three things:

- **what the art needs** — if the backdrop has a subject, the menu must leave it visible;
- **what the input is** — a pointer wants short travel, a pad wants a predictable order;
- **reading order** — top to bottom is scanned predictably; a diagonal or staggered layout costs
  comprehension for nothing.

| Layout | Reads as | Best for |
|---|---|---|
| vertical list, left column | classic, unambiguous, art has room | almost anything with a scene behind it |
| vertical list, centred | ceremonial, title-forward, symmetrical | few actions and symmetrical art |
| corner-anchored over key art | cinematic, the art is the product | art-led games, with disciplined contrast |
| in-world / diegetic | immersive, slower to read | horror, sim, narrative |
| hub / dashboard | dense, live-service | modes, seasons, progression |

Minecraft's menu is centred, not left: the panorama is symmetrical, the logo is the focal point,
and there are only a handful of actions. The precedent is the point — the art chose the layout.

### Geometry to start from

Sizes are in **units**, the same unit `scripts/ui_scale.gd` owns (one unit is one GUI-scale pixel,
so a 32-unit row is 64 px at scale 2). All of it is a hypothesis to test, not a measurement.

| Element | Starting value | Why |
|---|---|---|
| safe margin | 48 units | ~5% of a 1920-wide screen at scale 2; verify per platform |
| menu column | 210 units wide, left-aligned at the margin | fits the longest label with padding |
| row | 32 units tall, 6-unit gap | big target; 12 px apart at scale 2 |
| label | 15 units | readable at a glance, still a menu not a poster |
| wordmark | ~55 units cap height, on the column's left edge | sits above the list, one straight vertical reading path |
| first row | ~180 units from the top | leaves the wordmark its own air |
| footer | 7 units, ~65% white | version and seed, outside the reading path |
| rows on screen | at most five | the sixth is a sign the menu is carrying somebody's feature request |

Two rules the geometry follows from the evidence:

- **Never move a target while it is being aimed at.** A hover that slides a label 20 units
  sideways is hostile: Fitts's law assumes a stationary target. Scale, glow, fill and a 2–4 unit
  nudge are fine; travel is not.
- **Full-width rows beat text-width buttons.** Once the pointer is in the column the horizontal
  component of the movement stops mattering, and the hit area comes for free.

## Colour

### Value is the readable part

WCAG AA asks for **4.5:1** for normal text and **3:1** for large text (at least 18 pt, or 14 pt
bold); 4.499:1 fails ([W3C](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html)).
Non-text UI components need **3:1** against their neighbours
([W3C](https://www.w3.org/WAI/WCAG21/Understanding/non-text-contrast.html)). Xbox's accessibility
guideline applies the same numbers to **game menu text** and says to check the least-contrasting
part of a non-solid background
([Microsoft](https://learn.microsoft.com/en-us/xbox/accessibility/xbox-accessibility-guidelines/102)).

That last clause is the whole problem with a backdrop. There is no single background, only a
changing one, so the check is against the **brightest frame** of whatever is behind the labels, not
against the frame that looked good while designing. NN/g's text-over-image case study shows the
method: raising a black backing from 30% to 50% opacity moved white text from failing to meeting
the 3:1 large-text threshold ([NN/g](https://www.nngroup.com/articles/text-over-images/)). The
lesson is the method, not the number.

### One accent

NN/g's guidance is a small palette — typically no more than three colours — with the accent
reserved for what should stand out most, used consistently
([NN/g](https://www.nngroup.com/articles/color-enhance-design/)). The 60-30-10 split is the usual
shape of that:

- 60% the scene (sky, terrain, the game itself);
- 30% the quiet zone (scrim, plate, vignette);
- 10% the accent — the primary action **and** the focus state, nothing else.

Putting the accent on the wordmark, the primary button and the selected row at once is three
isolations cancelling out (Von Restorff again).

### Warm, cool and saturation

A 2025 systematic review covering **132 reports from 1895–2022, 42,266 participants in 64
countries** found recurring associations: yellow and orange with positive high-arousal emotions
(fun, excitement), green and blue with positive low-arousal ones (comfort, calm), red with high
arousal of both valences; lighter colours leaned positive, darker negative; and **more saturated
colours were associated with high arousal and high power**
([Psychonomic Society](https://featuredcontent.psychonomic.org/color-me-impressed-psychology-research-links-colors-and-emotions-for-over-a-century/)).
These are tendencies across a century of reports, not laws about a specific screen.

For a calm voxel sandbox that suggests a low-saturation, cool field and one warm saturated accent
for the action — which is also what this world does at sunrise. There is already a colour to read
it from: `ChunkManager.get_body_sky_warmth` is the same warmth the body's material is lit with, so
the UI accent and the world's light can agree by construction rather than by matching hex codes.

### Dark screens and glow

A review of polarity studies found dark text on light backgrounds performing better for acuity and
proofreading for normal vision, with some low-vision users (cataracts) doing better in dark mode
([NN/g](https://www.nngroup.com/articles/dark-mode/)). Short, large menu labels are the case where
a dark screen is least risky. What does not work is relying on bloom to make the type readable:
keep crisp letterforms, and use an outline or a backing plate where the art is busy. Xbox's
guideline illustrates exactly that and recommends configurable contrast.

### Colour is never the only carrier

Roughly **8–10% of males** have difficulty distinguishing red from green, and the Game
Accessibility Guidelines say to reinforce colour with a label, symbol, shape or pattern
([GAG](https://gameaccessibilityguidelines.com/ensure-no-essential-information-is-conveyed-by-a-fixed-colour-alone/)).
The focused row therefore needs a bar, ring, fill change or marker — not just a hue shift.

## Type

Two faces at most: one display face for the wordmark, one UI face for everything else. Labels at
15 units, footer at 7; all-caps wants +2–4% tracking; tabular figures where a number can change
("2 h 14 m played"). Avoid hairline weights over art — they vanish at the brightest frame. And run
the ten-foot test: stand across the room and read every label, because that is the distance a
console player actually reads from.

## Reaction: three layers, never a blocked input

| Event | Target | Why |
|---|---|---|
| any input acknowledged | **100 ms or less** | below it, the interface feels instantaneous ([NN/g](https://www.nngroup.com/articles/response-times-3-important-limits/)) |
| hover / focus transition | **100–150 ms**, ease-out | fast enough to feel caused, slow enough to read |
| press | state changes at once, 60–100 ms visual depress | a press that waits for an animation feels broken |
| screen transition | **200–300 ms**; exits shorter than entries (Material: 195 out, 225 in, above 400 feels slow) | the same range NN/g gives for modals ([NN/g](https://www.nngroup.com/articles/animation-duration/), [Material](https://m1.material.io/motion/duration-easing.html)) |
| entrance | 400–600 ms, **once**, staggered 30–50 ms | replaying it on every return is a tax |
| ambient backdrop | 0.3–0.8°/s drift, no cuts | atmosphere, not an attention grab |
| a wait | feedback after 1 s, progress and a way out after 10 s | Nielsen's three limits |

Three rules worth more than the timings:

1. **The state changes on press; the animation decorates it.** If the next input cannot register
   until the animation finishes, the menu is slow whatever the timer says.
2. **Focus is a persistent state, not a pulse.** WCAG requires keyboard focus to stay visible with
   no time limit ([W3C](https://www.w3.org/WAI/WCAG22/Understanding/focus-visible.html)); the
   stronger AAA criterion is a 2-unit-perimeter indicator with a 3:1 contrast change. On a pad,
   focus is the entire interaction.
3. **Ease-out for arrivals, ease-in for departures.** A bounce is a choice, and a bounce on every
   focus change becomes noise.

Sound belongs to the same system: a light focus tick, a distinct confirm, a distinct back, all
starting inside that 100 ms window, with a separate UI volume. Guard against held-input repeat —
a stick swept across five rows must not fire five ticks.

Motion accessibility is no longer optional. Xbox's guideline names moving backgrounds behind text
and asks for a way to pause, hide or disable them
([XAG 117](https://learn.microsoft.com/en-us/xbox/accessibility/xbox-accessibility-guidelines/117));
the Game Accessibility Guidelines say the same
([GAG](https://gameaccessibilityguidelines.com/provide-an-option-to-turn-off-hide-background-movement/));
Apple's reduced-motion guidance says to replace meaningful motion with a dissolve or a colour
shift rather than deleting all feedback
([Apple](https://developer.apple.com/help/app-store-connect/manage-app-accessibility/reduced-motion-evaluation-criteria/)).

## The two states of the menu

This is where the research is most unanimous and where menus most often ship broken.

| State | Primary action | What changes |
|---|---|---|
| no save | New World | Continue is **absent**, not greyed out, and never focused |
| save exists | Continue | say what is being continued: world name or seed, and when it was last played |
| New World | — | confirm the overwrite; the seed field lives here; say that it regenerates chunks |
| update / news | Continue, unchanged | a quiet marker on a secondary item, never a modal in front of Play |
| Quit | — | confirm; and Escape on the title screen must not quit |

The recurring bug is worth naming because it is one line: a greyed-out Continue on a first run,
above New Game, with focus on the disabled item. It only happens if nobody launched the game with
an empty save folder.

## What the genre does with the background

| Game | Backdrop | Shows the player's own world? |
|---|---|---|
| Minecraft (Java) | a slow-panning panorama built from six cube-face images, with adjustable blur ([wiki](https://minecraft.fandom.com/wiki/Panorama)) | no — a curated, version-specific scene |
| Vintage Story | a background image layer with menu dialogs drawn over it, blurred title bars ([1.17 notes](https://www.vintagestory.at/blog.html/news/stable-de-jank-and-game-juice-update-1170-r329/)) | no |
| Cube World Omega | a slowly rotating landscape starting near the last-played character, animated logo above it ([dev post](https://wollay.com/blog/a-new-start-screen/)) | partly — a remembered position, not a live save |

The pattern is that **full-bleed world art is the genre's signature and the menu owns a quiet zone
over it**. None of the verified examples uses a full-screen dark scrim by default; Minecraft blurs,
Vintage Story panels. All of them solve the same problem, and none of them shows the world the
player is about to enter.

## What was built

The engine offers something none of those games does: **our own generator is the art**, and here
it is not a picture of it. The title sits over the live world, which streams while the menu is up
and drifts its camera slowly so the backdrop is not a still frame. That makes CONTINUE literal —
the player is already standing in the world, so there is no load behind the button, and the cover
only has to hide a mouse-mode change.

The engineering shape is an autoload `CanvasLayer` rather than a node in `Main.tscn`. The reason
is the probe suite: seventeen probes instantiate `Main.tscn` by hand and drive the player, and a
title screen living in that scene would freeze every one of them. The layer arms itself only when
`SceneTree.current_scene` is the world, so a hand-made fixture never sees it.

The freeze reuses `PlayerController.set_settings_open`. That flag is already the gate every
system respects — movement, look, mouse capture, the block outline, the crosshair, the compass,
the interaction door — and a second "menu is open" flag would be one a future system could
forget, leaving a player who can break blocks through the title screen. The cost is that
`is_settings_open()` is true while the title is up, which is worth knowing when reading either
side.

Built composition, in units (`scripts/title_screen.gd`; the margin, wordmark and first row shrink
with the window so a 720p screen does not push Quit into the footer):

| Element | Value |
|---|---|
| scrim | 0.34 at the top to 0.80 at the bottom, in bands, plus a left plate at 0.30 |
| margin | 40; column 240 wide |
| rows | three at 28 tall, 6 apart, first at 168; labels 12, buttons from `button.png` |
| wordmark | 40 units cap height at 54 down, one accent underline |
| footer | 7 units: `v0.1 · seed 12345` |
| accent | one colour for the underline and the focused row's bar, 35% toward the sky warmth |

Sequence: CONTINUE when a save existed before this launch, PLAY when it did not, then SETTINGS,
then QUIT. Settings is the existing `scripts/settings_menu.gd` on its `pause` page, so there is
one settings screen rather than two, and closing it returns to the title with focus on the
primary action.

**No New World yet, on purpose.** A safe reset needs the chunk map, the edit maps, the save
generation and the files on disk cleared as one operation; from GDScript a wipe races the exit
flush, which can rewrite what was just deleted. Until the engine can do it, the menu does not
offer a button that destroys a world.

## Transitions between screens

The timing rules bound any transition effect: acknowledge the input immediately, keep the whole
thing inside 200–300 ms, make the exit shorter than the entrance, and never block the next input
while it plays. A transition that hides a wait is a different tool from one that is the reward
for pressing a button; the first has to be shorter than the wait it covers, the second still has
to clear the 300 ms ceiling.

The curtain (`scripts/ui_block_curtain.gd`) is the one deliberate exception to the 300 ms figure,
and it is worth being explicit about why: it is not hiding a wait, it is the reward for pressing
Play. What it must not do is cost the player anything they can feel — so it is skippable from 80
ms in, and it never runs before the world is ready.

It is also not an animation. A sheet of **12 × 8 rigid bodies** (one more row than fills a 16:9
screen) is released above the top edge and the engine does the rest: gravity, random mass and spin
per block, friction, collisions with each other and with a floor and two walls. Nothing schedules
an arrival. Measured by `probes/probe_menu.gd` on a 1080p surface: the wall covers **97% by about
1.3 s**, every block has landed by **1.0–1.4 s**, the pile holds for **0.6 s**, the swap happens
behind an opaque frame, and the reveal — the floor removed, nothing pushed — takes **0.9 s**.

Four things had to be true for that to look like weight instead of a glitch, and each was found by
measuring rather than by eye:

- **The solver has to see the landing.** A block falling a screen and a half under 4.4× gravity
  arrives at ~2,600 px/s, which at 60 Hz is forty pixels between contact checks — a quarter of the
  block. The engine then discovers the floor late and resolves the overlap it finds, which is the
  shake. The transition therefore runs its own physics at **240 Hz** and puts the project's rate
  back when the curtain is done; same gravity, same masses, just resolved. The deepest overlap
  between two colliders fell from ~63 px to ~15 px of a 138 px block.
- **A landed block becomes the pile.** Each body freezes as soon as it has stayed within 8 px of
  its window's start for 0.06 s while resting on the floor, a wall, or a block that has already
  landed. Frozen is static in the engine, so the pile cannot shiver itself — an awake block
  pressing on it has nothing to move.
- **"Settled" is the pile's surface, not a timer.** The top edge has to stop climbing and at least
  90% of the sheet has to be on screen; only then does the hold begin. The blocks still negotiating
  with each other below the surface are invisible, because the wall is pure black and so is the
  backing behind it.
- **The cover is structural at the swap.** The pile does not tile, so an opaque backing fades in
  under it at the end of the hold and `covered` is emitted behind that frame. There is no frame on
  which the world can show through, and the swap never depends on the pile being perfect.

Two details are invisible for the same reason the effect works: the collider is 0.86 of the drawn
square and the drawn square is 1.18 of its cell. Both are black on black, so neither the packing
gaps nor the overlaps between neighbours can be seen, while the bodies get the room a discrete
solver needs. It is what makes a wall of 96 hard squares rest instead of grinding.

**It is skippable.** Any key or click after 80 ms lands the cover at once and finishes the reveal
at once. A transition is not allowed to hold the player, however good the fall is.

Reduce Motion (`Settings → General → Interface → Reduce Motion`) drops the cells entirely and
fades both ways in 0.14 s. That switch exists because a full-screen field of falling objects is
exactly the motion the setting is for, and it is read from `settings.cfg` by the menu layer so the
transition itself does not depend on the settings scene being open.

## How to test it

1. **Five-second test**: freeze one frame and ask what the game is and what the viewer would press.
2. **First-click test** in both save states, with a new and a returning player.
3. **Brightest-frame contrast audit**: sample the label pixels across a full loop of the backdrop,
   at 1280 and 1920 wide, and at the largest GUI scale.
4. **Empty save folder**, and a corrupted save.
5. **Pad-only run**: is anything focused on arrival, is it obvious from ten feet, does the order
   wrap, does B do nothing on the title.
6. **Motion off**: everything still readable and operable.
7. **Time to play**: process start to interactive menu (target under a second) and to controllable
   (target under two, with the world already warm).

## What not to do

Two equally styled primaries; a greyed Continue on first run; nothing focused on arrival; buttons
that travel under the cursor; nine top-level items; a logo bigger than the actions; an unskippable
intro; news in front of Play; colour-only focus; music at full volume on the first frame;
unconfirmed Quit; unconfirmed New World overwrite; the entrance replayed on every return.

## Where the numbers come from

**Measured**: Fitts's law; Hick's law as a direction rather than a count; the WCAG contrast ratios;
Nielsen's 100 ms / 1 s / 10 s limits; Material's transition durations; the colour-emotion review,
as tendencies; choice overload as a conditional effect; first-click success as task-dependent.

**Craft convention**: left-aligned columns; four to six top-level items; 60-30-10; the 5% safe
margin; every unit value in the geometry table. Useful defaults with no experiment behind them.

**Unverified**: rule-of-thirds for menus has no game-specific evidence; no controlled study
isolates bloom on menu type; and there is no measured prevalence of layouts, because Game UI
Database refused the request and GDC Vault was down. Anyone quoting "most games do X" as a
statistic is guessing.

