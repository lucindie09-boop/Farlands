extends Control

@onready var player_controller = get_node("/root/Main/Player")
var inventory_texture: Texture2D = null

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9
const INVENTORY_SIZE = 27
const TOTAL_SLOTS = HOTBAR_SIZE + INVENTORY_SIZE

var selected_slot = -1
var is_open = false
var drag_slot = -1  # Slot being dragged
var drag_block_id = 0
var drag_count = 0

func _ready():
	# Load the inventory texture
	inventory_texture = load("res://textures/gui/inventory.png")
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	hide()

func _input(event):
	if event.is_action_pressed("toggle_inventory"):
		is_open = !is_open
		if is_open:
			show()
			player_controller.set_inventory_open(true)
		else:
			hide()
			player_controller.set_inventory_open(false)

func _process(_delta):
	if is_open:
		queue_redraw()
		
		# Update drag position if dragging
		if drag_slot >= 0:
			queue_redraw()

func _draw():
	if not player_controller:
		return
	
	# Draw inventory background
	if inventory_texture:
		var texture_width = inventory_texture.get_width()
		var texture_height = inventory_texture.get_height()
		var scale = 3.0  # Match hotbar scaling
		var scaled_width = texture_width * scale
		var scaled_height = texture_height * scale
		var texture_x = (size.x - scaled_width) / 2.0
		var texture_y = (size.y - scaled_height) / 2.0
		draw_texture_rect(inventory_texture, Rect2(texture_x, texture_y, scaled_width, scaled_height), false)
		
		# Calculate slot positions based on texture
		var slot_width = (float(texture_width) / 9.0) * scale  # 9 columns
		var slot_height = (float(texture_height) / 4.0) * scale  # 4 rows (3 main + 1 hotbar)
		var slot_spacing_x = slot_width * 0.1  # 10% spacing between slots
		var slot_spacing_y = slot_height * 0.1  # 10% spacing between rows
		var start_x = texture_x + slot_spacing_x / 2
		var start_y = texture_y + slot_spacing_y / 2
		var actual_slot_width = slot_width - slot_spacing_x
		var actual_slot_height = slot_height - slot_spacing_y
		
		# Draw hotbar slots (bottom row)
		for i in range(HOTBAR_SIZE):
			var slot_x = start_x + i * slot_width
			var slot_y = start_y + 3 * slot_height  # Bottom row
			_draw_slot(slot_x, slot_y, actual_slot_width, actual_slot_height, i, true)
		
		# Draw main inventory slots (3 rows above hotbar)
		for i in range(INVENTORY_SIZE):
			var row = i / 9
			var col = i % 9
			var slot_x = start_x + col * slot_width
			var slot_y = start_y + row * slot_height
			_draw_slot(slot_x, slot_y, actual_slot_width, actual_slot_height, i + HOTBAR_SIZE, false)
	else:
		# Fallback: draw without texture
		_draw_fallback_inventory()
	
	# Draw dragged item following mouse
	if drag_slot >= 0:
		var mouse_pos = get_local_mouse_position()
		var drag_size = 48
		var block_texture = _get_block_texture(drag_block_id)
		if block_texture:
			draw_texture_rect(block_texture, Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), false)
		else:
			var block_color = _get_block_color(drag_block_id)
			draw_rect(Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), block_color)

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
	
	# Draw selection highlight
	if selected_slot == slot_index:
		var highlight_margin = 2
		draw_rect(Rect2(x - highlight_margin, y - highlight_margin, 
					  width + highlight_margin * 2, height + highlight_margin * 2), 
				 Color(1.0, 1.0, 1.0, 0.5), false, 3)
	
	# Draw block icon if slot has blocks
	if block_id > 0 && count > 0:
		# Try to get actual block texture
		var block_texture = _get_block_texture(block_id)
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
			var count_text = str(count)
			var font_size = 16
			var count_pos = Vector2(x + width - 6, y + height - 6)
			draw_string(ThemeDB.fallback_font, count_pos, count_text, HORIZONTAL_ALIGNMENT_RIGHT, -1, font_size)

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
		var row = i / 9
		var col = i % 9
		var slot_x = start_x + col * (slot_width + slot_spacing)
		var slot_y = start_y + row * (slot_height + slot_spacing)
		
		var is_hotbar = i < HOTBAR_SIZE
		_draw_slot(slot_x, slot_y, slot_width, slot_height, i, is_hotbar)

func _get_block_texture(block_id: int) -> Texture2D:
	# Try to load the block texture from the block definitions
	# This is a simplified version - in a full implementation, you'd use the BlockRegistry
	var texture_name = _get_block_texture_name(block_id)
	if texture_name.is_empty():
		return null
	
	var texture_path = "res://textures/blocks/" + texture_name + ".png"
	if ResourceLoader.exists(texture_path):
		return load(texture_path)
	return null

