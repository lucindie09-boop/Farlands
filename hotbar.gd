extends Control

@onready var player_controller = get_node("/root/Main/Player")
var hotbar_texture: Texture2D = null
var _highlight_texture: Texture2D = null  # pre-built recolored selected slot

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9

# Slot fill geometry measured from hotbar.png: the #262505 fill is a 16x16 px
# region inset (3,3) in a 20-px-pitch cell.
const SLOT_FILL_X = 3
const SLOT_FILL_Y = 3
const SLOT_FILL_SIZE = 16
const SLOT_PITCH = 20

# Fill-key colors: pixels near FILL_BASE (incl. dithered variants) become
# FILL_HIGHLIGHT; everything else is copied untouched.
const FILL_BASE = Color(0.149, 0.145, 0.0196)      # #262505
const FILL_HIGHLIGHT = Color(0.227, 0.224, 0.027)  # #3a3907
const FILL_TOLERANCE = 0.012  # per channel, in 0..1 color space (~3/255)

func _ready():
	# Load the hotbar texture directly
	hotbar_texture = load("res://textures/gui/hotbar.png")
	# Set nearest-neighbor filtering on this Control to keep hard edges when scaling
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_highlight_texture = _build_fill_highlight_texture()

func _build_fill_highlight_texture() -> Texture2D:
	# Recolor the selected slot from a pixel copy of the real art: only pixels
	# matching the #262505 fill (incl. dithered near-variants) become #3a3907,
	# so bevel corners and any other non-fill texels are left exactly as-is.
	if not hotbar_texture:
		return null
	var img = hotbar_texture.get_image()
	var out = Image.create(SLOT_FILL_SIZE, SLOT_FILL_SIZE, false, Image.FORMAT_RGBA8)
	for y in range(SLOT_FILL_SIZE):
		for x in range(SLOT_FILL_SIZE):
			var px = img.get_pixel(SLOT_FILL_X + x, SLOT_FILL_Y + y)
			if _is_fill_pixel(px, FILL_BASE, FILL_TOLERANCE):
				out.set_pixel(x, y, FILL_HIGHLIGHT)
			else:
				out.set_pixel(x, y, px)
	return ImageTexture.create_from_image(out)

func _is_fill_pixel(px: Color, base: Color, tolerance: float) -> bool:
	return absf(px.r - base.r) <= tolerance and absf(px.g - base.g) <= tolerance and absf(px.b - base.b) <= tolerance

func _process(_delta):
	queue_redraw()

func _input(event):
	# Scroll cycles the selected hotbar slot, wrapping around. Ignored while
	# the inventory is open so the wheel isn't double-purposed there.
	if not player_controller or player_controller.is_inventory_open():
		return
	if event is InputEventMouseButton and event.pressed:
		var current = player_controller.get_selected_hotbar_slot()
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			player_controller.select_hotbar_slot((current - 1 + HOTBAR_SIZE) % HOTBAR_SIZE)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			player_controller.select_hotbar_slot((current + 1) % HOTBAR_SIZE)

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
	
	# Draw each hotbar slot content, positioned inside the exact 16x16 fill
	# box (SLOT_FILL_X/Y inset, SLOT_PITCH spacing) rather than the derived
	# texture_width/HOTBAR_SIZE pitch, so icons never drift across the bar.
	var fill_size = SLOT_FILL_SIZE * scale
	for i in range(HOTBAR_SIZE):
		var fill_x = texture_x + (SLOT_FILL_X + i * SLOT_PITCH) * scale
		var fill_y = texture_y + SLOT_FILL_Y * scale
		
		# Get slot data from C++ inventory
		var block_id = player_controller.get_hotbar_slot_block_id(i)
		var count = player_controller.get_hotbar_slot_count(i)
		
		# Draw selection highlight: pixel-copy of the slot's fill region with
		# only #262505-family fill pixels recolored to #3a3907. Non-fill
		# texels like the 4 shadow bevel corners stay untouched.
		var is_selected = (i == player_controller.get_selected_hotbar_slot())
		if is_selected and _highlight_texture:
			draw_texture_rect(_highlight_texture,
							  Rect2(fill_x, fill_y, fill_size, fill_size),
							  false)
		
		# Draw block icon if slot has blocks
		if block_id > 0 and count > 0:
			# Try to get actual block texture
			var block_texture = BlockTextures.get_texture(block_id)
			if block_texture:
				var icon_size = fill_size * 0.8
				var icon_x = fill_x + (fill_size - icon_size) / 2.0
				var icon_y = fill_y + (fill_size - icon_size) / 2.0
				draw_texture_rect(block_texture, Rect2(icon_x, icon_y, icon_size, icon_size), false)
			else:
				# Fallback to colored rectangle
				var block_color = _get_block_color(block_id)
				var icon_size = fill_size * 0.7
				var icon_x = fill_x + (fill_size - icon_size) / 2.0
				var icon_y = fill_y + (fill_size - icon_size) / 2.0
				draw_rect(Rect2(icon_x, icon_y, icon_size, icon_size), block_color)
			
			# Draw count text
			if count > 1:
				var count_text = str(count)
				var font_size = 16
				var count_pos = Vector2(fill_x + fill_size - 6, fill_y + fill_size - 6)
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
