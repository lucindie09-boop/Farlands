extends Control

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")
const BUTTON_SQUARE_TEX: Texture2D = preload("res://textures/gui/button_square.png")
const UNDO_TEX: Texture2D = preload("res://textures/gui/undo_button.png")
const SLIDER_TRACK_TEX: Texture2D = preload("res://textures/gui/slider_button.png")
const SLIDER_THUMB_TEX: Texture2D = preload("res://textures/gui/slider.png")
const SETTINGS_PATH := "user://settings.cfg"

# Actions exposed on the CONTROLS page. The engine keeps the pristine project
# defaults as the per-row reset target; runtime rebinding swaps InputMap events.
const CONTROL_BINDINGS := [
	["move_forward", "Walk Forward"],
	["move_back", "Walk Back"],
	["move_left", "Strafe Left"],
	["move_right", "Strafe Right"],
	["jump", "Jump"],
	["sprint", "Sprint"],
	["sneak", "Sneak"],
	["mouse_click_left", "Break / Attack"],
	["mouse_click_right", "Place / Use"],
	["fly_toggle", "Toggle Flight"],
	["toggle_inventory", "Inventory"],
	["toggle_chat", "Chat"],
	["toggle_third_person", "Third Person"],
]

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
var _default_far_lod_distance: int = 16
var _default_far_lod_detail: float = 0.25
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
var _default_compression_enabled: bool = false
var _default_old_reset_buttons: bool = false
var _default_fps_cap: int = 0
var _default_msaa_3d: int = 0

var _skin_dark_mode := false
var _skin_bg: ColorRect
var _skin_title: Label
var _skin_hex: Label
var _skin_hint: Label
var _skin_picker: ColorPicker
var _skin_preview: Control
var _skin_toggle: Button
var _skin_uv_toggle: Button
var _skin_undo_btn: Button
var _skin_tool_btn: Button
var _skin_name_edit: LineEdit
var _skin_save_btn: Button
var _skin_load_btn: Button
var _skin_noise_label: Label
var _skin_noise_slider: HSlider
var _skin_noise_value: Label
var _skin_noise := 0.0
var _skin_gallery: Control
var _skin_gallery_grid: GridContainer
var _skin_gallery_timer: Timer
var _skin_gallery_spins: Array = []

var _block_bg: ColorRect
var _block_title: Label
var _block_hex: Label
var _block_hint: Label
var _block_picker: ColorPicker
var _block_preview: Control
var _block_undo_btn: Button
var _block_tool_btn: Button
var _block_name_edit: LineEdit
var _block_save_btn: Button
var _block_load_btn: Button
var _block_dark_toggle: Button
var _block_uv_toggle: Button
var _block_gallery: Control
var _block_gallery_grid: GridContainer
var _block_gallery_timer: Timer
var _block_gallery_spins: Array = []
var _block_tool := "DRAW"
var _block_color := Color.WHITE
var _block_noise_label: Label
var _block_noise_slider: HSlider
var _block_noise_value: Label
var _block_noise := 0.0

var _controls_defaults := {}
var _control_buttons := {}
var _capturing_action := ""
var _controls_hint: Label = null

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
	_default_far_lod_distance = chunk_manager.get_far_lod_distance()
	_default_far_lod_detail = chunk_manager.get_far_lod_detail_level()
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
	_default_compression_enabled = chunk_manager.get_compression_enabled()
	_default_msaa_3d = get_viewport().msaa_3d
	if crosshair_node:
		for k in _crosshair_defaults:
			_crosshair_defaults[k] = crosshair_node.get(k)
	if block_outline_node:
		for k in _block_outline_defaults:
			_block_outline_defaults[k] = block_outline_node.get(k)
	# Snapshot each controls action's pristine project.godot bindings BEFORE any
	# saved overrides are applied, so the page's reset buttons always return to
	# the real defaults no matter what happened this session.
	for cb in CONTROL_BINDINGS:
		_controls_defaults[cb[0]] = InputMap.action_get_events(cb[0]).duplicate()
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
	cfg.set_value("render", "far_lod_distance", chunk_manager.get_far_lod_distance())
	cfg.set_value("render", "far_lod_detail_level", chunk_manager.get_far_lod_detail_level())
	cfg.set_value("render", "fog_mode", chunk_manager.get_fog_mode())
	cfg.set_value("render", "godrays", godrays_node.visible if godrays_node else _default_godrays)
	cfg.set_value("render", "mipmaps_enabled", chunk_manager.get_mipmaps_enabled())
	cfg.set_value("render", "mipmap_bias", chunk_manager.get_mipmap_bias())
	cfg.set_value("render", "textures_enabled", chunk_manager.get_textures_enabled())
	cfg.set_value("render", "compression_enabled", chunk_manager.get_compression_enabled())
	cfg.set_value("render", "fps_cap", _fps_cap)
	cfg.set_value("render", "msaa_3d", get_viewport().msaa_3d)
	cfg.set_value("gui", "old_reset_buttons", _old_reset_buttons)
	cfg.set_value("gui", "skin_dark_mode", _skin_dark_mode)
	cfg.set_value("gui", "skin_noise", _skin_noise)
	cfg.set_value("gui", "block_noise", _block_noise)
	for cb in CONTROL_BINDINGS:
		cfg.set_value("controls", cb[0], _serialize_action_events(cb[0]))
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
	chunk_manager.set_far_lod_distance(int(cfg.get_value("render", "far_lod_distance", chunk_manager.get_far_lod_distance())))
	chunk_manager.set_far_lod_detail_level(cfg.get_value("render", "far_lod_detail_level", chunk_manager.get_far_lod_detail_level()))
	chunk_manager.set_fog_mode(int(cfg.get_value("render", "fog_mode", chunk_manager.get_fog_mode())))
	if godrays_node:
		godrays_node.visible = cfg.get_value("render", "godrays", _default_godrays)
	chunk_manager.set_mipmaps_enabled(cfg.get_value("render", "mipmaps_enabled", chunk_manager.get_mipmaps_enabled()))
	chunk_manager.set_mipmap_bias(cfg.get_value("render", "mipmap_bias", chunk_manager.get_mipmap_bias()))
	chunk_manager.set_textures_enabled(cfg.get_value("render", "textures_enabled", chunk_manager.get_textures_enabled()))
	chunk_manager.set_compression_enabled(cfg.get_value("render", "compression_enabled", chunk_manager.get_compression_enabled()))
	var loaded_fps_cap = cfg.get_value("render", "fps_cap", _default_fps_cap)
	_fps_cap = loaded_fps_cap if loaded_fps_cap != 60 else _default_fps_cap
	Engine.max_fps = _fps_cap
	get_viewport().msaa_3d = int(cfg.get_value("render", "msaa_3d", _default_msaa_3d)) as Viewport.MSAA
	_old_reset_buttons = cfg.get_value("gui", "old_reset_buttons", _default_old_reset_buttons)
	_skin_dark_mode = cfg.get_value("gui", "skin_dark_mode", _skin_dark_mode)
	_skin_noise = float(cfg.get_value("gui", "skin_noise", _skin_noise))
	_block_noise = float(cfg.get_value("gui", "block_noise", _block_noise))
	for cb in CONTROL_BINDINGS:
		var saved: Array = cfg.get_value("controls", cb[0], [])
		if not saved.is_empty():
			_apply_action_events(cb[0], saved)

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
	_pages["controls"] = _build_controls_page()
	_pages["skin_maker"] = _build_skin_maker_page()
	_pages["block_maker"] = _build_block_maker_page()
	for p in _pages.values():
		p.hide()
		add_child(p)

func _show_page(page_name: String):
	_current_page = page_name
	for k in _pages:
		_pages[k].visible = (k == page_name)
	# The skin page was freshly rebuilt on open, so the preview node only exists
	# once it enters the tree. Re-apply any stored noise against the manager's
	# reversibility base now that it does; with an identical value it's a no-op.
	if page_name == "skin_maker" and _skin_preview != null && is_instance_valid(_skin_preview):
		_skin_preview.set_noise(_skin_noise)
	# The block page preview needs color/tool sync on open
	if page_name == "block_maker" and _block_preview != null && is_instance_valid(_block_preview):
		_block_preview.set_color(_block_color)
		_block_preview.set_tool(_block_tool)
		# Re-apply any stored noise against the manager's reversibility base now
		# that the preview exists; with an identical value it's a no-op.
		if _block_preview.has_method("set_noise"):
			_block_preview.set_noise(_block_noise)

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

	var controls_btn := _make_button("Controls")
	controls_btn.offset_top = 55.0 * s
	controls_btn.offset_bottom = 75.0 * s
	controls_btn.pressed.connect(func(): _show_page("controls"))
	page.add_child(controls_btn)

	var skin_btn := _make_button("Skin Maker")
	skin_btn.offset_top = 85.0 * s
	skin_btn.offset_bottom = 105.0 * s
	skin_btn.pressed.connect(func(): _show_page("skin_maker"))
	page.add_child(skin_btn)

	var block_btn := _make_button("Block Maker")
	block_btn.offset_top = 115.0 * s
	block_btn.offset_bottom = 135.0 * s
	block_btn.pressed.connect(func(): _show_page("block_maker"))
	page.add_child(block_btn)

	var back := _make_button("Back")
	back.offset_top = 145.0 * s
	back.offset_bottom = 165.0 * s
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

	# Status hint for import/export feedback
	var crosshair_hint := Label.new()
	crosshair_hint.text = ""
	crosshair_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	crosshair_hint.add_theme_font_override("font", MUNRO_FONT)
	crosshair_hint.add_theme_font_size_override("font_size", int(10 * s))
	crosshair_hint.add_theme_color_override("font_color", Color(1.0, 0.7, 0.3))
	crosshair_hint.set_anchors_preset(Control.PRESET_CENTER)
	crosshair_hint.offset_left = -260.0 * s
	crosshair_hint.offset_right = 260.0 * s
	crosshair_hint.offset_top = 200.0 * s
	crosshair_hint.offset_bottom = 216.0 * s
	page.add_child(crosshair_hint)

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

	var reset := _make_button("Reset", 100.0)
	reset.offset_top = 240.0 * s
	reset.offset_bottom = 260.0 * s
	reset.offset_left = -160.0 * s
	reset.offset_right = -60.0 * s
	reset.pressed.connect(func():
		for k in _crosshair_defaults:
			if crosshair_node:
				crosshair_node.set(k, _crosshair_defaults[k])
		_cross_refresh_controls(controls)
		_schedule_save())
	page.add_child(reset)

	var export_btn := _make_button("Export", 100.0)
	export_btn.offset_top = 240.0 * s
	export_btn.offset_bottom = 260.0 * s
	export_btn.offset_left = -50.0 * s
	export_btn.offset_right = 50.0 * s
	export_btn.pressed.connect(func():
		var code := _export_crosshair_code()
		if code != "":
			DisplayServer.clipboard_set(code)
			crosshair_hint.text = "Code copied to clipboard!"
		else:
			crosshair_hint.text = "Export failed"
		# Clear hint after 3 seconds
		get_tree().create_timer(3.0).timeout.connect(func(): crosshair_hint.text = "")
	)
	page.add_child(export_btn)

	var import_btn := _make_button("Import", 100.0)
	import_btn.offset_top = 240.0 * s
	import_btn.offset_bottom = 260.0 * s
	import_btn.offset_left = 60.0 * s
	import_btn.offset_right = 160.0 * s
	import_btn.pressed.connect(func():
		var code := DisplayServer.clipboard_get()
		if code != "":
			if _import_crosshair_code(code):
				_cross_refresh_controls(controls)
				crosshair_hint.text = "Code imported successfully!"
			else:
				crosshair_hint.text = "Invalid code format"
		else:
			crosshair_hint.text = "Clipboard is empty"
		# Clear hint after 3 seconds
		get_tree().create_timer(3.0).timeout.connect(func(): crosshair_hint.text = "")
	)
	page.add_child(import_btn)

	var back := _make_button("Back", 100.0)
	back.offset_top = 240.0 * s
	back.offset_bottom = 260.0 * s
	back.offset_left = 170.0 * s
	back.offset_right = 270.0 * s
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

	# Status hint for import/export feedback
	var outline_hint := Label.new()
	outline_hint.text = ""
	outline_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	outline_hint.add_theme_font_override("font", MUNRO_FONT)
	outline_hint.add_theme_font_size_override("font_size", int(10 * s))
	outline_hint.add_theme_color_override("font_color", Color(1.0, 0.7, 0.3))
	outline_hint.set_anchors_preset(Control.PRESET_CENTER)
	outline_hint.offset_left = -260.0 * s
	outline_hint.offset_right = 260.0 * s
	outline_hint.offset_top = 200.0 * s
	outline_hint.offset_bottom = 216.0 * s
	page.add_child(outline_hint)

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

	var reset := _make_button("Reset", 100.0)
	reset.offset_top = 240.0 * s
	reset.offset_bottom = 260.0 * s
	reset.offset_left = -160.0 * s
	reset.offset_right = -60.0 * s
	reset.pressed.connect(func():
		for k in _block_outline_defaults:
			if block_outline_node:
				block_outline_node.set(k, _block_outline_defaults[k])
		_block_outline_refresh_controls(controls)
		_schedule_save())
	page.add_child(reset)

	var export_btn := _make_button("Export", 100.0)
	export_btn.offset_top = 240.0 * s
	export_btn.offset_bottom = 260.0 * s
	export_btn.offset_left = -50.0 * s
	export_btn.offset_right = 50.0 * s
	export_btn.pressed.connect(func():
		var code := _export_block_outline_code()
		if code != "":
			DisplayServer.clipboard_set(code)
			outline_hint.text = "Code copied to clipboard!"
		else:
			outline_hint.text = "Export failed"
		# Clear hint after 3 seconds
		get_tree().create_timer(3.0).timeout.connect(func(): outline_hint.text = "")
	)
	page.add_child(export_btn)

	var import_btn := _make_button("Import", 100.0)
	import_btn.offset_top = 240.0 * s
	import_btn.offset_bottom = 260.0 * s
	import_btn.offset_left = 60.0 * s
	import_btn.offset_right = 160.0 * s
	import_btn.pressed.connect(func():
		var code := DisplayServer.clipboard_get()
		if code != "":
			if _import_block_outline_code(code):
				_block_outline_refresh_controls(controls)
				outline_hint.text = "Code imported successfully!"
			else:
				outline_hint.text = "Invalid code format"
		else:
			outline_hint.text = "Clipboard is empty"
		# Clear hint after 3 seconds
		get_tree().create_timer(3.0).timeout.connect(func(): outline_hint.text = "")
	)
	page.add_child(import_btn)

	var back := _make_button("Back", 100.0)
	back.offset_top = 240.0 * s
	back.offset_bottom = 260.0 * s
	back.offset_left = 170.0 * s
	back.offset_right = 270.0 * s
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

