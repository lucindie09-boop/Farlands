extends Node
## Liquid Texture Lab — opened from the pause menu's Tools page (settings_menu.gd),
## and closed with Escape or its Close button.
##
## An authoring tool for procedural animated liquid textures. It generates a
## frame strip in C++ (render/liquid_texture.hpp, exposed as LiquidTextureGen)
## for one of three styles — water, lava, acid — and lets you tune every knob
## that shapes it: resolution, frame count, how fast frames advance, the
## neighbour kernel and the physics constants behind the ripple, the colour
## ramp (four stops, a curve, posterizing, grain), static grain, and the
## flowing-liquid scroll.
##
## Everything is live: changing a slider regenerates the strip (debounced) and
## the preview panel animates the result at the frame time you set. "Sheet"
## shows the strip as it will be saved; "Tiled" repeats the current frame 3x3
## so a seam that would show up on a big pool is obvious; the thumbnails are
## frames 0..7.
##
## Three ways out of the tool:
##   Save      writes user://liquids/<name>.png (the vertical strip) plus a
##             <name>.json sidecar holding every setting, and Load restores them
##             exactly, so a texture can be reproduced and re-tuned later.
##   Live      pushes the current frame into the world's texture array layer for
##             the chosen liquid (all three exist: water, lava and acid are real
##             blocks using Assets/textures/<liquid>.png). That is
##             ChunkManager.push_texture_frame -> Texture2DArray.update_layer, so
##             the world animates without rebuilding the array; the layer is
##             restored from disk when Live is switched off. While Live owns a
##             liquid, the world animator is paused for it (see below).
##   Bind      saves the strip as <liquid>.json/.png for the chosen liquid, which
##             is the file LiquidAnimator (liquid_animator.gd) picks up: that node
##             animates water, lava and acid in the world whenever the game runs,
##             panel or no panel.
##
## "World anim" is the LiquidAnimator's own switch, shown here because this is
## where you would look for it.
##
## Legal note that also applies here: only the *approach* (a three-field
## automaton mapped through a colour ramp, and the classic 16x16 liquid look) is
## borrowed from how the classic block game generated these sprites; the field
## names, defaults, kernels and colours in this project are our own.

const SAVE_DIR := "user://liquids"
const MUNRO_FONT: Font = preload("res://Assets/fonts/munro.ttf")
const UIArt := preload("res://scripts/ui_art.gd")
const TICK_RATE := 20.0  # frames are held for `frame_time` game ticks

# Setting rows: [key, label, min, max, step]. Ints are whole numbers.
const INT_ROWS := [
	["resolution", "Resolution", 4, 256, 1],
	["frames", "Frames", 1, 256, 1],
	["frame_time", "Hold (ticks)", 1, 40, 1],
	["interpolate", "Sub-frames", 1, 8, 1],
	["warmup_steps", "Warm-up steps", 0, 400, 5],
	["steps_per_frame", "Steps per frame", 0, 8, 1],
	["shift_rows", "Scroll rows", -16, 16, 1],
	["shift_period", "Scroll every N frames", 1, 32, 1],
]
const FLOAT_ROWS := [
	["surface_divisor", "Neighbour divisor", 1.0, 16.0, 0.05],
	["flow_coupling", "Flow coupling", 0.0, 1.0, 0.01],
	["flow_gain", "Surge into flow", 0.0, 0.2, 0.001],
	["surge_decay", "Surge decay", 0.0, 0.5, 0.005],
	["excite_chance", "Excite chance", 0.0, 0.2, 0.001],
	["excite_strength", "Excite strength", 0.0, 4.0, 0.05],
	["warp_shift", "Warp offset", 0.0, 4.0, 0.1],
	["field_scale", "Field scale", 0.0, 4.0, 0.01],
	["ramp_curve", "Ramp curve (gamma)", 0.1, 4.0, 0.05],
	["grain", "Static grain", 0.0, 0.4, 0.01],
]
const CHECK_ROWS := [
	["flow_box", "2x2 flow window"],
	["wrap", "Wrap (torus, no seams)"],
]
# The loop rows get their own section: they are about the strip as a whole, not
# about the automaton.
const LOOP_CHECKS := [
	["loop", "Loop (last frame repeats the first)"],
]
const KERNELS := ["row", "box", "warp", "plus"]
const STYLES := ["water", "lava", "acid"]
# The row label's column: wide enough for `Scroll every N frames` at 13 px, so
# every slider starts at the same x and the labels sit flush with the sections.
const ROW_LABEL_W := 140.0
const TARGETS := ["water", "lava", "acid"]
const THUMBNAILS := 8

var settings: Dictionary = {}

var _open := false
var _layer: CanvasLayer = null
# The GUI scale the panel was built at (UIScale.value, rounded): every size in
# the layout is in units times this, the same grid the makers use.
var _s := 1.0
var _status: Label = null
var _name_edit: LineEdit = null
var _load_pick: OptionButton = null
var _live_toggle: Button = null
var _play_button: Button = null
var _target_pick: OptionButton = null
var _live_label: Label = null
var _world_toggle: Button = null
var _world_label: Label = null
# The liquid Live has taken the layer of, so the animator can be handed it back
# when Live moves to another liquid or stops.
var _live_owner := ""
var _world_report := 0.0

var _live_preview: TextureRect = null
var _sheet_preview: TextureRect = null
var _tiled_preview: TextureRect = null
var _thumbs: Array[TextureRect] = []
var _ramp_buttons: Array[ColorPickerButton] = []
var _sliders: Dictionary = {}     # key -> HSlider
var _value_labels: Dictionary = {}  # key -> Label
var _checks: Dictionary = {}      # key -> toggle Button
var _preset_button: Button = null
var _kernel_pick: OptionButton = null
var _seed_edit: LineEdit = null

