extends Control

@onready var player_controller = get_node("/root/Main/Player")

const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")

const MAX_MESSAGES = 100
const LOG_FONT_SIZE = 20
const INPUT_FONT_SIZE = 22
const INPUT_HEIGHT = 40.0
const INPUT_WIDTH_RATIO = 1.0
const H_MARGIN = 8.0
const COLOR_SYSTEM = Color(1.0, 0.85, 0.4)
const COLOR_PLAYER = Color.WHITE
const COLOR_ERROR = Color(1.0, 0.45, 0.45)
const COLOR_SUCCESS = Color(0.6, 0.95, 0.6)

var is_open = false
var messages: Array = []  # Array of {text: String, color: Color}
var _scroll_offset: float = 0.0  # Pixels scrolled back into chat history (0 = newest)
var _history: Array[String] = []
var _history_index = -1
var _input_edit: LineEdit = null
var _ghost_label: Label = null
var _completion_matches: Array[String] = []
var _completion_index = -1
var _completing = false
var _ghost_text: String = ""
var _pulse_time: float = 0.0
var _completion_word_start: int = -1  # Track where the completion word started
var _completion_original_word: String = ""  # Remember the original word before completion
var _tab_held: bool = false
var _tab_hold_time: float = 0.0
var _tab_cycle_delay: float = 0.1875
var _up_held: bool = false
var _up_hold_time: float = 0.0

const COMMANDS := ["/help", "/give", "/tp", "/fly", "/clearchat", "/clearinv", "/version"]

func _chat_scale() -> float:
	return 1.0  # Chat is not affected by the global GUI scale

func _apply_input_layout():
	var sc := _chat_scale()
	_input_edit.offset_left = (H_MARGIN * sc)
	_input_edit.offset_right = -(H_MARGIN * sc)
	_input_edit.offset_bottom = -(H_MARGIN * sc)
	_input_edit.offset_top = -(INPUT_HEIGHT * sc) - (H_MARGIN * sc)
	_input_edit.add_theme_font_size_override("font_size", int(INPUT_FONT_SIZE * sc))
	_ghost_label.add_theme_font_size_override("font_size", int(INPUT_FONT_SIZE * sc))

func _ready():
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_input_edit = LineEdit.new()
	_input_edit.max_length = 256
	_input_edit.placeholder_text = "Type a message or command..."
	_input_edit.add_theme_font_override("font", MUNRO_FONT)
	_input_edit.add_theme_color_override("font_color", Color.WHITE)
	_input_edit.add_theme_color_override("font_placeholder_color", Color(1, 1, 1, 0.45))
	_input_edit.add_theme_color_override("caret_color", Color.WHITE)
	_input_edit.caret_blink = true
	_input_edit.caret_blink_interval = 0.5
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
	_input_edit.text_changed.connect(_on_text_changed)
	add_child(_input_edit)
	_input_edit.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_input_edit.anchor_right = INPUT_WIDTH_RATIO
	_input_edit.visible = false
	
	# Ghost label for inline autocomplete suggestions
	_ghost_label = Label.new()
	_ghost_label.add_theme_font_override("font", MUNRO_FONT)
	_ghost_label.add_theme_color_override("font_color", Color(1, 1, 1, 0.3))
	_ghost_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_ghost_label.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_ghost_label.offset_left = 0
	_ghost_label.offset_top = 0
	_ghost_label.offset_right = 0
	_ghost_label.offset_bottom = 0
	_ghost_label.visible = false
	add_child(_ghost_label)
	_apply_input_layout()
	
	_add_message("Welcome! Type /help for a list of commands.", COLOR_SYSTEM)

func _process(delta):
	if is_open and not _ghost_text.is_empty():
		_pulse_time += delta
		var pulse_alpha = 0.25 + 0.15 * sin(_pulse_time * 3.0)
		_ghost_label.add_theme_color_override("font_color", Color(1, 1, 1, pulse_alpha))
	
	# Handle tab hold auto-cycling
	if _tab_held and not _completion_matches.is_empty():
		_tab_hold_time += delta
		if _tab_hold_time >= _tab_cycle_delay:
			_perform_tab_cycle(true)  # Forward cycle
			_tab_hold_time = 0.0
	
	# Handle up arrow hold auto-cycling
	if _up_held and not _completion_matches.is_empty():
		_up_hold_time += delta
		if _up_hold_time >= _tab_cycle_delay:
			_perform_tab_cycle(true)  # Forward cycle
			_up_hold_time = 0.0

