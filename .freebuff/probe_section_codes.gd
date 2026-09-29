extends SceneTree
## Round trip for the settings page's per-category export/import codes.
##
## Every category's heading icons must produce a code carrying that section's own
## prefix, consume it back into the settings, and resync the rows so an imported
## code is visible immediately. A code from another section must be refused.
##
## Runs WINDOWED (through .freebuff/run_probe_shot.sh) because the real OS
## clipboard is part of what is being tested; the clipboard is put back at the
## end, and the runner restores user:// so a run cannot leave settings changed.

## The settings page's categories, in page order, with their code prefixes.
const EXPECTED := [
	["General", "FG"],
	["Block Outline", "FO"],
	["Crosshair", "FC"],
	["Advanced Rendering", "FAR"],
	["Render", "FR"],
]

var menu: Control = null
var cm: Node = null
var ui: Node = null
var failures := 0


func _initialize() -> void:
	_run()


func _fail(message: String) -> void:
	failures += 1
	print("PROBE FAIL: %s" % message)


func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		print("PROBE FAIL: main.tscn missing")
		quit(1)
		return
	var main: Node3D = scene.instantiate()
	root.add_child(main)
	menu = main.get_node_or_null("HUD/SettingsMenu")
	cm = main.get_node_or_null("ChunkManager")
	ui = root.get_node_or_null("UIScale")
	if menu == null or cm == null or ui == null:
		print("PROBE FAIL: SettingsMenu / ChunkManager / UIScale missing")
		quit(1)
		return
	for i in range(60):
		await process_frame
	menu.call("_open")
	for i in range(4):
		await process_frame

	var saved_clipboard := DisplayServer.clipboard_get()
	var page: Control = menu.get("_pages")["settings"]
	var icons := _heading_icons(page)
	print("probe: %d heading icons for %d categories" % [icons.size(), EXPECTED.size()])
	if icons.size() != EXPECTED.size() * 3:
		_fail("setup: %d heading icons, want %d (export, import, reset per category)"
			% [icons.size(), EXPECTED.size() * 3])

	for i in range(EXPECTED.size()):
		if i * 3 + 1 >= icons.size():
			break
		var name: String = EXPECTED[i][0]
		var prefix: String = EXPECTED[i][1]
		var export_btn: Button = icons[i * 3]
		var import_btn: Button = icons[i * 3 + 1]

		# What this section owns, before anything touches it.
		var before := _values(name)

		# 1. Export: the clipboard must now hold this section's code. A sentinel
		# goes in first, so an export that silently did nothing reads back as the
		# sentinel instead of as some earlier section's code.
		_clear_messages(page)
		await _clip_set("SENTINEL")
		export_btn.pressed.emit()
		var code := await _clip_get()
		if not code.begins_with(prefix + "-"):
			_fail("%s: exported \"%s\", want a %s- code" % [name, code, prefix])
			continue
		print("probe: %-18s %2d chars  %s" % [name, code.length(), code])
		if not _find_text(page, "Code copied to clipboard!"):
			_fail("%s: export did not report into its hint line" % name)

		# 2. Change something this section owns, then import the code back.
		_disturb(name, before)
		_clear_messages(page)
		await _press_import(import_btn, page)
		if await _clip_get() != code:
			_fail("%s: import changed the clipboard" % name)
		for key in before:
			var want = before[key]
			var got = _values(name)[key]
			if not _same(want, got):
				_fail("%s: %s came back as %s, want %s" % [name, key, got, want])
		if not _find_text(page, "Code imported successfully!"):
			_fail("%s: import did not report into its hint line" % name)
		if not _rows_show(name):
			_fail("%s: rows did not resync after import" % name)

		# 3. Another section's code must be refused.
		_clear_messages(page)
		await _clip_set("ZZ-AAAA-AAAA")
		await _press_import(import_btn, page)
		if not _find_text(page, "Invalid code format"):
			_fail("%s: accepted a foreign code" % name)

	await _clip_set(saved_clipboard)
	print("probe: %d failures" % failures)
	quit(1 if failures > 0 else 0)


