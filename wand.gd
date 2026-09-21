extends Node

## The wand: a held tool whose clicks are answered HERE rather than in the world.
##
## `PlayerController` emits `wand_menu` / `wand_use` / `wand_confirm` when the
## held item declares the `wand` use action (data/items.json), and stops placing,
## mining and punching while one is held — so exactly one layer answers each
## click, and the wand can never chew a hole in whatever is behind the ghost.
##
## The flow, one click at a time:
##
##   middle click   open the menu: pick a function, then the build file it uses
##   right click    anchor the ghost where you are looking (again to move it)
##   left click     place it for real
##   Escape         drop the ghost
##
## The ghost is a MultiMesh of translucent cubes plus the volume's outline, and
## both come from ONE call (`ChunkManager.preview_schematic`) that plans the
## paste without writing: a 900k-cell build is one decode and one buffer, not a
## per-cell call from script, and the sample is strided so the shape still reads
## as a ghost of the whole thing. The numbers the HUD shows are the plan's, not
## the sample's — "showing 20,000 of 937,143 cells" is never a guess.
##
## Nothing here owns paste POLICY: the options handed to the preview and to the
## paste are the same Dictionary, built in one place (`_options`), because a
## preview that does not match what the click does is worse than no preview.

const FONT: Font = preload("res://fonts/munro.ttf")
const BUTTON_TEX: Texture2D = preload("res://textures/gui/button.png")
const CLOSE_TEX: Texture2D = preload("res://textures/gui/close_button.png")
# The square button art for the cards: the 50x50 version, not the 20x20 one the
# close button uses. A card is 192 px across, so the larger source is stretched
# less - the bevel is drawn in roughly 4 px blocks instead of 10 - which is the
# finer of the two and the reason it was picked. A card wears this as its
# background rather than a flat stylebox, so the grid reads as the same
# interface as every other button in the game.
const CARD_TEX: Texture2D = preload("res://textures/gui/button_square_large.png")
# The two states of any button that wears a texture: the art brightened under the
# pointer, darkened while held. Multiplied into the texture, so one image covers
# all three states.
const BUTTON_HOVER_TINT := Color(1.2, 1.2, 1.2)
const BUTTON_PRESS_TINT := Color(0.8, 0.8, 0.8)
const SchematicFiles := preload("res://schematic_files.gd")

# The interface grid, the same units the settings menu draws in, so the wand's
# menu is the same interface rather than a second one: a widget is UNIT_BUTTON_H
# units tall and its text UNIT_FONT, and the menu reads them at its own scale.
const UNIT_FONT := 8.0
const UNIT_BUTTON_H := 20.0
const UNIT_GAP := 4.0

# --- the menu, in the load menu's own numbers ---------------------------------
# The wand's menu is built from the SKIN MAKER'S LOAD MENU (settings_menu.gd's
# skin gallery), which is the closest thing the game has to a picker: the same
# panel colours, the same header with the title on the left and CLOSE on the
# right, and the same square bordered cards in a scrolling grid. Every number
# below is one of that menu's, so the two can be read side by side.
#
# The PANEL is the load menu at MENU_SCALE: it is read from a distance in the
# middle of the screen, so it stands where the load menu's panel does but much
# larger. Four times 660x504 is bigger than a 1080p screen, which
# MENU_MAX_FRACTION answers by clamping it to a share of the window, so the panel
# is always the same SHAPE and as large as the window can hold.
const MENU_SCALE := 4.0
const MENU_MAX_FRACTION := 0.92
# The CONTENT inside it is NOT scaled with the panel. At 4x, the load menu's own
# card comes out 430 px across and a row 90 px tall, which is enormous even in a
# panel this size - so the things in the panel are drawn at MENU_CONTENT_SCALE x
# the interface instead, and 1.0 means they are exactly the load menu's sizes:
# 16 px text, 192 px cards, 40 px buttons.
const MENU_CONTENT_SCALE := 1.0
# The load menu's panel, in interface units: 660 x 504 before the UI scale. Both
# are multiplied by MENU_SCALE to give the panel the wand's menu asks for.
const MENU_UNITS_WIDE := 660.0
const MENU_UNITS_TALL := 504.0
# A card: square, one of the load menu's 96-unit preview tiles.
const MENU_TILE_UNITS := 96.0
const MENU_TILE_GAP := 12.0   # between cards
const MENU_GAP := 8.0         # header to body
const MENU_TAB_W := 130.0    # a tab: wide enough for the longer of the two labels
# The panel's padding PER SIDE, in units: the load menu's stylebox margins (14
# left/right) plus its MarginContainer (8). Kept as a sum because the width the
# cards have to fit is the panel less exactly this, twice. Vertically it is 16
# at the top (10 + 6) and 18 at the bottom (14 + 4), which is the 34 counted into
# MENU_UNITS_TALL above rather than a constant, since nothing reads it.
const MENU_PANEL_PAD_H := 22.0

