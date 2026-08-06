extends Control

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")
const SETTINGS_PATH := "user://settings.cfg"

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")
@onready var crosshair_node = get_node_or_null("/root/Main/HUD/Crosshair")

var is_open = false
var _current_page: String = "pause"

var _bg: ColorRect
var _pages: Dictionary = {}

var _default_gui_scale: float = 3.0
var _default_day_duration: float = 10.0
var _default_day_sky: Color = Color(1, 1, 1, 1)
var _default_night_sky: Color = Color(0, 0, 0, 1)
var _default_render_distance: int = 32

var _crosshair_defaults := {
	"cross_enabled": true,
	"cross_length": 9.0,
	"cross_thickness": 2.0,
	"cross_opacity": 1.0,
	"cross_spacing": 0.0,
	"top_line_enabled": true,
	"cross_color": Color.WHITE,
	"cross_rotation": 0.0,
	"cross_contrast": true,
	"dot_enabled": false,
	"dot_size": 3.0,
	"dot_opacity": 1.0,
	"dot_color": Color.WHITE,
	"dot_rotation": 0.0,
	"dot_contrast": false,
	"cross_dot_collision": true,
}

var _save_timer: Timer = null

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_bg = ColorRect.new()
	_bg.color = Color(0.02, 0.02, 0.05, 0.5)
	_bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	_bg.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_bg)
	_default_gui_scale = UIScale.value
	_default_day_duration = chunk_manager.get_day_duration()
	_default_day_sky = chunk_manager.get_day_sky_color()
	_default_night_sky = chunk_manager.get_night_sky_color()
	_default_render_distance = chunk_manager.get_render_distance()
	if crosshair_node:
		for k in _crosshair_defaults:
			_crosshair_defaults[k] = crosshair_node.get(k)
	_load_settings()
	hide()
func _exit_tree():
	_save_settings()

func _schedule_save():
	if _save_timer == null:
		_save_timer = Timer.new()
		_save_timer.one_shot = true
		_save_timer.timeout.connect(_save_settings)
		add_child(_save_timer)
	_save_timer.start(0.5)

func _save_settings():
	var cfg := ConfigFile.new()
	cfg.set_value("gui", "scale", UIScale.value)
	if cfg.has_section_key("gui", "crosshair"):
		cfg.erase_section_key("gui", "crosshair")
	for k in _crosshair_defaults:
		cfg.set_value("gui", k, crosshair_node.get(k) if crosshair_node else _crosshair_defaults[k])
	cfg.set_value("lighting", "day_duration", chunk_manager.get_day_duration())
	cfg.set_value("lighting", "day_sky_color", chunk_manager.get_day_sky_color())
	cfg.set_value("lighting", "night_sky_color", chunk_manager.get_night_sky_color())
	cfg.set_value("render", "distance", chunk_manager.get_render_distance())
	cfg.save(SETTINGS_PATH)

func _load_settings():
	if not FileAccess.file_exists(SETTINGS_PATH):
		return
	var cfg := ConfigFile.new()
	if cfg.load(SETTINGS_PATH) != OK:
		return
	UIScale.value = clampf(cfg.get_value("gui", "scale", UIScale.value), 1.0, 4.0)
	for k in _crosshair_defaults:
		if crosshair_node:
			crosshair_node.set(k, cfg.get_value("gui", k, _crosshair_defaults[k]))
	chunk_manager.set_day_duration(cfg.get_value("lighting", "day_duration", chunk_manager.get_day_duration()))
	chunk_manager.set_day_sky_color(cfg.get_value("lighting", "day_sky_color", chunk_manager.get_day_sky_color()))
	chunk_manager.set_night_sky_color(cfg.get_value("lighting", "night_sky_color", chunk_manager.get_night_sky_color()))
	chunk_manager.set_render_distance(int(cfg.get_value("render", "distance", chunk_manager.get_render_distance())))

# Settings menu uses the global GUI scale with a 2/3 modifier so its default
# look (2x when UIScale is 3.0) is preserved while still scaling with the rest.
func _ui_scale() -> float:
	return UIScale.value * 2.0 / 3.0