var _strip: Image = null
var _frames: Array[Image] = []
# Mirrors Strips::looped: the last frame repeats the first, so playback has to
# wrap to index 1 instead of showing index 0 twice in a row.
var _looped := false
var _live_tex: ImageTexture = null
var _tiled_tex: ImageTexture = null
var _sheet_tex: ImageTexture = null
var _tiled_frame := -1
var _frame := 0
var _accum := 0.0
var _playing := true
var _dirty := true
var _dirty_delay := 0.0
var _refreshing := false
var _live := false
var _described: Dictionary = {}
var _compression_flipped := false
var _compression_was := false
var _last_ms := 0.0
var _flat_low := 0.0
var _flat_high := 0.0

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_ensure_settings()

# The defaults come from the C++ side (so there is exactly one copy of the
# schema), but resolving them lazily keeps this node safe to instantiate before
# the gdextension is up (headless probes, editor restarts).
func _ensure_settings() -> void:
	if not settings.is_empty():
		return
	settings = LiquidTextureGen.default_settings("water", 16)

## Escape closes the panel, the key that backs out of every other overlay, and the
## only key the lab reads (the Tools page opens it, not a key). Read in `_input`
## like the chat's: the panel's name field can hold the keyboard focus, and a
## focused field would take the key before `_unhandled_input` saw it.
func _input(event: InputEvent) -> void:
	if not _open or not event.is_action_pressed("ui_cancel") or event.is_echo():
		return
	_hide_lab()
	get_viewport().set_input_as_handled()

func _process(delta: float) -> void:
	if _dirty and _open:
		_dirty_delay -= delta
		if _dirty_delay <= 0.0:
			_regenerate()
	if _strip == null or _frames.is_empty():
		return
	# Playback: hold each frame for `frame_time` game ticks. Runs whether or not
	# the panel is open, so a Live texture keeps animating in the world.
	if _playing:
		_accum += delta
		var hold := maxf(1.0, float(int(settings.get("frame_time", 3)))) / TICK_RATE
		var guard := 0
		while _accum >= hold and guard < 64:
			_accum -= hold
			_advance()
			guard += 1
	if _open:
		_update_previews()
		if _live:
			_report_live_target()
		# The world animator runs on its own clock, so its readout is refreshed a
		# few times a second rather than every frame.
		_world_report -= delta
		if _world_report <= 0.0:
			_world_report = 0.25
			_report_world()
	if _live:
		_push_frame()

# One playback step. A looping strip's last frame is the first frame's image, so
# the frame after it is index 1: showing index 0 again would hold the image for
# two frame times in a row (a visible hitch at the seam).
func _advance() -> void:
	var next := _frame + 1
	if next >= _frames.size():
		next = 1 if _looped and _frames.size() > 1 else 0
	_frame = next

# ---------------------------------------------------------------------------
# Panel
# ---------------------------------------------------------------------------

## The Tools page's button has no key press to hand over, so it asks for the panel
## itself. `open()` is the one name ToolLoader knows for that.
func open() -> void:
	if not _open:
		_show_lab()

func _show_lab() -> void:
	_ensure_settings()
	if _layer == null:
		_build_ui()
	_refresh_widgets()
	# Every open, not just after a save: the saved strips live in a directory that
	# outlives the process, so the picker has to be filled from disk on the first
	# open of a session or nothing saved earlier can be loaded at all. It also
	# picks up files another run left behind.
	_refresh_load_list(_name_edit.text.strip_edges() if _name_edit else "")
	_dirty = true
	_dirty_delay = 0.0
	_open = true
	_layer.visible = true
	var player := _player()
	if player:
		player.set_settings_open(true)

func _hide_lab() -> void:
	_open = false
	if _layer:
		_layer.visible = false
	if not _live:
		# Live keeps animating in the world while the panel is closed, so the
		# compression flip has to outlive the panel exactly as long as it does.
		_restore_compression()
	var player := _player()
	if player:
		player.set_settings_open(false)
	# Back to where it was opened from: the Tools page closed this menu to hand
	# the screen over (settings_menu._open_liquid_lab), so leaving the lab puts
	# the menu back on the tools list instead of dropping the player into the
	# game still paused.
	if is_inside_tree():
		var menu := get_node_or_null("/root/Main/HUD/SettingsMenu")
		if menu != null and menu.has_method("open_tools_page"):
			menu.call("open_tools_page")

func _player() -> Node:
	return get_node_or_null("/root/Main/Player")

func _chunk_manager() -> Node:
	return get_node_or_null("/root/Main/ChunkManager")

func _build_ui() -> void:
	# The lab is a dense tool, so it keeps its own 1:1 unit grid rather than the
	# makers' GUI scale: at the makers' 40-px rows its thirty settings did not fit
	# on the screen at any window size and the list had to be scrolled.
	_s = 1.0
	_layer = CanvasLayer.new()
	_layer.layer = 100  # above the HUD
	_layer.visible = false
	add_child(_layer)

	var root := Control.new()
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_STOP
	_layer.add_child(root)

	var backdrop := ColorRect.new()
	backdrop.color = Color(0.0, 0.0, 0.0, 0.55)
	backdrop.set_anchors_preset(Control.PRESET_FULL_RECT)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	root.add_child(backdrop)

	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 16)
	root.add_child(margin)

	# The lab is a panel of the interface, not a sheet of controls laid on the
	# world: the same framed box the menus use, filling the window, so the rows
	# and the previews have the room they were being squeezed out of.
	var shell := PanelContainer.new()
	shell.add_theme_stylebox_override("panel", UIArt.panel(1.0, Color.WHITE, 10.0))
	margin.add_child(shell)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	shell.add_child(column)

	var title := Label.new()
	title.text = "LIQUID TEXTURE LAB   —   ESC to close"
	title.add_theme_font_override("font", MUNRO_FONT)
	title.add_theme_font_size_override("font_size", 18)
	title.add_theme_color_override("font_color", Color(0.75, 0.95, 1.0))
	column.add_child(title)

	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", 12)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(body)

	body.add_child(_build_settings_column())
	body.add_child(_build_preview_column())

	var footers := VBoxContainer.new()
	footers.add_theme_constant_override("separation", 3)
	column.add_child(footers)
	_build_footer(footers)

	_style_house_controls(root)