func _make_spin(value: float, min_value: float, max_value: float, step: float, field: String) -> Control:
	return _make_slider(value, min_value, max_value, step, func(v: float): _cross_set(field, v))

func _make_spin_outline(value: float, min_value: float, max_value: float, step: float, field: String) -> Control:
	return _make_slider(value, min_value, max_value, step, func(v: float): _block_outline_set(field, v))

func _make_slider(value: float, min_value: float, max_value: float, step: float, setter: Callable, suffix := "") -> Control:
	var s := _ui_scale()
	var box := Control.new()
	var slider := HSlider.new()
	slider.set_anchors_preset(Control.PRESET_FULL_RECT)
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step = step
	slider.value = value
	slider.custom_minimum_size = Vector2(0, 20 * s)
	var normal := _slider_track_style()
	var hover := _slider_track_style()
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	slider.add_theme_stylebox_override("slider", normal)
	# Suppress the separate "grabber_area" fill (Godot would draw a second
	# background for the portion left of the grabber) - we only want one.
	slider.add_theme_stylebox_override("grabber_area", StyleBoxEmpty.new())
	slider.add_theme_stylebox_override("grabber_area_highlight", StyleBoxEmpty.new())
	slider.add_theme_icon_override("grabber", _scaled_thumb_tex())
	slider.add_theme_icon_override("grabber_highlight", _scaled_thumb_tex())
	slider.add_theme_constant_override("grabber_offset", 0)
	slider.add_theme_constant_override("center_grabber", 1)
	# Highlight the whole track (not just the filled portion left of the
	# grabber) when the slider is hovered or focused.
	var set_highlight := func(on: bool):
		slider.add_theme_stylebox_override("slider", hover if on else normal)
	slider.mouse_entered.connect(set_highlight.bind(true))
	slider.mouse_exited.connect(set_highlight.bind(false))
	slider.focus_entered.connect(set_highlight.bind(true))
	slider.focus_exited.connect(set_highlight.bind(false))
	var label := Label.new()
	label.set_anchors_preset(Control.PRESET_FULL_RECT)
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	label.add_theme_font_override("font", MUNRO_FONT)
	label.add_theme_font_size_override("font_size", int(10 * s))
	label.add_theme_color_override("font_color", Color(0.85, 0.9, 1.0))
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	label.text = _format_slider_value(value, step) + suffix
	slider.value_changed.connect(func(v: float):
		label.text = _format_slider_value(v, step) + suffix
		setter.call(v))
	box.add_child(slider)
	box.add_child(label)
	box.set_meta("slider", slider)
	return box

func _slider_track_style() -> StyleBoxTexture:
	# The slider draws its track at the stylebox's minimum size height (the
	# sum of the texture margins), so we pre-scale the texture up to ui_scale
	# and use half the scaled height as each margin. The track ends up exactly
	# 20 * ui_scale tall (matching the buttons) and the top/bottom borders tile
	# the texture with no overlap. Using raw margins larger than the 20 px
	# source texture (UI scales 3-4) makes the nine-patch sample out of bounds
	# and draw a black seam in the middle of the track.
	var s := _ui_scale()
	var img := SLIDER_TRACK_TEX.get_image()
	img.resize(maxi(1, roundi(SLIDER_TRACK_TEX.get_width() * s)), maxi(1, roundi(SLIDER_TRACK_TEX.get_height() * s)), Image.INTERPOLATE_NEAREST)
	var style := StyleBoxTexture.new()
	style.texture = ImageTexture.create_from_image(img)
	style.texture_margin_left = 1.0
	style.texture_margin_right = 1.0
	var half := img.get_height() / 2.0
	style.texture_margin_top = half
	style.texture_margin_bottom = half
	return style

func _scaled_thumb_tex() -> Texture2D:
	# The grabber icon is drawn at native size, so scale it up to match the
	# stretched track/button height (20 * ui_scale), like everything else.
	var s := _ui_scale()
	var img := SLIDER_THUMB_TEX.get_image()
	img.resize(maxi(1, roundi(SLIDER_THUMB_TEX.get_width() * s)), maxi(1, roundi(SLIDER_THUMB_TEX.get_height() * s)), Image.INTERPOLATE_NEAREST)
	return ImageTexture.create_from_image(img)

func _format_slider_value(v: float, step: float) -> String:
	var decimals := 0
	var st := step
	while st < 1.0 and decimals < 6:
		st *= 10.0
		decimals += 1
	return ("%." + str(decimals) + "f") % v

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
		elif c.has_meta("slider"):
			c.get_meta("slider").value = v
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
		elif c.has_meta("slider"):
			c.get_meta("slider").value = v
		elif c is ColorPickerButton:
			c.color = v

func _build_lighting_page() -> Control:
	var dur := _make_slider(chunk_manager.get_day_duration(), 10.0, 600.0, 10.0,
		func(v: float):
			chunk_manager.set_day_duration(v)
			_schedule_save(), " s")
	var dur_reset := func():
		dur.get_meta("slider").value = _default_day_duration
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

	var ao_strength := _make_slider(chunk_manager.get_ao_strength(), 0.0, 2.0, 0.05,
		func(v: float):
			chunk_manager.set_ao_strength(v)
			_schedule_save())
	var ao_strength_reset := func():
		ao_strength.get_meta("slider").value = _default_ao_strength
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

	var contrast := _make_slider(chunk_manager.get_contrast(), 0.0, 2.0, 0.05,
		func(v: float):
			chunk_manager.set_contrast(v)
			_schedule_save())
	var contrast_reset := func():
		contrast.get_meta("slider").value = _default_contrast
		chunk_manager.set_contrast(_default_contrast)
		_schedule_save()

	var saturation := _make_slider(chunk_manager.get_saturation(), 0.0, 2.0, 0.05,
		func(v: float):
			chunk_manager.set_saturation(v)
			_schedule_save())
	var saturation_reset := func():
		saturation.get_meta("slider").value = _default_saturation
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
	var rd := _make_slider(chunk_manager.get_render_distance(), 2.0, 64.0, 1.0,
		func(v: float):
			chunk_manager.set_render_distance(int(v))
			_schedule_save(), " chunks")
	var rd_reset := func():
		rd.get_meta("slider").value = _default_render_distance
		chunk_manager.set_render_distance(_default_render_distance)
		_schedule_save()

	var lod_dist := _make_slider(chunk_manager.get_lod_distance(), 0.0, 64.0, 1.0,
		func(v: float):
			chunk_manager.set_lod_distance(int(v))
			_schedule_save(), " chunks")
	var lod_reset := func():
		lod_dist.get_meta("slider").value = _default_lod_distance
		chunk_manager.set_lod_distance(_default_lod_distance)
		_schedule_save()

	var lod_detail := _make_slider(chunk_manager.get_lod_detail_level(), 0.125, 1.0, 0.005,
		func(v: float):
			chunk_manager.set_lod_detail_level(v)
			_schedule_save())
	var lod_detail_reset := func():
		lod_detail.get_meta("slider").value = _default_lod_detail
		chunk_manager.set_lod_detail_level(_default_lod_detail)
		_schedule_save()

	var far_lod_dist := _make_slider(chunk_manager.get_far_lod_distance(), 0.0, 64.0, 1.0,
		func(v: float):
			chunk_manager.set_far_lod_distance(int(v))
			_schedule_save(), " chunks")
	var far_lod_reset := func():
		far_lod_dist.get_meta("slider").value = _default_far_lod_distance
		chunk_manager.set_far_lod_distance(_default_far_lod_distance)
		_schedule_save()

	var far_lod_detail := _make_slider(chunk_manager.get_far_lod_detail_level(), 0.125, 1.0, 0.005,
		func(v: float):
			chunk_manager.set_far_lod_detail_level(v)
			_schedule_save())
	var far_lod_detail_reset := func():
		far_lod_detail.get_meta("slider").value = _default_far_lod_detail
		chunk_manager.set_far_lod_detail_level(_default_far_lod_detail)
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

	# Viewport.msaa_3d enum values (0=disabled, 1=2x, 2=4x, 3=8x) double as
	# the label list indices, so the cycled index is the property value.
	var msaa_btn := _make_button("", 180.0)
	var msaa_names := ["Off", "2x", "4x", "8x"]
	msaa_btn.text = msaa_names[get_viewport().msaa_3d]
	msaa_btn.pressed.connect(func():
		var next: int = (int(get_viewport().msaa_3d) + 1) % msaa_names.size()
		get_viewport().msaa_3d = next as Viewport.MSAA
		msaa_btn.text = msaa_names[next]
		_schedule_save())
	var msaa_reset := func():
		get_viewport().msaa_3d = _default_msaa_3d as Viewport.MSAA
		msaa_btn.text = msaa_names[_default_msaa_3d]
		_schedule_save()

	var mipmap_bias := _make_slider(chunk_manager.get_mipmap_bias(), -4.0, 4.0, 0.01,
		func(v: float):
			chunk_manager.set_mipmap_bias(v)
			_schedule_save())
	mipmap_bias.get_meta("slider").editable = chunk_manager.get_mipmaps_enabled()
	var mipmap_bias_reset := func():
		mipmap_bias.get_meta("slider").value = _default_mipmap_bias
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
		mipmap_bias.get_meta("slider").editable = next
		_schedule_save())
	var mipmaps_reset := func():
		chunk_manager.set_mipmaps_enabled(_default_mipmaps_enabled)
		mipmaps_btn.text = "On" if _default_mipmaps_enabled else "Off"
		mipmap_bias.get_meta("slider").editable = _default_mipmaps_enabled
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

	var compression_btn := Button.new()
	var compression_enabled: bool = chunk_manager.get_compression_enabled()
	compression_btn.text = "On" if compression_enabled else "Off"
	_style_button(compression_btn, 180.0)
	compression_btn.pressed.connect(func():
		var next: bool = not chunk_manager.get_compression_enabled()
		chunk_manager.set_compression_enabled(next)
		compression_btn.text = "On" if next else "Off"
		_schedule_save())
	var compression_reset := func():
		chunk_manager.set_compression_enabled(_default_compression_enabled)
		compression_btn.text = "On" if _default_compression_enabled else "Off"
		_schedule_save()

	var fps_cap := _make_slider(float(_fps_cap), 0.0, 300.0, 1.0,
		func(v: float):
			_fps_cap = int(v)
			Engine.max_fps = _fps_cap
			_schedule_save(), " FPS")
	var fps_cap_reset := func():
		fps_cap.get_meta("slider").value = _default_fps_cap
		_fps_cap = _default_fps_cap
		Engine.max_fps = _default_fps_cap
		_schedule_save()

	return _build_option_page("RENDER", [
		["Render Distance", rd, rd_reset],
		["LOD Distance", lod_dist, lod_reset],
		["LOD Detail Level", lod_detail, lod_detail_reset],
		["Far LOD Distance", far_lod_dist, far_lod_reset],
		["Far LOD Detail Level", far_lod_detail, far_lod_detail_reset],
		["Fog Mode", fog_mode_btn, fog_mode_reset],
		["God Rays", godrays_btn, godrays_reset],
		["MSAA 3D", msaa_btn, msaa_reset],
		["Mipmaps", mipmaps_btn, mipmaps_reset],
		["Mipmap Bias", mipmap_bias, mipmap_bias_reset],
		["Textures", textures_btn, textures_reset],
		["Compression", compression_btn, compression_reset],
		["FPS Cap", fps_cap, fps_cap_reset],
	], "settings")

