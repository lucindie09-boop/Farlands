extends Control

@onready var player_controller = get_node("/root/Main/Player")
var hotbar_texture: Texture2D = null
var _highlight_texture: Texture2D = null  # pre-built recolored selected slot

## The dropped items in the world, found once: the node that owns the throw.
@onready var _items: Node = _find_dropped_items()

const SLOT_SIZE = 48
const HOTBAR_SIZE = 9
const MUNRO_FONT: Font = preload("res://Assets/fonts/munro.ttf")
const ItemNames := preload("res://scripts/item_names.gd")
const UIShatter := preload("res://scripts/ui_shatter.gd")
const UIIconThrow := preload("res://scripts/ui_icon_throw.gd")
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

# The pop a slot's icon plays when something ARRIVES in it -- the receiving half
# of the throw (ui_icon_throw.gd sends art out; a pickup lands it). The icon
# swells by POP_GROW about its centre and settles back over POP_TIME, so a pickup
# ends in the bar instead of a count silently ticking up beside it.
const POP_TIME = 0.18
const POP_GROW = 0.22

# The name of what you just took hold of, above the bar: vanilla says it, then
# takes it away.
#
# The size is 14 px of INK, which is not the same as a 14 px font: Munro's line
# box is taller than its letters. Measured by rasterising "Iron Sword" at each
# size - 14 px of font gives letters 10 px tall, 20 px gives 14 - so the font is
# 10 units, i.e. 20 px at the default scale of 2. Uniform, no stretch: the width
# is whatever the font draws the name at.
const NAME_FONT_UNITS = 10.0
const NAME_HOLD = 2.5
## The last half second of NAME_HOLD is the fade: solid until then, gone at the
## end.
const NAME_FADE = 0.5
## The text's TOP edge, in units above the bar's own top edge: 30 units is the
## 60 px above the bar asked for at the default scale of 2.
const NAME_ABOVE_UNITS = 30.0

func _ready():
	# Load the hotbar texture directly
	hotbar_texture = load("res://Assets/textures/gui/hotbar.png")
	_slot_pop.resize(HOTBAR_SIZE)
	# A held item that changes STATE without the slot gaining or losing anything
	# (a rod's cast/retract) is the same thing in the hand, so the id change it
	# causes answers with neither the arrival pop nor the name.
	if player_controller != null and player_controller.has_signal("item_swapped"):
		player_controller.item_swapped.connect(_on_item_swapped)
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
# The slot whose next id change is the held item's own state changing
# (`item_swapped`), not something arriving in or leaving it; -1 for none. A SLOT
# rather than a flag: two slots changing in one frame would otherwise let the
# other one take the quiet path. See `_on_item_swapped`.
var _state_swap_slot := -1
var _last_selected := -1
var _last_ui_scale := -1.0
var _last_size := Vector2.ZERO

# The pixels of a slot's icon still in the air after the last of a stack was
# spent: see scripts/ui_shatter.gd.
var _shards := UIShatter.new()

# The whole icons in the air after a throw: see scripts/ui_icon_throw.gd.
var _thrown := UIIconThrow.new()

# Slots whose last unit left by THROWING, so the spent-icon hook does not also
# come apart on top of the throw's own icon -- a drop is not destruction.
# Consumed (or forgotten) the next time _needs_redraw() looks at its slot.
var _thrown_out := {}

# Per-slot arrival pops, 0..1: how much of POP_TIME's swell each slot's icon is
# still owed. Driven by a count going UP in _needs_redraw(), which is the one
# place the HUD already sees every slot change.
var _slot_pop := PackedFloat32Array()

# The held item's name and how much of NAME_HOLD it has left.
var _name_text := ""
var _name_time := 0.0

func _process(delta):
	if _needs_redraw():
		queue_redraw()
	# The name is a clock of its own: it keeps the redraw gate open until it is
	# gone, or the bar would freeze with the last name still on it.
	if _name_time > 0.0:
		_name_time = maxf(0.0, _name_time - delta)
		queue_redraw()
	# The pixels and the thrown icons are moving for as long as they exist, so
	# their fall alone keeps the redraw gate open. Both advance every frame -- no
	# short-circuit -- or one of them would freeze behind the other's answer.
	var falling := _shards.advance(delta)
	falling = _thrown.advance(delta) or falling
	# The arrival pops are their own clock, and they keep the redraw gate open for
	# as long as any slot's icon is still settling.
	for i in range(HOTBAR_SIZE):
		if _slot_pop[i] > 0.0:
			_slot_pop[i] = maxf(0.0, _slot_pop[i] - delta / POP_TIME)
			falling = true
	if falling:
		queue_redraw()

