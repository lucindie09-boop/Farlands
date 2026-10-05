extends Control

@onready var player_controller = get_node("/root/Main/Player")
var hotbar_texture: Texture2D = null
var _highlight_texture: Texture2D = null  # pre-built recolored selected slot

## The dropped items in the world, found once: the node that owns the throw.
@onready var _items: Node = _find_dropped_items()

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9
const MUNRO_FONT: Font = preload("res://fonts/munro.ttf")
const UIShatter := preload("res://scripts/ui_shatter.gd")
const BlockIconArt := preload("res://scripts/block_icon_art.gd")

# Slot fill geometry measured from hotbar.png: the #262505 fill is a 16x16 px
# region inset (3,3) in a 20-px-pitch cell.
const SLOT_FILL_X = 3
const SLOT_FILL_Y = 3
const SLOT_FILL_SIZE = 16
const SLOT_PITCH = 20

# An item is 16 units across in every slot, whatever the slot art's inner box
# measures. Whole-unit destinations at every scale are what keep an item on the
# same grid as the panel behind it, and the icon renderer's size divides 16.
const ICON_SIZE_UNITS = 16

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

var _last_ids: Array[int] = [-1, -1, -1, -1, -1, -1, -1, -1, -1]
var _last_counts: Array[int] = [-1, -1, -1, -1, -1, -1, -1, -1, -1]
var _last_selected := -1
var _last_ui_scale := -1.0
var _last_size := Vector2.ZERO

# The pixels of a slot's icon still in the air after the last of a stack was
# spent: see scripts/ui_shatter.gd.
var _shards := UIShatter.new()

func _process(delta):
	if _needs_redraw():
		queue_redraw()
	# The pixels are moving for as long as they exist, so their fall alone keeps
	# the redraw gate open.
	if _shards.advance(delta):
		queue_redraw()

# Redraw only when the drawn state actually changed (slot contents, selection,
# GUI scale, or window size). The previous code queued a redraw every frame,
# which forced hotbar _draw() (9 slot lookups + 9 draw_texture_rect calls into
# the C++ inventory) to run even when nothing moved.
func _needs_redraw() -> bool:
	if not player_controller:
		return false
	if size != _last_size or not is_equal_approx(UIScale.value, _last_ui_scale):
		_last_size = size
		_last_ui_scale = UIScale.value
		return true
	var sel: int = player_controller.get_selected_hotbar_slot()
	if sel != _last_selected:
		_last_selected = sel
		return true
	for i in range(HOTBAR_SIZE):
		var id: int = player_controller.get_hotbar_slot_block_id(i)
		var cnt: int = player_controller.get_hotbar_slot_count(i)
		if id != _last_ids[i] or cnt != _last_counts[i]:
			# The last of a stack going is the item being spent: the icon it was
			# drawn as comes apart instead of just blinking out.
			if _last_counts[i] > 0 and cnt == 0 and _last_ids[i] > 0:
				_spend_icon(i, _last_ids[i])
			_last_ids[i] = id
			_last_counts[i] = cnt
			return true
	return false

func _input(event):
	# Scroll cycles the selected hotbar slot, wrapping around, and Q drops what is
	# in it. Both are ignored while the inventory is open so the wheel isn't
	# double-purposed there and a Q meant for the slot under the cursor isn't
	# swallowed, and while the chat is open so typing isn't interrupted.
	if not player_controller or player_controller.is_inventory_open() or player_controller.is_chat_open() or player_controller.is_settings_open():
		return
	if event.is_action_pressed("drop_item"):
		# is_action_pressed() is true for key repeats too; holding Q is one drop.
		if not event.is_echo():
			_drop_selected(event.ctrl_pressed)
		return
	if event is InputEventMouseButton and event.pressed:
		var current = player_controller.get_selected_hotbar_slot()
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			player_controller.select_hotbar_slot((current - 1 + HOTBAR_SIZE) % HOTBAR_SIZE)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			player_controller.select_hotbar_slot((current + 1) % HOTBAR_SIZE)

