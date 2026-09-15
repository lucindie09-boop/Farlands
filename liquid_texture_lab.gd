extends Node
## Liquid Texture Lab — press the bound action (liquid_lab_toggle, default O).
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
## Two ways out of the tool:
##   Save      writes user://liquids/<name>.png (the vertical strip) plus a
##             <name>.json sidecar holding every setting, and Load restores them
##             exactly, so a texture can be reproduced and re-tuned later.
##   Live      pushes the current frame into the world's texture array layer for
##             the chosen liquid (water exists today; lava/acid need a block that
##             uses textures/lava.png or textures/acid.png). That is
##             ChunkManager.push_texture_frame -> Texture2DArray.update_layer, so
##             the world animates without rebuilding the array; the layer is
##             restored from disk when Live is switched off.
##
## Legal note that also applies here: only the *approach* (a three-field
## automaton mapped through a colour ramp, and the classic 16x16 liquid look) is
## borrowed from how the classic block game generated these sprites; the field
## names, defaults, kernels and colours in this project are our own.

const ACTION := "liquid_lab_toggle"
const SAVE_DIR := "user://liquids"
const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
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
const KERNELS := ["row", "box", "warp", "plus"]
const TARGETS := ["water", "lava", "acid"]
const THUMBNAILS := 8

var settings: Dictionary = {}

var _open := false
var _layer: CanvasLayer = null
var _panel: Control = null
var _status: Label = null
var _name_edit: LineEdit = null
var _load_pick: OptionButton = null
var _live_toggle: CheckButton = null
var _play_button: Button = null
var _target_pick: OptionButton = null
var _live_label: Label = null

var _live_preview: TextureRect = null
var _sheet_preview: TextureRect = null
var _tiled_preview: TextureRect = null
var _thumbs: Array[TextureRect] = []
var _ramp_buttons: Array[ColorPickerButton] = []
var _sliders: Dictionary = {}     # key -> HSlider
var _value_labels: Dictionary = {}  # key -> Label
var _checks: Dictionary = {}      # key -> CheckButton
var _kernel_pick: OptionButton = null
var _seed_edit: LineEdit = null

var _strip: Image = null
var _frames: Array[Image] = []
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
var _compression_flipped := false
var _compression_was := false
var _last_ms := 0.0
var _endpoint_colors: Array[Color] = []
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

func _unhandled_input(event: InputEvent) -> void:
	if not event.is_action_pressed(ACTION) or event.is_echo():
		return
	if not _open:
		# Gameplay only, like the other debug keys: not while chat or a menu has
		# the keyboard.
		if Input.mouse_mode != Input.MOUSE_MODE_CAPTURED:
			return
		_show_lab()
	else:
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
			_frame = (_frame + 1) % _frames.size()
			guard += 1
	if _open:
		_update_previews()
		if _live:
			_report_live_target()
	if _live:
		_push_frame()

# ---------------------------------------------------------------------------
# Panel
# ---------------------------------------------------------------------------

func _show_lab() -> void:
	_ensure_settings()
	if _layer == null:
		_build_ui()
	_refresh_widgets()
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

func _player() -> Node:
	return get_node_or_null("/root/Main/Player")

func _chunk_manager() -> Node:
	return get_node_or_null("/root/Main/ChunkManager")

func _build_ui() -> void:
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

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	margin.add_child(column)

	var title := Label.new()
	title.text = "LIQUID TEXTURE LAB   —   %s to close" % _action_key_label()
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

	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", 6)
	column.add_child(footer)
	_build_footer(footer)

func _action_key_label() -> String:
	if InputMap.has_action(ACTION):
		for event in InputMap.action_get_events(ACTION):
			if event is InputEventKey:
				return OS.get_keycode_string(event.keycode)
	return "?"

func _make_panel(color: Color) -> PanelContainer:
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = color
	style.set_corner_radius_all(4)
	style.set_content_margin_all(8)
	panel.add_theme_stylebox_override("panel", style)
	return panel

func _label(text: String, size: int = 13, color: Color = Color(0.85, 0.87, 0.9)) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", size)
	label.add_theme_color_override("font_color", color)
	return label

func _section(parent: Node, text: String) -> void:
	var head := _label(text, 14, Color(0.6, 0.85, 1.0))
	parent.add_child(head)

