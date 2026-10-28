extends Control
## The title screen: the wordmark, the action column, the world's seed in the
## footer, and the update log on the right.
##
## Two pages share the screen: the title itself (five rows) and the world list that
## SINGLEPLAYER opens. A page swap rather than a second scene, because everything a
## second screen would need - the scrim, the wind, the row factory, the focus
## machinery, the accent bar - is already here, and a copy of it would drift.
##
## It is drawn in units (`scripts/ui_scale.gd`), so at GUI scale 2 a 240-unit row
## is 480 px and the whole screen grows with the rest of the interface. The list
## is a `VBoxContainer` of real `Button`s rather than painted rows: buttons bring
## mouse hover, keyboard focus, gamepad focus and `pressed` with them, and the
## focus state is the one thing this screen cannot afford to hand-roll.
##
## The screen only reports what was pressed. `menu_layer.gd` owns the state — the
## save that decides CONTINUE from PLAY, the transition, the player gate — because
## a title screen that freezes the game itself could not be used to test the game.

signal start_pressed
signal settings_pressed
signal quit_pressed
## A world was picked out of the list. The layer treats it exactly as it treats the
## primary action: the player is already standing in that world.
signal world_chosen

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
# The row's texture, swapped to the candidate asset (`test_menu_button.png`, 200 x 40) while it
# is looked at. The log's frame is still cut from the house `button.png`, which is why the probe
# reads those three bevel tones out of that file by path rather than through this constant.
const BUTTON_TEX: Texture2D = preload("res://textures/gui/test_menu_button.png")
# The player's own skin, for the profile card's portrait: the head's front face is the 8 x 8
# region at (8, 8) of Minecraft's 64 x 64 skin layout, drawn nearest so it stays pixels.
const SKIN_TEX: Texture2D = preload("res://textures/skin.png")
const SKIN_HEAD_REGION := Rect2(8, 8, 8, 8)

# Units. The row is the asset's own box: 200 x 40 for the test button, twice the house row's
# height, so the texture is drawn 1:1 instead of being squashed into a shape nobody can judge.
# The gap is the house 5, unchanged, so the five still read as one evenly spaced list.
const MARGIN := 40.0
const COLUMN := 200.0
const ROW_H := 40.0
const ROW_GAP := 5.0
# The column starts close under the wordmark's underline, not a screen-third down: the air
# between the two is `WORDMARK_AIR`, and this is only the floor under it - a wordmark that has
# grown to the column's width pushes the list down rather than landing on the first row.
const FIRST_ROW := 120.0
# Five rows is the documented ceiling for this screen (docs/main-menu.md): the sixth
# is a sign the menu is carrying somebody's feature request. CONTINUE is the primary
# action, SINGLEPLAYER is the world list, MULTIPLAYER is not in this build yet, and
# then the two housekeeping rows.
const ROW_TEXT_PRIMARY := "PLAY"
const ROW_TEXT_RESUME := "CONTINUE"
# The wordmark is the column's width: its right edge is the buttons' right edge, which is the one
# relationship here the eye can check on the screen. Its size is therefore MEASURED from the font
# rather than tabled - `WORDMARK_REF` is only the size it is measured at. The rule hangs a fixed
# air under the word's own BASELINE (below), not at a multiple of its size, and the air above the
# first row is a fixed number, so the block moves as one.
const WORDMARK_REF := 40.0
# Units between the letters and the rule. It is smaller than `WORDMARK_AIR`: the rule belongs to
# the word - it is the word's underline - and the list is the next thing on the screen, so the title
# and its rule read as one object with the rows below them. 14 units is 28 px at 1080p, against 44
# under the rule to CONTINUE: close enough to belong, far enough to be an underline rather than a
# shelf the letters stand on (6 units was tried, and read as the second).
const WORDMARK_RULE_AIR := 14.0
const WORDMARK_AIR := 22.0
const WORDMARK_TEXT := "FARLANDS"
# The border: the word again, in black, one copy per direction, drawn BEHIND the white one. It is
# a copy at the same size and not a larger font size, which is the obvious way to write this and
# the one that does not work: growing the font grows the WORD's width about six times faster than
# its height. Measured at +8 px on the fitted 111 px word, the black came out 15 px wide at the
# sides, 2.5 px above and below it, and the word itself 7.7% wider - past the right edge the
# white word is fitted to, which is the one relationship this screen can check by eye. Offsetting
# copies of the same word gives the same border everywhere instead.
# Eight copies, not four: a cross stops the edge but leaves the inside corner of every step bare,
# and on a pixel face that is exactly where a border is looked at. The diagonals are 1/sqrt(2) of
# the straight ones so no corner is thicker than an edge, and every offset is rounded to a whole
# device pixel in _layout(), because a border on a half pixel is a soft border.
# The thickness, in units, is the other half of that measurement. The letters of this word keep
# about 10 px of air between them at 1080p - `WORDMARK_GAP`, and it scales with the word, so the
# budget is scale-free. A border takes its share out of that air from BOTH sides, so anything over
# half the gap closes it: at 3 units (6 px) the neighbouring letters' borders met and the word drew
# as one black mass with a wedge in every gap, which is what a first pass at this did. 1.5 units
# leaves 2.5 of the 5 units of air open, and the letters keep their own edges.
const WORDMARK_BORDER := 1.5
const WORDMARK_GAP := 5.0
const WORDMARK_BORDER_COLOR := Color(0, 0, 0)
const WORDMARK_BORDER_DIRS := [
	Vector2(-1.0, 0.0), Vector2(1.0, 0.0), Vector2(0.0, -1.0), Vector2(0.0, 1.0),
	Vector2(-0.70710678, -0.70710678), Vector2(0.70710678, -0.70710678),
	Vector2(-0.70710678, 0.70710678), Vector2(0.70710678, 0.70710678),
]
const LABEL_UNITS := 8
const FOOTER_UNITS := 7
const FOCUS_BAR_W := 3.0
const FOCUS_BAR_INSET := 8.0
# The scrim is ONE darkening: the plate the interface sits on. It is darkest at the left
# edge, where the labels start, eases down across the column, and is gone just past the
# buttons - so the world is back to itself for the other four fifths of the screen. The
# easing is what makes it a plate rather than a slab: held flat to the column's edge and
# dropped over a short tail, the only tones on screen are the two ends and the two or three
# cells of the tail, which reads as a blob of four colours.
const PLATE_ALPHA := 0.58
# How far past the column's right edge the plate reaches nothing, in units, so it grows with
# the interface. "A little after the buttons" rather than a gradient across the screen.
const PLATE_TAIL := 36.0
# A clean gradient is a printed card, so every square carries a little darkness of its own:
# a per-square random offset, keyed on (col, row) so it survives a resize, scaled by how much
# plate is at that point. Per-square rather than smoothed, because the point is that the
# squares are the unit - a soft wash would be a second, invisible grid underneath this one.
const NOISE_ALPHA := 0.06
# The wind. Every `LIFT_EVERY` seconds a square is picked off the plate at random and carried
# to the right, fading over the last `LIFT_FADE` of its life; the hole it leaves fades back in
# over `HEAL_TIME`. A picked square stays a square: same size, same flat colour, just moved.
const LIFT_EVERY := 0.09
const LIFT_LIFE := 1.1
const LIFT_FADE := 0.45
# Speed is in cells, not pixels, so the wind is the same gesture at any GUI scale.
const LIFT_CELLS_PER_SEC := 7.0
const LIFT_DRIFT := 1.2
const HEAL_TIME := 0.5
const SCRIM_TINT := Color(0.015, 0.018, 0.024)
# What makes a square solid is that it is ONE fill, not that its neighbours are far from
# it. Quantising the tone to make the grid obvious costs the gradient: nine steps turn a
# smooth plate into a handful of visible terraces. So the tone is continuous, sampled once
# per cell at its centre, and each cell is filled flat - the gradient is the gradient, carried
# by squares.
# The accent, mixed with the world's own sky warmth when one is available: the focused row's
# bar and its border. The wordmark's rule is NOT the accent any more - it is steel (below),
# because the rule and the title are one object and a warm bar under a white word reads as a
# second, competing accent on the one element that is not asking for attention.
const ACCENT := Color(1.0, 0.76, 0.42)
# The wordmark's rule, as a piece of steel: a lit top edge, the face, the face turning away, and
# the underside in shadow. Four bands rather than one flat colour, because a single colour is a
# stroke - a bevel is an object, and the bevel is the language the buttons and both cards already
# speak (one texel of light above, one of shade below). The tones are the ROW's family, not a separate
# blue: the button texture is neutral grey (#6d6d6d fill, #9b9b9b lit, #424242 shaded) and the log's
# own bevel is #686e76, so the rule is the same near-neutral with a few points of blue in it - 14
# between red and blue, the log's own bias. The first version was a saturated steel (#b4c2ce to
# #2e3843, 40 points of blue over red) which read as a blue object parked next to grey ones. Written
# to the 8-bit values of their hex, like the row tints, so a measured colour and the constant are
# the same number.
const RULE_HI := Color(0.650980, 0.678431, 0.705882)  # #a6adb4
const RULE_FACE := Color(0.482353, 0.509804, 0.541176)  # #7b828a
const RULE_LOW := Color(0.341176, 0.364706, 0.392157)  # #575d64
const RULE_SHADE := Color(0.168627, 0.184314, 0.207843)  # #2b2f35
# One band is one interface unit, so the rule thickens with the rest of the interface instead of
# staying a hairline at 4K or a slab at 720p.
const RULE_BAND := 1.0
# The ends come to a point. A beam that stops in a flat wall leaves two square corners on the one
# horizontal the eye follows across the whole screen; tapering to a point at the rule's mid-line
# ends it in a shape instead. Every band's end lands on the same diagonal, so the silhouette is two
# straight edges meeting at a tip - not a stair of four steps, which at 2 px per unit is what a
# per-band inset would look like. The TIP, in units, is measured in from the column's edge and
# takes the same share of each end, so the rule still spans exactly the wordmark's width.
#
# 8 units for a 4-unit bar: a long, shallow taper. The first version was 3 units, which is a 37
# degree point - the diagonal climbs faster than the eye reads it, so what it actually showed was
# four coarse 2-px steps on each end rather than a point. A shallow taper also spends more of its
# length on the ramp, which is what "a transition into the edge" means here.
const RULE_TIP := 8.0
# A black edge round the beam, the way the buttons and both cards carry one. It is what separates
# a light object from whatever is behind it, and behind this one is the plate at its brightest end
# - the rule's own underside and the plate are close enough in tone that the point would otherwise
# dissolve into it. A whole unit: at 1080p that is 2 px, the same texel the buttons and both cards
# bevel with, and half a unit came out a hairline. It is drawn as an offset of the beam rather
# than an inset of it, which is why 2 px stays an edge and does not become the cap that made the
# point look thick.
const RULE_EDGE := 1.0
const RULE_EDGE_COLOR := Color(0.043137, 0.047059, 0.054902)  # #0b0c0e
# What each row turns while it is the current one. The colour says what the row does before it
# is pressed: green to go in, steel to open housekeeping, red to end the session - and Quit is
# already two presses, so the colour is a warning, not the only warning. Pressed takes the same
# hue darker rather than a different state.
#
# The green and the red are MUTED, each pulled about a third of the way to the button's own
# grey. At full strength (#66a93d and #a83c32) they read as signals on a dark plate - the green
# like a status light, the red like an error message - which is louder than a menu row should
# be. The steel is the full-strength colour because a desaturated blue is already quiet.
# Written to the 8-bit values of their hex rather than to three decimals: the probe multiplies
# each modulate back through the fill and compares it to the hex, so a rounded constant would
# be a different colour by four times the tolerance.
const TINT_PLAY := Color(0.407843, 0.580392, 0.305882)  # #68944e, was #66a93d
const TINT_SETTINGS := Color(0.356863, 0.450980, 0.568627)  # #5b7391
const TINT_QUIT := Color(0.576471, 0.301961, 0.278431)  # #934d47, was #a83c32
# SINGLEPLAYER is a second door into the same world, so it is a second green rather than
# a different colour: a teal, which reads as "go" and still tells the two rows apart.
const TINT_SINGLE := Color(0.329412, 0.564706, 0.478431)  # #54907a
# MULTIPLAYER does nothing in this build. Its colour is a muted violet - desaturated,
# because a row that cannot act must not advertise itself with a live-looking hue.
const TINT_MULTI := Color(0.435294, 0.415686, 0.549020)  # #6f6a8c
# The update log. It sits in the emptiness on the right, which is not emptiness any more
# once something is there - and it is a CARD, not floating text, because the right-hand
# sky is the brightest thing on the screen and grey text on it fails contrast outright.
# 0.52 of the scrim's tint puts the brightest sky at ~96 luminence, which is 4.9:1 against
# the entries; the research's own case (NN/g) is the same move, raising a black backing
# until the labels pass.
const LOG_PATH := "res://data/changelog.json"
# The column's own width, rounded to whole cells below - so the card is as wide as the buttons
# and the left side of the screen is one vertical block rather than three widths. The longest
# entry needs 129 of these units, so there is room to spare at every label size.
const LOG_W := COLUMN
const LOG_PAD := 9.0
const LOG_HEADING_UNITS := 7
const LOG_BODY_UNITS := 6
const LOG_LINE_H := 9.0
const LOG_TITLE := "WHAT'S NEW"
# The card's fill is the plate's full strength rather than a lighter 0.52, and the reason is the
# noise below: every square of a panel carries the same per-square offset the plate does, so the
# LIGHTEST a cell can get is `LOG_PLATE_ALPHA - NOISE_ALPHA`. At 0.52 that lightest cell measured
# 3.96:1 against the brightest sky - below the 4.5 the entries need - so the mean moved down by
# exactly one noise step. A cell at the top of the noise is now the 0.52 card that was measured
# at 4.75:1, and the rest are darker; nothing on the panel got lighter, so the AA figure holds
# for the worst cell rather than for an average one.
const LOG_PLATE_ALPHA := 0.58
const LOG_TEXT := Color(0.91, 0.93, 0.96)
const LOG_DIM := Color(0.80, 0.83, 0.87)
# The card's border is the button's own bevel, one texel thick: the tones come out of
# `textures/gui/button.png` rather than being picked by eye - an outline of #262626 and a
# shaded bottom and right of #424242, exactly as the texture has them, so the frame and the
# buttons are the same object at two sizes. `probe_menu.gd` reads those two texels back and
# compares them to these constants.
#
# The lit top and left is the one tone that does NOT survive the move. On a button the fill is
# #6d6d6d, so the texture's #9b9b9b is a +42 step and reads as a bevel; the card's fill is far
# darker, and the same tone read as a bright outline drawn round a nearly black panel - out of
# place in a screen made of quiet greys. It is the button's tone pulled to about 0.67 and
# cooled toward the card: #686e76. Darker still and the bevel stops being legible, which is the
# whole point of having one - the shade beside it is #424242, so there is still a step.
const LOG_EDGE_OUTER := Color8(38, 38, 38)
const LOG_EDGE_LIGHT := Color8(104, 110, 118)
const LOG_EDGE_SHADE := Color8(66, 66, 66)
# A note that replaces the footer for a moment: the honest answer to a row that cannot
# act, in the place the screen already uses for status.
const NOTE_SECONDS := 3.0
# The two pages the column can show. A page, not a second scene: the scrim, the wind, the
# row factory and the focus machinery all belong to this screen, and a second screen would
# be a copy of them that drifts.
const PAGE_TITLE := 0
const PAGE_WORLDS := 1