## Windows refuses to open the clipboard while another process holds it, which a
## machine with a clipboard manager (or the game itself) does constantly. That is
## an OS race, not the thing under test, so both directions retry for a frame.
func _clip_set(text: String) -> void:
	for attempt in range(8):
		DisplayServer.clipboard_set(text)
		await process_frame
		if DisplayServer.clipboard_get() == text:
			return
	_fail("clipboard would not take \"%s\" (another process holding it?)" % text.substr(0, 16))

func _clip_get() -> String:
	var text := ""
	for attempt in range(8):
		text = DisplayServer.clipboard_get()
		if text != "":
			return text
		await process_frame
	return text

## The section's import one more time when the clipboard read lost the race: the
## code is still on the clipboard, so pressing again is exactly what a user does.
func _press_import(button: Button, page: Control) -> void:
	for attempt in range(8):
		button.pressed.emit()
		await process_frame
		if not _find_text(page, "Clipboard is empty"):
			return

## Feedback lines linger for three seconds, so a category's check must not read
## the previous category's message as its own.
const MESSAGES := ["Code copied to clipboard!", "Code imported successfully!",
	"Invalid code format", "Clipboard is empty", "Export failed"]

func _clear_messages(node: Node) -> void:
	for child in node.get_children():
		if child is Label and MESSAGES.has(String(child.text)):
			child.text = ""
		_clear_messages(child)

## The values each category owns, read back from where they live.
func _values(name: String) -> Dictionary:
	match name:
		"General":
			return {
				"gui_scale": float(ui.get("value")),
				"render_distance": cm.get_render_distance(),
				"fps_cap": int(menu.get("_fps_cap")),
			}
		"Advanced Rendering":
			return {
				"day_duration": cm.get_day_duration(),
				"day_sky": cm.get_day_sky_color(),
				"night_sky": cm.get_night_sky_color(),
				"ao_color": cm.get_ao_color(),
				"ao_strength": cm.get_ao_strength(),
				"darkness": cm.get_darkness_color(),
				"contrast": cm.get_contrast(),
				"saturation": cm.get_saturation(),
				"smooth_lighting": cm.get_smooth_lighting(),
			}
		"Render":
			return {
				"lod_distance": cm.get_lod_distance(),
				"lod_detail": cm.get_lod_detail_level(),
				"far_lod_distance": cm.get_far_lod_distance(),
				"far_lod_detail": cm.get_far_lod_detail_level(),
				"fog_mode": cm.get_fog_mode(),
				"mipmaps": cm.get_mipmaps_enabled(),
				"mipmap_bias": cm.get_mipmap_bias(),
				"textures": cm.get_textures_enabled(),
				"compression": cm.get_compression_enabled(),
			}
		"Crosshair":
			var cross = menu.get("crosshair_node")
			return {
				"cross_length": cross.get("cross_length"),
				"cross_opacity": cross.get("cross_opacity"),
				"cross_color": cross.get("cross_color"),
				"dot_size": cross.get("dot_size"),
				"cross_contrast": cross.get("cross_contrast"),
			}
		"Block Outline":
			var outline = menu.get("block_outline_node")
			return {
				"outline_thickness": outline.get("outline_thickness"),
				"outline_opacity": outline.get("outline_opacity"),
				"outline_color": outline.get("outline_color"),
				"fill_enabled": outline.get("fill_enabled"),
			}
	return {}