# The load menu's dark palette, except that the panel is half TRANSPARENT: the
# wand is aimed at the world, so the pane shows the build behind it rather than
# replacing it - and the cards are opaque art of their own (CARD_TEX), which is
# what keeps them readable on top of whatever is behind the glass.
const PANEL_BG := Color(0.1, 0.1, 0.12, 0.5)
const PANEL_BORDER := Color(0.28, 0.28, 0.3)
# The tab pair. The one you are looking at wears the same tint a selected card
# does, so "which of these is current" is one language everywhere in the menu.
const TAB_CURRENT := Color(0.7, 1.0, 0.8)
const TAB_IDLE := Color(0.72, 0.72, 0.72)

# How many cells the ghost draws. Past this the sample is strided: a preview is
# for reading the shape and the size, and one buffer that a frame can upload
# beats a perfect ghost that stalls the game.
const PREVIEW_CELLS := 20000

const GHOST_COLOR := Color(0.55, 0.86, 1.0, 0.26)
const OUTLINE_COLOR := Color(0.75, 1.0, 0.95, 0.55)
const HINT_FONT_COLOR := Color.WHITE
const TEXT_SHADOW := Color(0.0, 0.0, 0.0, 0.7)

# The menu's tabs. Two pages, and the pair is what the top of the panel is: the
# tools, and the builds those tools place.
const TABS := [
	{"id": "tools", "name": "BUILDING TOOLS"},
	{"id": "schematics", "name": "SCHEMATICS"},
]

# The wand's functions. One today; the menu is a list so the next one is an
# entry here plus the branch that implements it, not a new interface.
const FUNCTIONS := [
	{
		"id": "paste",
		"name": "PASTE BUILD",
		"hint": "Pick a build file, then right click in the world to aim it.",
		"needs_file": true,
	},
]

var _player: Node = null
var _chunk_manager: Node = null

# --- what the wand is set to do ---------------------------------------------
var _function_id := "paste"
var _file_name := ""
var _file_path := ""
var _file_bytes := PackedByteArray()
var _file_info := {}

# --- the menu ---------------------------------------------------------------
var _menu: Control = null
var _menu_body: VBoxContainer = null
# The panel's column: the tab row and the body, both rebuilt per page, because
# the tab tints have to follow the page they switch to.
var _menu_column: VBoxContainer = null
var _menu_page := "tools"  # or "schematics"
var _menu_open := false
var _panel: PanelContainer = null
# Pixels per interface unit INSIDE the menu, and the width its content has to
# work with. Both are decided when the menu is built (see _fit_menu) rather than
# read from the UI scale at every widget: the menu is the one surface that is
# larger than the interface, and it is only as large as the window can hold.
var _menu_m := 0.0
var _menu_content_px := 0.0

# --- the ghost --------------------------------------------------------------
var _ghost_root: Node3D = null
var _ghost: MultiMeshInstance3D = null
var _outline: MeshInstance3D = null
var _hint: Label = null
var _preview_active := false
var _preview_origin := Vector3i.ZERO
var _preview_info := {}

func _ui_scale() -> float:
	var scale_node := get_node_or_null("/root/UIScale")
	if scale_node != null:
		return scale_node.value
	return 2.0

func _ready() -> void:
	_player = get_node_or_null("/root/Main/Player")
	_chunk_manager = get_node_or_null("/root/Main/ChunkManager")
	if _player == null or _chunk_manager == null:
		push_warning("wand: no player or chunk manager; the wand will do nothing")
		return
	_player.wand_menu.connect(_on_menu_pressed)
	_player.wand_use.connect(_on_use_pressed)
	_player.wand_confirm.connect(_on_confirm_pressed)
	# Deferred: _ready runs while the scene is still building its children, and
	# the ghost has to hang off the 3D scene root rather than this HUD node (a
	# Node3D under a CanvasLayer has no world to render in).
	_build_ghost.call_deferred()
	_build_hint()
	# Start on the first function so the very first right click does something
	# rather than printing a hint about a menu the player has not seen yet.
	if not FUNCTIONS.is_empty():
		_function_id = FUNCTIONS[0]["id"]

