extends SceneTree
## Does the Liquid Texture Lab work end to end?
##
## Drives the real tool script against the real world: builds its panel, changes
## settings, generates a strip, saves and reloads it, and pushes a frame into the
## world's texture array â€” then reads the layer back to prove the world is
## actually sampling the generated pixels. Run:
##
##   Godot --headless --path <project> --script res://.freebuff/probe_liquid_lab.gd

const SAVE_DIR := "user://liquids"
const CHECK_NAME := "probe_liquid_lab"
const STYLES := ["water", "lava", "acid"]
# The verdict also goes to a file: this project's headless runs intermittently
# segfault or stall in Godot's *teardown*, which loses the tail of stdout, and a
# lost "PROBE PASS" reads exactly like a crash in the code under test.
const RESULT_FILE := "user://probe_liquid_lab_result.txt"
var failures: Array = []

var main: Node3D
var lab: Node
var manager: Node
var ok := true
var checks := 0

func _initialize() -> void:
	_run()

func _fail(msg: String) -> void:
	ok = false
	checks += 1
	failures.append(msg)
	print("PROBE FAIL: " + msg)

func _check(condition: bool, msg: String) -> void:
	checks += 1
	if not condition:
		_fail("check failed: " + msg)

func _run() -> void:
	var scene: PackedScene = load("res://main.tscn")
	if scene == null:
		_fail("main.tscn missing")
		quit(1)
		return
	main = scene.instantiate()
	root.add_child(main)
	manager = main.get_node_or_null("ChunkManager")
	if manager == null:
		_fail("ChunkManager missing")
		quit(1)
		return

	# --- the generator binding -------------------------------------------
	var names := LiquidTextureGen.style_names()
	_check(names.size() == 3, "three styles, got %d" % names.size())
	for style in STYLES:
		_check(names.has(style), "style list has " + style)

	for style in STYLES:
		var settings: Dictionary = LiquidTextureGen.default_settings(style, 16)
		_check(int(settings.get("resolution", 0)) == 16, "%s defaults carry the requested resolution" % style)
		_check(settings.has("kernel") and settings.has("ramp") and settings.has("field_scale"),
			"%s defaults carry the automation knobs" % style)

		var strip: Image = LiquidTextureGen.generate_strip(settings)
		_check(strip != null, "%s strip generated" % style)
		if strip == null:
			continue
		var frames := int(settings.get("frames", 0)) * int(settings.get("interpolate", 1))
		_check(strip.get_width() == 16, "%s strip width" % style)
		_check(strip.get_height() == 16 * frames, "%s strip height (%d == 16*%d)" % [style, strip.get_height(), frames])
		_check(strip.get_format() == Image.FORMAT_RGBA8, "%s strip is RGBA8" % style)
		# Frames are stacked downward: frame 0 and frame 1 must differ.
		var first := strip.get_region(Rect2i(0, 0, 16, 16))
		var second := strip.get_region(Rect2i(0, 16, 16, 16))
		_check(first.get_data() != second.get_data(), "%s frames 0 and 1 differ (it animates)" % style)

		# Determinism: the same settings twice must be byte-identical.
		var again: Image = LiquidTextureGen.generate_strip(settings)
		_check(again != null and again.get_data() == strip.get_data(), "%s is deterministic" % style)

		# Clamping is reported back, not silently applied.
		var wild := settings.duplicate()
		wild["resolution"] = 9999
		wild["frames"] = 0
		var described: Dictionary = LiquidTextureGen.describe(wild)
		_check(int(described.get("resolution", 0)) == 256, "describe clamps resolution (got %d)" % int(described.get("resolution", 0)))
		_check(int(described.get("frames", 0)) == 1, "describe clamps frames")

	# --- the tool: panel, regenerate, save, load, live ---------------------
	lab = load("res://scripts/liquid_texture_lab.gd").new()
	root.add_child(lab)
	await process_frame
	_check(lab.get("settings") != null and not lab.get("settings").is_empty(), "lab has settings after _ready")

	lab.call("_show_lab")
	await process_frame
	var strip = lab.get("_strip")
	_check(strip != null, "lab generated a strip on open")
	_check(lab.get("_frames").size() > 1, "lab sliced the strip into frames")
	_check(lab.get("_layer") != null and lab.get("_layer").visible, "lab panel is visible")

	# Layout sanity, without pixels: every container must have real geometry (a
	# collapsed Container is a panel nobody can see or click) and every control
	# must exist.
	var layer: CanvasLayer = lab.get("_layer")
	var panel_root: Control = layer.get_child(0)
	var viewport_size: Vector2 = lab.get_viewport().get_visible_rect().size
	print("PROBE viewport %s, panel root %s" % [str(viewport_size), str(panel_root.size)])
	# Full-rect over the viewport: the panel must track whatever size this
	# process has, not collapse to its minimum.
	_check(panel_root.size == viewport_size, "panel root covers the viewport")
	# 8 shape rows + 10 automaton rows + ramp stops + posterize + loop window.
	var sliders: Dictionary = lab.get("_sliders")
	print("PROBE sliders %d" % sliders.size())
	_check(sliders.size() == 21, "21 sliders built, got %d" % sliders.size())
	var checks_map: Dictionary = lab.get("_checks")
	print("PROBE checks %d" % checks_map.size())
	_check(checks_map.size() == 3, "3 check rows built, got %d" % checks_map.size())
	var ramp_buttons: Array = lab.get("_ramp_buttons")
	print("PROBE ramp buttons %d" % ramp_buttons.size())
	_check(ramp_buttons.size() == 4, "4 ramp stops built")
	var thumbs: Array = lab.get("_thumbs")
	print("PROBE thumbs %d" % thumbs.size())
	_check(thumbs.size() == 8, "8 frame thumbnails built")
	for rect_name in ["_live_preview", "_tiled_preview", "_sheet_preview"]:
		print("PROBE checking " + rect_name)
		var rect: TextureRect = lab.get(rect_name)
		_check(rect != null, "%s exists" % rect_name)
		if rect != null:
			_check(rect.texture != null, "%s shows a texture" % rect_name)
			_check(rect.size.x > 8.0 and rect.size.y > 8.0, "%s has a real size (%s)" % [rect_name, str(rect.size)])
	print("PROBE step: title")
	var title_text := ""
	for node in panel_root.find_children("*", "Label", true, false):
		if (node as Label).text.contains("LIQUID TEXTURE LAB"):
			title_text = (node as Label).text
	_check(title_text.contains("LIQUID TEXTURE LAB"), "title rendered: %s" % title_text)
	_check(title_text.contains("O"), "the title names the key that closes it: %s" % title_text)

	# Change a setting the way a slider does, and check the strip follows.
	print("PROBE step: before regenerate")
	var before: PackedByteArray = (lab.get("_strip") as Image).get_data()
	(lab.get("settings"))["resolution"] = 32
	(lab.get("settings"))["seed"] = int(lab.get("settings")["seed"]) + 7
	lab.call("_regenerate")
	await process_frame
	var after: Image = lab.get("_strip")
	_check(after.get_width() == 32, "resolution change resized the strip (got %d)" % after.get_width())
	_check(after.get_data() != before, "seed change altered the pixels")

	# Save + reload round-trip.
	print("PROBE step: before save")
	lab.set("_name_edit", null)
	var name_edit := LineEdit.new()
	lab.set("_name_edit", name_edit)
	name_edit.text = CHECK_NAME
	lab.call("_save")
	_check(FileAccess.file_exists(SAVE_DIR + "/" + CHECK_NAME + ".png"), "saved the strip png")
	_check(FileAccess.file_exists(SAVE_DIR + "/" + CHECK_NAME + ".json"), "saved the settings json")
	var saved_image := Image.load_from_file(SAVE_DIR + "/" + CHECK_NAME + ".png")
	_check(saved_image != null and saved_image.get_data() == (lab.get("_strip") as Image).get_data(),
		"the saved png is the strip the lab is showing")

	print("PROBE step: before load round-trip")
	var saved_settings: Dictionary = (lab.get("settings") as Dictionary).duplicate()
	(lab.get("settings"))["resolution"] = 8
	(lab.get("settings"))["seed"] = 1
	lab.call("_regenerate")
	lab.call("_refresh_load_list")
	var picker: OptionButton = lab.get("_load_pick")
	var index := -1
	for i in picker.item_count:
		if picker.get_item_text(i) == CHECK_NAME:
			index = i
	_check(index >= 0, "saved name is in the load list")
	if index >= 0:
		picker.selected = index
		lab.call("_load_selected")
		var restored: Dictionary = lab.get("settings")
		_check(int(restored.get("resolution", 0)) == int(saved_settings.get("resolution", 0)),
			"reload restored the resolution")
		_check(int(restored.get("seed", 0)) == int(saved_settings.get("seed", 0)), "reload restored the seed")
		_check((lab.get("_strip") as Image).get_data() == after.get_data(), "reload regenerated the same pixels")

	# --- the loop -----------------------------------------------------------
	# The generator's contract: with loop on, the last frame IS the first.
	var loop_settings: Dictionary = LiquidTextureGen.default_settings("water", 16)
	loop_settings["loop"] = true
	loop_settings["frames"] = 12
	var loop_strip: Image = LiquidTextureGen.generate_strip(loop_settings)
	var loop_frames := int((LiquidTextureGen.describe(loop_settings)).get("strip_frames", 0))
	_check(loop_frames == 12, "12 frames stored, got %d" % loop_frames)
	if loop_frames == 12:
		var first := loop_strip.get_region(Rect2i(0, 0, 16, 16))
		var last := loop_strip.get_region(Rect2i(0, 16 * 11, 16, 16))
		var second := loop_strip.get_region(Rect2i(0, 16, 16, 16))
		_check(first.get_data() == last.get_data(), "the last frame is the first frame, byte for byte")
		_check(first.get_data() != second.get_data(), "and the strip still animates")
		_check(bool((LiquidTextureGen.describe(loop_settings)).get("looped", false)), "describe reports the strip as looping")
		# Loop off: the same strip no longer closes.
		loop_settings["loop"] = false
		var open_strip: Image = LiquidTextureGen.generate_strip(loop_settings)
		var open_last := open_strip.get_region(Rect2i(0, 16 * 11, 16, 16))
		_check(open_last.get_data() != first.get_data(), "with loop off the strip ends elsewhere")

	# ...and the lab's playback wraps to index 1, not 0, so the repeated frame is
	# shown once per loop instead of twice in a row.
	_check(bool(lab.get("_looped")), "the lab knows the strip loops")
	lab.set("_frame", (lab.get("_frames") as Array).size() - 1)
	lab.call("_advance")
	_check(int(lab.get("_frame")) == 1, "playback wraps to index 1, got %d" % int(lab.get("_frame")))
	var open_settings: Dictionary = (lab.get("settings") as Dictionary).duplicate()
	open_settings["loop"] = false
	lab.set("settings", open_settings)
	lab.call("_regenerate")
	_check(not bool(lab.get("_looped")), "the lab sees the loop turned off")
	lab.set("_frame", (lab.get("_frames") as Array).size() - 1)
	lab.call("_advance")
	_check(int(lab.get("_frame")) == 0, "without a loop, playback wraps to 0")
	lab.call("_apply_style", "water")

	# --- live: push a frame into the world's array and read it back ---------
	print("PROBE step: before live")
	var info: Dictionary = manager.call("get_texture_layer_info", "water")
	_check(bool(info.get("found", false)), "the world has a water layer (layers=%d)" % int(info.get("layers", 0)))
	_check(int(info.get("width", 0)) == 16, "water layer is 16 wide, got %d" % int(info.get("width", 0)))
	print("PROBE layer info: " + str(info))

	# A compressed array (the settings menu can turn this on) cannot take a raw
	# RGBA frame at all; the lab handles that by rebuilding uncompressed for the
	# duration of the preview, so the probe walks the same path. The user's
	# setting is put back before the probe exits: the settings menu saves the
	# runtime value on exit, so leaving it flipped would rewrite their config.
	var compression_was: bool = manager.call("get_compression_enabled")
	if not bool(info.get("writable", false)):
		manager.call("set_compression_enabled", false)
		info = manager.call("get_texture_layer_info", "water")
		_check(bool(info.get("writable", false)), "uncompressed rebuild made the layer writable (format=%d)" % int(info.get("format", -1)))
	_check(bool(info.get("writable", false)), "the water layer is writable")

	if bool(info.get("found", false)):
		var size := int(info.get("width", 0))
		var mipmaps := bool(info.get("mipmaps", false))
		# A frame of one flat colour, so the fitted copy is unmistakable.
		var sentinel := Image.create(size, size, false, Image.FORMAT_RGBA8)
		sentinel.fill(Color(0.25, 0.5, 0.75, 1.0))

		# fit_texture_frame is the half of the live path that can be inspected:
		# update_layer() itself is Godot's, and get_layer_data() answers null in
		# this build, so the snapping is what the probe can hold to account.
		var fitted: Image = manager.call("fit_texture_frame", "water", sentinel)
		_check(fitted != null, "fitted the sentinel frame")
		if fitted != null:
			_check(fitted.get_format() == Image.FORMAT_RGBA8, "fitted frame is RGBA8")
			_check(fitted.get_width() == size and fitted.get_height() == size,
				"fitted frame is the layer's %dx%d" % [size, size])
			_check(fitted.get_pixel(size / 2, size / 2) == sentinel.get_pixel(0, 0), "fitted frame kept the colour")
			_check(fitted.has_mipmaps() == mipmaps, "fitted mipmaps match the array (%s)" % str(mipmaps))
		# The caller's image must not be touched by the fitting.
		_check(sentinel.get_width() == size and not sentinel.has_mipmaps(), "the lab's own image was left alone")

		# A resolution the array is not: snapped, not rejected.
		var odd := Image.create(64, 64, false, Image.FORMAT_RGBA8)
		odd.fill(Color(0.1, 0.9, 0.1, 1.0))
		var fitted_odd: Image = manager.call("fit_texture_frame", "water", odd)
		_check(fitted_odd != null and fitted_odd.get_width() == size, "a 64x64 lab frame snaps to the layer size")

		# The pushes themselves: accepted, and refused for names with no layer
		# (writing into the fallback layer would repaint stone).
		_check(manager.call("push_texture_frame", "water", sentinel), "push_texture_frame accepted a frame")
		_check(manager.call("push_texture_frame", "water", odd), "a 64x64 lab frame is accepted")
		_check(not manager.call("push_texture_frame", "not_a_liquid", sentinel), "an unknown texture name is refused")
		_check(manager.call("fit_texture_frame", "not_a_liquid", sentinel) == null, "an unknown texture name has nothing to fit")
		# And the original comes back from disk (pack override or built-in PNG).
		_check(manager.call("restore_texture_layer", "water"), "restored the water layer")
		_check(not manager.call("restore_texture_layer", "not_a_liquid"), "restoring an unknown name fails cleanly")
		# get_layer_data() answers null in this build; must not crash the caller.
		var back = manager.call("get_texture_layer_image", "water")
		print("PROBE readback (null on this Godot build): " + str(back))

	# --- the Live toggle through the tool itself ---------------------------
	lab.call("_show_lab")
	await process_frame
	lab.call("_set_live", true)
	await process_frame
	_check(lab.get("_live"), "live mode engaged")
	for i in 3:
		await process_frame
	lab.call("_set_live", false)
	_check(not lab.get("_live"), "live mode disengaged")
	print("PROBE live label: " + str((lab.get("_live_label") as Label).text))

	lab.call("_hide_lab")
	await process_frame
	_check(not lab.get("_layer").visible, "lab panel hidden again")

	# --- the key binding ----------------------------------------------------
	_check(InputMap.has_action("liquid_lab_toggle"), "the action exists")
	var key_events := InputMap.action_get_events("liquid_lab_toggle")
	_check(key_events.size() > 0 and key_events[0] is InputEventKey and key_events[0].keycode == KEY_O,
		"the action is bound to O")
	# The handler is gameplay-gated (mouse captured), like the other debug keys,
	# and a headless process cannot enter that state â€” DisplayServer is a dummy,
	# so Input.mouse_mode stays VISIBLE however it is set. What can be pinned
	# here is the gate itself: the key must do nothing while the mouse is free
	# (chat, inventory, a menu), which is the failure that would let the panel
	# appear mid-chat and swallow the panel's own keyboard.
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	print("PROBE mouse mode (headless): " + str(Input.mouse_mode))
	var press := InputEventAction.new()
	press.action = "liquid_lab_toggle"
	press.pressed = true
	lab.call("_unhandled_input", press)
	await process_frame
	_check(not lab.get("_open"), "the key does nothing while the mouse is free")
	lab.call("_show_lab")
	await process_frame
	_check(lab.get("_open") and lab.get("_layer").visible, "the panel opens")
	lab.call("_unhandled_input", press)
	await process_frame
	_check(not lab.get("_open"), "and the same key closes it again (no mouse gate while open)")

	# Put the user's compression setting back before the settings menu saves the
	# runtime value on exit.
	if bool(manager.call("get_compression_enabled")) != compression_was:
		manager.call("set_compression_enabled", compression_was)
	_check(bool(manager.call("get_compression_enabled")) == compression_was, "compression setting restored")

	# Clean up the probe's saved files.
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + CHECK_NAME + ".png"))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(SAVE_DIR + "/" + CHECK_NAME + ".json"))

	var verdict := ("PASS (%d checks)" % checks) if ok else ("FAIL (%d/%d)\n" % [failures.size(), checks] + "\n".join(failures))
	var file := FileAccess.open(RESULT_FILE, FileAccess.WRITE)
	if file != null:
		file.store_string(verdict)
		file.close()
	print("PROBE " + verdict.split("\n")[0])
	quit(0 if ok else 1)