## The lab is a tool of the same interface as the menus, so its controls wear the
## same art: the button bevel, the slider track and thumb, the framed panel. They
## are found by walking the tree once after it is built rather than at each of the
## two dozen places a control is made, so a control added later is styled too.
## The lab's layout is in units at the GUI scale (`_s`), the same grid the
## makers use, so its art is drawn at that scale too.
func _style_house_controls(node: Node) -> void:
	for child in node.get_children():
		if child is HSlider:
			UIArt.style_slider(child, 1.0)
		elif child is LineEdit:
			UIArt.style_line_edit(child, 1.0, Color.WHITE, true)
		elif child is Button:
			var button := child as Button
			UIArt.style_button(button, 1.0, Color.WHITE, 3.0)
			# The bevel art carries no content margin, so a button sized by its
			# label alone sat 28 px tall beside a 40-px slider. Every control in a
			# row is one row tall.
			button.custom_minimum_size.y = maxf(button.custom_minimum_size.y, 22.0)
		_style_house_controls(child)

func _make_panel(_color: Color = Color.WHITE) -> PanelContainer:
	# The framed box the menus and the maker screens use, not a flat colour.
	var panel := PanelContainer.new()
	panel.add_theme_stylebox_override("panel", UIArt.panel(1.0, Color.WHITE, 8.0))
	return panel

func _label(text: String, size: int = 12, color: Color = Color(0.85, 0.87, 0.9)) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(roundf(size * _s)))
	label.add_theme_color_override("font_color", color)
	return label

func _section(parent: Node, text: String) -> void:
	var head := _label(text, 13, Color(0.6, 0.85, 1.0))
	parent.add_child(head)

## The three presets are one button: a click steps through them.
func _cycle_style() -> void:
	var current := String(settings.get("style", "water"))
	_apply_style(STYLES[(STYLES.find(current) + 1) % STYLES.size()])

func _build_settings_column() -> Control:
	var scroll := ScrollContainer.new()
	# Two columns of compact rows. One column at the house scale was taller than
	# the window and had to be scrolled; one column at this scale was a strip of
	# hairlines 530 px long. Two columns hold all thirty settings on one screen
	# beside the previews, with each row 140 units of label and the rest slider.
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.size_flags_stretch_ratio = 1.0
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED

	var columns := HBoxContainer.new()
	columns.add_theme_constant_override("separation", 14)
	columns.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(columns)
	var left := VBoxContainer.new()
	var right := VBoxContainer.new()
	for column in [left, right]:
		column.add_theme_constant_override("separation", 4)
		column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		columns.add_child(column)

	# Style + seed + shape + loop -------------------------------------------
	_section(left, "STYLE")
	# One button that cycles, not three: Water/Lava/Acid as separate buttons were
	# three chips the width of a word, and it is one choice either way.
	_preset_button = Button.new()
	_preset_button.text = String(settings.get("style", "water")).capitalize()
	_preset_button.tooltip_text = "Click to change style: water, lava, acid"
	_preset_button.add_theme_font_override("font", MUNRO_FONT)
	_preset_button.add_theme_font_size_override("font_size", 13)
	# As wide as the word: a button stretched across the column reads as a bar.
	_preset_button.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	_preset_button.custom_minimum_size = Vector2(90, 0)
	_preset_button.pressed.connect(_cycle_style)
	left.add_child(_preset_button)

	var seed_row := HBoxContainer.new()
	seed_row.add_theme_constant_override("separation", 6)
	left.add_child(seed_row)
	var seed_label := _label("Seed")
	seed_label.custom_minimum_size = Vector2(ROW_LABEL_W, 0)
	seed_row.add_child(seed_label)
	_seed_edit = LineEdit.new()
	_seed_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_seed_edit.add_theme_font_override("font", MUNRO_FONT)
	_seed_edit.add_theme_font_size_override("font_size", 13)
	_seed_edit.text_submitted.connect(func(_t): _commit_seed())
	_seed_edit.focus_exited.connect(_commit_seed)
	seed_row.add_child(_seed_edit)
	var roll := Button.new()
	roll.text = "Roll"
	roll.add_theme_font_override("font", MUNRO_FONT)
	roll.add_theme_font_size_override("font_size", 13)
	# A Roll button the width of the word next to a field that filled the column
	# was the wrong way round: both are one row of this panel's controls.
	roll.custom_minimum_size = Vector2(52, 0)
	roll.pressed.connect(func():
		settings["seed"] = randi() & 0x7fffffff
		_refresh_widgets()
		_mark_dirty())
	seed_row.add_child(roll)

	_section(left, "SHAPE")
	for row in INT_ROWS:
		_add_int_row(left, row[0], row[1], row[2], row[3], row[4])

	_section(left, "LOOP")
	for row in LOOP_CHECKS:
		_add_check_row(left, row[0], row[1])
	_add_int_row(left, "loop_window", "Blend window (frames)", 1, 32, 1)

	# Automaton + ramp -------------------------------------------------------
	_section(right, "AUTOMATON")
	var kernel_row := HBoxContainer.new()
	kernel_row.add_theme_constant_override("separation", 6)
	right.add_child(kernel_row)
	var kernel_label_ctl := _label("Kernel")
	kernel_label_ctl.custom_minimum_size = Vector2(ROW_LABEL_W, 0)
	kernel_row.add_child(kernel_label_ctl)
	_kernel_pick = OptionButton.new()
	_kernel_pick.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_kernel_pick.add_theme_font_override("font", MUNRO_FONT)
	_kernel_pick.add_theme_font_size_override("font_size", 13)
	for kernel_label in KERNELS:
		_kernel_pick.add_item(kernel_label)
	_kernel_pick.item_selected.connect(func(index: int):
		settings["kernel"] = KERNELS[index]
		_mark_dirty())
	kernel_row.add_child(_kernel_pick)
	for row in FLOAT_ROWS:
		_add_float_row(right, row[0], row[1], row[2], row[3], row[4])
	for row in CHECK_ROWS:
		_add_check_row(right, row[0], row[1])

	_section(right, "RAMP")
	var stops_row := HBoxContainer.new()
	stops_row.add_theme_constant_override("separation", 6)
	right.add_child(stops_row)
	var stops_label := _label("Stops")
	stops_label.custom_minimum_size = Vector2(ROW_LABEL_W, 0)
	stops_row.add_child(stops_label)
	for index in 4:
		var picker := ColorPickerButton.new()
		picker.custom_minimum_size = Vector2(34, 22)
		picker.edit_alpha = true
		picker.color_changed.connect(func(color: Color): _set_ramp_stop(index, color))
		_ramp_buttons.append(picker)
		stops_row.add_child(picker)
	_add_int_row(right, "ramp_stops", "Used stops", 2, 4, 1)
	_add_int_row(right, "posterize", "Posterize (0 = off)", 0, 16, 1)

	return scroll