# CONTROLS page: one row per rebindable action. Clicking a binding button arms
# capture; the next key/button press replaces the action's events (Escape
# cancels). Every row gets a reset to the pristine project.godot binding.
func _build_controls_page() -> Control:
	var s := _ui_scale()
	var rows: Array = []
	for cb in CONTROL_BINDINGS:
		var action: String = cb[0]
		var btn := _make_button(_binding_text(action))
		_control_buttons[action] = btn
		btn.pressed.connect(func(a := action, b := btn):
			_capturing_action = a
			b.text = " <Press> "
			_set_controls_hint(""))
		rows.append([cb[1], btn, func(a := action): _reset_control(a)])

	var page := _build_option_page("CONTROLS", rows, "settings", 40.0)

	# Conflict/status hint shown above the Reset All row (hidden until used).
	var hint := Label.new()
	hint.text = ""
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.add_theme_font_override("font", MUNRO_FONT)
	hint.add_theme_font_size_override("font_size", int(10 * s))
	hint.add_theme_color_override("font_color", Color(1.0, 0.7, 0.3))
	hint.set_anchors_preset(Control.PRESET_CENTER)
	hint.offset_left = -260.0 * s
	hint.offset_right = 260.0 * s
	hint.offset_top = 88.0 * s
	hint.offset_bottom = 104.0 * s
	_controls_hint = hint
	page.add_child(hint)

	# Reset All: restore every action's pristine project.godot binding.
	var reset_all := _make_button("Reset All", 160.0)
	reset_all.set_anchors_preset(Control.PRESET_CENTER)
	reset_all.offset_left = -80.0 * s
	reset_all.offset_right = 80.0 * s
	reset_all.offset_top = 108.0 * s
	reset_all.offset_bottom = 128.0 * s
	reset_all.pressed.connect(_reset_all_controls)
	page.add_child(reset_all)

	return page

func _set_controls_hint(text: String) -> void:
	if _controls_hint != null:
		_controls_hint.text = text

# Restore every rebindable action to its pristine project.godot binding.
func _reset_all_controls() -> void:
	_capturing_action = ""
	for cb in CONTROL_BINDINGS:
		InputMap.action_erase_events(cb[0])
		for e in _controls_defaults[cb[0]]:
			InputMap.action_add_event(cb[0], e)
		var btn: Button = _control_buttons.get(cb[0])
		if btn != null:
			btn.text = _binding_text(cb[0])
	_schedule_save()

# Human-readable label for an action's first binding (key name or mouse side).
func _binding_text(action_name: String) -> String:
	var events := InputMap.action_get_events(action_name)
	if events.is_empty():
		return "Unbound"
	return _event_label(events[0])

func _event_label(e: InputEvent) -> String:
	if e is InputEventKey:
		var code: int = e.keycode
		if code == 0:
			code = e.physical_keycode
		return OS.get_keycode_string(code)
	if e is InputEventMouseButton:
		match e.button_index:
			MOUSE_BUTTON_LEFT:
				return "Left Click"
			MOUSE_BUTTON_RIGHT:
				return "Right Click"
			MOUSE_BUTTON_MIDDLE:
				return "Middle Click"
	return "Button " + String.num_int64(e.button_index)

func _reset_control(action_name: String) -> void:
	if _capturing_action == action_name:
		_capturing_action = ""
	InputMap.action_erase_events(action_name)
	for e in _controls_defaults[action_name]:
		InputMap.action_add_event(action_name, e)
	var btn: Button = _control_buttons.get(action_name)
	if btn != null:
		btn.text = _binding_text(action_name)
	_schedule_save()

# Apply a captured event (or null to cancel) to the armed action, then refresh
# its button. Rebinding wipes the action's previous events. A conflict (the
# key/button is already bound to another action) is rejected with a hint.
func _finish_capture(new_event: InputEvent) -> void:
	var action := _capturing_action
	_capturing_action = ""
	if new_event != null and action != "":
		var other := _conflicting_action(action, new_event)
		if other != "":
			_set_controls_hint("%s is already bound to %s" % [_event_label(new_event), _friendly_action(other)])
		else:
			InputMap.action_erase_events(action)
			InputMap.action_add_event(action, new_event)
			_schedule_save()
	var btn: Button = _control_buttons.get(action)
	if btn != null:
		btn.text = _binding_text(action)

# Human-readable name of a conflicting action (from CONTROL_BINDINGS).
func _friendly_action(action_name: String) -> String:
	for cb in CONTROL_BINDINGS:
		if cb[0] == action_name:
			return cb[1]
	return action_name

# Return the name of another action whose first binding matches `new_event`'s
# key/button (physical-key aware), or "" if the binding is free.
func _conflicting_action(current_action: String, new_event: InputEvent) -> String:
	var want_label := _binding_label(new_event)
	for cb in CONTROL_BINDINGS:
		var a: String = cb[0]
		if a == current_action:
			continue
		var events := InputMap.action_get_events(a)
		if events.is_empty():
			continue
		for e in events:
			if want_label != "" and want_label == _binding_label(e):
				return a
	return ""

# Compare two events by the same identity used in _binding_text so a new key
# matches an existing binding even if the keycode/physical split differs.
func _binding_label(e: InputEvent) -> String:
	if e is InputEventKey:
		var code: int = e.keycode
		if code == 0:
			code = e.physical_keycode
		return "k:" + String.num_int64(code)
	if e is InputEventMouseButton:
		return "m:" + String.num_int64(e.button_index)
	return ""

# Consume the next key/button press while a binding is being captured, or the
# Escape key to cancel. Returns true when the event was used as a capture.
func _capture_binding(event: InputEvent) -> bool:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_ESCAPE or event.physical_keycode == KEY_ESCAPE:
			_finish_capture(null)
			return true
		var ne := InputEventKey.new()
		ne.keycode = event.keycode
		ne.physical_keycode = event.physical_keycode
		if ne.keycode == 0 and ne.physical_keycode == 0:
			return true
		_finish_capture(ne)
		return true
	if event is InputEventMouseButton and event.pressed:
		var ne := InputEventMouseButton.new()
		ne.button_index = event.button_index
		_finish_capture(ne)
		return true
	return false

# Flatten an action's bindings to config strings ("k:<keycode>:<physical>",
# "m:<button_index>") so ConfigFile round-trips them across sessions.
func _serialize_action_events(action_name: String) -> Array:
	var out: Array = []
	for e in InputMap.action_get_events(action_name):
		if e is InputEventKey:
			out.append("k:%d:%d" % [e.keycode, e.physical_keycode])
		elif e is InputEventMouseButton:
			out.append("m:%d" % e.button_index)
	return out

func _apply_action_events(action_name: String, data: Array) -> void:
	InputMap.action_erase_events(action_name)
	for entry in data:
		var seg: PackedStringArray = String(entry).split(":")
		if seg[0] == "k" and seg.size() >= 3:
			var ne := InputEventKey.new()
			ne.keycode = int(seg[1]) as Key
			ne.physical_keycode = int(seg[2]) as Key
			InputMap.action_add_event(action_name, ne)
		elif seg[0] == "m" and seg.size() >= 2:
			var ne := InputEventMouseButton.new()
			ne.button_index = int(seg[1]) as MouseButton
			InputMap.action_add_event(action_name, ne)