## Q: throw what is in the selected slot into the world. One unit, or the whole
## stack with Ctrl held. The item leaves through the C++ inventory -- the slot is
## written back with one fewer -- and what is left of the slot is drawn as it
## always was: the last unit going is the stack being spent, so the icon comes
## apart on its own, through _needs_redraw()'s spent-stack hook.
func _drop_selected(all: bool) -> void:
	var slot: int = player_controller.get_selected_hotbar_slot()
	var block_id: int = player_controller.get_hotbar_slot_block_id(slot)
	var count: int = player_controller.get_hotbar_slot_count(slot)
	if block_id <= 0 or count <= 0:
		return
	var dropped: int = count if all else 1
	# The item is thrown from the hand: where the aim enters the viewmodel, and
	# along the aim, so a throw from a still camera goes where the crosshair is.
	var camera := get_viewport().get_camera_3d()
	var dir := -camera.global_transform.basis.z
	if _items == null:
		return
	_items.spawn(block_id, dropped, camera.global_position, dir)
	player_controller.set_hotbar_slot(slot, block_id, count - dropped)
	# The throw PUNCHES: the same full-strength swing the hand makes when it hits
	# something, not the weaker place stroke -- nothing was put down, the item
	# left the hand. Reached through the player controller, the node the rest of the
	# HUD already holds: the viewmodel hangs off the CAMERA, and a path taken from
	# this HUD node resolves inside the HUD and finds nothing.
	var viewmodel := player_controller.get_node_or_null("Camera3D/Viewmodel")
	if viewmodel != null and viewmodel.has_method("punch"):
		viewmodel.punch()

## The world node items are thrown into: under Main, beside the player.
##
## Asked for by path rather than through the current scene, because the HUD is
## also instantiated by probes and smoke tests, which add Main.tscn to the tree
## without it being the tree's current scene. The path is the same one every
## other script here uses to reach the player.
func _find_dropped_items() -> Node:
	return get_node_or_null("/root/Main/DroppedItems")

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
	
	# Draw the texture centered at the bottom, snapped to the scale grid.
	var ui_scale = UIScale.value  # Global GUI scale
	var scaled_width = texture_width * ui_scale
	var scaled_height = texture_height * ui_scale
	var texture_x = UIScale.centered_origin(size.x, texture_width)
	var texture_y = UIScale.edge_origin(size.y, texture_height, 0.0)
	draw_texture_rect(hotbar_texture, Rect2(texture_x, texture_y, scaled_width, scaled_height), false)
	
	# Draw each hotbar slot content, positioned inside the exact 16x16 fill
	# box (SLOT_FILL_X/Y inset, SLOT_PITCH spacing) rather than the derived
	# texture_width/HOTBAR_SIZE pitch, so icons never drift across the bar.
	var fill_size = SLOT_FILL_SIZE * ui_scale
	for i in range(HOTBAR_SIZE):
		var fill_x = texture_x + (SLOT_FILL_X + i * SLOT_PITCH) * ui_scale
		var fill_y = texture_y + SLOT_FILL_Y * ui_scale
		
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
			# Try to get isometric block icon from BlockIconRenderer
			var icon_renderer = get_node_or_null("/root/BlockIconRenderer")
			var block_icon = null
			if icon_renderer != null:
				block_icon = icon_renderer.get_block_icon(block_id)
			
			var icon_rect := _slot_icon_rect(i, ui_scale)
			var icon_x = icon_rect.position.x
			var icon_y = icon_rect.position.y
			var icon_size = icon_rect.size.x
			if block_icon:
				draw_texture_rect(block_icon, Rect2(icon_x, icon_y, icon_size, icon_size), false)
			else:
				# Fallback to block texture (items like the stick have no iso icon)
				var block_texture = BlockTextures.get_texture(block_id)
				if block_texture:
					draw_texture_rect(block_texture, Rect2(icon_x, icon_y, icon_size, icon_size), false)
				else:
					# Fallback to colored rectangle
					draw_rect(Rect2(icon_x, icon_y, icon_size, icon_size), _get_block_color(block_id))
			
			# Draw count text
			if count > 1:
				_draw_item_count(str(count), fill_x + fill_size, fill_y + fill_size, fill_size)
	# Spent items' pixels, over the bar they came off.
	_shards.draw(self)