# Redraw only when the drawn state actually changed (slot contents, selection,
# GUI scale, or window size). The previous code queued a redraw every frame,
# which forced hotbar _draw() (9 slot lookups + 9 draw_texture_rect calls into
# the C++ inventory) to run even when nothing moved.
func _needs_redraw() -> bool:
	if not player_controller:
		return false
	# One check's worth of state change: the id is already written when the signal
	# fires, so the next check is the one that sees it. Read once and cleared, so a
	# swap that somehow changes nothing cannot silence a later, unrelated change.
	var swap_slot := _state_swap_slot
	_state_swap_slot = -1
	if size != _last_size or not is_equal_approx(UIScale.value, _last_ui_scale):
		_last_size = size
		_last_ui_scale = UIScale.value
		return true
	var sel: int = player_controller.get_selected_hotbar_slot()
	if sel != _last_selected:
		# The first look at the bar is not a selection - it is the bar being
		# found - so the slot the game starts on does not introduce itself.
		if _last_selected >= 0:
			_announce_selection(sel)
		_last_selected = sel
		return true
	for i in range(HOTBAR_SIZE):
		var id: int = player_controller.get_hotbar_slot_block_id(i)
		var cnt: int = player_controller.get_hotbar_slot_count(i)
		# A slot refilled before its emptied frame came round is not a throw any
		# more: forget it, so the next real spend still comes apart.
		if cnt > 0:
			_thrown_out.erase(i)
		if id != _last_ids[i] or cnt != _last_counts[i]:
			if i == swap_slot:
				# The same item in its other state: nothing arrived, nothing was
				# spent, so no pop and no name -- just take the new id quietly.
				_last_ids[i] = id
				_last_counts[i] = cnt
				return true
			# More of it than there was: something ARRIVED in this slot, so its
			# icon plays the arrival pop. `_last_counts` starts at -1, which is
			# what makes the first look at a slot (a loaded inventory) not one.
			if cnt > _last_counts[i] and _last_counts[i] >= 0:
				_slot_pop[i] = 1.0
			# The last of a stack going is the item being spent: the icon it was
			# drawn as comes apart instead of just blinking out.
			if _last_counts[i] > 0 and cnt == 0 and _last_ids[i] > 0:
				if _thrown_out.has(i):
					# ...unless it left by being THROWN: the whole icon is already
					# out falling (ui_icon_throw.gd), and a shatter on top of it would
					# say the dropped item was destroyed.
					_thrown_out.erase(i)
				else:
					_spend_icon(i, _last_ids[i])
			# The selected stack leaving - dropped, or spent to nothing - takes the
			# name with it: a label that outlives its item is describing an empty slot.
			if cnt == 0 and i == sel:
				_name_text = ""
				_name_time = 0.0
			# ...and the other way round: something ARRIVING in the selected slot
			# (a pickup, a craft, or a different stack swapped in) says its name,
			# which is the case a selection change never sees. `_last_counts` >= 0
			# keeps the first look at a loaded inventory from introducing itself.
			if i == sel and _last_counts[i] >= 0 and cnt > 0 and (id != _last_ids[i]
					or _last_counts[i] <= 0):
				_announce_selection(sel)
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
	# The slot's own icon leaves the slot WHOLE and falls out of the bar after the
	# item (ui_icon_throw.gd), because the throw's own read is that the item is in
	# transit and not destroyed. The direction is its own: the aim says where the
	# item went, and this is the HUD reacting to the slot emptying, not a view of
	# the world -- so reading the aim's sideways part off the throw made the icon
	# leave east or west with where the player happened to be looking, and stood
	# still for a drop straight ahead. It picks a side instead.
	_throw_icon(slot, block_id)
	if count - dropped <= 0:
		# The slot emptying is NOT the spent-stack shatter: the icon that went was
		# this one (_needs_redraw), not one destroyed in place.
		_thrown_out[slot] = true

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
			# The one lookup the drawing and the throwing share (BlockIconArt): the
			# isometric render where there is one -- items like the stick have none --
			# and the block's own texture otherwise, so the art a throw clones is the
			# art the slot drew.
			var icon_rect := _slot_icon_rect(i, ui_scale)
			# A slot that has just received something swells about its centre and
			# settles back onto it (squared, so the pop starts fast and eases out).
			var pop: float = _slot_pop[i]
			if pop > 0.0:
				var grow := 1.0 + POP_GROW * pop * pop
				icon_rect = Rect2(icon_rect.get_center() - icon_rect.size * grow * 0.5,
					icon_rect.size * grow)
			var block_icon := BlockIconArt.texture(block_id)
			if block_icon:
				draw_texture_rect(block_icon, icon_rect, false)
			else:
				# No art at all: a coloured rectangle stands in, and there is nothing
				# in it for a throw to clone.
				draw_rect(icon_rect, _get_block_color(block_id))
			
			# Draw count text
			if count > 1:
				_draw_item_count(str(count), fill_x + fill_size, fill_y + fill_size, fill_size)
	# Spent items' pixels, over the bar they came off, and whole icons on their
	# way out of it after them -- drawn last so a throw is never hidden by debris.
	_shards.draw(self)
	_thrown.draw(self)
	# What the selection just became, over the bar: drawn last because it is about
	# the selection rather than part of the bar.
	_draw_held_name(ui_scale)