func _build_skin_maker_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	_skin_bg = ColorRect.new()
	_skin_bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	_skin_bg.mouse_filter = Control.MOUSE_FILTER_STOP
	page.add_child(_skin_bg)

	_skin_title = _make_title("SKIN MAKER")
	_skin_title.set_anchors_preset(Control.PRESET_TOP_WIDE)
	_skin_title.offset_top = 20.0 * s
	_skin_title.offset_bottom = 60.0 * s
	page.add_child(_skin_title)

	_skin_picker = ColorPicker.new()
	# PickerShapeType.VHS_CIRCLE = 2 (full colour wheel; enum symbols not
	# exposed in GDScript, so cast the int to the enum type).
	_skin_picker.picker_shape = 2 as ColorPicker.PickerShapeType
	_skin_picker.edit_alpha = false
	_skin_picker.color_modes_visible = false
	_skin_picker.hex_visible = false
	_skin_picker.color = Color.WHITE
	# The ColorPicker's content is drawn at its natural theme size (measured
	# 290x478 px) regardless of GUI scale, so size the box in fixed px large
	# enough to contain it and place the hex label below its real bottom edge.
	_skin_picker.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_skin_picker.offset_left = 20.0
	_skin_picker.offset_top = 20.0
	_skin_picker.offset_right = 360.0
	_skin_picker.offset_bottom = 20.0 + 478.0
	page.add_child(_skin_picker)

	var hex_label := Label.new()
	hex_label.text = "#FFFFFF"
	hex_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hex_label.add_theme_font_override("font", MUNRO_FONT)
	hex_label.add_theme_font_size_override("font_size", int(14 * s))
	hex_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	hex_label.offset_left = -120.0
	hex_label.offset_right = 120.0
	hex_label.offset_top = 60.0 * s + 8.0
	hex_label.offset_bottom = 60.0 * s + 38.0
	_skin_picker.color_changed.connect(func(c: Color):
		hex_label.text = "#" + c.to_html(false)
		_skin_preview.set_paint_color(c))
	page.add_child(hex_label)
	_skin_hex = hex_label

	_skin_preview = (preload("res://skin_preview.gd") as GDScript).new()
	_skin_preview.name = "SkinPreview"
	_skin_preview.set_anchors_preset(Control.PRESET_CENTER)
	_skin_preview.offset_left = -260.0
	_skin_preview.offset_right = 260.0
	_skin_preview.offset_top = -220.0
	_skin_preview.offset_bottom = 220.0
	page.add_child(_skin_preview)

	var hint := Label.new()
	hint.text = "ESC to exit"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_LEFT
	hint.add_theme_font_override("font", MUNRO_FONT)
	hint.add_theme_font_size_override("font_size", int(13 * s))
	hint.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	hint.offset_left = 12.0
	hint.offset_top = -38.0
	hint.offset_right = 200.0
	hint.offset_bottom = -12.0
	page.add_child(hint)
	_skin_hint = hint

	var toggle := Button.new()
	toggle.name = "DarkModeToggle"
	toggle.toggle_mode = true
	toggle.button_pressed = _skin_dark_mode
	toggle.add_theme_font_override("font", MUNRO_FONT)
	toggle.add_theme_font_size_override("font_size", int(14 * s))
	toggle.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	toggle.offset_left = -190.0
	toggle.offset_right = -12.0
	toggle.offset_top = 20.0 * s
	toggle.offset_bottom = 20.0 * s + 40.0
	toggle.pressed.connect(func():
		_skin_dark_mode = not _skin_dark_mode
		_apply_skin_palette()
		_schedule_save())
	page.add_child(toggle)
	_skin_toggle = toggle

	var uv_toggle := Button.new()
	uv_toggle.name = "UvOverlayToggle"
	uv_toggle.toggle_mode = true
	uv_toggle.add_theme_font_override("font", MUNRO_FONT)
	uv_toggle.add_theme_font_size_override("font_size", int(14 * s))
	uv_toggle.text = "UV OVERLAY"
	uv_toggle.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	uv_toggle.offset_left = -190.0
	uv_toggle.offset_right = -12.0
	uv_toggle.offset_top = 20.0 * s + 48.0
	uv_toggle.offset_bottom = 20.0 * s + 88.0
	uv_toggle.pressed.connect(func():
		_skin_preview.set_uv_overlay(uv_toggle.button_pressed))
	page.add_child(uv_toggle)
	_skin_uv_toggle = uv_toggle

	var undo_btn := Button.new()
	undo_btn.name = "PaintUndo"
	undo_btn.text = "UNDO"
	undo_btn.disabled = true
	undo_btn.add_theme_font_override("font", MUNRO_FONT)
	undo_btn.add_theme_font_size_override("font_size", int(14 * s))
	undo_btn.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	undo_btn.offset_left = -190.0
	undo_btn.offset_right = -12.0
	undo_btn.offset_top = 20.0 * s + 144.0
	undo_btn.offset_bottom = 20.0 * s + 184.0
	undo_btn.pressed.connect(func(): _skin_preview.undo_last())
	_skin_preview.paint_history_changed.connect(
		func(has_undo: bool): undo_btn.disabled = not has_undo)
	page.add_child(undo_btn)
	_skin_undo_btn = undo_btn

	var tool_btn := Button.new()
	tool_btn.name = "SkinTool"
	tool_btn.text = "DRAW"
	var tool_names := ["DRAW", "FILL", "BOX"]
	tool_btn.add_theme_font_override("font", MUNRO_FONT)
	tool_btn.add_theme_font_size_override("font_size", int(14 * s))
	tool_btn.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	tool_btn.offset_left = -190.0
	tool_btn.offset_right = -12.0
	tool_btn.offset_top = 20.0 * s + 96.0
	tool_btn.offset_bottom = 20.0 * s + 136.0
	tool_btn.pressed.connect(func():
		var idx := tool_names.find(tool_btn.text)
		idx = (idx + 1) % tool_names.size()
		tool_btn.text = tool_names[idx]
		_skin_preview.set_tool(idx))
	page.add_child(tool_btn)
	_skin_tool_btn = tool_btn

	var name_edit := LineEdit.new()
	name_edit.name = "SkinName"
	name_edit.text = "myskin"
	name_edit.placeholder_text = "skin name"
	name_edit.max_length = 32
	name_edit.add_theme_font_override("font", MUNRO_FONT)
	name_edit.add_theme_font_size_override("font_size", int(13 * s))
	name_edit.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	name_edit.offset_left = -190.0
	name_edit.offset_right = -12.0
	name_edit.offset_top = -140.0
	name_edit.offset_bottom = -108.0
	page.add_child(name_edit)
	_skin_name_edit = name_edit

	var save_btn := Button.new()
	save_btn.name = "SkinSave"
	save_btn.text = "SAVE"
	save_btn.add_theme_font_override("font", MUNRO_FONT)
	save_btn.add_theme_font_size_override("font_size", int(14 * s))
	save_btn.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	save_btn.offset_left = -190.0
	save_btn.offset_right = -12.0
	save_btn.offset_top = -100.0
	save_btn.offset_bottom = -60.0
	save_btn.pressed.connect(func():
		var skin_name := _sanitize_skin_name(name_edit.text)
		DirAccess.make_dir_recursive_absolute("user://skins")
		if _skin_preview.save_skin("user://skins/" + skin_name + ".png"):
			name_edit.text = skin_name
			# Save the noise value alongside the skin so LOAD can restore it.
			_save_skin_sidecar(skin_name, _skin_noise))
	page.add_child(save_btn)
	_skin_save_btn = save_btn

	var load_btn := Button.new()
	load_btn.name = "SkinLoad"
	load_btn.text = "LOAD"
	load_btn.add_theme_font_override("font", MUNRO_FONT)
	load_btn.add_theme_font_size_override("font_size", int(14 * s))
	load_btn.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	load_btn.offset_left = -190.0
	load_btn.offset_right = -12.0
	load_btn.offset_top = -52.0
	load_btn.offset_bottom = -12.0
	load_btn.pressed.connect(_open_skin_gallery)
	page.add_child(load_btn)
	_skin_load_btn = load_btn

	var noise_row := HBoxContainer.new()
	noise_row.name = "NoiseRow"
	noise_row.alignment = BoxContainer.ALIGNMENT_CENTER
	noise_row.add_theme_constant_override("separation", int(10 * s))
	noise_row.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	noise_row.offset_left = -200.0
	noise_row.offset_right = 200.0
	noise_row.offset_top = -52.0
	noise_row.offset_bottom = -12.0

	var noise_label := Label.new()
	noise_label.text = "NOISE"
	noise_label.add_theme_font_override("font", MUNRO_FONT)
	noise_label.add_theme_font_size_override("font_size", int(13 * s))
	noise_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	noise_label.custom_minimum_size = Vector2(50 * s, 0.0)

	var noise_slider := HSlider.new()
	noise_slider.min_value = 0.0
	noise_slider.max_value = 100.0
	noise_slider.step = 1.0
	noise_slider.value = _skin_noise
	noise_slider.custom_minimum_size = Vector2(120 * s, 0.0)
	noise_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	noise_slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var noise_value := Label.new()
	noise_value.text = str(int(_skin_noise))
	noise_value.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	noise_value.add_theme_font_override("font", MUNRO_FONT)
	noise_value.add_theme_font_size_override("font_size", int(13 * s))
	noise_value.custom_minimum_size = Vector2(32 * s, 0.0)

	noise_slider.value_changed.connect(func(v: float):
		noise_value.text = str(int(v))
		_skin_noise = v
		_skin_preview.set_noise(v)
		_schedule_save())

	noise_row.add_child(noise_label)
	noise_row.add_child(noise_slider)
	noise_row.add_child(noise_value)
	page.add_child(noise_row)
	_skin_noise_label = noise_label
	_skin_noise_slider = noise_slider
	_skin_noise_value = noise_value

	page.add_child(_build_skin_gallery(s))

	_apply_skin_palette()
	return page

func _build_block_maker_page() -> Control:
	var s := _ui_scale()
	var page := Control.new()
	page.set_anchors_preset(Control.PRESET_FULL_RECT)
	page.mouse_filter = Control.MOUSE_FILTER_IGNORE

	_block_bg = ColorRect.new()
	_block_bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	_block_bg.mouse_filter = Control.MOUSE_FILTER_STOP
	page.add_child(_block_bg)

	_block_title = _make_title("BLOCK MAKER")
	_block_title.set_anchors_preset(Control.PRESET_TOP_WIDE)
	_block_title.offset_top = 20.0 * s
	_block_title.offset_bottom = 60.0 * s
	page.add_child(_block_title)

	_block_picker = ColorPicker.new()
	_block_picker.picker_shape = 2 as ColorPicker.PickerShapeType
	_block_picker.edit_alpha = false
	_block_picker.color_modes_visible = false
	_block_picker.hex_visible = false
	_block_picker.color = Color.WHITE
	_block_picker.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_block_picker.offset_left = 20.0
	_block_picker.offset_top = 20.0
	_block_picker.offset_right = 360.0
	_block_picker.offset_bottom = 20.0 + 478.0
	page.add_child(_block_picker)

	var hex_label := Label.new()
	hex_label.text = "#FFFFFF"
	hex_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hex_label.add_theme_font_override("font", MUNRO_FONT)
	hex_label.add_theme_font_size_override("font_size", int(14 * s))
	hex_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	hex_label.offset_left = -120.0
	hex_label.offset_right = 120.0
	hex_label.offset_top = 60.0 * s + 8.0
	hex_label.offset_bottom = 60.0 * s + 38.0
	_block_picker.color_changed.connect(func(c: Color):
		hex_label.text = "#" + c.to_html(false)
		_block_color = c
		if _block_preview != null:
			_block_preview.set_color(c))
	page.add_child(hex_label)
	_block_hex = hex_label

	_block_preview = (preload("res://block_preview.gd") as GDScript).new()
	_block_preview.name = "BlockPreview"
	_block_preview.set_anchors_preset(Control.PRESET_CENTER)
	_block_preview.offset_left = -200.0
	_block_preview.offset_right = 200.0
	_block_preview.offset_top = -180.0
	_block_preview.offset_bottom = 180.0
	page.add_child(_block_preview)

	var dark_toggle := Button.new()
	dark_toggle.name = "BlockDarkModeToggle"
	dark_toggle.toggle_mode = true
	dark_toggle.button_pressed = _skin_dark_mode
	dark_toggle.text = "DARK MODE" if not _skin_dark_mode else "LIGHT MODE"
	dark_toggle.add_theme_font_override("font", MUNRO_FONT)
	dark_toggle.add_theme_font_size_override("font_size", int(14 * s))
	dark_toggle.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	dark_toggle.offset_left = -190.0
	dark_toggle.offset_right = -12.0
	dark_toggle.offset_top = 20.0 * s
	dark_toggle.offset_bottom = 20.0 * s + 40.0
	dark_toggle.pressed.connect(func():
		_skin_dark_mode = not _skin_dark_mode
		dark_toggle.text = "DARK MODE" if not _skin_dark_mode else "LIGHT MODE"
		_apply_block_palette()
		_schedule_save())
	page.add_child(dark_toggle)
	_block_dark_toggle = dark_toggle

	var uv_toggle := Button.new()
	uv_toggle.name = "BlockUvOverlayToggle"
	uv_toggle.toggle_mode = true
	uv_toggle.add_theme_font_override("font", MUNRO_FONT)
	uv_toggle.add_theme_font_size_override("font_size", int(14 * s))
	uv_toggle.text = "UV OVERLAY"
	uv_toggle.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	uv_toggle.offset_left = -190.0
	uv_toggle.offset_right = -12.0
	uv_toggle.offset_top = 20.0 * s + 48.0
	uv_toggle.offset_bottom = 20.0 * s + 88.0
	uv_toggle.pressed.connect(func():
		if _block_preview != null:
			_block_preview.set_uv_overlay(uv_toggle.button_pressed))
	page.add_child(uv_toggle)
	_block_uv_toggle = uv_toggle

	var hint := Label.new()
	hint.text = "ESC to exit"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_LEFT
	hint.add_theme_font_override("font", MUNRO_FONT)
	hint.add_theme_font_size_override("font_size", int(13 * s))
	hint.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	hint.offset_left = 12.0
	hint.offset_top = -38.0
	hint.offset_right = 200.0
	hint.offset_bottom = -12.0
	page.add_child(hint)
	_block_hint = hint

	var undo_btn := Button.new()
	undo_btn.name = "BlockUndo"
	undo_btn.text = "UNDO"
	undo_btn.disabled = true
	undo_btn.add_theme_font_override("font", MUNRO_FONT)
	undo_btn.add_theme_font_size_override("font_size", int(14 * s))
	undo_btn.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	undo_btn.offset_left = -190.0
	undo_btn.offset_right = -12.0
	undo_btn.offset_top = 20.0 * s + 144.0
	undo_btn.offset_bottom = 20.0 * s + 184.0
	undo_btn.pressed.connect(func(): 
		if _block_preview != null:
			_block_preview.undo())
	_block_preview.paint_history_changed.connect(
		func(has_undo: bool): undo_btn.disabled = not has_undo)
	page.add_child(undo_btn)
	_block_undo_btn = undo_btn

	var tool_btn := Button.new()
	tool_btn.name = "BlockTool"
	tool_btn.text = "DRAW"
	var tool_names := ["DRAW", "FILL", "BOX"]
	tool_btn.add_theme_font_override("font", MUNRO_FONT)
	tool_btn.add_theme_font_size_override("font_size", int(14 * s))
	tool_btn.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	tool_btn.offset_left = -190.0
	tool_btn.offset_right = -12.0
	tool_btn.offset_top = 20.0 * s + 96.0
	tool_btn.offset_bottom = 20.0 * s + 136.0
	tool_btn.pressed.connect(func():
		var idx := tool_names.find(tool_btn.text)
		idx = (idx + 1) % tool_names.size()
		tool_btn.text = tool_names[idx]
		_block_tool = tool_names[idx]
		if _block_preview != null:
			_block_preview.set_tool(_block_tool))
	page.add_child(tool_btn)
	_block_tool_btn = tool_btn

	var name_edit := LineEdit.new()
	name_edit.name = "BlockName"
	name_edit.text = "myblock"
	name_edit.placeholder_text = "block name"
	name_edit.max_length = 32
	name_edit.add_theme_font_override("font", MUNRO_FONT)
	name_edit.add_theme_font_size_override("font_size", int(13 * s))
	name_edit.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	name_edit.offset_left = -190.0
	name_edit.offset_right = -12.0
	name_edit.offset_top = -140.0
	name_edit.offset_bottom = -108.0
	page.add_child(name_edit)
	_block_name_edit = name_edit

	var load_btn := Button.new()
	load_btn.name = "BlockLoad"
	load_btn.text = "LOAD"
	load_btn.add_theme_font_override("font", MUNRO_FONT)
	load_btn.add_theme_font_size_override("font_size", int(14 * s))
	load_btn.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	load_btn.offset_left = -190.0
	load_btn.offset_right = -12.0
	load_btn.offset_top = -52.0
	load_btn.offset_bottom = -12.0
	load_btn.pressed.connect(_open_block_gallery)
	page.add_child(load_btn)
	_block_load_btn = load_btn

	var save_btn := Button.new()
	save_btn.name = "BlockSave"
	save_btn.text = "SAVE"
	save_btn.add_theme_font_override("font", MUNRO_FONT)
	save_btn.add_theme_font_size_override("font_size", int(14 * s))
	save_btn.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
	save_btn.offset_left = -190.0
	save_btn.offset_right = -12.0
	save_btn.offset_top = -100.0
	save_btn.offset_bottom = -60.0
	save_btn.pressed.connect(func():
		var block_name := _sanitize_block_name(name_edit.text)
		DirAccess.make_dir_recursive_absolute("user://blocks")
		if _block_preview != null and _block_preview.save_block("user://blocks/" + block_name + ".png"):
			name_edit.text = block_name
			# Save the noise value alongside the block so LOAD can restore it.
			_save_block_sidecar(block_name, _block_noise))
	page.add_child(save_btn)
	_block_save_btn = save_btn

	var noise_row := HBoxContainer.new()
	noise_row.name = "NoiseRow"
	noise_row.alignment = BoxContainer.ALIGNMENT_CENTER
	noise_row.add_theme_constant_override("separation", int(10 * s))
	noise_row.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	noise_row.offset_left = -200.0
	noise_row.offset_right = 200.0
	noise_row.offset_top = -52.0
	noise_row.offset_bottom = -12.0

	var noise_label := Label.new()
	noise_label.text = "NOISE"
	noise_label.add_theme_font_override("font", MUNRO_FONT)
	noise_label.add_theme_font_size_override("font_size", int(13 * s))
	noise_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	noise_label.custom_minimum_size = Vector2(50 * s, 0.0)

	var noise_slider := HSlider.new()
	noise_slider.min_value = 0.0
	noise_slider.max_value = 100.0
	noise_slider.step = 1.0
	noise_slider.value = _block_noise
	noise_slider.custom_minimum_size = Vector2(120 * s, 0.0)
	noise_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	noise_slider.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var noise_value := Label.new()
	noise_value.text = str(int(_block_noise))
	noise_value.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	noise_value.add_theme_font_override("font", MUNRO_FONT)
	noise_value.add_theme_font_size_override("font_size", int(13 * s))
	noise_value.custom_minimum_size = Vector2(32 * s, 0.0)

	noise_slider.value_changed.connect(func(v: float):
		noise_value.text = str(int(v))
		_block_noise = v
		if _block_preview != null and _block_preview.has_method("set_noise"):
			_block_preview.set_noise(v)
		_schedule_save())

	noise_row.add_child(noise_label)
	noise_row.add_child(noise_slider)
	noise_row.add_child(noise_value)
	page.add_child(noise_row)
	_block_noise_label = noise_label
	_block_noise_slider = noise_slider
	_block_noise_value = noise_value

	page.add_child(_build_block_gallery(s))

	# Style buttons to match skin maker
	var dark := _skin_dark_mode
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK
	for btn in [_block_undo_btn, _block_tool_btn, _block_save_btn, _block_load_btn, _block_dark_toggle, _block_uv_toggle]:
		if btn != null:
			_style_skin_button(btn, fg_col, dark)

	_apply_block_palette()
	return page