func _build_preview_column() -> Control:
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 9)
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# The previews take the wider share of the panel: the settings are two compact
	# columns and the textures are what the tool is being used to look at.
	column.size_flags_stretch_ratio = 1.5

	# The two preview rows share whatever height the panel has left, so the
	# previews grow with the window instead of sitting in a corner of it.
	var live_panel := _make_panel(Color(0.05, 0.06, 0.08, 0.9))
	live_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var live_box := VBoxContainer.new()
	live_panel.add_child(live_box)
	var live_row := HBoxContainer.new()
	live_row.add_theme_constant_override("separation", int(5 * _s))
	live_row.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# The live pair is what the tool is looked at through; the sheet below it is
	# a reference. Sharing the height evenly left both of them small squares in
	# a wide panel.
	live_row.size_flags_stretch_ratio = 1.7
	live_box.add_child(live_row)

	# The previews take the width they are given: at a fixed 224 px they were
	# two small squares in the corner of a mostly empty panel.
	_live_preview = _preview_rect(200)
	live_row.add_child(_preview_box("LIVE", _live_preview))
	_tiled_preview = _preview_rect(200)
	live_row.add_child(_preview_box("TILED 3x3 (seam check)", _tiled_preview))

	var sheet_row := HBoxContainer.new()
	sheet_row.add_theme_constant_override("separation", int(5 * _s))
	sheet_row.size_flags_vertical = Control.SIZE_EXPAND_FILL
	live_box.add_child(sheet_row)
	_sheet_preview = _preview_rect(200)
	sheet_row.add_child(_preview_box("SHEET (what gets saved)", _sheet_preview))
	var thumbs_box := VBoxContainer.new()
	thumbs_box.add_theme_constant_override("separation", int(2 * _s))
	thumbs_box.alignment = BoxContainer.ALIGNMENT_CENTER
	sheet_row.add_child(thumbs_box)
	thumbs_box.add_child(_label("FRAMES 0-7"))
	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", int(2 * _s))
	grid.add_theme_constant_override("v_separation", int(2 * _s))
	thumbs_box.add_child(grid)
	for index in THUMBNAILS:
		var thumb := TextureRect.new()
		thumb.custom_minimum_size = Vector2(32 * _s, 32 * _s)
		thumb.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		thumb.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		thumb.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		grid.add_child(thumb)
		_thumbs.append(thumb)
	column.add_child(live_panel)

	_status = _label("", 12, Color(0.8, 0.9, 0.8))
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(_status)
	_live_label = _label("", 12, Color(0.95, 0.85, 0.6))
	column.add_child(_live_label)
	_world_label = _label("", 12, Color(0.6, 0.9, 0.7))
	column.add_child(_world_label)
	return column

func _preview_rect(_size: int) -> TextureRect:
	var rect := TextureRect.new()
	# The minimum is only what the box needs to stay usable: both directions
	# expand, so the previews fill the column from there. A square minimum as
	# tall as it is wide made the two preview rows 860 px tall between them,
	# which pushed the footer clean out of the panel.
	rect.custom_minimum_size = Vector2(140 * _s, 110 * _s)
	rect.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rect.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Without this the box's MINIMUM is the texture's own size: a 16x512 sheet
	# asked for 512 px of height, which made the panel taller than the window
	# and pushed the footer off the bottom of it.
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	return rect

func _preview_box(caption: String, rect: TextureRect) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", int(1 * _s))
	# Both preview boxes share the row, not just the row's minimum: without this
	# they stayed 400 px wide and the rest of the panel was a strip of nothing.
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_label(caption, 11, Color(0.6, 0.75, 0.9)))
	box.add_child(rect)
	return box