func _process(_delta: float) -> void:
	# Putting the wand away puts its ghost away too: the ghost is the tool's, not
	# the world's, and leaving it behind would look like a planned paste.
	if (_preview_active or _menu_open) and not _player.is_wand_held():
		cancel_preview()
		close_menu()
	_update_hint()

# ---------------------------------------------------------------------------
# The item's clicks
# ---------------------------------------------------------------------------

func _on_menu_pressed() -> void:
	if _menu_open:
		close_menu()
	else:
		open_menu()

func _on_use_pressed() -> void:
	if _menu_open:
		return
	anchor_preview_from_aim()

func _on_confirm_pressed() -> void:
	if _menu_open:
		return
	confirm_paste()

func _unhandled_input(event: InputEvent) -> void:
	if not event.is_action_pressed("ui_cancel") or event.is_echo():
		return
	# Escape backs out one step at a time: the menu first (it is the modal one),
	# then the ghost.
	if _menu_open:
		close_menu()
		get_viewport().set_input_as_handled()
		return
	if _preview_active:
		cancel_preview()
		get_viewport().set_input_as_handled()

# ---------------------------------------------------------------------------
# The menu
# ---------------------------------------------------------------------------

func is_menu_open() -> bool:
	return _menu_open

func open_menu() -> void:
	if _menu == null:
		_build_menu()
	if _menu == null:
		return
	_menu_page = "tools"
	_refresh_menu()
	_menu.visible = true
	_menu_open = true
	# A modal like the inventory: no walking, no mining, and the cursor comes back
	# so the buttons can be clicked.
	_player.set_wand_menu_open(true)

func close_menu() -> void:
	if _menu != null:
		_menu.visible = false
	if _menu_open:
		_player.set_wand_menu_open(false)
	_menu_open = false

func get_function() -> String:
	return _function_id

func set_function(id: String) -> bool:
	for entry in FUNCTIONS:
		if entry["id"] == id:
			_function_id = id
			if _menu_open:
				_refresh_menu()
			return true
	return false

func get_file() -> String:
	return _file_name

## Reads a build file so the wand can preview and place it. Answers false (and
## leaves the current choice alone) when the file cannot be read.
func choose_file(file_name: String) -> bool:
	var path := SchematicFiles.resolve(file_name)
	if path.is_empty():
		return false
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return false
	var info: Dictionary = _chunk_manager.inspect_schematic(bytes, _options())
	if not info.get("ok", false):
		return false
	_file_name = path.get_file()
	_file_path = path
	_file_bytes = bytes
	_file_info = info
	# The file changed, so what is on screen is stale.
	cancel_preview()
	if _menu_open:
		_refresh_menu()
	return true

func _options() -> Dictionary:
	# One place, used by the preview AND the paste. The wand's own extras
	# (how many ghost cells) are dropped before a paste, since the writer has no
	# use for them.
	var options := {}
	if _preview_cells() > 0:
		options["preview_cells"] = _preview_cells()
	return options

func _paste_options() -> Dictionary:
	return {}

func _preview_cells() -> int:
	return PREVIEW_CELLS

func _refresh_menu() -> void:
	if _menu_column == null:
		return
	for child in _menu_column.get_children():
		# Hidden now, freed at the end of the frame: this runs from inside a
		# button's own `pressed` signal, and Godot refuses to free a node while
		# it is emitting. Hiding first is what keeps one frame from showing the
		# old page and the new one at once.
		child.visible = false
		child.queue_free()
	_build_tabs()
	_menu_body = VBoxContainer.new()
	_menu_body.add_theme_constant_override("separation", int(UNIT_GAP * _menu_m))
	# The body fills the panel, so a page puts its state line on the bottom edge
	# and its one growing row (the cards) in the middle of it.
	_menu_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_menu_column.add_child(_menu_body)
	if _menu_page == "schematics":
		_build_schematic_page()
	else:
		_build_tool_page()

## The tab row across the top, with CLOSE on the right. The tabs ARE the page
## title, which is why the panel has none: the label under the pointer is what
## says where you are.
func _build_tabs() -> void:
	var m := _menu_m
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", int(MENU_GAP * m))
	for entry in TABS:
		var btn := _make_button(String(entry["name"]), MENU_TAB_W)
		btn.modulate = TAB_CURRENT if String(entry["id"]) == _menu_page else TAB_IDLE
		btn.pressed.connect(func() -> void:
			_menu_page = String(entry["id"])
			_refresh_menu()
		)
		header.add_child(btn)
	var room := Control.new()
	room.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(room)
	# The icon, not the word: a square the height of the row, named so a caller
	# (or a probe) can find it without matching on text it does not have.
	var close := _make_icon_button(CLOSE_TEX, UNIT_BUTTON_H)
	close.name = "MenuClose"
	close.tooltip_text = "Close"
	close.pressed.connect(close_menu)
	header.add_child(close)
	_menu_column.add_child(header)