func _input(event):
	if not is_open:
		if player_controller.is_inventory_open() or player_controller.is_settings_open():
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
		get_viewport().set_input_as_handled()
		return
	if event is InputEventMouseButton and event.pressed:
		match event.button_index:
			MOUSE_BUTTON_WHEEL_UP:
				_scroll_chat(1)
				get_viewport().set_input_as_handled()
			MOUSE_BUTTON_WHEEL_DOWN:
				_scroll_chat(-1)
				get_viewport().set_input_as_handled()
	if event is InputEventKey and event.pressed and not event.is_echo():
		match event.keycode:
			KEY_TAB:
				_tab_held = true
				_tab_hold_time = 0.0
				_on_tab()
				get_viewport().set_input_as_handled()
			KEY_UP:
				# If we have active completions, cycle forward through them (same as tab)
				if not _completion_matches.is_empty():
					_up_held = true
					_up_hold_time = 0.0
					_perform_tab_cycle(true)  # Forward cycle
					get_viewport().set_input_as_handled()
				else:
					_history_back()
					get_viewport().set_input_as_handled()
			KEY_DOWN:
				# If we have active completions, cycle backwards through them
				if not _completion_matches.is_empty():
					_perform_tab_cycle(false)  # Backward cycle
					get_viewport().set_input_as_handled()
				else:
					_history_forward()
					get_viewport().set_input_as_handled()
	elif event is InputEventKey and not event.pressed:
		match event.keycode:
			KEY_TAB:
				_tab_held = false
				_tab_hold_time = 0.0
			KEY_UP:
				_up_held = false
				_up_hold_time = 0.0

func _open_chat(initial_text: String = ""):
	is_open = true
	mouse_filter = Control.MOUSE_FILTER_STOP
	_apply_input_layout()
	_reset_completion()
	_scroll_offset = 0.0
	_input_edit.visible = true
	_input_edit.text = initial_text
	_input_edit.caret_column = initial_text.length()
	_ghost_label.visible = true
	_pulse_time = 0.0
	player_controller.set_chat_open(true)
	_input_edit.grab_focus()
	queue_redraw()
	_update_ghost_text()

func _close_chat():
	is_open = false
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_scroll_offset = 0.0
	_input_edit.visible = false
	_ghost_label.visible = false
	_ghost_text = ""
	_input_edit.release_focus()
	player_controller.set_chat_open(false)
	_history_index = -1
	queue_redraw()

func _on_text_changed(_new_text: String):
	if _completing:
		_update_ghost_text()
		return
	# Always reset completion when text changes to avoid stale matches
	_reset_completion()
	_update_ghost_text()

func _reset_completion():
	_completion_matches = []
	_completion_index = -1
	_completion_word_start = -1
	_completion_original_word = ""
	_tab_held = false
	_tab_hold_time = 0.0
	_up_held = false
	_up_hold_time = 0.0
	_ghost_text = ""
	_ghost_label.text = ""
	_ghost_label.visible = false