func _build_footer(parent: VBoxContainer) -> void:
	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 6)
	parent.add_child(footer)
	_play_button = _footer_button(footer, "Pause", func(): _toggle_play())
	_footer_button(footer, "Step", func():
		if _frames.is_empty():
			return
		_playing = false
		_advance()
		_update_previews()
		_refresh_play_button())
	_footer_button(footer, "Regenerate", func():
		_dirty_delay = 0.0
		_regenerate())
	_footer_button(footer, "Reset style", func():
		_apply_style(String(settings.get("style", "water"))))

	footer.add_child(_label("Name", 11))
	_name_edit = LineEdit.new()
	_name_edit.custom_minimum_size = Vector2(110, 0)
	_name_edit.add_theme_font_override("font", MUNRO_FONT)
	_name_edit.add_theme_font_size_override("font_size", 13)
	_name_edit.text = "water_anim"
	footer.add_child(_name_edit)
	_footer_button(footer, "Save", func(): _save())
	_footer_button(footer, "Delete", func(): _delete())

	_load_pick = OptionButton.new()
	_load_pick.add_theme_font_override("font", MUNRO_FONT)
	_load_pick.add_theme_font_size_override("font_size", 13)
	_load_pick.custom_minimum_size = Vector2(130, 0)
	footer.add_child(_load_pick)
	_footer_button(footer, "Load", func(): _load_selected())

	footer.add_child(_label("Live onto", 11))
	_target_pick = OptionButton.new()
	_target_pick.add_theme_font_override("font", MUNRO_FONT)
	_target_pick.add_theme_font_size_override("font_size", 13)
	for target in TARGETS:
		_target_pick.add_item(target)
	_target_pick.item_selected.connect(func(_index: int):
		_set_live_owner(_live_target() if _live else "")
		_report_live_target())
	footer.add_child(_target_pick)
	# Toggles rather than CheckButtons: a 16-px tick is a speck beside the house
	# scale's text, and the bevel's dimmed state is the interface's own way of
	# showing a switch that is on.
	_live_toggle = Button.new()
	_live_toggle.toggle_mode = true
	_live_toggle.text = "Live"
	_live_toggle.add_theme_font_override("font", MUNRO_FONT)
	_live_toggle.toggled.connect(_set_live)
	footer.add_child(_live_toggle)
	_footer_button(footer, "Bind to world", func(): _bind_to_world())

	_world_toggle = Button.new()
	_world_toggle.toggle_mode = true
	_world_toggle.text = "World anim"
	_world_toggle.add_theme_font_override("font", MUNRO_FONT)
	_world_toggle.toggled.connect(_set_world_animation)
	footer.add_child(_world_toggle)
	_footer_button(footer, "Close", func(): _hide_lab())