func _sanitize_block_name(raw: String) -> String:
	var cleaned := ""
	for ch in raw.strip_edges().replace(" ", "_"):
		if ch.is_valid_identifier() or ch == "-":
			cleaned += ch
	return cleaned if not cleaned.is_empty() else "myblock"

# Sidecar <name>.json holds the noise value a block was saved with, so loading
# the PNG (which has the noise baked in) also restores the matching slider.
func _save_block_sidecar(block_name: String, noise: float) -> void:
	var f := FileAccess.open("user://blocks/" + block_name + ".json", FileAccess.WRITE)
	if f == null:
		return
	f.store_string(JSON.stringify({"noise": clampf(noise, 0.0, 100.0)}))
	f.close()

func _load_block_sidecar(block_name: String) -> float:
	var path := "user://blocks/" + block_name + ".json"
	if not FileAccess.file_exists(path):
		return 0.0
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return 0.0
	var json := JSON.new()
	if json.parse(f.get_as_text()) != OK:
		return 0.0
	var data: Variant = json.data
	if data is Dictionary and data.has("noise"):
		return clampf(float(data["noise"]), 0.0, 100.0)
	return 0.0

func _apply_block_palette() -> void:
	var s := _ui_scale()
	var dark := _skin_dark_mode  # Reuse skin dark mode setting
	var bg_col := Color(0.08, 0.08, 0.1) if dark else Color.WHITE
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK
	var hover_col := Color(0.72, 0.72, 0.72, 1) if dark else Color(0.35, 0.35, 0.35, 1)
	var hint_col := Color(0.75, 0.75, 0.75, 1) if dark else Color(0.2, 0.2, 0.2)
	
	if _block_bg:
		_block_bg.color = bg_col
	if _block_title:
		_block_title.add_theme_color_override("font_color", fg_col)
	if _block_hex:
		_block_hex.add_theme_color_override("font_color", fg_col)
	if _block_hint:
		_block_hint.add_theme_color_override("font_color", hint_col)
	if _block_picker:
		_block_picker.theme = _make_picker_theme(s, dark)
		_tint_picker_internals(_block_picker, fg_col, hover_col)
	if _block_dark_toggle:
		_block_dark_toggle.text = "LIGHT MODE" if dark else "DARK MODE"
		_style_skin_button(_block_dark_toggle, fg_col, dark)
	if _block_uv_toggle:
		_style_skin_button(_block_uv_toggle, fg_col, dark)
	if _block_noise_label:
		_block_noise_label.add_theme_color_override("font_color", hint_col)
	if _block_noise_value:
		_block_noise_value.add_theme_color_override("font_color", hint_col)
	
	# Update button colors
	for btn in [_block_undo_btn, _block_tool_btn, _block_save_btn, _block_load_btn]:
		if btn != null:
			_style_skin_button(btn, fg_col, dark)

func _sanitize_skin_name(raw: String) -> String:
	var cleaned := ""
	for ch in raw.strip_edges().replace(" ", "_"):
		if ch.is_valid_identifier() or ch == "-":
			cleaned += ch
	return cleaned if not cleaned.is_empty() else "myskin"

# Sidecar <name>.json holds the noise value a skin was saved with, so loading
# the PNG (which has the noise baked in) also restores the matching slider.
func _save_skin_sidecar(skin_name: String, noise: float) -> void:
	var f := FileAccess.open("user://skins/" + skin_name + ".json", FileAccess.WRITE)
	if f == null:
		return
	f.store_string(JSON.stringify({"noise": clampf(noise, 0.0, 100.0)}))
	f.close()

func _load_skin_sidecar(skin_name: String) -> float:
	var path := "user://skins/" + skin_name + ".json"
	if not FileAccess.file_exists(path):
		return 0.0
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return 0.0
	var json := JSON.new()
	if json.parse(f.get_as_text()) != OK:
		return 0.0
	var data: Variant = json.data
	if data is Dictionary and data.has("noise"):
		return clampf(float(data["noise"]), 0.0, 100.0)
	return 0.0

func _list_saved_skins() -> Array:
	var names: Array = []
	var dir := DirAccess.open("user://skins")
	if dir == null:
		return names
	dir.list_dir_begin()
	var f := dir.get_next()
	while f != "":
		if not dir.current_is_dir() and f.ends_with(".png"):
			names.append(f.get_basename())
		f = dir.get_next()
	names.sort()
	return names

func _open_skin_gallery() -> void:
	if _skin_gallery == null:
		return
	_refresh_skin_gallery()
	_skin_gallery.visible = true
	if _skin_gallery_timer:
		_skin_gallery_timer.start()

func _close_skin_gallery() -> void:
	if _skin_gallery_timer:
		_skin_gallery_timer.stop()
	if _skin_gallery:
		_skin_gallery.visible = false

func _refresh_skin_gallery() -> void:
	if _skin_gallery_grid == null:
		return
	for c in _skin_gallery_grid.get_children():
		c.free()
	_skin_gallery_spins.clear()
	var names := _list_saved_skins()
	if names.is_empty():
		var empty_hint := Label.new()
		empty_hint.text = "No saved skins yet — use SAVE to create one."
		empty_hint.add_theme_font_override("font", MUNRO_FONT)
		empty_hint.add_theme_font_size_override("font_size", int(14 * _ui_scale()))
		empty_hint.add_theme_color_override("font_color",
			Color(0.75, 0.75, 0.75, 1) if _skin_dark_mode else Color(0.2, 0.2, 0.2))
		_skin_gallery_grid.add_child(empty_hint)
		return
	for skin_name in names:
		_skin_gallery_grid.add_child(_make_skin_card(_ui_scale(), skin_name))

func _spin_gallery_models() -> void:
	if _skin_gallery != null and _skin_gallery.is_visible_in_tree():
		for h in _skin_gallery_spins:
			if is_instance_valid(h):
				h.rotate_y(0.03)
	if _block_gallery != null and _block_gallery.is_visible_in_tree():
		for h in _block_gallery_spins:
			if is_instance_valid(h):
				h.rotate_y(0.03)

func _make_skin_card(s: float, skin_name: String) -> VBoxContainer:
	var dark := _skin_dark_mode
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK
	var border_col := Color(0.35, 0.35, 0.4) if dark else Color(0.55, 0.55, 0.58)
	var card_bg := Color(0.14, 0.14, 0.17) if dark else Color(0.99, 0.99, 0.99)
	var hover_col := Color(0.22, 0.24, 0.3) if dark else Color(0.84, 0.88, 0.94)

	var card := VBoxContainer.new()
	card.name = "Card_" + skin_name
	card.add_theme_constant_override("separation", int(4 * s))
	# Keep the card at its 96-wide minimum so the preview button (and with it
	# the 96x96 viewport) stays exactly square. Without this the grid stretches
	# each cell horizontally and the viewport's render gets squished into a
	# wider-than-tall rect, smearing the texture on every cube face.
	card.size_flags_horizontal = Control.SIZE_SHRINK_CENTER

	var preview_btn := Button.new()
	preview_btn.custom_minimum_size = Vector2(96 * s, 96 * s)
	preview_btn.clip_text = true
	preview_btn.tooltip_text = "Load " + skin_name
	var sb := StyleBoxFlat.new()
	sb.bg_color = card_bg
	sb.set_border_width_all(int(2 * s))
	sb.border_color = border_col
	sb.set_corner_radius_all(int(4 * s))
	var hov := sb.duplicate()
	hov.bg_color = hover_col
	var pres := sb.duplicate()
	pres.bg_color = hover_col
	pres.border_color = fg_col
	preview_btn.add_theme_stylebox_override("normal", sb)
	preview_btn.add_theme_stylebox_override("hover", hov)
	preview_btn.add_theme_stylebox_override("pressed", pres)
	preview_btn.add_theme_stylebox_override("focus", pres)
	card.add_child(preview_btn)

	var model_box := _make_gallery_model_view(s, skin_name)
	model_box.set_anchors_preset(Control.PRESET_FULL_RECT)
	model_box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	preview_btn.add_child(model_box)

	var foot := HBoxContainer.new()
	foot.add_theme_constant_override("separation", int(4 * s))
	var name_btn := Button.new()
	name_btn.text = skin_name
	name_btn.clip_text = true
	name_btn.flat = true
	name_btn.tooltip_text = "Load " + skin_name
	name_btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_btn.add_theme_font_override("font", MUNRO_FONT)
	name_btn.add_theme_font_size_override("font_size", int(12 * s))
	name_btn.add_theme_color_override("font_color", fg_col)
	name_btn.add_theme_color_override("font_hover_color", fg_col)
	name_btn.add_theme_color_override("font_pressed_color", fg_col)
	foot.add_child(name_btn)
	var del_btn := Button.new()
	del_btn.text = "X"
	del_btn.flat = true
	del_btn.custom_minimum_size = Vector2(int(24 * s), 0)
	del_btn.tooltip_text = "Delete " + skin_name
	del_btn.add_theme_font_override("font", MUNRO_FONT)
	del_btn.add_theme_font_size_override("font_size", int(13 * s))
	del_btn.add_theme_color_override("font_color", Color(1, 0.42, 0.42))
	del_btn.add_theme_color_override("font_hover_color", Color(1, 0.6, 0.6))
	foot.add_child(del_btn)
	card.add_child(foot)

	preview_btn.pressed.connect(func(): _load_named_skin(skin_name))
	name_btn.pressed.connect(func(): _load_named_skin(skin_name))
	del_btn.pressed.connect(func(): _delete_named_skin(skin_name))
	return card