## Where slot `i`'s icon sits, in units: the one layout the icons and their
## shattering pixels both go through, so a spent stack can only ever come apart
## from the place its icon was drawn at.
func _slot_icon_rect(i: int, ui_scale: float) -> Rect2:
	return BlockIconArt.icon_rect(_slot_rect(i, ui_scale), ui_scale)

## The slot's own box, which is what the icon is centred in, and what its debris
## comes to rest on.
##
## NOT the panel's top edge, which is texel-accurate and still not the floor for
## this. A floor has to be at or below where the shards spawn, and these spawn
## inside the slot, which is panel-relative y 3..19 -- entirely underneath a top
## edge that sits at y 0..3. Given one, every shard fails its very first
## collision check (`pos.y + size.y > rest`) and is yanked up to above the bar,
## about 57 units straight through the hotbar's art. The panel's edge is the
## hearts' floor instead, because the hearts are drawn above the hotbar and fall
## onto it; see healthbar.gd:_hotbar_floor().
func _slot_rect(i: int, ui_scale: float) -> Rect2:
	var panel := _panel_rect(ui_scale)
	return Rect2(panel.position.x + (SLOT_FILL_X + i * SLOT_PITCH) * ui_scale,
		panel.position.y + SLOT_FILL_Y * ui_scale,
		SLOT_FILL_SIZE * ui_scale, SLOT_FILL_SIZE * ui_scale)

## The whole hotbar panel's box: where its art is drawn.
func _panel_rect(ui_scale: float) -> Rect2:
	var at := Vector2(UIScale.centered_origin(size.x, hotbar_texture.get_width()),
		UIScale.edge_origin(size.y, hotbar_texture.get_height(), 0.0))
	return Rect2(at, Vector2(hotbar_texture.get_width(), hotbar_texture.get_height()) * ui_scale)

## The last of a stack is gone: throw the icon it was drawn as.
func _spend_icon(slot: int, block_id: int) -> void:
	if hotbar_texture == null:
		return
	var pixels := BlockIconArt.pixels(block_id)
	if pixels["mask"].is_empty():
		return
	var ui_scale = UIScale.value
	_shards.burst(pixels["mask"], pixels["art"],
		_slot_icon_rect(slot, ui_scale).position, ui_scale, Color.WHITE, 0.0, pixels["colours"],
		1.0, UIShatter.Surface.box(_slot_rect(slot, ui_scale)))

func _draw_custom_hotbar():
	# Fallback custom drawing if texture not available
	var slot_width = SLOT_SIZE
	var slot_height = SLOT_SIZE
	var slot_spacing = 4
	
	var total_width = HOTBAR_SIZE * slot_width + (HOTBAR_SIZE - 1) * slot_spacing
	var start_x = (size.x - total_width) / 2
	var start_y = size.y - slot_height
	
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
				_draw_item_count(str(count), slot_x + slot_width, slot_y + slot_height, slot_width)
		
		var slot_num_text = str(i + 1)
		var num_font_size = 20
		# pos is the baseline, so push down by the ascent to pin the glyph tops at slot_y + 2
		var num_pos = Vector2(slot_x + 2, slot_y + 2 + MUNRO_FONT.get_ascent(num_font_size))
		draw_string(MUNRO_FONT, num_pos, slot_num_text, HORIZONTAL_ALIGNMENT_LEFT, -1, num_font_size)

func _draw_item_count(count_text: String, right_x: float, bottom_y: float, slot_size: float) -> void:
	# draw_string positions the BASELINE at pos (not the text box corner), and
	# horizontal alignment is ignored when width is -1, so back the position off
	# by the text's measured width and font descent to pin the glyphs inside the
	# slot's bottom-right corner.
	var font_size = int(round(slot_size * 0.5))       # half the slot's height, in units
	# One unit of inset, so the label sits on the same grid as the slot it is on.
	var margin = maxf(1.0, UIScale.value)
	var text_width = MUNRO_FONT.get_string_size(count_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
	var descent = MUNRO_FONT.get_descent(font_size)
	var pos = Vector2(right_x - margin - text_width, bottom_y - margin - descent)
	var shadow = Vector2(margin, margin)
	draw_string(MUNRO_FONT, pos + shadow, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color(0.09, 0.09, 0.09))
	draw_string(MUNRO_FONT, pos, count_text,
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, Color.WHITE)

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
