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
# The scrim: how much world is left at the top and at the bottom. The column is
# read against the bottom half, which is where the darker end has to be.
const SCRIM_TOP := 0.34
const SCRIM_BOTTOM := 0.80
const PLATE_ALPHA := 0.30
# The accent, mixed with the world's own sky warmth when one is available. One
# accent for the primary action and the focus state, and nothing else.
const ACCENT := Color(1.0, 0.76, 0.42)

var _wordmark: Label
var _rows: VBoxContainer
var _footer: Label
var _buttons: Array = []
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
	set_anchors_preset(Control.PRESET_FULL_RECT)

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

	# A Control whose parent is a CanvasLayer has no parent rect to anchor
	# against, so the full-rect preset leaves it at zero: the screen's size has to
	# be taken from the viewport and kept on resize.
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
func open(world_exists: bool, live_seed, version_text: String, accent: Color) -> void:
	_accent = ACCENT.lerp(accent, 0.35) if accent != null else ACCENT
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
	queue_redraw()


func _draw() -> void:
	var s := UIScale.value
	var w := size.x
	var h := size.y
	# The scrim, in bands: a vertical gradient dark enough at the bottom for the
	# column to be read against, and a left plate so the labels never depend on
	# whichever frame of the world happens to be behind them.
	var bands := 48
	for i in range(bands):
		var t := float(i) / float(bands - 1)
		var alpha := lerpf(SCRIM_TOP, SCRIM_BOTTOM, t * t)
		draw_rect(Rect2(0.0, h * float(i) / float(bands), w, h / float(bands) + 1.0),
			Color(0.015, 0.018, 0.024, alpha))
	var plate_w := (_margin + COLUMN + 60.0) * s
	var plate_bands := 12
	for i in range(plate_bands):
		var t := float(i) / float(plate_bands - 1)
		var alpha := PLATE_ALPHA * (1.0 - t)
		draw_rect(Rect2(plate_w * float(i) / float(plate_bands), 0.0,
			plate_w / float(plate_bands) + 1.0, h), Color(0.01, 0.012, 0.018, alpha))
	# The accent: one underline for the wordmark and one bar on the focused row.
	draw_rect(Rect2(_margin * s, (_wordmark_y + WORDMARK_UNITS * 1.30) * s,
		COLUMN * 0.42 * s, 2.0 * s), _accent)
	if _focus_index >= 0 and _focus_index < _buttons.size():
		var row_y := _rows_y + float(_focus_index) * (ROW_H + ROW_GAP)
		draw_rect(Rect2((_margin - FOCUS_BAR_INSET) * s, row_y * s + ROW_H * 0.10 * s,
			FOCUS_BAR_W * s, ROW_H * 0.80 * s), _accent)


func _process(delta: float) -> void:
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