func _make_gallery_model_view(s: float, skin_name: String) -> SubViewportContainer:
	var box := SubViewportContainer.new()
	box.stretch = true
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var vp := SubViewport.new()
	vp.transparent_bg = true
	vp.msaa_3d = Viewport.MSAA_2X
	vp.size = Vector2i(int(96 * s), int(96 * s))
	box.add_child(vp)
	# The card button expands horizontally to fill its grid cell, so a fixed
	# 96x96 viewport would be squished into the non-square button and every
	# texture (and the whole scene) would render stretched. Keep the viewport
	# resolution matched to the container so there is never any stretch.
	box.resized.connect(func():
		var sz := box.size
		if sz.x > 1.0 and sz.y > 1.0:
			vp.size = Vector2i(int(sz.x), int(sz.y)))
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.6, 0.6, 0.65)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.WHITE
	env.ambient_light_energy = 0.45
	var world := World3D.new()
	world.environment = env
	vp.world_3d = world
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.35
	sun.rotation_degrees = Vector3(-45, -35, 0)
	vp.add_child(sun)
	var holder := Node3D.new()
	holder.name = "SpinPivot"
	vp.add_child(holder)
	var model: Node3D = load("res://player.glb").instantiate()
	model.scale = Vector3(0.9, 0.9, 0.9)
	holder.add_child(model)
	_apply_skin_textures(model, "user://skins/" + skin_name + ".png")
	var cam := Camera3D.new()
	cam.fov = 70.0
	var target := Vector3(0, 15, 0)
	var pitch := deg_to_rad(12.0)
	var yaw := deg_to_rad(-40.0)
	# Visible height at the target is ~1.4 * dist; 33 shows the whole ~32-block
	# model plus some margin on a 96-tall card (9.5 framed only the torso).
	var dist := 33.0
	vp.add_child(cam)
	# look_at_from_position works even before the node is inside the tree.
	cam.look_at_from_position(
		target + Vector3(
			dist * cos(pitch) * sin(yaw), dist * sin(pitch), dist * cos(pitch) * cos(yaw)),
		target, Vector3.UP)
	cam.make_current()
	_skin_gallery_spins.append(holder)
	return box

func _apply_skin_textures(model: Node3D, png_path: String) -> void:
	var img := Image.load_from_file(png_path)
	if img == null or img.is_empty():
		return
	
	# Load the noise value from the sidecar and apply it for the gallery preview
	var skin_name := png_path.get_file().get_basename()
	var noise_value := _load_skin_sidecar(skin_name)
	if noise_value > 0.0:
		# Apply noise to the image for the gallery preview
		var noise_map := _make_gallery_noise_map()
		var max_grain := 0.35
		var amount := noise_value / 100.0 * max_grain
		var out: Image = img.duplicate()
		for y in range(64):
			for x in range(64):
				var c: Color = out.get_pixel(x, y)
				var delta := (noise_map.get_pixel(x, y).r - 0.5) * 2.0 * amount
				out.set_pixel(x, y, Color(
					clampf(c.r + delta, 0.0, 1.0),
					clampf(c.g + delta, 0.0, 1.0),
					clampf(c.b + delta, 0.0, 1.0),
					c.a))
		img = out
	
	var tex := ImageTexture.create_from_image(img)
	for mi in model.find_children("", "MeshInstance3D", true, false):
		var mesh := (mi as MeshInstance3D).mesh
		if mesh == null:
			continue
		for si in range(mesh.get_surface_count()):
			var mat := StandardMaterial3D.new()
			mat.albedo_texture = tex
			mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
			(mi as MeshInstance3D).set_surface_override_material(si, mat)

# Create a noise map for gallery previews (same seed as SkinManager for consistency)
func _make_gallery_noise_map() -> Image:
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	var rng := RandomNumberGenerator.new()
	rng.seed = 20240829  # Same seed as SkinManager.NOISE_SEED
	for y in range(64):
		for x in range(64):
			img.set_pixel(x, y, Color(rng.randf(), 0.0, 0.0, 1.0))
	return img

# Create a 16x16 noise map for block gallery previews (same seed as
# BlockManager for consistency).
func _make_block_noise_map() -> Image:
	var img := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	var rng := RandomNumberGenerator.new()
	rng.seed = 20240829  # Same seed as BlockManager.NOISE_SEED
	for y in range(16):
		for x in range(16):
			img.set_pixel(x, y, Color(rng.randf(), 0.0, 0.0, 1.0))
	return img

func _load_named_skin(skin_name: String) -> void:
	if _skin_preview == null or not is_instance_valid(_skin_preview):
		return
	if _skin_preview.load_skin("user://skins/" + skin_name + ".png"):
		_skin_name_edit.text = skin_name
		# Restore the skin's own noise value; the slider's value_changed
		# handler re-applies it against the freshly loaded image.
		var sv := _load_skin_sidecar(skin_name)
		_skin_noise_slider.value = sv
		_skin_noise_value.text = str(int(sv))
		_close_skin_gallery()
		_schedule_save()

func _delete_named_skin(skin_name: String) -> void:
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://skins/" + skin_name + ".png"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://skins/" + skin_name + ".json"))
	_refresh_skin_gallery()

func _build_skin_gallery(s: float) -> Control:
	var dark := _skin_dark_mode
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK

	var overlay := Control.new()
	overlay.name = "SkinGallery"
	overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	overlay.visible = false
	overlay.mouse_filter = Control.MOUSE_FILTER_STOP

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	overlay.add_child(dim)

	var panel := PanelContainer.new()
	var psb := StyleBoxFlat.new()
	psb.bg_color = Color(0.1, 0.1, 0.12) if dark else Color(0.94, 0.94, 0.93)
	psb.set_border_width_all(int(2 * s))
	psb.border_color = Color(0.28, 0.28, 0.3) if dark else Color(0.55, 0.55, 0.55)
	psb.set_corner_radius_all(int(6 * s))
	psb.content_margin_left = int(14 * s)
	psb.content_margin_right = int(14 * s)
	psb.content_margin_top = int(10 * s)
	psb.content_margin_bottom = int(14 * s)
	panel.add_theme_stylebox_override("panel", psb)
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.offset_left = -330.0 * s
	panel.offset_right = 330.0 * s
	panel.offset_top = -252.0 * s
	panel.offset_bottom = 252.0 * s
	panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	overlay.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", int(8 * s))
	margin.add_theme_constant_override("margin_right", int(8 * s))
	margin.add_theme_constant_override("margin_top", int(6 * s))
	margin.add_theme_constant_override("margin_bottom", int(4 * s))
	panel.add_child(margin)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", int(8 * s))
	margin.add_child(vbox)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", int(8 * s))
	var title := Label.new()
	title.text = "LOAD SKIN"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.add_theme_font_override("font", MUNRO_FONT)
	title.add_theme_font_size_override("font_size", int(17 * s))
	title.add_theme_color_override("font_color", fg_col)
	header.add_child(title)
	var close_btn := Button.new()
	close_btn.text = "CLOSE"
	close_btn.add_theme_font_override("font", MUNRO_FONT)
	close_btn.add_theme_font_size_override("font_size", int(13 * s))
	close_btn.pressed.connect(_close_skin_gallery)
	header.add_child(close_btn)
	vbox.add_child(header)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(scroll)

	var grid := GridContainer.new()
	grid.columns = 5
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", int(12 * s))
	grid.add_theme_constant_override("v_separation", int(12 * s))
	scroll.add_child(grid)

	var timer := Timer.new()
	timer.name = "GallerySpin"
	timer.wait_time = 0.016
	timer.one_shot = false
	timer.autostart = false
	timer.timeout.connect(_spin_gallery_models)
	overlay.add_child(timer)

	_skin_gallery = overlay
	_skin_gallery_grid = grid
	_skin_gallery_timer = timer
	return overlay

func _open_block_gallery() -> void:
	if _block_gallery == null:
		return
	_refresh_block_gallery()
	_block_gallery.visible = true
	if _block_gallery_timer:
		_block_gallery_timer.start()

func _close_block_gallery() -> void:
	if _block_gallery_timer:
		_block_gallery_timer.stop()
	if _block_gallery:
		_block_gallery.visible = false

func _refresh_block_gallery() -> void:
	if _block_gallery_grid == null:
		return
	for c in _block_gallery_grid.get_children():
		c.free()
	_block_gallery_spins.clear()
	var names := _list_saved_blocks()
	if names.is_empty():
		var empty_hint := Label.new()
		empty_hint.text = "No saved blocks yet — use SAVE to create one."
		empty_hint.add_theme_font_override("font", MUNRO_FONT)
		empty_hint.add_theme_font_size_override("font_size", int(14 * _ui_scale()))
		empty_hint.add_theme_color_override("font_color",
			Color(0.75, 0.75, 0.75, 1) if _skin_dark_mode else Color(0.2, 0.2, 0.2))
		_block_gallery_grid.add_child(empty_hint)
		return
	for block_name in names:
		_block_gallery_grid.add_child(_make_block_card(_ui_scale(), block_name))

func _list_saved_blocks() -> PackedStringArray:
	var dir := DirAccess.open("user://blocks")
	if dir == null:
		return []
	var names := PackedStringArray()
	dir.list_dir_begin()
	var file_name := dir.get_next()
	while file_name != "":
		if file_name.ends_with(".png"):
			names.append(file_name.get_basename())
		file_name = dir.get_next()
	dir.list_dir_end()
	names.sort()
	return names

func _make_block_card(s: float, block_name: String) -> VBoxContainer:
	var dark := _skin_dark_mode
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK
	var border_col := Color(0.35, 0.35, 0.4) if dark else Color(0.55, 0.55, 0.58)
	var card_bg := Color(0.14, 0.14, 0.17) if dark else Color(0.99, 0.99, 0.99)
	var hover_col := Color(0.22, 0.24, 0.3) if dark else Color(0.84, 0.88, 0.94)

	var card := VBoxContainer.new()
	card.name = "Card_" + block_name
	card.add_theme_constant_override("separation", int(4 * s))
	# Same as the skin cards: pinch the card to its 96-wide minimum so the
	# preview button stays exactly square and the viewport render can never be
	# stretched into a wider-than-tall rect (which smears the block texture).
	card.size_flags_horizontal = Control.SIZE_SHRINK_CENTER

	var preview_btn := Button.new()
	preview_btn.custom_minimum_size = Vector2(96 * s, 96 * s)
	preview_btn.clip_text = true
	preview_btn.tooltip_text = "Load " + block_name
	var sb := StyleBoxFlat.new()
	sb.bg_color = card_bg
	sb.set_border_width_all(int(2 * s))
	sb.border_color = border_col
	sb.set_corner_radius_all(int(4 * s))
	var hov := sb.duplicate()
	hov.bg_color = hover_col
	var pres := sb.duplicate()
	pres.bg_color = hover_col
	pres.border_color = fg_col
	preview_btn.add_theme_stylebox_override("normal", sb)
	preview_btn.add_theme_stylebox_override("hover", hov)
	preview_btn.add_theme_stylebox_override("pressed", pres)
	preview_btn.add_theme_stylebox_override("focus", pres)
	card.add_child(preview_btn)

	var model_box := _make_block_gallery_view(s, block_name)
	model_box.set_anchors_preset(Control.PRESET_FULL_RECT)
	model_box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	preview_btn.add_child(model_box)

	var foot := HBoxContainer.new()
	foot.add_theme_constant_override("separation", int(4 * s))
	var name_btn := Button.new()
	name_btn.text = block_name
	name_btn.clip_text = true
	name_btn.flat = true
	name_btn.tooltip_text = "Load " + block_name
	name_btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_btn.add_theme_font_override("font", MUNRO_FONT)
	name_btn.add_theme_font_size_override("font_size", int(12 * s))
	name_btn.add_theme_color_override("font_color", fg_col)
	name_btn.add_theme_color_override("font_hover_color", fg_col)
	name_btn.add_theme_color_override("font_pressed_color", fg_col)
	foot.add_child(name_btn)
	var del_btn := Button.new()
	del_btn.text = "X"
	del_btn.flat = true
	del_btn.custom_minimum_size = Vector2(int(24 * s), 0)
	del_btn.tooltip_text = "Delete " + block_name
	del_btn.add_theme_font_override("font", MUNRO_FONT)
	del_btn.add_theme_font_size_override("font_size", int(13 * s))
	del_btn.add_theme_color_override("font_color", Color(1, 0.42, 0.42))
	del_btn.add_theme_color_override("font_hover_color", Color(1, 0.6, 0.6))
	foot.add_child(del_btn)
	card.add_child(foot)

	preview_btn.pressed.connect(func(): _load_named_block(block_name))
	name_btn.pressed.connect(func(): _load_named_block(block_name))
	del_btn.pressed.connect(func(): _delete_named_block(block_name))
	return card