func _rebuild_pages():
	for p in _pages.values():
		if is_instance_valid(p):
			p.queue_free()
	_pages.clear()
	_pages["pause"] = _build_pause_page()
	_pages["settings"] = _build_settings_page()
	_pages["gui"] = _build_gui_page()
	_pages["crosshair"] = _build_crosshair_page()
	_pages["lighting"] = _build_lighting_page()
	_pages["render"] = _build_render_page()
	for p in _pages.values():
		p.hide()
		add_child(p)

func _show_page(name: String):
	_current_page = name
	for k in _pages:
		_pages[k].visible = (k == name)

func _build_pause_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title := _make_title("PAUSED")
	title.offset_top = -80.0 * s
	title.offset_bottom = -40.0 * s
	page.add_child(title)

	var resume := _make_button("Resume")
	resume.offset_top = -10.0 * s
	resume.offset_bottom = 10.0 * s
	resume.pressed.connect(_close)
	page.add_child(resume)

	var settings := _make_button("Settings")
	settings.offset_top = 30.0 * s
	settings.offset_bottom = 50.0 * s
	settings.pressed.connect(func(): _show_page("settings"))
	page.add_child(settings)
	return page

func _build_settings_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title := _make_title("SETTINGS")
	title.offset_top = -80.0 * s
	title.offset_bottom = -40.0 * s
	page.add_child(title)

	var gui_btn := _make_button("GUI")
	gui_btn.offset_top = -35.0 * s
	gui_btn.offset_bottom = -15.0 * s
	gui_btn.pressed.connect(func(): _show_page("gui"))
	page.add_child(gui_btn)

	var light_btn := _make_button("Lighting")
	light_btn.offset_top = -5.0 * s
	light_btn.offset_bottom = 15.0 * s
	light_btn.pressed.connect(func(): _show_page("lighting"))
	page.add_child(light_btn)

	var render_btn := _make_button("Render")
	render_btn.offset_top = 25.0 * s
	render_btn.offset_bottom = 45.0 * s
	render_btn.pressed.connect(func(): _show_page("render"))
	page.add_child(render_btn)

	var back := _make_button("Back")
	back.offset_top = 55.0 * s
	back.offset_bottom = 75.0 * s
	back.pressed.connect(func(): _show_page("pause"))
	page.add_child(back)
	return page

func _build_gui_page() -> Control:
	var scale_btn := _make_button("", 180.0)
	var scale_values := [1.0, 2.0, 3.0, 4.0]
	scale_btn.text = str(int(round(UIScale.value))) + "x"
	scale_btn.pressed.connect(func():
		var i := scale_values.find(float(int(round(UIScale.value))))
		i = (i + 1) % scale_values.size()
		UIScale.value = scale_values[i]
		scale_btn.text = str(int(scale_values[i])) + "x"
		_schedule_save())
	var reset := func():
		UIScale.value = _default_gui_scale
		scale_btn.text = str(int(round(_default_gui_scale))) + "x"

	var crosshair_btn := _make_button("Crosshair")
	crosshair_btn.pressed.connect(func(): _show_page("crosshair"))

	return _build_option_page("GUI", [
		["GUI Scale", scale_btn, reset],
		["Crosshair", crosshair_btn, null],
	], "settings")

