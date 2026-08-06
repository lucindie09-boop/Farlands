extends Control

@onready var player_controller = get_node("/root/Main/Player")
var inventory_texture: Texture2D = null

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9
const INVENTORY_SIZE = 27
const TOTAL_SLOTS = HOTBAR_SIZE + INVENTORY_SIZE
const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")

# Slot grid geometry, measured from the #7e7d7d slot-background color.
# Only these 36 slots (hotbar + main inventory) have real data behind them --
# the flanking/crafting/output slots in the texture are decorative for now.
const GRID_LEFT = 16
const SLOT_PITCH = 21
const SLOT_SIZE_PX = 18
const MAIN_GRID_TOP = 105
const HOTBAR_TOP = 173

# Fill-key colors: pixels near FILL_BASE become FILL_HIGHLIGHT; everything
# else (frame/bevel outside the 18x18 box) is copied untouched.
const FILL_BASE = Color(0.494, 0.490, 0.490)       # #7e7d7d
const FILL_HIGHLIGHT = Color(0.612, 0.608, 0.608)  # #9c9b9b
const FILL_TOLERANCE = 0.012  # per channel, in 0..1 color space (~3/255)

var selected_slot = -1
var is_open = false
var hovered_slot = -1
var held_block_id = 0  # Stack held at the cursor (click-to-hold model)
var held_count = 0
var _hover_texture: Texture2D = null  # pre-built recolored hovered slot

# Drag state
var _drag_button = -1  # which button is currently held, -1 if none
var _drag_shift = false  # shift state captured at press time
var _drag_last_slot = -1  # last slot a drag-action fired for

func _ready():
	# Load the inventory texture
	inventory_texture = load("res://textures/gui/inventory.png")
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_hover_texture = _build_fill_hover_texture()
	hide()

func _build_fill_hover_texture() -> Texture2D:
	# Recolor the hovered slot from a pixel copy of the real art: only pixels
	# matching the #7e7d7d fill become #9c9b9b, leaving the slot's frame and
	# bevel ring outside the 18x18 box untouched.
	if not inventory_texture:
		return null
	var img = inventory_texture.get_image()
	var out = Image.create(SLOT_SIZE_PX, SLOT_SIZE_PX, false, Image.FORMAT_RGBA8)
	for y in range(SLOT_SIZE_PX):
		for x in range(SLOT_SIZE_PX):
			var px = img.get_pixel(GRID_LEFT + x, MAIN_GRID_TOP + y)
			if _is_fill_pixel(px, FILL_BASE, FILL_TOLERANCE):
				out.set_pixel(x, y, FILL_HIGHLIGHT)
			else:
				out.set_pixel(x, y, px)
	return ImageTexture.create_from_image(out)

func _is_fill_pixel(px: Color, base: Color, tolerance: float) -> bool:
	return absf(px.r - base.r) <= tolerance and absf(px.g - base.g) <= tolerance and absf(px.b - base.b) <= tolerance

func _input(event):
	# While the chat is open, E should type "e" into the chat box, not
	# toggle the inventory. Check first so the guard also covers ui_cancel.
	if player_controller and (player_controller.is_chat_open() or player_controller.is_settings_open()):
		return
	if event.is_action_pressed("toggle_inventory"):
		_toggle_inventory()
	elif event.is_action_pressed("ui_cancel") and is_open:
		_close_inventory()
		get_viewport().set_input_as_handled()

func _toggle_inventory():
	if is_open:
		_close_inventory()
	else:
		is_open = true
		hovered_slot = -1
		show()
		player_controller.set_inventory_open(true)
		queue_redraw()

func _close_inventory():
	is_open = false
	# Clear drag state to prevent stuck drags surviving close/reopen
	_drag_button = -1
	_drag_shift = false
	_drag_last_slot = -1
	hide()
	player_controller.set_inventory_open(false)
	queue_redraw()

func _process(_delta):
	# Redraw while holding so the held stack follows the mouse; hover
	# highlight updates are driven by InputEventMouseMotion in _gui_input.
	if is_open and _is_holding():
		queue_redraw()