func _make_block_gallery_view(s: float, block_name: String) -> SubViewportContainer:
	var box := SubViewportContainer.new()
	box.stretch = true
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var vp := SubViewport.new()
	vp.transparent_bg = true
	vp.msaa_3d = Viewport.MSAA_2X
	vp.size = Vector2i(int(96 * s), int(96 * s))
	box.add_child(vp)
	# Same fix as the skin gallery: the card button expands to fill its grid
	# cell (wider than tall), and stretching a fixed 96x96 viewport into it
	# renders every face's texture squished. Track the container's size so the
	# render always matches it 1:1.
	box.resized.connect(func():
		var sz := box.size
		if sz.x > 1.0 and sz.y > 1.0:
			vp.size = Vector2i(int(sz.x), int(sz.y)))
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.6, 0.6, 0.65)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color.WHITE
	env.ambient_light_energy = 0.45
	var world := World3D.new()
	world.environment = env
	vp.world_3d = world
	var sun := DirectionalLight3D.new()
	sun.light_energy = 1.35
	sun.rotation_degrees = Vector3(-45, -35, 0)
	vp.add_child(sun)
	var holder := Node3D.new()
	holder.name = "SpinPivot"
	vp.add_child(holder)
	var cube := MeshInstance3D.new()
	cube.mesh = _build_cube_mesh()
	# Unit mesh scaled up so the card framing matches the main preview cube.
	cube.scale = Vector3(12.0, 12.0, 12.0)
	holder.add_child(cube)
	var img := Image.load_from_file("user://blocks/" + block_name + ".png")
	if img != null and not img.is_empty():
		# Re-bake the saved noise into the preview like the skin gallery does.
		var nv := _load_block_sidecar(block_name)
		if nv > 0.0:
			var noise_map := _make_block_noise_map()
			var max_grain := 0.35
			var amount := nv / 100.0 * max_grain
			var out: Image = img.duplicate()
			for y in range(16):
				for x in range(16):
					var c: Color = out.get_pixel(x, y)
					var delta := (noise_map.get_pixel(x, y).r - 0.5) * 2.0 * amount
					out.set_pixel(x, y, Color(
						clampf(c.r + delta, 0.0, 1.0),
						clampf(c.g + delta, 0.0, 1.0),
						clampf(c.b + delta, 0.0, 1.0),
						c.a))
			img = out
		var tex := ImageTexture.create_from_image(img)
		if tex != null:
			var mat := StandardMaterial3D.new()
			mat.albedo_texture = tex
			mat.texture_filter = BaseMaterial3D.TEXTURE_FILTER_NEAREST
			mat.albedo_color = Color.WHITE  # Ensure no black overlay
			mat.roughness = 1.0  # Ensure proper lighting
			cube.set_surface_override_material(0, mat)
	var cam := Camera3D.new()
	cam.fov = 70.0
	var target := Vector3.ZERO
	var pitch := deg_to_rad(12.0)
	var yaw := deg_to_rad(-40.0)
	var dist := 33.0
	vp.add_child(cam)
	cam.look_at_from_position(
		target + Vector3(
			dist * cos(pitch) * sin(yaw), dist * sin(pitch), dist * cos(pitch) * cos(yaw)),
		target, Vector3.UP)
	cam.make_current()
	_block_gallery_spins.append(holder)
	return box