func _build_crosshair_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title := _make_title("CROSSHAIR")
	title.offset_top = -150.0 * s
	title.offset_bottom = -110.0 * s
	page.add_child(title)

	var preview: Control = (preload("res://crosshair_preview.gd") as GDScript).new()
	preview.set_anchors_preset(Control.PRESET_CENTER)
	preview.offset_left = -70.0 * s
	preview.offset_right = 70.0 * s
	preview.offset_top = -3.0 * s
	preview.offset_bottom = 117.0 * s
	page.add_child(preview)

	_crosshair_header(page, 0, -102.0, "CROSS")
	_crosshair_header(page, 1, -102.0, "DOT")

	var controls := {}
	controls["cross_enabled"] = _make_toggle("cross_enabled", _crosshair_val("cross_enabled"))
	controls["cross_color"] = _make_color("cross_color", _crosshair_val("cross_color"))
	controls["cross_length"] = _make_spin(_crosshair_val("cross_length"), 0.0, 40.0, 1.0, "cross_length")
	controls["cross_thickness"] = _make_spin(_crosshair_val("cross_thickness"), 0.0, 10.0, 0.5, "cross_thickness")
	controls["cross_opacity"] = _make_spin(_crosshair_val("cross_opacity"), 0.0, 1.0, 0.05, "cross_opacity")
	controls["cross_spacing"] = _make_spin(_crosshair_val("cross_spacing"), 0.0, 10.0, 0.5, "cross_spacing")
	controls["top_line_enabled"] = _make_toggle("top_line_enabled", _crosshair_val("top_line_enabled"))
	controls["cross_rotation"] = _make_spin(_crosshair_val("cross_rotation"), 0.0, 360.0, 1.0, "cross_rotation")
	controls["cross_contrast"] = _make_toggle("cross_contrast", _crosshair_val("cross_contrast"))
	controls["dot_enabled"] = _make_toggle("dot_enabled", _crosshair_val("dot_enabled"))
	controls["dot_color"] = _make_color("dot_color", _crosshair_val("dot_color"))
	controls["dot_size"] = _make_spin(_crosshair_val("dot_size"), 0.0, 40.0, 1.0, "dot_size")
	controls["dot_opacity"] = _make_spin(_crosshair_val("dot_opacity"), 0.0, 1.0, 0.05, "dot_opacity")
	controls["dot_rotation"] = _make_spin(_crosshair_val("dot_rotation"), 0.0, 45.0, 1.0, "dot_rotation")
	controls["cross_dot_collision"] = _make_toggle("cross_dot_collision", _crosshair_val("cross_dot_collision"))
	controls["dot_contrast"] = _make_toggle("dot_contrast", _crosshair_val("dot_contrast"))

	var y := -82.0
	_crosshair_place(page, 0, y, "Show", controls["cross_enabled"])
	y += 35.0
	_crosshair_place(page, 0, y, "Colour", controls["cross_color"])
	y += 35.0
	_crosshair_place(page, 0, y, "Length", controls["cross_length"])
	y += 35.0
	_crosshair_place(page, 0, y, "Thickness", controls["cross_thickness"])
	y += 35.0
	_crosshair_place(page, 0, y, "Opacity", controls["cross_opacity"])
	y += 35.0
	_crosshair_place(page, 0, y, "Spacing", controls["cross_spacing"])
	y += 35.0
	_crosshair_place(page, 0, y, "Rotation", controls["cross_rotation"])
	y += 35.0
	_crosshair_place(page, 0, y, "Top Line", controls["top_line_enabled"])
	y += 35.0
	_crosshair_place(page, 0, y, "Contrast", controls["cross_contrast"])

	var yd := -82.0
	_crosshair_place(page, 1, yd, "Show", controls["dot_enabled"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Colour", controls["dot_color"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Size", controls["dot_size"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Opacity", controls["dot_opacity"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Rotation", controls["dot_rotation"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Collision", controls["cross_dot_collision"])
	yd += 35.0
	_crosshair_place(page, 1, yd, "Contrast", controls["dot_contrast"])

	var reset := _make_button("Reset")
	reset.offset_top = 240.0 * s
	reset.offset_bottom = 260.0 * s
	reset.offset_left = -210.0 * s
	reset.offset_right = -10.0 * s
	reset.pressed.connect(func():
		for k in _crosshair_defaults:
			if crosshair_node:
				crosshair_node.set(k, _crosshair_defaults[k])
		_cross_refresh_controls(controls)
		_schedule_save())
	page.add_child(reset)

	var back := _make_button("Back")
	back.offset_top = 240.0 * s
	back.offset_bottom = 260.0 * s
	back.offset_left = 10.0 * s
	back.offset_right = 210.0 * s
	back.pressed.connect(func(): _show_page("gui"))
	page.add_child(back)
	return page

func _crosshair_val(field: String) -> Variant:
	return crosshair_node.get(field) if crosshair_node else _crosshair_defaults[field]

func _cross_set(field: String, value):
	if crosshair_node:
		crosshair_node.set(field, value)
	_schedule_save()

func _make_spin(value: float, min_value: float, max_value: float, step: float, field: String) -> SpinBox:
	var s := _ui_scale()
	var sp := SpinBox.new()
	sp.min_value = min_value
	sp.max_value = max_value
	sp.step = step
	sp.value = value
	sp.get_line_edit().add_theme_font_override("font", MUNRO_FONT)
	sp.get_line_edit().add_theme_font_size_override("font_size", int(10 * s))
	sp.get_line_edit().custom_minimum_size = Vector2(0, 0)
	sp.value_changed.connect(func(v: float):
		_cross_set(field, v))
	return sp

func _make_color(field: String, value: Color) -> ColorPickerButton:
	var cp := ColorPickerButton.new()
	cp.color = value
	cp.color_changed.connect(func(c: Color):
		_cross_set(field, c))
	return cp

func _make_toggle(field: String, value: bool) -> Button:
	var btn := Button.new()
	btn.text = "On" if value else "Off"
	_style_button(btn, 180.0)
	btn.pressed.connect(func():
		var nxt := not bool(_crosshair_val(field))
		_cross_set(field, nxt)
		btn.text = "On" if nxt else "Off")
	return btn

func _crosshair_header(page: Control, col: int, y: float, text: String):
	var s := _ui_scale()
	var label := Label.new()
	label.text = text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(14 * s))
	label.add_theme_color_override("font_color", Color(0.8, 0.85, 1.0))
	label.set_anchors_preset(Control.PRESET_CENTER)
	label.offset_top = y * s
	label.offset_bottom = (y + 14.0) * s
	if col == 0:
		label.offset_left = -280.0 * s
		label.offset_right = -80.0 * s
	else:
		label.offset_left = 80.0 * s
		label.offset_right = 280.0 * s
	page.add_child(label)

func _crosshair_place(page: Control, col: int, y: float, label_text: String, control: Control):
	var s := _ui_scale()
	var label := Label.new()
	label.text = label_text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(10 * s))
	label.add_theme_color_override("font_color", Color.WHITE)
	label.set_anchors_preset(Control.PRESET_CENTER)
	label.offset_top = y * s
	label.offset_bottom = (y + 12.0) * s
	control.set_anchors_preset(Control.PRESET_CENTER)
	if col == 0:
		label.offset_left = -280.0 * s
		label.offset_right = -80.0 * s
		control.offset_left = -280.0 * s
		control.offset_right = -80.0 * s
	else:
		label.offset_left = 80.0 * s
		label.offset_right = 280.0 * s
		control.offset_left = 80.0 * s
		control.offset_right = 280.0 * s
	control.offset_top = (y + 12.0) * s
	control.offset_bottom = (y + 34.0) * s
	page.add_child(label)
	page.add_child(control)

func _cross_refresh_controls(controls: Dictionary):
	for k in controls:
		var v = crosshair_node.get(k) if crosshair_node else _crosshair_defaults[k]
		var c: Control = controls[k]
		if c is Button:
			c.text = "On" if v else "Off"
		elif c is SpinBox:
			c.value = v
		elif c is ColorPickerButton:
			c.color = v

func _build_lighting_page() -> Control:
	var dur := SpinBox.new()
	dur.min_value = 10.0
	dur.max_value = 600.0
	dur.step = 10.0
	dur.suffix = " s"
	dur.value = chunk_manager.get_day_duration()
	dur.value_changed.connect(func(v: float):
		chunk_manager.set_day_duration(v)
		_schedule_save())
	var dur_reset := func():
		dur.value = _default_day_duration

	var day_color := ColorPickerButton.new()
	day_color.color = chunk_manager.get_day_sky_color()
	day_color.color_changed.connect(func(c: Color):
		chunk_manager.set_day_sky_color(c)
		_schedule_save())
	var day_reset := func():
		day_color.color = _default_day_sky
		chunk_manager.set_day_sky_color(_default_day_sky)
		_schedule_save()

	var night_color := ColorPickerButton.new()
	night_color.color = chunk_manager.get_night_sky_color()
	night_color.color_changed.connect(func(c: Color):
		chunk_manager.set_night_sky_color(c)
		_schedule_save())
	var night_reset := func():
		night_color.color = _default_night_sky
		chunk_manager.set_night_sky_color(_default_night_sky)
		_schedule_save()

	return _build_option_page("LIGHTING", [
		["Day Duration", dur, dur_reset],
		["Day Sky Color", day_color, day_reset],
		["Night Sky Color", night_color, night_reset],
	], "settings")

func _build_render_page() -> Control:
	var rd := SpinBox.new()
	rd.min_value = 2.0
	rd.max_value = 64.0
	rd.step = 1.0
	rd.suffix = " chunks"
	rd.value = chunk_manager.get_render_distance()
	rd.value_changed.connect(func(v: float):
		chunk_manager.set_render_distance(int(v))
		_schedule_save())
	var reset := func():
		rd.value = _default_render_distance
	return _build_option_page("RENDER", [["Render Distance", rd, reset]], "settings")

func _build_option_page(title_text: String, rows: Array, back_target: String) -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title := _make_title(title_text)
	title.offset_top = -80.0 * s
	title.offset_bottom = -40.0 * s
	page.add_child(title)

	var y := -30.0
	for row in rows:
		var label := Label.new()
		label.text = row[0]
		label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		label.add_theme_font_override("font", MUNRO_FONT)
		label.add_theme_font_size_override("font_size", int(10 * s))
		label.add_theme_color_override("font_color", Color.WHITE)
		label.set_anchors_preset(Control.PRESET_CENTER)
		label.offset_left = -200.0 * s
		label.offset_right = 200.0 * s
		label.offset_top = y * s
		label.offset_bottom = (y + 12.0) * s
		page.add_child(label)

		var control: Control = row[1]
		control.set_anchors_preset(Control.PRESET_CENTER)
		if row.size() > 2 and row[2] != null:
			control.offset_left = -140.0 * s
			control.offset_right = 40.0 * s
		else:
			control.offset_left = -100.0 * s
			control.offset_right = 100.0 * s
		control.offset_top = (y + 14.0) * s
		control.offset_bottom = (y + 34.0) * s
		page.add_child(control)

		if row.size() > 2 and row[2] != null:
			var reset := _make_button("Reset", 80.0)
			reset.offset_left = 48.0 * s
			reset.offset_right = 128.0 * s
			reset.offset_top = (y + 14.0) * s
			reset.offset_bottom = (y + 34.0) * s
			reset.pressed.connect(row[2])
			page.add_child(reset)

		y += 44.0

	var back := _make_button("Back")
	back.offset_top = (y + 8.0) * s
	back.offset_bottom = (y + 28.0) * s
	back.pressed.connect(func(): _show_page(back_target))
	page.add_child(back)
	return page

func _make_title(text: String) -> Label:
	var s := _ui_scale()
	var title := Label.new()
	title.text = text
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_override("font", MUNRO_FONT)
	title.add_theme_font_size_override("font_size", int(24 * s))
	title.add_theme_color_override("font_color", Color.WHITE)
	title.set_anchors_preset(Control.PRESET_CENTER)
	title.offset_left = -200.0 * s
	title.offset_right = 200.0 * s
	return title

func _make_button(text: String, width := 200.0) -> Button:
	var s := _ui_scale()
	var btn := Button.new()
	btn.text = text
	_style_button(btn, width)
	btn.set_anchors_preset(Control.PRESET_CENTER)
	btn.offset_left = -(width / 2.0) * s
	btn.offset_right = (width / 2.0) * s
	return btn

func _style_button(btn: Button, width: float):
	var s := _ui_scale()
	btn.add_theme_font_override("font", MUNRO_FONT)
	btn.add_theme_font_size_override("font_size", int(12 * s))
	btn.add_theme_color_override("font_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color.WHITE)
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(0.75, 0.75, 0.75)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", normal)
	btn.custom_minimum_size = Vector2(width, 20) * s

func _input(event):
	if not event.is_action_pressed("ui_cancel"):
		return
	if get_viewport().is_input_handled():
		return
	if is_open:
		match _current_page:
			"pause":
				_close()
			"settings":
				_show_page("pause")
			"crosshair":
				_show_page("gui")
			_:
				_show_page("settings")
		get_viewport().set_input_as_handled()
	elif not player_controller.is_chat_open() and not player_controller.is_inventory_open():
		_open()
		get_viewport().set_input_as_handled()

func _open():
	is_open = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	_rebuild_pages()
	_show_page("pause")
	show()
	player_controller.set_settings_open(true)

func _close():
	is_open = false
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	hide()
	player_controller.set_settings_open(false)
	_save_settings()