func _footer_button(parent: Node, text: String, action: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.add_theme_font_override("font", MUNRO_FONT)
	button.add_theme_font_size_override("font_size", 13)
	button.pressed.connect(action)
	parent.add_child(button)
	return button

# ---------------------------------------------------------------------------
# Setting rows
# ---------------------------------------------------------------------------

func _add_int_row(parent: Node, key: String, label: String, low: int, high: int, step: int) -> void:
	var row := _row_shell(parent, key, label)
	var slider := HSlider.new()
	slider.min_value = low
	slider.max_value = high
	slider.step = step
	slider.value_changed.connect(func(value: float):
		settings[key] = int(roundf(value))
		_show_value(key, "%d" % int(roundf(value)))
		_mark_dirty())
	row.add_child(_slider_stack(slider, key))
	_sliders[key] = slider

func _add_float_row(parent: Node, key: String, label: String, low: float, high: float, step: float) -> void:
	var row := _row_shell(parent, key, label)
	var slider := HSlider.new()
	slider.min_value = low
	slider.max_value = high
	slider.step = step
	slider.value_changed.connect(func(value: float):
		settings[key] = value
		_show_value(key, "%.3f" % value)
		_mark_dirty())
	row.add_child(_slider_stack(slider, key))
	_sliders[key] = slider

func _add_check_row(parent: Node, key: String, label: String) -> void:
	# A toggle button, not a CheckButton: the house art gives it the same 40-px
	# row as every other setting, and a switch reads as ON when its bevel is
	# held down.
	var box := Button.new()
	box.text = label
	box.toggle_mode = true
	box.alignment = HORIZONTAL_ALIGNMENT_LEFT
	# As wide as its own name: a switch stretched to the column's width reads as
	# a bar, not as something to press.
	box.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	box.add_theme_font_override("font", MUNRO_FONT)
	box.add_theme_font_size_override("font_size", 13)
	box.toggled.connect(func(pressed: bool):
		settings[key] = pressed
		_mark_dirty())
	parent.add_child(box)
	_checks[key] = box

func _row_shell(parent: Node, _key: String, label: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	parent.add_child(row)
	var name_label := _label(label, 13)
	# Flush left, in line with the section headers above it, and one fixed width
	# so every slider starts at the same x. The number is not a column of its
	# own any more: it is drawn on the track (see _slider_stack).
	name_label.custom_minimum_size = Vector2(ROW_LABEL_W, 0)
	name_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_LEFT
	row.add_child(name_label)
	return row

## The slider with its value drawn on the track, the way the makers' NOISE row
## does it: a number in a column of its own was most of the gap a row carried.
func _slider_stack(slider: HSlider, key: String) -> Control:
	var box := Control.new()
	box.custom_minimum_size = Vector2(0, 20)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.set_anchors_preset(Control.PRESET_FULL_RECT)
	box.add_child(slider)
	var value_label := _label("", 12, Color(0.98, 0.98, 0.8))
	value_label.set_anchors_preset(Control.PRESET_FULL_RECT)
	value_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	value_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	box.add_child(value_label)
	_value_labels[key] = value_label
	return box

func _show_value(key: String, text: String) -> void:
	if _value_labels.has(key):
		_value_labels[key].text = text

# ---------------------------------------------------------------------------
# Generation + previews
# ---------------------------------------------------------------------------

func _apply_style(style_name: String) -> void:
	var resolution := int(settings.get("resolution", 16))
	settings = LiquidTextureGen.default_settings(style_name, resolution)
	# Keep the authoring conveniences that are not style properties.
	_refresh_widgets()
	_dirty_delay = 0.0
	_regenerate()

func _mark_dirty() -> void:
	_dirty = true
	_dirty_delay = 0.12  # debounce: dragging a slider should not regenerate per pixel

func _commit_seed() -> void:
	if _seed_edit == null:
		return
	var text := _seed_edit.text.strip_edges()
	if text.is_valid_int():
		settings["seed"] = absi(text.to_int())
		_mark_dirty()
	else:
		_refresh_widgets()

func _set_ramp_stop(index: int, color: Color) -> void:
	var ramp: PackedFloat32Array = settings.get("ramp", PackedFloat32Array())
	if ramp.size() < 16:
		ramp.resize(16)
	ramp[index * 4 + 0] = color.r
	ramp[index * 4 + 1] = color.g
	ramp[index * 4 + 2] = color.b
	ramp[index * 4 + 3] = color.a
	settings["ramp"] = ramp
	_mark_dirty()

func _regenerate() -> void:
	_dirty = false
	var started := Time.get_ticks_usec()
	# The C++ side wants a PackedFloat32Array; JSON hands the ramp back as a
	# plain Array, so normalize every time instead of trusting the dictionary.
	var request := settings.duplicate()
	request["ramp"] = PackedFloat32Array(request.get("ramp", PackedFloat32Array()))
	_strip = LiquidTextureGen.generate_strip(request)
	_last_ms = float(Time.get_ticks_usec() - started) / 1000.0
	if _strip == null or _strip.get_width() <= 0:
		_status.text = "generation failed"
		return
	_slice_frames()
	_frame = 0
	_described = LiquidTextureGen.describe(request)
	_looped = bool(_described.get("looped", false))
	_measure_flatness()
	_update_sheet()
	_update_thumbs()
	_update_previews()
	_report_status()

func _slice_frames() -> void:
	_frames.clear()
	var size := _strip.get_width()
	var count := int(_strip.get_height() / float(size))
	for index in count:
		_frames.append(_strip.get_region(Rect2i(0, index * size, size, size)))

# Fraction of pixels sitting exactly on the bottom/top ramp colour: a high
# number means the field is pinned against a ramp end, i.e. the liquid is being
# clipped into a flat colour. This is the number to watch while tuning the ramp
# stops against the field scale.
func _measure_flatness() -> void:
	_flat_low = 0.0
	_flat_high = 0.0
	var stops := int(settings.get("ramp_stops", 2))
	var ramp: PackedFloat32Array = settings.get("ramp", PackedFloat32Array())
	if _strip == null or ramp.size() < stops * 4:
		return
	# The ramp endpoints are the two colours a clipped field lands on. Bytes, by
	# get_data(), not get_pixel(): this runs after every regeneration and a
	# 256x256x64 strip is four million pixels.
	var low := _ramp_byte(ramp, 0)
	var last := (stops - 1) * 4
	var high := _ramp_byte(ramp, last)
	var data := _strip.get_data()
	var cells := int(data.size() / 4.0)
	var stride := maxi(1, int(cells / 4096.0))
	var total := 0
	var at_low := 0
	var at_high := 0
	var index := 0
	while index < cells:
		var offset := index * 4
		total += 1
		if data[offset] == low[0] and data[offset + 1] == low[1] and data[offset + 2] == low[2]:
			at_low += 1
		elif data[offset] == high[0] and data[offset + 1] == high[1] and data[offset + 2] == high[2]:
			at_high += 1
		index += stride
	if total == 0:
		return
	_flat_low = float(at_low) / float(total)
	_flat_high = float(at_high) / float(total)

# One ramp stop as the RGBA bytes the generator writes (posterize included, so
# the comparison is against what is actually on screen).
func _ramp_byte(ramp: PackedFloat32Array, offset: int) -> Array:
	var levels := int(settings.get("posterize", 0))
	var out := []
	for channel in 4:
		var value := clampf(ramp[offset + channel], 0.0, 1.0)
		if levels > 1 and channel < 3:
			value = roundf(value * float(levels - 1)) / float(levels - 1)
		out.append(int(roundf(value * 255.0)))
	return out

func _update_previews() -> void:
	if _frames.is_empty():
		return
	var frame := _frames[clampi(_frame, 0, _frames.size() - 1)]
	# Reuse the GPU textures: the live preview is refilled every tick.
	_live_tex = _swap_texture(_live_tex, frame)
	_live_preview.texture = _live_tex
	# The 3x3 seam check only changes when the frame does — nine blits a tick at
	# 256x256 for a still image is pure waste.
	if _tiled_frame != _frame:
		_tiled_frame = _frame
		_tiled_tex = _swap_texture(_tiled_tex, _tile_image(frame, 3))
		_tiled_preview.texture = _tiled_tex

func _update_sheet() -> void:
	_sheet_tex = _swap_texture(_sheet_tex, _strip)
	_sheet_preview.texture = _sheet_tex

func _update_thumbs() -> void:
	for index in _thumbs.size():
		var rect := _thumbs[index]
		if index < _frames.size():
			var texture: ImageTexture = rect.texture
			rect.texture = _swap_texture(texture, _frames[index])
		else:
			rect.texture = null

# Updates an existing ImageTexture in place when the shape still matches (no new
# GPU allocation), and replaces it when it does not.
func _swap_texture(texture: ImageTexture, image: Image) -> ImageTexture:
	if texture == null or texture.get_width() != image.get_width() or texture.get_height() != image.get_height():
		return ImageTexture.create_from_image(image)
	texture.update(image)
	return texture

func _tile_image(image: Image, times: int) -> Image:
	var width := image.get_width()
	var height := image.get_height()
	var out := Image.create(width * times, height * times, false, Image.FORMAT_RGBA8)
	for y in times:
		for x in times:
			out.blit_rect(image, Rect2i(0, 0, width, height), Vector2i(x * width, y * height))
	return out

func _report_status() -> void:
	var described: Dictionary = _described if not _described.is_empty() else LiquidTextureGen.describe(settings)
	_status.text = ("%s  %dx%d  %d frames (strip %dx%d)%s  %0.1f ms\nflat top %0.1f%%   flat bottom %0.1f%%" % [
		settings.get("style", "?"),
		int(described.get("resolution", 0)), int(described.get("resolution", 0)),
		int(described.get("strip_frames", 0)),
		int(described.get("strip_width", 0)), int(described.get("strip_height", 0)),
		"  loop" if _looped else "  no loop",
		_last_ms, _flat_high * 100.0, _flat_low * 100.0])

# ---------------------------------------------------------------------------
# Live application to the world
# ---------------------------------------------------------------------------

# The animator is an autoload, but this panel is also built by probes that never
# start a game, so every call goes through here and tolerates its absence.
func _animator() -> Node:
	return get_node_or_null("/root/LiquidAnimator")

# Live writes frames into one layer at a time; the animator would be writing to
# the same layer, so hand it over for as long as Live owns that liquid.
func _set_live_owner(target: String) -> void:
	var animator := _animator()
	if _live_owner != "" and _live_owner != target:
		if animator != null:
			animator.call("resume", _live_owner)
		_live_owner = ""
	if target != "":
		if animator != null:
			animator.call("pause", target)
		_live_owner = target
	_report_world()

func _live_target() -> String:
	if _target_pick == null:
		return "water"
	return TARGETS[clampi(_target_pick.selected, 0, TARGETS.size() - 1)]

func _set_live(enabled: bool) -> void:
	_live = enabled
	_set_live_owner(_live_target() if enabled else "")
	var manager := _chunk_manager()
	if manager == null:
		_live = false
		_live_toggle.set_pressed_no_signal(false)
		_live_label.text = "live: no world loaded"
		return
	if not enabled:
		var target := _live_target()
		var restored: bool = manager.restore_texture_layer(target)
		_restore_compression()
		if restored:
			_live_label.text = "live: %s restored from disk" % target
		else:
			_live_label.text = "live: off (%s had no layer to restore)" % target
		return
	if not _prepare_live_layer():
		_live = false
		_live_toggle.set_pressed_no_signal(false)
		return
	_push_frame()
	_after_live_prepare()

# A GPU-compressed layer (texture compression in the settings) cannot take a
# raw RGBA frame, and nothing on the CPU can produce a matching DXT chain for
# it. The array rebuilds from the same 29 small PNGs in a few milliseconds, so
# live previewing simply flips compression off for its duration and puts the
# user's setting back when it stops.
func _prepare_live_layer() -> bool:
	var manager := _chunk_manager()
	if manager == null:
		return false
	var target := _live_target()
	var info: Dictionary = manager.get_texture_layer_info(target)
	if not bool(info.get("found", false)):
		_report_live_target()
		return false
	if bool(info.get("writable", false)):
		return true
	if not _compression_flipped:
		_compression_was = bool(manager.get_compression_enabled())
		manager.set_compression_enabled(false)
		_compression_flipped = true
	info = manager.get_texture_layer_info(target)
	return bool(info.get("writable", false))

func _restore_compression() -> void:
	if not _compression_flipped:
		return
	var manager := _chunk_manager()
	_compression_flipped = false
	if manager != null and bool(manager.get_compression_enabled()) != _compression_was:
		manager.set_compression_enabled(_compression_was)

func _after_live_prepare() -> void:
	_report_live_target()
	if _compression_flipped:
		_live_label.text += "   [compression off for the preview; restored when Live is off]"

func _push_frame() -> void:
	if _live and not _frames.is_empty():
		var manager := _chunk_manager()
		if manager != null:
			manager.push_texture_frame(_live_target(), _frames[_frame % _frames.size()])

# ---------------------------------------------------------------------------
# World animation (LiquidAnimator)
# ---------------------------------------------------------------------------

# Binding saves the current strip under the LIQUID's own name, which is the file
# the animator loads for that liquid — so what animates in the world from now on
# is exactly what the panel is showing, and it survives a restart.
func _bind_to_world() -> void:
	if _strip == null:
		return
	var target := _live_target()
	DirAccess.make_dir_recursive_absolute(SAVE_DIR)
	var base := SAVE_DIR + "/" + target
	var error := _strip.save_png(base + ".png")
	if error != OK:
		_live_label.text = "bind failed: %s" % error
		return
	var file := FileAccess.open(base + ".json", FileAccess.WRITE)
	if file == null:
		_live_label.text = "bind failed: settings file"
		return
	file.store_string(JSON.stringify(settings, "  "))
	file.close()
	if _name_edit:
		_name_edit.text = target
	var animator := _animator()
	if animator != null:
		animator.call("reload", target)
	_refresh_load_list(target)
	_live_label.text = "bound %s -> the world animator (user://liquids/%s.json)" % [target, target]
	_report_world()

func _set_world_animation(value: bool) -> void:
	var animator := _animator()
	if animator != null:
		animator.call("set_enabled", value)
	_report_world()

func _report_world() -> void:
	if _world_label == null:
		return
	var animator := _animator()
	if animator == null:
		_world_label.text = "world: animator not running"
		return
	var status: Dictionary = animator.call("get_status")
	var target := _live_target()
	var entry: Dictionary = status.get(target, {})
	var running := bool(status.get("enabled", false))
	if entry.is_empty() or not running:
		_world_label.text = "world: %s paused (animator off)" % target
		return
	var source := String(entry.get("source", ""))
	if not bool(entry.get("animating", false)):
		_world_label.text = "world: %s idle (%s)" % [target, String(entry.get("note", "?"))]
		return
	_world_label.text = "world: %s <- %s, %d frames, frame %d/%d%s" % [
		target, "preset" if source == "preset" else source,
		int(entry.get("frames", 0)), int(entry.get("frame", 0)) + 1, int(entry.get("frames", 0)),
		"" if bool(entry.get("paused", false)) == false else "  [Live owns it]"]

func _report_live_target() -> void:
	var manager := _chunk_manager()
	if manager == null:
		_live_label.text = "live: no world loaded"
		return
	var target := _live_target()
	var info: Dictionary = manager.get_texture_layer_info(target)
	if not bool(info.get("found", false)):
		_live_label.text = "live: no layer named \"%s\" (no block uses Assets/textures/%s.png)" % [target, target]
		return
	_live_label.text = "live: %s -> layer %d of %d (%dx%d%s) frame %d/%d%s" % [
		target, int(info.get("index", -1)), int(info.get("layers", 0)),
		int(info.get("width", 0)), int(info.get("height", 0)),
		", mipmaps" if bool(info.get("mipmaps", false)) else "",
		_frame + 1, maxi(1, _frames.size()),
		"" if bool(info.get("writable", false)) else "  [READ-ONLY: layer is GPU-compressed]"]

# ---------------------------------------------------------------------------
# Save / load
# ---------------------------------------------------------------------------

func _save() -> void:
	if _strip == null:
		return
	DirAccess.make_dir_recursive_absolute(SAVE_DIR)
	var strip_name := _name_edit.text.strip_edges() if _name_edit else "water_anim"
	if strip_name.is_empty():
		strip_name = "water_anim"
	var base := SAVE_DIR + "/" + strip_name
	var error := _strip.save_png(base + ".png")
	if error != OK:
		_live_label.text = "save failed: %s" % error
		return
	var file := FileAccess.open(base + ".json", FileAccess.WRITE)
	if file == null:
		_live_label.text = "save failed: settings file"
		return
	file.store_string(JSON.stringify(settings, "  "))
	file.close()
	_live_label.text = "saved %s.png + .json  (strip %dx%d)" % [strip_name, _strip.get_width(), _strip.get_height()]
	_refresh_load_list()

func _load_selected() -> void:
	if _load_pick == null or _load_pick.item_count == 0:
		return
	var strip_name := _load_pick.get_item_text(_load_pick.selected)
	var file := FileAccess.open(SAVE_DIR + "/" + strip_name + ".json", FileAccess.READ)
	if file == null:
		_live_label.text = "load failed: %s.json" % strip_name
		return
	var parsed = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(parsed) != TYPE_DICTIONARY:
		_live_label.text = "load failed: %s.json is not a settings dictionary" % strip_name
		return
	settings = parsed
	settings["ramp"] = PackedFloat32Array(settings.get("ramp", PackedFloat32Array()))
	if _name_edit:
		_name_edit.text = strip_name
	_refresh_widgets()
	_dirty_delay = 0.0
	_regenerate()
	_live_label.text = "loaded %s" % strip_name

func _delete() -> void:
	if _load_pick == null or _load_pick.item_count == 0:
		return
	var strip_name := _load_pick.get_item_text(_load_pick.selected)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + strip_name + ".png"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + strip_name + ".json"))
	_live_label.text = "deleted %s" % strip_name
	_refresh_load_list()

