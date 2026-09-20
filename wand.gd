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
const SchematicFiles := preload("res://schematic_files.gd")

# The interface grid, the same units the settings menu draws in, so the wand's
# menu is the same interface rather than a second one.
const UNIT_FONT := 8.0
const UNIT_BUTTON_W := 200.0
const UNIT_BUTTON_H := 20.0
const UNIT_GAP := 4.0
const UNIT_MARGIN := 8.0
const UNIT_TITLE_W := 320.0
const UNIT_FILE_W := 150.0

# How many cells the ghost draws. Past this the sample is strided: a preview is
# for reading the shape and the size, and one buffer that a frame can upload
# beats a perfect ghost that stalls the game.
const PREVIEW_CELLS := 20000

const GHOST_COLOR := Color(0.55, 0.86, 1.0, 0.26)
const OUTLINE_COLOR := Color(0.75, 1.0, 0.95, 0.55)
const HINT_FONT_COLOR := Color.WHITE
const TEXT_SHADOW := Color(0.0, 0.0, 0.0, 0.7)

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
var _menu_title: Label = null
var _menu_body: VBoxContainer = null
var _menu_page := "functions"  # or "files"
var _menu_open := false

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
	_menu_page = "functions"
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

func _function_entry(id: String) -> Dictionary:
	for entry in FUNCTIONS:
		if entry["id"] == id:
			return entry
	return {}

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
	if _menu == null:
		return
	for child in _menu_body.get_children():
		# Hidden now, freed at the end of the frame: this runs from inside a
		# button's own `pressed` signal, and Godot refuses to free a node while
		# it is emitting. Hiding first is what keeps one frame from showing the
		# old page and the new one at once.
		child.visible = false
		child.queue_free()
	if _menu_page == "files":
		_menu_title.text = "CHOOSE A BUILD"
		_build_file_page()
	else:
		_menu_title.text = "WAND"
		_build_function_page()

func _build_function_page() -> void:
	var s := _ui_scale()
	for entry in FUNCTIONS:
		var btn := _make_button(String(entry["name"]), UNIT_BUTTON_W)
		# The chosen function reads as chosen: it is the one the clicks use.
		if entry["id"] == _function_id:
			btn.modulate = Color(0.7, 1.0, 0.8)
		btn.pressed.connect(func() -> void:
			set_function(String(entry["id"]))
			if bool(entry.get("needs_file", false)):
				_menu_page = "files"
				_refresh_menu()
		)
		_menu_body.add_child(btn)

	var hint := Label.new()
	hint.add_theme_font_override("font", FONT)
	hint.add_theme_font_size_override("font_size", int(UNIT_FONT * s))
	hint.add_theme_color_override("font_color", Color(1, 1, 1, 0.8))
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.custom_minimum_size = Vector2(UNIT_TITLE_W * s, 0)
	var entry := _function_entry(_function_id)
	hint.text = String(entry.get("hint", ""))
	_menu_body.add_child(hint)

	var current := Label.new()
	current.add_theme_font_override("font", FONT)
	current.add_theme_font_size_override("font_size", int(UNIT_FONT * s))
	current.add_theme_color_override("font_color", Color(0.75, 1.0, 0.95))
	if _file_name.is_empty():
		current.text = "build: (none chosen)"
	else:
		current.text = "build: %s · %d cells" % [_file_name, int(_file_info.get("planned", 0))]
	_menu_body.add_child(current)

	var pick := _make_button("CHOOSE BUILD", UNIT_BUTTON_W)
	pick.pressed.connect(func() -> void:
		_menu_page = "files"
		_refresh_menu()
	)
	_menu_body.add_child(pick)

	var close := _make_button("CLOSE", UNIT_BUTTON_W)
	close.pressed.connect(close_menu)
	_menu_body.add_child(close)

