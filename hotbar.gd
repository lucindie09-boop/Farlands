extends Control

@onready var player_controller = get_node("/root/Main/Player")
var hotbar_texture: Texture2D = null

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9

func _ready():
	# Load the hotbar texture directly
	hotbar_texture = load("res://textures/gui/hotbar.png")
	# Set nearest-neighbor filtering on this Control to keep hard edges when scaling
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST

func _process(_delta):
	queue_redraw()

func _draw():
	if not player_controller:
		return
	
	# Get hotbar texture dimensions if available
	var texture_width = 0
	var texture_height = 0
	if hotbar_texture:
		texture_width = hotbar_texture.get_width()
		texture_height = hotbar_texture.get_height()
	
	# If no texture, fall back to custom drawing
	if texture_width == 0 or texture_height == 0:
		_draw_custom_hotbar()
		return
	
	# Draw the texture centered at bottom with scaling
	var scale = 3.0  # Scale up the texture
	var scaled_width = texture_width * scale
	var scaled_height = texture_height * scale
	var texture_x = (size.x - scaled_width) / 2.0
	var texture_y = size.y - scaled_height - 20.0
	draw_texture_rect(hotbar_texture, Rect2(texture_x, texture_y, scaled_width, scaled_height), false)
	
	# Calculate slot dimensions (scaled)
	var slot_width = (float(texture_width) / float(HOTBAR_SIZE)) * scale
	var slot_height = float(texture_height) * scale
	
	# Draw each hotbar slot content
	for i in range(HOTBAR_SIZE):
		var slot_x = texture_x + i * slot_width
		var slot_y = texture_y
		
		# Get slot data from C++ inventory
		var block_id = player_controller.get_hotbar_slot_block_id(i)
		var count = player_controller.get_hotbar_slot_count(i)
		
		# Draw selection highlight
		var is_selected = (i == player_controller.get_selected_hotbar_slot())
		if is_selected:
			var highlight_margin = 2
			draw_rect(Rect2(slot_x - highlight_margin, slot_y - highlight_margin, 
						  slot_width + highlight_margin * 2, slot_height + highlight_margin * 2), 
					 Color(1.0, 1.0, 1.0, 0.3), false, 2)
		
		# Draw block icon if slot has blocks
		if block_id > 0 and count > 0:
			# Draw colored rectangle as block placeholder
			var block_color = _get_block_color(block_id)
			var icon_size = slot_width * 0.7
			var icon_x = slot_x + (slot_width - icon_size) / 2.0
			var icon_y = slot_y + (slot_height - icon_size) / 2.0
			draw_rect(Rect2(icon_x, icon_y, icon_size, icon_size), block_color)
			
			# Draw count text
			if count > 1:
				var count_text = str(count)
				var font_size = 16
				var count_pos = Vector2(slot_x + slot_width - 6, slot_y + slot_height - 6)
				draw_string(ThemeDB.fallback_font, count_pos, count_text, HORIZONTAL_ALIGNMENT_RIGHT, -1, font_size)

func _draw_custom_hotbar():
	# Fallback custom drawing if texture not available
	var slot_width = SLOT_SIZE
	var slot_height = SLOT_SIZE
	var slot_spacing = 4
	
	var total_width = HOTBAR_SIZE * slot_width + (HOTBAR_SIZE - 1) * slot_spacing
	var start_x = (size.x - total_width) / 2
	var start_y = size.y - slot_height - 20
	
	for i in range(HOTBAR_SIZE):
		var slot_x = start_x + i * (slot_width + slot_spacing)
		var slot_y = start_y
		
		var block_id = player_controller.get_hotbar_slot_block_id(i)
		var count = player_controller.get_hotbar_slot_count(i)
		
		var is_selected = (i == player_controller.get_selected_hotbar_slot())
		var slot_color = Color(0.1, 0.1, 0.1, 0.9) if is_selected else Color(0.0, 0.0, 0.0, 0.7)
		draw_rect(Rect2(slot_x, slot_y, slot_width, slot_height), slot_color)
		
		var border_color = Color(1.0, 1.0, 1.0, 0.9) if is_selected else Color(0.6, 0.6, 0.6, 0.6)
		var border_width = 3 if is_selected else 2
		draw_rect(Rect2(slot_x, slot_y, slot_width, slot_height), border_color, false, border_width)
		
		if block_id > 0 and count > 0:
			var block_color = _get_block_color(block_id)
			var icon_margin = 6
			draw_rect(Rect2(slot_x + icon_margin, slot_y + icon_margin, 
						  slot_width - icon_margin * 2, slot_height - icon_margin * 2), 
					 block_color)
			
			if count > 1:
				var count_text = str(count)
				var font_size = 14
				var count_pos = Vector2(slot_x + slot_width - 4, slot_y + slot_height - 4)
				draw_string(ThemeDB.fallback_font, count_pos, count_text, HORIZONTAL_ALIGNMENT_RIGHT, -1, font_size)
		
		var slot_num_text = str(i + 1)
		var num_font_size = 10
		var num_pos = Vector2(slot_x + 2, slot_y + 2)
		draw_string(ThemeDB.fallback_font, num_pos, slot_num_text, HORIZONTAL_ALIGNMENT_LEFT, -1, num_font_size)

func _get_block_color(block_id: int) -> Color:
	# Simple color mapping for different block types
	# In a full implementation, you'd use actual block textures
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