func _get_block_texture_name(block_id: int) -> String:
	# Simple mapping based on block_definitions.json structure
	match block_id:
		1: return "stone"
		2: return "dirt"
		3: return "grass"
		4: return "sand"
		5: return "water"
		6: return "wood"
		7: return "leaves"
		8: return "gravel"
		_: return ""

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
	if event is InputEventMouseButton:
		if event.pressed:
			# Handle slot selection on click
			var mouse_pos = event.position
			if is_open:
				_select_slot_at_position(mouse_pos)
		else:
			# Handle drop on mouse release
			if drag_slot >= 0:
				var mouse_pos = event.position
				_drop_slot_at_position(mouse_pos)
				drag_slot = -1
				drag_block_id = 0
				drag_count = 0

func _select_slot_at_position(pos: Vector2):
	# Calculate which slot was clicked
	if inventory_texture:
		var texture_width = inventory_texture.get_width()
		var texture_height = inventory_texture.get_height()
		var scale = 3.0  # Match hotbar scaling
		var scaled_width = texture_width * scale
		var scaled_height = texture_height * scale
		var start_x = (size.x - scaled_width) / 2.0
		var start_y = (size.y - scaled_height) / 2.0
		
		var slot_width = (float(texture_width) / 9.0) * scale
		var slot_height = (float(texture_height) / 4.0) * scale
		var slot_spacing_x = slot_width * 0.1
		var slot_spacing_y = slot_height * 0.1
		var actual_start_x = start_x + slot_spacing_x / 2
		var actual_start_y = start_y + slot_spacing_y / 2
		
		# Check if click is within inventory bounds
		if pos.x >= start_x && pos.x < start_x + scaled_width && pos.y >= start_y && pos.y < start_y + scaled_height:
			var rel_x = pos.x - actual_start_x
			var rel_y = pos.y - actual_start_y
			
			var col = int(rel_x / slot_width)
			var row = int(rel_y / slot_height)
			
			if col >= 0 && col < 9 && row >= 0 && row < 4:
				var slot_index = row * 9 + col
				if slot_index < TOTAL_SLOTS:
					selected_slot = slot_index
					if slot_index < HOTBAR_SIZE:
						player_controller.select_hotbar_slot(slot_index)
					
					# Start drag if slot has items
					var block_id = 0
					var count = 0
					if slot_index < HOTBAR_SIZE:
						block_id = player_controller.get_hotbar_slot_block_id(slot_index)
						count = player_controller.get_hotbar_slot_count(slot_index)
					else:
						var main_slot_index = slot_index - HOTBAR_SIZE
						block_id = player_controller.get_inventory_slot_block_id(main_slot_index)
						count = player_controller.get_inventory_slot_count(main_slot_index)
					
					if block_id > 0 && count > 0:
						drag_slot = slot_index
						drag_block_id = block_id
						drag_count = count
					
					queue_redraw()

func _drop_slot_at_position(pos: Vector2):
	# Calculate which slot was dropped on
	if inventory_texture:
		var texture_width = inventory_texture.get_width()
		var texture_height = inventory_texture.get_height()
		var scale = 3.0
		var scaled_width = texture_width * scale
		var scaled_height = texture_height * scale
		var start_x = (size.x - scaled_width) / 2.0
		var start_y = (size.y - scaled_height) / 2.0
		
		var slot_width = (float(texture_width) / 9.0) * scale
		var slot_height = (float(texture_height) / 4.0) * scale
		var slot_spacing_x = slot_width * 0.1
		var slot_spacing_y = slot_height * 0.1
		var actual_start_x = start_x + slot_spacing_x / 2
		var actual_start_y = start_y + slot_spacing_y / 2
		
		# Check if drop is within inventory bounds
		if pos.x >= start_x && pos.x < start_x + scaled_width && pos.y >= start_y && pos.y < start_y + scaled_height:
			var rel_x = pos.x - actual_start_x
			var rel_y = pos.y - actual_start_y
			
			var col = int(rel_x / slot_width)
			var row = int(rel_y / slot_height)
			
			if col >= 0 && col < 9 && row >= 0 && row < 4:
				var target_slot = row * 9 + col
				if target_slot < TOTAL_SLOTS && target_slot != drag_slot:
					# Move items from drag_slot to target_slot
					_move_slot_items(drag_slot, target_slot)
		else:
			# Dropped outside inventory - return items to original slot
			drag_slot = -1
			drag_block_id = 0
			drag_count = 0

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

func _move_slot_items(from_slot: int, to_slot: int):
	# Read what's in the destination before changing anything
	var dest_block_id = _get_slot_block_id(to_slot)
	var dest_count = _get_slot_count(to_slot)
	
	if dest_block_id == drag_block_id || dest_count == 0:
		# Same block (or empty slot): merge stacks, overflowing back into source
		var merged = drag_count + dest_count
		if merged <= 64:
			_set_slot(to_slot, drag_block_id, merged)
			_set_slot(from_slot, 0, 0)
		else:
			_set_slot(to_slot, drag_block_id, 64)
			_set_slot(from_slot, drag_block_id, merged - 64)
	else:
		# Incompatible block: swap the two slots' contents
		_set_slot(to_slot, drag_block_id, drag_count)
		_set_slot(from_slot, dest_block_id, dest_count)
	
	# Update hotbar selection if needed
	if to_slot < HOTBAR_SIZE:
		player_controller.select_hotbar_slot(to_slot)