func _build_cube_mesh() -> ArrayMesh:
	# Byte-for-byte copy of block_preview's cube builder: every face is built
	# with the SAME upright UV mapping (texture top = world top), so the load
	# gallery cube renders its texture exactly like the editor preview cube.
	# The old generic builder here mapped 4 of the 6 faces with the texture
	# rotated 90 degrees, which made the pattern slant along the diagonal.
	var arrays = []
	arrays.resize(Mesh.ARRAY_MAX)

	var verts = PackedVector3Array()
	var uvs = PackedVector2Array()
	var normals = PackedVector3Array()
	var indices = PackedInt32Array()

	# +X face (right)
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	normals.append(Vector3(1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	var base := 0
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -X face (left)
	verts.append(Vector3(-0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, -0.5, 0.5))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	normals.append(Vector3(-1, 0, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 4
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# +Y face (top)
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	normals.append(Vector3(0, 1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 8
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -Y face (bottom)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	normals.append(Vector3(0, -1, 0))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 12
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# +Z face (front)
	verts.append(Vector3(-0.5, -0.5, 0.5))
	verts.append(Vector3(-0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, 0.5, 0.5))
	verts.append(Vector3(0.5, -0.5, 0.5))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	normals.append(Vector3(0, 0, 1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 16
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	# -Z face (back)
	verts.append(Vector3(0.5, -0.5, -0.5))
	verts.append(Vector3(0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, 0.5, -0.5))
	verts.append(Vector3(-0.5, -0.5, -0.5))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	normals.append(Vector3(0, 0, -1))
	uvs.append(Vector2(0.0, 1.0))
	uvs.append(Vector2(0.0, 0.0))
	uvs.append(Vector2(1.0, 0.0))
	uvs.append(Vector2(1.0, 1.0))
	base = 20
	indices.append(base + 0)
	indices.append(base + 1)
	indices.append(base + 2)
	indices.append(base + 0)
	indices.append(base + 2)
	indices.append(base + 3)

	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh = ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh

func _load_named_block(block_name: String) -> void:
	if _block_preview != null:
		_block_preview.load_block("user://blocks/" + block_name + ".png")
		_block_name_edit.text = block_name
		# Restore the block's own noise value; the slider's value_changed
		# handler re-applies it against the freshly loaded image.
		var nv := _load_block_sidecar(block_name)
		_block_noise = nv
		if _block_noise_slider != null:
			_block_noise_slider.value = nv
		if _block_noise_value != null:
			_block_noise_value.text = str(int(nv))
		if _block_preview.has_method("set_noise"):
			_block_preview.set_noise(nv)
	_close_block_gallery()
	_schedule_save()

func _delete_named_block(block_name: String) -> void:
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://blocks/" + block_name + ".png"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path("user://blocks/" + block_name + ".json"))
	_refresh_block_gallery()

func _build_block_gallery(s: float) -> Control:
	var dark := _skin_dark_mode
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK

	var overlay := Control.new()
	overlay.name = "BlockGallery"
	overlay.set_anchors_preset(Control.PRESET_FULL_RECT)
	overlay.visible = false
	overlay.mouse_filter = Control.MOUSE_FILTER_STOP

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	overlay.add_child(dim)

	var panel := PanelContainer.new()
	var psb := StyleBoxFlat.new()
	psb.bg_color = Color(0.1, 0.1, 0.12) if dark else Color(0.94, 0.94, 0.93)
	psb.set_border_width_all(int(2 * s))
	psb.border_color = Color(0.28, 0.28, 0.3) if dark else Color(0.55, 0.55, 0.55)
	psb.set_corner_radius_all(int(6 * s))
	psb.content_margin_left = int(14 * s)
	psb.content_margin_right = int(14 * s)
	psb.content_margin_top = int(10 * s)
	psb.content_margin_bottom = int(14 * s)
	panel.add_theme_stylebox_override("panel", psb)
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.offset_left = -330.0 * s
	panel.offset_right = 330.0 * s
	panel.offset_top = -252.0 * s
	panel.offset_bottom = 252.0 * s
	panel.grow_horizontal = Control.GROW_DIRECTION_BOTH
	panel.grow_vertical = Control.GROW_DIRECTION_BOTH
	overlay.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", int(8 * s))
	margin.add_theme_constant_override("margin_right", int(8 * s))
	margin.add_theme_constant_override("margin_top", int(6 * s))
	margin.add_theme_constant_override("margin_bottom", int(4 * s))
	panel.add_child(margin)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", int(8 * s))
	margin.add_child(vbox)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", int(8 * s))
	var title := Label.new()
	title.text = "LOAD BLOCK"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.add_theme_font_override("font", MUNRO_FONT)
	title.add_theme_font_size_override("font_size", int(17 * s))
	title.add_theme_color_override("font_color", fg_col)
	header.add_child(title)
	var close_btn := Button.new()
	close_btn.text = "CLOSE"
	close_btn.add_theme_font_override("font", MUNRO_FONT)
	close_btn.add_theme_font_size_override("font_size", int(13 * s))
	close_btn.pressed.connect(_close_block_gallery)
	header.add_child(close_btn)
	vbox.add_child(header)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(scroll)

	var grid := GridContainer.new()
	grid.columns = 5
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", int(12 * s))
	grid.add_theme_constant_override("v_separation", int(12 * s))
	scroll.add_child(grid)

	var timer := Timer.new()
	timer.name = "BlockGallerySpin"
	timer.wait_time = 0.016
	timer.one_shot = false
	timer.autostart = false
	timer.timeout.connect(_spin_gallery_models)
	overlay.add_child(timer)

	_block_gallery = overlay
	_block_gallery_grid = grid
	_block_gallery_timer = timer
	return overlay

func _apply_skin_palette() -> void:
	var s := _ui_scale()
	var dark := _skin_dark_mode
	var bg_col := Color(0.08, 0.08, 0.1) if dark else Color.WHITE
	var fg_col := Color(1, 1, 1, 1) if dark else Color.BLACK
	var hover_col := Color(0.72, 0.72, 0.72, 1) if dark else Color(0.35, 0.35, 0.35, 1)
	var hint_col := Color(0.75, 0.75, 0.75, 1) if dark else Color(0.2, 0.2, 0.2)
	if _skin_bg:
		_skin_bg.color = bg_col
	if _skin_title:
		_skin_title.add_theme_color_override("font_color", fg_col)
	if _skin_hex:
		_skin_hex.add_theme_color_override("font_color", fg_col)
	if _skin_hint:
		_skin_hint.add_theme_color_override("font_color", hint_col)
	if _skin_noise_label:
		_skin_noise_label.add_theme_color_override("font_color", hint_col)
	if _skin_noise_value:
		_skin_noise_value.add_theme_color_override("font_color", hint_col)
	if _skin_picker:
		_skin_picker.theme = _make_picker_theme(s, dark)
		_tint_picker_internals(_skin_picker, fg_col, hover_col)
	if _skin_toggle:
		_skin_toggle.text = "LIGHT MODE" if dark else "DARK MODE"
		_style_skin_button(_skin_toggle, fg_col, dark)
	if _skin_uv_toggle:
		_style_skin_button(_skin_uv_toggle, fg_col, dark)
	if _skin_undo_btn:
		_style_skin_button(_skin_undo_btn, fg_col, dark)
	if _skin_tool_btn:
		_style_skin_button(_skin_tool_btn, fg_col, dark)
	if _skin_save_btn:
		_style_skin_button(_skin_save_btn, fg_col, dark)
	if _skin_load_btn:
		_style_skin_button(_skin_load_btn, fg_col, dark)
	if _skin_name_edit:
		_skin_name_edit.add_theme_color_override("font_color", fg_col)
		_skin_name_edit.add_theme_color_override("caret_color", fg_col)
		_skin_name_edit.add_theme_color_override("placeholder_font_color", hint_col)
		var le_bg := StyleBoxFlat.new()
		le_bg.bg_color = Color(0.12, 0.12, 0.16, 1) if dark else Color(0.92, 0.92, 0.92, 1)
		le_bg.border_color = fg_col
		le_bg.set_border_width_all(1)
		le_bg.set_corner_radius_all(2)
		_skin_name_edit.add_theme_stylebox_override("normal", le_bg)
		_skin_name_edit.add_theme_stylebox_override("focus", le_bg.duplicate())

func _style_skin_button(btn: Button, fg_col: Color, dark: bool) -> void:
	btn.add_theme_color_override("font_color", fg_col)
	var disabled_fg := fg_col.lerp(Color(0.5, 0.5, 0.5), 0.6)
	btn.add_theme_color_override("font_disabled_color", disabled_fg)
	var sw_bg := Color(0.15, 0.15, 0.18) if dark else Color(1, 1, 1, 1)
	var sb := StyleBoxFlat.new()
	sb.bg_color = sw_bg
	sb.border_color = fg_col
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(2)
	var sb_hover := sb.duplicate() as StyleBoxFlat
	sb_hover.bg_color = fg_col.lerp(sw_bg, 0.5)
	var sb_disabled := sb.duplicate() as StyleBoxFlat
	sb_disabled.bg_color = fg_col.lerp(sw_bg, 0.9)
	btn.add_theme_stylebox_override("normal", sb)
	btn.add_theme_stylebox_override("hover", sb_hover)
	btn.add_theme_stylebox_override("pressed", sb_hover)
	btn.add_theme_stylebox_override("disabled", sb_disabled)
	btn.add_theme_stylebox_override("focus", StyleBoxEmpty.new())

func _make_picker_theme(s: float, dark: bool) -> Theme:
	var th := Theme.new()
	th.default_font = MUNRO_FONT
	th.default_font_size = int(12 * s)
	var fg := Color(1, 1, 1, 1) if dark else Color.BLACK
	var hover := Color(0.72, 0.72, 0.72, 1) if dark else Color(0.35, 0.35, 0.35, 1)
	# The ColorPicker's internals are C++-built (inaccessible child controls),
	# so colour lookups happen under its own "ColorPicker" theme type — set the
	# items there and on the fallback types too.
	for type_name in ["ColorPicker", "Label", "Button", "LineEdit"]:
		th.set_color("font_color", type_name, fg)
		th.set_color("icon_color", type_name, fg)
		for state in ["font_hover_color", "font_pressed_color", "font_focus_color",
				"icon_normal_color", "icon_hover_color", "icon_pressed_color", "icon_focus_color"]:
			var col := fg
			if state.contains("hover") or state.contains("pressed") or state.contains("focus"):
				col = hover
			th.set_color(state, type_name, col)
	var swatch := StyleBoxFlat.new()
	swatch.bg_color = Color(0.15, 0.15, 0.18) if dark else Color(1, 1, 1, 1)
	swatch.border_color = fg
	swatch.set_border_width_all(1)
	swatch.set_corner_radius_all(2)
	for state in ["normal", "hover", "pressed"]:
		th.set_stylebox(state, "Button", swatch)
	th.set_stylebox("focus", "Button", StyleBoxEmpty.new())
	return th

func _tint_picker_internals(node: Node, fg: Color, hover: Color) -> void:
	for child in node.get_children(true):
		if child is Button:
			var btn := child as Button
			btn.add_theme_color_override("font_color", fg)
			btn.add_theme_color_override("icon_color", fg)
			for state in ["font_hover_color", "font_pressed_color", "font_focus_color",
					"icon_normal_color", "icon_hover_color", "icon_pressed_color", "icon_focus_color"]:
				var col := fg
				if state.contains("hover") or state.contains("pressed") or state.contains("focus"):
					col = hover
				btn.add_theme_color_override(state, col)
		_tint_picker_internals(child, fg, hover)

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

# Crosshair export/import (CS-style codes)
func _export_crosshair_code() -> String:
	if not crosshair_node:
		return ""
	
	# Pack data into bytes for compact encoding
	var data := PackedByteArray()
	
	# Version byte (1)
	data.append(1)
	
	# Pack booleans into first byte (7 bits)
	var bool_byte := 0
	bool_byte |= (1 if crosshair_node.cross_enabled else 0) << 0
	bool_byte |= (1 if crosshair_node.top_line_enabled else 0) << 1
	bool_byte |= (1 if crosshair_node.cross_contrast else 0) << 2
	bool_byte |= (1 if crosshair_node.dot_enabled else 0) << 3
	bool_byte |= (1 if crosshair_node.dot_contrast else 0) << 4
	bool_byte |= (1 if crosshair_node.cross_dot_collision else 0) << 5
	data.append(bool_byte)
	
	# Floats packed as uint16 (scaled)
	data.append_array(_pack_float16(crosshair_node.cross_length, 0.0, 40.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.cross_thickness, 0.0, 10.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.cross_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.cross_spacing, 0.0, 10.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.cross_rotation, 0.0, 360.0, 10.0))
	data.append_array(_pack_float16(crosshair_node.dot_size, 0.0, 40.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.dot_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(crosshair_node.dot_rotation, 0.0, 45.0, 10.0))
	
	# Colors as 32-bit RGBA
	data.append_array(_pack_color32(crosshair_node.cross_color))
	data.append_array(_pack_color32(crosshair_node.dot_color))
	
	# Base32 encode (more compact than hex, no overflow issues)
	var b32 := _base32_encode(data)
	return _format_cs_code("FC", b32)

func _import_crosshair_code(code: String) -> bool:
	if not crosshair_node:
		return false
	
	# Parse CS-style format
	var parts := code.split("-")
	if parts.size() < 2 or not parts[0].begins_with("FC"):
		return false
	
	# Convert base32 back to bytes (skip the prefix)
	var b32 := "".join(parts.slice(1))
	var data := _base32_decode(b32)
	
	if data.size() < 26:  # version(1) + bool(1) + 8*float16(16) + 2*color32(8) = 26 bytes
		return false
	
	var version := data[0]
	if version != 1:
		return false
	
	var idx := 1
	
	# Unpack booleans
	var bool_byte := data[idx]; idx += 1
	crosshair_node.cross_enabled = (bool_byte & (1 << 0)) != 0
	crosshair_node.top_line_enabled = (bool_byte & (1 << 1)) != 0
	crosshair_node.cross_contrast = (bool_byte & (1 << 2)) != 0
	crosshair_node.dot_enabled = (bool_byte & (1 << 3)) != 0
	crosshair_node.dot_contrast = (bool_byte & (1 << 4)) != 0
	crosshair_node.cross_dot_collision = (bool_byte & (1 << 5)) != 0
	
	# Unpack floats
	crosshair_node.cross_length = _unpack_float16(data, idx, 0.0, 40.0, 100.0); idx += 2
	crosshair_node.cross_thickness = _unpack_float16(data, idx, 0.0, 10.0, 100.0); idx += 2
	crosshair_node.cross_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	crosshair_node.cross_spacing = _unpack_float16(data, idx, 0.0, 10.0, 100.0); idx += 2
	crosshair_node.cross_rotation = _unpack_float16(data, idx, 0.0, 360.0, 10.0); idx += 2
	crosshair_node.dot_size = _unpack_float16(data, idx, 0.0, 40.0, 100.0); idx += 2
	crosshair_node.dot_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	crosshair_node.dot_rotation = _unpack_float16(data, idx, 0.0, 45.0, 10.0); idx += 2
	
	# Unpack colors
	crosshair_node.cross_color = _unpack_color32(data, idx); idx += 4
	crosshair_node.dot_color = _unpack_color32(data, idx); idx += 4
	
	_schedule_save()
	return true

# Block outline export/import (CS-style codes)
func _export_block_outline_code() -> String:
	if not block_outline_node:
		return ""
	
	# Pack data into bytes for compact encoding
	var data := PackedByteArray()
	
	# Version byte (1)
	data.append(1)
	
	# Pack booleans into first byte (6 bits)
	var bool_byte := 0
	bool_byte |= (1 if block_outline_node.outline_enabled else 0) << 0
	bool_byte |= (1 if block_outline_node.outline_pulse_enabled else 0) << 1
	bool_byte |= (1 if block_outline_node.fill_enabled else 0) << 2
	bool_byte |= (1 if block_outline_node.fill_pulse_enabled else 0) << 3
	data.append(bool_byte)
	
	# Floats packed as uint16 (scaled)
	data.append_array(_pack_float16(block_outline_node.outline_thickness, 0.0, 0.99, 100.0))
	data.append_array(_pack_float16(block_outline_node.outline_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.outline_pulse_speed, 0.5, 10.0, 10.0))
	data.append_array(_pack_float16(block_outline_node.outline_pulse_min_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.outline_pulse_max_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.fill_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.fill_pulse_speed, 0.5, 10.0, 10.0))
	data.append_array(_pack_float16(block_outline_node.fill_pulse_min_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.fill_pulse_max_opacity, 0.0, 1.0, 100.0))
	data.append_array(_pack_float16(block_outline_node.reach_distance, 0.0, 10.0, 10.0))
	
	# Colors as 32-bit RGBA
	data.append_array(_pack_color32(block_outline_node.outline_color))
	data.append_array(_pack_color32(block_outline_node.fill_color))
	
	# Base32 encode (more compact than hex, no overflow issues)
	var b32 := _base32_encode(data)
	return _format_cs_code("FO", b32)

func _import_block_outline_code(code: String) -> bool:
	if not block_outline_node:
		return false
	
	# Parse CS-style format
	var parts := code.split("-")
	if parts.size() < 2 or not parts[0].begins_with("FO"):
		return false
	
	# Convert base32 back to bytes (skip the prefix)
	var b32 := "".join(parts.slice(1))
	var data := _base32_decode(b32)
	
	if data.size() < 30:  # version(1) + bool(1) + 10*float16(20) + 2*color32(8) = 30 bytes
		return false
	
	var version := data[0]
	if version != 1:
		return false
	
	var idx := 1
	
	# Unpack booleans
	var bool_byte := data[idx]; idx += 1
	block_outline_node.outline_enabled = (bool_byte & (1 << 0)) != 0
	block_outline_node.outline_pulse_enabled = (bool_byte & (1 << 1)) != 0
	block_outline_node.fill_enabled = (bool_byte & (1 << 2)) != 0
	block_outline_node.fill_pulse_enabled = (bool_byte & (1 << 3)) != 0
	
	# Unpack floats
	block_outline_node.outline_thickness = _unpack_float16(data, idx, 0.0, 0.99, 100.0); idx += 2
	block_outline_node.outline_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.outline_pulse_speed = _unpack_float16(data, idx, 0.5, 10.0, 10.0); idx += 2
	block_outline_node.outline_pulse_min_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.outline_pulse_max_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.fill_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.fill_pulse_speed = _unpack_float16(data, idx, 0.5, 10.0, 10.0); idx += 2
	block_outline_node.fill_pulse_min_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.fill_pulse_max_opacity = _unpack_float16(data, idx, 0.0, 1.0, 100.0); idx += 2
	block_outline_node.reach_distance = _unpack_float16(data, idx, 0.0, 10.0, 10.0); idx += 2
	
	# Unpack colors
	block_outline_node.outline_color = _unpack_color32(data, idx); idx += 4
	block_outline_node.fill_color = _unpack_color32(data, idx); idx += 4
	
	_schedule_save()
	return true

# Helper functions for compact encoding
func _pack_float16(value: float, min_val: float, max_val: float, scale_factor: float) -> PackedByteArray:
	var scaled := clampf(value, min_val, max_val) * scale_factor
	var uint16 := int(round(scaled))
	return PackedByteArray([uint16 & 0xFF, (uint16 >> 8) & 0xFF])

func _unpack_float16(data: PackedByteArray, idx: int, min_val: float, max_val: float, scale_factor: float) -> float:
	var uint16 := data[idx] | (data[idx + 1] << 8)
	return clampf(float(uint16) / scale_factor, min_val, max_val)

func _pack_color32(c: Color) -> PackedByteArray:
	return PackedByteArray([
		int(c.r * 255),
		int(c.g * 255),
		int(c.b * 255),
		int(c.a * 255)
	])

func _unpack_color32(data: PackedByteArray, idx: int) -> Color:
	return Color(
		data[idx] / 255.0,
		data[idx + 1] / 255.0,
		data[idx + 2] / 255.0,
		data[idx + 3] / 255.0
	)

func _format_cs_code(prefix: String, encoded: String) -> String:
	# Format like CS: PREFIX-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
	var result := prefix + "-"
	var chunk_size := 5
	for i in range(0, encoded.length(), chunk_size):
		if i > 0:
			result += "-"
		var end := mini(i + chunk_size, encoded.length())
		result += encoded.substr(i, end - i)
	return result

# Base32 encoding (A-Z, 2-7) - more compact than hex, no overflow issues
const BASE32_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

func _base32_encode(data: PackedByteArray) -> String:
	if data.is_empty():
		return ""
	
	var result := ""
	var buffer := 0
	var bits_left := 0
	
	for byte in data:
		buffer = (buffer << 8) | byte
		bits_left += 8
		
		while bits_left >= 5:
			bits_left -= 5
			var index = (buffer >> bits_left) & 0x1F
			result += BASE32_CHARS[index]
	
	# Handle remaining bits
	if bits_left > 0:
		var index = (buffer << (5 - bits_left)) & 0x1F
		result += BASE32_CHARS[index]
	
	return result

func _base32_decode(encoded: String) -> PackedByteArray:
	if encoded.is_empty():
		return PackedByteArray()
	
	var data := PackedByteArray()
	var buffer := 0
	var bits_left := 0
	
	# Case-insensitive: convert to uppercase for lookup
	encoded = encoded.to_upper()
	
	for ch in encoded:
		var index := BASE32_CHARS.find(ch)
		if index == -1:
			return PackedByteArray()  # Invalid character
		
		buffer = (buffer << 5) | index
		bits_left += 5
		
		while bits_left >= 8:
			bits_left -= 8
			data.append((buffer >> bits_left) & 0xFF)
	
	return data

func _input(event):
	if is_open and _capturing_action != "":
		# A binding capture is waiting: swallow the next key/button press (or
		# escape to cancel) so it neither closes the menu nor leaks to gameplay.
		if _capture_binding(event):
			get_viewport().set_input_as_handled()
			return
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
			"controls":
				_show_page("settings")
			"crosshair":
				_show_page("gui")
			"block_outline":
				_show_page("gui")
			"skin_maker":
				_close()
			_:
				_show_page("settings")
		get_viewport().set_input_as_handled()
	elif not player_controller.is_chat_open() and not player_controller.is_inventory_open() \
			and not player_controller.is_table_menu_open():
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