# The profile card, above the update log in the right-hand column. There is no account system,
# so the name and the state are the one thing this card cannot discover: it says who the save
# belongs to and nothing about a service, and its rows answer in the footer until there is
# something behind them. The head is the player's own skin, so it cannot disagree with the body.
const PROFILE_NAME := "VoxelSteve"
const PROFILE_W := COLUMN
const PROFILE_PAD := 6.0
const PROFILE_HEAD := 20.0
# The head's air: the same number above it, under it (to the rule) and on both sides, and the
# same again as the rows' left inset, so the face and the labels share one left margin.
const PROFILE_AIR := 5.0
# The header is the head's box plus one unit of air above it and one below it (the band ends at
# the rule); the rows are the reference's smaller height. The card's own height is that sum
# rounded UP to whole cells, and what the rounding leaves goes half under the rule and half under
# the last row - so the labels have the same air above and below them.
const PROFILE_HEADER := 30.0
const PROFILE_ROW_H := 11.0
const PROFILE_NAME_UNITS := 8
const PROFILE_SMALL_UNITS := 6
const PROFILE_TEXT := Color(0.94, 0.95, 0.97)
const PROFILE_DIM := Color(0.74, 0.77, 0.82)
const STATUS_ONLINE := Color(0.36, 0.78, 0.42)
const CROWN := Color(0.95, 0.78, 0.32)
# What each row answers while there is nothing behind it. The footer is this screen's status
# line, so a row that cannot act says so there instead of doing nothing at all.
# The friends panel in the bottom-left corner. There is no service, no account and no friends
# list, so this is the panel's SHAPE with nothing in it: the tabs, the search field and Add
# Friend are real controls that do what they say over a list that is empty, and the list says
# why it is empty. When multiplayer exists the rows, the counts and the status arrive with it.
const FRIENDS_TABS := ["Friends", "Requests", "Blocked"]
const FRIENDS_STATUS := "Not connected"
const FRIENDS_EMPTY := "NO FRIENDS YET"
const FRIENDS_REASON := "MULTIPLAYER IS NOT IN THIS BUILD"
const FRIENDS_NO_MATCH := "NO FRIENDS MATCH THAT"
const FRIENDS_EMPTY_TAB := ["NO FRIEND REQUESTS", "NOBODY IS BLOCKED"]
const FRIENDS_HEADER_H := 12.0
const FRIENDS_SEARCH_H := 11.0
const FRIENDS_TABS_H := 10.0
const FRIENDS_LIST_H := 24.0
const FRIENDS_FOOTER_H := 12.0
const FRIENDS_PAD := 4.0
# The three gaps between the bands (search to tabs, tabs to list, list to the footer rule).
const FRIENDS_GAPS := 11.0 + FRIENDS_PAD
const FRIENDS_LABEL_UNITS := 7
const FRIENDS_SMALL_UNITS := 6
const FRIENDS_DOT := Color(0.62, 0.65, 0.70)
const PROFILE_ROWS := [
	["Change Skin", "CHANGE SKIN IS NOT WIRED HERE YET"],
	["Cape", "CAPES ARE NOT IN THIS BUILD YET"],
	["Account Settings", "ACCOUNT SETTINGS ARE NOT WIRED HERE YET"],
]
# The one number a tint depends on: the row texture's own fill, which for the test asset is the
# dark blue-grey (35, 39, 45). A stylebox tint is a MULTIPLY of it, so a target colour is divided
# by this - the hover state comes out at the target itself, which is why this has to be re-read
# from the texture whenever the asset changes.
const BUTTON_FILL := Color(35.0 / 255.0, 39.0 / 255.0, 45.0 / 255.0)
const PRESSED_DARKEN := 0.78

# The sheen: one light travelling round the three right-hand cards' frames. It used to cross the
# ROWS' faces as well, and that was what made the buttons read as busy - a 40-unit face takes the
# whole band, and a row's own colour is a signal (green for play, red for quit) that a wash of
# violet argued with. It spent one build on the wordmark, masked to the letters, and came off
# there too: the title is the one element on this screen that has to read instantly as a word,
# and a band moving through its letters is motion in the middle of it. The cards keep it on their
# frames, which is the part of a card that is an edge.
const SHEEN_SCRIPT: GDScript = preload("res://scripts/ui_sheen.gd")
const SHEEN_CARD_STRENGTH := 0.21
# Fraction of a period: where the cards sit in the light's cycle. They are phase-shifted from each
# other so three frames never light in step.
const SHEEN_CARD_PHASE := [0.31, 0.57, 0.83]