func _refresh_load_list(prefer_name: String = "") -> void:
	if _load_pick == null:
		return
	_load_pick.clear()
	var directory := DirAccess.open(SAVE_DIR)
	if directory == null:
		return
	for file_name in directory.get_files():
		if file_name.ends_with(".json"):
			_load_pick.add_item(file_name.get_basename())
	# Land on the strip the name box is showing, if it is one of them, so opening
	# the panel does not silently point the Load button at a different file.
	for index in _load_pick.item_count:
		if _load_pick.get_item_text(index) == prefer_name:
			_load_pick.selected = index
			return

# ---------------------------------------------------------------------------
# Widget sync
# ---------------------------------------------------------------------------

func _refresh_widgets() -> void:
	_refreshing = true
	for row in INT_ROWS:
		_refresh_slider(row[0], true)
	for row in FLOAT_ROWS:
		_refresh_slider(row[0], false)
	for row in CHECK_ROWS + LOOP_CHECKS:
		if _checks.has(row[0]):
			_checks[row[0]].set_pressed_no_signal(bool(settings.get(row[0], false)))
	if _kernel_pick:
		var kernel := String(settings.get("kernel", "row"))
		_kernel_pick.selected = maxi(0, KERNELS.find(kernel))
	if _seed_edit:
		_seed_edit.text = str(int(settings.get("seed", 0)))
	if _preset_button:
		_preset_button.text = String(settings.get("style", "water")).capitalize()
	var ramp: PackedFloat32Array = settings.get("ramp", PackedFloat32Array())
	var stops := int(settings.get("ramp_stops", 2))
	for index in _ramp_buttons.size():
		var picker := _ramp_buttons[index]
		if ramp.size() >= (index + 1) * 4:
			picker.color = Color(ramp[index * 4], ramp[index * 4 + 1], ramp[index * 4 + 2], ramp[index * 4 + 3])
		picker.visible = index < stops
	if _world_toggle:
		var animator := _animator()
		_world_toggle.set_pressed_no_signal(bool(animator.call("is_enabled")) if animator != null else false)
	_refreshing = false
	_report_live_target()
	_report_world()

func _refresh_slider(key: String, is_int: bool) -> void:
	if not _sliders.has(key):
		return
	var slider: HSlider = _sliders[key]
	var value := float(settings.get(key, slider.min_value))
	slider.set_value_no_signal(clampf(value, slider.min_value, slider.max_value))
	if is_int:
		_show_value(key, "%d" % int(roundf(value)))
	else:
		_show_value(key, "%.3f" % value)

func _toggle_play() -> void:
	_playing = not _playing
	_refresh_play_button()

func _refresh_play_button() -> void:
	if _play_button:
		_play_button.text = "Pause" if _playing else "Play"