## Move every value this section owns away from where it was, so the import back
## has something to prove.
func _disturb(name: String, before: Dictionary) -> void:
	match name:
		"General":
			ui.set("value", 1.0 if absf(float(before["gui_scale"]) - 1.0) > 0.001 else 2.0)
			cm.set_render_distance(2 if int(before["render_distance"]) != 2 else 5)
			menu.set("_fps_cap", 5 if int(before["fps_cap"]) != 5 else 7)
		"Advanced Rendering":
			cm.set_day_duration(20.0 if absf(float(before["day_duration"]) - 20.0) > 0.001 else 30.0)
			cm.set_day_sky_color(Color.RED)
			cm.set_night_sky_color(Color.GREEN)
			cm.set_ao_color(Color.BLUE)
			cm.set_ao_strength(0.0 if absf(float(before["ao_strength"])) > 0.001 else 1.5)
			cm.set_darkness_color(Color.MAGENTA)
			cm.set_contrast(0.0 if absf(float(before["contrast"])) > 0.001 else 1.5)
			cm.set_saturation(0.0 if absf(float(before["saturation"])) > 0.001 else 1.5)
			cm.set_smooth_lighting(not bool(before["smooth_lighting"]))
		"Render":
			cm.set_lod_distance(0 if int(before["lod_distance"]) != 0 else 4)
			cm.set_lod_detail_level(0.5 if absf(float(before["lod_detail"]) - 0.5) > 0.001 else 0.75)
			cm.set_far_lod_distance(0 if int(before["far_lod_distance"]) != 0 else 4)
			cm.set_far_lod_detail_level(0.25 if absf(float(before["far_lod_detail"]) - 0.25) > 0.001 else 0.75)
			cm.set_fog_mode(0 if int(before["fog_mode"]) != 0 else 2)
			cm.set_mipmaps_enabled(not bool(before["mipmaps"]))
			cm.set_mipmap_bias(-3.0 if absf(float(before["mipmap_bias"]) + 3.0) > 0.001 else 3.0)
			cm.set_textures_enabled(not bool(before["textures"]))
			cm.set_compression_enabled(not bool(before["compression"]))
		"Crosshair":
			var cross = menu.get("crosshair_node")
			cross.set("cross_length", 1.0 if absf(float(before["cross_length"]) - 1.0) > 0.001 else 9.0)
			cross.set("cross_opacity", 0.2 if absf(float(before["cross_opacity"]) - 0.2) > 0.001 else 0.8)
			cross.set("cross_color", Color.RED)
			cross.set("dot_size", 1.0 if absf(float(before["dot_size"]) - 1.0) > 0.001 else 9.0)
			cross.set("cross_contrast", not bool(before["cross_contrast"]))
		"Block Outline":
			var outline = menu.get("block_outline_node")
			outline.set("outline_thickness", 0.5 if absf(float(before["outline_thickness"]) - 0.5) > 0.001 else 0.1)
			outline.set("outline_opacity", 0.2 if absf(float(before["outline_opacity"]) - 0.2) > 0.001 else 0.8)
			outline.set("outline_color", Color.RED)
			outline.set("fill_enabled", not bool(before["fill_enabled"]))


## The row the eye reads has to agree with the value that came back: a section
## whose import silently left its widgets stale is the bug this catches.
func _rows_show(name: String) -> bool:
	var page: Control = menu.get("_pages")["settings"]
	match name:
		"General":
			return _find_text(page, "GUI Scale: %dx" % int(round(float(ui.get("value")))))
		"Advanced Rendering":
			return _find_text(page, "Smooth Lighting: %s"
				% ("On" if cm.get_smooth_lighting() else "Off"))
		"Render":
			return _find_text(page, "Fog Mode: %s"
				% ["Off", "Edge", "Linear", "Exponential"][cm.get_fog_mode()])
		"Crosshair":
			return _find_prefix(page, "Length: ")
		"Block Outline":
			return _find_prefix(page, "Thickness: ")
	return false


## Packed values round-trip through a scale, so a fraction lands near itself
## rather than exactly on it.
func _same(a, b) -> bool:
	if a is float or b is float:
		if a is Color or b is Color:
			var ca: Color = a
			var cb: Color = b
			return ca.is_equal_approx(cb)
		return absf(float(a) - float(b)) < 0.02
	return a == b


func _heading_icons(node: Node, out := []) -> Array:
	for child in node.get_children():
		if child is Button and absf(float(child.get_meta("icon_w", 0.0)) - 10.0) < 0.01:
			out.append(child)
		_heading_icons(child, out)
	return out


func _find_text(node: Node, text: String) -> bool:
	for child in node.get_children():
		if child is Label and String(child.text) == text:
			return true
		if child is Button and String(child.text) == text:
			return true
		if _find_text(child, text):
			return true
	return false


func _find_prefix(node: Node, prefix: String) -> bool:
	for child in node.get_children():
		if child is Label and String(child.text).begins_with(prefix):
			return true
		if _find_prefix(child, prefix):
			return true
	return false