var _wordmark: Label
# The word's border, one black copy of it per direction, drawn under the white word.
var _wordmark_border: Array[Label] = []
var _rows: VBoxContainer
var _footer: Label
var _buttons: Array = []
# One Control over each right-hand card's frame, grown once and re-placed by `_layout`.
var _card_sheen: Array = []
# The grid the scrim is drawn on, and the per-square darkness offset on it.
var _cell := 27.0
var _grid_w := 1
var _grid_h := 1
var _jitter := PackedFloat32Array()
# Squares in the air, by cell index: origin, velocity (px/s), age and the tone they had at
# home. `_healing` is the holes, by the same index, as the time since the square left.
var _lifted := {}
var _healing := {}
var _emit := 0.0
var _motion := true
var _rng := RandomNumberGenerator.new()
var _accent := ACCENT
var _focus_index := 0
var _world_exists := false
# Which page the column is showing, and the rows it is showing: the layer hands the
# worlds in, because it is the side that already knows where a save lives.
var _page := 0
var _worlds: Array = []
var _quit_index := 4
# The update log: the parsed file, the lines it draws, and the card's rect for this window.
var _log_version := ""
var _log_entries: Array = []
var _log_rect := Rect2()
# The footer is the screen's status line: version and seed on the title page, the selected
# world's detail on the list page, and a transient note over either.
var _footer_default := ""
var _note := ""
var _note_timer := 0.0
# The profile card's geometry for this window, and its three rows.
var _profile_rect := Rect2()
var _profile_rows: Array = []
# The friends panel, bottom left: its rect, the three tabs, the search field and Add Friend.
var _friends_rect := Rect2()
var _friends_tabs: Array = []
var _friends_search: LineEdit
var _friends_add: Button
var _friends_tab := 0
# The half of the cell rounding that sits above the chrome, for the panel and the profile card.
var _friends_top_inset := 0.0
# The profile card's resolved geometry - the head's line, the rule under the header - and the
# row the pointer is over. Kept on the node so the drawing and the layout cannot disagree.
var _profile_head_y := 0.0
var _profile_rule_y := 0.0
var _profile_hover := -1
# Layout values that depend on the window, resolved in _layout() and read back by
# _draw(): a small window must not push the column off the bottom of the screen.
var _margin := MARGIN
var _wordmark_y := 54.0
# The rule's own line, resolved in _layout() from the wordmark's measured size.
var _mark_rule_y := 0.0
var _rows_y := FIRST_ROW
# Quit is two presses, not one: a mis-click on the bottom row of the only screen
# standing between the player and the game should not end the session. No modal -
# the row itself changes, and it changes back if the second press never comes.
const QUIT_ARM_SECONDS := 2.5
var _quit_armed := false
var _quit_timer := 0.0


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	# A fixed seed: which squares lift off is random to the eye but the same run to run, so
	# the probe can step the wind and get the same answer every time.
	_rng.seed = 0x5EED
	# No anchors: the parent is a CanvasLayer, not a Control, so a full-rect preset only
	# makes the layout overwrite the size `_fit` sets. The viewport is the authority.

	# The border goes in BEFORE the word it borders, because Godot draws children in order: these
	# are behind the white text by position in the tree, not by a depth setting. They carry no size
	# of their own - _layout() gives each one the offset and the size the word was fitted to, so the
	# border rides whatever the wordmark is, candidate face and all.
	for i in range(WORDMARK_BORDER_DIRS.size()):
		var copy := Label.new()
		copy.text = WORDMARK_TEXT
		copy.add_theme_color_override("font_color", WORDMARK_BORDER_COLOR)
		copy.add_theme_constant_override("outline_size", 0)
		_wordmark_border.append(copy)
		add_child(copy)

	_wordmark = Label.new()
	_wordmark.text = WORDMARK_TEXT
	_wordmark.add_theme_font_override("font", MUNRO_FONT)
	_wordmark.add_theme_color_override("font_color", Color(0.96, 0.97, 0.99))
	# One face, no shadow: an outline of one texel (the default) would feather the letters
	# themselves, and a pixel font drawn soft is a pixel font drawn wrong. The border is copies
	# of the word rather than a shadow of it, which is why `font_shadow_color` stays off.
	_wordmark.add_theme_constant_override("outline_size", 0)
	add_child(_wordmark)

	_rows = VBoxContainer.new()
	_rows.add_theme_constant_override("separation", 12)
	add_child(_rows)

	# Row order is the interaction contract: the primary action first, the two modes, then
	# housekeeping, and Quit last. Not reordered at runtime - only the first row's LABEL
	# changes with the save state - so a returning player's muscle memory holds.
	_build_page()

	_build_profile_rows()
	_build_friends()

	# The three cards' frames, grown once and re-placed by `_layout` from the rects the positioning
	# passes resolve. Children of the screen, so they lie over the fills, the bevels and the text
	# `_draw` puts inside them - and they are not part of the column, because the cards never
	# change identity, only geometry.
	for i in range(SHEEN_CARD_PHASE.size()):
		var card: Control = SHEEN_SCRIPT.new()
		add_child(card)
		_card_sheen.append(card)

	_footer = Label.new()
	_footer.add_theme_font_override("font", MUNRO_FONT)
	_footer.add_theme_color_override("font_color", Color(0.72, 0.75, 0.80))
	add_child(_footer)

	# The screen's size is taken from the viewport and kept on resize: there is no parent
	# Control under a CanvasLayer to derive it from.
	get_viewport().size_changed.connect(_fit)
	_fit()


func _fit() -> void:
	size = get_viewport().get_visible_rect().size
	_layout()


## Rebuild the column for `_page`. The rows are freed and made again rather than hidden,
## because a hidden row still takes part in focus order and in `_buttons` - and the one
## thing this screen cannot get wrong is which rows the player can reach.
func _build_page() -> void:
	for child in _rows.get_children():
		_rows.remove_child(child)
		child.queue_free()
	_buttons.clear()
	_focus_index = 0
	if _page == PAGE_WORLDS:
		if _worlds.is_empty():
			# Nothing to pick. The row says so instead of the list being silently blank.
			_add_row("NO WORLDS YET", _on_back, TINT_MULTI)
		for i in range(_worlds.size()):
			_add_row(String(_worlds[i]["name"]), func(): _on_world(i), TINT_PLAY)
		_add_row("BACK", _on_back, TINT_SETTINGS)
		_quit_index = -1
	else:
		_add_row(ROW_TEXT_RESUME if _world_exists else ROW_TEXT_PRIMARY, _on_start, TINT_PLAY)
		_add_row("SINGLEPLAYER", _on_singleplayer, TINT_SINGLE)
		_add_row("MULTIPLAYER", _on_multiplayer, TINT_MULTI)
		_add_row("SETTINGS", _on_settings, TINT_SETTINGS)
		_quit_index = _buttons.size()
		_add_row("QUIT", _on_quit, TINT_QUIT)


## The profile card's three rows, as real Buttons: hover, focus, click and press all come from
## the engine, which is the same reason the menu column uses Buttons. They are FLAT - a
## transient tint of their own rather than `button.png` - because they sit inside a panel and a
## grey button on a dark card reads as a second, louder interface. They are NOT in `_buttons`:
## that array is the action column, and its index drives the accent bar's position.
func _build_profile_rows() -> void:
	for i in range(PROFILE_ROWS.size()):
		var btn := _flat_button(String(PROFILE_ROWS[i][0]))
		btn.pressed.connect(_on_profile_row.bind(i))
		btn.mouse_entered.connect(_on_profile_hover.bind(i, true))
		btn.mouse_exited.connect(_on_profile_hover.bind(i, false))
		add_child(btn)
		_profile_rows.append(btn)


## Which row the pointer is on, for the chevron `_draw_profile` adds to it. The exit is filtered
## by the row's own index because leaving row A can arrive after entering row B, and an
## unfiltered exit would take the arrow off the row the pointer is actually on.
func _on_profile_hover(index: int, entered: bool) -> void:
	var wanted := index if entered else (-1 if _profile_hover == index else _profile_hover)
	if wanted == _profile_hover:
		return
	_profile_hover = wanted
	queue_redraw()


## The friends panel's controls: a search field, three tabs and Add Friend. All flat, all the
## same treatment as the profile rows - they live inside a panel, not on it.
func _build_friends() -> void:
	_friends_search = LineEdit.new()
	_friends_search.placeholder_text = "Search friends..."
	_friends_search.max_length = 32
	_friends_search.add_theme_font_override("font", MUNRO_FONT)
	_friends_search.add_theme_color_override("font_color", PROFILE_TEXT)
	_friends_search.add_theme_color_override("font_placeholder_color", PROFILE_DIM)
	_friends_search.add_theme_color_override("caret_color", _accent)
	var field := StyleBoxFlat.new()
	field.bg_color = Color(0.0, 0.0, 0.0, 0.25)
	field.border_color = LOG_EDGE_SHADE
	field.border_width_bottom = 1
	field.border_width_top = 1
	field.border_width_left = 1
	field.border_width_right = 1
	field.content_margin_left = 4.0
	field.content_margin_right = 4.0
	_friends_search.add_theme_stylebox_override("normal", field)
	var focused := field.duplicate() as StyleBoxFlat
	focused.border_color = _accent
	_friends_search.add_theme_stylebox_override("focus", focused)
	# The field filters the list. With no friends the result is the same either way, but the field
	# is wired rather than decorative: the empty state changes to say nothing matched.
	_friends_search.text_changed.connect(func(_text: String) -> void: queue_redraw())
	add_child(_friends_search)

	for i in range(FRIENDS_TABS.size()):
		var tab := _flat_button(String(FRIENDS_TABS[i]))
		tab.pressed.connect(_on_friends_tab.bind(i))
		add_child(tab)
		_friends_tabs.append(tab)

	_friends_add = _flat_button("+  Add Friend")
	_friends_add.pressed.connect(func() -> void:
		_flash("FRIENDS NEED MULTIPLAYER, WHICH IS NOT IN THIS BUILD"))
	add_child(_friends_add)


## One flat row button, the shape both panels use: no texture, a transient tint, the engine's
## own hover/focus/press and an accent bar on the focused one.
func _flat_button(text: String) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	btn.add_theme_font_override("font", MUNRO_FONT)
	# The row shows where you are with its TEXT, not with a box: a filled highlight rectangle
	# inside a panel is the panel drawing a second, lighter panel around the pointer. Rest is
	# recessed, the row you are on is white, and it presses to the accent.
	btn.add_theme_color_override("font_color", PROFILE_DIM)
	btn.add_theme_color_override("font_hover_color", PROFILE_TEXT)
	btn.add_theme_color_override("font_focus_color", PROFILE_TEXT)
	btn.add_theme_color_override("font_pressed_color", PROFILE_TEXT)
	var normal := StyleBoxFlat.new()
	normal.bg_color = Color(0.0, 0.0, 0.0, 0.0)
	var hover := StyleBoxFlat.new()
	hover.bg_color = Color(0.0, 0.0, 0.0, 0.0)
	var pressed := StyleBoxFlat.new()
	pressed.bg_color = Color(0.0, 0.0, 0.0, 0.0)
	var focus := StyleBoxFlat.new()
	focus.bg_color = Color(0.0, 0.0, 0.0, 0.0)
	focus.border_width_left = 2
	# The accent bar on the left edge, the same marker the column uses for the row you are on.
	focus.border_width_left = 2
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", focus)
	return btn


## Every flat row's metrics take the window's scale, and its focus bar takes the accent - which
## is mixed with the world's sky warmth in `open`, so it cannot be known when the rows are built.
func _style_flat(btn: Button, s: float, left_inset: float) -> void:
	btn.add_theme_font_size_override("font_size", int(FRIENDS_SMALL_UNITS * s))
	for state in ["normal", "hover", "pressed", "focus"]:
		var box := btn.get_theme_stylebox(state).duplicate() as StyleBoxFlat
		box.content_margin_left = left_inset * s
		box.content_margin_right = 2.0 * s
		if state == "focus":
			# The bar alone marks the row you are on; there is no fill behind it.
			box.border_color = _accent
		btn.add_theme_stylebox_override(state, box)
	# Pressed is the accent for the moment the finger is down: feedback without a shape change,
	# and it cannot be known at build time because the accent is mixed with the sky's warmth.
	btn.add_theme_color_override("font_pressed_color", _accent)