func _build_file_page() -> void:
	var s := _ui_scale()
	var names := SchematicFiles.list()
	if names.is_empty():
		var none := Label.new()
		none.add_theme_font_override("font", FONT)
		none.add_theme_font_size_override("font_size", int(UNIT_FONT * s))
		none.add_theme_color_override("font_color", Color(1, 0.8, 0.8))
		none.text = "No build files. Put them in schematics/."
		_menu_body.add_child(none)
	else:
		# A grid of names, the same shape the skin and block galleries use: the
		# files ARE the menu, so they are the buttons.
		var grid := GridContainer.new()
		grid.columns = 2
		grid.add_theme_constant_override("h_separation", int(8 * s))
		grid.add_theme_constant_override("v_separation", int(UNIT_GAP * s))
		for entry_name in names:
			var btn := _make_button(SchematicFiles.display_name(entry_name), UNIT_FILE_W)
			if entry_name == _file_name:
				btn.modulate = Color(0.7, 1.0, 0.8)
			# No per-file size here on purpose: reading one is a decode, and the
			# big ones are hundreds of milliseconds. The size appears on the
			# function page once a file is chosen, where it is read once.
			btn.pressed.connect(func() -> void:
				if not choose_file(entry_name):
					_hint_flash("Could not read %s" % entry_name)
					return
				_menu_page = "functions"
				_refresh_menu()
			)
			grid.add_child(btn)
		_menu_body.add_child(grid)

	var back := _make_button("BACK", UNIT_BUTTON_W)
	back.pressed.connect(func() -> void:
		_menu_page = "functions"
		_refresh_menu()
	)
	_menu_body.add_child(back)

func _build_menu() -> void:
	var s := _ui_scale()
	var root := Control.new()
	root.name = "WandMenu"
	root.set_anchors_preset(Control.PRESET_FULL_RECT)
	root.mouse_filter = Control.MOUSE_FILTER_STOP
	root.visible = false
	add_child(root)

	var backdrop := ColorRect.new()
	backdrop.set_anchors_preset(Control.PRESET_FULL_RECT)
	backdrop.color = Color(0, 0, 0, 0.45)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	root.add_child(backdrop)

	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.custom_minimum_size = Vector2((UNIT_TITLE_W + 2 * UNIT_MARGIN) * s, 0)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.08, 0.08, 0.10, 0.92)
	style.border_color = Color(1, 1, 1, 0.35)
	style.set_border_width_all(int(2 * s))
	style.set_content_margin_all(int(UNIT_MARGIN * s))
	panel.add_theme_stylebox_override("panel", style)
	root.add_child(panel)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", int(UNIT_GAP * s))
	panel.add_child(column)

	_menu_title = Label.new()
	_menu_title.add_theme_font_override("font", FONT)
	_menu_title.add_theme_font_size_override("font_size", int(UNIT_FONT * s * 1.5))
	_menu_title.add_theme_color_override("font_color", Color.WHITE)
	_menu_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	column.add_child(_menu_title)

	_menu_body = VBoxContainer.new()
	_menu_body.add_theme_constant_override("separation", int(UNIT_GAP * s))
	column.add_child(_menu_body)

	_menu = root

func _make_button(text: String, width: float) -> Button:
	var s := _ui_scale()
	var btn := Button.new()
	btn.text = text
	btn.clip_text = true
	btn.add_theme_font_override("font", FONT)
	btn.add_theme_font_size_override("font_size", int(UNIT_FONT * s))
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
	btn.custom_minimum_size = Vector2(width, UNIT_BUTTON_H) * s
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
	var packed: PackedByteArray = info.get("cells", PackedByteArray())
	var values := packed.to_int32_array()
	# Four int32 per cell (x, y, z, block id). Written as a float divide because
	# GDScript's integer division is a warning for a reason: a silent truncation
	# here would drop the last cell of every ghost.
	var count := int(floor(values.size() / 4.0))
	var multi: MultiMesh = _ghost.multimesh
	multi.instance_count = count
	for i in count:
		var at := Vector3(
			float(values[i * 4]) + 0.5,
			float(values[i * 4 + 1]) + 0.5,
			float(values[i * 4 + 2]) + 0.5)
		multi.set_instance_transform(i, Transform3D(Basis(), at))
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
