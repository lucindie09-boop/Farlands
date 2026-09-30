extends Control

# A minimal compass readout: the cardinal direction the player's camera
# currently faces, drawn top-centre of the screen. Follows the world's north
# convention (-Z is north). Hidden while any menu is open, like the crosshair.

@onready var player_controller = get_node("/root/Main/Player")

const DIRECTIONS := ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]

# The readout in GUI units: 10 units of text (the settings menu's label size,
# 20 px at scale 2), 1.5 units of outline, 7 units down from the screen's top
# edge. Each is multiplied by the scale, so the compass grows with the HUD
# instead of holding a fixed pixel size while everything around it scales.
const FONT_UNITS := 10.0
const OUTLINE_UNITS := 1.5
const TOP_UNITS := 7.0

var _text := ""
var _visible_state := false
var _last_ui_scale := -1.0

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE

func _process(_delta):
	var blocked: bool = player_controller.is_chat_open() \
		or player_controller.is_inventory_open() \
		or player_controller.is_settings_open() \
		or player_controller.is_table_menu_open()
	var text := ""
	if not blocked:
		var cam := get_viewport().get_camera_3d()
		if cam != null:
			var forward := -cam.global_transform.basis.z
			# 0 = north (-Z), 90 = east (+X)
			var yaw := rad_to_deg(atan2(forward.x, -forward.z))
			var idx := wrapi(roundi(yaw / 45.0), 0, 8)
			text = DIRECTIONS[idx]
	if text != _text or blocked != _visible_state or not is_equal_approx(UIScale.value, _last_ui_scale):
		_text = text
		_visible_state = not blocked
		_last_ui_scale = UIScale.value
		queue_redraw()

func _draw():
	if _text.is_empty():
		return
	var u := UIScale.value
	var font := ThemeDB.fallback_font
	var font_size := int(round(FONT_UNITS * u))
	var pos := Vector2(size.x / 2.0, TOP_UNITS * u)
	# Slight outline so it reads against both sky and ground.
	draw_string_outline(font, pos, _text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size,
			maxi(1, int(round(OUTLINE_UNITS * u))), Color(0, 0, 0, 0.7))
	draw_string(font, pos, _text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size, Color(1, 1, 1, 0.85))