func _build_settings_column() -> Control:
	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(392, 0)
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED

	var page := VBoxContainer.new()
	page.add_theme_constant_override("separation", 4)
	page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(page)

	# Preset + target -------------------------------------------------------
	_section(page, "STYLE")
	var preset_row := HBoxContainer.new()
	preset_row.add_theme_constant_override("separation", 6)
	page.add_child(preset_row)
	for style_name in ["water", "lava", "acid"]:
		var button := Button.new()
		button.text = style_name.capitalize()
		button.add_theme_font_override("font", MUNRO_FONT)
		button.add_theme_font_size_override("font_size", 13)
		button.pressed.connect(func(): _apply_style(style_name))
		preset_row.add_child(button)

	var seed_row := HBoxContainer.new()
	seed_row.add_theme_constant_override("separation", 6)
	page.add_child(seed_row)
	seed_row.add_child(_label("Seed"))
	_seed_edit = LineEdit.new()
	_seed_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_seed_edit.add_theme_font_override("font", MUNRO_FONT)
	_seed_edit.text_submitted.connect(func(_t): _commit_seed())
	_seed_edit.focus_exited.connect(_commit_seed)
	seed_row.add_child(_seed_edit)
	var roll := Button.new()
	roll.text = "Roll"
	roll.add_theme_font_override("font", MUNRO_FONT)
	roll.add_theme_font_size_override("font_size", 13)
	roll.pressed.connect(func():
		settings["seed"] = randi() & 0x7fffffff
		_refresh_widgets()
		_mark_dirty())
	seed_row.add_child(roll)

	# Shape -----------------------------------------------------------------
	_section(page, "SHAPE")
	for row in INT_ROWS:
		_add_int_row(page, row[0], row[1], row[2], row[3], row[4])

	# Physics ---------------------------------------------------------------
	_section(page, "AUTOMATON")
	var kernel_row := HBoxContainer.new()
	kernel_row.add_theme_constant_override("separation", 6)
	page.add_child(kernel_row)
	kernel_row.add_child(_label("Kernel"))
	_kernel_pick = OptionButton.new()
	_kernel_pick.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_kernel_pick.add_theme_font_override("font", MUNRO_FONT)
	for name in KERNELS:
		_kernel_pick.add_item(name)
	_kernel_pick.item_selected.connect(func(index: int):
		settings["kernel"] = KERNELS[index]
		_mark_dirty())
	kernel_row.add_child(_kernel_pick)
	for row in FLOAT_ROWS:
		_add_float_row(page, row[0], row[1], row[2], row[3], row[4])
	for row in CHECK_ROWS:
		_add_check_row(page, row[0], row[1])

	# Ramp ------------------------------------------------------------------
	_section(page, "RAMP")
	var stops_row := HBoxContainer.new()
	stops_row.add_theme_constant_override("separation", 6)
	page.add_child(stops_row)
	stops_row.add_child(_label("Stops"))
	for index in 4:
		var picker := ColorPickerButton.new()
		picker.custom_minimum_size = Vector2(34, 24)
		picker.edit_alpha = true
		picker.color_changed.connect(func(color: Color): _set_ramp_stop(index, color))
		_ramp_buttons.append(picker)
		stops_row.add_child(picker)
	_add_int_row(page, "ramp_stops", "Used stops", 2, 4, 1)
	_add_int_row(page, "posterize", "Posterize (0 = off)", 0, 16, 1)

	return scroll

func _build_preview_column() -> Control:
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 8)
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var live_panel := _make_panel(Color(0.05, 0.06, 0.08, 0.9))
	var live_box := VBoxContainer.new()
	live_panel.add_child(live_box)
	var live_row := HBoxContainer.new()
	live_row.add_theme_constant_override("separation", 8)
	live_box.add_child(live_row)

	_live_preview = _preview_rect(224)
	live_row.add_child(_preview_box("LIVE", _live_preview))
	_tiled_preview = _preview_rect(224)
	live_row.add_child(_preview_box("TILED 3x3 (seam check)", _tiled_preview))

	var sheet_row := HBoxContainer.new()
	sheet_row.add_theme_constant_override("separation", 8)
	live_box.add_child(sheet_row)
	_sheet_preview = _preview_rect(224)
	sheet_row.add_child(_preview_box("SHEET (what gets saved)", _sheet_preview))
	var thumbs_box := VBoxContainer.new()
	thumbs_box.add_theme_constant_override("separation", 4)
	sheet_row.add_child(thumbs_box)
	thumbs_box.add_child(_label("FRAMES 0-7"))
	var grid := GridContainer.new()
	grid.columns = 4
	grid.add_theme_constant_override("h_separation", 4)
	grid.add_theme_constant_override("v_separation", 4)
	thumbs_box.add_child(grid)
	for index in THUMBNAILS:
		var thumb := TextureRect.new()
		thumb.custom_minimum_size = Vector2(48, 48)
		thumb.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		thumb.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
		grid.add_child(thumb)
		_thumbs.append(thumb)
	column.add_child(live_panel)

	_status = _label("", 13, Color(0.8, 0.9, 0.8))
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	column.add_child(_status)
	_live_label = _label("", 13, Color(0.95, 0.85, 0.6))
	column.add_child(_live_label)
	return column