## The menu's state line, in the bottom left corner of the panel: what a right
## click in the world would place. On both tabs, because it is the one thing the
## whole menu is about.
func _build_build_line() -> void:
	var label := Label.new()
	label.add_theme_font_override("font", FONT)
	label.add_theme_font_size_override("font_size", int(UNIT_FONT * _menu_m))
	label.add_theme_color_override("font_color", Color(0.75, 1.0, 0.95))
	if _file_name.is_empty():
		label.text = "build: (none chosen)"
	else:
		label.text = "build: %s · %d cells" % [_file_name, int(_file_info.get("planned", 0))]
	_menu_body.add_child(label)

func _build_tool_page() -> void:
	var m := _menu_m
	var gap := MENU_TILE_GAP * m
	var tile := MENU_TILE_UNITS * m
	# The functions ARE the menu: one card each, the shape the galleries use, so
	# the next function is another card here rather than another kind of row.
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_menu_body.add_child(scroll)

	var grid := GridContainer.new()
	grid.columns = _tile_columns(tile, gap)
	grid.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	grid.add_theme_constant_override("h_separation", int(gap))
	grid.add_theme_constant_override("v_separation", int(gap))
	for entry in FUNCTIONS:
		var card := _make_tile(String(entry["name"]), tile, String(entry.get("hint", "")))
		# The chosen function reads as chosen: it is the one the clicks use.
		if entry["id"] == _function_id:
			card.modulate = Color(0.7, 1.0, 0.8)
		card.pressed.connect(func() -> void:
			set_function(String(entry["id"]))
			# A tool that works on a build is no use until one is chosen, so
			# picking it turns to the tab where that happens.
			if bool(entry.get("needs_file", false)):
				_menu_page = "schematics"
				_refresh_menu()
		)
		grid.add_child(card)
	scroll.add_child(grid)
	_build_build_line()

func _build_schematic_page() -> void:
	var m := _menu_m
	var gap := MENU_TILE_GAP * m
	var tile := MENU_TILE_UNITS * m
	var names := SchematicFiles.list()
	if names.is_empty():
		var none := Label.new()
		none.add_theme_font_override("font", FONT)
		none.add_theme_font_size_override("font_size", int(UNIT_FONT * m))
		none.add_theme_color_override("font_color", Color(1, 0.8, 0.8))
		none.text = "No build files. Put them in schematics/."
		_menu_body.add_child(none)
	else:
		# The same cards again, one per build. It scrolls, because a folder of
		# builds is longer than any panel and a menu that grew with the folder
		# would run off the window.
		var scroll := ScrollContainer.new()
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_menu_body.add_child(scroll)

		var grid := GridContainer.new()
		grid.columns = _tile_columns(tile, gap)
		grid.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
		grid.add_theme_constant_override("h_separation", int(gap))
		grid.add_theme_constant_override("v_separation", int(gap))
		for entry_name in names:
			# No per-file size on the card on purpose: reading one is a decode,
			# and the big ones are hundreds of milliseconds. The size appears in
			# the state line once a file is chosen, where it is read once.
			var card := _make_tile(SchematicFiles.display_name(entry_name), tile, entry_name)
			if entry_name == _file_name:
				card.modulate = Color(0.7, 1.0, 0.8)
			card.pressed.connect(func() -> void:
				if not choose_file(entry_name):
					_hint_flash("Could not read %s" % entry_name)
					return
				_menu_page = "tools"
				_refresh_menu()
			)
			grid.add_child(card)
		scroll.add_child(grid)
	_build_build_line()