func _slot_screen_rect(slot_index: int, texture_x: float, texture_y: float) -> Rect2:
	var col: int
	var row_top_px: float
	if slot_index < HOTBAR_SIZE:
		col = slot_index
		row_top_px = HOTBAR_TOP
	else:
		var i = slot_index - HOTBAR_SIZE
		col = i % 9
		row_top_px = MAIN_GRID_TOP + int(i / 9.0) * SLOT_PITCH
	var x = texture_x + (GRID_LEFT + col * SLOT_PITCH) * UIScale.value
	var y = texture_y + row_top_px * UIScale.value
	var s = SLOT_SIZE_PX * UIScale.value
	return Rect2(x, y, s, s)

func _draw():
	if not player_controller:
		return
	
	# Draw inventory background
	if inventory_texture:
		var texture_width = inventory_texture.get_width()
		var texture_height = inventory_texture.get_height()
		var ui_scale = UIScale.value  # Match hotbar scaling
		var scaled_width = texture_width * ui_scale
		var scaled_height = texture_height * ui_scale
		var texture_x = (size.x - scaled_width) / 2.0
		var texture_y = (size.y - scaled_height) / 2.0
		draw_texture_rect(inventory_texture, Rect2(texture_x, texture_y, scaled_width, scaled_height), false)
		
		# Draw all real slots (hotbar + main inventory) via shared geometry
		for i in range(TOTAL_SLOTS):
			var rect = _slot_screen_rect(i, texture_x, texture_y)
			_draw_slot(rect.position.x, rect.position.y, rect.size.x, rect.size.y, i, i < HOTBAR_SIZE)
	else:
		# Fallback: draw without texture
		_draw_fallback_inventory()
	
	# Draw held stack following the mouse
	if _is_holding():
		var mouse_pos = get_local_mouse_position()
		var drag_size = 48.0
		var block_texture = BlockTextures.get_texture(held_block_id)
		if block_texture:
			draw_texture_rect(block_texture, Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), false)
		else:
			var block_color = _get_block_color(held_block_id)
			draw_rect(Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), block_color)
		if held_count > 1:
			_draw_item_count(str(held_count), mouse_pos.x + drag_size / 2, mouse_pos.y + drag_size / 2, drag_size)

func _draw_slot(x, y, width, height, slot_index, is_hotbar):
	var block_id = 0
	var count = 0
	
	if is_hotbar:
		block_id = player_controller.get_hotbar_slot_block_id(slot_index)
		count = player_controller.get_hotbar_slot_count(slot_index)
	else:
		var main_slot_index = slot_index - HOTBAR_SIZE
		block_id = player_controller.get_inventory_slot_block_id(main_slot_index)
		count = player_controller.get_inventory_slot_count(main_slot_index)
	
	# Hover highlight: pixel-copy of the slot box with only #7e7d7d fill pixels
	# recolored to #9c9b9b, so the frame/bevel around the fill is left alone.
	if hovered_slot == slot_index and _hover_texture:
		draw_texture_rect(_hover_texture, Rect2(x, y, width, height), false)
	
	# Draw block icon if slot has blocks
	if block_id > 0 && count > 0:
		# Try to get actual block texture
		var block_texture = BlockTextures.get_texture(block_id)
		if block_texture:
			var icon_size = width * 0.8
			var icon_x = x + (width - icon_size) / 2.0
			var icon_y = y + (height - icon_size) / 2.0
			draw_texture_rect(block_texture, Rect2(icon_x, icon_y, icon_size, icon_size), false)
		else:
			# Fallback to colored rectangle
			var block_color = _get_block_color(block_id)
			var icon_size = width * 0.7
			var icon_x = x + (width - icon_size) / 2.0
			var icon_y = y + (height - icon_size) / 2.0
			draw_rect(Rect2(icon_x, icon_y, icon_size, icon_size), block_color)
		
		# Draw count text
		if count > 1:
			_draw_item_count(str(count), x + width, y + height, width)

func _draw_fallback_inventory():
	var slot_width = SLOT_SIZE
	var slot_height = SLOT_SIZE
	var slot_spacing = 4
	
	# Calculate starting position to center the inventory
	var hotbar_width = HOTBAR_SIZE * slot_width + (HOTBAR_SIZE - 1) * slot_spacing
	var inv_width = hotbar_width
	var inv_height = 4 * slot_height + 3 * slot_spacing  # 4 rows
	var start_x = (size.x - inv_width) / 2
	var start_y = (size.y - inv_height) / 2
	
	# Draw all slots
	for i in range(TOTAL_SLOTS):
		var row = int(i / 9.0)
		var col = i % 9
		var slot_x = start_x + col * (slot_width + slot_spacing)
		var slot_y = start_y + row * (slot_height + slot_spacing)
		
		var is_hotbar = i < HOTBAR_SIZE
		_draw_slot(slot_x, slot_y, slot_width, slot_height, i, is_hotbar)