func _on_friends_tab(index: int) -> void:
	if index == _friends_tab:
		return
	_friends_tab = index
	queue_redraw()


## A message in the footer, for three seconds. Every row on this screen that cannot act yet
## answers here, so there is one place to look for "why did nothing happen".
func _flash(text: String) -> void:
	_note = text
	_note_timer = NOTE_SECONDS
	_refresh_footer()


## A profile row with nothing behind it answers in the footer, the place this screen already
## uses for status - the same answer Multiplayer gives.
func _on_profile_row(index: int) -> void:
	if index < 0 or index >= PROFILE_ROWS.size():
		return
	_note = String(PROFILE_ROWS[index][1])
	_note_timer = NOTE_SECONDS
	_refresh_footer()


## Move between the two pages: the column is rebuilt, the layout re-taken (a new set of
## rows changes nothing about the window, but `_layout` also re-measures the log card) and
## the footer re-read, because on the list page it is the selected world's detail.
func _set_page(page: int) -> void:
	_page = page
	_build_page()
	_layout()
	_refresh_footer()
	refocus()


## The footer describes what the screen is showing: the launch's own save on the title
## page, and the world under the focus on the list. A note, when there is one, wins.
func _refresh_footer() -> void:
	if _footer == null:
		return
	if _note != "":
		_footer.text = _note
		return
	if _page == PAGE_WORLDS and not _worlds.is_empty():
		var index := clampi(_focus_index, 0, _worlds.size() - 1)
		_footer.text = _world_detail(_worlds[index])
		return
	_footer.text = _footer_default


func _world_detail(world: Dictionary) -> String:
	var seed_text := "seed %d" % int(world["seed"]) if world["seed"] != null else "new world"
	var when := int(world.get("when", 0))
	if when <= 0:
		return seed_text
	return "%s  ·  %s" % [seed_text, _format_age(when)]


## How long ago a world was last written, in the one unit that matters on a menu. Coarse
## on purpose: a player reads "2 h ago", never "1 h 58 m".
func _format_age(modified_unix: int) -> String:
	var ago := maxi(0, int(Time.get_unix_time_from_system()) - modified_unix)
	if ago < 90:
		return "just saved"
	if ago < 3600:
		return "%d m ago" % int(ago / 60.0)
	if ago < 86400:
		return "%d h ago" % int(ago / 3600.0)
	return "%d d ago" % int(ago / 86400.0)


## The update log, read once per open. Missing or broken falls back to an empty log: a
## title screen that cannot draw its changelog still has to draw its buttons.
func _load_log() -> void:
	_log_version = ""
	_log_entries.clear()
	var file := FileAccess.open(LOG_PATH, FileAccess.READ)
	if file == null:
		push_error("Failed to load " + LOG_PATH)
		return
	var text := file.get_as_text()
	file.close()
	var json := JSON.new()
	if json.parse(text) != OK:
		push_error("Failed to parse %s: %s" % [LOG_PATH, json.get_error_message()])
		return
	var data = json.data
	if typeof(data) != TYPE_DICTIONARY:
		return
	_log_version = String(data.get("version", ""))
	for entry in data.get("entries", []):
		_log_entries.append(String(entry))


## Show the screen for a given world. `world_exists` is whether a save was there
## before this launch: false turns the primary action from CONTINUE into PLAY.
## `live_seed` is the seed the running world is using, shown in the footer whether
## or not that world was already saved - on a first run it is the seed about to be
## played, which is the one fact worth knowing before pressing Play. `worlds` is the
## list SINGLEPLAYER opens, handed in by the layer because it is the side that knows
## where a save lives.
func open(world_exists: bool, live_seed, version_text: String, accent: Color,
		motion: bool = true, worlds: Array = []) -> void:
	_accent = ACCENT.lerp(accent, 0.35) if accent != null else ACCENT
	# Reduce Motion comes from the settings menu through `menu_layer.gd`, like the
	# transition's: the plate keeps its noise and stops losing squares, which is the switch.
	_motion = motion
	_lifted.clear()
	_healing.clear()
	_emit = 0.0
	_world_exists = world_exists
	_worlds = worlds
	_page = PAGE_TITLE
	var seed_text := "seed %d" % int(live_seed) if live_seed != null else "new world"
	_footer_default = "v%s  ·  %s" % [version_text, seed_text]
	_note = ""
	_note_timer = 0.0
	_load_log()
	_disarm_quit()
	_build_page()
	_refresh_footer()
	# Re-taken every open: a window resized while the title was behind another
	# screen fires its signal at a Control that is not listening, and the layout
	# would still be the old viewport's.
	_fit()
	show()
	refocus()


## Focus the primary action. Called on open and whenever this returns from the
## settings menu: a screen with nothing focused leaves a pad player pressing
## directions to find out where they are.
func refocus() -> void:
	if _buttons.is_empty():
		return
	_buttons[0].grab_focus()


func has_world() -> bool:
	return _world_exists