## Decides the panel's size, the scale its content is drawn at, and the width the
## cards have to fit, all from the window. Called once, when the menu is built.
func _fit_menu() -> void:
	var s := _ui_scale()
	var panel_w: float = MENU_UNITS_WIDE * s * MENU_SCALE
	var panel_h: float = MENU_UNITS_TALL * s * MENU_SCALE
	# The window bounds the PANEL: MENU_SCALE times the load menu is wider and
	# taller than a 1080p screen, so it is the smaller of the two per axis. A
	# window with no size at all - the dummy driver behind --headless - bounds
	# nothing, and gets the size the panel asks for.
	var room := get_viewport().get_visible_rect().size
	if room.x >= 1.0 and room.y >= 1.0:
		panel_w = minf(panel_w, room.x * MENU_MAX_FRACTION)
		panel_h = minf(panel_h, room.y * MENU_MAX_FRACTION)
	# The content is drawn at its own scale, which the window does not touch.
	_menu_m = s * MENU_CONTENT_SCALE
	# Whole pixels: a fractional size centres with a floor, and the two gaps come
	# out differing by a pixel or two instead of matching.
	panel_w = floorf(panel_w)
	panel_h = floorf(panel_h)
	_menu_content_px = maxf(panel_w - 2.0 * MENU_PANEL_PAD_H * _menu_m, 1.0)
	if _panel != null:
		# A box of a size rather than a panel that grows to its content: the
		# panel is what stays on screen, and a page with more cards than fit
		# scrolls inside it instead of pushing its own border off the window.
		_panel.custom_minimum_size = Vector2(panel_w, panel_h)

## How many cards fit across the panel. The load menu hard-codes 5 columns for
## its own width; the wand's panel is whatever the window allowed, so the count
## comes from the room the cards actually have - which is what keeps them square
## and the grid looking like the gallery rather than a stretched row.
func _tile_columns(tile: float, gap: float) -> int:
	return maxi(int(floorf((_menu_content_px + gap) / (tile + gap))), 1)

func _build_menu() -> void:
	var root := Control.new()
	root.name = "WandMenu"
	# Nearest, exactly as the settings menu and the crafting table set on their own
	# root: the interface textures are pixel art, and Godot's canvas default is
	# LINEAR, which smears the button art as soon as it is scaled up. Every Control
	# under this one inherits it, because a child's filter is PARENT_NODE by
	# default - so this single line is what keeps the whole menu crisp, cards and
	# tabs and CLOSE alike.
	root.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_STOP
	root.visible = false
	add_child(root)

	var backdrop := ColorRect.new()
	backdrop.set_anchors_preset(Control.PRESET_FULL_RECT)
	backdrop.color = Color(0, 0, 0, 0.45)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	root.add_child(backdrop)

	# The panel is held by a full-rect container rather than anchored itself:
	# anchored on the middle it put its top-left corner ON the middle of the
	# screen and grew down-right from there, which is why the dialog sat off
	# centre. A container centres it, and keeps it centred as pages change size.
	var centre := CenterContainer.new()
	centre.set_anchors_preset(Control.PRESET_FULL_RECT)
	centre.mouse_filter = Control.MOUSE_FILTER_IGNORE
	root.add_child(centre)

	_panel = PanelContainer.new()
	centre.add_child(_panel)
	_fit_menu()

	var m := _menu_m
	# The load menu's panel: dark, rounded, a two-pixel border, and its padding
	# split the same way (the stylebox's own margins plus a MarginContainer).
	var style := StyleBoxFlat.new()
	style.bg_color = PANEL_BG
	style.border_color = PANEL_BORDER
	style.set_border_width_all(int(2 * m))
	style.set_corner_radius_all(int(6 * m))
	style.content_margin_left = int(14 * m)
	style.content_margin_right = int(14 * m)
	style.content_margin_top = int(10 * m)
	style.content_margin_bottom = int(14 * m)
	_panel.add_theme_stylebox_override("panel", style)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", int(8 * m))
	margin.add_theme_constant_override("margin_right", int(8 * m))
	margin.add_theme_constant_override("margin_top", int(6 * m))
	margin.add_theme_constant_override("margin_bottom", int(4 * m))
	_panel.add_child(margin)

	# Left empty: the tab row and the page are built by _refresh_menu, which runs
	# on every switch and so keeps the tabs' contents honest about which is on.
	_menu_column = VBoxContainer.new()
	_menu_column.add_theme_constant_override("separation", int(MENU_GAP * m))
	margin.add_child(_menu_column)

	_menu = root

## A full-width row button: `width` is in interface units and the height is a
## widget's, both read at the menu's own scale.
func _make_button(text: String, width: float) -> Button:
	return _make_button_px(text, Vector2(width, UNIT_BUTTON_H) * _menu_m)

