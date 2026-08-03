extends Control

@onready var player_controller = get_node("/root/Main/Player")

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")

const MAX_MESSAGES = 8
const LOG_FONT_SIZE = 20
const LINE_HEIGHT = 24.0
const INPUT_FONT_SIZE = 22
const INPUT_HEIGHT = 40.0
const H_MARGIN = 8.0
const COLOR_SYSTEM = Color(1.0, 0.85, 0.4)
const COLOR_PLAYER = Color.WHITE
const COLOR_ERROR = Color(1.0, 0.45, 0.45)
const COLOR_SUCCESS = Color(0.6, 0.95, 0.6)

var is_open = false
var messages: Array = []  # Array of {text: String, color: Color}
var _history: Array[String] = []
var _history_index = -1
var _input_edit: LineEdit = null

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_input_edit = LineEdit.new()
	_input_edit.max_length = 256
	_input_edit.placeholder_text = "Type a message or command..."
	_input_edit.add_theme_font_override("font", MUNRO_FONT)
	_input_edit.add_theme_font_size_override("font_size", INPUT_FONT_SIZE)
	_input_edit.add_theme_color_override("font_color", Color.WHITE)
	_input_edit.add_theme_color_override("font_placeholder_color", Color(1, 1, 1, 0.45))
	_input_edit.add_theme_color_override("caret_color", Color.WHITE)
	var box := StyleBoxFlat.new()
	box.bg_color = Color(0, 0, 0, 0.55)
	box.corner_radius_top_left = 4
	box.corner_radius_top_right = 4
	box.corner_radius_bottom_left = 4
	box.corner_radius_bottom_right = 4
	box.content_margin_left = 12.0
	box.content_margin_right = 12.0
	box.content_margin_top = 6.0
	box.content_margin_bottom = 6.0
	_input_edit.add_theme_stylebox_override("normal", box)
	_input_edit.add_theme_stylebox_override("focus", box)
	_input_edit.text_submitted.connect(_on_text_submitted)
	add_child(_input_edit)
	_input_edit.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	_input_edit.offset_left = H_MARGIN
	_input_edit.offset_right = -H_MARGIN
	_input_edit.offset_bottom = -H_MARGIN
	_input_edit.offset_top = -INPUT_HEIGHT - H_MARGIN
	_input_edit.visible = false
	_add_message("Welcome! Type /help for a list of commands.", COLOR_SYSTEM)

func _input(event):
	if not is_open:
		if player_controller.is_inventory_open():
			return
		if event.is_action_pressed("toggle_chat"):
			_open_chat()
			get_viewport().set_input_as_handled()
		elif event is InputEventKey and event.pressed and not event.is_echo() \
				and event.keycode == KEY_SLASH:
			_open_chat("/")
			get_viewport().set_input_as_handled()
		return
	# Chat is open: only Esc closes it. T and / keep typing their characters.
	if event.is_action_pressed("ui_cancel"):
		_close_chat()
		return
	if event is InputEventKey and event.pressed and not event.is_echo():
		match event.keycode:
			KEY_UP:
				_history_back()
				get_viewport().set_input_as_handled()
			KEY_DOWN:
				_history_forward()
				get_viewport().set_input_as_handled()

func _open_chat(initial_text: String = ""):
	is_open = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	_input_edit.visible = true
	_input_edit.text = initial_text
	_input_edit.caret_column = initial_text.length()
	player_controller.set_chat_open(true)
	_input_edit.grab_focus()
	queue_redraw()

func _close_chat():
	is_open = false
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_input_edit.visible = false
	_input_edit.release_focus()
	player_controller.set_chat_open(false)
	_history_index = -1
	queue_redraw()

func _on_text_submitted(text: String):
	var trimmed := text.strip_edges()
	_input_edit.clear()
	if trimmed.is_empty():
		_close_chat()
		return
	_history.append(trimmed)
	if _history.size() > 64:
		_history.remove_at(0)
	_history_index = -1
	if trimmed.begins_with("/"):
		_run_command(trimmed)
	else:
		_add_message(trimmed, COLOR_PLAYER)
	_close_chat()

