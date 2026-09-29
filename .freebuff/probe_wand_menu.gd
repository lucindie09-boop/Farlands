extends SceneTree
## Presses the wand menu's buttons for real: the grid cells, a file tile, CLOSE and
## BACK. The layout probe checks where the widgets are; this checks that pressing
## them still does what it did, which is the half a rewrite of the menu can break
## silently (a cell that draws but is not wired to anything looks fine).
##
##   Godot --headless --path <project> --script res://.freebuff/probe_wand_menu.gd
##   PROBE_W=1280 PROBE_H=720 .freebuff/run_probe_shot.sh .freebuff/probe_wand_menu.gd
##
## The sizes are only asserted in a real window: --headless gets a 115x256 dummy
## viewport, where the menu is deliberately tiny, and its numbers mean nothing.
## The click path is checked in both, because it does not depend on the size.

var wand: Node = null
var player: Node = null
var failures := 0


func _initialize() -> void:
	_run()


func _run() -> void:
	if OS.get_environment("PROBE_W").is_valid_int() and OS.get_environment("PROBE_H").is_valid_int():
		DisplayServer.window_set_size(Vector2i(int(OS.get_environment("PROBE_W")),
			int(OS.get_environment("PROBE_H"))))
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	wand = main.get_node_or_null("HUD/Wand")
	player = main.get_node_or_null("Player")
	if wand == null or player == null:
		print("PROBE FAIL: no Wand or Player node")
		quit(1)
		return

	for i in range(40):
		await process_frame

	# Hold the wand. wand.gd closes its menu the moment the player is holding
	# something else (the tool's ghost goes with the tool), so a probe that opens
	# the menu while holding a pickaxe would be testing nothing - and the saved
	# inventory lands a frame or two after the scene builds, so put it in slot 0
	# until it sticks rather than once.
	var wand_id: int = BlockTextures.get_block_id_by_name("wand")
	if wand_id <= 0:
		_fail("the wand item did not resolve (data/items.json)")
		_report_and_quit()
		return
	for attempt in 30:
		player.call("set_hotbar_slot", 0, wand_id, 1)
		player.call("select_hotbar_slot", 0)
		await process_frame
		if int(player.call("get_selected_block")) == wand_id:
			break
	if not player.call("is_wand_held"):
		_fail("the wand is not the held item, so the menu will not stay open")
		_report_and_quit()
		return

	# --- open, and the function grid ----------------------------------------
	wand.call("_on_menu_pressed")
	await _frames(4)
	if not wand.call("is_menu_open"):
		_fail("the menu did not open")
		_report_and_quit()
		return
	if not player.call("is_wand_menu_open"):
		_fail("the open menu did not make the game modal")

	var cells := _buttons(_panel(), true)
	print("probe: function page buttons: %s" % [_describe(cells)])
	var cell: Button = _button_titled(cells, "PASTE BUILD")
	if cell == null:
		_fail("no cell titled PASTE BUILD")
		_report_and_quit()
		return
	var cell_rect := cell.get_global_rect()
	var view := root.get_visible_rect().size
	var windowed := view.x >= 640.0 and view.y >= 480.0
	print("probe: the paste cell is %.0fx%.0f in a %s viewport (windowed=%s)"
		% [cell_rect.size.x, cell_rect.size.y, view, windowed])
	if windowed and (cell_rect.size.y < 100.0 or cell_rect.size.x < 120.0):
		_fail("the cell is only %.0fx%.0f" % [cell_rect.size.x, cell_rect.size.y])

	# A cell is the whole click path: it selects the function AND turns to the
	# file page when the function needs a file.
	if _button_titled(cells, "BUILDING TOOLS") == null or _button_titled(cells, "SCHEMATICS") == null:
		_fail("the two tabs are not both on the page")

	cell.emit_signal("pressed")
	await _frames(4)
	if wand.get("_function_id") != "paste":
		_fail("pressing the cell did not select paste")
	if wand.get("_menu_page") != "schematics":
		_fail("pressing a build-needing tool did not turn to the schematics tab")

	# --- the file grid ------------------------------------------------------
	var tiles := _buttons(_panel(), true)
	print("probe: file page buttons: %s" % [_describe(tiles)])
	if tiles.size() < 2:
		_fail("the file page has %d buttons" % tiles.size())
	var church: Button = _button_titled(tiles, "church")
	if church == null:
		_fail("no file tile for church")
		_report_and_quit()
		return
	church.emit_signal("pressed")
	await _frames(30)
	if wand.call("get_file") != "church.schematic":
		_fail("pressing the church tile chose '%s'" % wand.call("get_file"))
	elif wand.get("_menu_page") != "tools":
		_fail("choosing a build did not return to the tools tab")

	# --- the tabs, then CLOSE -----------------------------------------------
	wand.set("_menu_page", "schematics")
	wand.call("_refresh_menu")
	await _frames(4)
	var tabs := _buttons(_panel(), true)
	var to_tools: Button = _button_titled(tabs, "BUILDING TOOLS")
	if to_tools == null:
		_fail("the schematics tab has no BUILDING TOOLS tab beside it")
	else:
		to_tools.emit_signal("pressed")
		await _frames(4)
		if wand.get("_menu_page") != "tools":
			_fail("the BUILDING TOOLS tab did not switch back")

	# CLOSE is an icon now, so it is found by name rather than by the word it no
	# longer carries.
	var close: Button = _panel().find_child("MenuClose", true, false) as Button
	if close == null:
		_fail("the tools page has no close button")
	else:
		close.emit_signal("pressed")
		await _frames(4)
		if wand.call("is_menu_open"):
			_fail("CLOSE did not close the menu")
		elif player.call("is_wand_menu_open"):
			_fail("closing did not hand the game back")

	_report_and_quit()


func _frames(n: int) -> void:
	for i in range(n):
		await process_frame


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: " + message)


func _report_and_quit() -> void:
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)


func _panel() -> Control:
	var menu: Control = wand.get_node_or_null("WandMenu")
	if menu == null:
		return null
	return _find_panel(menu)


func _find_panel(node: Node) -> Control:
	for child in node.get_children():
		if child is PanelContainer:
			return child
		var found := _find_panel(child)
		if found != null:
			return found
	return null


func _buttons(node: Node, recursive: bool) -> Array:
	var out := []
	if node == null:
		return out
	for child in node.get_children():
		if child is Button:
			out.append(child)
		elif recursive:
			out.append_array(_buttons(child, recursive))
	return out


func _button_titled(buttons: Array, title: String) -> Button:
	for button in buttons:
		if button.text == title:
			return button
	return null


func _describe(buttons: Array) -> String:
	var parts := []
	for button in buttons:
		var r: Rect2 = button.get_global_rect()
		parts.append("'%s' %.0fx%.0f@(%.0f,%.0f)" % [button.text, r.size.x, r.size.y, r.position.x, r.position.y])
	return ", ".join(parts)