## One card: a SQUARE button wearing the interface's own square button art
## (`button_square_large.png`, CARD_TEX) as its background, brightened under the
## pointer and darkened while held. The image is stretched to the whole card and
## there is NO 9-slice - the menu root's NEAREST filter is what makes that
## stretch read as pixel art, each source pixel a solid block, so the bevel scales
## with the card instead of smearing into a blur. The wand's cards carry a name
## where the skin and block galleries carry a rendered model, which is the one
## difference.
func _make_tile(text: String, side_px: float, hint: String) -> Button:
	var m := _menu_m
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.tooltip_text = hint
	btn.add_theme_font_override("font", FONT)
	btn.add_theme_font_size_override("font_size", int(UNIT_FONT * m))
	btn.add_theme_color_override("font_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color.WHITE)
	btn.add_theme_stylebox_override("normal", _square_style(CARD_TEX, Color.WHITE))
	btn.add_theme_stylebox_override("hover", _square_style(CARD_TEX, BUTTON_HOVER_TINT))
	btn.add_theme_stylebox_override("pressed", _square_style(CARD_TEX, BUTTON_PRESS_TINT))
	btn.add_theme_stylebox_override("focus", _square_style(CARD_TEX, Color.WHITE))
	btn.custom_minimum_size = Vector2(side_px, side_px)
	return btn

## Any texture as a button background: stretched to whatever box it is given,
## tinted for the state. One definition shared by the cards and the icon button,
## so the two cannot disagree about what a button looks like.
func _square_style(tex: Texture2D, tint: Color) -> StyleBoxTexture:
	var box := StyleBoxTexture.new()
	box.texture = tex
	box.modulate_color = tint
	return box

## A SQUARE icon button: the texture stretched onto a `side` (in units) square,
## so the icon stays 1:1 at every scale - the same shape settings_menu.gd uses
## for its reset and heading icons, and the reason CLOSE is a picture here rather
## than the word it used to be.
func _make_icon_button(tex: Texture2D, side: float) -> Button:
	var btn := Button.new()
	btn.text = ""
	btn.add_theme_stylebox_override("normal", _square_style(tex, Color.WHITE))
	btn.add_theme_stylebox_override("hover", _square_style(tex, BUTTON_HOVER_TINT))
	btn.add_theme_stylebox_override("pressed", _square_style(tex, BUTTON_PRESS_TINT))
	btn.add_theme_stylebox_override("focus", _square_style(tex, Color.WHITE))
	btn.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	btn.custom_minimum_size = Vector2(side, side) * _menu_m
	return btn

## A text button of an exact pixel size. Every other button in the menu lands
## here - the tabs and the cards - so their style and their text cannot differ.
func _make_button_px(text: String, size_px: Vector2) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.add_theme_font_override("font", FONT)
	btn.add_theme_font_size_override("font_size", int(UNIT_FONT * _menu_m))
	btn.add_theme_color_override("font_color", Color.WHITE)
	btn.add_theme_color_override("font_hover_color", Color.WHITE)
	btn.add_theme_color_override("font_pressed_color", Color.WHITE)
	var normal := StyleBoxTexture.new()
	normal.texture = BUTTON_TEX
	var hover := StyleBoxTexture.new()
	hover.texture = BUTTON_TEX
	hover.modulate_color = Color(1.2, 1.2, 1.2)
	var pressed := StyleBoxTexture.new()
	pressed.texture = BUTTON_TEX
	pressed.modulate_color = Color(0.75, 0.75, 0.75)
	btn.add_theme_stylebox_override("normal", normal)
	btn.add_theme_stylebox_override("hover", hover)
	btn.add_theme_stylebox_override("pressed", pressed)
	btn.add_theme_stylebox_override("focus", normal)
	btn.custom_minimum_size = size_px
	return btn

# ---------------------------------------------------------------------------
# The ghost
# ---------------------------------------------------------------------------

func _build_ghost() -> void:
	var parent := get_tree().current_scene
	if parent == null:
		parent = get_parent()
	if parent == null:
		return
	_ghost_root = Node3D.new()
	_ghost_root.name = "WandPreview"
	parent.add_child(_ghost_root)

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = GHOST_COLOR
	#   * both faces drawn (CULL_DISABLED), because the ghost is a volume you can
	#     stand inside: culling the back faces made a big build's preview vanish the
	#     moment the camera was within it.
	material.blend_mode = BaseMaterial3D.BLEND_MODE_MIX
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.no_depth_test = false
	#   * depth WRITTEN (DEPTH_DRAW_ALWAYS) and drawn before the translucent things
	#     that write depth (render_priority -1). The liquid shader is
	#     depth_draw_always, so a ghost that wrote no depth was blended UNDER any
	#     water or lava between you and it, however far away that liquid was — the
	#     "liquids draw on top of the overlay" bug. Writing depth is what lets the
	#     liquid's own depth TEST hide it behind the ghost. Terrain still hides the
	#     ghost itself, because the depth test here stays on.
	material.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_ALWAYS
	material.render_priority = -1

	var cube := BoxMesh.new()
	cube.size = Vector3.ONE
	var multi := MultiMesh.new()
	multi.transform_format = MultiMesh.TRANSFORM_3D
	multi.mesh = cube

	_ghost = MultiMeshInstance3D.new()
	_ghost.name = "Cells"
	_ghost.multimesh = multi
	_ghost.material_override = material
	_ghost.visible = false
	_ghost_root.add_child(_ghost)

	var outline_material := StandardMaterial3D.new()
	outline_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	outline_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	outline_material.albedo_color = OUTLINE_COLOR
	outline_material.disable_receive_shadows = true
	# The volume's extent has to be readable even when the build is inside a hill:
	# that is the question the outline answers, so it draws through terrain.
	outline_material.no_depth_test = true
	_outline = MeshInstance3D.new()
	_outline.name = "Volume"
	_outline.material_override = outline_material
	_outline.visible = false
	_ghost_root.add_child(_outline)

func _build_hint() -> void:
	_hint = Label.new()
	_hint.name = "WandHint"
	_hint.add_theme_font_override("font", FONT)
	_hint.add_theme_font_size_override("font_size", int(UNIT_FONT * _ui_scale()))
	_hint.add_theme_color_override("font_color", HINT_FONT_COLOR)
	_hint.add_theme_color_override("font_shadow_color", TEXT_SHADOW)
	_hint.add_theme_constant_override("shadow_offset_x", 1)
	_hint.add_theme_constant_override("shadow_offset_y", 1)
	_hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_hint.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	_hint.offset_left = -320.0 * _ui_scale()
	_hint.offset_right = 320.0 * _ui_scale()
	_hint.offset_top = -64.0 * _ui_scale()
	_hint.offset_bottom = -48.0 * _ui_scale()
	_hint.visible = false
	add_child(_hint)

func has_preview() -> bool:
	return _preview_active

func preview_info() -> Dictionary:
	return _preview_info

func preview_origin() -> Vector3i:
	return _preview_origin

## Works the tool at the crosshair: the ghost goes where the crosshair points, on
## the face it points at, exactly like placing a block.
func anchor_preview_from_aim() -> Dictionary:
	var aim: Dictionary = _chunk_manager.raycast_from_camera(8.0)
	if not aim.get("success", false):
		return {"ok": false, "error": "nothing in range to place against"}
	var spot: Vector3 = aim.get("place_position", Vector3.ZERO)
	return anchor_preview_at(Vector3i(spot.floor()))

## Plans the paste at `origin` without writing and shows the ghost. The origin is
## the build's own (0, 0, 0) corner, so where you aim is where its corner lands.
func anchor_preview_at(origin: Vector3i) -> Dictionary:
	if _file_bytes.is_empty() or _file_name.is_empty():
		_hint_flash("No build chosen — middle click for the wand menu")
		return {"ok": false, "error": "no build file chosen"}
	var info: Dictionary = _chunk_manager.preview_schematic(
		_file_bytes, origin.x, origin.y, origin.z, _options())
	if not info.get("ok", false):
		_hint_flash("Cannot place this build: %s" % info.get("error", "?"))
		return info
	_preview_info = info
	_preview_origin = origin
	_preview_active = true
	_draw_ghost(info, origin)
	_update_hint()
	return info

func cancel_preview() -> void:
	_preview_active = false
	_preview_info = {}
	if _ghost != null:
		_ghost.visible = false
	if _outline != null:
		_outline.visible = false
	_update_hint()

func _draw_ghost(info: Dictionary, origin: Vector3i) -> void:
	if _ghost == null or _outline == null:
		return
	# The instance transforms arrive ready to upload: one unit transform per cell,
	# twelve floats each, built engine-side. A GDScript loop over the same cells was a
	# `set_instance_transform` call per cell on the main thread, which is a hitch at
	# every re-aim on a big build.
	var transforms: PackedFloat32Array = info.get("transforms", PackedFloat32Array())
	var count := int(floor(transforms.size() / 12.0))
	var multi: MultiMesh = _ghost.multimesh
	multi.instance_count = count
	if count > 0:
		multi.buffer = transforms
	_ghost.visible = count > 0

	# The volume's outline, one cube bigger than the cells' own bounds so it reads
	# as a box the build sits inside.
	var lo: Vector3i = info.get("min", origin)
	var hi: Vector3i = info.get("max", origin)
	var mesh := ImmediateMesh.new()
	var material := _outline.material_override
	mesh.surface_begin(Mesh.PRIMITIVE_LINES, material)
	var a := Vector3(float(lo.x), float(lo.y), float(lo.z))
	var b := Vector3(float(hi.x) + 1.0, float(hi.y) + 1.0, float(hi.z) + 1.0)
	for edge in [
			[a, Vector3(b.x, a.y, a.z)], [a, Vector3(a.x, b.y, a.z)], [a, Vector3(a.x, a.y, b.z)],
			[Vector3(b.x, a.y, a.z), Vector3(b.x, b.y, a.z)], [Vector3(b.x, a.y, a.z), Vector3(b.x, a.y, b.z)],
			[Vector3(a.x, b.y, a.z), Vector3(b.x, b.y, a.z)], [Vector3(a.x, b.y, a.z), Vector3(a.x, b.y, b.z)],
			[Vector3(a.x, a.y, b.z), Vector3(b.x, a.y, b.z)], [Vector3(a.x, a.y, b.z), Vector3(a.x, b.y, b.z)],
			[Vector3(b.x, b.y, a.z), b], [Vector3(b.x, a.y, b.z), b], [Vector3(a.x, b.y, b.z), b]]:
		mesh.surface_add_vertex(edge[0])
		mesh.surface_add_vertex(edge[1])
	mesh.surface_end()
	_outline.mesh = mesh
	_outline.visible = true

## Places what the ghost is showing. The options are the preview's own, so what
## lands is what was on screen.
func confirm_paste() -> Dictionary:
	if not _preview_active:
		_hint_flash("Right click to aim the build first")
		return {"ok": false, "error": "nothing to place"}
	var origin := _preview_origin
	var result: Dictionary = _chunk_manager.paste_schematic(
		_file_bytes, origin.x, origin.y, origin.z, _paste_options())
	cancel_preview()
	if not result.get("ok", false):
		_hint_flash("Paste failed: %s" % result.get("error", "?"))
		return result
	var summary := "Placed %s: %d cells" % [_file_name, int(result.get("cells", 0))]
	if int(result.get("stilled", 0)) > 0:
		summary += " (%d still liquid)" % int(result.get("stilled", 0))
	if int(result.get("pending_cells", 0)) > 0:
		summary += " — %d cells waiting for chunks" % int(result.get("pending_cells", 0))
	_hint_flash(summary)
	_report_to_chat(summary)
	return result

func _report_to_chat(text: String) -> void:
	# Chat is where every other engine answer is written down, and a paste that
	# spanned chunks finishes there minutes later; the hint line is for the
	# gesture, the log is for the record.
	var chat := get_node_or_null("/root/Main/HUD/Chat")
	if chat != null and chat.has_method("_add_message"):
		chat._add_message(text, Color(0.6, 1.0, 0.6))

# ---------------------------------------------------------------------------
# The HUD line
# ---------------------------------------------------------------------------

var _hint_flash_text := ""
var _hint_flash_until := 0.0

func _hint_flash(text: String) -> void:
	_hint_flash_text = text
	_hint_flash_until = Time.get_ticks_msec() / 1000.0 + 3.0

func preview_summary() -> String:
	if not _preview_active:
		return ""
	var planned := int(_preview_info.get("planned", 0))
	var returned := int(_preview_info.get("cells_returned", 0))
	var text := "%s: %d cells" % [_file_name, planned]
	if bool(_preview_info.get("cells_sampled", false)):
		text += " (showing %d)" % returned
	var skipped := int(_preview_info.get("skipped", 0))
	var unknown := int(_preview_info.get("unknown", 0))
	var substituted := int(_preview_info.get("substituted", 0))
	if substituted > 0:
		text += ", %d stand-ins" % substituted
	if skipped + unknown > 0:
		text += ", %d cells have no counterpart" % (skipped + unknown)
	return text

func _update_hint() -> void:
	if _hint == null:
		return
	var now := Time.get_ticks_msec() / 1000.0
	if now < _hint_flash_until and not _hint_flash_text.is_empty():
		_hint.text = _hint_flash_text
		_hint.visible = true
		return
	_hint_flash_text = ""
	if _preview_active:
		_hint.text = "%s   ·   RIGHT CLICK to move   ·   LEFT CLICK to place   ·   ESC to cancel" % preview_summary()
		_hint.visible = true
		return
	_hint.visible = false
