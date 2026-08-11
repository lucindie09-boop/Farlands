extends Control

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")
const BUTTON_SQUARE_TEX: Texture2D = preload("res://textures/gui/button_square.png")
const UNDO_TEX: Texture2D = preload("res://textures/gui/undo_button.png")
const SETTINGS_PATH := "user://settings.cfg"

@onready var player_controller = get_node("/root/Main/Player")
@onready var chunk_manager = get_node("/root/Main/ChunkManager")
@onready var crosshair_node = get_node_or_null("/root/Main/HUD/Crosshair")
@onready var block_outline_node = get_node_or_null("/root/Main/BlockOutline")
@onready var godrays_node = get_node_or_null("/root/Main/HUD/GodRaysOverlay")

var is_open = false
var _current_page: String = "pause"

var _bg: ColorRect
var _pages: Dictionary = {}

var _default_gui_scale: float = 3.0
var _default_day_duration: float = 10.0
var _default_day_sky: Color = Color(1, 1, 1, 1)
var _default_night_sky: Color = Color(0, 0, 0, 1)
var _default_render_distance: int = 32
var _default_lod_distance: int = 0
var _default_lod_detail: float = 0.5
var _default_contrast: float = 1.0
var _default_saturation: float = 1.0
var _default_ao_color: Color = Color(0, 0, 0, 1)
var _default_ao_strength: float = 1.0
var _default_darkness_color: Color = Color(0, 0, 0, 1)
var _default_smooth_lighting: bool = false
var _default_fog_mode: int = 1  # 0=disabled, 1=edge, 2=linear, 3=exponential
var _default_godrays: bool = true
var _default_mipmaps_enabled: bool = true
var _default_mipmap_bias: float = 0.1
var _default_textures_enabled: bool = true
var _default_old_reset_buttons: bool = false
var _default_fps_cap: int = 0