func _preview_rect(size: int) -> TextureRect:
	var rect := TextureRect.new()
	rect.custom_minimum_size = Vector2(size, size)
	rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	return rect

func _preview_box(caption: String, rect: TextureRect) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	box.add_child(_label(caption, 12, Color(0.6, 0.75, 0.9)))
	box.add_child(rect)
	return box

func _build_footer(footer: HBoxContainer) -> void:
	_play_button = _footer_button(footer, "Pause", func(): _toggle_play())
	_footer_button(footer, "Step", func():
		if _frames.is_empty():
			return
		_playing = false
		_frame = (_frame + 1) % _frames.size()
		_update_previews()
		_refresh_play_button())
	_footer_button(footer, "Regenerate", func():
		_dirty_delay = 0.0
		_regenerate())
	_footer_button(footer, "Reset style", func():
		_apply_style(String(settings.get("style", "water"))))

	footer.add_child(_label("  Name"))
	_name_edit = LineEdit.new()
	_name_edit.custom_minimum_size = Vector2(140, 0)
	_name_edit.add_theme_font_override("font", MUNRO_FONT)
	_name_edit.text = "water_anim"
	footer.add_child(_name_edit)
	_footer_button(footer, "Save", func(): _save())
	_footer_button(footer, "Delete", func(): _delete())

	_load_pick = OptionButton.new()
	_load_pick.add_theme_font_override("font", MUNRO_FONT)
	_load_pick.custom_minimum_size = Vector2(150, 0)
	footer.add_child(_load_pick)
	_footer_button(footer, "Load", func(): _load_selected())

	footer.add_child(_label("  Live onto"))
	_target_pick = OptionButton.new()
	_target_pick.add_theme_font_override("font", MUNRO_FONT)
	for target in TARGETS:
		_target_pick.add_item(target)
	_target_pick.item_selected.connect(func(_index: int): _report_live_target())
	footer.add_child(_target_pick)
	_live_toggle = CheckButton.new()
	_live_toggle.text = "Live"
	_live_toggle.add_theme_font_override("font", MUNRO_FONT)
	_live_toggle.toggled.connect(_set_live)
	footer.add_child(_live_toggle)
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
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size = Vector2(150, 0)
	slider.value_changed.connect(func(value: float):
		settings[key] = int(roundf(value))
		_show_value(key, "%d" % int(roundf(value)))
		_mark_dirty())
	row.add_child(slider)
	_sliders[key] = slider

func _add_float_row(parent: Node, key: String, label: String, low: float, high: float, step: float) -> void:
	var row := _row_shell(parent, key, label)
	var slider := HSlider.new()
	slider.min_value = low
	slider.max_value = high
	slider.step = step
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size = Vector2(150, 0)
	slider.value_changed.connect(func(value: float):
		settings[key] = value
		_show_value(key, "%.3f" % value)
		_mark_dirty())
	row.add_child(slider)
	_sliders[key] = slider

func _add_check_row(parent: Node, key: String, label: String) -> void:
	var box := CheckButton.new()
	box.text = label
	box.add_theme_font_override("font", MUNRO_FONT)
	box.add_theme_font_size_override("font_size", 13)
	box.toggled.connect(func(pressed: bool):
		settings[key] = pressed
		_mark_dirty())
	parent.add_child(box)
	_checks[key] = box