func _draw_item_count(count_text: String, right_x: float, bottom_y: float, slot_size: float) -> void:
	# draw_string positions the BASELINE at pos (not the text box corner), and
	# horizontal alignment is ignored when width is -1, so back the position off
	# by the text's measured width and font descent to pin the glyphs inside the
	# slot's bottom-right corner.
	var font_size = int(round(slot_size * 0.5))       # ~half the slot height, like Minecraft
	var margin = max(1.0, slot_size / 18.0)             # scales with slot size instead of being flat
	var text_width = MUNRO_FONT.get_string_size(count_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	var descent = MUNRO_FONT.get_descent(font_size)
	var pos = Vector2(right_x - margin - text_width, bottom_y - margin - descent)
	var shadow = Vector2(margin * 0.5, margin * 0.5)
	draw_string(MUNRO_FONT, pos + shadow, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.09, 0.09, 0.09))
	draw_string(MUNRO_FONT, pos, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color.WHITE)

func _get_block_color(block_id: int) -> Color:
	# Fallback color mapping for when textures aren't available
	match block_id:
		1: return Color(0.5, 0.5, 0.5)  # Stone
		2: return Color(0.6, 0.4, 0.2)  # Dirt
		3: return Color(0.2, 0.6, 0.2)  # Grass
		4: return Color(0.8, 0.8, 0.7)  # Sand
		5: return Color(0.4, 0.4, 0.5)  # Water
		6: return Color(0.3, 0.3, 0.2)  # Wood
		7: return Color(0.2, 0.5, 0.2)  # Leaves
		8: return Color(0.4, 0.4, 0.4)  # Gravel
		_: return Color(0.5, 0.5, 0.5)  # Default gray

func _gui_input(event):
	if not is_open:
		return
	
	if event is InputEventMouseMotion:
		var slot = _slot_at_position(event.position)
		if slot != hovered_slot:
			hovered_slot = slot
			queue_redraw()
		
		# Check for drag action: armed button and slot changed since last action
		if _drag_button >= 0 and slot != _drag_last_slot:
			if slot >= 0:  # Only fire when entering a slot, not when leaving to background
				_handle_drag_action(slot)
			_drag_last_slot = slot
	
	elif event is InputEventMouseButton:
		var slot = _slot_at_position(event.position)
		
		# Handle mouse wheel events first (before any drag-arming logic)
		if event.button_index == MOUSE_BUTTON_WHEEL_UP or event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if slot >= 0:
				_handle_scroll_transfer(slot, event)
			return  # Don't process wheel events as drag buttons
		
		if event.pressed:
			# Button press: stash drag state and run existing single-click behavior
			if slot >= 0:
				# Handle double-click gather (second press of double-click)
				if event.button_index == MOUSE_BUTTON_LEFT and event.double_click and _is_holding():
					_collect_all_matching()
					return  # Skip normal click handling for this second press
				
				_drag_button = event.button_index
				_drag_shift = Input.is_key_pressed(KEY_SHIFT)
				_drag_last_slot = slot
				
				if event.button_index == MOUSE_BUTTON_LEFT:
					_left_click_slot(slot)
				elif event.button_index == MOUSE_BUTTON_RIGHT:
					_right_click_slot(slot)
		else:
			# Button release: clear drag state
			if event.button_index == _drag_button:
				_drag_button = -1
				_drag_shift = false
				_drag_last_slot = -1

func _slot_at_position(pos: Vector2) -> int:
	if not inventory_texture:
		return -1
	var texture_x = (size.x - inventory_texture.get_width() * UIScale.value) / 2.0
	var texture_y = (size.y - inventory_texture.get_height() * UIScale.value) / 2.0
	for i in range(TOTAL_SLOTS):
		if _slot_screen_rect(i, texture_x, texture_y).has_point(pos):
			return i
	return -1

func _is_holding() -> bool:
	return held_block_id > 0 and held_count > 0

func _clear_held():
	held_block_id = 0
	held_count = 0

