extends Control

# A minimal compass readout: the cardinal direction the player's camera
# currently faces, drawn top-centre of the screen. Follows the world's north
# convention (-Z is north). Hidden while any menu is open, like the crosshair.

@onready var player_controller = get_node("/root/Main/Player")

const DIRECTIONS := ["N", "NE", "E", "SE", "S", "SW", "W", "NW"]

var _text := ""
var _visible_state := false

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
	if text != _text or blocked != _visible_state:
		_text = text
		_visible_state = not blocked
		queue_redraw()

func _draw():
	if _text.is_empty():
		return
	var font := ThemeDB.fallback_font
	var font_size := 20
	var pos := Vector2(size.x / 2.0, 14.0)
	# Slight outline so it reads against both sky and ground.
	draw_string_outline(font, pos, _text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size, 3, Color(0, 0, 0, 0.7))
	draw_string(font, pos, _text, HORIZONTAL_ALIGNMENT_CENTER, -1, font_size, Color(1, 1, 1, 0.85))
