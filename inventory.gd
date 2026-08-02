extends Control

@onready var player_controller = get_node("/root/Main/Player")
var inventory_texture: Texture2D = null

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9
const INVENTORY_SIZE = 27
const TOTAL_SLOTS = HOTBAR_SIZE + INVENTORY_SIZE

# Slot grid geometry, measured from the #7e7d7d slot-background color.
# Only these 36 slots (hotbar + main inventory) have real data behind them --
# the flanking/crafting/output slots in the texture are decorative for now.
const GRID_LEFT = 16
const SLOT_PITCH = 21
const SLOT_SIZE_PX = 18
const MAIN_GRID_TOP = 105
const HOTBAR_TOP = 173
const TEX_SCALE = 3.0

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
	if event.is_action_pressed("toggle_inventory"):
		_toggle_inventory()
	elif event.is_action_pressed("ui_cancel") and is_open:
		_close_inventory()

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
		row_top_px = MAIN_GRID_TOP + int(i / 9) * SLOT_PITCH
	var x = texture_x + (GRID_LEFT + col * SLOT_PITCH) * TEX_SCALE
	var y = texture_y + row_top_px * TEX_SCALE
	var s = SLOT_SIZE_PX * TEX_SCALE
	return Rect2(x, y, s, s)

func _draw():
	if not player_controller:
		return
	
	# Draw inventory background
	if inventory_texture:
		var texture_width = inventory_texture.get_width()
		var texture_height = inventory_texture.get_height()
		var scale = TEX_SCALE  # Match hotbar scaling
		var scaled_width = texture_width * scale
		var scaled_height = texture_height * scale
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
		var drag_size = 48
		var block_texture = BlockTextures.get_texture(held_block_id)
		if block_texture:
			draw_texture_rect(block_texture, Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), false)
		else:
			var block_color = _get_block_color(held_block_id)
			draw_rect(Rect2(mouse_pos.x - drag_size/2, mouse_pos.y - drag_size/2, drag_size, drag_size), block_color)
		if held_count > 1:
			var count_pos = Vector2(mouse_pos.x + drag_size/2 - 6, mouse_pos.y + drag_size/2 - 6)
			draw_string(ThemeDB.fallback_font, count_pos, str(held_count), HORIZONTAL_ALIGNMENT_RIGHT, -1, 16)

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
		var row = int(i / 9)
		var col = i % 9
		var slot_x = start_x + col * (slot_width + slot_spacing)
		var slot_y = start_y + row * (slot_height + slot_spacing)
		
		var is_hotbar = i < HOTBAR_SIZE
		_draw_slot(slot_x, slot_y, slot_width, slot_height, i, is_hotbar)

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
	elif event is InputEventMouseButton and event.pressed:
		var slot = _slot_at_position(event.position)
		if slot < 0:
			return
		if event.button_index == MOUSE_BUTTON_LEFT:
			_left_click_slot(slot)
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_right_click_slot(slot)

func _slot_at_position(pos: Vector2) -> int:
	if not inventory_texture:
		return -1
	var texture_x = (size.x - inventory_texture.get_width() * TEX_SCALE) / 2.0
	var texture_y = (size.y - inventory_texture.get_height() * TEX_SCALE) / 2.0
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
			var pick = int((slot_count + 1) / 2)
			held_block_id = slot_id
			held_count = pick
			var rest = slot_count - pick
			if rest <= 0:
				_set_slot(slot, 0, 0)
			else:
				_set_slot(slot, slot_id, rest)
	else:
		# Place one item into an empty or same-block slot with room
		if slot_id == 0:
			_set_slot(slot, held_block_id, 1)
			held_count -= 1
		elif slot_id == held_block_id and slot_count < 64:
			_set_slot(slot, slot_id, slot_count + 1)
			held_count -= 1
		# Incompatible block or full stack: nothing happens
		if held_count <= 0:
			_clear_held()
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