func _update_ghost_text():
	if not is_open:
		return
	
	var text := _input_edit.text
	var caret := _input_edit.caret_column
	var word_start := (text.rfind(" ", caret - 1) + 1) if caret > 0 else 0
	var word := text.substr(word_start, caret - word_start)
	var prefix := text.substr(0, word_start)
	
	# Don't show autocomplete if text is completely empty
	if text.is_empty():
		_ghost_text = ""
		_ghost_label.text = ""
		_ghost_label.visible = false
		return
	
	# Position ghost label to align right after the caret
	# Use the same font and size as LineEdit for accurate positioning
	var font := MUNRO_FONT
	var font_size := int(INPUT_FONT_SIZE * _chat_scale())
	
	# Calculate text width up to caret position
	var text_before_caret := text.substr(0, caret)
	var text_width := font.get_string_size(text_before_caret, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	
	# Account for LineEdit's internal padding (content_margin from StyleBox)
	var content_margin_left := 12.0
	var content_margin_top := 6.0
	
	# Position the ghost label exactly where the caret is
	_ghost_label.position.x = (H_MARGIN * _chat_scale()) + content_margin_left + text_width
	_ghost_label.position.y = _input_edit.position.y + content_margin_top
	
	# Check if we should show parameter hints for commands
	if word.is_empty() and not prefix.is_empty():
		var parts := prefix.split(" ", false)
		if parts.size() > 0:
			var cmd := parts[0].to_lower()
			var param_hint := _get_command_param_hint(cmd, parts.size())
			if not param_hint.is_empty():
				_ghost_text = param_hint
				_ghost_label.text = _ghost_text
				_ghost_label.visible = true
				return
	
	# If we have active completion matches (from tab cycling), use them
	if not _completion_matches.is_empty():
		# Show the current indexed match as ghost suggestion
		var current_match := _completion_matches[_completion_index]
		if current_match.length() > word.length():
			_ghost_text = current_match.substr(word.length())
			_ghost_label.text = _ghost_text
			_ghost_label.visible = true
		else:
			# Word is already complete, don't show ghost text
			_ghost_text = ""
			_ghost_label.text = ""
			_ghost_label.visible = false
			return
	else:
		# No active completion - get fresh matches based on current text
		var matches := _tab_candidates(prefix, word)
		if matches.is_empty():
			_ghost_text = ""
			_ghost_label.text = ""
			_ghost_label.visible = false
			return
		
		# Store matches for tab cycling and track word start
		_completion_matches = matches
		_completion_index = 0
		_completion_word_start = word_start
		_completion_original_word = word
		
		# Show the first match as ghost suggestion, but only if it's longer than current word
		var first_match := matches[0]
		if first_match.length() > word.length():
			_ghost_text = first_match.substr(word.length())
			_ghost_label.text = _ghost_text
			_ghost_label.visible = true
		else:
			# Word is already complete, don't show ghost text
			_ghost_text = ""
			_ghost_label.text = ""
			_ghost_label.visible = false
			return

func _get_command_param_hint(cmd: String, arg_count: int) -> String:
	match cmd:
		"/give":
			if arg_count == 1:
				return "<block>"
			elif arg_count >= 2:
				return "[count]"
		"/tp":
			if arg_count == 1:
				return "<x>"
			elif arg_count == 2:
				return "<y>"
			elif arg_count >= 3:
				return "<z>"
		"/help", "/fly", "/clearchat", "/clearinv", "/version":
			# These commands take no arguments
			return ""
		_:
			return ""
	return ""

func _on_tab():
	_perform_tab_cycle(true)

func _perform_tab_cycle(forward: bool):
	var text := _input_edit.text
	var caret := _input_edit.caret_column
	var word_start := (text.rfind(" ", caret - 1) + 1) if caret > 0 else 0
	var word := text.substr(word_start, caret - word_start)
	var prefix := text.substr(0, word_start)

	# If we're showing a parameter hint (no word to complete), start cycling through completions
	if word.is_empty() and not prefix.is_empty():
		var param_matches := _tab_candidates(prefix, word)
		if not param_matches.is_empty():
			# Initialize completion cycling
			_completion_matches = param_matches
			_completion_index = 0
			_completion_word_start = word_start
			_completion_original_word = word
			
			# Insert the first completion
			var first_completion := param_matches[0]
			_completing = true
			_input_edit.text = text + first_completion
			_input_edit.caret_column = word_start + first_completion.length()
			_completing = false
			_ghost_text = ""
			_ghost_label.text = ""
			_ghost_label.visible = false
		return

	var matches := _completion_matches
	if matches.is_empty():
		matches = _tab_candidates(prefix, word)
		if matches.is_empty():
			return
		_completion_matches = matches
		_completion_index = 0
		_completion_word_start = word_start
		_completion_original_word = word

	# If we have ghost text showing, accept it (complete the word)
	if not _ghost_text.is_empty():
		_completing = true
		_input_edit.text = text + _ghost_text + text.substr(caret)
		_input_edit.caret_column = caret + _ghost_text.length()
		_completing = false
		# Don't reset completion - allow cycling through other options
		_ghost_text = ""
		_ghost_label.text = ""
		_ghost_label.visible = false
		return

	# No ghost text - cycle through available matches based on original word
	if forward:
		_completion_index = (_completion_index + 1) % matches.size()
	else:
		_completion_index = (_completion_index - 1 + matches.size()) % matches.size()
	var completion := matches[_completion_index]
	
	# Find where the current completed word ends to remove it before adding new one
	# We need to find the end of whatever is currently at word_start
	var current_word_end := text.find(" ", word_start)
	if current_word_end == -1:
		current_word_end = text.length()
	
	var text_after_completion := text.substr(current_word_end)
	var new_text := prefix + completion + text_after_completion
	
	_completing = true
	_input_edit.text = new_text
	_input_edit.caret_column = _completion_word_start + completion.length()
	_completing = false
	_ghost_text = ""
	_ghost_label.text = ""
	_ghost_label.visible = false

func _tab_candidates(prefix: String, word: String) -> Array[String]:
	var out: Array[String] = []
	if prefix.is_empty():
		for c in COMMANDS:
			if c.begins_with(word):
				out.append(c)
	else:
		var parts := prefix.split(" ", false)
		if parts.size() == 1 and parts[0].to_lower() == "/give":
			for b in BlockTextures.get_block_names():
				if b.begins_with(word):
					out.append(b)
	# For other parameters, return empty so we can show parameter hints instead
	return out

func _longest_common_prefix(words: Array[String]) -> String:
	if words.is_empty():
		return ""
	var lcp := words[0]
	for w in words:
		while not w.begins_with(lcp):
			lcp = lcp.substr(0, lcp.length() - 1)
			if lcp.is_empty():
				break
		if lcp.is_empty():
			break
	return lcp

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
			_add_message("/clearchat - clear the chat", COLOR_SYSTEM)
			_add_message("/clearinv - clear your inventory", COLOR_SYSTEM)
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
		"/clearchat":
			messages.clear()
			_add_message("Chat cleared.", COLOR_SYSTEM)
		"/clearinv":
			player_controller.clear_inventory()
			_add_message("Inventory cleared.", COLOR_SUCCESS)
		"/version":
			_add_message("Farlands - Godot 4 + C++ GDExtension", COLOR_SYSTEM)
		_:
			_add_message("Unknown command: %s (type /help)" % parts[0], COLOR_ERROR)

func _add_message(text: String, color: Color):
	messages.append({"text": text, "color": color})
	if messages.size() > MAX_MESSAGES:
		messages.remove_at(0)
	_scroll_offset = 0.0
	queue_redraw()

func _chat_total_height() -> float:
	var wrap_width := maxf(size.x - (H_MARGIN * _chat_scale()) * 2.0, 1.0)
	var total := 0.0
	for m in messages:
		total += MUNRO_FONT.get_multiline_string_size(
				m.text, HORIZONTAL_ALIGNMENT_LEFT, wrap_width, int(LOG_FONT_SIZE * _chat_scale())).y
	return total

func _chat_max_scroll() -> float:
	var view_height := size.y - (INPUT_HEIGHT * _chat_scale()) - (H_MARGIN * _chat_scale()) - 10.0
	return maxf(_chat_total_height() - view_height, 0.0)

func _scroll_chat(direction: int):
	# direction: +1 = wheel up (older history), -1 = wheel down (newer)
	_scroll_offset += direction * MUNRO_FONT.get_height(int(LOG_FONT_SIZE * _chat_scale()))
	_scroll_offset = clampf(_scroll_offset, 0.0, _chat_max_scroll())
	queue_redraw()

func _draw():
	if messages.is_empty():
		return
	var wrap_width := maxf(size.x - (H_MARGIN * _chat_scale()) * 2.0, 1.0)
	var shadow := Color(0.09, 0.09, 0.09)
	# Newest message at the bottom (closest to the input box), older ones
	# stacked above it, Minecraft-style. Long text wraps onto extra lines.
	# draw_multiline_string's pos is the baseline of the FIRST line, so place
	# the first line up by the wrapped height so the bottom line stays in the
	# same spot the old single-line text used.
	var line_height := MUNRO_FONT.get_height(int(LOG_FONT_SIZE * _chat_scale()))
	var bottom_y := size.y - (INPUT_HEIGHT * _chat_scale()) - (H_MARGIN * _chat_scale()) - 10.0
	var heights: Array[float] = []
	heights.resize(messages.size())
	var total_height := 0.0
	for i in range(messages.size()):
		var h := MUNRO_FONT.get_multiline_string_size(
				messages[i].text, HORIZONTAL_ALIGNMENT_LEFT, wrap_width, int(LOG_FONT_SIZE * _chat_scale())).y
		heights[i] = h
		total_height += h
	# Scrolling shifts the whole stack down to reveal older messages above.
	_scroll_offset = clampf(_scroll_offset, 0.0, maxf(total_height - bottom_y, 0.0))
	var cursor := bottom_y + _scroll_offset  # Bottom-line baseline of the newest message
	for i in range(messages.size() - 1, -1, -1):
		var h := heights[i]
		var m = messages[i]
		var first_baseline := cursor - h + line_height
		# Cull messages outside the visible chat area
		var block_top := first_baseline - MUNRO_FONT.get_ascent(int(LOG_FONT_SIZE * _chat_scale()))
		var block_bottom := cursor + MUNRO_FONT.get_descent(int(LOG_FONT_SIZE * _chat_scale()))
		if block_top <= bottom_y and block_bottom >= 0.0:
			draw_multiline_string(MUNRO_FONT, Vector2((H_MARGIN * _chat_scale()) + 1, first_baseline + 1), m.text,
					HORIZONTAL_ALIGNMENT_LEFT, wrap_width, int(LOG_FONT_SIZE * _chat_scale()), -1, shadow)
			draw_multiline_string(MUNRO_FONT, Vector2((H_MARGIN * _chat_scale()), first_baseline), m.text,
					HORIZONTAL_ALIGNMENT_LEFT, wrap_width, int(LOG_FONT_SIZE * _chat_scale()), -1, m.color)
		cursor -= h