func _left_click_slot(slot: int):
	selected_slot = slot
	if slot < HOTBAR_SIZE:
		player_controller.select_hotbar_slot(slot)
	
	# Handle shift-click quick-transfer
	if Input.is_key_pressed(KEY_SHIFT):
		_quick_transfer_slot(slot)
		return
	
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	if not _is_holding():
		# Pick up the whole stack
		if slot_id > 0 and slot_count > 0:
			held_block_id = slot_id
			held_count = slot_count
			_set_slot(slot, 0, 0)
	else:
		if slot_id == 0:
			# Empty slot: place the entire held stack
			_set_slot(slot, held_block_id, held_count)
			_clear_held()
		elif slot_id == held_block_id:
			# Same block: merge up to 64, overflow stays in hand
			var merged = held_count + slot_count
			if merged <= 64:
				_set_slot(slot, held_block_id, merged)
				_clear_held()
			else:
				_set_slot(slot, held_block_id, 64)
				held_count = merged - 64
		else:
			# Different block: swap the two stacks
			_set_slot(slot, held_block_id, held_count)
			held_block_id = slot_id
			held_count = slot_count
	queue_redraw()

func _right_click_slot(slot: int):
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	if not _is_holding():
		# Pick up half the stack (rounded up), leave the rest
		if slot_id > 0 and slot_count > 0:
			var pick = int((slot_count + 1) / 2.0)
			held_block_id = slot_id
			held_count = pick
			var rest = slot_count - pick
			if rest <= 0:
				_set_slot(slot, 0, 0)
			else:
				_set_slot(slot, slot_id, rest)
	else:
		# Place one item using shared helper (same logic as RMB drag)
		_try_place_one(slot)
	queue_redraw()

func _get_slot_block_id(slot_index: int) -> int:
	if slot_index < HOTBAR_SIZE:
		return player_controller.get_hotbar_slot_block_id(slot_index)
	return player_controller.get_inventory_slot_block_id(slot_index - HOTBAR_SIZE)

func _get_slot_count(slot_index: int) -> int:
	if slot_index < HOTBAR_SIZE:
		return player_controller.get_hotbar_slot_count(slot_index)
	return player_controller.get_inventory_slot_count(slot_index - HOTBAR_SIZE)

func _set_slot(slot_index: int, block_id: int, count: int):
	if slot_index < HOTBAR_SIZE:
		player_controller.set_hotbar_slot(slot_index, block_id, count)
	else:
		player_controller.set_inventory_slot(slot_index - HOTBAR_SIZE, block_id, count)

func _is_hotbar_slot(slot: int) -> bool:
	return slot < HOTBAR_SIZE

func _handle_drag_action(slot: int):
	# Handle drag actions based on button and shift state
	# Shift has highest priority (explicit modifier overrides hold-based behaviors)
	if _drag_button == MOUSE_BUTTON_LEFT and _drag_shift:
		# Shift-drag quick-transfer: move to other zone
		_quick_transfer_slot(slot)
	elif _drag_button == MOUSE_BUTTON_RIGHT and _is_holding():
		# RMB drag-place: place 1 unit per slot entered
		_try_place_one(slot)
	elif _drag_button == MOUSE_BUTTON_LEFT and _is_holding():
		# LMB drag-collect: collect matching blocks from slot
		_try_collect_from_slot(slot)

func _try_place_one(slot: int) -> bool:
	# Shared helper for RMB place compatibility check
	# Returns true if placement succeeded, false if blocked
	if not _is_holding():
		return false
	
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	# Can place if slot is empty or has same block with room
	if slot_id == 0:
		_set_slot(slot, held_block_id, 1)
		held_count -= 1
	elif slot_id == held_block_id and slot_count < 64:
		_set_slot(slot, slot_id, slot_count + 1)
		held_count -= 1
	else:
		return false  # Incompatible or full
	
	if held_count <= 0:
		_clear_held()
	
	queue_redraw()
	return true

func _try_collect_from_slot(slot: int):
	# LMB drag-collect: sweep as much as fits from matching slot
	if not _is_holding():
		return
	
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	# Only collect if block matches
	if slot_id == held_block_id and slot_count > 0:
		var space_left = 64 - held_count
		if space_left > 0:
			var take = min(slot_count, space_left)
			held_count += take
			_set_slot(slot, slot_id, slot_count - take)
			queue_redraw()

