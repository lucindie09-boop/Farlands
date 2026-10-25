extends Control
## The title screen: the wordmark, one primary action, Settings, Quit, and the
## world's seed in the footer.
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

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")

# Units. The column is wider and the rows taller than the settings menu's, because
# a title screen is read from further away and has one job rather than thirty.
const MARGIN := 40.0
const COLUMN := 240.0
const ROW_H := 28.0
const ROW_GAP := 6.0
const FIRST_ROW := 168.0
const WORDMARK_UNITS := 40
const LABEL_UNITS := 12
const FOOTER_UNITS := 7
const FOCUS_BAR_W := 3.0
const FOCUS_BAR_INSET := 11.0
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
# The accent, mixed with the world's own sky warmth when one is available. One
# accent for the primary action and the focus state, and nothing else.
const ACCENT := Color(1.0, 0.76, 0.42)

var _wordmark: Label
var _rows: VBoxContainer
var _footer: Label
var _buttons: Array = []
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
# Layout values that depend on the window, resolved in _layout() and read back by
# _draw(): a small window must not push the column off the bottom of the screen.
var _margin := MARGIN
var _wordmark_y := 54.0
var _rows_y := FIRST_ROW
# Quit is two presses, not one: a mis-click on the bottom row of the only screen
# standing between the player and the game should not end the session. No modal —
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

	_wordmark = Label.new()
	_wordmark.text = "FARLANDS"
	_wordmark.add_theme_font_override("font", MUNRO_FONT)
	_wordmark.add_theme_color_override("font_color", Color(0.96, 0.97, 0.99))
	_wordmark.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.55))
	_wordmark.add_theme_constant_override("shadow_offset_x", 2)
	_wordmark.add_theme_constant_override("shadow_offset_y", 3)
	_wordmark.add_theme_constant_override("outline_size", 0)
	add_child(_wordmark)

	_rows = VBoxContainer.new()
	_rows.add_theme_constant_override("separation", 12)
	add_child(_rows)

	# Row order is the interaction contract: the primary action first, housekeeping
	# after it, Quit last. Not reordered at runtime — only the first row's LABEL
	# changes with the save state, so a returning player's muscle memory holds.
	_add_row("PLAY", _on_start)
	_add_row("SETTINGS", _on_settings)
	_add_row("QUIT", _on_quit)

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


## Show the screen for a given world. `world_exists` is whether a save was there
## before this launch: false turns the primary action from CONTINUE into PLAY.
## `live_seed` is the seed the running world is using, shown in the footer whether
## or not that world was already saved — on a first run it is the seed about to be
## played, which is the one fact worth knowing before pressing Play.
func open(world_exists: bool, live_seed, version_text: String, accent: Color,
		motion: bool = true) -> void:
	_accent = ACCENT.lerp(accent, 0.35) if accent != null else ACCENT
	# Reduce Motion comes from the settings menu through `menu_layer.gd`, like the
	# transition's: the plate keeps its noise and stops losing squares, which is the switch.
	_motion = motion
	_lifted.clear()
	_healing.clear()
	_emit = 0.0
	_world_exists = world_exists
	_set_row_text(0, "CONTINUE" if _world_exists else "PLAY")
	var seed_text := "seed %d" % int(live_seed) if live_seed != null else "new world"
	_footer.text = "v%s  ·  %s" % [version_text, seed_text]
	_disarm_quit()
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