var _block_outline_defaults := {
	"outline_enabled": true,
	"outline_color": Color.BLACK,
	"outline_thickness": 0.1,
	"outline_opacity": 1.0,
	"outline_pulse_enabled": false,
	"outline_pulse_speed": 2.0,
	"outline_pulse_min_opacity": 0.3,
	"outline_pulse_max_opacity": 1.0,
	"fill_enabled": false,
	"fill_color": Color(0.0, 0.0, 0.0, 1.0),
	"fill_opacity": 0.3,
	"fill_pulse_enabled": false,
	"fill_pulse_speed": 2.0,
	"fill_pulse_min_opacity": 0.1,
	"fill_pulse_max_opacity": 0.5,
	"reach_distance": 5.0,
}

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
var _old_reset_buttons: bool = false
var _fps_cap: int = 0

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
	_default_lod_distance = chunk_manager.get_lod_distance()
	_default_lod_detail = chunk_manager.get_lod_detail_level()
	_default_contrast = chunk_manager.get_contrast()
	_default_saturation = chunk_manager.get_saturation()
	_default_ao_color = chunk_manager.get_ao_color()
	_default_ao_strength = chunk_manager.get_ao_strength()
	_default_darkness_color = chunk_manager.get_darkness_color()
	_default_smooth_lighting = chunk_manager.get_smooth_lighting()
	# Don't load fog_mode from chunk_manager - keep hardcoded default for reset
	# _default_fog_mode = chunk_manager.get_fog_mode()
	_default_mipmaps_enabled = chunk_manager.get_mipmaps_enabled()
	_default_mipmap_bias = chunk_manager.get_mipmap_bias()
	_default_textures_enabled = chunk_manager.get_textures_enabled()
	if crosshair_node:
		for k in _crosshair_defaults:
			_crosshair_defaults[k] = crosshair_node.get(k)
	if block_outline_node:
		for k in _block_outline_defaults:
			_block_outline_defaults[k] = block_outline_node.get(k)
	_load_settings()
	Engine.max_fps = _fps_cap
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
	if cfg.has_section_key("gui", "block_outline"):
		cfg.erase_section_key("gui", "block_outline")
	for k in _crosshair_defaults:
		cfg.set_value("gui", k, crosshair_node.get(k) if crosshair_node else _crosshair_defaults[k])
	for k in _block_outline_defaults:
		cfg.set_value("gui", k, block_outline_node.get(k) if block_outline_node else _block_outline_defaults[k])
	cfg.set_value("lighting", "day_duration", chunk_manager.get_day_duration())
	cfg.set_value("lighting", "day_sky_color", chunk_manager.get_day_sky_color())
	cfg.set_value("lighting", "night_sky_color", chunk_manager.get_night_sky_color())
	cfg.set_value("lighting", "contrast", chunk_manager.get_contrast())
	cfg.set_value("lighting", "saturation", chunk_manager.get_saturation())
	cfg.set_value("lighting", "ao_color", chunk_manager.get_ao_color())
	cfg.set_value("lighting", "ao_strength", chunk_manager.get_ao_strength())
	cfg.set_value("lighting", "darkness_color", chunk_manager.get_darkness_color())
	cfg.set_value("lighting", "smooth_lighting", chunk_manager.get_smooth_lighting())
	cfg.set_value("render", "distance", chunk_manager.get_render_distance())
	cfg.set_value("render", "lod_distance", chunk_manager.get_lod_distance())
	cfg.set_value("render", "lod_detail_level", chunk_manager.get_lod_detail_level())
	cfg.set_value("render", "fog_mode", chunk_manager.get_fog_mode())
	cfg.set_value("render", "godrays", godrays_node.visible if godrays_node else _default_godrays)
	cfg.set_value("render", "mipmaps_enabled", chunk_manager.get_mipmaps_enabled())
	cfg.set_value("render", "mipmap_bias", chunk_manager.get_mipmap_bias())
	cfg.set_value("render", "textures_enabled", chunk_manager.get_textures_enabled())
	cfg.set_value("render", "fps_cap", _fps_cap)
	cfg.set_value("gui", "old_reset_buttons", _old_reset_buttons)
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
	for k in _block_outline_defaults:
		if block_outline_node:
			block_outline_node.set(k, cfg.get_value("gui", k, _block_outline_defaults[k]))
	chunk_manager.set_day_duration(cfg.get_value("lighting", "day_duration", chunk_manager.get_day_duration()))
	chunk_manager.set_day_sky_color(cfg.get_value("lighting", "day_sky_color", chunk_manager.get_day_sky_color()))
	chunk_manager.set_night_sky_color(cfg.get_value("lighting", "night_sky_color", chunk_manager.get_night_sky_color()))
	chunk_manager.set_contrast(cfg.get_value("lighting", "contrast", chunk_manager.get_contrast()))
	chunk_manager.set_saturation(cfg.get_value("lighting", "saturation", chunk_manager.get_saturation()))
	chunk_manager.set_ao_color(cfg.get_value("lighting", "ao_color", chunk_manager.get_ao_color()))
	chunk_manager.set_ao_strength(cfg.get_value("lighting", "ao_strength", chunk_manager.get_ao_strength()))
	chunk_manager.set_darkness_color(cfg.get_value("lighting", "darkness_color", chunk_manager.get_darkness_color()))
	chunk_manager.set_smooth_lighting(cfg.get_value("lighting", "smooth_lighting", chunk_manager.get_smooth_lighting()))
	chunk_manager.set_render_distance(int(cfg.get_value("render", "distance", chunk_manager.get_render_distance())))
	chunk_manager.set_lod_distance(int(cfg.get_value("render", "lod_distance", chunk_manager.get_lod_distance())))
	chunk_manager.set_lod_detail_level(cfg.get_value("render", "lod_detail_level", chunk_manager.get_lod_detail_level()))
	chunk_manager.set_fog_mode(int(cfg.get_value("render", "fog_mode", chunk_manager.get_fog_mode())))
	if godrays_node:
		godrays_node.visible = cfg.get_value("render", "godrays", _default_godrays)
	chunk_manager.set_mipmaps_enabled(cfg.get_value("render", "mipmaps_enabled", chunk_manager.get_mipmaps_enabled()))
	chunk_manager.set_mipmap_bias(cfg.get_value("render", "mipmap_bias", chunk_manager.get_mipmap_bias()))
	chunk_manager.set_textures_enabled(cfg.get_value("render", "textures_enabled", chunk_manager.get_textures_enabled()))
	var loaded_fps_cap = cfg.get_value("render", "fps_cap", _default_fps_cap)
	_fps_cap = loaded_fps_cap if loaded_fps_cap != 60 else _default_fps_cap
	Engine.max_fps = _fps_cap
	_old_reset_buttons = cfg.get_value("gui", "old_reset_buttons", _default_old_reset_buttons)

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
	_pages["block_outline"] = _build_block_outline_page()
	_pages["lighting"] = _build_lighting_page()
	_pages["render"] = _build_render_page()
	for p in _pages.values():
		p.hide()
		add_child(p)