func _add_row(text: String, handler: Callable, tint: Color) -> void:
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	btn.add_theme_font_override("font", MUNRO_FONT)
	btn.add_theme_color_override("font_color", Color.WHITE)
	btn.add_theme_color_override("font_focus_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color.WHITE)
	# The house fill, exactly as the death screen and the settings menu use it: the texture at
	# its own grey, not darkened - and then the row's own colour while it is the current one.
	# A darker resting row was tried to sit the list into the plate and it read as a dark-mode
	# menu dropped into a game whose buttons are all this one, so the resting state stays the
	# house grey and only the state you are on is coloured.
	#
	# The focus stylebox is EMPTY on purpose: Godot draws it over the state's own box, so a
	# filled one hides the press and the row would look identical down and held. Selection is
	# the accent bar in `_draw` instead, which also leaves the tint free to be the state's.
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	var tinted := _tint(tint)
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = tinted
	var focused := StyleBoxEmpty.new()
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(tinted.r * PRESSED_DARKEN, tinted.g * PRESSED_DARKEN,
		tinted.b * PRESSED_DARKEN)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", focused)
	btn.add_theme_stylebox_override("disabled", normal)
	btn.pressed.connect(handler)
	var index := _buttons.size()
	btn.focus_entered.connect(func():
		_focus_index = index
		# The footer describes the row under the focus on the world list, so it is re-read
		# here and not only when the page changes.
		_refresh_footer()
		queue_redraw())
	# Hover moves the focus rather than sitting beside it: two highlighted rows at
	# once is the state a menu must never be in, and a mouse player who then
	# reaches for Enter should continue from the row they were pointing at.
	btn.mouse_entered.connect(func():
		if not btn.has_focus():
			btn.grab_focus())
	_rows.add_child(btn)
	_buttons.append(btn)


## The modulate that makes the grey texture read as `target`: the fill it multiplies is
## `BUTTON_FILL`, so the tint is that target over the grey.
func _tint(target: Color) -> Color:
	return Color(target.r / BUTTON_FILL.r, target.g / BUTTON_FILL.g, target.b / BUTTON_FILL.b)


func _set_row_text(index: int, text: String) -> void:
	if index >= 0 and index < _buttons.size():
		_buttons[index].text = text


## The font size, in units, at which the wordmark is exactly as wide as the column. Measured
## rather than tabled, because a font's advance widths are not linear enough in the size to trust
## one multiplication: it scales from the reference size and corrects once against what that
## produced, which lands on the column's edge within a pixel.
func _wordmark_size(s: float) -> float:
	var target := COLUMN * s
	var px := WORDMARK_REF * s
	# The font the label actually draws with, not the house constant: measuring one face and
	# drawing another is how a candidate font ends up overflowing the column it was fitted to.
	var face: Font = _wordmark.get_theme_font("font")
	for _pass in 2:
		var w := face.get_string_size(_wordmark.text, HORIZONTAL_ALIGNMENT_LEFT, -1,
			int(round(px))).x
		if w <= 0.0:
			break
		px *= target / w
	return px / s


func _layout() -> void:
	var s := maxf(UIScale.value, 1.0)
	var height_units := size.y / s
	# The screen's own height decides the margin and the first row, so the same
	# design survives a 720p window: a fixed row of units designed for 1080p
	# would run the Quit row into the footer on anything shorter.
	_margin = minf(MARGIN, height_units * 0.08)
	_wordmark_y = minf(54.0, height_units * 0.10)
	# The title is the column's width, and the list follows its rule down: the air between the rule
	# and the first row is the one relationship the two keep, so a bigger wordmark lengthens the
	# block instead of landing on CONTINUE. The row is rounded up to a whole unit so the labels
	# stay on the pixel grid at any GUI scale.
	var mark := _wordmark_size(s)
	var mark_px := int(round(mark * s))
	var at := Vector2(_margin, _wordmark_y) * s
	_wordmark.position = at
	_wordmark.add_theme_font_size_override("font_size", mark_px)
	# The border: one copy per direction, one border out from the word, at the word's own size and
	# face - the face read back off the LABEL rather than the house constant, so a candidate font
	# borders itself and measuring one face while drawing another cannot happen here either. The
	# offsets are rounded to whole device pixels, which is what keeps a pixel face's border a hard
	# edge rather than a soft one.
	var border := WORDMARK_BORDER * s
	for i in range(_wordmark_border.size()):
		var copy: Label = _wordmark_border[i]
		var dir: Vector2 = WORDMARK_BORDER_DIRS[i]
		copy.add_theme_font_override("font", _wordmark.get_theme_font("font"))
		copy.add_theme_font_size_override("font_size", mark_px)
		copy.position = at + Vector2(roundf(dir.x * border), roundf(dir.y * border))
	# Under the BASELINE, which is where the letters actually end: a multiple of the font size puts
	# the rule wherever the face's own proportions land, so a face with deep descent left a band of
	# nothing between the word and its own underline. The ascent is read off the label's face, like
	# the width is, for the same reason.
	var face: Font = _wordmark.get_theme_font("font")
	_mark_rule_y = _wordmark_y + float(face.get_ascent(mark_px)) / s + WORDMARK_RULE_AIR
	_rows_y = ceilf(maxf(minf(FIRST_ROW, height_units * 0.34), _mark_rule_y + WORDMARK_AIR))
	_rows.position = Vector2(_margin, _rows_y) * s
	_rows.add_theme_constant_override("separation", int(ROW_GAP * s))
	for btn in _buttons:
		btn.custom_minimum_size = Vector2(COLUMN, ROW_H) * s
		btn.add_theme_font_size_override("font_size", int(LABEL_UNITS * s))
		# Only the text inset is rewritten here, so the per-state tints above survive a
		# re-layout. Focus is not in this list: it is an empty box (it is drawn over the
		# state's own, see `_add_row`), so it has no texture to inset.
		for state in ["normal", "hover", "pressed", "disabled"]:
			var box := btn.get_theme_stylebox(state).duplicate() as StyleBoxTexture
			box.content_margin_left = 6.0 * s
			box.content_margin_right = 6.0 * s
			btn.add_theme_stylebox_override(state, box)
	_footer.position = Vector2(_margin, (height_units - _margin - FOOTER_UNITS * 1.7) * s)
	_footer.add_theme_font_size_override("font_size", int(FOOTER_UNITS * s))
	_build_grid()
	# The right-hand stack, top to bottom: each panel's top depends on where the one above it
	# ended, so the order of these three calls is the layout.
	_position_profile()
	_position_friends()
	_position_log()
	_place_card_sheen()
	queue_redraw()


## The profile card's rect and its rows for this window. Same three rules as the log: the
## column's width rounded to whole cells, the right edge on the margin snapped to a cell, the
## top on the wordmark's line - and the rows are placed inside it, under the header rule.
func _position_profile() -> void:
	_profile_rect = Rect2()
	var s := maxf(UIScale.value, 1.0)
	var width_units := size.x / s
	var room := width_units - (_margin + COLUMN) - 24.0 - _margin
	var w_units := minf(PROFILE_W, room)
	var fits := w_units >= PROFILE_W * 0.6
	if not fits:
		for btn in _profile_rows:
			btn.visible = false
		return
	var h_units := PROFILE_HEADER + float(_profile_rows.size()) * PROFILE_ROW_H + PROFILE_PAD
	var cell := maxf(_cell, 2.0)
	var cols := int(round(w_units * s / cell))
	var rows := int(ceil(h_units * s / cell))
	var w := float(cols) * cell
	var h := float(rows) * cell
	var right := roundf((size.x - _margin * s) / cell) * cell
	var left := maxf(_margin * s, right - w)
	var top := roundf(_wordmark_y * s / cell) * cell
	_profile_rect = Rect2(Vector2(left, top), Vector2(w, h))

	# The head's air is ONE number on all four sides, and it is the same number as the rows' left
	# inset - so the face is not closer to the side than to the top, and its left edge is the
	# labels' left edge. Three gaps of equal size were the previous answer and they were still
	# wrong: the rule sitting half a gap under the head made the header read low. The band now
	# ends one `air` under the head, and whatever the cell rounding leaves below the rule is split
	# evenly between the first row and the bottom, so the labels have equal air above and below.
	var px := maxf(1.0, roundf(s))
	var air := px + PROFILE_AIR * s
	var head_h := PROFILE_HEAD * s
	var rows_h := float(_profile_rows.size()) * PROFILE_ROW_H * s
	_profile_head_y = top + air
	_profile_rule_y = _profile_head_y + head_h + air
	var spare := maxf(0.0, h - (_profile_rule_y - top) - rows_h)
	var row_w := w - air * 2.0
	var row_y := _profile_rule_y + spare * 0.5
	for i in range(_profile_rows.size()):
		var btn: Button = _profile_rows[i]
		btn.visible = true
		btn.position = Vector2(left + air, row_y + float(i) * PROFILE_ROW_H * s)
		btn.size = Vector2(row_w, PROFILE_ROW_H * s)
		# The label is indented past a chevron's width even at rest, so the arrow that appears on
		# hover has a gutter of its own and the row does not move when the pointer arrives.
		_style_flat(btn, s, 6.0)


## The friends panel is the MIDDLE panel of the right-hand column: under the profile card, above
## the update log, the same width as both. It is also the panel that STRETCHES: the stack fills
## the column, its last card's bottom edge as far from the bottom of the screen as the profile
## card's top is from the top, and the list is where that room goes. The chrome - header, search,
## tabs, Add Friend - stays where it is, pinned to the top and bottom of the taller box.
func _position_friends() -> void:
	_friends_rect = Rect2()
	var s := maxf(UIScale.value, 1.0)
	var width_units := size.x / s
	var room := width_units - (_margin + COLUMN) - 24.0 - _margin
	var w_units := minf(COLUMN, room)
	if w_units < COLUMN * 0.6 or _profile_rect.size.x <= 0.0:
		_hide_friends()
		return
	var cell := maxf(_cell, 2.0)
	var content_h := FRIENDS_HEADER_H + FRIENDS_SEARCH_H + FRIENDS_TABS_H \
		+ FRIENDS_LIST_H + FRIENDS_FOOTER_H + FRIENDS_GAPS
	var cols := int(round(w_units * s / cell))
	var natural := int(ceil(content_h * s / cell))
	var rows := natural
	# The stack FILLS the column: the log card's bottom edge sits exactly as far from the bottom
	# of the screen as the profile card's top does from the top. The friends list is the panel
	# that stretches for it - a list is what has somewhere for the room to go, and its chrome
	# stays at the top with Add Friend pinned to the bottom. The log keeps its content's height,
	# because a changelog card with a foot of empty card under its last line reads as broken.
	var log_rows := _log_cell_rows()
	if log_rows > 0:
		var want := size.y - _profile_rect.position.y * 2.0 - _profile_rect.size.y \
			- cell * 2.0 - float(log_rows) * cell
		rows = maxi(rows, int(round(want / cell)))
	var w := float(cols) * cell
	var h := float(rows) * cell
	var right := roundf((size.x - _margin * s) / cell) * cell
	var left := maxf(_margin * s, right - w)
	var top := roundf((_profile_rect.end.y + cell) / cell) * cell
	_friends_rect = Rect2(Vector2(left, top), Vector2(w, h))
	# The same half-and-half of the cell rounding the profile card gets, so the chrome sits in the
	# middle of its box rather than against the top of it - measured against the CONTENT's height,
	# not the stretched panel's, or a taller list would lift the header into the middle of the box.
	_friends_top_inset = (float(natural) * cell - content_h * s) * 0.5

	var boxes := _friends_boxes()
	_friends_search.visible = true
	var search_box: Rect2 = boxes["search"]
	_friends_search.position = search_box.position
	_friends_search.size = search_box.size
	_friends_search.add_theme_font_size_override("font_size", int(FRIENDS_SMALL_UNITS * s))
	var tabs_box: Rect2 = boxes["tabs"]
	var tab_w := tabs_box.size.x / float(FRIENDS_TABS.size())
	for i in range(_friends_tabs.size()):
		var tab: Button = _friends_tabs[i]
		tab.visible = true
		tab.position = Vector2(tabs_box.position.x + tab_w * float(i), tabs_box.position.y)
		tab.size = Vector2(tab_w, tabs_box.size.y)
		tab.add_theme_constant_override("h_separation", 0)
		# Centred, because the three tabs are segments of one box of equal width: a label pinned
		# to the left of an even box reads as text that fell into it, not as a tab.
		tab.alignment = HORIZONTAL_ALIGNMENT_CENTER
		_style_flat(tab, s, 2.0)
		# The active tab is the bright label; the others are dim, so the tab you are on is
		# legible without a second marker beside the underline `_draw_friends` paints.
		var on := i == _friends_tab
		for key in ["font_color", "font_hover_color", "font_focus_color", "font_pressed_color"]:
			tab.add_theme_color_override(key, PROFILE_TEXT if on else PROFILE_DIM)
	_friends_add.visible = true
	var add_box: Rect2 = boxes["add"]
	_friends_add.position = add_box.position
	_friends_add.size = add_box.size
	_style_flat(_friends_add, s, 4.0)


## The panel's interior, computed once so the layout and the drawing cannot drift: the header
## band at the top, then search, tabs, the list, and Add Friend at the bottom.
func _friends_boxes() -> Dictionary:
	var s := maxf(UIScale.value, 1.0)
	var px := maxf(1.0, roundf(s))
	var pad := px + FRIENDS_PAD * 0.5 * s
	var x := _friends_rect.position.x + pad
	var w := _friends_rect.size.x - pad * 2.0
	var y := _friends_rect.position.y + _friends_top_inset + FRIENDS_PAD * 0.5 * s
	var search := Rect2(Vector2(x, y + FRIENDS_HEADER_H * s), Vector2(w, FRIENDS_SEARCH_H * s))
	var tabs := Rect2(Vector2(x, search.end.y + 3.0 * s), Vector2(w, FRIENDS_TABS_H * s))
	var add := Rect2(Vector2(x, _friends_rect.end.y - _friends_top_inset
		- FRIENDS_PAD * 0.5 * s - FRIENDS_FOOTER_H * s), Vector2(w, FRIENDS_FOOTER_H * s))
	var list := Rect2(Vector2(x, tabs.end.y + 3.0 * s),
		Vector2(w, maxf(0.0, add.position.y - 5.0 * s - (tabs.end.y + 3.0 * s))))
	return {"search": search, "tabs": tabs, "add": add, "list": list, "pad": pad,
		"header_y": _friends_rect.position.y + _friends_top_inset,
		"header_h": FRIENDS_HEADER_H * s}


func _hide_friends() -> void:
	if _friends_search != null:
		_friends_search.visible = false
	for tab in _friends_tabs:
		tab.visible = false
	if _friends_add != null:
		_friends_add.visible = false


## The log card's rect for this window, snapped to whole scrim cells so the card is made of
## the same squares as the plate rather than a rectangle laid over a grid.
##
## It is the BOTTOM of the right-hand rail: one cell under the friends panel, its right edge on
## the same margin the column's left edge uses, and its bottom edge closing the stack - as far
## from the bottom of the screen as the profile card's top is from the top. It keeps its
## content's height rather than stretching: a changelog with a hand's width of empty card under
## its last line reads as a card that failed to fill.
##
## The card is dropped rather than shrunk below the point where a line still fits: a changelog
## in a column narrower than its own words is worse than no changelog.
##
## Its height is its content's, in whole cells. `_position_friends` reads this number before the
## card is placed, because the stack's bottom edge is what sizes the friends panel.
func _log_cell_rows() -> int:
	if _log_entries.is_empty():
		return 0
	var s := maxf(UIScale.value, 1.0)
	var h_units := LOG_PAD + (LOG_HEADING_UNITS + 9.0) + (LOG_BODY_UNITS + 8.0) \
		+ float(_log_entries.size()) * LOG_LINE_H + LOG_PAD * 0.6
	return int(ceil(h_units * s / maxf(_cell, 2.0)))


func _position_log() -> void:
	_log_rect = Rect2()
	if _log_entries.is_empty():
		return
	var s := maxf(UIScale.value, 1.0)
	var width_units := size.x / s
	# One column plus the card plus a gap between them, or the card is not drawn: this is
	# the same "shrink with the window" rule the margin and the first row follow.
	var room := width_units - (_margin + COLUMN) - 24.0 - _margin
	var w_units := minf(LOG_W, room)
	if w_units < LOG_W * 0.6:
		return
	var cell := maxf(_cell, 2.0)
	var natural := _log_cell_rows()
	# Rounded, not filled: a card that took the next whole cell would sit a cell past the
	# buttons' right edge, and the point of the width is that it is the column's.
	var cols := int(round(w_units * s / cell))
	# At most down to the footer's line: on a short window the card stops growing before it
	# reaches the bottom of the screen rather than running off it.
	var floor_y := size.y - _margin * s
	# Directly under the profile card, one cell of air between them - the right-hand column is
	# one stack, profile then log, the way the left one is wordmark then rows.
	var anchor := _friends_rect if _friends_rect.size.x > 0.0 else _profile_rect
	var stack_top := anchor.end.y + cell if anchor.size.x > 0.0 else _wordmark_y * s
	var top := roundf(stack_top / cell) * cell
	var rows := mini(natural, int(floor((floor_y - top) / cell)))
	var w := float(cols) * cell
	var h := float(rows) * cell
	# Both edges land on cell boundaries, so the panel never cuts a square behind it in half.
	# The right edge is the margin snapped to the nearest cell: up to half a cell of movement,
	# invisible against the margin and the price of the grid.
	var right := roundf((size.x - _margin * s) / cell) * cell
	var left := maxf(_margin * s, right - w)
	_log_rect = Rect2(Vector2(left, top), Vector2(w, h))


## The same light on the three cards, confined to their frames. A card's inside is a surface with
## text on it, and additive light over text is a smudge - but its edge is where a reflection would
## catch, so that is what is lit. The ring is two texels because that is what `_draw_panel` draws a
## frame with: one of outline and one of lit or shaded edge. A card with no rect this window (one
## dropped for room, the log before its entries load) has its light put out rather than left at a
## stale size.
func _place_card_sheen() -> void:
	var s := maxf(UIScale.value, 1.0)
	var ring := 2.0 * maxf(1.0, roundf(s))
	var rects := [_profile_rect, _friends_rect, _log_rect]
	for i in range(_card_sheen.size()):
		var sheen: Control = _card_sheen[i]
		var rect: Rect2 = rects[i]
		if rect.size.x <= 0.0 or rect.size.y <= 0.0:
			sheen.set_moving(false)
			continue
		sheen.position = rect.position
		sheen.size = rect.size
		sheen.configure(SHEEN_CARD_STRENGTH, float(SHEEN_CARD_PHASE[i]), ring, 0.0, s)
		sheen.set_moving(_motion)


func _draw() -> void:
	var s := UIScale.value
	_draw_scrim()
	# The wordmark's rule: the full column, in steel. Full width because the wordmark is - its
	# right edge is the buttons' right edge, and a rule stopping at 42% of that measured against
	# nothing and left the title looking unfinished at the end the eye reads last. The four bands
	# are stacked, not smoothed: a pixel rule that fades is a soft line, and every other edge on
	# this screen is hard.
	# On the device grid, like the wordmark's border: neither the margin nor the rule's drop is a
	# whole pixel once the GUI scale has been through them (51.8 and 208.4 at 1080p), and a beam
	# drawn from a fractional origin rasterises its two diagonals differently - the lower one covers
	# a column the upper one misses, so one end of the point reads longer than the other. The shape
	# is symmetric; the pixel grid it landed on was not.
	var rule_x := roundf(_margin * s)
	var rule_y := roundf(_mark_rule_y * s)
	var rule_w := COLUMN * s
	var band := RULE_BAND * s
	var half := band * 2.0
	var tip := RULE_TIP * s
	var tall := half * 2.0
	var tones: Array[Color] = [RULE_HI, RULE_FACE, RULE_LOW, RULE_SHADE]
	# The rim: the same beam OFFSET perpendicular by half a unit, one polygon. Offsetting rather
	# than insetting is the whole of it. At this angle the taper is nearly horizontal, so a rim
	# drawn a pixel in from each row's edge is one dot per row - a dotted line, which is what "the
	# outline is messed up on the point" was - while a rim one pixel away ACROSS the edge runs four
	# or five pixels in every row and reads as a line, because that is how any line this shallow
	# rasterises. The tip is dragged out by `edge` and no further: a true miter at 28 degrees is a
	# needle, and a clipped one is the fat black nose that read as a thick point.
	var edge := roundf(RULE_EDGE * s)
	if edge > 0.0:
		var diagonal := sqrt(tip * tip + half * half)
		var out_x := edge * half / diagonal
		var out_y := edge * tip / diagonal
		draw_colored_polygon(PackedVector2Array([
			Vector2(rule_x - edge, rule_y + half),
			Vector2(rule_x + tip - out_x, rule_y - out_y),
			Vector2(rule_x + rule_w - tip + out_x, rule_y - out_y),
			Vector2(rule_x + rule_w + edge, rule_y + half),
			Vector2(rule_x + rule_w - tip + out_x, rule_y + tall + out_y),
			Vector2(rule_x + tip - out_x, rule_y + tall + out_y)]), RULE_EDGE_COLOR)
	# The beam itself: four bands, each a trapezoid, so the taper is one straight edge across the
	# whole bar. The insets are deliberately NOT rounded - a row's own inset rounded to the pixel is
	# what turned the end into a staircase in an earlier build, and the staircase is also what the
	# rim cannot be parallel to.
	for i in range(tones.size()):
		var top := float(i) * band
		var low := top + band
		var it := tip * absf(top - half) / half
		var ib := tip * absf(low - half) / half
		draw_colored_polygon(PackedVector2Array([
			Vector2(rule_x + it, rule_y + top), Vector2(rule_x + rule_w - it, rule_y + top),
			Vector2(rule_x + rule_w - ib, rule_y + low), Vector2(rule_x + ib, rule_y + low)]),
			tones[i])
	# The accent: one bar on the focused row.
	if _focus_index >= 0 and _focus_index < _buttons.size():
		var row_y := _rows_y + float(_focus_index) * (ROW_H + ROW_GAP)
		draw_rect(Rect2((_margin - FOCUS_BAR_INSET) * s, row_y * s + ROW_H * 0.10 * s,
			FOCUS_BAR_W * s, ROW_H * 0.80 * s), _accent)
	_draw_profile()
	_draw_log()
	_draw_friends()


## One panel: the fill, then the button texture's bevel outside-in - one texel of outline
## all the way round, then one texel of light on the top and left and of shade on the bottom
## and right. At GUI scale 2 a texel is 2 px, so the frame thickens with the rest of the
## interface instead of staying a hairline. The profile card and the update log are both this.
##
## The fill is CELLS, not one rect: the three right-hand panels are made of the same squares as
## the plate behind them and carry the same per-square darkness, so a panel reads as a piece of
## the same material rather than a sheet laid over a grid. Every panel's rect is snapped to the
## cell grid, so its cells ARE the plate's cells - and the noise is keyed on the cell's place in
## the SCREEN grid, so a square that the plate has and a panel covers carries one tone in both.
func _draw_panel(rect: Rect2) -> void:
	var s := maxf(UIScale.value, 1.0)
	var cell := maxf(_cell, 2.0)
	var tint := SCRIM_TINT
	var cols := maxi(1, int(ceil(rect.size.x / cell)))
	var rows := maxi(1, int(ceil(rect.size.y / cell)))
	var grid_x := int(round(rect.position.x / cell))
	var grid_y := int(round(rect.position.y / cell))
	for row in range(rows):
		var y := rect.position.y + float(row) * cell
		var h := minf(cell, rect.end.y - y)
		if h <= 0.0:
			continue
		for col in range(cols):
			var x := rect.position.x + float(col) * cell
			var w := minf(cell, rect.end.x - x)
			if w <= 0.0:
				continue
			tint.a = _panel_alpha(grid_x + col, grid_y + row)
			draw_rect(Rect2(Vector2(x, y), Vector2(w, h)), tint)
	var px := maxf(1.0, roundf(s))
	var inner := Rect2(rect.position + Vector2(px, px), rect.size - Vector2(px, px) * 2.0)
	draw_rect(Rect2(rect.position, Vector2(rect.size.x, px)), LOG_EDGE_OUTER)
	draw_rect(Rect2(Vector2(rect.position.x, rect.end.y - px), Vector2(rect.size.x, px)),
		LOG_EDGE_OUTER)
	draw_rect(Rect2(rect.position, Vector2(px, rect.size.y)), LOG_EDGE_OUTER)
	draw_rect(Rect2(Vector2(rect.end.x - px, rect.position.y), Vector2(px, rect.size.y)),
		LOG_EDGE_OUTER)
	draw_rect(Rect2(inner.position, Vector2(inner.size.x, px)), LOG_EDGE_LIGHT)
	draw_rect(Rect2(inner.position, Vector2(px, inner.size.y)), LOG_EDGE_LIGHT)
	draw_rect(Rect2(Vector2(inner.position.x, inner.end.y - px), Vector2(inner.size.x, px)),
		LOG_EDGE_SHADE)
	draw_rect(Rect2(Vector2(inner.end.x - px, inner.position.y), Vector2(px, inner.size.y)),
		LOG_EDGE_SHADE)


## The profile card: the player's own face, their name, one state, and three rows that the
## rows' own Buttons draw the interaction for. Everything here is the head's side - the name,
## the crown beside it, the dot and the word under it, and the chevron on the right.
func _draw_profile() -> void:
	if _profile_rect.size.x <= 0.0:
		return
	_draw_panel(_profile_rect)
	var s := maxf(UIScale.value, 1.0)
	var px := maxf(1.0, roundf(s))
	var inset := px + PROFILE_AIR * s
	var head := Rect2(Vector2(_profile_rect.position.x + inset, _profile_head_y),
		Vector2(PROFILE_HEAD, PROFILE_HEAD) * s)
	draw_texture_rect_region(SKIN_TEX, head, SKIN_HEAD_REGION)
	var name_size := int(PROFILE_NAME_UNITS * s)
	var small := int(PROFILE_SMALL_UNITS * s)
	var text_x := head.end.x + 5.0 * s
	var name_y := head.position.y + float(MUNRO_FONT.get_ascent(name_size))
	draw_string(MUNRO_FONT, Vector2(text_x, name_y), PROFILE_NAME, HORIZONTAL_ALIGNMENT_LEFT,
		-1, name_size, PROFILE_TEXT)
	var name_w := MUNRO_FONT.get_string_size(PROFILE_NAME, HORIZONTAL_ALIGNMENT_LEFT, -1,
		name_size).x
	_draw_crown(Vector2(text_x + name_w + 4.0 * s, name_y - 5.0 * s), s)
	# The dot and the word share a baseline at the bottom of the head, so the pair reads as one
	# line rather than a bullet floating beside a label.
	var dot := 3.0 * s
	var status_y := head.end.y - float(MUNRO_FONT.get_descent(small))
	draw_rect(Rect2(Vector2(text_x, status_y - dot * 1.4), Vector2(dot, dot)), STATUS_ONLINE)
	draw_string(MUNRO_FONT, Vector2(text_x + dot + 4.0 * s, status_y), "Online",
		HORIZONTAL_ALIGNMENT_LEFT, -1, small, PROFILE_DIM)
	var chev := ">"
	var chev_w := MUNRO_FONT.get_string_size(chev, HORIZONTAL_ALIGNMENT_LEFT, -1, name_size).x
	draw_string(MUNRO_FONT, Vector2(_profile_rect.end.x - inset - chev_w, name_y), chev,
		HORIZONTAL_ALIGNMENT_LEFT, -1, name_size, PROFILE_DIM)
	# The rule under the header: the shade tone, so it reads as a groove in the panel rather
	# than a second outline.
	draw_rect(Rect2(Vector2(_profile_rect.position.x + px, _profile_rule_y),
		Vector2(_profile_rect.size.x - 2.0 * px, px)), LOG_EDGE_SHADE)
	# A row the pointer is on gets a chevron in the gutter BEFORE its label, at the row's own
	# size - the row already turns its label white, and the arrow says "this one goes somewhere"
	# without a fill, which is the rule for every flat row in this screen. It is drawn into the
	# space the row's own left inset leaves, so the label does not move when the arrow appears.
	if _profile_hover >= 0 and _profile_hover < _profile_rows.size():
		var hovered: Button = _profile_rows[_profile_hover]
		var glyph := ">"
		var glyph_size := int(PROFILE_SMALL_UNITS * s)
		# On the row's own left edge - the head's line - because the label's inset already reserves
		# room for it (`_style_flat(btn, s, 6.0)`); the arrow fills the gutter instead of pushing
		# the text aside, so hovering a row moves nothing.
		var base := (hovered.position.y + (hovered.size.y
			+ float(MUNRO_FONT.get_height(glyph_size))) * 0.5
			- float(MUNRO_FONT.get_descent(glyph_size)))
		draw_string(MUNRO_FONT, Vector2(hovered.position.x, base), glyph,
			HORIZONTAL_ALIGNMENT_LEFT, -1, glyph_size, PROFILE_TEXT)


## The friends panel: header, search (a real field, drawn by itself), tabs with an accent
## underline on the active one, the list - which is empty, and says why - and Add Friend.
## Nothing here draws a friend row, because there are no friends to draw: the rows arrive with
## multiplayer, and a renderer for data that cannot exist yet is a guess.
func _draw_friends() -> void:
	if _friends_rect.size.x <= 0.0:
		return
	_draw_panel(_friends_rect)
	var s := maxf(UIScale.value, 1.0)
	var px := maxf(1.0, roundf(s))
	var boxes := _friends_boxes()
	var pad := float(boxes["pad"])
	var label := int(FRIENDS_LABEL_UNITS * s)
	var small := int(FRIENDS_SMALL_UNITS * s)
	var x := _friends_rect.position.x + pad
	var head_mid := float(boxes["header_y"]) + float(boxes["header_h"]) * 0.5
	# Two heads and two shoulders, six rects: the smallest thing that reads as "people" at
	# this size, and no new asset for it.
	var u := 2.0 * s
	var glyph_x := x
	draw_rect(Rect2(Vector2(glyph_x, head_mid - u * 2.0), Vector2(u * 1.4, u * 1.4)), PROFILE_DIM)
	draw_rect(Rect2(Vector2(glyph_x - u * 0.4, head_mid - u * 0.4), Vector2(u * 2.2, u * 1.4)),
		PROFILE_DIM)
	draw_rect(Rect2(Vector2(glyph_x + u * 1.6, head_mid - u * 1.6),
		Vector2(u * 1.2, u * 1.2)), PROFILE_DIM)
	draw_rect(Rect2(Vector2(glyph_x + u * 1.2, head_mid - u * 0.2),
		Vector2(u * 1.8, u * 1.2)), PROFILE_DIM)
	var title_x := glyph_x + u * 4.0
	draw_string(MUNRO_FONT, Vector2(title_x, head_mid + float(MUNRO_FONT.get_ascent(label)) * 0.5),
		"FRIENDS", HORIZONTAL_ALIGNMENT_LEFT, -1, label, PROFILE_TEXT)
	# The state, right-aligned: there is no service to count, so it says what is true.
	var st_w := MUNRO_FONT.get_string_size(FRIENDS_STATUS, HORIZONTAL_ALIGNMENT_LEFT, -1,
		small).x
	var st_x := _friends_rect.end.x - pad - st_w
	var dot := 3.0 * s
	var st_y := head_mid + float(MUNRO_FONT.get_ascent(small)) * 0.5
	draw_rect(Rect2(Vector2(st_x - dot - 3.0 * s, st_y - dot * 1.35), Vector2(dot, dot)),
		FRIENDS_DOT)
	draw_string(MUNRO_FONT, Vector2(st_x, st_y), FRIENDS_STATUS, HORIZONTAL_ALIGNMENT_LEFT, -1,
		small, PROFILE_DIM)
	draw_rect(Rect2(Vector2(_friends_rect.position.x + px,
		float(boxes["header_y"]) + float(boxes["header_h"])),
		Vector2(_friends_rect.size.x - 2.0 * px, px)), LOG_EDGE_SHADE)

	# The tabs are a SEGMENTED CONTROL, not three labels sitting in the open: one 1-texel box
	# round the strip, a hairline between the segments, and the accent along the bottom edge of
	# the one that is on - the same marker the menu column uses for the row you are on. The box is
	# what stops them reading as loose text, and the segments are why the labels are centred
	# rather than left-aligned like the rows elsewhere on the panel (a left-aligned label inside
	# an even third of a box reads as a paragraph someone cut up).
	if not _friends_tabs.is_empty() and _friends_tabs[0].visible:
		var strip := Rect2(_friends_tabs[0].position, _friends_tabs[0].size)
		for tab in _friends_tabs:
			strip = strip.merge(Rect2(tab.position, tab.size))
		draw_rect(strip, LOG_EDGE_SHADE, false, px)
		for i in range(1, _friends_tabs.size()):
			var divider: Button = _friends_tabs[i]
			draw_rect(Rect2(Vector2(divider.position.x, strip.position.y), Vector2(px, strip.size.y)),
				LOG_EDGE_SHADE)
		if _friends_tab >= 0 and _friends_tab < _friends_tabs.size():
			var on: Button = _friends_tabs[_friends_tab]
			# One texel, sitting exactly on the strip's bottom border where the active segment's
			# edge would be - an accent EDGE, not a bar laid over the frame. The 2-texel version
			# read as a thick stripe that floated across the bottom of the box.
			draw_rect(Rect2(Vector2(on.position.x + px, strip.end.y - px),
				Vector2(on.size.x - px * 2.0, px)), _accent)

	# The list. Its first two lines are the same on every tab where nothing can be here.
	var list: Rect2 = boxes["list"]
	var list_top := list.position.y
	var list_bottom := list.end.y
	var add_top: float = float(boxes["add"].position.y)
	var lines := []
	var query := _friends_search.text.strip_edges() if _friends_search != null else ""
	if _friends_tab == 0:
		if query != "":
			lines = [FRIENDS_NO_MATCH]
		else:
			lines = [FRIENDS_EMPTY, FRIENDS_REASON]
	else:
		lines = [String(FRIENDS_EMPTY_TAB[_friends_tab - 1])]
	var line_y := (list_top + list_bottom) * 0.5 - float(lines.size()) * LOG_LINE_H * s * 0.5
	for i in range(lines.size()):
		var text_px := small if i > 0 or _friends_tab > 0 else int(FRIENDS_SMALL_UNITS * s)
		var text := String(lines[i])
		var w := MUNRO_FONT.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, text_px).x
		var colour := PROFILE_TEXT if i == 0 else PROFILE_DIM
		draw_string(MUNRO_FONT,
			Vector2(_friends_rect.get_center().x - w * 0.5,
				line_y + float(MUNRO_FONT.get_ascent(text_px))),
			text, HORIZONTAL_ALIGNMENT_LEFT, -1, text_px, colour)
		line_y += LOG_LINE_H * s
	draw_rect(Rect2(Vector2(_friends_rect.position.x + px, add_top - 5.0 * s),
		Vector2(_friends_rect.size.x - 2.0 * px, px)), LOG_EDGE_SHADE)