func _add_row(text: String, handler: Callable) -> void:
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	btn.add_theme_font_override("font", MUNRO_FONT)
	btn.add_theme_color_override("font_color", Color(0.88, 0.90, 0.93))
	btn.add_theme_color_override("font_focus_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color(0.85, 0.85, 0.85))
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	normal.modulate_color = Color(0.62, 0.64, 0.70)
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = Color(1.0, 1.0, 1.0)
	var focused := StyleBoxTexture.new()
	focused.texture = BUTTON_TEX
	focused.modulate_color = Color(1.06, 1.03, 0.98)
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(0.80, 0.80, 0.80)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", focused)
	btn.add_theme_stylebox_override("disabled", normal)
	btn.pressed.connect(handler)
	var index := _buttons.size()
	btn.focus_entered.connect(func():
		_focus_index = index
		queue_redraw())
	# Hover moves the focus rather than sitting beside it: two highlighted rows at
	# once is the state a menu must never be in, and a mouse player who then
	# reaches for Enter should continue from the row they were pointing at.
	btn.mouse_entered.connect(func():
		if not btn.has_focus():
			btn.grab_focus())
	_rows.add_child(btn)
	_buttons.append(btn)


func _set_row_text(index: int, text: String) -> void:
	if index >= 0 and index < _buttons.size():
		_buttons[index].text = text


func _layout() -> void:
	var s := maxf(UIScale.value, 1.0)
	var height_units := size.y / s
	# The screen's own height decides the margin and the first row, so the same
	# design survives a 720p window: a fixed row of units designed for 1080p
	# would run the Quit row into the footer on anything shorter.
	_margin = minf(MARGIN, height_units * 0.08)
	_wordmark_y = minf(54.0, height_units * 0.10)
	_rows_y = minf(FIRST_ROW, height_units * 0.34)
	_wordmark.position = Vector2(_margin, _wordmark_y) * s
	_wordmark.add_theme_font_size_override("font_size", int(WORDMARK_UNITS * s))
	_rows.position = Vector2(_margin, _rows_y) * s
	_rows.add_theme_constant_override("separation", int(ROW_GAP * s))
	for btn in _buttons:
		btn.custom_minimum_size = Vector2(COLUMN, ROW_H) * s
		btn.add_theme_font_size_override("font_size", int(LABEL_UNITS * s))
		# Only the text inset is rewritten here, so the per-state fills above
		# survive a re-layout.
		for state in ["normal", "hover", "focus", "pressed", "disabled"]:
			var box: StyleBoxTexture = btn.get_theme_stylebox(state).duplicate()
			box.content_margin_left = 9.0 * s
			box.content_margin_right = 9.0 * s
			btn.add_theme_stylebox_override(state, box)
	_footer.position = Vector2(_margin, (height_units - _margin - FOOTER_UNITS * 1.7) * s)
	_footer.add_theme_font_size_override("font_size", int(FOOTER_UNITS * s))
	_build_grid()
	queue_redraw()


func _draw() -> void:
	var s := UIScale.value
	_draw_scrim()
	# The accent: one underline for the wordmark and one bar on the focused row.
	draw_rect(Rect2(_margin * s, (_wordmark_y + WORDMARK_UNITS * 1.30) * s,
		COLUMN * 0.42 * s, 2.0 * s), _accent)
	if _focus_index >= 0 and _focus_index < _buttons.size():
		var row_y := _rows_y + float(_focus_index) * (ROW_H + ROW_GAP)
		draw_rect(Rect2((_margin - FOCUS_BAR_INSET) * s, row_y * s + ROW_H * 0.10 * s,
			FOCUS_BAR_W * s, ROW_H * 0.80 * s), _accent)


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
	if not _quit_armed:
		return
	_quit_timer -= delta
	if _quit_timer <= 0.0:
		_disarm_quit()


func _on_start() -> void:
	_disarm_quit()
	start_pressed.emit()


func _on_settings() -> void:
	_disarm_quit()
	settings_pressed.emit()


func _on_quit() -> void:
	if _quit_armed:
		quit_pressed.emit()
		return
	_quit_armed = true
	_quit_timer = QUIT_ARM_SECONDS
	_set_row_text(2, "QUIT?  PRESS AGAIN")


func _disarm_quit() -> void:
	_quit_armed = false
	_quit_timer = 0.0
	_set_row_text(2, "QUIT")