func _history_back():
	if _history.is_empty():
		return
	_history_index = mini(_history_index + 1, _history.size() - 1)
	_input_edit.text = _history[_history.size() - 1 - _history_index]
	_input_edit.caret_column = _input_edit.text.length()

func _history_forward():
	if _history_index < 0:
		return
	_history_index -= 1
	if _history_index < 0:
		_input_edit.text = ""
		return
	_input_edit.text = _history[_history.size() - 1 - _history_index]
	_input_edit.caret_column = _input_edit.text.length()

func _run_command(raw: String):
	var parts := raw.split(" ", false)
	var cmd := parts[0].to_lower()
	match cmd:
		"/help":
			_add_message("/give <block> [count] - add blocks to your inventory", COLOR_SYSTEM)
			_add_message("/tp <x> <y> <z> - teleport to a position", COLOR_SYSTEM)
			_add_message("/fly - toggle flying", COLOR_SYSTEM)
			_add_message("/clear - clear the chat", COLOR_SYSTEM)
			_add_message("/version - show the engine version", COLOR_SYSTEM)
		"/give":
			if parts.size() < 2:
				_add_message("Usage: /give <block> [count]", COLOR_ERROR)
				return
			var block_id := BlockTextures.get_block_id_by_name(parts[1])
			if block_id <= 0:
				_add_message("Unknown block: %s" % parts[1], COLOR_ERROR)
				return
			var count := 1
			if parts.size() >= 3:
				count = int(parts[2].to_float())
				if count <= 0 or count > 64:
					_add_message("Count must be between 1 and 64.", COLOR_ERROR)
					return
			if player_controller.give_block(block_id, count):
				_add_message("Gave you %d x %s" % [count, parts[1]], COLOR_SUCCESS)
			else:
				_add_message("Inventory is full.", COLOR_ERROR)
		"/tp":
			if parts.size() < 4:
				_add_message("Usage: /tp <x> <y> <z>", COLOR_ERROR)
				return
			var x := parts[1].to_float()
			var y := parts[2].to_float()
			var z := parts[3].to_float()
			if not (is_finite(x) and is_finite(y) and is_finite(z)):
				_add_message("Invalid coordinates.", COLOR_ERROR)
				return
			player_controller.teleport_to(Vector3(x, y, z))
			_add_message("Teleported to (%d, %d, %d)" % [int(x), int(y), int(z)], COLOR_SUCCESS)
		"/fly":
			player_controller.set_fly_mode(not player_controller.get_fly_mode())
			_add_message("Flight %s" % ("enabled" if player_controller.get_fly_mode() else "disabled"), COLOR_SUCCESS)
		"/clear":
			messages.clear()
		"/version":
			_add_message("Farlands - Godot 4 + C++ GDExtension", COLOR_SYSTEM)
		_:
			_add_message("Unknown command: %s (type /help)" % parts[0], COLOR_ERROR)

func _add_message(text: String, color: Color):
	messages.append({"text": text, "color": color})
	if messages.size() > MAX_MESSAGES:
		messages.remove_at(0)
	queue_redraw()

func _draw():
	if messages.is_empty():
		return
	# Newest message at the bottom (closest to the input box), older ones
	# stacked above it, Minecraft-style.
	var baseline_y := size.y - INPUT_HEIGHT - H_MARGIN - 10.0
	for i in range(messages.size() - 1, -1, -1):
		var m = messages[i]
		var shadow := Color(0.09, 0.09, 0.09)
		draw_string(MUNRO_FONT, Vector2(H_MARGIN + 1, baseline_y + 1), m.text,
					HORIZONTAL_ALIGNMENT_LEFT, -1, LOG_FONT_SIZE, shadow)
		draw_string(MUNRO_FONT, Vector2(H_MARGIN, baseline_y), m.text,
					HORIZONTAL_ALIGNMENT_LEFT, -1, LOG_FONT_SIZE, m.color)
		baseline_y -= LINE_HEIGHT