## A five-unit crown of three spikes and a band. Drawn rather than shipped as a texture: it is
## six rects, and a new image for it would be the only asset on the screen that is not used
## somewhere else in the game.
func _draw_crown(at: Vector2, s: float) -> void:
	var u := 1.5 * s
	draw_rect(Rect2(at + Vector2(0.0, u * 2.0), Vector2(u * 5.0, u * 1.3)), CROWN)
	for i in range(3):
		draw_rect(Rect2(at + Vector2(u * (0.4 + 2.0 * float(i)), 0.0), Vector2(u * 1.1, u * 2.2)),
			CROWN)


## The update log: one flat card, its rect snapped to the scrim's own grid.
##
## It is a card because the sky is the brightest thing here and grey text laid straight on
## it fails contrast; the entries are near-white over a 0.52 tint, which holds 4.75:1 against
## the brightest sky the world can put behind it.
##
## It is filled square by square, with the same per-square noise the plate carries - see
## `_draw_panel`. There is no wind on a panel: a square of the changelog flying off to the right
## would take the words with it, so a panel's squares vary but never move.
func _draw_log() -> void:
	if _log_rect.size.x <= 0.0 or _log_rect.size.y <= 0.0:
		return
	var s := maxf(UIScale.value, 1.0)
	var px := maxf(1.0, roundf(s))
	_draw_panel(_log_rect)
	var pad := LOG_PAD * s
	var x := _log_rect.position.x + pad
	var head_size := int(LOG_HEADING_UNITS * s)
	var body_size := int(LOG_BODY_UNITS * s)
	var y := _log_rect.position.y + pad + float(MUNRO_FONT.get_ascent(head_size))
	draw_string(MUNRO_FONT, Vector2(x, y), LOG_TITLE, HORIZONTAL_ALIGNMENT_LEFT, -1,
		head_size, LOG_TEXT)
	y += (LOG_HEADING_UNITS + 9.0) * s - float(MUNRO_FONT.get_ascent(body_size))
	var version := "v" + _log_version if _log_version != "" else ""
	draw_string(MUNRO_FONT, Vector2(x, y), version, HORIZONTAL_ALIGNMENT_LEFT, -1,
		body_size, LOG_DIM)
	y += (LOG_BODY_UNITS + 8.0) * s - float(MUNRO_FONT.get_descent(body_size))
	var bullet := Vector2(2.0, 2.0) * s
	for entry in _log_entries:
		# A short window can leave fewer cells for the card than it has entries: the last ones
		# stop at the frame rather than drawing over it.
		if y + LOG_LINE_H * s > _log_rect.end.y - px * 2.0:
			break
		var baseline := y + float(MUNRO_FONT.get_ascent(body_size))
		# A two-unit square instead of a hyphen: the smallest thing on the card is still
		# a square, which is the whole screen's unit.
		draw_rect(Rect2(Vector2(x, baseline - 3.0 * s), bullet), LOG_DIM)
		draw_string(MUNRO_FONT, Vector2(x + 6.0 * s, baseline), String(entry),
			HORIZONTAL_ALIGNMENT_LEFT, _log_rect.size.x - pad - 6.0 * s, body_size, LOG_TEXT)
		y += LOG_LINE_H * s