func _row_shell(parent: Node, key: String, label: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	parent.add_child(row)
	var name_label := _label(label)
	name_label.custom_minimum_size = Vector2(150, 0)
	row.add_child(name_label)
	var value_label := _label("", 12, Color(0.95, 0.95, 0.7))
	value_label.custom_minimum_size = Vector2(56, 0)
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_value_labels[key] = value_label
	row.add_child(value_label)
	return row

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
	_measure_flatness()
	_update_sheet()
	_update_thumbs()
	_update_previews()
	_report_status()

func _slice_frames() -> void:
	_frames.clear()
	var size := _strip.get_width()
	var count := int(_strip.get_height() / size)
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
	var cells := int(data.size() / 4)
	var stride := maxi(1, int(cells / 4096))
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
	var recorded: Dictionary = LiquidTextureGen.describe(settings)
	_status.text = ("%s  %dx%d  %d frames (strip %dx%d)  %0.1f ms\nflat top %0.1f%%   flat bottom %0.1f%%" % [
		settings.get("style", "?"),
		int(recorded.get("resolution", 0)), int(recorded.get("resolution", 0)),
		int(recorded.get("strip_frames", 0)),
		int(recorded.get("strip_width", 0)), int(recorded.get("strip_height", 0)),
		_last_ms, _flat_high * 100.0, _flat_low * 100.0])

# ---------------------------------------------------------------------------
# Live application to the world
# ---------------------------------------------------------------------------

func _live_target() -> String:
	if _target_pick == null:
		return "water"
	return TARGETS[clampi(_target_pick.selected, 0, TARGETS.size() - 1)]

func _set_live(enabled: bool) -> void:
	_live = enabled
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

func _report_live_target() -> void:
	var manager := _chunk_manager()
	if manager == null:
		_live_label.text = "live: no world loaded"
		return
	var target := _live_target()
	var info: Dictionary = manager.get_texture_layer_info(target)
	if not bool(info.get("found", false)):
		_live_label.text = "live: no layer named \"%s\" (no block uses textures/%s.png)" % [target, target]
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
	var name := _name_edit.text.strip_edges() if _name_edit else "water_anim"
	if name.is_empty():
		name = "water_anim"
	var base := SAVE_DIR + "/" + name
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
	_live_label.text = "saved %s.png + .json  (strip %dx%d)" % [name, _strip.get_width(), _strip.get_height()]
	_refresh_load_list()

func _load_selected() -> void:
	if _load_pick == null or _load_pick.item_count == 0:
		return
	var name := _load_pick.get_item_text(_load_pick.selected)
	var file := FileAccess.open(SAVE_DIR + "/" + name + ".json", FileAccess.READ)
	if file == null:
		_live_label.text = "load failed: %s.json" % name
		return
	var parsed = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(parsed) != TYPE_DICTIONARY:
		_live_label.text = "load failed: %s.json is not a settings dictionary" % name
		return
	settings = parsed
	settings["ramp"] = PackedFloat32Array(settings.get("ramp", PackedFloat32Array()))
	if _name_edit:
		_name_edit.text = name
	_refresh_widgets()
	_dirty_delay = 0.0
	_regenerate()
	_live_label.text = "loaded %s" % name

func _delete() -> void:
	if _load_pick == null or _load_pick.item_count == 0:
		return
	var name := _load_pick.get_item_text(_load_pick.selected)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + name + ".png"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + name + ".json"))
	_live_label.text = "deleted %s" % name
	_refresh_load_list()

func _refresh_load_list() -> void:
	if _load_pick == null:
		return
	_load_pick.clear()
	var directory := DirAccess.open(SAVE_DIR)
	if directory == null:
		return
	for file_name in directory.get_files():
		if file_name.ends_with(".json"):
			_load_pick.add_item(file_name.get_basename())

# ---------------------------------------------------------------------------
# Widget sync
# ---------------------------------------------------------------------------

func _refresh_widgets() -> void:
	_refreshing = true
	for row in INT_ROWS:
		_refresh_slider(row[0], true)
	for row in FLOAT_ROWS:
		_refresh_slider(row[0], false)
	for row in CHECK_ROWS:
		if _checks.has(row[0]):
			_checks[row[0]].set_pressed_no_signal(bool(settings.get(row[0], false)))
	if _kernel_pick:
		var kernel := String(settings.get("kernel", "row"))
		_kernel_pick.selected = maxi(0, KERNELS.find(kernel))
	if _seed_edit:
		_seed_edit.text = str(int(settings.get("seed", 0)))
	var ramp: PackedFloat32Array = settings.get("ramp", PackedFloat32Array())
	var stops := int(settings.get("ramp_stops", 2))
	for index in _ramp_buttons.size():
		var picker := _ramp_buttons[index]
		if ramp.size() >= (index + 1) * 4:
			picker.color = Color(ramp[index * 4], ramp[index * 4 + 1], ramp[index * 4 + 2], ramp[index * 4 + 3])
		picker.visible = index < stops
	_refreshing = false
	_report_live_target()

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