func _quick_transfer_slot(slot: int):
	# Shift-click/drag: move stack to other zone
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	if slot_id == 0 or slot_count == 0:
		return
	
	var is_source_hotbar = _is_hotbar_slot(slot)
	var source_is_hotbar = is_source_hotbar
	
	# Move as much as possible to the other zone
	var moved = _quick_transfer_to_zone(slot_id, slot_count, not source_is_hotbar)
	
	if moved > 0:
		var remaining = slot_count - moved
		if remaining <= 0:
			_set_slot(slot, 0, 0)
		else:
			_set_slot(slot, slot_id, remaining)
		queue_redraw()

func _quick_transfer_to_zone(block_id: int, count: int, target_is_hotbar: bool) -> int:
	# Move count of block_id to target zone (hotbar or main inventory)
	# Returns how many were actually moved
	var moved = 0
	var remaining = count
	
	# First, fill existing same-block slots in target zone
	var zone_size = HOTBAR_SIZE if target_is_hotbar else INVENTORY_SIZE
	var zone_offset = 0 if target_is_hotbar else HOTBAR_SIZE
	
	for i in range(zone_size):
		if remaining <= 0:
			break
		
		var slot_index = zone_offset + i
		var slot_id = _get_slot_block_id(slot_index)
		var slot_count = _get_slot_count(slot_index)
		
		if slot_id == block_id and slot_count < 64:
			var space = 64 - slot_count
			var add = min(remaining, space)
			_set_slot(slot_index, block_id, slot_count + add)
			moved += add
			remaining -= add
	
	# Then, put remainder in first empty slot
	if remaining > 0:
		for i in range(zone_size):
			if remaining <= 0:
				break
			
			var slot_index = zone_offset + i
			var slot_id = _get_slot_block_id(slot_index)
			
			if slot_id == 0:
				var add = min(remaining, 64)
				_set_slot(slot_index, block_id, add)
				moved += add
				remaining -= add
				break  # Only fill one empty slot
	
	return moved

func _handle_scroll_transfer(slot: int, event: InputEventMouseButton):
	# Scroll wheel quick-transfer between zones
	var slot_id = _get_slot_block_id(slot)
	var slot_count = _get_slot_count(slot)
	
	if event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
		# Scroll down: push 1 unit from hovered slot to other zone
		if slot_id > 0 and slot_count > 0:
			var is_source_hotbar = _is_hotbar_slot(slot)
			var moved = _quick_transfer_to_zone(slot_id, 1, not is_source_hotbar)
			if moved > 0:
				var remaining = slot_count - moved
				if remaining <= 0:
					_set_slot(slot, 0, 0)
				else:
					_set_slot(slot, slot_id, remaining)
				queue_redraw()
	
	elif event.button_index == MOUSE_BUTTON_WHEEL_UP:
		# Scroll up: pull 1 unit from other zone of same block type
		if slot_id > 0:
			# Only pull if slot has a block type to match against
			var is_source_hotbar = _is_hotbar_slot(slot)
			var source_is_hotbar = is_source_hotbar
			var target_is_hotbar = not source_is_hotbar
			
			# Search other zone for same block
			var zone_size = HOTBAR_SIZE if target_is_hotbar else INVENTORY_SIZE
			var zone_offset = 0 if target_is_hotbar else HOTBAR_SIZE
			
			for i in range(zone_size):
				var source_slot = zone_offset + i
				var source_id = _get_slot_block_id(source_slot)
				var source_count = _get_slot_count(source_slot)
				
				if source_id == slot_id and source_count > 0:
					# Found matching block, pull 1 unit
					var target_space = 64 - slot_count
					if target_space > 0:
						_set_slot(source_slot, source_id, source_count - 1)
						_set_slot(slot, slot_id, slot_count + 1)
						queue_redraw()
						break

func _collect_all_matching():
	# Double-click: sweep every slot (hotbar + main) matching the held block into the cursor stack
	if not _is_holding():
		return
	
	for i in range(TOTAL_SLOTS):
		if held_count >= 64:
			break
		
		var slot_id = _get_slot_block_id(i)
		var slot_count = _get_slot_count(i)
		
		if slot_id == held_block_id and slot_count > 0:
			var take = min(slot_count, 64 - held_count)
			held_count += take
			var remaining = slot_count - take
			if remaining <= 0:
				_set_slot(i, 0, 0)
			else:
				_set_slot(i, slot_id, remaining)
	
	queue_redraw()