## The scrim behind the menu, as a grid of SQUARES. It used to be 48 bands across the width
## with a dozen tall rectangles for the left plate: two grids with two different cell shapes,
## so the lattice they overlapped into was made of rectangles (57 x 22 px on the left,
## full-width strips above and below it). Now there is one cell, the same size on both axes
## and a whole number of pixels, filled with a single tone.
##
## Every square carries a little darkness of its own, and every so often one lifts off the
## plate and blows away to the right - leaving the world showing through the hole until it
## fades back in. Both are per-square: the tone is decided once per cell and filled flat, so
## the noise never softens an edge and a lifted square is the same square, carried.
func _draw_scrim() -> void:
	var square := Vector2(_cell, _cell)
	for row in range(_grid_h):
		for col in range(_grid_w):
			var index := row * _grid_w + col
			var alpha := _cell_alpha(col, row)
			if alpha <= 0.0 or _lifted.has(index):
				continue
			if _healing.has(index):
				# It fades back in where it left: a hole that snapped shut would read as a
				# flicker, which is the one thing a still screen must not do.
				alpha *= clampf(float(_healing[index]) / HEAL_TIME, 0.0, 1.0)
			if alpha <= 0.0:
				continue
			var tint := SCRIM_TINT
			tint.a = alpha
			draw_rect(Rect2(Vector2(float(col) * _cell, float(row) * _cell), square), tint)
	# What has lifted off: the same flat square, carried right on the wind and fading out.
	for index in _lifted.keys():
		var lift: Dictionary = _lifted[index]
		var age: float = lift["age"]
		var fade := clampf((LIFT_LIFE - age) / (LIFT_LIFE * LIFT_FADE), 0.0, 1.0)
		if fade <= 0.0:
			continue
		var tint := SCRIM_TINT
		tint.a = float(lift["alpha"]) * fade
		draw_rect(Rect2(lift["origin"] + lift["velocity"] * age, square), tint)