func _show_page(page_name: String):
	_current_page = page_name
	for k in _pages:
		_pages[k].visible = (k == page_name)

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

	var block_outline_btn := _make_button("Block Outline")
	block_outline_btn.pressed.connect(func(): _show_page("block_outline"))

	var old_reset_btn := _make_button("Old" if _old_reset_buttons else "New", 180.0)
	old_reset_btn.pressed.connect(func():
		_old_reset_buttons = not _old_reset_buttons
		old_reset_btn.text = "Old" if _old_reset_buttons else "New"
		_schedule_save())
	var old_reset_reset := func():
		_old_reset_buttons = _default_old_reset_buttons
		old_reset_btn.text = "Old" if _default_old_reset_buttons else "New"
		_schedule_save()

	return _build_option_page("GUI", [
		["GUI Scale", scale_btn, reset],
		["Reset Button Type", old_reset_btn, old_reset_reset],
		["Crosshair", crosshair_btn, null],
		["Block Outline", block_outline_btn, null],
	], "settings", 44.0)

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
	_crosshair_place(page, 0, y, "Dynamic Contrast", controls["cross_contrast"])

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
	_crosshair_place(page, 1, yd, "Dynamic Contrast", controls["dot_contrast"])

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

func _build_block_outline_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title := _make_title("BLOCK OUTLINE")
	title.offset_top = -150.0 * s
	title.offset_bottom = -110.0 * s
	page.add_child(title)

	_block_outline_header(page, 0, -102.0, "OUTLINE")
	_block_outline_header(page, 1, -102.0, "FILL")

	var controls := {}
	controls["outline_enabled"] = _make_toggle_outline("outline_enabled", _block_outline_val("outline_enabled"))
	controls["outline_color"] = _make_color_outline("outline_color", _block_outline_val("outline_color"))
	controls["outline_thickness"] = _make_spin_outline(_block_outline_val("outline_thickness"), 0.0, 0.99, 0.01, "outline_thickness")
	controls["outline_opacity"] = _make_spin_outline(_block_outline_val("outline_opacity"), 0.0, 1.0, 0.05, "outline_opacity")
	controls["outline_pulse_enabled"] = _make_toggle_outline("outline_pulse_enabled", _block_outline_val("outline_pulse_enabled"))
	controls["outline_pulse_speed"] = _make_spin_outline(_block_outline_val("outline_pulse_speed"), 0.5, 10.0, 0.5, "outline_pulse_speed")
	controls["outline_pulse_min_opacity"] = _make_spin_outline(_block_outline_val("outline_pulse_min_opacity"), 0.0, 1.0, 0.05, "outline_pulse_min_opacity")
	controls["outline_pulse_max_opacity"] = _make_spin_outline(_block_outline_val("outline_pulse_max_opacity"), 0.0, 1.0, 0.05, "outline_pulse_max_opacity")
	controls["fill_enabled"] = _make_toggle_outline("fill_enabled", _block_outline_val("fill_enabled"))
	controls["fill_color"] = _make_color_outline("fill_color", _block_outline_val("fill_color"))
	controls["fill_opacity"] = _make_spin_outline(_block_outline_val("fill_opacity"), 0.0, 1.0, 0.05, "fill_opacity")
	controls["fill_pulse_enabled"] = _make_toggle_outline("fill_pulse_enabled", _block_outline_val("fill_pulse_enabled"))
	controls["fill_pulse_speed"] = _make_spin_outline(_block_outline_val("fill_pulse_speed"), 0.5, 10.0, 0.5, "fill_pulse_speed")
	controls["fill_pulse_min_opacity"] = _make_spin_outline(_block_outline_val("fill_pulse_min_opacity"), 0.0, 1.0, 0.05, "fill_pulse_min_opacity")
	controls["fill_pulse_max_opacity"] = _make_spin_outline(_block_outline_val("fill_pulse_max_opacity"), 0.0, 1.0, 0.05, "fill_pulse_max_opacity")

	var y := -82.0
	_block_outline_place(page, 0, y, "Show", controls["outline_enabled"])
	y += 35.0
	_block_outline_place(page, 0, y, "Colour", controls["outline_color"])
	y += 35.0
	_block_outline_place(page, 0, y, "Thickness", controls["outline_thickness"])
	y += 35.0
	_block_outline_place(page, 0, y, "Opacity", controls["outline_opacity"])
	y += 35.0
	_block_outline_place(page, 0, y, "Pulse", controls["outline_pulse_enabled"])
	y += 35.0
	_block_outline_place(page, 0, y, "Pulse Speed", controls["outline_pulse_speed"])
	y += 35.0
	_block_outline_place(page, 0, y, "Pulse Min", controls["outline_pulse_min_opacity"])
	y += 35.0
	_block_outline_place(page, 0, y, "Pulse Max", controls["outline_pulse_max_opacity"])

	var yf := -82.0
	_block_outline_place(page, 1, yf, "Show", controls["fill_enabled"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Colour", controls["fill_color"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Opacity", controls["fill_opacity"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Pulse", controls["fill_pulse_enabled"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Pulse Speed", controls["fill_pulse_speed"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Pulse Min", controls["fill_pulse_min_opacity"])
	yf += 35.0
	_block_outline_place(page, 1, yf, "Pulse Max", controls["fill_pulse_max_opacity"])

	var reset := _make_button("Reset")
	reset.offset_top = 240.0 * s
	reset.offset_bottom = 260.0 * s
	reset.offset_left = -210.0 * s
	reset.offset_right = -10.0 * s
	reset.pressed.connect(func():
		for k in _block_outline_defaults:
			if block_outline_node:
				block_outline_node.set(k, _block_outline_defaults[k])
		_block_outline_refresh_controls(controls)
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

func _block_outline_val(field: String) -> Variant:
	return block_outline_node.get(field) if block_outline_node else _block_outline_defaults[field]

func _block_outline_set(field: String, value):
	if block_outline_node:
		block_outline_node.set(field, value)
	_schedule_save()

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

func _make_spin_outline(value: float, min_value: float, max_value: float, step: float, field: String) -> SpinBox:
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
		_block_outline_set(field, v))
	return sp

func _make_color(field: String, value: Color) -> ColorPickerButton:
	var cp := ColorPickerButton.new()
	cp.color = value
	cp.color_changed.connect(func(c: Color):
		_cross_set(field, c))
	return cp

func _make_color_outline(field: String, value: Color) -> ColorPickerButton:
	var cp := ColorPickerButton.new()
	cp.color = value
	cp.color_changed.connect(func(c: Color):
		_block_outline_set(field, c))
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

func _make_toggle_outline(field: String, value: bool) -> Button:
	var btn := Button.new()
	btn.text = "On" if value else "Off"
	_style_button(btn, 180.0)
	btn.pressed.connect(func():
		var nxt := not bool(_block_outline_val(field))
		_block_outline_set(field, nxt)
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
	control.set_anchors_preset(Control.PRESET_CENTER)
	if col == 0:
		control.offset_left = -280.0 * s
		control.offset_right = -80.0 * s
	else:
		control.offset_left = 80.0 * s
		control.offset_right = 280.0 * s
	control.offset_top = (y + 12.0) * s
	control.offset_bottom = (y + 34.0) * s
	page.add_child(control)

	var label := Label.new()
	label.text = label_text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(10 * s))
	label.add_theme_color_override("font_color", Color.WHITE)
	label.set_anchors_preset(Control.PRESET_CENTER)
	label.offset_left = control.offset_left
	label.offset_right = control.offset_right
	label.offset_top = y * s
	label.offset_bottom = (y + 12.0) * s
	page.add_child(label)

func _block_outline_header(page: Control, col: int, y: float, text: String):
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

func _block_outline_place(page: Control, col: int, y: float, label_text: String, control: Control):
	var s := _ui_scale()
	control.set_anchors_preset(Control.PRESET_CENTER)
	if col == 0:
		control.offset_left = -280.0 * s
		control.offset_right = -80.0 * s
	else:
		control.offset_left = 80.0 * s
		control.offset_right = 280.0 * s
	control.offset_top = (y + 12.0) * s
	control.offset_bottom = (y + 34.0) * s
	page.add_child(control)

	var label := Label.new()
	label.text = label_text
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(10 * s))
	label.add_theme_color_override("font_color", Color.WHITE)
	label.set_anchors_preset(Control.PRESET_CENTER)
	label.offset_left = control.offset_left
	label.offset_right = control.offset_right
	label.offset_top = y * s
	label.offset_bottom = (y + 12.0) * s
	page.add_child(label)

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

func _block_outline_refresh_controls(controls: Dictionary):
	for k in controls:
		var v = block_outline_node.get(k) if block_outline_node else _block_outline_defaults[k]
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
		chunk_manager.set_day_duration(_default_day_duration)
		_schedule_save()

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

	var ao_color := ColorPickerButton.new()
	ao_color.color = chunk_manager.get_ao_color()
	ao_color.color_changed.connect(func(c: Color):
		chunk_manager.set_ao_color(c)
		_schedule_save())
	var ao_reset := func():
		ao_color.color = _default_ao_color
		chunk_manager.set_ao_color(_default_ao_color)
		_schedule_save()

	var ao_strength := SpinBox.new()
	ao_strength.min_value = 0.0
	ao_strength.max_value = 2.0
	ao_strength.step = 0.05
	ao_strength.value = chunk_manager.get_ao_strength()
	ao_strength.value_changed.connect(func(v: float):
		chunk_manager.set_ao_strength(v)
		_schedule_save())
	var ao_strength_reset := func():
		ao_strength.value = _default_ao_strength
		chunk_manager.set_ao_strength(_default_ao_strength)
		_schedule_save()

	var dark_color := ColorPickerButton.new()
	dark_color.color = chunk_manager.get_darkness_color()
	dark_color.color_changed.connect(func(c: Color):
		chunk_manager.set_darkness_color(c)
		_schedule_save())
	var dark_reset := func():
		dark_color.color = _default_darkness_color
		chunk_manager.set_darkness_color(_default_darkness_color)
		_schedule_save()

	var contrast := SpinBox.new()
	contrast.min_value = 0.0
	contrast.max_value = 2.0
	contrast.step = 0.05
	contrast.value = chunk_manager.get_contrast()
	contrast.value_changed.connect(func(v: float):
		chunk_manager.set_contrast(v)
		_schedule_save())
	var contrast_reset := func():
		contrast.value = _default_contrast
		chunk_manager.set_contrast(_default_contrast)
		_schedule_save()

	var saturation := SpinBox.new()
	saturation.min_value = 0.0
	saturation.max_value = 2.0
	saturation.step = 0.05
	saturation.value = chunk_manager.get_saturation()
	saturation.value_changed.connect(func(v: float):
		chunk_manager.set_saturation(v)
		_schedule_save())
	var saturation_reset := func():
		saturation.value = _default_saturation
		chunk_manager.set_saturation(_default_saturation)
		_schedule_save()

	var smooth_lighting := Button.new()
	smooth_lighting.text = "On" if chunk_manager.get_smooth_lighting() else "Off"
	_style_button(smooth_lighting, 180.0)
	# Note: Smooth lighting setting only takes effect on game restart
	smooth_lighting.pressed.connect(func():
		chunk_manager.set_smooth_lighting(not chunk_manager.get_smooth_lighting())
		smooth_lighting.text = "On" if chunk_manager.get_smooth_lighting() else "Off"
		_schedule_save())
	var smooth_lighting_reset := func():
		chunk_manager.set_smooth_lighting(_default_smooth_lighting)
		smooth_lighting.text = "On" if _default_smooth_lighting else "Off"
		_schedule_save()

	return _build_option_page("LIGHTING", [
		["Day Duration", dur, dur_reset],
		["Day Sky Color", day_color, day_reset],
		["Night Sky Color", night_color, night_reset],
		["Ambient Occlusion Color", ao_color, ao_reset],
		["AO Strength", ao_strength, ao_strength_reset],
		["Darkness Color", dark_color, dark_reset],
		["Contrast", contrast, contrast_reset],
		["Saturation", saturation, saturation_reset],
		["Smooth Lighting", smooth_lighting, smooth_lighting_reset],
	], "settings", 40.0)

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
	var rd_reset := func():
		rd.value = _default_render_distance
		chunk_manager.set_render_distance(_default_render_distance)
		_schedule_save()

	var lod_dist := SpinBox.new()
	lod_dist.min_value = 0.0
	lod_dist.max_value = 64.0
	lod_dist.step = 1.0
	lod_dist.suffix = " chunks"
	lod_dist.value = chunk_manager.get_lod_distance()
	lod_dist.value_changed.connect(func(v: float):
		chunk_manager.set_lod_distance(int(v))
		_schedule_save())
	var lod_reset := func():
		lod_dist.value = _default_lod_distance
		chunk_manager.set_lod_distance(_default_lod_distance)
		_schedule_save()

	var lod_detail := SpinBox.new()
	lod_detail.min_value = 0.125
	lod_detail.max_value = 1.0
	lod_detail.step = 0.005
	lod_detail.value = chunk_manager.get_lod_detail_level()
	lod_detail.value_changed.connect(func(v: float):
		chunk_manager.set_lod_detail_level(v)
		_schedule_save())
	var lod_detail_reset := func():
		lod_detail.value = _default_lod_detail
		chunk_manager.set_lod_detail_level(_default_lod_detail)
		_schedule_save()

	var fog_mode_btn := _make_button("", 180.0)
	var fog_mode_names := ["Off", "Edge", "Linear", "Exponential"]
	fog_mode_btn.text = fog_mode_names[chunk_manager.get_fog_mode()]
	fog_mode_btn.pressed.connect(func():
		var current: int = chunk_manager.get_fog_mode()
		var next: int = (current + 1) % fog_mode_names.size()
		chunk_manager.set_fog_mode(next)
		fog_mode_btn.text = fog_mode_names[next]
		_schedule_save())
	var fog_mode_reset := func():
		chunk_manager.set_fog_mode(_default_fog_mode)
		fog_mode_btn.text = fog_mode_names[_default_fog_mode]
		_schedule_save()

	var godrays_btn := Button.new()
	var godrays_enabled: bool = godrays_node.visible if godrays_node else _default_godrays
	godrays_btn.text = "On" if godrays_enabled else "Off"
	_style_button(godrays_btn, 180.0)
	godrays_btn.pressed.connect(func():
		var next: bool = not (godrays_node.visible if godrays_node else _default_godrays)
		if godrays_node:
			godrays_node.visible = next
		godrays_btn.text = "On" if next else "Off"
		_schedule_save())
	var godrays_reset := func():
		if godrays_node:
			godrays_node.visible = _default_godrays
		godrays_btn.text = "On" if _default_godrays else "Off"
		_schedule_save()

	var mipmap_bias_spin := SpinBox.new()
	mipmap_bias_spin.min_value = -4.0
	mipmap_bias_spin.max_value = 4.0
	mipmap_bias_spin.step = 0.01
	mipmap_bias_spin.value = chunk_manager.get_mipmap_bias()
	mipmap_bias_spin.editable = chunk_manager.get_mipmaps_enabled()
	mipmap_bias_spin.value_changed.connect(func(v: float):
		chunk_manager.set_mipmap_bias(v)
		_schedule_save())
	var mipmap_bias_reset := func():
		mipmap_bias_spin.value = _default_mipmap_bias
		chunk_manager.set_mipmap_bias(_default_mipmap_bias)
		_schedule_save()

	var mipmaps_btn := Button.new()
	var mipmaps_enabled: bool = chunk_manager.get_mipmaps_enabled()
	mipmaps_btn.text = "On" if mipmaps_enabled else "Off"
	_style_button(mipmaps_btn, 180.0)
	mipmaps_btn.pressed.connect(func():
		var next: bool = not chunk_manager.get_mipmaps_enabled()
		chunk_manager.set_mipmaps_enabled(next)
		mipmaps_btn.text = "On" if next else "Off"
		mipmap_bias_spin.editable = next
		_schedule_save())
	var mipmaps_reset := func():
		chunk_manager.set_mipmaps_enabled(_default_mipmaps_enabled)
		mipmaps_btn.text = "On" if _default_mipmaps_enabled else "Off"
		mipmap_bias_spin.editable = _default_mipmaps_enabled
		_schedule_save()

	var textures_btn := Button.new()
	var textures_enabled: bool = chunk_manager.get_textures_enabled()
	textures_btn.text = "On" if textures_enabled else "Off"
	_style_button(textures_btn, 180.0)
	textures_btn.pressed.connect(func():
		var next: bool = not chunk_manager.get_textures_enabled()
		chunk_manager.set_textures_enabled(next)
		textures_btn.text = "On" if next else "Off"
		_schedule_save())
	var textures_reset := func():
		chunk_manager.set_textures_enabled(_default_textures_enabled)
		textures_btn.text = "On" if _default_textures_enabled else "Off"
		_schedule_save()

	var fps_cap_spin := SpinBox.new()
	fps_cap_spin.min_value = 0.0
	fps_cap_spin.max_value = 300.0
	fps_cap_spin.step = 1.0
	fps_cap_spin.suffix = " FPS"
	fps_cap_spin.value = _fps_cap
	fps_cap_spin.value_changed.connect(func(v: float):
		_fps_cap = int(v)
		Engine.max_fps = _fps_cap
		_schedule_save())
	var fps_cap_reset := func():
		fps_cap_spin.value = _default_fps_cap
		_fps_cap = _default_fps_cap
		Engine.max_fps = _default_fps_cap
		_schedule_save()

	return _build_option_page("RENDER", [
		["Render Distance", rd, rd_reset],
		["LOD Distance", lod_dist, lod_reset],
		["LOD Detail Level", lod_detail, lod_detail_reset],
		["Fog Mode", fog_mode_btn, fog_mode_reset],
		["God Rays", godrays_btn, godrays_reset],
		["Mipmaps", mipmaps_btn, mipmaps_reset],
		["Mipmap Bias", mipmap_bias_spin, mipmap_bias_reset],
		["Textures", textures_btn, textures_reset],
		["FPS Cap", fps_cap_spin, fps_cap_reset],
	], "settings")

func _build_option_page(title_text: String, rows: Array, back_target: String, row_spacing := 44.0) -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var title_h := 40.0
	var title_gap := 10.0
	var n := float(rows.size())
	var content_h := title_h + title_gap + (n - 1.0) * row_spacing + 38.0 + 8.0 + 20.0
	var y0 := -content_h / 2.0

	var title := _make_title(title_text)
	title.offset_top = y0 * s
	title.offset_bottom = (y0 + title_h) * s
	page.add_child(title)

	var y := y0 + title_h + title_gap
	for row in rows:
		var control: Control = row[1]
		control.set_anchors_preset(Control.PRESET_CENTER)
		var has_reset: bool = row.size() > 2 and row[2] != null
		if has_reset:
			if _old_reset_buttons:
				control.offset_left = -140.0 * s
				control.offset_right = 40.0 * s
			else:
				control.offset_left = -120.0 * s
				control.offset_right = 60.0 * s
		else:
			control.offset_left = -100.0 * s
			control.offset_right = 100.0 * s
		control.offset_top = (y + 18.0) * s
		control.offset_bottom = (y + 38.0) * s
		page.add_child(control)

		var label := Label.new()
		label.text = row[0]
		label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		label.add_theme_font_override("font", MUNRO_FONT)
		label.add_theme_font_size_override("font_size", int(10 * s))
		label.add_theme_color_override("font_color", Color.WHITE)
		label.set_anchors_preset(Control.PRESET_CENTER)
		label.offset_left = control.offset_left
		label.offset_right = control.offset_right
		label.offset_top = y * s
		label.offset_bottom = (y + 16.0) * s
		page.add_child(label)

		if row.size() > 2 and row[2] != null:
			var reset: Button
			if _old_reset_buttons:
				reset = _make_button("Reset", 80.0)
				reset.offset_left = 48.0 * s
				reset.offset_right = 128.0 * s
				reset.offset_top = (y + 18.0) * s
				reset.offset_bottom = (y + 38.0) * s
			else:
				reset = _make_undo_button(20.0)
				reset.offset_left = 78.0 * s
				reset.offset_right = 98.0 * s
				reset.offset_top = (y + 18.0) * s
				reset.offset_bottom = (y + 38.0) * s
			reset.pressed.connect(row[2])
			page.add_child(reset)

		y += row_spacing

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

func _make_undo_button(width := 200.0) -> Button:
	var btn := Button.new()
	btn.text = ""
	_style_undo_button(btn, width)
	btn.set_anchors_preset(Control.PRESET_CENTER)
	return btn

func _style_undo_button(btn: Button, width: float):
	var s := _ui_scale()
	var normal := StyleBoxTexture.new()
	normal.texture = UNDO_TEX
	var hover := StyleBoxTexture.new()
	hover.texture = UNDO_TEX
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	var pressed := StyleBoxTexture.new()
	pressed.texture = UNDO_TEX
	pressed.modulate_color = Color(0.75, 0.75, 0.75)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", normal)
	btn.custom_minimum_size = Vector2(width, width) * s

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
			"block_outline":
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