## A swap of the held item's own state, announced by the C++ inventory with the
## slot it changed: THAT slot's next id change is the state change, not a gain or
## a spend. Any other slot keeps its ordinary arrival/spend handling.
func _on_item_swapped(slot: int) -> void:
	_state_swap_slot = slot

## Say what the newly selected slot holds. An empty slot says nothing, the way
## vanilla's own label does.
func _announce_selection(slot: int) -> void:
	var count: int = player_controller.get_hotbar_slot_count(slot)
	_name_text = "" if count <= 0 else ItemNames.display_name(
			self, player_controller.get_hotbar_slot_block_id(slot))
	_name_time = NAME_HOLD if _name_text != "" else 0.0

## What the label's alpha is right now: solid until the fade starts, then down to
## nothing over NAME_FADE.
func _name_alpha() -> float:
	return clampf(_name_time / NAME_FADE, 0.0, 1.0)

## The held item's name, centred on the bar, its top edge NAME_ABOVE_UNITS above
## the bar's own - the vanilla label over the hotbar.
func _draw_held_name(ui_scale: float) -> void:
	if _name_text == "" or not hotbar_texture:
		return
	var alpha := _name_alpha()
	if alpha <= 0.0:
		return
	var font_size := int(roundf(NAME_FONT_UNITS * ui_scale))
	var text_width := MUNRO_FONT.get_string_size(_name_text, HORIZONTAL_ALIGNMENT_LEFT, -1,
			font_size).x
	var panel := _panel_rect(ui_scale)
	# draw_string takes the BASELINE, not the box's corner, so the ascent is what
	# pins the glyph tops on the line the label starts at.
	var pos := Vector2(floorf(panel.position.x + (panel.size.x - text_width) * 0.5),
			floorf(panel.position.y - NAME_ABOVE_UNITS * ui_scale
					+ MUNRO_FONT.get_ascent(font_size)))
	var shadow := Vector2(maxf(1.0, ui_scale), maxf(1.0, ui_scale))
	draw_string(MUNRO_FONT, pos + shadow, _name_text, HORIZONTAL_ALIGNMENT_LEFT, -1,
			font_size, Color(0.09, 0.09, 0.09, alpha))
	draw_string(MUNRO_FONT, pos, _name_text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size,
			Color(1.0, 1.0, 1.0, alpha))

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

## Clone the slot's icon and let it fall out of the bar (ui_icon_throw.gd). The
## art is the very texture the slot draws, from the very rect it is drawn in, so
## the thing that leaves is the thing that was there; a block with no art of its
## own (the coloured-rectangle fallback) has nothing to clone and no throw.
##
## The side is a random -1..1 rather than the aim's: the HUD is reacting to a slot
## emptying, and which way the player is facing is not part of that. It is uniform
## rather than left/right equally by coin flip so two throws rarely leave on the
## same spot, which is what makes a run of drops read as a run.
##
## The scale handed over is the GUI scale, so the throw covers the bar in the same
## proportion whatever the UI is drawn at -- the icon is bigger on screen at scale
## 2, and an unscaled throw would drop it a third of the way off the bar.
func _throw_icon(slot: int, block_id: int) -> void:
	var art := BlockIconArt.texture(block_id)
	if art == null:
		return
	_thrown.launch(art, _slot_icon_rect(slot, UIScale.value), randf_range(-1.0, 1.0), size.y, UIScale.value)


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