## Rebuild the grid for the current window: the cells are a function of the height, so a
## resize changes every one of them. The noise is keyed on (col, row) rather than on a running
## random, so the plate looks the same after a resize as before it.
func _build_grid() -> void:
	_cell = _cell_size()
	_grid_w = maxi(1, int(ceil(size.x / _cell)))
	_grid_h = maxi(1, int(ceil(size.y / _cell)))
	_jitter.resize(_grid_w * _grid_h)
	for row in range(_grid_h):
		for col in range(_grid_w):
			var base := _scrim_shade((float(col) + 0.5) * _cell)
			# Scaled by the plate, so the lit end of the falloff is not noisy and the dark
			# end is not patchy: the same relative variation everywhere, which is what reads
			# as one surface rather than as two.
			var amount := NOISE_ALPHA * clampf(base / PLATE_ALPHA, 0.0, 1.0)
			_jitter[row * _grid_w + col] = (_hash01(col, row) * 2.0 - 1.0) * amount
	# A flight is a position in pixels of the old grid, so it is dropped rather than
	# teleported; the plate refills on its own within a second.
	_lifted.clear()
	_healing.clear()


## One square of a panel: the panel's own tone plus its share of the same per-square noise, in
## the plate's own amount (`NOISE_ALPHA`), so the three cards and the plate vary by one rule.
## Keyed on the cell's place on the SCREEN grid - the panels' rects are snapped to it - so the
## pattern is anchored to the plate's rather than restarting at each panel's corner, and a resize
## hands a square the same tone it had before.
func _panel_alpha(grid_x: int, grid_y: int) -> float:
	return clampf(LOG_PLATE_ALPHA + (_hash01(grid_x, grid_y) * 2.0 - 1.0) * NOISE_ALPHA, 0.0, 1.0)


## One square's tone: the plate's ramp where it sits, plus its own share of the noise.
func _cell_alpha(col: int, row: int) -> float:
	var index := row * _grid_w + col
	if index < 0 or index >= _jitter.size():
		return 0.0
	return maxf(_scrim_shade((float(col) + 0.5) * _cell) + _jitter[index], 0.0)


## The wind, one step. Corrected for the delta rather than for a frame count: a square's life
## is in seconds, so a hitch in the frame rate moves it further, not for longer.
func _advance(delta: float) -> void:
	for index in _healing.keys():
		var age: float = float(_healing[index]) + delta
		if age >= HEAL_TIME:
			_healing.erase(index)
		else:
			_healing[index] = age
	for index in _lifted.keys():
		var lift: Dictionary = _lifted[index]
		lift["age"] = float(lift["age"]) + delta
		if lift["age"] >= LIFT_LIFE:
			_lifted.erase(index)
			_healing[index] = 0.0
	_emit += delta
	while _emit >= LIFT_EVERY:
		_emit -= LIFT_EVERY
		_launch_one()


## Pick one square off the plate and give it a velocity. Picks are random over the whole plate
## rather than along an edge, so the wind arrives from everywhere at once; a square that is
## already in the air, or still fading back in, is skipped, so the plate never tears open.
func _launch_one() -> void:
	if _jitter.is_empty():
		return
	for attempt in range(8):
		var col := _rng.randi_range(0, _grid_w - 1)
		var row := _rng.randi_range(0, _grid_h - 1)
		var index := row * _grid_w + col
		if _lifted.has(index) or _healing.has(index):
			continue
		var alpha := _cell_alpha(col, row)
		if alpha <= 0.02:
			continue
		var speed := _cell * LIFT_CELLS_PER_SEC * _rng.randf_range(0.7, 1.3)
		var drift := _cell * LIFT_DRIFT * _rng.randf_range(-1.0, 1.0)
		_lifted[index] = {
			"age": 0.0,
			"alpha": alpha,
			"origin": Vector2(float(col) * _cell, float(row) * _cell),
			"velocity": Vector2(speed, drift),
		}
		return


## A stable pseudo-random 0..1 for a cell: the noise has to be the same every build, or the
## plate would reshuffle on a window resize.
func _hash01(x: int, y: int) -> float:
	var h := x * 374761393 + y * 668265263
	h = (h ^ (h >> 13)) * 1274126177
	h = h ^ (h >> 16)
	return float(h & 0x7fffffff) / 2147483647.0


## One cell's side, in pixels: square by construction, whole pixels, and the same in both
## axes.
func _cell_size() -> float:
	return maxf(2.0, roundf(size.y / 40.0))


## The scrim's tone at a point, as an alpha: one plate, from the left edge to nothing just
## past the column. Separate from the draw loop so the question "how dark is it there?" has an
## answer a probe can ask instead of one it has to re-implement.
func _scrim_shade(x: float) -> float:
	var s := UIScale.value
	var reach := (_margin + COLUMN + PLATE_TAIL) * s
	var t := clampf(x / maxf(reach, 1.0), 0.0, 1.0)
	# Smoothstep eases both ends: the full strength holds under the start of the labels, the
	# shallow part is where the eye reads a gradient, and the last of it disappears just past
	# the buttons. A straight ramp is the other option and leaves a visible crease at the
	# left edge, where the tone changes fastest.
	return PLATE_ALPHA * (1.0 - smoothstep(0.0, 1.0, t))


func _process(delta: float) -> void:
	# The wind is the only motion: the plate's noise is still, so Reduce Motion leaves a
	# static plate rather than a differently-broken one.
	if _motion and visible:
		_advance(delta)
		queue_redraw()
	if _note_timer > 0.0:
		_note_timer -= delta
		if _note_timer <= 0.0:
			_note = ""
			_refresh_footer()
	if not _quit_armed:
		return
	_quit_timer -= delta
	if _quit_timer <= 0.0:
		_disarm_quit()


func _on_start() -> void:
	_disarm_quit()
	start_pressed.emit()


## The world list is a page of this screen, not a second screen: same scrim, same rows,
## same focus. The list itself was handed in by the layer with `open`.
func _on_singleplayer() -> void:
	_disarm_quit()
	_set_page(PAGE_WORLDS)


## Multiplayer is not in this build. The row answers in the footer - the place this screen
## already uses for status - rather than doing nothing at all, which reads as a bug.
func _on_multiplayer() -> void:
	_disarm_quit()
	_note = "MULTIPLAYER IS NOT IN THIS BUILD YET"
	_note_timer = NOTE_SECONDS
	_refresh_footer()


func _on_back() -> void:
	_disarm_quit()
	_set_page(PAGE_TITLE)


func _on_world(index: int) -> void:
	_disarm_quit()
	if index < 0 or index >= _worlds.size():
		return
	world_chosen.emit()


func _on_settings() -> void:
	_disarm_quit()
	settings_pressed.emit()


func _on_quit() -> void:
	if _quit_index < 0:
		return
	if _quit_armed:
		quit_pressed.emit()
		return
	_quit_armed = true
	_quit_timer = QUIT_ARM_SECONDS
	_set_row_text(_quit_index, "QUIT?  PRESS AGAIN")


func _disarm_quit() -> void:
	_quit_armed = false
	_quit_timer = 0.0
	if _quit_index >= 0:
		_set_row_text(_quit_index, "QUIT")


## Escape (or a pad's back button) leaves the world list for the title. It is handled here
## rather than by the layer because it is the screen's own state, and there is nothing else
## for Escape to mean while the player is frozen behind the title.
func _unhandled_key_input(event: InputEvent) -> void:
	if _page != PAGE_WORLDS:
		return
	if event.is_action_pressed("ui_cancel"):
		_on_back()
		accept_event()
